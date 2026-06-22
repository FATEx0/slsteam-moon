// SPDX-License-Identifier: AGPL-3.0-only
//
// Manifest GID pinning feature.
//
// Background: a Lua script in `<Steam>/config/stplug-in/` may carry
//
//     setManifestid(<depotId>, "<gid>")
//
// alongside `addappid(...)`.  Without `setManifestid`, the depot is
// installed at whatever `manifests.public.gid` PICS happens to ship
// at the moment of install — which is fine for "I want the latest"
// but breaks reproducibility, version-pinned mods, and any flow that
// expects a specific build.
//
// This feature ingests `setManifestid` calls into a depot-id keyed
// catalog and rewrites the manifests block of the PICS wire-format
// app buffer so Steam's installer picks our pinned GID instead of
// upstream's `public`.
//
// Catalog layout (mirrors DepotKey):
//   <SLSsteam config dir>/cache/manifestid_<depotId>.yaml
//     ---
//     depotId: <uint32>
//     gid: "<numeric-string>"
//
// Application points:
//   - Importer runs at startup alongside DepotKey::importLuaScripts.
//   - Buffer rewrite runs inside PICS::recvProductInfoResponse, just
//     before persistAppBuffer, only for AdditionalApps.

#pragma once

#include <cstdint>
#include <string>

namespace ManifestId
{
	// Disk paths.
	std::string getCatalogDir();
	std::string getCatalogPath(uint32_t depotId);

	// Returns the pinned GID for `depotId`, or empty string if none.
	std::string getPinnedGid(uint32_t depotId);

	// Persist a (depotId, gid) pair to the catalog.  Idempotent.
	bool savePin(uint32_t depotId, const std::string& gid);

	// Importer: scans `<Steam>/config/stplug-in/*.lua`, extracts every
	// `setManifestid(<depotId>, "<gid>")` and ingests into the catalog.
	void importLuaScripts();
}
