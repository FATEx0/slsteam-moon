
#include "pics.hpp"

#include "appinfo_provision.hpp"
#include "provision_cache.hpp"
#include "appinfo_vdf.hpp"
#include "depotkey.hpp"
#include "manifeststore.hpp"
#include "prewarm.hpp"
#include "synthmark.hpp"
#include "apps.hpp"
#include "packagepatch.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../bootprof.hpp"
#include "../ownerwork.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../update.hpp"

#include "../utils/ManifestFetch.hpp"
#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
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

// A provider-normalized pair, or a legacy pair with no provenance marker,
// remains authoritative only while its metadata still describes the complete
// binary. Legacy pairs are deliberately preserved until a provider refresh
// rewrites them with an explicit marker; otherwise a raw PICS response could
// silently replace a previously normalized cache.
// This also prevents a failed metadata publication from leaving an old
// normalized=true marker that suppresses raw PICS recovery for a newly
// replaced or truncated buffer.
bool hasNormalizedCache(uint32_t appId)
{
	try
	{
		const auto metadata = YAML::LoadFile(getMetaPath(appId));
		if (metadata["appid"].as<uint32_t>() != appId)
			return false;
		if (!AppInfoProvision::cacheMarkerAllowsRead(appId))
			return false;
		const auto normalizedField = metadata["normalized"];
		const bool hasNormalizedMarker = normalizedField.IsDefined();
		const bool normalized = hasNormalizedMarker &&
		                        normalizedField.as<bool>();
		if (!AppInfoProvision::cache::shouldPreserveCacheFromRawPics(
		        hasNormalizedMarker, normalized))
			return false;

		const auto declaredSize = metadata["wire_size"].as<std::size_t>();
		const auto declaredSha = std::string(
			base64::from_base64(metadata["sha_b64"].as<std::string>()));
		constexpr std::size_t kSha1Size = 20;
		if (declaredSha.size() != kSha1Size)
			return false;

		std::ifstream ifs(getBufferPath(appId),
		                  std::ios::binary | std::ios::ate);
		if (!ifs.is_open()) return false;
		const std::streamsize rawSize = ifs.tellg();
		if (rawSize <= 0 || rawSize > (16LL << 20) ||
		    declaredSize != static_cast<std::size_t>(rawSize))
			return false;
		std::string wire(static_cast<std::size_t>(rawSize), '\0');
		ifs.seekg(0, std::ios::beg);
		if (!ifs.read(wire.data(), rawSize)) return false;

		std::uint8_t digest[kSha1Size]{};
		AppInfoProvision::sha1Bytes(wire.data(), wire.size(), digest);
		return std::memcmp(declaredSha.data(), digest, kSha1Size) == 0;
	}
	catch (...)
	{
		return false;
	}
}

bool persistAppBuffer(uint32_t appId, uint32_t changeNumber,
                      const std::string& sha, const std::string& buffer)
{
	const auto publication =
		AppInfoProvision::snapshotCachePublication(appId);
	if (!publication.managed)
	{
		g_pLog->debug("PICS: ignoring stale response for removed app=%u\n", appId);
		return false;
	}
	(void)getCacheDir();
	std::lock_guard<std::mutex> publicationLock(
	    AppInfoProvision::cachePublicationMutex());
	ProcessLock::FileLock cacheLock(AppInfoProvision::cacheLockPath(), false);
	if (!cacheLock.acquired())
	{
		g_pLog->info("PICS: unable to lock cache pair for app=%u\n", appId);
		return false;
	}
	if (!AppInfoProvision::cache::cachePublicationAllowed(
	        publication.managed, publication.generation,
	        AppInfoProvision::cachePublicationGenerationLocked(appId)))
	{
		g_pLog->debug(
		    "PICS: rejecting stale cache publication for app=%u\n", appId);
		return false;
	}
	if (buffer.empty()) return false;
	if (sha.size() != 20)
	{
		g_pLog->debug("PICS: refusing to persist app=%u (bad sha size %zu)\n",
		              appId, sha.size());
		return false;
	}
	if (hasNormalizedCache(appId))
	{
		g_pLog->debug(
		    "PICS: retaining normalized or legacy cache for app=%u\n", appId);
		return false;
	}

	const auto bufPath = getBufferPath(appId);

	YAML::Emitter em;
	em << YAML::BeginMap;
	em << YAML::Key << "appid"          << YAML::Value << appId;
	em << YAML::Key << "change_number"  << YAML::Value << changeNumber;
	em << YAML::Key << "wire_size"      << YAML::Value << buffer.size();
	em << YAML::Key << "sha_b64"        << YAML::Value << base64::to_base64(sha);
	em << YAML::Key << "normalized"     << YAML::Value << false;
	em << YAML::Key << "synthetic"      << YAML::Value << false;
	em << YAML::EndMap;
	const std::string metadata(em.c_str(), em.size());
	const bool markerBefore = SynthMark::isMarked(getCacheDir(), appId);
	std::string writeError;
	if (!AppInfoProvision::publishCachePairLocked(
	        appId, buffer, metadata, /*synthetic=*/false, markerBefore,
	        writeError))
	{
		g_pLog->debug(
		    "PICS: unable to publish cache pair for app=%u: %s\n",
		    appId, writeError.c_str());
		return false;
	}
	AppInfoProvision::clearCacheReadInvalidation(appId);

	g_pLog->debug("PICS: cached app=%u change=%u buffer=%zu bytes -> %s\n",
	              appId, changeNumber, buffer.size(), bufPath.c_str());
	return true;
}

