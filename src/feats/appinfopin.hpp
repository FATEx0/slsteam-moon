// SPDX-License-Identifier: AGPL-3.0-only
//
// AppInfoPin — pure helper that rewrites the public-branch manifest gid of a
// LOCKED app's pinned depots inside a provisioned appinfo body
// (manifest-pin-NEXT-STEPS.md §3, Approach A).
//
// Why this exists: Steam's post-commit reconcile compares the INSTALLED depot
// gid against the IN-MEMORY appinfo gid (loaded from the provisioned
// appinfo.vdf at startup).  When the user pins a game to an older build, Steam
// downloads + commits the pinned depot gid, but appinfo still carries the live
// public gid — so the reconcile flags "Update Required" forever (proven loop,
// planner-port.md §14/§16).  Emitting the pinned gid into the provisioned
// appinfo makes installed==appinfo for the locked app: the loop never starts,
// while the older build still downloads once (appinfo's pinned gid != the
// installed public build, so Steam does run the one downgrade).
//
// Kept free of globals/I/O (only yaml-cpp) so it is host-unit-testable
// (tools/test_appinfopin.cpp).  The provisioning glue (env gate, locked-app
// check, logging) lives in appinfo_provision.cpp.

#pragma once

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace AppInfoPin
{
	// Rewrite body.depots.<depot>.manifests.public.gid to the pinned gid for
	// each depot in `pins` (the app's depot->gid pin map).  The SteamCMD/CM
	// product-info shape stores gids as quoted strings, so we read/write
	// strings.  Returns the number of depot gids actually rewritten.
	//
	// Conservative: only touches a depot that (a) exists in appinfo, (b) has a
	// manifests/public/gid leaf, and (c) whose current gid differs from the
	// pin — so an already-matching depot is left byte-identical (keeps the
	// idempotency sha stable) and unknown/virtual depots are ignored.
	inline int applyDepotGidPins(
	    YAML::Node& body,
	    const std::unordered_map<uint32_t, uint64_t>& pins)
	{
		if (pins.empty() || !body.IsMap()) return 0;

		YAML::Node depots = body["depots"];
		if (!depots || !depots.IsMap()) return 0;

		int changed = 0;
		for (const auto& [depotId, gid] : pins)
		{
			YAML::Node depot = depots[std::to_string(depotId)];
			if (!depot || !depot.IsMap()) continue;

			YAML::Node manifests = depot["manifests"];
			if (!manifests || !manifests.IsMap()) continue;

			YAML::Node pub = manifests["public"];
			if (!pub || !pub.IsMap()) continue;

			const std::string gidStr = std::to_string(gid);
			std::string cur;
			if (pub["gid"])
			{
				try { cur = pub["gid"].as<std::string>(); }
				catch (...) {}
			}
			if (cur == gidStr) continue;

			pub["gid"] = gidStr;
			++changed;
		}
		return changed;
	}

	// Rewrite body.depots.branches.public.buildid to `buildId` (the build
	// NUMBER the crack/`GetAppBuildId` reads).  Only the `public` branch is
	// touched — other branches (beta/etc.) carry their own builds.  Returns
	// true iff the public branch buildid was set.  No-op (false) when
	// buildId==0 or the branches/public node is absent.
	//
	// Why separate from the gid pin: the manifest gid (content) and the
	// branch buildid (number) are independent levers.  Evidence on the live
	// target (Pragmata 3357650) showed the pinned CONTENT already satisfies
	// the depot, but the crack still rejected the build because appinfo's
	// branch buildid stayed at the live public number — and `GetAppBuildId`
	// reads appinfo, not the appmanifest (so editing the .acf buildid, as a
	// prior attempt did, had no effect).
	inline bool applyBranchBuildId(YAML::Node& body, uint32_t buildId)
	{
		if (buildId == 0 || !body.IsMap()) return false;

		YAML::Node depots = body["depots"];
		if (!depots || !depots.IsMap()) return false;
		YAML::Node branches = depots["branches"];
		if (!branches || !branches.IsMap()) return false;
		YAML::Node pub = branches["public"];
		if (!pub || !pub.IsMap()) return false;

		pub["buildid"] = std::to_string(buildId);
		return true;
	}
}
