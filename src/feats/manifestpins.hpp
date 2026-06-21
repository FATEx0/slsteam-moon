// SPDX-License-Identifier: AGPL-3.0-only
//
// ManifestPins — pure container logic for the manifest-pinning feature
// A "pin" locks a depot to a specific manifest gid the
// user has archived; CConfig parses config.yaml's `ManifestPins:` map into
// the structured PinMap and derives a flattened depot->gid index (the
// redirect lookup) plus a locked-app set (the update-lock).  Kept free of
// yaml/globals/I/O so it is host-unit-testable (tools/test_manifestpins.cpp).

#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace ManifestPins
{
	struct AppPins
	{
		bool locked = false;
		uint32_t buildId = 0; // pinned build NUMBER (0 = none); GetAppBuildId
		std::unordered_map<uint32_t, uint64_t> depots; // depot -> gid
	};

	// appid -> its pins
	using PinMap = std::unordered_map<uint32_t, AppPins>;

	// Union every app's depot->gid into one index.  A depot belongs to a
	// single app, so collisions are not expected; if one occurs the last
	// app iterated wins (deterministic enough for a should-never-happen case).
	inline std::unordered_map<uint32_t, uint64_t> flattenDepots(const PinMap& pins)
	{
		std::unordered_map<uint32_t, uint64_t> flat;
		for (const auto& [appId, app] : pins)
		{
			for (const auto& [depotId, gid] : app.depots)
			{
				flat[depotId] = gid;
			}
		}
		return flat;
	}

	inline std::unordered_set<uint32_t> lockedAppSet(const PinMap& pins)
	{
		std::unordered_set<uint32_t> locked;
		for (const auto& [appId, app] : pins)
		{
			if (app.locked) locked.insert(appId);
		}
		return locked;
	}

	inline uint64_t getPin(const std::unordered_map<uint32_t, uint64_t>& flat,
	                       uint32_t depotId)
	{
		const auto it = flat.find(depotId);
		return it == flat.end() ? 0ULL : it->second;
	}

	inline bool isLocked(const std::unordered_set<uint32_t>& locked,
	                     uint32_t appId)
	{
		return locked.count(appId) != 0;
	}

	inline void purgeApps(PinMap& pins,
	                      const std::unordered_set<uint32_t>& dropApps)
	{
		for (uint32_t appId : dropApps) pins.erase(appId);
	}

	inline void purgeOrphans(PinMap& pins,
	                         const std::unordered_set<uint32_t>& keepApps)
	{
		for (auto it = pins.begin(); it != pins.end(); )
		{
			if (keepApps.count(it->first) == 0) it = pins.erase(it);
			else ++it;
		}
	}
}
