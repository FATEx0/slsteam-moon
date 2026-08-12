// SPDX-License-Identifier: AGPL-3.0-only
//
// See appinfo_provision.hpp for design notes.

#include "appinfo_provision.hpp"
#include "provision_result.hpp"

#include "appinfo_vdf.hpp"
#include "appinfostate.hpp"
#include "cmclient.hpp"
#include "compattool.hpp"
#include "depotkey.hpp"
#include "emptydepot.hpp"
#include "dlcids.hpp"
#include "manifestid.hpp"
#include "manifeststore.hpp"
#include "manifeststore_io.hpp"
#include "manifestsynth.hpp"
#include "provision_cache.hpp"
#include "cache_pair.hpp"
#include "provision_network.hpp"
#include "provision_schedule.hpp"
#include "pending_proton.hpp"
#include "provision_pass.hpp"
#include "synthmark.hpp"
#include "usabledepot.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../bootprof.hpp"
#include "../thread_start.hpp"
#include "../cainfo.hpp"

#include "../utils/ManifestFetch.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/yaml.h"
#include "yaml-cpp/emitter.h"

#include <openssl/sha.h>

#include <curl/curl.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace AppInfoProvision
{

namespace
{

// AdditionalApps that ended up windows-only after pruning native depots
// we have no key for.  These need a Proton CompatToolMapping so Steam
// will actually download + run the windows depot on Linux instead of
// treating the title as "no applicable platform" (which surfaces as the
// install committing 0 bytes / "0 mounted depots").
std::set<uint32_t> g_needProton;
// Config watcher removals can race a nonblocking cache lock. Keep a
// retryable in-memory tombstone so the next cache publication removes the
// corresponding pending Proton mapping instead of leaving stale state.
std::set<uint32_t> g_pendingProtonRemovals;

struct CacheValidationResult
{
	bool valid = false;
	std::string diag;
};

std::mutex g_cacheValidationMu;
std::map<cache::CacheValidationKey, CacheValidationResult> g_cacheValidationMemo;
std::mutex g_provisionPassMu;
std::mutex g_cachePublicationMu;
std::mutex g_cacheReadInvalidationMu;
std::unordered_set<uint32_t> g_cacheReadInvalidated;
std::unordered_map<uint32_t, std::uint64_t> g_cachePublicationGenerations;
// Apps whose last provisioning attempt ended in a result that can never
// publish a cache pair, keyed by the generation that produced it. Guarded by
// g_cachePublicationMu together with the generation map above.
std::unordered_map<uint32_t, std::uint64_t> g_terminalProvisionResults;
std::mutex g_refreshScheduleMu;
bool g_refreshInFlight = false;
bool g_refreshPending = false;
std::string g_refreshPendingPath;
std::vector<std::uint32_t> g_refreshPendingRuntimeApps;
std::uint64_t g_refreshWorkerToken = 0;
std::mutex g_coldRetryMu;
std::chrono::steady_clock::time_point g_coldRetryAfter{};
unsigned int g_coldRetryFailures = 0;

void markProtonNeeded(uint32_t appId,
                      const CachePublicationToken& publication)
{
	std::lock_guard<std::mutex> passLock(g_provisionPassMu);
	if (!publication.managed ||
	    g_config.managedAppIds.get().count(appId) == 0)
		return;

	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	if (!cache::protonPublicationAllowed(
	        publication.managed, publication.generation,
	        cachePublicationGenerationLocked(appId)))
	{
		g_pLog->debug(
		    "AppInfoProvision: rejecting stale Proton mark for app=%u\n",
		    appId);
		return;
	}
	g_pendingProtonRemovals.erase(appId);
	g_needProton.insert(appId);
}

bool coldRetryBlocked()
{
	std::lock_guard<std::mutex> lock(g_coldRetryMu);
	return g_coldRetryAfter != std::chrono::steady_clock::time_point{} &&
	       std::chrono::steady_clock::now() < g_coldRetryAfter;
}

void noteColdRetryOutcome(bool unresolved)
{
	std::lock_guard<std::mutex> lock(g_coldRetryMu);
	if (!unresolved)
	{
		g_coldRetryAfter = {};
		g_coldRetryFailures = 0;
		return;
	}

	++g_coldRetryFailures;
	const unsigned int exponent =
		g_coldRetryFailures > 4 ? 4 : g_coldRetryFailures - 1;
	const unsigned int delaySeconds = std::min(60u, 5u << exponent);
	g_coldRetryAfter = std::chrono::steady_clock::now() +
	                   std::chrono::seconds(delaySeconds);
}



// ---------------------------------------------------------------------------
// libcurl loaded via dlsym to follow the project's portable pattern
// (see utils/ManifestFetch.cpp).  Tied to libcurl.so.4 if available.
// ---------------------------------------------------------------------------

typedef CURL*       (*curl_easy_init_t)();
typedef CURLcode    (*curl_easy_setopt_t)(CURL*, CURLoption, ...);
typedef CURLcode    (*curl_easy_perform_t)(CURL*);
typedef void        (*curl_easy_cleanup_t)(CURL*);
typedef CURLcode    (*curl_easy_getinfo_t)(CURL*, CURLINFO, ...);
typedef const char* (*curl_easy_strerror_t)(CURLcode);

static curl_easy_init_t     p_curl_easy_init     = nullptr;
static curl_easy_setopt_t   p_curl_easy_setopt   = nullptr;
static curl_easy_perform_t  p_curl_easy_perform  = nullptr;
static curl_easy_cleanup_t  p_curl_easy_cleanup  = nullptr;
static curl_easy_getinfo_t  p_curl_easy_getinfo  = nullptr;
static curl_easy_strerror_t p_curl_easy_strerror = nullptr;

bool loadCurl()
{
	if (p_curl_easy_init) return true;
	void* h = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
	if (!h) h = dlopen("libcurl.so.4", RTLD_LAZY);
	if (!h) h = RTLD_DEFAULT;
	p_curl_easy_init     = (curl_easy_init_t)     dlsym(h, "curl_easy_init");
	p_curl_easy_setopt   = (curl_easy_setopt_t)   dlsym(h, "curl_easy_setopt");
	p_curl_easy_perform  = (curl_easy_perform_t)  dlsym(h, "curl_easy_perform");
	p_curl_easy_cleanup  = (curl_easy_cleanup_t)  dlsym(h, "curl_easy_cleanup");
	p_curl_easy_getinfo  = (curl_easy_getinfo_t)  dlsym(h, "curl_easy_getinfo");
	p_curl_easy_strerror = (curl_easy_strerror_t) dlsym(h, "curl_easy_strerror");
	return p_curl_easy_init && p_curl_easy_setopt &&
	       p_curl_easy_perform && p_curl_easy_cleanup;
}

std::size_t curlWriteCb(const char* p, std::size_t sz, std::size_t n, std::string* dst)
{
	dst->append(p, sz * n);
	return sz * n;
}

NetworkFailure httpGetJson(const std::string& url, std::string& body,
                           std::string& diag, long timeoutMs)
{
	if (!loadCurl()) { diag = "libcurl unavailable"; return NetworkFailure::Provider; }
	if (timeoutMs <= 0)
	{
		diag = "startup network budget exhausted";
		return NetworkFailure::Transient;
	}
	CURL* c = p_curl_easy_init();
	if (!c) { diag = "curl_easy_init failed"; return NetworkFailure::Provider; }

	body.clear();
	p_curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	p_curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	p_curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
	p_curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	// Bounded timeouts.  AppInfoProvision runs in setup() on the startup
	// path, so we must not stall Steam's launch for too long if
	// steamcmd.net is slow or unreachable.  The fetch is now wrapped in a
	// bounded retry (see provisionApp), and all provider attempts in this
	// startup pass share one 15-second wall-clock budget. A slow mirror can
	// therefore never multiply this timeout by app count or retry count.
	p_curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
	p_curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS,
	                   std::min(timeoutMs, 8000L));
	// Same multi-thread safety justification as ManifestFetch::httpGet.
	p_curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	p_curl_easy_setopt(c, CURLOPT_USERAGENT, "SLSsteam-AppInfoProvision/0.1");
	// Pin the system trust store (see cainfo.hpp): the libcurl Steam loads
	// otherwise fails CA verification on SteamOS/Arch with curl error 60
	// ("Peer certificate cannot be authenticated"), the exact failure that
	// sinks the steamcmd.net fallback. No-op when no bundle is found.
	if (const char* f = ca::bundleFile()) p_curl_easy_setopt(c, CURLOPT_CAINFO, f);
	if (const char* d = ca::bundleDir())  p_curl_easy_setopt(c, CURLOPT_CAPATH, d);

	const CURLcode rc = p_curl_easy_perform(c);
	long status = 0;
	if (p_curl_easy_getinfo) p_curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	p_curl_easy_cleanup(c);

	if (rc != CURLE_OK)
	{
		diag = p_curl_easy_strerror ? p_curl_easy_strerror(rc) : "curl error";
		return classifyCurlFailure(rc);
	}
	if (status != 200)
	{
		std::stringstream s; s << "HTTP " << status;
		diag = s.str();
		return classifyHttpFailure(status);
	}
	if (body.empty()) { diag = "empty body"; return NetworkFailure::Provider; }
	return NetworkFailure::None;
}

// ---------------------------------------------------------------------------
// Provider list.  Each entry is a URL template with `{appid}`.
// First success wins.  Override via env SLSSTEAM_APPINFO_PROVIDER.
// ---------------------------------------------------------------------------

const std::vector<std::string>& providerChain()
{
	// Built once.  The first entry can be overridden via the env var
	// SLSSTEAM_APPINFO_PROVIDER (a URL template containing `{appid}`),
	// which is handy for testing the fetch/retry path against a custom
	// or deliberately-slow endpoint without rebuilding.
	static const std::vector<std::string> chain = [] {
		std::vector<std::string> c;
		if (const char* ov = std::getenv("SLSSTEAM_APPINFO_PROVIDER");
		    ov && *ov)
		{
			c.emplace_back(ov);
		}
		c.emplace_back("https://api.steamcmd.net/v1/info/{appid}");
		return c;
	}();
	return chain;
}

std::string expandUrl(const std::string& tmpl, uint32_t appId)
{
	std::string out;
	out.reserve(tmpl.size() + 16);
	const std::string needle = "{appid}";
	std::size_t i = 0;
	while (i < tmpl.size())
	{
		if (tmpl.compare(i, needle.size(), needle) == 0)
		{
			out += std::to_string(appId);
			i += needle.size();
		}
		else
		{
			out.push_back(tmpl[i++]);
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// VDF text writer.
// ---------------------------------------------------------------------------
//
// `AppInfoVdf::translateWireToIndexed` accepts the same KV1-text dialect
// that PICS uses on the wire ("appinfo" { ... } with quoted keys/values
// and braces).  We emit a strict subset of that here: every leaf is a
// quoted string (Steam's appinfo schema is all-strings anyway).
//
// We also keep a fallback v39-binary writer in case the project ever
// switches to that.  Currently unused.

void emitEscaped(std::string& out, const std::string& s)
{
	out.push_back('"');
	for (char c : s)
	{
		if (c == '\\' || c == '"') out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
}

void emitNode(std::string& out, const YAML::Node& node, int depth);

void emitMap(std::string& out, const YAML::Node& node, int depth)
{
	for (auto it = node.begin(); it != node.end(); ++it)
	{
		const std::string key = it->first.as<std::string>();
		// SteamCMD JSON adds "_change_number", "_sha", "_size",
		// "_missing_token" sibling fields at the top level.  These
		// belong to the *envelope*, not the appinfo body, so the
		// caller filters them before calling emitMap.
		out.append(depth, '\t');
		emitEscaped(out, key);
		const auto& val = it->second;
		if (val.IsMap())
		{
			out.append("\n");
			out.append(depth, '\t');
			out.append("{\n");
			emitNode(out, val, depth + 1);
			out.append(depth, '\t');
			out.append("}\n");
		}
		else if (val.IsScalar())
		{
			out.append("\t\t");
			emitEscaped(out, val.as<std::string>());
			out.append("\n");
		}
		else if (val.IsSequence())
		{
			// Sequences shouldn't appear in appinfo, but if they do,
			// render as an indexed map: "0" "v0" "1" "v1"...
			out.append("\n");
			out.append(depth, '\t');
			out.append("{\n");
			int idx = 0;
			for (const auto& child : val)
			{
				out.append(depth + 1, '\t');
				emitEscaped(out, std::to_string(idx++));
				if (child.IsMap())
				{
					out.append("\n");
					out.append(depth + 1, '\t');
					out.append("{\n");
					emitNode(out, child, depth + 2);
					out.append(depth + 1, '\t');
					out.append("}\n");
				}
				else
				{
					out.append("\t\t");
					emitEscaped(out, child.IsScalar() ? child.as<std::string>() : std::string{});
					out.append("\n");
				}
			}
			out.append(depth, '\t');
			out.append("}\n");
		}
		else
		{
			// Null / undefined → empty string
			out.append("\t\t\"\"\n");
		}
	}
}

void emitNode(std::string& out, const YAML::Node& node, int depth)
{
	if (node.IsMap())
	{
		emitMap(out, node, depth);
	}
}

// Strip depots that we have no decryption key for so Steam's
// downloader picks a depot we *can* actually decrypt.
//
// Background: api.steamcmd.net returns the full depot list (e.g. for
// dotAGE that's 638511=windows, 638512=macos, 638513=linux).  If the
// LuaTools plugin only provided a key for the windows depot, Steam on
// a Linux client picks the linux depot, finds no key, and drops to
// "0 mounted depots" — same observable bug as if appinfo were missing
// entirely.
//
// Behaviour:
//   - Drops every "<depot_id>" child of "depots" that lacks a cached
//     DepotKey, unless it's a DLC entry (`dlcappid` only) which carries
//     no payload to decrypt.
//   - Removes a DLC from extended.listofdlc when all of that DLC's content
//     depots were dropped.  Otherwise PackagePatch advertises the DLC as
//     owned and Steam independently plans the rejected encrypted content.
//   - If the surviving set has at least one playable depot but the
//     original "common.oslist" included an OS we just dropped, narrow
//     "oslist" to the OSes we still have so Steam picks Proton (for
//     "windows") rather than a phantom native binary.
//
// Mutates `body` in place.  `appId` is for log lines only.
void pruneUnsupportedDepots(YAML::Node& body, uint32_t appId,
                             const CachePublicationToken& publication)
{
	if (!body.IsMap()) return;
	YAML::Node depots = body["depots"];
	if (!depots || !depots.IsMap()) return;

	// yaml-cpp's Node::remove() is unreliable when nodes are aliased
	// (which they are here — `body` was built by aliasing the parsed
	// tree).  Instead build a fresh depots map containing only the
	// entries we keep, then replace the whole block.  Cloning each
	// kept value via YAML::Clone breaks the alias so the rebuild is
	// self-contained.
	YAML::Node newDepots(YAML::NodeType::Map);
	std::set<std::string> survivingOs;
	int kept = 0;
	int dropped = 0;
	int totalNumeric = 0;
	std::unordered_set<uint32_t> contentDlcAppIds;
	std::unordered_set<uint32_t> usableDlcAppIds;

	for (auto it = depots.begin(); it != depots.end(); ++it)
	{
		const std::string key = it->first.as<std::string>();

		// Non-numeric keys are metadata (branches, baselanguages, …).
		// Always keep verbatim.
		bool numeric = !key.empty();
		for (char c : key) if (c < '0' || c > '9') { numeric = false; break; }
		if (!numeric)
		{
			newDepots[key] = YAML::Clone(it->second);
			continue;
		}
		++totalNumeric;

		uint32_t depotId = 0;
		try { depotId = static_cast<uint32_t>(std::stoul(key)); }
		catch (...) { newDepots[key] = YAML::Clone(it->second); continue; }

		const YAML::Node depotNode = it->second;
		const bool hasDlcMarker = depotNode.IsMap() && depotNode["dlcappid"];
		const bool hasManifests = depotNode.IsMap() && depotNode["manifests"];
		const bool isVirtualDlc = hasDlcMarker && !hasManifests;
		uint32_t dlcAppId = 0;
		if (hasDlcMarker)
		{
			try { dlcAppId = depotNode["dlcappid"].as<uint32_t>(); }
			catch (...) {}
		}

		// A DLC can own multiple depots.  Record every content-bearing
		// candidate now, before any drop path, and mark it usable only when at
		// least one of its entries survives.  This prevents one missing-key
		// sibling from hiding a valid keyed sibling.
		if (dlcAppId != 0 && hasManifests)
		{
			contentDlcAppIds.insert(dlcAppId);
		}

		// Drop empty (size-0) content depots.  Their manifest is a
		// degenerate stub — a single file mapping with an EMPTY name — and
		// Steam SEGV-crashes loading it during reconfigure
		// (Assert(!m_strName.IsEmpty()):contentmanifest.cpp:1630, seen live
		// on app=1868140 depot=4394810).  A size-0 depot has nothing to
		// install, so dropping it loses no content and keeps Steam from ever
		// planning the crash-inducing manifest.
		if (depotPublicManifestIsEmpty(depotNode))
		{
			++dropped;
			g_pLog->info("AppInfoProvision: app=%u dropping empty depot %u "
			             "(public manifest size 0)\n", appId, depotId);
			continue;
		}

		// DLC entries have no `manifests` block; they're virtual and
		// don't need a decryption key — keep them.
		const auto savedKey = DepotKey::getCachedKey(depotId);
		const bool hasKey = !savedKey.key.empty();

		if (!hasKey && !isVirtualDlc)
		{
			++dropped;
			continue;  // omit from newDepots
		}
		++kept;
		newDepots[key] = YAML::Clone(depotNode);
		if (dlcAppId != 0)
		{
			usableDlcAppIds.insert(dlcAppId);
		}

		// Track which OS this surviving depot supports so we can
		// narrow common.oslist later.
		if (depotNode.IsMap() && depotNode["config"] &&
		    depotNode["config"]["oslist"])
		{
			std::string osStr;
			try { osStr = depotNode["config"]["oslist"].as<std::string>(); }
			catch (...) {}
			std::size_t i = 0;
			while (i < osStr.size())
			{
				std::size_t j = osStr.find(',', i);
				if (j == std::string::npos) j = osStr.size();
				const auto piece = osStr.substr(i, j - i);
				if (!piece.empty()) survivingOs.insert(piece);
				i = j + 1;
			}
		}
	}

	// Keep PackagePatch's synthetic ownership list aligned with the depots
	// above.  A DLC is unsupported only when it had content entries and none
	// survived; list-only/virtual DLCs and DLCs with at least one keyed depot
	// remain untouched.
	std::unordered_set<uint32_t> unsupportedDlcAppIds;
	for (uint32_t dlcAppId : contentDlcAppIds)
	{
		if (usableDlcAppIds.count(dlcAppId) == 0)
		{
			unsupportedDlcAppIds.insert(dlcAppId);
		}
	}
	if (!unsupportedDlcAppIds.empty())
	{
		YAML::Node extended = body["extended"];
		YAML::Node listNode = extended && extended.IsMap()
			? extended["listofdlc"]
			: YAML::Node();
		if (listNode && listNode.IsScalar())
		{
			try
			{
				const std::string oldList = listNode.as<std::string>();
				std::size_t removed = 0;
				const std::string newList =
					filterUnsupportedDlcAppIds(oldList, unsupportedDlcAppIds,
					                           &removed);
				if (removed != 0)
				{
					body["extended"]["listofdlc"] = newList;
					g_pLog->info("AppInfoProvision: app=%u removed %zu unsupported "
					             "content DLC appid(s) from extended.listofdlc\n",
					             appId, removed);
				}
			}
			catch (...) {}
		}
	}

	if (dropped == 0)
	{
		// Even when we drop nothing, the app may be natively
		// non-Linux (e.g. the user added a windows-only title).  Mark
		// it for Proton when no surviving depot targets Linux.
		if (!survivingOs.empty() && !survivingOs.count("linux"))
		{
			markProtonNeeded(appId, publication);
		}
		return;
	}

	body["depots"] = newDepots;
	g_pLog->info("AppInfoProvision: app=%u dropped %d/%d unsupported depots (kept %d)\n",
	             appId, dropped, totalNumeric, kept);

	// If none of the surviving depots target Linux natively, the app
	// can only run through Proton.  Register it for a CompatToolMapping
	// so Steam downloads + runs the windows depot on Linux instead of
	// skipping it as "no applicable platform".  (Covers windows-only
	// and windows+macos apps.)
	if (!survivingOs.empty() && !survivingOs.count("linux"))
	{
		markProtonNeeded(appId, publication);
	}

	// NOTE: we intentionally do NOT narrow common.oslist.  Leaving the
	// upstream oslist (e.g. "windows,macos,linux") intact while only
	// the windows depot survives matches what Steam itself stores for
	// Proton titles and lets the downloader pick the windows depot via
	// the CompatToolMapping.  Narrowing it to "windows" was observed to
	// leave the downloader stuck in "Reconfiguring" forever for
	// single-depot apps.
	(void)0;
}

// Forward declaration: defined with the on-disk cache helpers below.
const std::string& getCacheDir();

// Rebuild a missing `depots` block for a token-locked app from data we
// already hold on disk.  Some titles (e.g. Risk of Rain 2, app 632360)
// have their PICS product-info gated behind an app access token Valve
// DENIES to anonymous sessions, so the anonymous-CM buffer (and the
// steamcmd fallback) come back with NO depots and provisioning would bail
// — leaving Steam at 0 B.  But the LuaTools zip already gave us the depot
// key (DepotKey cache) and the depot manifest (ManifestStore), so we can
// reconstruct the depots block ourselves: managed depots for this app,
// each pointing at its best archived manifest gid, with config.oslist
// inferred from the manifest so a Windows-only depot still gets the Proton
// CompatTool mapping downstream.  No-op when the body already has real
// depots (see ManifestSynth::injectSynthesizedDepots).  Mutates `body`;
// returns the number of depots synthesized.
int synthesizeDepotsFromStore(YAML::Node& body, uint32_t appId)
{
	std::vector<ManifestSynth::SynthDepot> depots;
	// Accumulate file lists per OS across all of the app's depots so we can
	// synthesize one launch entry per platform (native + forced-Proton).
	std::vector<std::string> winFiles, linFiles, macFiles;
	for (uint32_t depotId : DepotKey::managedDepotsForApp(appId))
	{
		const uint64_t gid = ManifestStore::bestArchivedGid(depotId, /*excludeGid=*/0);
		if (gid == 0) continue; // no archived manifest -> can't plan it

		// Read the archived manifest, derive the depot's OS from its parsed
		// file list (config.oslist, so pruneUnsupportedDepots mounts it on
		// the right platform / forces Proton only for genuinely windows-only
		// titles), and its total sizes (so the install dialog shows a real
		// size instead of "0 B").
		std::string oslist;
		uint64_t size = 0, download = 0;
		{
			const std::string man =
				ManifestStore::dir() + "/" + std::to_string(depotId) + "_" +
				std::to_string(gid) + ".manifest";
			std::ifstream ifs(man, std::ios::binary);
			if (ifs)
			{
				std::string bytes((std::istreambuf_iterator<char>(ifs)),
				                  std::istreambuf_iterator<char>());
				const auto files = ManifestSynth::extractManifestFilenames(bytes);
				oslist = ManifestSynth::detectOsFromFiles(files);
				if (oslist == "windows")     { for (auto& f : files) winFiles.push_back(f); }
				else if (oslist == "linux")  { for (auto& f : files) linFiles.push_back(f); }
				else if (oslist == "macos")  { for (auto& f : files) macFiles.push_back(f); }
				ManifestSynth::parseManifestSizes(bytes, size, download);
			}
		}
		depots.push_back({depotId, gid, oslist, size, download});
	}
	const int n = ManifestSynth::injectSynthesizedDepots(body, depots);
	if (n > 0)
	{
		// A token-locked app's product-info has no config block either, so
		// give Steam an installdir (derived from common.name) or it fails the
		// install with "Invalid install path".
		ManifestSynth::ensureInstallDir(body);

		// ...and no config.launch, so Steam refuses to start it ("Invalid
		// game configuration").  Synthesize one launch option per OS we hold
		// a depot for: the native one lets it run directly, the windows one
		// covers a forced Proton/compat tool.
		std::string installdir;
		if (YAML::Node d = body["config"]["installdir"]; d && d.IsScalar())
			installdir = d.as<std::string>();
		std::vector<std::pair<std::string, std::string>> launchers;
		if (!linFiles.empty())
			launchers.push_back({ManifestSynth::pickLauncher(linFiles, installdir, "linux"), "linux"});
		if (!winFiles.empty())
			launchers.push_back({ManifestSynth::pickLauncher(winFiles, installdir, "windows"), "windows"});
		if (!macFiles.empty())
			launchers.push_back({ManifestSynth::pickLauncher(macFiles, installdir, "macos"), "macos"});
		const int le = ManifestSynth::ensureLaunchEntries(body, launchers);
		if (le > 0)
		{
			std::string summary;
			for (const auto& [exe, os] : launchers)
				if (!exe.empty()) summary += " " + os + ":'" + exe + "'";
			g_pLog->info("AppInfoProvision: app=%u synthesized %d launch entry(ies):%s\n",
			             appId, le, summary.c_str());
		}

		// The synthetic marker is published together with the validated cache
		// pair by persistBuffer. Keeping it out of this render phase prevents
		// stale work from recreating the marker after a managed-source removal.
	}
	return n;
}

// Neutralize the legacy third-party CD-key requirement.
//
// appinfo's `extended/hadthirdpartycdkey "1"` makes Steam's launch
// pipeline run a GettingLegacyKey step: it issues ClientGetLegacyGameKey
// to the CM, which validates ownership server-side and answers
// AccessDenied (EResult 15) for an app the account doesn't actually own.
// The launch then fails BEFORE the compat tool / game process is ever
// spawned — the "updating product key" flash that drops straight back to
// Play (console_log: "LaunchApp failed with GettingLegacyKey with 15",
// and no ~/steam-<appid>.log because Proton never starts).
//
// The IClientUser::RequiresLegacyCDKey detour only suppresses the CD-key
// *prompt* (ShowCDKey) path; it does NOT gate this launch-time fetch,
// which reads straight off appinfo.  Zeroing the field here — in the same
// offline appinfo rewrite that prunes depots and pins gids, NOT on the
// live product-info buffer (which Steam sha-validates) — makes the launch
// skip GettingLegacyKey entirely.  The game's own activation DRM (EA
// serial, Uplay, ...) is a separate layer untouched by this.
void neutralizeLegacyCdKey(YAML::Node& body, uint32_t appId)
{
	if (!body.IsMap()) return;
	YAML::Node ext = body["extended"];
	if (!ext || !ext.IsMap()) return;
	YAML::Node had = ext["hadthirdpartycdkey"];
	if (!had || !had.IsScalar()) return;

	std::string cur;
	try { cur = had.as<std::string>(); } catch (...) { return; }
	if (cur == "0") return;

	ext["hadthirdpartycdkey"] = "0";
	g_pLog->info("AppInfoProvision: app=%u cleared extended.hadthirdpartycdkey "
	             "(was %s) so launch skips GettingLegacyKey\n",
	             appId, cur.c_str());
}

// Render the SteamCMD-style JSON response for one app into the wire-text
// VDF format that AppInfoVdf::translateWireToIndexed accepts.
bool isDlcApp(const YAML::Node& body)
{
	if (!body || !body.IsMap()) return false;
	const YAML::Node common = body["common"];
	if (!common || !common.IsMap()) return false;
	const YAML::Node type = common["type"];
	if (!type || !type.IsScalar()) return false;
	std::string value = type.as<std::string>("");
	for (char& ch : value)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return value == "dlc";
}

SourceResult renderAppinfoBuffer(
    const YAML::Node& appNode, uint32_t appId, std::string& wireOut,
    const CachePublicationToken& publication, bool* synthesizedOut)
{
	if (synthesizedOut) *synthesizedOut = false;
	if (!appNode.IsMap()) return SourceResult::InvalidResponse;

	// Drop SteamCMD synthetic envelope fields ("_change_number", "_sha",
	// "_size", "_missing_token").  They are not part of the appinfo
	// document Steam stores.
	YAML::Node body;
	for (auto it = appNode.begin(); it != appNode.end(); ++it)
	{
		const std::string key = it->first.as<std::string>();
		if (!key.empty() && key.front() == '_') continue;
		body[key] = it->second;
	}
	if (!body.IsMap() || body.size() == 0) return SourceResult::InvalidResponse;

	// Remember whether the provider supplied any concrete content before
	// pruning. A valid token-limited response with no depot data may benefit
	// from a secondary provider; concrete depots that are all rejected locally
	// will not, so that result must be terminal.
	bool hadConcreteContent = hasUsableContentDepot(body);

	// Token-locked apps (product-info access token denied to anonymous
	// sessions) arrive with NO depots.  Rebuild the block from the depot
	// key + archived manifest we already hold, so the prune/Proton/splice
	// tail below runs unchanged.  No-op when real depots are present.
	if (YAML::Node d = body["depots"]; !d || !d.IsMap() || d.size() == 0)
	{
		const int n = synthesizeDepotsFromStore(body, appId);
		if (n > 0)
		{
			if (synthesizedOut) *synthesizedOut = true;
			g_pLog->info("AppInfoProvision: app=%u synthesized %d depot(s) from "
			             "stored manifests (product-info had none)\n", appId, n);
			hadConcreteContent = hasUsableContentDepot(body);
		}
	}

	// Strip depots we can't decrypt; narrow common.oslist accordingly.
	// Done here (post-envelope-strip, pre-emit) so the output Steam
	// reads is consistent and the change is invisible to Steam beyond
	// "the user only owns the windows depot".
	pruneUnsupportedDepots(body, appId, publication);
	const SourceResult contentResult = classifyContentResult(
	    hadConcreteContent, hasUsableContentDepot(body), isDlcApp(body));
	if (contentResult != SourceResult::Success)
	{
		g_pLog->info("AppInfoProvision: app=%u has no usable content depots "
		             "after pruning, skipping\n", appId);
		return contentResult;
	}

	// Clear the launch-time legacy CD-key gate (see helper above): without
	// this the GettingLegacyKey step fails AccessDenied for an unowned app
	// and the launch aborts before the game/Proton ever starts.
	neutralizeLegacyCdKey(body, appId);

	// A locked app keeps the LIVE public gid in its provisioned appinfo so
	// Steam computes a genuine content delta; the pinned target is applied at
	// the post-commit reconcile instead, which avoids contaminating the
	// active/baseline manifest read.

	// Steam's appinfo wire format wraps the document in "appinfo" { ... }.
	wireOut.clear();
	wireOut.append("\"appinfo\"\n{\n");
	emitNode(wireOut, body, 1);
	wireOut.append("}\n");
	return SourceResult::Success;
}

// ---------------------------------------------------------------------------
// On-disk cache (mirrors feats/pics.cpp layout so AppInfoVdf::injectAllCached
// picks the buffers up at next start).
// ---------------------------------------------------------------------------

const std::string& getCacheDir()
{
	static const std::string dir = [] {
		const std::string value = g_config.getDir() + "/cache";
		if (!std::filesystem::exists(value))
		{
			std::error_code ec;
			std::filesystem::create_directories(value, ec);
		}
		return value;
	}();
	return dir;
}

std::string getBufferPath(uint32_t appId)
{
	return getCacheDir() + "/picsbuffer_" + std::to_string(appId) + ".bin";
}

std::string getMetaPath(uint32_t appId)
{
	return getCacheDir() + "/picsbuffer_" + std::to_string(appId) + ".yaml";
}

std::string pendingProtonPath()
{
	return getCacheDir() + "/proton-mappings.pending";
}

enum class PendingProtonFileStatus
{
	Missing,
	Valid,
	Invalid,
};

struct PendingProtonFile
{
	PendingProtonFileStatus status = PendingProtonFileStatus::Invalid;
	std::set<uint32_t> ids;
};

// Read the pending mapping file while the caller holds cacheLock. Missing is
// a normal first-run state; every other open/read/parse failure is invalid and
// must leave the existing file and any in-memory removals untouched.
PendingProtonFile readPendingProtonFileLocked()
{
	const auto path = pendingProtonPath();
	std::ifstream ifs(path);
	if (!ifs.is_open())
	{
		std::error_code ec;
		const bool exists = std::filesystem::exists(path, ec);
		if (!exists && !ec)
			return {PendingProtonFileStatus::Missing, {}};
		return {PendingProtonFileStatus::Invalid, {}};
	}

	std::set<uint32_t> ids;
	std::string token;
	while (ifs >> token)
	{
		const auto parsed = parsePendingProtonText(token);
		if (parsed.status != PendingProtonParseStatus::Valid ||
		    parsed.ids.size() != 1)
		{
			return {PendingProtonFileStatus::Invalid, {}};
		}
		ids.insert(*parsed.ids.begin());
	}
	if (ifs.bad() || !ifs.eof())
		return {PendingProtonFileStatus::Invalid, {}};
	return {PendingProtonFileStatus::Valid, std::move(ids)};
}

// Runtime PICS provisioning must not replace Steam's live config.vdf. Keep
// the ids in a small cache record for the next preinit pass instead.
void persistPendingProtonMappings(bool waitForLock)
{
	if (g_needProton.empty() && g_pendingProtonRemovals.empty()) return;
	(void)getCacheDir();
	ProcessLock::FileLock cacheLock(cacheLockPath(), !waitForLock);
	if (!cacheLock.acquired()) return;

	const auto existing = readPendingProtonFileLocked();
	if (existing.status == PendingProtonFileStatus::Invalid)
	{
		if (g_pLog)
			g_pLog->debug("AppInfoProvision: preserving invalid or unreadable pending Proton file\n");
		return;
	}

	std::set<uint32_t> pending;
	const auto managed = g_config.managedAppIds.get();
	for (uint32_t appId : existing.ids)
	{
		if (managed.count(appId) != 0)
			pending.insert(appId);
	}
	for (uint32_t appId : g_needProton)
	{
		if (managed.count(appId) != 0)
			pending.insert(appId);
	}
	for (uint32_t appId : g_pendingProtonRemovals)
		pending.erase(appId);

	bool persisted = false;
	std::string error;
	if (pending.empty())
	{
		std::error_code ec;
		std::filesystem::remove(pendingProtonPath(), ec);
		persisted = !ec;
	}
	else
	{
		std::string content;
		for (uint32_t appId : pending)
			content += std::to_string(appId) + "\n";
		persisted = AtomicFile::write(pendingProtonPath(), content, error);
	}
	if (!persisted)
	{
		if (g_pLog)
			g_pLog->debug("AppInfoProvision: cannot persist pending Proton mappings: %s\n",
			              error.empty() ? "remove failed" : error.c_str());
		return;
	}
	g_pendingProtonRemovals.clear();
}

// Return the full identity of `appId`'s on-disk provisioned buffer, and
// whether it exists and is non-empty.  Used by provisionApp's short-lived
// cache to skip the network fetch during the setup() re-exec storm of a
// single boot and to key the expensive validation memo safely across atomic
// replacements.
bool statBuffer(uint32_t appId, cache::CacheValidationKey& identityOut)
{
	struct stat st{};
	if (stat(getBufferPath(appId).c_str(), &st) != 0) return false;
	if (st.st_size <= 0) return false;
	identityOut = cache::CacheValidationKey{
	    .appId = appId,
	    .mtimeSecs = static_cast<long long>(st.st_mtime),
	    .mtimeNsecs = static_cast<long long>(st.st_mtim.tv_nsec),
	    .size = static_cast<long long>(st.st_size),
	    .inode = static_cast<std::uint64_t>(st.st_ino),
	};
	return true;
}

bool hasBufferOnDisk(uint32_t appId)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return false;
	cache::CacheValidationKey identity{};
	return statBuffer(appId, identity);
}

// Freshness window for the provisioning cache, in seconds.  Short by
// design: it must cover Steam's setup() re-exec storm within one boot
// (so the 8-app fleet is fetched once, not once per pass) without
// surviving into a later genuine relaunch, where we re-fetch the live
// public gid (see provision_cache.hpp for the gid-staleness rationale).
// Override via SLSSTEAM_PROVISION_TTL (seconds; 0 disables the cache).
long long provisionTtlSecs()
{
	if (const char* ov = std::getenv("SLSSTEAM_PROVISION_TTL"); ov && *ov)
	{
		try { return std::stoll(ov); } catch (...) {}
	}
	return 300; // 5 minutes
}

bool persistBuffer(uint32_t appId, uint32_t changeNumber,
                   const std::string& sha20, const std::string& wire,
                   const CachePublicationToken& publication,
                   bool markSynthetic)
{
	(void)getCacheDir();
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired())
	{
		g_pLog->info("AppInfoProvision: unable to lock cache pair for app=%u\n", appId);
		return false;
	}
	if (!cache::cachePublicationAllowed(
	        publication.managed, publication.generation,
	        cachePublicationGenerationLocked(appId)))
	{
		g_pLog->debug(
		    "AppInfoProvision: rejecting stale cache publication for app=%u\n",
		    appId);
		return false;
	}

	if (sha20.size() != 20)
	{
		g_pLog->debug("AppInfoProvision: refuse to persist app=%u, sha size %zu\n",
		              appId, sha20.size());
		return false;
	}
	YAML::Emitter em;
	em << YAML::BeginMap;
	em << YAML::Key << "appid"         << YAML::Value << appId;
	em << YAML::Key << "change_number" << YAML::Value << changeNumber;
	em << YAML::Key << "wire_size"     << YAML::Value << wire.size();
	em << YAML::Key << "sha_b64"       << YAML::Value << base64::to_base64(sha20);
	em << YAML::Key << "normalized"    << YAML::Value << true;
	em << YAML::Key << "synthetic"     << YAML::Value << markSynthetic;
	em << YAML::EndMap;
	const std::string metadata(em.c_str(), em.size());
	// The marker is published inside the same locked transaction as the pair,
	// so the generation gate above already covers it: nothing can change the
	// generation while this thread holds the publication mutex and cache lock.
	const bool markerBefore = SynthMark::isMarked(getCacheDir(), appId);
	std::string writeError;
	if (!publishCachePairLocked(appId, wire, metadata, markSynthetic,
	                            markerBefore, writeError))
	{
		g_pLog->debug(
		    "AppInfoProvision: unable to publish cache pair for app=%u: %s\n",
		    appId, writeError.c_str());
		return false;
	}
	clearCacheReadInvalidation(appId);
	return true;
}

// ---------------------------------------------------------------------------
// Decide whether a given appId already has depots in the on-disk
// appinfo.vdf.  We don't fully parse v41 here — we only care about
// finding the literal "depots" key inside that app's binary KV blob,
// which is enough to skip already-rich entries.
// ---------------------------------------------------------------------------

[[maybe_unused]]
bool hasDepotsForApp(const std::string& appinfoVdfPath, uint32_t appId)
{
	std::ifstream ifs(appinfoVdfPath, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return false;
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (1LL << 30)) return false;
	std::string buf;
	buf.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	ifs.read(buf.data(), sz);

	// Linear scan for the appid little-endian followed by enough header
	// bytes to be a real entry header.  The appinfo.vdf entry header
	// layout is:
	//   uint32 appid; uint32 size; uint32 info_state; uint64 last_updated;
	//   uint64 token; bytes sha[20]; uint32 change#; bytes binsha[20]
	// total = 72 bytes.
	const auto* data = reinterpret_cast<const std::uint8_t*>(buf.data());
	const std::size_t n = buf.size();
	if (n < 76) return false;

	for (std::size_t i = 0; i + 76 <= n; ++i)
	{
		uint32_t a;
		std::memcpy(&a, data + i, sizeof(a));
		if (a != appId) continue;
		uint32_t entrySize;
		std::memcpy(&entrySize, data + i + 4, sizeof(entrySize));
		// sanity
		if (entrySize < 64 || entrySize > 8u * 1024u * 1024u) continue;
		const std::size_t kvStart = i + 72;
		const std::size_t kvEnd   = i + 8 + entrySize;
		if (kvEnd > n || kvEnd <= kvStart) continue;
		const std::string_view kv(reinterpret_cast<const char*>(data) + kvStart,
		                          kvEnd - kvStart);
		// Binary v41 KV uses uint32 string-table indices, so "depots"
		// won't appear as ASCII inside the body.  We instead look for
		// "manifests" or any depot-like ASCII fragment that the wire
		// reader writes raw.  Safer: the binary VDF *also* embeds
		// string values verbatim, including manifest gids and section
		// labels like "public", but those are noisy.  Use a simple
		// heuristic: the entry must contain the literal "manifests"
		// somewhere — every depot row carries it as a value.
		if (kv.find("manifests") != std::string_view::npos)
		{
			return true;
		}
		// Even if the v41 binary blob is fully indexed, the original
		// raw string table at the file end resolves them — but for our
		// purposes "depot key not yet pinned" means we provision.
		return false;
	}
	return false;
}

// ---------------------------------------------------------------------------
// SHA-1 helper (libcrypto via dlsym, same pattern as appinfo_vdf.cpp).
// ---------------------------------------------------------------------------

void sha1BytesInternal(const void* data, std::size_t n, std::uint8_t out[20])
{
	static unsigned char* (*p_SHA1)(const unsigned char*, size_t, unsigned char*) = nullptr;
	if (!p_SHA1)
	{
		void* h = dlopen("libcrypto.so.3", RTLD_NOLOAD | RTLD_LAZY);
		if (!h) h = dlopen("libcrypto.so.3", RTLD_LAZY);
		if (!h) h = dlopen("libcrypto.so.1.1", RTLD_LAZY);
		if (!h) h = RTLD_DEFAULT;
		p_SHA1 = (unsigned char* (*)(const unsigned char*, size_t, unsigned char*))
		         dlsym(h, "SHA1");
	}
	if (p_SHA1)
	{
		p_SHA1(reinterpret_cast<const unsigned char*>(data), n, out);
	}
	else
	{
		std::memset(out, 0, 20);
	}
}

// ---------------------------------------------------------------------------
// Parse SteamCMD JSON response and extract envelope fields.  YAML-cpp
// reads JSON since JSON is a strict subset of YAML.
// ---------------------------------------------------------------------------

bool extractAppNode(const std::string& json, uint32_t appId,
                    YAML::Node& outApp, std::string& err)
{
	try
	{
		YAML::Node root = YAML::Load(json);
		if (!root.IsMap()) { err = "root not a map"; return false; }
		if (!root["data"]) { err = "no 'data'"; return false; }
		const auto data = root["data"];
		if (!data.IsMap()) { err = "data not a map"; return false; }
		const std::string key = std::to_string(appId);
		if (!data[key]) { err = "no entry for appid"; return false; }
		outApp = data[key];
		if (!outApp.IsMap()) { err = "app entry not a map"; return false; }
	}
	catch (const std::exception& e)
	{
		err = e.what();
		return false;
	}
	return true;
}

uint32_t pickChangeNumber(const YAML::Node& app)
{
	if (app["_change_number"])
	{
		try { return app["_change_number"].as<uint32_t>(); }
		catch (...) {}
	}
	return 0;
}

// (g_needProton declared near the top of this anonymous namespace.)

std::string steamRootForConfig()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	const std::vector<std::string> candidates = {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam",
	};
	for (const auto& c : candidates)
	{
		struct stat st{};
		if (stat((c + "/steam.sh").c_str(), &st) == 0) return c;
	}
	return {};
}

// Build one index of depot ids backed by valid manifest artifacts.  The
// collector below may see many advertised DLC ids, so querying
// ManifestStore::bestArchivedGid() for each one would rescan the store once
// per id.  Index both durable storage locations once per collection pass;
// filenames are parsed with the same strict shape used by the DLC tests and
// invalid/corrupt files never qualify an id.
std::unordered_set<uint32_t> collectValidManifestDepotIds()
{
	std::unordered_set<uint32_t> depotIds;

	auto scanDirectory = [&depotIds](const std::filesystem::path& directory)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(directory, ec) || ec) return;

		std::filesystem::directory_iterator it(directory, ec);
		const std::filesystem::directory_iterator end;
		while (!ec && it != end)
		{
			const auto path = it->path();
			uint32_t depotId = 0;
			if (depotIdFromManifestName(path.filename().string(), depotId) &&
			    ManifestStoreIO::isValidManifest(path))
			{
				depotIds.insert(depotId);
			}
			it.increment(ec);
		}
	};

	scanDirectory(ManifestStore::dir());
	const auto root = steamRootForConfig();
	if (!root.empty())
	{
		scanDirectory(std::filesystem::path(root) / "depotcache");
	}
	return depotIds;
}

