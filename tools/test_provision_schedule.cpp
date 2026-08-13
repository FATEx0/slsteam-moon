// TDD regression tests for Phase 4.5 provisioning scheduling.

#include "feats/provision_schedule.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include <vector>

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
    using AppInfoProvision::cachePairReady;
    using AppInfoProvision::coldFallbackNeeded;
    using AppInfoProvision::shouldRequeueRefreshAfterStartFailure;
    using AppInfoProvision::asyncProvisionEnabled;
    using AppInfoProvision::shouldWarmCurlBeforePics;
    using AppInfoProvision::shouldMarkColdCacheSanitized;
    using AppInfoProvision::PreinitProvisionAction;
    using AppInfoProvision::preinitProvisionAction;
    using AppInfoProvision::mergeRuntimePublishCandidates;
    using AppInfoProvision::selectRuntimePublishCandidates;

    check(preinitProvisionAction(true) ==
              PreinitProvisionAction::ColdFallbackThenSplice,
          "async setup provisions only missing apps before the appinfo splice");
    check(preinitProvisionAction(false) ==
              PreinitProvisionAction::SynchronousProvision,
          "disabled async mode keeps the full synchronous setup pass");

    // A structurally valid pair is startup-usable regardless of its freshness.
    // The PICS callback likewise keeps stale pairs out of synchronous work.
    check(coldFallbackNeeded(CacheReadiness::Missing),
          "a missing pair is always a cold start");
    check(!coldFallbackNeeded(CacheReadiness::ValidStale),
          "startup accepts a valid stale pair without synchronous recovery");
    check(!coldFallbackNeeded(CacheReadiness::Fresh),
          "a fresh pair is never a cold start");

    check(cachePairReady(true, true, true),
          "only a complete and validated cache pair is ready");
    check(!cachePairReady(true, false, true),
          "a buffer without metadata is not a ready cache pair");
    check(!cachePairReady(true, true, false),
          "an invalid cache record is not a ready cache pair");
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

    const std::unordered_set<std::uint32_t> managed{420530, 777, 888};
    const std::unordered_set<std::uint32_t> synthetic{420530, 999, 888};
    check(selectRuntimePublishCandidates(
              std::vector<std::uint32_t>{999, 420530, 420530, 777},
              managed, synthetic) == std::vector<std::uint32_t>{420530},
          "runtime publication selects only current requested synthetic apps");
    check(mergeRuntimePublishCandidates(
              std::vector<std::uint32_t>{888, 420530},
              std::vector<std::uint32_t>{777, 888, 777}) ==
              std::vector<std::uint32_t>({777, 888, 420530}),
          "queued refreshes retain a sorted unique union of publication ids");

    if (failures != 0)
    {
        std::fprintf(stderr, "test_provision_schedule: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("provision schedule tests passed");
    return 0;
}
