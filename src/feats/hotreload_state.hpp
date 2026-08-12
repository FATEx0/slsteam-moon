#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace HotReloadState {

struct Record {
    const uint32_t appId;
    const uint64_t generation;
    std::atomic<uint64_t> resolvedGeneration{0};

    Record(uint32_t appId, uint64_t generation) noexcept
        : appId(appId), generation(generation) {}
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "record generation state must be lock-free");

struct Snapshot {
    uint64_t generation = 0;
    std::vector<std::shared_ptr<Record>> records;
};

struct Transition {
    uint64_t generation = 0;
    std::vector<uint32_t> added;
    std::vector<uint32_t> removed;
    std::shared_ptr<const Snapshot> snapshot;

    [[nodiscard]] bool changed() const noexcept {
        return !added.empty() || !removed.empty();
    }
};

class Store {
public:
    class ReadHandle {
    public:
        ReadHandle() noexcept = default;
        ReadHandle(const ReadHandle&) = delete;
        ReadHandle& operator=(const ReadHandle&) = delete;

        ReadHandle(ReadHandle&& other) noexcept
            : store_(other.store_),
              snapshot_(other.snapshot_),
              record_(other.record_),
              lookupDone_(other.lookupDone_) {
            other.releaseOwnership();
        }

        ReadHandle& operator=(ReadHandle&& other) noexcept {
            if (this != &other) {
                release();
                store_ = other.store_;
                snapshot_ = other.snapshot_;
                record_ = other.record_;
                lookupDone_ = other.lookupDone_;
                other.releaseOwnership();
            }
            return *this;
        }

        ~ReadHandle() noexcept {
            release();
        }

        bool contains(uint32_t appId) noexcept {
            if (lookupDone_) {
                return record_ != nullptr && record_->appId == appId;
            }

            lookupDone_ = true;
            record_ = store_ != nullptr && snapshot_ != nullptr
                ? Store::findRecord(*snapshot_, appId)
                : nullptr;
            return record_ != nullptr;
        }

        bool noteResolved() noexcept {
            if (store_ == nullptr || record_ == nullptr) {
                return false;
            }

            // The handle retains the record it found.  Accept completion only
            // while that exact record/generation is still current.  If a
            // publish races after this check, takeResolvedDirty() performs the
            // same generation validation before exposing the signal.
            const Snapshot* current =
                store_->currentRaw_.load(std::memory_order_seq_cst);
            Record* currentRecord = current == nullptr
                ? nullptr
                : Store::findRecord(*current, record_->appId);
            if (currentRecord != record_ ||
                currentRecord->generation != record_->generation) {
                return false;
            }

            uint64_t unresolved = 0;
            if (!record_->resolvedGeneration.compare_exchange_strong(
                    unresolved, record_->generation,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return false;
            }

            store_->resolvedDirtyGeneration_.store(
                record_->generation, std::memory_order_release);
            return true;
        }

    private:
        friend class Store;

        explicit ReadHandle(const Store& store) noexcept
            : store_(&store) {
            store_->activeReaders_.fetch_add(
                1, std::memory_order_seq_cst);
            snapshot_ = store_->currentRaw_.load(
                std::memory_order_seq_cst);
        }

        void releaseOwnership() noexcept {
            otherReset();
        }

        void otherReset() noexcept {
            store_ = nullptr;
            snapshot_ = nullptr;
            record_ = nullptr;
            lookupDone_ = false;
        }

        void release() noexcept {
            if (store_ != nullptr) {
                store_->activeReaders_.fetch_sub(
                    1, std::memory_order_seq_cst);
            }
            otherReset();
        }

        const Store* store_ = nullptr;
        const Snapshot* snapshot_ = nullptr;
        Record* record_ = nullptr;
        bool lookupDone_ = false;
    };

    Store() noexcept
        : currentRaw_(&emptySnapshot_) {}

    ~Store() noexcept {
        if (activeReaders_.load(std::memory_order_seq_cst) != 0) {
            std::terminate();
        }
    }

    ReadHandle readHandle() const noexcept {
        return ReadHandle(*this);
    }