// Inject a `CompatToolMapping` entry for each app in g_needProton into
// `config/config.vdf`, so Steam runs them through Proton.  Best-effort,
// text-level edit (same approach as DepotKey::disableShaderCache).  Only
// adds entries that are missing; never overwrites a user's existing
// choice.  Each entry uses the user's default Steam Play tool (the "0" key
// of CompatToolMapping); Proton Experimental is only the fallback when no
// default is configured.
bool injectProtonMappings()
{
	if (g_needProton.empty()) return true;
	const auto root = steamRootForConfig();
	if (root.empty()) return false;
	const auto path = root + "/config/config.vdf";
	if (!std::filesystem::exists(path)) return false;

	// This function is restricted to setup()'s preinit window. Runtime PICS
	// workers persist a pending set instead; Steam's ConfigStore writers do
	// not participate in our advisory lock, so a stat-then-rename check here
	// would still be a TOCTOU race against Steam.
	(void)getCacheDir();
	const auto configLockPath = cacheLockPath();
	ProcessLock::FileLock configLock(configLockPath, false);
	if (!configLock.acquired())
	{
		g_pLog->debug("AppInfoProvision: config.vdf writer lock is busy\n");
		return false;
	}

	AtomicFile::FileIdentity expected{};
	if (!AtomicFile::readIdentity(path, expected)) return false;

	std::string content;
	{
		std::ifstream ifs(path);
		if (!ifs.is_open()) return false;
		std::stringstream ss; ss << ifs.rdbuf();
		content = ss.str();
	}

	// Locate (or create) the CompatToolMapping block under
	// InstallConfigStore/Software/Valve/Steam.
	std::size_t mapPos = content.find("\"CompatToolMapping\"");
	std::size_t mapBrace = std::string::npos;
	if (mapPos != std::string::npos)
	{
		mapBrace = content.find('{', mapPos);
	}
	else
	{
		// Insert a fresh CompatToolMapping block right after the
		// "Steam" object's opening brace.
		const auto steamPos = content.find("\"Steam\"");
		if (steamPos == std::string::npos) return false;
		const auto steamBrace = content.find('{', steamPos);
		if (steamBrace == std::string::npos) return false;
		const std::string block =
			"\n\t\t\t\t\t\"CompatToolMapping\"\n\t\t\t\t\t{\n\t\t\t\t\t}";
		content.insert(steamBrace + 1, block);
		mapPos = content.find("\"CompatToolMapping\"");
		mapBrace = content.find('{', mapPos);
	}
	if (mapBrace == std::string::npos) return false;

	// Honour the user's default Steam Play compatibility tool (Settings ->
	// Compatibility -> Default compatibility tool), stored as the special
	// "0" key in this same block.  Fall back to Proton Experimental only
	// when the user has not chosen a default.
	std::string toolName = CompatTool::parseDefaultTool(content);
	if (toolName.empty()) toolName = "proton_experimental";

	int added = 0;
	for (uint32_t appId : g_needProton)
	{
		const std::string key = "\"" + std::to_string(appId) + "\"";
		// Already mapped (by us or the user)?  Search only within the
		// mapping block to avoid matching the same id elsewhere.
		// Cheap: search from mapBrace forward; CompatToolMapping is
		// near the end of the file in practice.
		if (content.find(key, mapBrace) != std::string::npos)
		{
			continue;
		}
		const std::string entry =
			"\n\t\t\t\t\t\t" + key + "\n\t\t\t\t\t\t{\n"
			"\t\t\t\t\t\t\t\"name\"\t\t\"" + toolName + "\"\n"
			"\t\t\t\t\t\t\t\"config\"\t\t\"\"\n"
			"\t\t\t\t\t\t\t\"priority\"\t\t\"250\"\n"
			"\t\t\t\t\t\t}";
		content.insert(mapBrace + 1, entry);
		++added;
	}

	if (added == 0) return true;

	std::string writeError;
	if (!AtomicFile::writeIfUnchanged(path, expected, content, writeError))
	{
		g_pLog->debug(
		    "AppInfoProvision: config.vdf changed before conditional publish: %s\n",
		    writeError.c_str());
		return false;
	}
	g_pLog->infoOnce("AppInfoProvision: injected %d Proton CompatToolMapping entr%s (tool=%s) into config.vdf\n",
	             added, added == 1 ? "y" : "ies", toolName.c_str());
	return true;
}