std::vector<std::pair<uint32_t, uint64_t>> extractDepotsAndGids(const std::string& buf)
{
	// Single definition shared with the background pre-warm worker
	// (feats/prewarm.hpp) so the install path and the warm path mine the
	// provisioned buffer identically.
	return Prewarm::extractDepotsAndGids(buf);
}

// Read a previously-persisted product-info buffer from our cache.
// Used to recover the depot/gid list for AdditionalApps whose live CM
// product-info response carries an empty buffer (see the staging
// fallback in recvProductInfoResponse). AppInfoProvision and
// persistAppBuffer share this `picsbuffer_<appid>.bin` path; provider-
// normalized pairs are marked in metadata and remain authoritative over
// raw responses.
std::string readCachedBuffer(uint32_t appId)
{
	std::string out;
	if (!AppInfoProvision::readValidatedCacheBuffer(appId, out)) return {};
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

void refreshDlcInjectionAfterColdProvision()
{
	std::lock_guard<std::mutex> passLock(
	    AppInfoProvision::provisioningPassMutex());
	bool complete = false;
	const auto dlcIds = AppInfoProvision::collectDlcAppIdsForAddedApps(&complete);
	if (!complete)
	{
		g_pLog->debug(
		    "PICS: DLC cache snapshot incomplete; retaining existing "
		    "DLC/package-0 injection\n");
		return;
	}
	PackagePatch::setExtraAppIds(dlcIds.package0);
	Apps::setAddedAppDlcIds(dlcIds.appDlc);

	const auto added = g_config.addedAppIds.get();
	const auto ids = AppInfoProvision::mergePackage0AppIds(
	    added, dlcIds.package0);
	if (ids.empty()) return;

	const auto mode = OwnerWork::submitHotAdd(ids);
	g_pLog->debug(
	    "PICS: refreshed DLC/package-0 injection after cold provisioning "
	    "(package0 DLC=%zu, mode=%s)\n",
	    dlcIds.package0.size(), OwnerWork::modeName(mode));
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

	const bool legacyStaging = legacyManifestStagingEnabled(
	    std::getenv("SLSSTEAM_LEGACY_MANIFEST_STAGING"));
	g_pLog->infoOnce(
	    "PICS: manifest staging mode=%s\n",
	    legacyStaging ? "legacy synchronous + prewarm"
	                  : "event-driven Steam install plan");

	// A genuinely missing provisioned buffer is the one case that remains
	// synchronous. During the normal startup pass it is handled before
	// appinfo.vdf is spliced; this callback-side path remains for a late
	// hot-add or an interrupted/invalid publication. It runs on this real
	// Steam worker thread and persists a complete cache pair for the next
	// setup pass. Never rewrite Steam's live appinfo file from this callback:
	// its ConfigStore writers do not share our lock and the current process
	// has already loaded its in-memory map.
	const std::string appinfoVdfPath = AppInfoVdf::findExistingPath();
	std::unordered_set<uint32_t> coldSanitizedApps;
	std::unordered_set<uint32_t> coldFallbackApps;
	int coldProvisioned = 0;
	{
		BootProf::Span profile(g_pLog.get(), "provision.cold_sync");
		coldProvisioned = AppInfoProvision::provisionColdStartApps(
		    appinfoVdfPath, &coldSanitizedApps, false, &coldFallbackApps);
	}
	if (coldProvisioned > 0)
	{
		refreshDlcInjectionAfterColdProvision();
	}

	// Rollback-only collection for the old architecture. In normal operation
	// PICS still persists and supplies product info, but performs no manifest
	// directory scans, network fetches, or waits. BuildDepotDependency stages
	// only the depots Steam selected for the real install plan.
	std::vector<AppDepots> toStage;

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
			// product-info rewrite loses nothing.  A successful cold pass already
			// wrote the normalized provider buffer. Keep this callback's raw
			// response from overwriting that sanitized pair; a later refresh can
			// replace it through the same normalization path.
			if (coldSanitizedApps.count(app->appid()) == 0)
			{
				persistAppBuffer(app->appid(), app->change_number(),
				                 app->sha(), app->buffer());
			}
			else
			{
				g_pLog->debug(
				    "PICS: retaining normalized cold cache for app=%u\n",
				    app->appid());
			}

			cleanShaderHitCache(app->appid());
		}

		if (!legacyStaging)
		{
			continue;
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

		// Archive every depot manifest the zip shipped (all platforms)
		// into the purge-proof ManifestStore while they're still in
		// depotcache — before Steam's post-commit purge removes the
		// non-mounted ones (e.g. the windows depot of a native-linux
		// title).  This is what lets a later offline Proton-switch restore
		// + install the windows depot.  AdditionalApps only.
		if (emptyLiveBuffer)
		{
			std::vector<uint32_t> depotIds;
			depotIds.reserve(depots.size());
			for (const auto& [depotId, gid] : depots) depotIds.push_back(depotId);
			ManifestStore::archiveDepots(depotIds);
		}

		if (!emptyLiveBuffer)
		{
			// Library app: Steam drives its own manifest fetch.  A
			// best-effort async prefetch is harmless but never on the
			// critical path, so don't block the recv thread.
			for (const auto& [depotId, gid] : depots)
			{
				// Only prefetch depots WE manage (LuaTools).  An owned
				// library app's depots are Steam's job — touching them here
				// is needless wudrm traffic for content we don't manage.
				if (!DepotKey::isManagedDepot(depotId))
				{
					continue;
				}
				g_pLog->info("PICS: prefetching manifest for app=%u depot=%u gid=%llu\n",
				             app->appid(), depotId, static_cast<unsigned long long>(gid));
				ManifestFetch::submitManifestBlob(gid, app->appid(), depotId);
			}
			continue;
		}

		// AdditionalApp: defer staging to the concurrent pass below.  The
		// key filter (we only stage depots we hold a key for — staging a
		// blob we can't decrypt just wastes a CDN round-trip) is applied
		// there, in buildSyncStagePlan.
		toStage.push_back({app->appid(), std::move(depots)});
	}

	if (legacyStaging)
	{
		// Legacy rollback: ensure every AdditionalApp depot manifest is staged
		// before this handler returns. Ready targets do no work; misses use
		// bounded concurrency.
		//
		// Why staging must finish before we return (confirmed in testing):
		// clicking Install triggers a fresh PICS product-info request, and
		// Steam cannot begin update *planning* until this response is
		// processed (it's what tells Steam which depots/manifests exist), so
		// this recv handler strictly precedes planning. During planning Steam
		// decides whether to call CDepotDownloadMgr::BYldRequestDepotManifest:
		//   - manifest NOT on disk at planning -> Steam calls BYld -> the
		//     ORIGINAL BYld returns 'Access Denied' -> the attempt is canceled
		//     with "No connection" (only the ~30s auto-retry, which left the
		//     blob on disk, ever recovered);
		//   - manifest ALREADY on disk at planning -> Steam SKIPS BYld and goes
		//     straight to Downloading -> success.
		// So the blobs must be on disk before we return. This runs on a genuine
		// Steam worker thread (the InitFromPacket detour).
		//
		// We used to create one std::async thread for EVERY target, including
		// manifests restoreToDepotcache had already made ready. Large depot
		// graphs (311210 exposes 800+) exhausted the 32-bit Steam process during
		// this callback. The corrected flow:
		//   1. performs only a cheap exact on-disk check here;
		//   2. gives already-staged targets no task and no await at all;
		//   3. sends only missing targets to ManifestFetch's fixed worker pool,
		//      where restore-from-store and network I/O happen off this thread.
		// There is no manifest-count cap. We still await genuinely-missing
		// targets because returning before they reach disk makes Steam plan BYld
		// and fail the first install attempt.
		const auto plan = buildSyncStagePlan(
		    toStage,
		    [](uint32_t depotId)
		    {
		        return !DepotKey::getCachedKey(depotId).key.empty();
		    });

		std::size_t alreadyStaged = 0;
		const auto pending = buildPendingStagePlan(
		    plan,
		    [&](const StageTarget& target)
		    {
		        if (!ManifestStore::isInDepotcache(target.depotId, target.gid))
		        {
		            return false;
		        }
		        ++alreadyStaged;
		        return true;
		    });

		if (!plan.empty())
		{
			g_pLog->info(
			    "PICS: manifest plan targets=%zu already_on_disk=%zu pending=%zu\n",
			    plan.size(), alreadyStaged, pending.size());
		}

		// Pass 1: queue only manifests not already present. The fixed executor
		// restores from ManifestStore first and reaches the CDN only on a miss.
		for (const auto& t : pending)
		{
			g_pLog->info(
			    "PICS: staging manifest for app=%u depot=%u gid=%llu "
			    "(bounded worker)\n",
			    t.appId, t.depotId, static_cast<unsigned long long>(t.gid));
			ManifestFetch::submitManifestBlob(t.gid, t.appId, t.depotId);
		}

		// Pass 2: block only for the missing subset (joins the queued work).
		for (const auto& t : pending)
		{
			const bool staged = ManifestFetch::awaitManifestBlob(
			    t.gid, t.depotId, ManifestFetch::getTimeoutSec());
			if (staged)
			{
				ManifestStore::archiveManifest(t.depotId, t.gid);
				ManifestStore::markPreferredGid(t.depotId, t.gid);
			}
			g_pLog->info(
			    "PICS: manifest staging for app=%u depot=%u gid=%llu -> %s\n",
			    t.appId, t.depotId, static_cast<unsigned long long>(t.gid),
			    staged ? "on disk"
			           : "FAILED (will fall back to BYld retry)");
		}

		// Start the background manifest pre-warm worker now that we're on a
		// real Steam worker thread (post-login PICS recv). ensureStarted() is
		// idempotent, so calling it on every recv is cheap. This is rollback
		// behavior only; event-driven staging never starts the worker.
		Prewarm::ensureStarted();
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

	// Existing buffers are refreshed asynchronously from this sanctioned PICS
	// worker-thread entry point. The worker never runs from setup()/load(); its
	// output is consumed by AppInfoVdf::injectAllCached on the next Steam start.
	AppInfoProvision::refreshInBackground(appinfoVdfPath);

	// Refresh the safe-mode-hash cache (updates.yaml) off the boot path.
	// init() served it from disk synchronously so Steam's launch never
	// blocks on GitHub; this brings it up to date from a real worker
	// thread, gated by a TTL so we don't fetch on every relaunch.
	Updater::refreshInBackgroundIfStale();
}

void recvChangesSinceResponse(CMsgClientPICSChangesSinceResponse* resp)
{
	if (!resp) return;

	int stripped = 0;
	for (int i = resp->app_changes_size() - 1; i >= 0; --i)
	{
		if (AppInfoProvision::isSynthesizedApp(resp->app_changes(i).appid()))
		{
			g_pLog->debug("PICS: stripping synthetic app %u from changelist\n",
			              resp->app_changes(i).appid());
			resp->mutable_app_changes()->DeleteSubrange(i, 1);
			++stripped;
		}
	}
	if (stripped > 0)
	{
		g_pLog->info("PICS: filtered %d synthetic app(s) from changelist (%d remaining)\n",
		             stripped, resp->app_changes_size());
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
		case EMSG_PICS_CHANGES_RESPONSE:
			recvChangesSinceResponse(msg->getBody<CMsgClientPICSChangesSinceResponse>());
			break;
		default:
			break;
	}
}

} // namespace PICS
