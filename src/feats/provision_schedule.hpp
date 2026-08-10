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

// How much of a cache pair a caller found on disk.
enum class CacheReadiness
{
	Missing,     // absent, incomplete, or failed validation
	ValidStale,  // complete and validated, but older than the freshness window
	Fresh,       // complete, validated and inside the freshness window
};

// Decide whether an app must take the SYNCHRONOUS provisioning path.
//
// The two callers want different things and conflating them was expensive:
//
//   * setup()/preinit (requireFresh = true) re-fetches a stale pair on purpose,
//     because the live public gid must be current before appinfo.vdf is
//     spliced; serving a cross-session buffer reintroduces the staged-gid vs
//     requested-gid mismatch that broke first-attempt installs.
//
//   * the PICS callback (requireFresh = false) runs on Steam's own worker
//     thread while the user is clicking. A valid pair already serves the app
//     there, and refreshing a stale one is the async worker's job. Treating
//     stale as cold made every response past the TTL re-provision the whole
//     fleet inline — 101 apps, one CM batch, a full DLC recollection and a
//     package-0 broadcast — which is exactly what froze the install dialog.
inline bool coldFallbackNeeded(CacheReadiness readiness, bool requireFresh)
{
	if (readiness == CacheReadiness::Missing) return true;
	return requireFresh && readiness != CacheReadiness::Fresh;
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
