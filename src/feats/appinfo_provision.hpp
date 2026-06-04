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
// cold-cache login forever (see HANDOFF.md §3, FINDINGS.md).
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
// `picsbuffer_<appid>.yaml`).  The existing `AppInfoVdf::injectAllCached`
// then handles the splice at the next Steam start.
//
// Idempotent and best-effort: any network/parse failure is logged and
// skipped — Steam continues with whatever it has.

#pragma once

#include <cstdint>
#include <string>

namespace AppInfoProvision
{

// Fetch and persist a synthetic PICS buffer for `appId` if needed.
// `appinfoVdfPath` is the path to Steam's appcache/appinfo.vdf and is
// used to skip apps that already have a usable entry.  Returns true if
// a new buffer was written (or already cached).
bool provisionApp(uint32_t appId, const std::string& appinfoVdfPath);

// Run `provisionApp` for every AdditionalApps id in the loaded config.
// Returns the number of buffers newly written (0 means everything was
// already provisioned or none needed).
int provisionAllAddedApps(const std::string& appinfoVdfPath);

} // namespace AppInfoProvision
