
#include "pics.hpp"

#include "depotkey.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"

#include "../utils/ManifestFetch.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace PICS
{

namespace
{

std::string getCacheDir()
{
	std::stringstream ss;
	ss << g_config.getDir() << "/cache";
	const auto dir = ss.str();
	if (!std::filesystem::exists(dir))
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
	}
	return dir;
}

std::string getBufferPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".bin";
	return ss.str();
}

std::string getMetaPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".yaml";
	return ss.str();
}

bool persistAppBuffer(uint32_t appId, uint32_t changeNumber,
                      const std::string& sha, const std::string& buffer)
{
	if (buffer.empty()) return false;
	if (sha.size() != 20)
	{
		g_pLog->debug("PICS: refusing to persist app=%u (bad sha size %zu)\n",
		              appId, sha.size());
		return false;
	}

	const auto bufPath = getBufferPath(appId);
	const auto metaPath = getMetaPath(appId);

	if (std::filesystem::exists(metaPath) && std::filesystem::exists(bufPath))
	{
		try
		{
			auto node = YAML::LoadFile(metaPath);
			const auto cachedChange = node["change_number"].as<uint32_t>();
			const auto cachedSize = node["wire_size"].as<size_t>();
			if (cachedChange == changeNumber && cachedSize == buffer.size())
			{
				return true;
			}
		}
		catch (...) { /* fall through to rewrite */ }
	}

	{
		std::ofstream ofs(bufPath, std::ios::binary | std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("PICS: cannot write %s\n", bufPath.c_str());
			return false;
		}
		ofs.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
	}

	{
		YAML::Emitter em;
		em << YAML::BeginMap;
		em << YAML::Key << "appid"          << YAML::Value << appId;
		em << YAML::Key << "change_number"  << YAML::Value << changeNumber;
		em << YAML::Key << "wire_size"      << YAML::Value << buffer.size();
		em << YAML::Key << "sha_b64"        << YAML::Value << base64::to_base64(sha);
		em << YAML::EndMap;

		std::ofstream ofs(metaPath, std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("PICS: cannot write %s\n", metaPath.c_str());
			return false;
		}
		ofs.write(em.c_str(), em.size());
	}

	g_pLog->debug("PICS: cached app=%u change=%u buffer=%zu bytes -> %s\n",
	              appId, changeNumber, buffer.size(), bufPath.c_str());
	return true;
}

std::vector<std::pair<uint32_t, uint64_t>> extractDepotsAndGids(const std::string& buf)
{
	std::vector<std::pair<uint32_t, uint64_t>> results;
	size_t depotsPos = buf.find("\"depots\"");
	if (depotsPos == std::string::npos) return results;

	size_t openBrace = buf.find('{', depotsPos + 8);
	if (openBrace == std::string::npos) return results;

	int depth = 1;
	size_t scan = openBrace + 1;
	uint32_t currentDepotId = 0;
	std::string currentSection;
	std::string currentBranch;

	while (scan < buf.size() && depth > 0)
	{
		char c = buf[scan];
		if (c == '"')
		{
			size_t end = buf.find('"', scan + 1);
			if (end == std::string::npos) break;
			std::string token = buf.substr(scan + 1, end - scan - 1);
			scan = end + 1;

			if (depth == 1)
			{
				bool isDigits = !token.empty();
				for (char ch : token)
				{
					if (ch < '0' || ch > '9') { isDigits = false; break; }
				}
				if (isDigits)
				{
					try { currentDepotId = std::stoul(token); }
					catch (...) { currentDepotId = 0; }
				}
			}
			else if (depth == 2)
			{
				currentSection = token;
			}
			else if (depth == 3 && currentSection == "manifests")
			{
				currentBranch = token;
			}
			else if (depth == 4 && currentSection == "manifests" && currentBranch == "public" && token == "gid" && currentDepotId != 0)
			{
				size_t valStart = buf.find('"', scan);
				if (valStart != std::string::npos)
				{
					size_t valEnd = buf.find('"', valStart + 1);
					if (valEnd != std::string::npos)
					{
						std::string valToken = buf.substr(valStart + 1, valEnd - valStart - 1);
						bool isDigits = !valToken.empty();
						for (char ch : valToken)
						{
							if (ch < '0' || ch > '9') { isDigits = false; break; }
						}
						if (isDigits)
						{
							try
							{
								uint64_t gid = std::stoull(valToken);
								results.push_back({currentDepotId, gid});
							}
							catch (...) {}
						}
						scan = valEnd + 1;
					}
				}
			}
			continue;
		}
		if (c == '{') ++depth;
		else if (c == '}')
		{
			--depth;
			if (depth == 1)
			{
				currentDepotId = 0;
			}
		}
		++scan;
	}
	return results;
}

