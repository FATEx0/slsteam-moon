// SPDX-License-Identifier: AGPL-3.0-only
//
// Cold-start provisioner for AdditionalApps.
//
// Background
// ----------
// For an AdditionalApp that isn't in the user's library (no ticket / no
// AppToken), Valve's CM responds to PICS product-info with a stripped
// buffer that has no `depots` / `manifests` / `branches` blocks.  Steam's
// downloader then sees "0 depots" and the install dialog reports 0 B and
// finishes without downloading anything (visible in `content_log.txt`:
// `0 mounted depots`, `has no changes, 0 active: 0 target`).
//
// We previously tried to enrich the in-flight PICS buffer.  Steam
// validates the buffer against the SHA-1 it received in the PICS
// changelist, so any rewrite tripped the integrity check and looped the
// cold-cache login forever.
//
// This module addresses that case at a different layer: it pulls a
// complete product-info text buffer from a public mirror and splices
// a synthetic entry into `appcache/appinfo.vdf` *before Steam opens the
// file*.  Steam then reads its own cache, finds the depots/manifests,
// and the downloader proceeds normally.  The runtime PICS path is left
// untouched, so the cold-loop fix is preserved.
//
// Provider: `https://api.steamcmd.net/v1/info/{appid}` (Pavel/SteamDB,
// JSON; identical schema to the SteamCMD `app_info_print`, including
// `_change_number` and `_sha`).  Selectable via
// `AppInfoProvision::setProvider` if a future config knob is wired up.
//
// Output writes through the same on-disk cache layout that
// `feats/pics.cpp` uses (`<config>/cache/picsbuffer_<appid>.bin` plus
// `picsbuffer_<appid>.yaml`).  During setup, missing pairs are provisioned
// before `AppInfoVdf::injectAllCached` splices them into the appinfo file;
// background refreshes for already-known apps are consumed on the next
// Steam start.
//
// Idempotent and best-effort: any network/parse failure is logged and
// skipped — Steam continues with whatever it has.

#pragma once

#include "dlcids.hpp"
#include "../config_path.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace AppInfoProvision
{

// Serialize state snapshots, Proton-state transitions, and commit sections.
// CM/provider I/O, retry sleeps, and cache-pair writes must not run while this
// mutex is held; cache writers still take cacheLockPath() for cross-process
// file exclusion.
std::mutex& provisioningPassMutex();

// Compute the 20-byte SHA-1 digest through the same dynamically resolved
// libcrypto helper used by the provisioning paths. The plugin deliberately
// does not link libcrypto directly because Steam supplies the runtime.
void sha1Bytes(const void* data, std::size_t size, std::uint8_t out[20]);

// Cache publication tokens invalidate provider/PICS work that started before
// an app was removed. The publication mutex is also held across quarantine so
// a late writer cannot recreate a pair after cleanup; callers of
// cachePublicationGenerationLocked() must already hold that mutex.
struct CachePublicationToken
{
	bool managed = false;
	std::uint64_t generation = 0;
};

std::mutex& cachePublicationMutex();
std::uint64_t cachePublicationGenerationLocked(uint32_t appId);
CachePublicationToken snapshotCachePublication(uint32_t appId);

// Collect the DLC appids advertised by every managed app from the on-disk
// `picsbuffer_<appid>.bin` buffers.  `package0` contains the planner-facing
// subset: every depot-tagged id plus advertised ids with known own content
// (or all advertised ids when InjectAllAdvertisedDlc is enabled).  `appDlc`
// retains the complete deduplicated set for local launch-time decisions.
// `complete` is false when the cache lock prevents a consistent snapshot or
// any managed buffer is missing, oversized, seek-failed, truncated, or
// otherwise unreadable; callers must retain their previous injection state in
// every incomplete case.
DlcInjectionIds collectDlcAppIdsForAddedApps(bool* complete = nullptr);

// Fetch and persist a synthetic PICS buffer for `appId` if needed.
// `appinfoVdfPath` is the path to Steam's appcache/appinfo.vdf and is
// used to skip apps that already have a usable entry.  Returns true if
// a new buffer was written (or already cached).
bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath);

