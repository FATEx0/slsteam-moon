// SPDX-License-Identifier: AGPL-3.0-only

#include "hotreload.hpp"

#include "appinfo_provision.hpp"
#include "appinfo_vdf.hpp"
#include "appinfostate.hpp"
#include "hotreload_inputs.hpp"
#include "hotreload_publish_policy.hpp"
#include "libraryremoval.hpp"
#include "manifeststore.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../ownerwork.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct CoordinatorState
{
	std::mutex mutex;
	bool initialized = false;
	std::uint64_t generation = 0;
	std::unordered_set<std::uint32_t> managedAppIds;
	std::unordered_map<std::uint32_t, std::string> contentFingerprints;
};

CoordinatorState& coordinator()
{
	static CoordinatorState state;
	return state;
}

HotReloadState::Store& membershipStore()
{
	// AppInfoState retains a non-owning pointer to this object.  Function-static
	// lifetime keeps it valid through hook teardown and any retained detour.
	static HotReloadState::Store state;
	return state;
}

std::vector<std::uint32_t> difference(
	const std::unordered_set<std::uint32_t>& left,
	const std::unordered_set<std::uint32_t>& right)
{
	std::vector<std::uint32_t> sortedLeft(left.begin(), left.end());
	std::vector<std::uint32_t> sortedRight(right.begin(), right.end());
	std::sort(sortedLeft.begin(), sortedLeft.end());
	std::sort(sortedRight.begin(), sortedRight.end());

	std::vector<std::uint32_t> result;
	result.reserve(sortedLeft.size());
	std::set_difference(
		sortedLeft.begin(), sortedLeft.end(),
		sortedRight.begin(), sortedRight.end(),
		std::back_inserter(result));
	return result;
}