// Read a previously-persisted product-info buffer from our cache.
// Used to recover the depot/gid list for AdditionalApps whose live CM
// product-info response carries an empty buffer (see the staging
// fallback in recvProductInfoResponse).  AppInfoProvision and
// persistAppBuffer both write to this same `picsbuffer_<appid>.bin`
// path, so whichever ran last is what we read.
std::string readCachedBuffer(uint32_t appId)
{
	const auto path = getBufferPath(appId);
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return {};
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (8LL << 20)) return {};
	std::string out;
	out.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	if (!ifs.read(out.data(), sz)) return {};
	return out;
}

void cleanShaderHitCache(uint32_t appId)
{
	const char* home = std::getenv("HOME");
	if (!home) return;

	static const char* steamRoots[] = {
		"/.steam/steam",
		"/.steam/debian-installation",
		"/.local/share/Steam",
	};

	const std::string needle = std::to_string(appId) + "_pbuf";

	for (const char* suffix : steamRoots)
	{
		const auto userdataDir = std::string(home) + suffix + "/userdata";
		if (!std::filesystem::exists(userdataDir)) continue;

		std::error_code ec;
		for (auto& entry : std::filesystem::recursive_directory_iterator(userdataDir, ec))
		{
			if (!entry.is_regular_file()) continue;
			const auto fn = entry.path().filename().string();
			if (fn == needle)
			{
				g_pLog->info("PICS: removing shader hit cache: %s\n",
				             entry.path().c_str());
				std::filesystem::remove(entry.path(), ec);
			}
		}
	}
}

} // namespace

