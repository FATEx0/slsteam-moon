// SPDX-License-Identifier: AGPL-3.0-only
//
// Bounded, deterministic inputs for live package-state reconciliation.
#pragma once

#include "hotreload_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace HotReloadInputs
{

inline constexpr std::size_t kMaxSnapshotIds = 4096;

struct AppInput
{
	std::uint32_t baseAppId = 0;
	bool cacheValid = false;
	std::vector<std::uint32_t> plannerAppIds;
	std::vector<std::uint32_t> depotIds;
};

struct BuildResult
{
	PackageSnapshot snapshot;
	bool valid = false;
};

inline BuildResult build(std::uint64_t generation,
	const std::vector<AppInput>& inputs,
	std::size_t maxIds = kMaxSnapshotIds)
{
	BuildResult out;
	out.snapshot.generation = generation;
	out.snapshot.metadataComplete = true;

	std::unordered_set<std::uint32_t> appIds;
	std::unordered_set<std::uint32_t> depotIds;
	appIds.reserve(std::min(inputs.size(), maxIds));
	depotIds.reserve(std::min(inputs.size(), maxIds));

	for (const AppInput& input : inputs)
	{
		if (input.baseAppId != 0)
			appIds.insert(input.baseAppId);
		else
			out.snapshot.metadataComplete = false;

		if (input.cacheValid)
		{
			for (const std::uint32_t appId : input.plannerAppIds)
				if (appId != 0) appIds.insert(appId);
		}
		else
		{
			out.snapshot.metadataComplete = false;
		}

		for (const std::uint32_t depotId : input.depotIds)
			if (depotId != 0) depotIds.insert(depotId);

		if (appIds.size() > maxIds || depotIds.size() > maxIds)
		{
			out.snapshot.appIds.clear();
			out.snapshot.depotIds.clear();
			out.snapshot.metadataComplete = false;
			return out;
		}
	}

	out.snapshot.appIds.assign(appIds.begin(), appIds.end());
	out.snapshot.depotIds.assign(depotIds.begin(), depotIds.end());
	std::sort(out.snapshot.appIds.begin(), out.snapshot.appIds.end());
	std::sort(out.snapshot.depotIds.begin(), out.snapshot.depotIds.end());
	out.valid = true;
	return out;
}

BuildResult buildFromCaches(std::uint64_t generation,
	const std::unordered_set<std::uint32_t>& managedAppIds);

} // namespace HotReloadInputs