// ---------------------------------------------------------------------------
// Native CM provider: parse the wire-text VDF buffer the anonymous CM
// returns into the same YAML::Node shape extractAppNode produces from
// steamcmd's JSON, so it flows through the identical prune/render/persist
// path.  The CM buffer is `"appinfo" { ... }` KV1 text;
// we parse its inner body into a map.
// ---------------------------------------------------------------------------

// Minimal KV1-text reader: builds a YAML::Node tree from a VDF-text body.
// `p`/`end` bracket the buffer.  Returns the node for the object whose
// opening brace has already been consumed by the caller (or, at top
// level, the single "appinfo" wrapper's body).  Defensive: bails to an
// empty node on malformed input.
class CmVdfReader
{
public:
	CmVdfReader(const char* p, const char* end) : p_(p), end_(end) {}

	// Parse the top-level `"appinfo" { ... }` and return the inner body
	// node (equivalent to steamcmd's data[appid]).  Empty on failure.
	YAML::Node parseAppinfo()
	{
		std::string key;
		Tok t = next(key);
		if (t != Tok::String) return YAML::Node(YAML::NodeType::Undefined);
		t = next(key /*reused as scratch*/);
		// After the top key we expect an opening brace.
		if (t != Tok::OpenBrace) return YAML::Node(YAML::NodeType::Undefined);
		return parseObject();
	}

private:
	enum class Tok { String, OpenBrace, CloseBrace, End };

