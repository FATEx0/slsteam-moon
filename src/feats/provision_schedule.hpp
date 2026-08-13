// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure scheduling policy for Phase 4.5 provisioning.

#pragma once

#include "provision_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace AppInfoProvision
{

// A known startup failure leaves no worker to drain a queued request, so the
// request must remain retryable. An uncertain detach recovery may still have
// a live worker; that worker must retain the gate and drain the queue itself.
inline bool shouldRequeueRefreshAfterStartFailure(bool workerMayStillExist)
{
	return !workerMayStillExist;
}

inline std::vector<std::uint32_t> mergeRuntimePublishCandidates(
	std::vector<std::uint32_t> current,
	const std::vector<std::uint32_t>& incoming)
{
	current.insert(current.end(), incoming.begin(), incoming.end());
	std::sort(current.begin(), current.end());
	current.erase(std::unique(current.begin(), current.end()), current.end());
	current.erase(std::remove(current.begin(), current.end(), 0), current.end());
	return current;
}

inline std::vector<std::uint32_t> selectRuntimePublishCandidates(
	const std::vector<std::uint32_t>& requested,
	const std::unordered_set<std::uint32_t>& managed,
	const std::unordered_set<std::uint32_t>& synthetic)
{
	std::vector<std::uint32_t> selected;
	selected.reserve(requested.size());
	for (const std::uint32_t appId : requested)
	{
		if (appId != 0 && managed.count(appId) != 0 &&
			synthetic.count(appId) != 0)
		{
			selected.push_back(appId);
		}
	}
	std::sort(selected.begin(), selected.end());
	selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
	return selected;
}

enum class PreinitProvisionAction
{
	SynchronousProvision,
	ColdFallbackThenSplice,
};

inline PreinitProvisionAction preinitProvisionAction(bool asyncEnabled)
{
	return asyncEnabled
		? PreinitProvisionAction::ColdFallbackThenSplice
		: PreinitProvisionAction::SynchronousProvision;
}

// A cache is warm only when both files are present and the decoded record has
// passed its metadata, digest, and content checks.  A lone .bin must remain a
// cold-start case so a failed metadata publication cannot force a second boot.
inline bool cachePairReady(bool bufferExists, bool metadataExists,
                           bool recordValid)
{
	return bufferExists && metadataExists && recordValid;
}

// Decide whether an app must take the SYNCHRONOUS provisioning path. Both
// startup and the PICS callback require a structurally usable cache pair, not
// a fresh one: a validated stale pair is usable while missing and invalid
// pairs require recovery. Busy and unverified states remain deferred work.
inline bool coldFallbackNeeded(CacheReadiness readiness)
{
	if (readiness == CacheReadiness::Missing ||
	    readiness == CacheReadiness::Invalid) return true;
	return false;
}

// PICS provisioning can be triggered for a game added after setup() as well
// as for apps already managed at boot. Warm the loader unconditionally so a
// later hot-add cannot become the first worker-thread dlopen.
inline bool shouldWarmCurlBeforePics(bool /*hasManagedApps*/)
{
	return true;
}

// Resolve the config value and the optional field override. Invalid explicit
// values fail closed: a field/debug typo must not silently enable a new
// startup behavior in the field.
inline bool asyncProvisionEnabled(bool configEnabled, const char* envOverride)
{
	if (!envOverride || *envOverride == '\0') return configEnabled;

	const std::string_view value(envOverride);
	if (value == "1" || value == "true" || value == "yes" || value == "on")
		return true;
	if (value == "0" || value == "false" || value == "no" || value == "off")
		return false;
	return false;
}

// A cache accepted only as an explicit stale fallback must remain eligible for
// a newer raw PICS response. Fresh/provider-normalized cold results can be
// marked in-process so the callback does not race their publication.
inline bool shouldMarkColdCacheSanitized(bool explicitFallback)
{
	return !explicitFallback;
}

} // namespace AppInfoProvision