void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp)
{
	if (!resp) return;

	g_pLog->debug
	(
		"PICS: response apps=%d packages=%d unknown_apps=%d unknown_packages=%d meta_only=%i\n",
		resp->apps_size(),
		resp->packages_size(),
		resp->unknown_appids_size(),
		resp->unknown_packageids_size(),
		resp->meta_data_only() ? 1 : 0
	);


	const auto added = g_config.addedAppIds.get();
	for (int i = 0; i < resp->apps_size(); ++i)
	{
		auto* app = resp->mutable_apps(i);
		g_pLog->debug
		(
			"PICS: app=%u change=%u missing_token=%i only_public=%i sha_size=%zu buffer_size=%zu\n",
			app->appid(),
			app->change_number(),
			app->missing_token() ? 1 : 0,
			app->only_public() ? 1 : 0,
			app->sha().size(),
			app->buffer().size()
		);

		if (!added.count(app->appid()))
		{
			continue;
		}

		if (app->buffer().size() > 0)
		{
			// IMPORTANT: do NOT rewrite app->buffer() here.
			//
			// We used to pin manifest GIDs by editing the product-info
			// text buffer (ManifestId::applyToWireBuffer) and then
			// re-stamping app->sha().  Steam, however, validates the
			// product-info buffer against the SHA-1 it received in the
			// PICS *changelist* (the authoritative hash from the prior
			// request stage), not against the sha field in this
			// response.  Any edit to the buffer therefore fails Steam's
			// integrity check:
			//
			//     appinfo_log: "Corrupt data in text buffer for app N"
			//     "UpdatesJob: apps still needs updates, run again"
			//
			// which makes Steam re-request product info forever and
			// hangs the client at "Loading user data" on a cold cache
			// (reproduced; the loop only ever hit the AddedApps whose
			// depots had manifest pins, i.e. the ones we rewrote).
			//
			// Manifest-GID pinning is handled at the download layer
			// instead — ManifestCode's GetManifestRequestCode /
			// BYldRequestDepotManifest hooks redirect the actual
			// manifest request to the pinned gid — so dropping the
			// product-info rewrite loses nothing.  We persist the
			// pristine, server-validated buffer (verbatim sha) so the
			// appinfo.vdf warm-cache splice stays consistent too.
			persistAppBuffer(app->appid(), app->change_number(),
			                 app->sha(), app->buffer());

			cleanShaderHitCache(app->appid());
		}

		// Decide which buffer to mine for depots/gids.  For an
		// AdditionalApp whose product info isn't in the local library,
		// the live CM response carries an EMPTY buffer (no depots), so we
		// fall back to the product-info buffer we provisioned to disk
		// during setup() (picsbuffer_<appid>.bin).
		const bool emptyLiveBuffer = app->buffer().size() == 0;
		const std::string warmBuf =
		    emptyLiveBuffer ? readCachedBuffer(app->appid()) : app->buffer();
		if (warmBuf.empty())
		{
			g_pLog->debug("PICS: app=%u no buffer to stage (live=%zu, no cache)\n",
			              app->appid(), app->buffer().size());
			continue;
		}

		auto depots = extractDepotsAndGids(warmBuf);
		for (const auto& [depotId, gid] : depots)
		{
			// Only stage depots we can actually decrypt; the provisioned
			// buffer still lists depots whose keys we don't have (other
			// OSes, DLC), and staging those just wastes a CDN round-trip.
			if (DepotKey::getCachedKey(depotId).key.empty())
			{
				continue;
			}

			if (!emptyLiveBuffer)
			{
				// Library app: Steam drives its own manifest fetch.  A
				// best-effort async prefetch is harmless but never on
				// the critical path, so don't block the recv thread.
				g_pLog->info("PICS: prefetching manifest for app=%u depot=%u gid=%llu\n",
				             app->appid(), depotId, static_cast<unsigned long long>(gid));
				ManifestFetch::submitManifestBlob(gid, app->appid(), depotId);
				continue;
			}

			// AdditionalApp: stage the manifest SYNCHRONOUSLY, before
			// this handler returns.
			//
			// Why synchronous here is the actual fix (verified on the VM
			// 2026-06-04, superseding the HANDOFF "timing race" theory):
			//
			// Clicking Install triggers a fresh PICS product-info request
			// for the app; Steam cannot begin update *planning* until that
			// response is processed (it's what tells Steam which depots /
			// manifests exist).  So this recv handler strictly precedes
			// planning.
			//
			// During planning Steam decides whether to call
			// CDepotDownloadMgr::BYldRequestDepotManifest.  Content-log
			// evidence (both a native-Linux depot 285903 AND a windows
			// depot 638511) shows:
			//   - manifest NOT on disk at planning  -> Steam calls BYld ->
			//     the ORIGINAL BYld returns 'Access Denied' -> the whole
			//     attempt is canceled with "No connection".  Our injected
			//     request-code lands in Steam's cache but does NOT rescue
			//     that in-flight call, so the first attempt always failed.
			//   - manifest ALREADY on disk at planning -> Steam SKIPS BYld
			//     entirely and goes straight to Downloading -> success.
			//     (This is exactly why the ~30s auto-retry always worked:
			//     attempt 1 left the blob on disk.)
			//
			// Staging the blob here, before we return, guarantees the
			// .manifest is on disk before planning runs, so BYld is never
			// called on the first attempt and the install succeeds without
			// the retry.  This runs on a genuine Steam worker thread (the
			// InitFromPacket detour), the same context the prefetch above
			// has always used safely; blocking it briefly is acceptable
			// (the BYld sync fetch already blocked a Steam thread the same
			// way).
			g_pLog->info("PICS: staging manifest synchronously for app=%u depot=%u gid=%llu\n",
			             app->appid(), depotId, static_cast<unsigned long long>(gid));
			const bool staged = ManifestFetch::awaitManifestBlob(
			    gid, depotId, ManifestFetch::getTimeoutSec());
			g_pLog->info("PICS: manifest staging for app=%u depot=%u gid=%llu -> %s\n",
			             app->appid(), depotId, static_cast<unsigned long long>(gid),
			             staged ? "on disk" : "FAILED (will fall back to BYld retry)");
		}
	}

	if (resp->unknown_appids_size() > 0)
	{
		std::stringstream ss;
		for (int i = 0; i < resp->unknown_appids_size(); ++i)
		{
			if (i) ss << ',';
			ss << resp->unknown_appids(i);
		}
		g_pLog->debug("PICS: unknown_appids=[%s]\n", ss.str().c_str());
	}
}

void recvMsg(CProtoBufMsgBase* msg)
{
	if (!msg) return;
	switch (msg->type)
	{
		case EMSG_PICS_PRODUCTINFO_RESPONSE:
			recvProductInfoResponse(msg->getBody<CMsgClientPICSProductInfoResponse>());
			break;
		default:
			break;
	}
}

} // namespace PICS
