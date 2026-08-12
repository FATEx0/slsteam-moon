// SPDX-License-Identifier: AGPL-3.0-only

#include "hotreload_inputs.hpp"

#include "appinfo_provision.hpp"
#include "dlcids.hpp"

#include "../config.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace DepotKey
{
std::vector<std::uint32_t> managedDepotsForApp(std::uint32_t appId);
}

namespace HotReloadInputs
{
BuildResult buildFromCaches(
	std::uint64_t generation,
	const std::unordered_set<std::uint32_t>& managedAppIds)
{
	std::vector<std::uint32_t> sortedBases(
		managedAppIds.begin(), managedAppIds.end());
	std::sort(sortedBases.begin(), sortedBases.end());

	std::vector<AppInput> inputs;
	inputs.reserve(sortedBases.size());
	for (const std::uint32_t baseAppId : sortedBases)
	{
		AppInput input;
		input.baseAppId = baseAppId;

		try
		{
			std::string wire;
			input.cacheValid =
				AppInfoProvision::readValidatedCacheBuffer(baseAppId, wire);
			if (input.cacheValid)
			{
				const auto sources =
					AppInfoProvision::extractDlcAppIdsBySource(wire, baseAppId);
				std::unordered_set<std::uint32_t> advertisedWithContent;
				const bool baseHasDlcDepots =
					AppInfoProvision::hasDepotsInDlc(wire);
				for (const std::uint32_t dlcId : sources.advertised)
				{
					if (baseHasDlcDepots ||
						!DepotKey::managedDepotsForApp(dlcId).empty())
					{
						advertisedWithContent.insert(dlcId);
					}
				}

				const auto selected = AppInfoProvision::selectDlcInjectionIds(
					sources, advertisedWithContent,
					g_config.injectAllAdvertisedDlc.get());
				input.plannerAppIds = selected.package0;
			}

			input.depotIds = DepotKey::managedDepotsForApp(baseAppId);
			for (const std::uint32_t plannerAppId : input.plannerAppIds)
			{
				const auto dlcDepots =
					DepotKey::managedDepotsForApp(plannerAppId);
				input.depotIds.insert(
					input.depotIds.end(), dlcDepots.begin(), dlcDepots.end());
			}
		}
		catch (...)
		{
			// One malformed or concurrently-changing app remains unresolved.  Its
			// base id still participates, and no partial planner ids escape.
			input.cacheValid = false;
			input.plannerAppIds.clear();
		}

		inputs.push_back(std::move(input));
	}

	try
	{
		return build(generation, inputs);
	}
	catch (...)
	{
		BuildResult failed;
		failed.snapshot.generation = generation;
		failed.snapshot.metadataComplete = false;
		return failed;
	}
}
} // namespace HotReloadInputs
