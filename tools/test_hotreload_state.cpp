#include "../src/feats/hotreload_state.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

} // namespace

static_assert(std::is_const_v<decltype(HotReloadState::Record::appId)>,
              "Record::appId must be immutable");
static_assert(std::is_const_v<decltype(HotReloadState::Record::generation)>,
              "Record::generation must be immutable");
static_assert(!std::is_copy_constructible_v<HotReloadState::Store::ReadHandle>,
              "ReadHandle must be move-only");
static_assert(std::is_nothrow_move_constructible_v<HotReloadState::Store::ReadHandle>,
              "ReadHandle moves must be noexcept");
static_assert(std::is_nothrow_default_constructible_v<HotReloadState::Store>,
              "bootstrap Store construction must be noexcept");
static_assert(noexcept(std::declval<const HotReloadState::Store&>().contains(0)),
              "hook membership lookup must stay noexcept");
static_assert(noexcept(std::declval<HotReloadState::Store&>().noteResolved(0)),
              "hook resolution signaling must stay noexcept");
static_assert(noexcept(std::declval<const HotReloadState::Store&>().resolvedDirtyHint()),
              "owner-frame dirty hint must stay lock-free and noexcept");
static_assert(!noexcept(std::declval<HotReloadState::Store&>().takeResolvedDirty()),
              "worker dirty consumption may lock and must not be noexcept");
static_assert(!noexcept(std::declval<const HotReloadState::Store&>().snapshot()),
              "worker snapshot may lock/allocate and must not be noexcept");

static bool test_pinned_read_handle()
{
    bool ok = true;
    HotReloadState::Store store;
    store.publish({77});

    auto handle = store.readHandle();
    ok &= check(handle.contains(77),
                "read handle finds the member in its pinned snapshot");
    auto moved = std::move(handle);
    ok &= check(moved.noteResolved(),
                "read handle resolves the record it already found");
    ok &= check(store.resolvedDirtyHint(),
                "lock-free hint observes pending resolved metadata");
    ok &= check(!store.noteResolved(77),
                "the same generation coalesces duplicate resolution");
    ok &= check(store.takeResolvedDirty(),
                "read-handle resolution raises the dirty signal");
    ok &= check(!store.resolvedDirtyHint(),
                "consuming the dirty signal clears the lock-free hint");
    ok &= check(!store.takeResolvedDirty(),
                "dirty signal is consumed once");

    store.publish({});
    store.publish({77});
    ok &= check(!moved.noteResolved(),
                "a retained handle cannot complete a removed generation");
    ok &= check(!store.takeResolvedDirty(),
                "removed-generation completion cannot signal current state");

    auto current = store.readHandle();
    ok &= check(current.contains(77),
                "a fresh handle sees the re-added member");
    ok &= check(current.noteResolved(),
                "the re-added generation can resolve again");
    ok &= check(store.takeResolvedDirty(),
                "the re-added generation signals once");

    return ok;
}

static bool test_concurrent_publish_readd()
{
    bool ok = true;
    HotReloadState::Store store;
    store.publish({91});

    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::atomic<bool> oldContains{false};
    std::atomic<bool> oldResolved{true};

    std::thread reader([&]
    {
        auto handle = store.readHandle();
        held.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        oldContains.store(handle.contains(91), std::memory_order_release);
        oldResolved.store(handle.noteResolved(), std::memory_order_release);
    });

    while (!held.load(std::memory_order_acquire))
        std::this_thread::yield();
    store.publish({});
    store.publish({91});
    release.store(true, std::memory_order_release);
    reader.join();

    ok &= check(oldContains.load(std::memory_order_acquire),
                "a pinned reader remains valid across publish and re-add");
    ok &= check(!oldResolved.load(std::memory_order_acquire),
                "the old reader cannot signal a stale re-added generation");
    ok &= check(!store.takeResolvedDirty(),
                "the concurrent stale completion leaves current dirty state clear");
    return ok;
}