// Run `provisionApp` only for managed ids sourced from stplug-in or
// luaappids.yaml. Installed compatibility ids never enter provider calls.
// Returns the number of buffers newly written (0 means everything was
// already provisioned or none needed).  `allowConfigWrite` is true only for
// the preinit pass, before Steam can concurrently rewrite config.vdf.
int provisionAllAddedApps(const std::string& appinfoVdfPath,
                          bool allowConfigWrite = true);

// Apply Proton mappings deferred by runtime provisioning. This is called from
// setup() before Steam starts its live ConfigStore writers.
void flushPendingProtonMappings();

// Provision managed apps whose cache pair is not ready for the current PICS
// response. This synchronous fallback runs on the PICS recv thread when
// called at runtime, or during setup() before the appinfo splice when
// `allowConfigWrite` is true. It publishes a normalized cache pair;
// `sanitizedApps`, when supplied, receives ids successfully normalized during
// this invocation. `fallbackApps`, when supplied, receives ids for which a
// valid stale pair was accepted as `FallbackCache`; those ids are safe to
// splice during this restart but remain stale for future refresh decisions.
int provisionColdStartApps(
    const std::string& appinfoVdfPath,
    std::unordered_set<uint32_t>* sanitizedApps = nullptr,
    bool allowConfigWrite = false,
    std::unordered_set<uint32_t>* fallbackApps = nullptr);

// Resolve the config/env gate for the asynchronous refresh path.
bool asyncProvisioningEnabled();

// Start one detached refresh pass from a post-setup worker context, such as
// the PICS receive path or the config watcher. Never call it from setup() or
// the LD_AUDIT la_preinit path. Existing buffers are refreshed asynchronously;
// the next Steam start consumes the new pair.
void refreshInBackground(const std::string& appinfoVdfPath);

// Common lock held while the cache's .bin/.yaml pair is read or published.
// Keep this inline because AppInfoVdf's standalone transaction test links
// without the provisioner object itself.
inline std::string cacheLockPath()
{
	return ConfigPath::slsteamConfigDir(
	           std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME")) +
	       "/cache/.picsbuffer.lock";
}

// Forget the on-disk app-scoped state for an app that was removed from
// LuaTools. Artifacts are quarantined with a recoverable suffix rather than
// deleted, so a mistaken removal can be restored without data loss. Returns
// false for an invalid app id or when any app-scoped artifact could not be
// quarantined; a missing artifact is a successful no-op.
bool forgetApp(uint32_t appId);

// Forget app-scoped artifacts while retaining ownership ticket files and
// synthetic-PICS protection for an id that remains active through the
// compatibility set.
bool forgetManagedSourceApp(uint32_t appId);

// True iff the cache metadata and synthetic marker describe the same
// explicit publication state, or a marker-only retention state protects a
// still-active compatibility app after managed-source cleanup, and the app
// has not been invalidated for cache reads in this process. Direct cache
// readers use this before consuming a pair without going through
// cacheUseForApp().
bool cacheMarkerAllowsRead(uint32_t appId);

// Publish a complete bin/yaml pair and its synthetic marker. Callers must hold
// cachePublicationMutex() and cacheLockPath() before entering this helper;
// every post-write failure restores the previous pair and marker state.
bool publishCachePairLocked(uint32_t appId, const std::string& wire,
                            const std::string& metadata, bool synthetic,
                            bool markerBefore, std::string& error);

// Read a cache pair only after validating metadata, SHA-1, VDF structure and
// usable depot content. The helper acquires the cross-process cache lock and
// is the single safe entry point for direct buffer consumers.
bool readValidatedCacheBuffer(uint32_t appId, std::string& buffer);

// Allow a newly published pair to become readable after a prior managed-app
// removal invalidated the old cache in this process.
void clearCacheReadInvalidation(uint32_t appId);

// True iff `appId`'s appinfo depots were SYNTHESIZED from local manifests
// because its product-info is token-locked (access token denied -> empty
// PICS buffer), with either a consistent cache marker or a retained marker
// protecting an active compatibility app after managed-source cleanup.
// Persisted across the setup() re-exec storm. The outgoing PICS hook
// (apps.cpp::sendPICSInfoRequest) strips these from Steam's product-info
// request so a later empty refresh can't clobber the appinfo we spliced at
// startup (otherwise: install dialog -> 0 B / "Invalid install path").
bool isSynthesizedApp(uint32_t appId);

} // namespace AppInfoProvision
