// TDD regression tests for Phase 4.5 provisioning scheduling.

#include "feats/provision_schedule.hpp"

#include <cstdio>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main()
{
    using AppInfoProvision::CacheReadiness;
    using AppInfoProvision::RefreshScheduleAction;
    using AppInfoProvision::cachePairReady;
    using AppInfoProvision::coldFallbackNeeded;
    using AppInfoProvision::refreshScheduleAction;
    using AppInfoProvision::shouldRequeueRefreshAfterStartFailure;
    using AppInfoProvision::shouldRerunPendingRefresh;
    using AppInfoProvision::asyncProvisionEnabled;
    using AppInfoProvision::shouldWarmCurlBeforePics;
    using AppInfoProvision::shouldSkipStaleAsyncSplice;
    using AppInfoProvision::shouldMarkColdCacheSanitized;
    using AppInfoProvision::PreinitProvisionAction;
    using AppInfoProvision::preinitProvisionAction;

    check(preinitProvisionAction(true) ==
              PreinitProvisionAction::ColdFallbackThenSplice,
          "async setup provisions only missing apps before the appinfo splice");
    check(preinitProvisionAction(false) ==
              PreinitProvisionAction::SynchronousProvision,
          "disabled async mode keeps the full synchronous setup pass");

    // The preinit pass re-fetches a stale pair on purpose (live gid before the
    // splice); the PICS callback must not, or every response past the TTL
    // re-provisions the whole fleet on Steam's worker thread.
    check(coldFallbackNeeded(CacheReadiness::Missing, /*requireFresh=*/true),
          "a missing pair is always a cold start");
    check(coldFallbackNeeded(CacheReadiness::Missing, /*requireFresh=*/false),
          "a missing pair is a cold start for the callback too");
    check(coldFallbackNeeded(CacheReadiness::ValidStale, /*requireFresh=*/true),
          "startup re-fetches a stale pair to pin the live gid");
    check(!coldFallbackNeeded(CacheReadiness::ValidStale, /*requireFresh=*/false),
          "a valid stale pair does not drag the callback into a full pass");
    check(!coldFallbackNeeded(CacheReadiness::Fresh, /*requireFresh=*/true),
          "a fresh pair is never a cold start");
    check(!coldFallbackNeeded(CacheReadiness::Fresh, /*requireFresh=*/false),
          "a fresh pair is never a cold start for the callback");

    check(cachePairReady(true, true, true),
          "only a complete and validated cache pair is ready");
    check(!cachePairReady(true, false, true),
          "a buffer without metadata is not a ready cache pair");
    check(!cachePairReady(true, true, false),
          "an invalid cache record is not a ready cache pair");
    check(refreshScheduleAction(true, true, false) ==
              RefreshScheduleAction::Start,
          "the first refresh request starts a worker");
    check(refreshScheduleAction(true, true, true) ==
              RefreshScheduleAction::Queue,
          "a refresh request arriving during an active worker is queued");
    check(refreshScheduleAction(false, true, false) ==
              RefreshScheduleAction::Ignore,
          "a disabled async refresh is ignored");
    check(refreshScheduleAction(true, false, false) ==
              RefreshScheduleAction::Ignore,
          "a refresh with no managed apps is ignored");
    check(shouldRerunPendingRefresh(true, true, true),
          "a pending refresh reruns after the active worker exits");
    check(!shouldRerunPendingRefresh(false, true, true),
          "a pending refresh is dropped when async provisioning is disabled");
    check(!shouldRerunPendingRefresh(true, false, true),
          "a pending refresh is dropped when managed apps are gone");
    check(!shouldRerunPendingRefresh(true, true, false),
          "an idle refresh completion does not start a second worker");
    check(shouldRequeueRefreshAfterStartFailure(false),
          "a known failed follow-up start retains the queued refresh");
    check(!shouldRequeueRefreshAfterStartFailure(true),
          "an uncertain worker recovery keeps the active refresh gate");
    check(shouldMarkColdCacheSanitized(false),
          "provider-normalized cold results suppress raw PICS replacement");
    check(!shouldMarkColdCacheSanitized(true),
          "explicit fallback stays replaceable until cache provenance is checked");

    check(asyncProvisionEnabled(true, nullptr),
          "config enables async provisioning by default");
    check(!asyncProvisionEnabled(true, "0"),
          "environment zero disables async provisioning");
    check(!asyncProvisionEnabled(true, "false"),
          "environment false disables async provisioning");
    check(asyncProvisionEnabled(false, "1"),
          "environment one can re-enable async provisioning");
    check(!asyncProvisionEnabled(true, "unexpected"),
          "invalid environment value fails closed");
    check(shouldWarmCurlBeforePics(true),
          "managed apps require curl warmup before the PICS worker");
    check(shouldWarmCurlBeforePics(false),
          "future hot-adds also require curl warmup before the PICS worker");

    if (failures != 0)
    {
        std::fprintf(stderr, "test_provision_schedule: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("provision schedule tests passed");
    return 0;
}