int main() {
    bool ok = true;
    HotReloadState::Store store;

    auto first = store.publish({10, 20});
    ok &= check(first.changed() && first.generation == 1,
                "changed publish advances generation");
    ok &= check(first.added == std::vector<uint32_t>({10, 20}),
                "IDs are sorted");
    ok &= check(first.removed.empty(), "initial publish has no removals");
    ok &= check(store.contains(10) && !store.contains(30),
                "snapshot lookup works");

    const auto firstSnapshot = store.snapshot();
    ok &= check(firstSnapshot->generation == 1 &&
                    firstSnapshot->records.size() == 2 &&
                    firstSnapshot->records[0]->appId == 10 &&
                    firstSnapshot->records[1]->appId == 20,
                "snapshot records are sorted");
    const auto firstTwenty = firstSnapshot->records[1];

    auto same = store.publish({20, 10});
    ok &= check(!same.changed() && same.generation == 1,
                "equivalent set is a no-op");
    ok &= check(same.snapshot == firstSnapshot,
                "no-op keeps the immutable snapshot");

    auto second = store.publish({20, 30});
    ok &= check(second.changed() && second.generation == 2,
                "changed membership advances generation");
    ok &= check(second.added == std::vector<uint32_t>({30}),
                "new ID is reported");
    ok &= check(second.removed == std::vector<uint32_t>({10}),
                "removed ID is reported");

    const auto secondSnapshot = store.snapshot();
    ok &= check(secondSnapshot->records.size() == 2 &&
                    secondSnapshot->records[0]->appId == 20 &&
                    secondSnapshot->records[1]->appId == 30,
                "next snapshot remains sorted");
    ok &= check(secondSnapshot->records[0] == firstTwenty,
                "unchanged IDs preserve their record objects");
    ok &= check(secondSnapshot->records[0]->generation == 1 &&
                    secondSnapshot->records[1]->generation == 2,
                "records retain their introduction generations");

    ok &= check(store.noteResolved(20),
                "first resolution signals owner");
    ok &= check(store.noteResolved(30),
                "distinct resolution signals owner");
    ok &= check(!store.noteResolved(20),
                "duplicate resolution coalesces");
    ok &= check(!store.noteResolved(30),
                "the second distinct resolution also coalesces duplicates");
    const bool firstDirty = store.takeResolvedDirty();
    const bool secondDirty = store.takeResolvedDirty();
    ok &= check(firstDirty && !secondDirty,
                "distinct resolutions share one coalesced dirty signal");
    ok &= check(!store.noteResolved(10),
                "removed IDs cannot signal");
    ok &= check(!store.takeResolvedDirty(),
                "removed resolution failure leaves dirty signal clear");
    ok &= check(!store.noteResolved(99),
                "absent IDs cannot signal");
    ok &= check(!store.takeResolvedDirty(),
                "absent resolution failure leaves dirty signal clear");

    auto third = store.publish({20, 30, 40});
    ok &= check(third.generation == 3 &&
                    third.added == std::vector<uint32_t>({40}) &&
                    third.removed.empty(),
                "a later membership change reports sorted additions");
    ok &= check(!store.noteResolved(20),
                "a preserved record signals at most once");
    ok &= check(store.noteResolved(40),
                "a new record can signal in its active generation");
    const bool laterDirty = store.takeResolvedDirty();
    const bool laterSecondDirty = store.takeResolvedDirty();
    ok &= check(laterDirty && !laterSecondDirty,
                "a later dirty signal is coalesced");

    auto removed = store.publish({30});
    ok &= check(removed.generation == 4 &&
                    removed.removed == std::vector<uint32_t>({20, 40}),
                "remove transition advances generation");
    const auto finalSnapshot = store.snapshot();
    ok &= check(finalSnapshot->records.size() == 1 &&
                    finalSnapshot->records[0]->appId == 30,
                "removed IDs are absent from the current snapshot");
    ok &= check(!store.noteResolved(20),
                "removed generation cannot signal");
    ok &= check(!store.takeResolvedDirty(),
                "removed generation failure leaves dirty signal clear");

    auto readded = store.publish({20, 30});
    ok &= check(readded.generation == 5 &&
                    readded.added == std::vector<uint32_t>({20}) &&
                    readded.removed.empty(),
                "re-add advances generation with a new membership");
    const auto readdedSnapshot = store.snapshot();
    const auto readdedTwenty = readdedSnapshot->records[0];
    ok &= check(readdedTwenty->appId == 20 &&
                    readdedTwenty != firstTwenty &&
                    readdedTwenty->generation == 5 &&
                    readdedTwenty->generation > firstTwenty->generation &&
                    readdedTwenty->resolvedGeneration.load(
                        std::memory_order_relaxed) == 0,
                "re-add creates a fresh record with reset resolution state");
    ok &= check(store.noteResolved(20),
                "re-added record can signal again");
    const bool readdedDirty = store.takeResolvedDirty();
    const bool readdedSecondDirty = store.takeResolvedDirty();
    ok &= check(readdedDirty && !readdedSecondDirty,
                "re-added resolution has one coalesced dirty signal");

    ok &= test_pinned_read_handle();
    ok &= test_concurrent_publish_readd();

    return ok ? 0 : 1;
}