	YAML::Node parseObject()
	{
		YAML::Node node(YAML::NodeType::Map);
		std::string key;
		for (;;)
		{
			Tok t = next(key);
			if (t == Tok::CloseBrace || t == Tok::End) break;
			if (t != Tok::String) break; // malformed
			std::string val;
			Tok vt = next(val);
			if (vt == Tok::OpenBrace)
			{
				node[key] = parseObject();
			}
			else if (vt == Tok::String)
			{
				node[key] = val;
			}
			else
			{
				break; // malformed
			}
		}
		return node;
	}

	Tok next(std::string& out)
	{
		out.clear();
		skipWs();
		if (p_ >= end_) return Tok::End;
		const char c = *p_;
		if (c == '{') { ++p_; return Tok::OpenBrace; }
		if (c == '}') { ++p_; return Tok::CloseBrace; }
		if (c == '"') return readQuoted(out);
		return readBare(out);
	}

	void skipWs()
	{
		while (p_ < end_)
		{
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p_; continue; }
			if (c == '/' && p_ + 1 < end_ && p_[1] == '/')
			{
				while (p_ < end_ && *p_ != '\n') ++p_;
				continue;
			}
			break;
		}
	}

	Tok readQuoted(std::string& out)
	{
		++p_;
		while (p_ < end_)
		{
			const char c = *p_++;
			if (c == '"') return Tok::String;
			if (c == '\\' && p_ < end_)
			{
				const char e = *p_++;
				switch (e)
				{
					case 'n':  out.push_back('\n'); break;
					case 't':  out.push_back('\t'); break;
					case 'r':  out.push_back('\r'); break;
					case '"':  out.push_back('"');  break;
					case '\\': out.push_back('\\'); break;
					default:   out.push_back(e);    break;
				}
				continue;
			}
			out.push_back(c);
		}
		return Tok::End;
	}

	Tok readBare(std::string& out)
	{
		while (p_ < end_)
		{
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
			    c == '{' || c == '}' || c == '"') break;
			out.push_back(*p_++);
		}
		return Tok::String;
	}

	const char* p_;
	const char* end_;
};

bool readCacheMetadataFile(
	uint32_t appId,
	std::string& storage,
	cache::CacheMetadataView& metadata,
	std::string* diag = nullptr)
{
	const auto reject = [&](const char* message)
	{
		if (diag) *diag = message;
		return false;
	};

	std::ifstream input(getMetaPath(appId), std::ios::binary | std::ios::ate);
	if (!input.is_open()) return reject("metadata is missing");
	const std::streamsize rawSize = input.tellg();
	if (rawSize <= 0 || rawSize > (64LL << 10))
		return reject("metadata size is outside the accepted range");

	storage.assign(static_cast<std::size_t>(rawSize), '\0');
	input.seekg(0, std::ios::beg);
	if (!input.read(storage.data(), rawSize) || input.gcount() != rawSize)
		return reject("metadata read failed");
	if (!cache::parseCacheMetadata(storage, metadata))
		return reject("metadata format is invalid");
	return true;
}

bool cacheMetadataMarkerAllowsRead(
	uint32_t appId,
	const cache::CacheMetadataView& metadata)
{
	{
		std::lock_guard<std::mutex> invalidationLock(g_cacheReadInvalidationMu);
		if (g_cacheReadInvalidated.count(appId) != 0)
			return false;
	}
	const bool markerPresent = SynthMark::isMarked(getCacheDir(), appId);
	return cache::syntheticMarkerStateConsistent(
		metadata.hasSynthetic, metadata.synthetic, markerPresent);
}

