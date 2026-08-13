// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "hotreload_types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace HotReloadPublishPolicy
{
inline constexpr bool shouldEvaluateInputs(
	bool initialPublication,
	bool membershipChanged,
	bool forceSourceRefresh) noexcept
{
	return initialPublication || membershipChanged || forceSourceRefresh;
}

inline constexpr bool shouldPublish(
	bool initialPublication,
	bool membershipChanged,
	bool fingerprintsChanged) noexcept
{
	return initialPublication || membershipChanged || fingerprintsChanged;
}

inline constexpr bool shouldAwaitDlcMetadata(
	bool initialPublication,
	bool addedBaseNeedsMetadata) noexcept
{
	return !initialPublication && addedBaseNeedsMetadata;
}

inline constexpr bool metadataRepairDue(
	bool hasPending,
	std::uint64_t nowMs,
	std::uint64_t retryAfterMs) noexcept
{
	return hasPending && nowMs >= retryAfterMs;
}

inline constexpr std::uint64_t metadataRepairDeadlineMs(
	std::uint64_t nowMs,
	std::uint64_t delayMs) noexcept
{
	const auto max = std::numeric_limits<std::uint64_t>::max();
	return delayMs > max - nowMs ? max : nowMs + delayMs;
}

inline constexpr bool shouldArmMetadataRepair(
	bool postLoginOpportunitySeen) noexcept
{
	return !postLoginOpportunitySeen;
}

inline constexpr bool metadataCacheVisibleInSession(
	bool cacheReady,
	bool deferredUntilRestart) noexcept
{
	return cacheReady && !deferredUntilRestart;
}

inline constexpr bool metadataRepairDefersUntilRestart(
	bool runtimePending) noexcept
{
	return !runtimePending;
}

struct MetadataRepairCandidate
{
	std::uint32_t appId = 0;
	bool runtimePending = false;
	std::int64_t cacheMtimeSecs = 0;
};

inline std::vector<std::uint32_t> prioritizeMetadataRepairs(
	std::vector<MetadataRepairCandidate> candidates)
{
	std::sort(candidates.begin(), candidates.end(),
		[](const MetadataRepairCandidate& left,
		   const MetadataRepairCandidate& right) {
			if (left.runtimePending != right.runtimePending)
				return left.runtimePending > right.runtimePending;
			if (left.cacheMtimeSecs != right.cacheMtimeSecs)
				return left.cacheMtimeSecs > right.cacheMtimeSecs;
			return left.appId < right.appId;
		});
	std::vector<std::uint32_t> out;
	out.reserve(candidates.size());
	for (const auto& candidate : candidates)
		if (candidate.appId != 0) out.push_back(candidate.appId);
	return out;
}

inline std::vector<std::uint32_t> mergeMetadataRepairIds(
	const std::vector<std::uint32_t>& missing,
	const std::vector<std::uint32_t>& runtimePending)
{
	std::vector<std::uint32_t> out = missing;
	out.insert(out.end(), runtimePending.begin(), runtimePending.end());
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	out.erase(std::remove(out.begin(), out.end(), 0), out.end());
	return out;
}

inline bool metadataSnapshotChanged(
	const PackageSnapshot& previous,
	const PackageSnapshot& next) noexcept
{
	return previous.appIds != next.appIds ||
		previous.depotIds != next.depotIds ||
		previous.metadataComplete != next.metadataComplete;
}
} // namespace HotReloadPublishPolicy