bool publishLocked(
	CoordinatorState& state,
	const std::unordered_set<std::uint32_t>& managedAppIds,
	bool force,
	bool allowBackgroundRefresh)
{
	const bool initialPublication = state.generation == 0;
	const bool membershipChanged = managedAppIds != state.managedAppIds;
	if (!HotReloadPublishPolicy::shouldEvaluateInputs(
		initialPublication, membershipChanged, force))
		return false;

	const auto added = difference(managedAppIds, state.managedAppIds);
	const auto removed = difference(state.managedAppIds, managedAppIds);
	const std::uint64_t nextGeneration = state.generation + 1;
	std::unordered_map<std::uint32_t, std::string> nextFingerprints;
	std::vector<std::uint32_t> locallyChanged;
	const auto archivedGids = ManifestStore::archivedGidIndex();
	for (const std::uint32_t appId : managedAppIds)
	{
		const std::string current =
			AppInfoProvision::localContentFingerprint(appId, archivedGids);
		const auto old = state.contentFingerprints.find(appId);
		if (old == state.contentFingerprints.end() || old->second != current)
			locallyChanged.push_back(appId);
		nextFingerprints.emplace(appId, current);
	}
	const bool fingerprintsChanged = !locallyChanged.empty();
	if (!HotReloadPublishPolicy::shouldPublish(
		initialPublication, membershipChanged, fingerprintsChanged))
	{
		return false;
	}
	for (const auto& [appId, fingerprint] : nextFingerprints)
		AppInfoProvision::primeTerminalMemo(appId, fingerprint);
	auto built = HotReloadInputs::buildFromCaches(
		nextGeneration, managedAppIds);
	if (!built.valid)
	{
		if (g_pLog != nullptr)
		{
			g_pLog->warn(
				"HotReload: generation %llu exceeds bounded snapshot capacity; "
				"previous state retained\n",
				static_cast<unsigned long long>(nextGeneration));
		}
		return false;
	}
	built.snapshot.addedAppIds = added;

	std::unordered_set<std::uint32_t> guardedAppIds(
		built.snapshot.appIds.begin(), built.snapshot.appIds.end());
	(void)membershipStore().publish(guardedAppIds);
	for (const std::uint32_t appId : added)
		LibraryRemoval::cancel(appId);

	const OwnerWork::Mode mode =
		OwnerWork::submitManagedState(built.snapshot);
	if (mode == OwnerWork::Mode::Abandoned)
	{
		if (g_pLog != nullptr)
			g_pLog->warn("HotReload: managed snapshot abandoned during teardown\n");
		return false;
	}

	state.managedAppIds = managedAppIds;
	state.generation = nextGeneration;
	state.contentFingerprints = std::move(nextFingerprints);
	for (const std::uint32_t appId : removed)
		LibraryRemoval::queue(appId);

	if (g_pLog != nullptr)
	{
		g_pLog->info(
			"HotReload: generation %llu dispatched %s (managed=%zu added=%zu "
			"removed=%zu planner_apps=%zu depots=%zu metadata=%s)\n",
			static_cast<unsigned long long>(nextGeneration),
			OwnerWork::modeName(mode), managedAppIds.size(), added.size(),
			removed.size(), built.snapshot.appIds.size(),
			built.snapshot.depotIds.size(),
			built.snapshot.metadataComplete ? "complete" : "pending");
	}

	if (allowBackgroundRefresh && (!added.empty() || !locallyChanged.empty()))
	{
		const std::string appinfoPath = AppInfoVdf::findExistingPath();
		if (!appinfoPath.empty())
		{
			// Best effort only.  Live readiness is driven by the appinfo hook;
			// this detached pass merely publishes a reusable disk cache.
			std::unordered_set<std::uint32_t> addedSet(added.begin(), added.end());
			std::vector<AppInfoProvision::RefreshRequest> requests;
			requests.reserve(added.size() + locallyChanged.size());
			for (const std::uint32_t appId : added)
				requests.push_back({appId, 0,
					AppInfoProvision::snapshotCachePublication(appId).generation,
					AppInfoProvision::reasonMask(
						AppInfoProvision::RefreshReason::HotAdd), true, true});
			for (const std::uint32_t appId : locallyChanged)
			{
				if (addedSet.count(appId) != 0) continue;
				requests.push_back({appId, 0,
					AppInfoProvision::snapshotCachePublication(appId).generation,
					AppInfoProvision::reasonMask(
						AppInfoProvision::RefreshReason::LocalInputs), true, true});
			}
			AppInfoProvision::refreshInBackground(appinfoPath, requests);
		}
	}

	return true;
}
} // namespace

namespace HotReload
{
HotReloadState::Store& store() noexcept
{
	return membershipStore();
}

void initialize() noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.initialized)
			return;

		// Hooks::setup may already have installed the optional detour against its
		// bootstrap Store. Rebind it before the first runtime snapshot becomes
		// visible; failure remains restart-recoverable in PackagePatch.
		const bool guardReady = AppInfoState::setup(membershipStore());
		state.initialized = true;
		(void)publishLocked(
			state, g_config.managedAppIds.get(), true, false);

		if (!guardReady && g_pLog != nullptr)
		{
			g_pLog->warn(
				"HotReload: appinfo guard unavailable; unresolved additions defer "
				"until restart\n");
		}
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn("HotReload: initialization failed; startup state retained\n");
	}
}

void publish(
	const std::unordered_set<std::uint32_t>& managedAppIds,
	bool forceSourceRefresh) noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.initialized)
			return;
		(void)publishLocked(
			state, managedAppIds, forceSourceRefresh, true);
	}
	catch (...)
	{
		if (g_pLog != nullptr)
			g_pLog->warn("HotReload: watcher publication failed; previous state retained\n");
	}
}

void shutdown() noexcept
{
	try
	{
		CoordinatorState& state = coordinator();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (!state.initialized)
			return;
		state.initialized = false;
	}
	catch (...)
	{
		// Teardown continues through Hooks::remove, which disables the appinfo
		// hook and closes the owner queue independently.
	}
}
} // namespace HotReload