bool readValidatedCacheBufferLocked(uint32_t appId, std::string& wireOut,
                                    std::string& diag)
{
	try
	{
		std::string metadataText;
		cache::CacheMetadataView metadata;
		if (!readCacheMetadataFile(
				appId, metadataText, metadata, &diag))
		{
			return false;
		}
		if (!cacheMetadataMarkerAllowsRead(appId, metadata))
		{
			diag = "synthetic marker state is inconsistent";
			return false;
		}
		const std::string declaredSha = std::string(
		    base64::from_base64(std::string(metadata.shaBase64)));

		std::ifstream ifs(getBufferPath(appId), std::ios::binary | std::ios::ate);
		if (!ifs.is_open()) { diag = "buffer is missing"; return false; }
		const std::streamsize rawSize = ifs.tellg();
		if (rawSize <= 0 || rawSize > (16LL << 20))
		{
			diag = "buffer size is outside the accepted range";
			return false;
		}
		std::string wire(static_cast<size_t>(rawSize), '\0');
		ifs.seekg(0, std::ios::beg);
		if (!ifs.read(wire.data(), rawSize))
		{
			diag = "buffer read failed";
			return false;
		}

		std::uint8_t digestBytes[20]{};
		sha1BytesInternal(wire.data(), wire.size(), digestBytes);
		const std::string actualSha(
		    reinterpret_cast<const char*>(digestBytes), sizeof(digestBytes));

		CmVdfReader reader(wire.data(), wire.data() + wire.size());
		const YAML::Node appNode = reader.parseAppinfo();
		const bool parsed = appNode && appNode.IsMap() && appNode.size() > 0;
		const cache::CacheRecordFacts facts{
		    .requestedAppId = appId,
		    .metadataAppId = metadata.appId,
		    .declaredSize = metadata.wireSize,
		    .actualSize = wire.size(),
		    .shaSize = declaredSha.size(),
		    .shaMatches = declaredSha == actualSha,
		    .parsed = parsed,
		    .hasUsableContent = parsed && hasUsableContentDepot(appNode),
		};
		if (!cache::isCacheRecordValid(facts))
		{
			diag = "metadata, SHA-1, or depot validation failed";
			return false;
		}
		wireOut = std::move(wire);
		return true;
	}
	catch (const std::exception& e)
	{
		diag = e.what();
		return false;
	}
}

bool hasValidatedCachedBuffer(uint32_t appId, std::string& diag)
{
	std::string wire;
	return readValidatedCacheBufferLocked(appId, wire, diag);
}

bool cachedWireSizeMatches(uint32_t appId, long long actualSize)
{
	if (actualSize <= 0) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	return readCacheMetadataFile(appId, metadataText, metadata) &&
		cache::wireSizeMatches(
			static_cast<unsigned long long>(actualSize), metadata.wireSize);
}

cache::CacheUse cacheUseForApp(uint32_t appId, bool refreshUnavailable)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return cache::CacheUse::None;

	cache::CacheValidationKey key{};
	const bool present = statBuffer(appId, key);
	if (!present) return cache::CacheUse::None;

	const long long now = static_cast<long long>(std::time(nullptr));
	const bool fresh = cache::isBufferReusable(
	    present, key.mtimeSecs, now, provisionTtlSecs());
	if (!cache::shouldValidateCache(fresh, refreshUnavailable))
	{
		// A stale online buffer will be refreshed; do not parse YAML, hash the
		// whole wire or build a VDF tree just to discard it below.
		return cache::CacheUse::None;
	}

	// Cheap metadata gate before the expensive integrity/structure check.
	if (!cachedWireSizeMatches(appId, key.size))
	{
		g_pLog->info("AppInfoProvision: app=%u cached buffer rejected (wire_size mismatch)\n",
		             appId);
		return cache::CacheUse::None;
	}

	CacheValidationResult result;
	{
		std::lock_guard<std::mutex> lk(g_cacheValidationMu);
		auto it = g_cacheValidationMemo.find(key);
		if (it != g_cacheValidationMemo.end())
		{
			result = it->second;
		}
		else
		{
			result.valid = hasValidatedCachedBuffer(appId, result.diag);
			g_cacheValidationMemo.emplace(key, result);
		}
	}
	if (result.valid && !cacheMarkerAllowsRead(appId))
	{
		result.valid = false;
		result.diag = "synthetic marker is missing";
	}
	if (!result.valid)
	{
		g_pLog->info("AppInfoProvision: app=%u cached buffer rejected (%s)\n",
		             appId, result.diag.c_str());
	}
	return cache::chooseCacheUse(result.valid, fresh, refreshUnavailable);
}

// A warm cache is a complete, validated pair.  statBuffer() alone is not
// enough: persistBuffer() publishes the binary before its YAML metadata, so
// an interrupted metadata write must remain a cold-start case rather than
// silently deferring the first usable buffer to the next Steam restart.
bool hasReadyCacheOnDisk(uint32_t appId)
{
	const bool bufferExists = hasBufferOnDisk(appId);
	bool metadataExists = false;
	{
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (!cacheLock.acquired()) return false;
		struct stat st{};
		metadataExists = stat(getMetaPath(appId).c_str(), &st) == 0 &&
		                 st.st_size > 0;
	}
	if (!bufferExists || !metadataExists) return false;

	// Only a complete, fresh record is warm for startup. A stale record may
	// be used as an offline fallback after a provider failure, but it must not
	// be spliced into async startup before the live refresh has run.
	const bool recordFresh =
	    cacheUseForApp(appId, false) == cache::CacheUse::Fresh;
	return cachePairReady(bufferExists, metadataExists, recordFresh);
}

// Classify what is on disk for one app, separating "unusable" from "usable but
// past the freshness window" so each caller can apply its own policy.
CacheReadiness cacheReadinessOnDisk(uint32_t appId)
{
	const bool bufferExists = hasBufferOnDisk(appId);
	bool metadataExists = false;
	{
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (!cacheLock.acquired()) return CacheReadiness::Missing;
		struct stat st{};
		metadataExists = stat(getMetaPath(appId).c_str(), &st) == 0 &&
		                 st.st_size > 0;
	}
	if (!bufferExists || !metadataExists) return CacheReadiness::Missing;

	// `refreshUnavailable = true` asks the question this classification needs:
	// is the pair itself valid? Fresh answers Fresh; a valid pair past the TTL
	// answers Fallback, which is precisely the ValidStale case.
	switch (cacheUseForApp(appId, true))
	{
		case cache::CacheUse::Fresh:    return CacheReadiness::Fresh;
		case cache::CacheUse::Fallback: return CacheReadiness::ValidStale;
		default:                        return CacheReadiness::Missing;
	}
}

// Render+prune+sha+persist a parsed appinfo node (shared tail used by
// both the CM and steamcmd paths). `changeNumber` is the PICS/JSON change
// number for the meta record.
SourceResult renderAndPersist(uint32_t appId, const YAML::Node& appNode,
                              uint32_t changeNumber,
                              const CachePublicationToken& publication)
{
	std::string wire;
	bool synthesized = false;
	const SourceResult renderResult = renderAppinfoBuffer(
	    appNode, appId, wire, publication, &synthesized);
	if (renderResult != SourceResult::Success)
	{
		const char* reason = "invalid response";
		if (renderResult == SourceResult::IncompleteContent)
			reason = "response contains no concrete depot data";
		else if (renderResult == SourceResult::NoUsableContent)
			reason = "concrete depots are not usable";
		else if (renderResult == SourceResult::VirtualDlc)
			reason = "DLC has no usable content depots";
		g_pLog->info("AppInfoProvision: app=%u render stopped (%s)\n", appId, reason);
		return renderResult;
	}
	if (wire.find("\"depots\"") == std::string::npos)
	{
		g_pLog->info("AppInfoProvision: app=%u buffer has no depots, skipping\n", appId);
		return SourceResult::IncompleteContent;
	}

	std::string sha20;
	{
		std::uint8_t tmp[20];
		sha1BytesInternal(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	if (!persistBuffer(appId, changeNumber, sha20, wire, publication,
	                   synthesized))
	{
		g_pLog->info("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return SourceResult::LocalFailure;
	}

	g_pLog->infoOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	             appId, changeNumber, wire.size());
	return SourceResult::Success;
}

// Provision one app from a native-CM wire buffer. Mirrors the steamcmd
// path's tail but skips the JSON parse — the CM buffer is already wire VDF.
SourceResult provisionAppFromCmBuffer(uint32_t appId, const std::string& cmWire,
                                      uint32_t changeNumber,
                                      const CachePublicationToken& publication)
{
	if (cmWire.empty()) return SourceResult::InvalidResponse;
	CmVdfReader reader(cmWire.data(), cmWire.data() + cmWire.size());
	YAML::Node appNode = reader.parseAppinfo();
	if (!appNode || !appNode.IsMap() || appNode.size() == 0)
	{
		g_pLog->info("AppInfoProvision: app=%u CM buffer parse failed, fallback\n", appId);
		return SourceResult::InvalidResponse;
	}
	return renderAndPersist(appId, appNode, changeNumber, publication);
}

struct ProvisionPassContext
{
	std::unordered_map<uint32_t, std::string> cmBuffers;
	std::unordered_map<uint32_t, uint32_t> cmChanges;
	std::unordered_map<uint32_t, CachePublicationToken> cachePublications;
};

struct PendingProtonLoad
{
	bool lockAcquired = false;
	PendingProtonFileStatus status = PendingProtonFileStatus::Invalid;
	std::set<uint32_t> fileIds;
	std::set<uint32_t> ids;
};

PendingProtonLoad loadPendingProtonMappings()
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return {};

	const auto file = readPendingProtonFileLocked();
	PendingProtonLoad result;
	result.lockAcquired = true;
	result.status = file.status;
	result.fileIds = file.ids;
	if (file.status != PendingProtonFileStatus::Valid) return result;

	const auto managed = g_config.managedAppIds.get();
	for (uint32_t appId : file.ids)
	{
		if (managed.count(appId) != 0)
			result.ids.insert(appId);
	}
	return result;
}

bool removePendingProtonMappingLocked(uint32_t appId)
{
	const auto file = readPendingProtonFileLocked();
	if (file.status == PendingProtonFileStatus::Missing) return true;
	if (file.status != PendingProtonFileStatus::Valid)
	{
		if (g_pLog)
			g_pLog->debug("AppInfoProvision: preserving invalid or unreadable pending Proton file while removing app=%u\n",
		              appId);
		return false;
	}

	std::set<uint32_t> pending = file.ids;
	if (pending.erase(appId) == 0) return true;

	if (pending.empty())
	{
		std::error_code ec;
		std::filesystem::remove(pendingProtonPath(), ec);
		return !ec;
	}

	std::string content;
	for (uint32_t pendingAppId : pending)
		content += std::to_string(pendingAppId) + "\n";
	std::string error;
	const bool removed = AtomicFile::write(pendingProtonPath(), content, error);
	if (!removed && g_pLog)
	{
		g_pLog->debug("AppInfoProvision: cannot remove pending Proton mapping for app=%u: %s\n",
		              appId, error.c_str());
	}
	return removed;
}

// Remove a successfully applied record only if no writer changed it after the
// initial locked read. This closes the gap between load/apply and cleanup
// without holding the cache lock across the config.vdf rewrite.
bool clearPendingProtonMappingsIfUnchanged(
    const std::set<uint32_t>& expectedFileIds)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return false;

	const auto current = readPendingProtonFileLocked();
	if (current.status != PendingProtonFileStatus::Valid ||
	    current.ids != expectedFileIds)
	{
		if (g_pLog)
			g_pLog->debug(
			    "AppInfoProvision: pending Proton file changed during flush; preserving it\n");
		return false;
	}

	std::error_code ec;
	std::filesystem::remove(pendingProtonPath(), ec);
	return !ec;
}

bool isSynthesizedAppLocked(uint32_t appId)
{
	if (appId == 0) return false;
	const bool active = g_config.isAddedAppId(appId);
	bool invalidated = false;
	{
		std::lock_guard<std::mutex> invalidationLock(g_cacheReadInvalidationMu);
		invalidated = g_cacheReadInvalidated.count(appId) != 0;
	}
	// Managed-source removal invalidates cache reads but may retain an active
	// compatibility app. Keep the live appinfo protected while its marker is
	// preserved; a later full removal clears both ownership and protection.
	if (invalidated && !active) return false;
	const bool markerPresent = SynthMark::isMarked(getCacheDir(), appId);
	if (!markerPresent) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	if (readCacheMetadataFile(appId, metadataText, metadata))
	{
		if (metadata.hasSynthetic)
			return metadata.synthetic;
		// Before explicit provenance metadata existed, the persisted marker
		// itself was the synthetic bit. Preserve that behavior for existing
		// installations instead of requiring migration on first boot.
		return true;
	}

	// Managed-source cleanup deliberately retains the marker after moving
	// the cache pair. An active compatibility app must remain protected by
	// that marker until a new publication or full removal reconciles it.
	std::error_code metadataError;
	const bool metadataPresent = std::filesystem::exists(
	    getMetaPath(appId), metadataError);
	return !metadataError &&
	       cache::retainedSyntheticMarkerProtectionAllowed(
	           markerPresent, metadataPresent, active);
}

} // namespace

void sha1Bytes(const void* data, std::size_t size, std::uint8_t out[20])
{
	sha1BytesInternal(data, size, out);
}

bool publishCachePairLocked(uint32_t appId, const std::string& wire,
                            const std::string& metadata, bool synthetic,
                            bool markerBefore, std::string& error)
{
	const auto cacheDir = getCacheDir();
	return CachePair::publish(
		getBufferPath(appId), getMetaPath(appId), wire, metadata, synthetic,
		markerBefore,
		[&](bool desired) {
			return desired ? SynthMark::mark(cacheDir, appId)
			               : SynthMark::unmark(cacheDir, appId);
		},
		[&] { return SynthMark::isMarked(cacheDir, appId); }, error);
}

