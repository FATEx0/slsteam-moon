// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure scheduling policy for Phase 4.5 provisioning.

#pragma once

#include <string_view>

namespace AppInfoProvision
{

enum class RefreshScheduleAction
{
	Ignore,
	Start,
	Queue,
};

inline RefreshScheduleAction refreshScheduleAction(bool asyncEnabled,
                                                   bool hasManagedApps,
                                                   bool inFlight)
{
	if (!asyncEnabled || !hasManagedApps)
		return RefreshScheduleAction::Ignore;
	return inFlight ? RefreshScheduleAction::Queue
	                : RefreshScheduleAction::Start;
}

inline bool shouldRerunPendingRefresh(bool asyncEnabled,
                                      bool hasManagedApps,
                                      bool pending)
{
	return pending && asyncEnabled && hasManagedApps;
}

// A known startup failure leaves no worker to drain a queued request, so the
// request must remain retryable. An uncertain detach recovery may still have
// a live worker; that worker must retain the gate and drain the queue itself.
inline bool shouldRequeueRefreshAfterStartFailure(bool workerMayStillExist)
{
	return !workerMayStillExist;
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

// PICS provisioning can be triggered for a game added after setup() as well
// as for apps already managed at boot. Warm the loader unconditionally so a
// later hot-add cannot become the first worker-thread dlopen.
inline bool shouldWarmCurlBeforePics(bool /*hasManagedApps*/)
{
	return true;
}

// The pass mutex serializes cold provisioning with the sanctioned background
// refresh. Do not suppress a missing app merely because some other refresh is
// in flight: that worker may have snapshotted a different app set.
inline bool shouldRunColdFallback(bool cacheReady)
{
	return !cacheReady;
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

inline bool shouldSkipStaleAsyncSplice(bool asyncEnabled,
                                       bool explicitFallback)
{
	return asyncEnabled && !explicitFallback;
}

// A cache accepted only as an explicit stale fallback must remain eligible for
// a newer raw PICS response. Fresh/provider-normalized cold results can be
// marked in-process so the callback does not race their publication.
inline bool shouldMarkColdCacheSanitized(bool explicitFallback)
{
	return !explicitFallback;
}

} // namespace AppInfoProvision