    Transition publish(const std::unordered_set<uint32_t>& desired) {
        std::lock_guard<std::mutex> lock(publishMutex_);
        const Snapshot* previous = currentRaw_.load(
            std::memory_order_seq_cst);

        std::vector<uint32_t> desiredIds(desired.begin(), desired.end());
        std::sort(desiredIds.begin(), desiredIds.end());

        std::vector<uint32_t> previousIds;
        previousIds.reserve(previous->records.size());
        for (const auto& record : previous->records) {
            previousIds.push_back(record->appId);
        }

        if (desiredIds == previousIds) {
            Transition transition;
            transition.generation = previous->generation;
            transition.snapshot = currentOwner_ != nullptr
                ? currentOwner_
                : emptySnapshotOwner();
            return transition;
        }

        Transition transition;
        transition.generation = previous->generation + 1;
        transition.added.reserve(desiredIds.size());
        transition.removed.reserve(previousIds.size());
        std::set_difference(desiredIds.begin(), desiredIds.end(),
                            previousIds.begin(), previousIds.end(),
                            std::back_inserter(transition.added));
        std::set_difference(previousIds.begin(), previousIds.end(),
                            desiredIds.begin(), desiredIds.end(),
                            std::back_inserter(transition.removed));

        auto next = std::make_shared<Snapshot>();
        next->generation = transition.generation;
        next->records.reserve(desiredIds.size());

        std::size_t previousIndex = 0;
        for (const uint32_t appId : desiredIds) {
            while (previousIndex < previous->records.size() &&
                   previous->records[previousIndex]->appId < appId) {
                ++previousIndex;
            }

            if (previousIndex < previous->records.size() &&
                previous->records[previousIndex]->appId == appId) {
                next->records.push_back(previous->records[previousIndex]);
                ++previousIndex;
                continue;
            }

            auto record =
                std::make_shared<Record>(appId, transition.generation);
            next->records.push_back(std::move(record));
        }

        // Retain the current owner before changing the raw pointer.  If the
        // vector append throws, currentOwner_ and currentRaw_ still describe
        // the live snapshot and no reader lifetime has been shortened.
        if (currentOwner_ != nullptr) {
            retired_.push_back(currentOwner_);
        }
        currentOwner_ = std::move(next);
        currentRaw_.store(currentOwner_.get(), std::memory_order_seq_cst);
        reclaimRetiredLocked();
        transition.snapshot = currentOwner_;
        return transition;
    }

    bool contains(uint32_t appId) const noexcept {
        auto handle = readHandle();
        return handle.contains(appId);
    }

    bool noteResolved(uint32_t appId) noexcept {
        auto handle = readHandle();
        return handle.contains(appId) && handle.noteResolved();
    }

    bool resolvedDirtyHint() const noexcept {
        return resolvedDirtyGeneration_.load(std::memory_order_acquire) != 0;
    }

    bool takeResolvedDirty() {
        std::lock_guard<std::mutex> lock(publishMutex_);
        const uint64_t dirtyGeneration = resolvedDirtyGeneration_.exchange(
            0, std::memory_order_acq_rel);
        if (dirtyGeneration == 0) {
            return false;
        }

        const Snapshot* current = currentRaw_.load(
            std::memory_order_seq_cst);
        if (current == nullptr) {
            return false;
        }
        for (const auto& record : current->records) {
            if (record->generation == dirtyGeneration &&
                record->resolvedGeneration.load(std::memory_order_acquire) ==
                    dirtyGeneration) {
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<const Snapshot> snapshot() const {
        std::lock_guard<std::mutex> lock(publishMutex_);
        return currentOwner_ != nullptr ? currentOwner_ : emptySnapshotOwner();
    }

private:
    using RecordIterator =
        std::vector<std::shared_ptr<Record>>::const_iterator;

    static Record* findRecord(const Snapshot& snapshot,
                              uint32_t appId) noexcept {
        const auto record = std::lower_bound(
            snapshot.records.begin(), snapshot.records.end(), appId,
            [](const std::shared_ptr<Record>& value, uint32_t target) {
                return value->appId < target;
            });
        return record != snapshot.records.end() && (*record)->appId == appId
            ? record->get()
            : nullptr;
    }

    static std::shared_ptr<const Snapshot> emptySnapshotOwner() {
        static const std::shared_ptr<const Snapshot> owner =
            std::make_shared<const Snapshot>();
        return owner;
    }

    void reclaimRetiredLocked() noexcept {
        if (activeReaders_.load(std::memory_order_seq_cst) == 0) {
            retired_.clear();
        }
    }

    mutable std::mutex publishMutex_;
    Snapshot emptySnapshot_;
    std::atomic<const Snapshot*> currentRaw_;
    mutable std::atomic<uint32_t> activeReaders_{0};
    mutable std::atomic<uint64_t> resolvedDirtyGeneration_{0};
    std::shared_ptr<const Snapshot> currentOwner_;
    std::vector<std::shared_ptr<const Snapshot>> retired_;
};

static_assert(std::atomic<const Snapshot*>::is_always_lock_free,
              "snapshot pointer must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "snapshot reader counter must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "dirty generation must be lock-free");

} // namespace HotReloadState