bool readValidatedCacheBuffer(uint32_t appId, std::string& buffer)
{
	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired()) return false;
	std::string diag;
	return readValidatedCacheBufferLocked(appId, buffer, diag);
}

std::mutex& cachePublicationMutex()
{
	return g_cachePublicationMu;
}

std::uint64_t cachePublicationGenerationLocked(uint32_t appId)
{
	const auto it = g_cachePublicationGenerations.find(appId);
	return it == g_cachePublicationGenerations.end() ? 0 : it->second;
}

// Remember that this app cannot produce a cache pair, so later passes skip it
// instead of paying another CM round-trip for the same answer.
void noteTerminalProvisionResult(uint32_t appId)
{
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	g_terminalProvisionResults[appId] = cachePublicationGenerationLocked(appId);
}

bool terminalProvisionResultKnown(uint32_t appId)
{
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	const auto it = g_terminalProvisionResults.find(appId);
	return cache::terminalResultStillApplies(
	    it != g_terminalProvisionResults.end(),
	    it == g_terminalProvisionResults.end() ? 0 : it->second,
	    cachePublicationGenerationLocked(appId));
}

void forgetTerminalProvisionResult(uint32_t appId)
{
	std::lock_guard<std::mutex> lock(g_cachePublicationMu);
	g_terminalProvisionResults.erase(appId);
}

CachePublicationToken snapshotCachePublication(uint32_t appId)
{
	CachePublicationToken token;
	std::lock_guard<std::mutex> passLock(g_provisionPassMu);
	token.managed = g_config.managedAppIds.get().count(appId) != 0;
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	token.generation = cachePublicationGenerationLocked(appId);
	return token;
}

std::mutex& provisioningPassMutex()
{
	return g_provisionPassMu;
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ProvisionOutcome provisionAppDetailed(uint32_t appId,
                                      const std::string& appinfoVdfPath,
                                      ProvisionPassState& pass,
                                      ProvisionPassContext& context)
{
	(void)appinfoVdfPath;
	if (appId == 0) return ProvisionOutcome::IncompleteContent;

	auto publicationIt = context.cachePublications.find(appId);
	if (publicationIt == context.cachePublications.end())
	{
		publicationIt = context.cachePublications.emplace(
		    appId, snapshotCachePublication(appId)).first;
	}
	const CachePublicationToken publication = publicationIt->second;

	// Short-lived on-disk cache to tame startup cost.  Steam re-execs
	// setup() several times during a single cold boot (observed 4x on
	// the Zorin VM), and each pass would otherwise issue one synchronous
	// HTTP GET per AddedApp — so the boot cost grew O(n_apps * n_passes)
	// and stalled Steam's launch the more games the user added.
	//
	// If we already wrote picsbuffer_<appid>.bin within the (short) TTL,
	// reuse it and skip the network: the buffer the earlier pass produced
	// reflects the SAME live state (DepotKeys; pins are disabled), so the
	// AppInfoVdf splice — idempotent on (appid, change, sha) — is a no-op
	// the second time anyway.  The TTL is deliberately short so a genuine
	// relaunch (minutes/hours later, > TTL) re-fetches the live public
	// gid; we must NOT serve a stale cross-session buffer, or we'd
	// reintroduce the staged-gid vs requested-gid mismatch the
	// install-first-attempt fix resolved.
	if (cacheUseForApp(appId, false) == cache::CacheUse::Fresh)
	{
		g_pLog->debug("AppInfoProvision: app=%u reusing validated same-boot cache\n",
		              appId);
		return ProvisionOutcome::FreshCache;
	}

	// Native CM batch result (fetched once per provisionAllAddedApps pass,
	// directly from Valve — the PRIMARY source).  Falls through to the
	// steamcmd.net HTTP chain below if this app wasn't in the batch (CM
	// failed, or it was provisioned individually).
	{
		auto it = context.cmBuffers.find(appId);
		if (it != context.cmBuffers.end())
		{
			uint32_t cn = 0;
			if (auto ci = context.cmChanges.find(appId); ci != context.cmChanges.end())
				cn = ci->second;
			const SourceResult cmResult =
			    provisionAppFromCmBuffer(appId, it->second, cn, publication);
			if (cmResult == SourceResult::Success)
			{
				g_pLog->info("AppInfoProvision: app=%u provisioned via CM\n", appId);
				return ProvisionOutcome::Updated;
			}
			if (!shouldTryProviderFallback(cmResult))
			{
				g_pLog->info(
				    "AppInfoProvision: app=%u CM result is terminal (%s); "
				    "provider fallback suppressed\n", appId,
				    cmResult == SourceResult::NoUsableContent
				        ? "concrete depots are not usable"
				        : cmResult == SourceResult::VirtualDlc
				            ? "DLC has no usable content depots"
				            : "local cache write failed");
				// A content verdict is a property of the app, not of this
				// attempt: repeating it costs a CM round-trip and returns the
				// same answer. A local write failure is NOT terminal in that
				// sense — the next pass may well succeed — so it stays
				// retryable.
				if (cmResult == SourceResult::NoUsableContent ||
				    cmResult == SourceResult::VirtualDlc)
					noteTerminalProvisionResult(appId);
				if (cmResult == SourceResult::VirtualDlc)
					return ProvisionOutcome::NotApplicable;
				return cmResult == SourceResult::LocalFailure
				    ? ProvisionOutcome::LocalFailure
				    : ProvisionOutcome::IncompleteContent;
			}
			g_pLog->info("AppInfoProvision: app=%u CM response %s, trying steamcmd\n",
			             appId,
			             cmResult == SourceResult::IncompleteContent
			                 ? "contains no concrete depot data"
			                 : "is invalid");
		}
	}

	if (!pass.shouldAttemptProvider())
	{
		if (cacheUseForApp(appId, true) == cache::CacheUse::Fallback)
		{
			g_pLog->info("AppInfoProvision: app=%u using validated cached buffer "
			             "because live sources are unavailable\n", appId);
			return ProvisionOutcome::FallbackCache;
		}
		return ProvisionOutcome::NetworkUnavailable;
	}

	std::string body, diag;
	std::string url;
	bool fetched = false;
	NetworkFailure finalFailure = NetworkFailure::Provider;
	for (const auto& tmpl : providerChain())
	{
		url = expandUrl(tmpl, appId);
		g_pLog->info("AppInfoProvision: app=%u GET %s\n", appId, url.c_str());

		// Retry transient failures (timeouts, 5xx, cold-network DNS) with
		// a bounded linear backoff.  A single steamcmd.net timeout used to
		// leave the app unprovisioned for the whole session unless Steam
		// happened to re-exec setup(); the larger an app's product-info
		// JSON is, the more likely the 30s total-transfer timeout trips on
		// a slow first request (observed: Outlast 238320's 8-depot JSON
		// timed out while the smaller 2262770 succeeded in the same pass).
		finalFailure = retryNetworkOperation(
			[&] {
				return httpGetJson(
				    url, body, diag,
				    pass.providerOperationTimeoutMs(/*operationCapMs=*/12000));
			},
			/*maxAttempts=*/3, /*baseDelayMs=*/1000,
			[&](int ms) {
				const long delay = pass.providerDelayMs(ms);
				if (delay > 0)
					std::this_thread::sleep_for(std::chrono::milliseconds(delay));
			});
		if (finalFailure == NetworkFailure::None)
		{
			fetched = true;
			pass.noteProviderSuccess();
			break;
		}

		// Use info, not warn: warn fires a critical notify-send popup
		// (CLog ctor configures urgency=critical for warn).  A single
		// provider exhausting its retries isn't user-actionable noise.
		g_pLog->info("AppInfoProvision: app=%u provider failed after retries (%s), trying next\n",
		             appId, diag.c_str());
		if (pass.providerBudgetExhausted()) break;
	}
	if (!fetched)
	{
		g_pLog->info("AppInfoProvision: app=%u all providers failed\n", appId);
		pass.noteFinalProviderFailure(finalFailure);
		if (pass.providerCircuitOpen() &&
		    cacheUseForApp(appId, true) == cache::CacheUse::Fallback)
		{
			g_pLog->info("AppInfoProvision: app=%u using validated cached buffer "
			             "after provider transport failure\n", appId);
			return ProvisionOutcome::FallbackCache;
		}
		return pass.providerCircuitOpen()
		    ? ProvisionOutcome::NetworkUnavailable
		    : ProvisionOutcome::IncompleteContent;
	}

	YAML::Node appNode;
	std::string err;
	if (!extractAppNode(body, appId, appNode, err))
	{
		g_pLog->info("AppInfoProvision: app=%u parse failed: %s\n", appId, err.c_str());
		return ProvisionOutcome::IncompleteContent;
	}

	std::string wire;
	bool synthesized = false;
	const SourceResult renderResult = renderAppinfoBuffer(
	    appNode, appId, wire, publication, &synthesized);
	if (renderResult != SourceResult::Success)
	{
		const char* reason = "invalid response";
		if (renderResult == SourceResult::IncompleteContent)
			reason = "response contains no concrete depot data";
		else if (renderResult == SourceResult::NoUsableContent)
			reason = "concrete depots are not usable";
		else if (renderResult == SourceResult::VirtualDlc)
			reason = "DLC has no usable content depots";
		g_pLog->info("AppInfoProvision: app=%u render stopped (%s)\n", appId, reason);
		if (renderResult == SourceResult::VirtualDlc)
			return ProvisionOutcome::NotApplicable;
		return renderResult == SourceResult::LocalFailure
		    ? ProvisionOutcome::LocalFailure
		    : ProvisionOutcome::IncompleteContent;
	}

	// Spot-check: the wire must contain the depots block, otherwise the
	// upstream JSON itself is stripped (rare, but happens for retired
	// titles).  Skip the splice in that case rather than persist a
	// useless entry.
	if (wire.find("\"depots\"") == std::string::npos)
	{
		g_pLog->info("AppInfoProvision: app=%u JSON has no depots, skipping\n", appId);
		return ProvisionOutcome::IncompleteContent;
	}

	// Manifest-GID pins are DELIBERATELY NOT applied to the provisioned wire
	// buffer.
	//
	// We used to rewrite every provisioned buffer's public gid to the
	// pinned gid.  That is structurally defeated by Steam: when the user
	// clicks Install, Steam issues a `RequestAppInfoUpdate` that
	// downloads fresh product-info over HTTP and OVERWRITES our
	// appinfo.vdf entry with the live public gid (confirmed in
	// appinfo_log.txt).  Steam then plans the install with the live
	// public gid, not our pin.  So pinning the provisioned buffer only
	// caused a mismatch: we pre-staged the pinned manifest, Steam asked
	// for the live one, BYldRequestDepotManifest got called and returned
	// "Access Denied" -> first-attempt "No connection".
	//
	// Depot decryption keys are per-DEPOT, not per-manifest, so the live
	// public build decrypts and installs fine with the same key (verified
	// on the VM: Gang Beasts depot 285903 committed successfully with the
	// live gid).  Provisioning the live public gid therefore means the
	// gid we pre-stage in PICS recv == the gid Steam requests == BYld is
	// skipped == first-attempt install succeeds.
	//
	// ManifestPins are applied later, after Steam has built the plan:
	// BuildDepotDependency rewrites each DepotEntry using its AppId, and
	// ReconcilePin patches the in-memory TARGET vectors.  This applies to
	// every configured app-scoped pin.  lockedApps remains separate and only
	// controls update suppression in Apps::shouldDisableUpdates.

	// Compute sha[20] over the FINAL wire buffer (after prune).
	//
	// We must NOT reuse SteamCMD's `_sha`: that hash describes the
	// upstream, unmodified product-info, but we've dropped depots and
	// repointed manifest GIDs.  AppInfoVdf::injectApp treats
	// (appid, change_number, sha) as an idempotency key and skips the
	// rewrite when all three match an existing entry.  If we kept the
	// upstream sha, a previously-injected full-depot entry would never
	// be replaced by our pruned one — the on-disk appinfo.vdf would
	// keep stale depots and Steam would show 0 B.  Hashing our own
	// bytes guarantees the key changes whenever our output changes.
	std::string sha20;
	{
		std::uint8_t tmp[20];
		sha1BytesInternal(wire.data(), wire.size(), tmp);
		sha20.assign(reinterpret_cast<const char*>(tmp), 20);
	}

	const uint32_t changeNumber = pickChangeNumber(appNode);

	if (!persistBuffer(appId, changeNumber, sha20, wire, publication,
	                   synthesized))
	{
		g_pLog->info("AppInfoProvision: app=%u failed to persist buffer to cache\n", appId);
		return ProvisionOutcome::LocalFailure;
	}

	g_pLog->infoOnce("AppInfoProvision: app=%u provisioned (change=%u, %zu bytes wire)\n",
	             appId, changeNumber, wire.size());
	return ProvisionOutcome::Updated;
}

bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath)
{
	ProvisionPassState pass;
	ProvisionPassContext context;
	return isProvisioned(provisionAppDetailed(
		appId, appinfoVdfPath, pass, context));
}

int provisionAppsPass(const std::string& appinfoVdfPath,
                       const std::unordered_set<uint32_t>& added,
                       bool onlyMissing, ProvisionPassContext& context,
                       std::unordered_set<uint32_t>* fallbackApps)
{
	if (added.empty()) return 0;

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	coordinator.snapshot([&] {
		const auto managed = g_config.managedAppIds.get();
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		context.cachePublications.clear();
		for (const uint32_t appId : added)
		{
			context.cachePublications.emplace(
			    appId,
			    CachePublicationToken{
			        .managed = managed.count(appId) != 0,
			        .generation = cachePublicationGenerationLocked(appId),
			    });
		}
	});
	// PRIMARY source: one batched anonymous-CM product-info request to
	// Valve for the whole fleet (≈0.3s for dozens of apps; replaces the
	// per-app steamcmd.net round-trips).  Best-effort: any miss falls
	// through to the steamcmd.net HTTP chain inside provisionApp.  Skip
	// only those apps whose buffer is still fresh on disk (the cache TTL
	// would short-circuit them anyway), so a warm relaunch makes no CM
	// request at all.  Disable entirely via SLSSTEAM_DISABLE_CM=1.
	ProvisionPassState pass;
	const bool cmDisabled = [] {
		const char* v = std::getenv("SLSSTEAM_DISABLE_CM");
		return v && *v && std::string(v) != "0";
	}();
	if (!cmDisabled)
	{
		std::vector<uint32_t> toFetch;
		for (uint32_t appId : added)
		{
			if (onlyMissing)
			{
				if (!hasReadyCacheOnDisk(appId)) toFetch.push_back(appId);
			}
			else if (cacheUseForApp(appId, false) != cache::CacheUse::Fresh)
			{
				toFetch.push_back(appId);
			}
		}
		if (!toFetch.empty())
		{
			g_pLog->info("AppInfoProvision: fetching %zu app(s) via native CM\n",
			             toFetch.size());
			const auto cmResult = coordinator.network([&] {
				return CmClient::fetchProductInfoDetailed(
				    toFetch, context.cmBuffers, &context.cmChanges);
			});
			if (cmResult != CmClient::FetchResult::Success)
			{
				context.cmBuffers.clear();
				context.cmChanges.clear();
				pass.noteCmBatchFailure();
				if (cmResult == CmClient::FetchResult::NetworkUnavailable)
				{
					pass.noteFinalProviderFailure(NetworkFailure::Connectivity);
					g_pLog->info("AppInfoProvision: native CM batch found no network; "
					             "using validated local buffers\n");
				}
				else
				{
					g_pLog->info("AppInfoProvision: native CM batch failed, "
					             "probing provider fallback\n");
				}
			}
		}
	}

	int provisioned = 0;
	auto appIt = added.begin();
	while (appIt != added.end())
	{
		const uint32_t appId = *appIt++;
		const ProvisionOutcome outcome =
		    provisionAppDetailed(appId, appinfoVdfPath, pass, context);
		if (fallbackApps && outcome == ProvisionOutcome::FallbackCache)
			fallbackApps->insert(appId);
		if (isProvisioned(outcome))
		{
			++provisioned;
		}

		// A successful fallback proves that connectivity returned after the
		// initial CM attempt. Give the primary source one recovery batch for
		// every app still pending; success keeps the rest of the fleet off the
		// slower per-app mirror.
		if (pass.takeCmRecoveryRequest() && appIt != added.end())
		{
			std::vector<uint32_t> remaining(appIt, added.end());
			g_pLog->info("AppInfoProvision: provider reachable; retrying native CM "
			             "for %zu remaining app(s)\n", remaining.size());
			const auto recovery = coordinator.network([&] {
				return CmClient::fetchProductInfoDetailed(
				    remaining, context.cmBuffers, &context.cmChanges);
			});
			if (recovery == CmClient::FetchResult::Success)
			{
				g_pLog->info("AppInfoProvision: native CM recovered for remaining apps\n");
			}
			else if (recovery == CmClient::FetchResult::NetworkUnavailable)
			{
				pass.noteFinalProviderFailure(NetworkFailure::Connectivity);
				g_pLog->info("AppInfoProvision: native CM recovery lost connectivity; "
				             "using validated local buffers\n");
			}
			else
			{
				g_pLog->info("AppInfoProvision: native CM recovery failed; "
				             "provider fallback remains active\n");
			}
		}

		const ProvisionNotice notice = noticeForOutcome(outcome);
		if (notice == ProvisionNotice::MetadataUnavailable)
		{
			if (pass.takeConnectivityNotice())
				g_pLog->notifyUser(UserMsg::GameMetadataUnavailable,
				                   std::to_string(appId));
		}
		else if (notice == ProvisionNotice::ReviewGameData)
		{
			g_pLog->notifyUser(UserMsg::GamePreparationFailed,
			                   std::to_string(appId));
		}
		else if (notice == ProvisionNotice::LocalStorage)
		{
			g_pLog->notifyUser(UserMsg::LocalStorageError);
		}
	}
	if (provisioned > 0)
	{
		g_pLog->info("AppInfoProvision: %d/%zu AdditionalApps provisioned\n",
		             provisioned, added.size());
	}

	// Drop the batch buffers; they can be large and are only needed for
	// this pass.
	context.cmBuffers.clear();
	context.cmChanges.clear();

	return provisioned;
}

int provisionApps(const std::string& appinfoVdfPath,
                  const std::unordered_set<uint32_t>& added,
                  bool onlyMissing)
{
	if (added.empty()) return 0;

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto addedSnapshot = coordinator.snapshot([&] { return added; });
	if (addedSnapshot.empty()) return 0;

	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		appinfoVdfPath, addedSnapshot, onlyMissing, context, nullptr);
	coordinator.commit([&] {
		persistPendingProtonMappings(false);
	});
	return provisioned;
}

int provisionAllAddedApps(const std::string& appinfoVdfPath,
                          bool allowConfigWrite)
{
	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto added = coordinator.snapshot(
		[] { return g_config.managedAppIds.get(); });
	if (added.empty()) return 0;

	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		appinfoVdfPath, added, false, context, nullptr);
	coordinator.commit([&] {
		if (allowConfigWrite)
		{
			if (!injectProtonMappings())
				persistPendingProtonMappings(true);
		}
		else
		{
			persistPendingProtonMappings(false);
		}
	});
	return provisioned;
}

int provisionColdStartApps(const std::string& appinfoVdfPath,
                           std::unordered_set<uint32_t>* sanitizedApps,
                           bool allowConfigWrite,
                           std::unordered_set<uint32_t>* fallbackApps)
{
	if (sanitizedApps) sanitizedApps->clear();
	if (fallbackApps) fallbackApps->clear();

	ProvisionPassCoordinator coordinator(g_provisionPassMu);
	const auto managedApps = coordinator.snapshot(
		[] { return g_config.managedAppIds.get(); });
	// The preinit pass wants the live gid before the splice, so a stale pair is
	// worth re-fetching there. The PICS callback runs on Steam's worker thread
	// while the user is interacting, so it must only rescue a pair that is
	// missing or invalid; refreshing stale pairs belongs to the async worker.
	const bool requireFresh = allowConfigWrite;
	std::unordered_set<uint32_t> cold;
	std::size_t skippedTerminal = 0;
	for (const uint32_t appId : managedApps)
	{
		if (!coldFallbackNeeded(cacheReadinessOnDisk(appId), requireFresh))
			continue;
		// Apps with a terminal content verdict can never satisfy this loop, so
		// including them would refetch the same answer on every pass and keep
		// the pass permanently incomplete.
		if (terminalProvisionResultKnown(appId))
		{
			++skippedTerminal;
			continue;
		}
		cold.insert(appId);
	}
	if (skippedTerminal != 0)
	{
		g_pLog->debug(
		    "AppInfoProvision: skipping %zu app(s) with a known terminal "
		    "content result\n", skippedTerminal);
	}
	if (cold.empty())
	{
		noteColdRetryOutcome(false);
		return 0;
	}
	if (coldRetryBlocked())
	{
		g_pLog->debug(
		    "AppInfoProvision: cold-start retry backoff active; skipping %zu app(s)\n",
		    cold.size());
		return 0;
	}

	g_pLog->info("AppInfoProvision: cold-start fallback for %zu app(s)\n",
	             cold.size());
	ProvisionPassContext context;
	const int provisioned = provisionAppsPass(
		appinfoVdfPath, cold, true, context, fallbackApps);
	bool unresolved = false;
	for (const uint32_t appId : cold)
	{
		const bool explicitFallback =
			fallbackApps && fallbackApps->count(appId) != 0;
		if (explicitFallback)
		{
			// Keep the explicit fallback allowlist separate from the PICS
			// suppression set. A fallback may be an explicit raw cache
			// (normalized=false), which must still be replaceable by a newer
			// response. persistAppBuffer() applies the final provenance guard
			// for normalized and legacy pairs.
			if (sanitizedApps &&
			    shouldMarkColdCacheSanitized(explicitFallback))
				sanitizedApps->insert(appId);
			continue;
		}
		if (!hasReadyCacheOnDisk(appId))
		{
			// A terminal verdict is resolved, not pending: there is nothing a
			// retry could produce. Counting it as unresolved armed the backoff
			// forever and made every later PICS response repeat the pass.
			if (!terminalProvisionResultKnown(appId)) unresolved = true;
			continue;
		}
		if (sanitizedApps &&
		    shouldMarkColdCacheSanitized(explicitFallback))
			sanitizedApps->insert(appId);
	}
	noteColdRetryOutcome(unresolved);
	if (unresolved)
	{
		g_pLog->debug(
		    "AppInfoProvision: cold-start fallback incomplete; subsequent PICS "
		    "responses are temporarily rate-limited\n");
	}

	coordinator.commit([&] {
		if (allowConfigWrite)
		{
			if (!injectProtonMappings())
				persistPendingProtonMappings(true);
		}
		else
		{
			persistPendingProtonMappings(false);
		}
	});
	return provisioned;
}

bool asyncProvisioningEnabled()
{
	return asyncProvisionEnabled(
		g_config.asyncProvision.get(),
		std::getenv("SLSSTEAM_ASYNC_PROVISION"));
}

void flushPendingProtonMappings()
{
	std::lock_guard<std::mutex> passLock(g_provisionPassMu);
	const auto loaded = loadPendingProtonMappings();
	if (!loaded.lockAcquired || loaded.status == PendingProtonFileStatus::Invalid)
	{
		// A lock/read/parse failure is not evidence that the file is empty.
		// Leave both the on-disk mappings and any in-memory retry state intact.
		return;
	}
	if (loaded.status == PendingProtonFileStatus::Missing)
		return;
	if (loaded.ids.empty())
	{
		// This also removes entries left behind by a previous config removal;
		// the valid file was filtered against current management above.
		(void)clearPendingProtonMappingsIfUnchanged(loaded.fileIds);
		return;
	}
	g_needProton.insert(loaded.ids.begin(), loaded.ids.end());
	if (injectProtonMappings())
		(void)clearPendingProtonMappingsIfUnchanged(loaded.fileIds);
}

enum class RefreshWorkerStartResult
{
	Started,
	NotStarted,
	Uncertain,
};

void publishRuntimeAppInfo(
    const std::string& appinfoVdfPath,
    const std::vector<std::uint32_t>& requested)
{
	const auto selected = selectRuntimePublishCandidates(
		requested, g_config.managedAppIds.get(), SynthMark::loadAll(getCacheDir()));
	if (selected.empty())
		return;

	// A pre-existing validated synthetic pair is an explicit fallback for this
	// just-added app even when its async freshness window elapsed.  The splice
	// remains transactional and AppInfoVdf revalidates the complete pair.
	const std::unordered_set<std::uint32_t> explicitFallback(
		selected.begin(), selected.end());
	if (AppInfoVdf::injectAllCached(appinfoVdfPath, explicitFallback) == 0)
	{
		g_pLog->warn(
			"AppInfoProvision: live synthetic appinfo splice failed for %zu app(s); "
			"restart remains available\n",
			selected.size());
		return;
	}

	const AppInfoReload::Result reloaded =
		AppInfoState::reloadFromDisk(selected);
	switch (reloaded.status)
	{
		case AppInfoReload::Status::Unavailable:
			g_pLog->warn(
				"AppInfoProvision: live appinfo disk reload unavailable for %zu "
				"synthetic app(s); restart remains available\n",
				selected.size());
			break;
		case AppInfoReload::Status::ReadFailed:
			g_pLog->warn(
				"AppInfoProvision: Steam rejected live appinfo disk reload for %zu "
				"synthetic app(s); restart remains available\n",
				selected.size());
			break;
		case AppInfoReload::Status::Loaded:
			if (reloaded.resolved == 0)
			{
				g_pLog->warn(
					"AppInfoProvision: live appinfo reload completed but no current "
					"synthetic app resolved; restart remains available\n");
			}
			else
			{
				g_pLog->info(
					"AppInfoProvision: published %zu synthetic app(s) into the live "
					"appinfo cache\n",
					reloaded.resolved);
			}
			break;
	}
}

void refreshWorkerStartFailed() noexcept
{
	if (g_pLog)
		g_pLog->warn(
		    "AppInfoProvision: unable to start async refresh worker; "
		    "will retry on the next refresh request\n");
}

void resetRefreshAfterStartFailure(std::uint64_t token) noexcept
{
	std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
	if (g_refreshInFlight && g_refreshWorkerToken == token)
		g_refreshInFlight = false;
}

void finishRefresh(const std::string& completedPath, std::uint64_t token);

RefreshWorkerStartResult startRefreshWorker(
    const std::string& appinfoVdfPath, std::uint64_t token,
    const std::vector<std::uint32_t>& runtimePublishApps)
{
	bool workerMayStillExist = false;
	const bool started = ThreadStart::startDetached(
		[path = appinfoVdfPath, token, runtimePublishApps]
		{
			ThreadStart::runGuarded(
				[path, runtimePublishApps]
				{
					ScopedNotifySuppression suppression;
					BootProf::Span profile(g_pLog.get(), "provision.async");
					provisionAllAddedApps(path, false);
					publishRuntimeAppInfo(path, runtimePublishApps);
				},
				[]
				{
					if (g_pLog)
						g_pLog->warn(
						    "AppInfoProvision: async refresh worker failed; will retry\n");
				},
				[path, token] { finishRefresh(path, token); });
		},
		[] { refreshWorkerStartFailed(); },
		ThreadStart::NoopDetachFailure{},
		ThreadStart::StdThreadDetacher{},
		ThreadStart::StdThreadJoiner{},
		[&workerMayStillExist] {
			workerMayStillExist = true;
			refreshWorkerStartFailed();
		});
	if (started)
		return RefreshWorkerStartResult::Started;
	if (workerMayStillExist)
		return RefreshWorkerStartResult::Uncertain;

	resetRefreshAfterStartFailure(token);
	return RefreshWorkerStartResult::NotStarted;
}

void retainRefreshAfterFailedStart(const std::string& appinfoVdfPath,
                                   std::uint64_t token,
                                   RefreshWorkerStartResult result,
                                   const std::vector<std::uint32_t>& runtimePublishApps)
{
	if (result != RefreshWorkerStartResult::NotStarted ||
	    !shouldRequeueRefreshAfterStartFailure(/*workerMayStillExist=*/false))
		return;

	// A known construction failure cannot safely recurse into another thread
	// attempt: persistent resource pressure would otherwise create an
	// unbounded retry loop. Keep the request dormant until the next PICS or
	// config-watcher refresh request reopens the gate.
	std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
	if (g_refreshInFlight || g_refreshWorkerToken != token)
		return;
	g_refreshPending = true;
	if (g_refreshPendingPath.empty())
		g_refreshPendingPath = appinfoVdfPath;
	g_refreshPendingRuntimeApps = mergeRuntimePublishCandidates(
		std::move(g_refreshPendingRuntimeApps), runtimePublishApps);
}

void finishRefresh(const std::string& completedPath, std::uint64_t token)
{
	std::string nextPath;
	std::vector<std::uint32_t> nextRuntimePublishApps;
	std::uint64_t nextToken = 0;
	{
		std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
		if (!g_refreshInFlight || g_refreshWorkerToken != token)
			return;

		const bool asyncEnabled = asyncProvisioningEnabled();
		const bool hasManagedApps = !g_config.managedAppIds.get().empty();
		if (!shouldRerunPendingRefresh(
		        asyncEnabled, hasManagedApps, g_refreshPending))
		{
			g_refreshPending = false;
			g_refreshPendingPath.clear();
			g_refreshPendingRuntimeApps.clear();
			g_refreshInFlight = false;
			return;
		}

		nextPath = g_refreshPendingPath.empty()
		               ? completedPath
		               : g_refreshPendingPath;
		nextRuntimePublishApps = std::move(g_refreshPendingRuntimeApps);
		g_refreshPending = false;
		g_refreshPendingPath.clear();
		g_refreshPendingRuntimeApps.clear();
		// Keep the gate closed while handing the queued request to the next
		// worker. A request arriving in this window queues behind that worker.
		nextToken = ++g_refreshWorkerToken;
	}

	const auto startResult = startRefreshWorker(
		nextPath, nextToken, nextRuntimePublishApps);
	retainRefreshAfterFailedStart(
		nextPath, nextToken, startResult, nextRuntimePublishApps);
}

void refreshInBackground(const std::string& appinfoVdfPath)
{
	refreshInBackground(appinfoVdfPath, {});
}

void refreshInBackground(
	const std::string& appinfoVdfPath,
	const std::vector<std::uint32_t>& runtimePublishApps)
{
	std::string workerPath = appinfoVdfPath;
	std::vector<std::uint32_t> workerRuntimePublishApps;
	std::uint64_t token = 0;
	{
		std::lock_guard<std::mutex> lock(g_refreshScheduleMu);
		const bool asyncEnabled = asyncProvisioningEnabled();
		const bool hasManagedApps = !g_config.managedAppIds.get().empty();
		const auto action = refreshScheduleAction(
		    asyncEnabled, hasManagedApps, g_refreshInFlight);
		if (action == RefreshScheduleAction::Ignore)
			return;
		if (action == RefreshScheduleAction::Queue)
		{
			g_refreshPending = true;
			g_refreshPendingPath = appinfoVdfPath;
			g_refreshPendingRuntimeApps = mergeRuntimePublishCandidates(
				std::move(g_refreshPendingRuntimeApps), runtimePublishApps);
			return;
		}

		if (!g_refreshPendingPath.empty())
			workerPath = g_refreshPendingPath;
		workerRuntimePublishApps = mergeRuntimePublishCandidates(
			std::move(g_refreshPendingRuntimeApps), runtimePublishApps);
		g_refreshInFlight = true;
		g_refreshPending = false;
		g_refreshPendingPath.clear();
		g_refreshPendingRuntimeApps.clear();
		token = ++g_refreshWorkerToken;
	}

	const auto startResult = startRefreshWorker(
		workerPath, token, workerRuntimePublishApps);
	retainRefreshAfterFailedStart(
		workerPath, token, startResult, workerRuntimePublishApps);
}

DlcInjectionIds collectDlcAppIdsForAddedApps(bool* complete)
{
	if (complete) *complete = false;
	DlcAppIds sources;
	std::unordered_set<uint32_t> taggedSeen;
	std::unordered_set<uint32_t> advertisedSeen;
	std::unordered_set<uint32_t> advertisedWithContent;

	const auto added = g_config.managedAppIds.get();
	if (added.empty())
	{
		if (complete) *complete = true;
		return {};
	}

	ProcessLock::FileLock cacheLock(cacheLockPath(), false);
	if (!cacheLock.acquired())
	{
		if (g_pLog)
			g_pLog->debug(
			    "AppInfoProvision: DLC snapshot cache lock busy\n");
		return {};
	}

	// Build this once: each advertised DLC is only a membership lookup after
	// the two manifest directories have been scanned.
	const auto manifestDepotIds = collectValidManifestDepotIds();

	for (uint32_t appId : added)
	{
		// Validate the complete cache pair, not just the current .bin size.
		// A file can be truncated before this snapshot opens while remaining
		// syntactically parseable; the metadata size and SHA-1 are authoritative.
		std::string validationDiag;
		if (!hasValidatedCachedBuffer(appId, validationDiag))
		{
			if (g_pLog)
				g_pLog->debug(
				    "AppInfoProvision: DLC snapshot rejected cache pair for app=%u: %s\n",
				    appId, validationDiag.c_str());
			return {};
		}

		const auto path = getBufferPath(appId);
		std::ifstream ifs(path, std::ios::binary | std::ios::ate);
		if (!ifs.is_open())
		{
			if (g_pLog)
				g_pLog->debug(
				    "AppInfoProvision: DLC snapshot missing buffer for app=%u\n",
				    appId);
			return {};
		}

		const std::streamsize sz = ifs.tellg();
		if (sz <= 0 || sz > (64LL << 20))
		{
			if (g_pLog)
				g_pLog->debug(
				    "AppInfoProvision: DLC snapshot rejected buffer size for app=%u\n",
				    appId);
			return {};
		}
		std::string wire;
		wire.resize(static_cast<std::size_t>(sz));
		ifs.seekg(0);
		if (!ifs)
		{
			if (g_pLog)
				g_pLog->debug(
				    "AppInfoProvision: DLC snapshot could not seek buffer for app=%u\n",
				    appId);
			return {};
		}
		ifs.read(wire.data(), sz);
		if (ifs.gcount() != sz || !ifs)
		{
			if (g_pLog)
				g_pLog->debug(
				    "AppInfoProvision: DLC snapshot read failed for app=%u\n",
				    appId);
			return {};
		}

		const auto grouped = extractDlcAppIdsBySource(wire, appId);
		const bool baseHasDlcDepots = hasDepotsInDlc(wire);
		for (uint32_t dlcId : grouped.depotTagged)
		{
			// Never shadow a base AddedApp, and dedup across apps.
			if (!added.count(dlcId))
			{
				appendUnique(sources.depotTagged, taggedSeen, dlcId);
			}
		}
		for (uint32_t dlcId : grouped.advertised)
		{
			if (added.count(dlcId)) continue;
			appendUnique(sources.advertised, advertisedSeen, dlcId);

			// `hasdepotsindlc` is a base-app marker for DLCs with their own
			// appinfo/depots.  A matching depot artifact is the offline
			// fallback for buffers that do not carry the marker.
			if (baseHasDlcDepots || manifestDepotIds.count(dlcId) != 0)
			{
				advertisedWithContent.insert(dlcId);
			}
		}
	}

	const auto selected = selectDlcInjectionIds(
		sources, advertisedWithContent, g_config.injectAllAdvertisedDlc.get());
	if (!selected.appDlc.empty())
	{
		// List the ids, not just counts: the local set may include
		// storefront-only entries that were intentionally kept out of
		// package 0.  Bounded so a large library cannot flood the file.
		constexpr std::size_t kMaxLogged = 24;
		std::string ids;
		for (std::size_t i = 0; i < selected.appDlc.size() && i < kMaxLogged; ++i)
		{
			if (i) ids += ' ';
			ids += std::to_string(selected.appDlc[i]);
		}
		if (selected.appDlc.size() > kMaxLogged)
		{
			ids += " ... (+" +
				std::to_string(selected.appDlc.size() - kMaxLogged) + ")";
		}
		g_pLog->info(
			"AppInfoProvision: collected %zu DLC appid(s) from %zu AdditionalApps: "
			"package0=%zu local=%zu: %s\n",
			selected.appDlc.size(), added.size(), selected.package0.size(),
			selected.appDlc.size(), ids.c_str());
	}
	if (complete) *complete = true;
	return selected;
}

bool forgetAppImpl(uint32_t appId, bool preserveTicketArtifacts)
{
	if (appId == 0) return false;
	// Serialize only the state transition with synchronous and async
	// provisioning passes. Cache and manifest cleanup runs after the lock is
	// released and is protected by its own file/catalog locks.
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		++g_cachePublicationGenerations[appId];
		// Bumping the generation already invalidates any terminal verdict for
		// this id; drop the entry so the map cannot accumulate dead records.
		// Erased inline because the publication mutex is already held here.
		g_terminalProvisionResults.erase(appId);
		{
			std::lock_guard<std::mutex> invalidationLock(
			    g_cacheReadInvalidationMu);
			g_cacheReadInvalidated.insert(appId);
		}
		g_needProton.erase(appId);
		g_pendingProtonRemovals.insert(appId);
	}

	const auto cacheDir = getCacheDir();
	bool pendingMappingRemoved = false;
	bool complete = false;
	{
		std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
		ProcessLock::FileLock cacheLock(cacheLockPath(), false);
		if (!cacheLock.acquired()) return false;
		// Decide marker retention only after both publication and cache locks are
		// held. A concurrent synthetic publication must be visible here before
		// the cleanup chooses whether its protection marker is preserved.
		const bool preserveSyntheticMarker =
			cache::shouldPreserveSyntheticMarker(
				preserveTicketArtifacts,
				preserveTicketArtifacts && isSynthesizedAppLocked(appId));
		pendingMappingRemoved = removePendingProtonMappingLocked(appId);

		const auto relatedDepots = ManifestId::getExclusiveDepotsForApp(appId);
		const std::string suffix =
			".forgotten." +
			std::to_string(static_cast<long long>(std::time(nullptr))) + "." +
			std::to_string(static_cast<long long>(::getpid()));
		const auto records = SynthMark::quarantineAppArtifacts(
			cacheDir, appId, relatedDepots, suffix,
			/*includeTicketArtifacts=*/!preserveTicketArtifacts,
			/*includeSyntheticMarker=*/!preserveSyntheticMarker);
		for (const auto& record : records)
		{
			g_pLog->infoOnce("AppInfoProvision: quarantined removed-app cache %s -> %s\n",
			                record.original.string().c_str(),
			                record.quarantined.string().c_str());
		}
		complete = !SynthMark::hasAppArtifacts(
			cacheDir, appId, relatedDepots,
			/*includeTicketArtifacts=*/!preserveTicketArtifacts,
			/*includeSyntheticMarker=*/!preserveSyntheticMarker);
		if (complete)
		{
			// Drop this app's relation only after every source artifact has been
			// moved. A partial quarantine remains retryable with the same depot
			// ownership information, while shared depots stay in the catalog.
			ManifestId::forgetApp(appId);
		}
		if (!complete)
		{
			g_pLog->warn("AppInfoProvision: app=%u cleanup left one or more cache "
			             "artifacts in place\n", appId);
		}
		g_pLog->infoOnce("AppInfoProvision: forgot app=%u (%zu cache artifact(s), %s)\n",
		             appId, records.size(), complete ? "complete" : "partial");
	}

	if (pendingMappingRemoved)
	{
		std::lock_guard<std::mutex> passLock(g_provisionPassMu);
		g_pendingProtonRemovals.erase(appId);
	}
	return complete;
}

bool forgetApp(uint32_t appId)
{
	return forgetAppImpl(appId, false);
}

bool forgetManagedSourceApp(uint32_t appId)
{
	return forgetAppImpl(appId, true);
}

void clearCacheReadInvalidation(uint32_t appId)
{
	if (appId == 0) return;
	std::lock_guard<std::mutex> invalidationLock(g_cacheReadInvalidationMu);
	g_cacheReadInvalidated.erase(appId);
}

bool cacheMarkerAllowsRead(uint32_t appId)
{
	if (appId == 0) return false;
	std::string metadataText;
	cache::CacheMetadataView metadata;
	return readCacheMetadataFile(appId, metadataText, metadata) &&
		cacheMetadataMarkerAllowsRead(appId, metadata);
}

bool isSynthesizedApp(uint32_t appId)
{
	std::lock_guard<std::mutex> publicationLock(g_cachePublicationMu);
	return isSynthesizedAppLocked(appId);
}

} // namespace AppInfoProvision
