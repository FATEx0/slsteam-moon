#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace SteamStub
{
	// Claim a new prewarm generation while serialising the gate reset with
	// finishWarmup().  Without the shared mutex, an exiting failed worker can
	// set done=true after a retry has already reset it to false.
	inline bool tryBeginWarmup(std::mutex& warmupMu,
	                           std::atomic<bool>& started,
	                           std::atomic<bool>& done,
	                           std::atomic<bool>& failed,
	                           std::uint64_t& generation)
	{
		std::lock_guard<std::mutex> lk(warmupMu);
		bool expected = false;
		if (!started.compare_exchange_strong(expected, true,
		                                     std::memory_order_acq_rel))
		{
			return false;
		}
		++generation;
		done.store(false, std::memory_order_release);
		failed.store(false, std::memory_order_release);
		return true;
	}

	// Compatibility overload for small state-machine tests that do not need
	// to observe generation identity.
	inline bool tryBeginWarmup(std::mutex& warmupMu,
	                           std::atomic<bool>& started,
	                           std::atomic<bool>& done,
	                           std::atomic<bool>& failed)
	{
		std::uint64_t generation = 0;
		return tryBeginWarmup(warmupMu, started, done, failed, generation);
	}

	// Record a normal helper exit as a failed generation.  Exceptions use the
	// runGuarded failure callback, while non-zero exit codes arrive through the
	// ordinary body return path and must release the retry gate as well.
	inline void recordWarmupResult(std::atomic<bool>& failed, int exitCode) noexcept
	{
		if (exitCode != 0)
			failed.store(true, std::memory_order_release);
	}

	// A failed generation keeps failed=true after notifying waiters.  The
	// start gate is released only after the old worker has finished, and
	// tryBeginWarmup() clears the failure state when it claims the retry.
	inline void finishWarmup(std::mutex& warmupMu,
	                         std::atomic<bool>& started,
	                         std::atomic<bool>& done,
	                         std::atomic<bool>& failed)
	{
		std::lock_guard<std::mutex> lk(warmupMu);
		if (failed.load(std::memory_order_acquire))
			started.store(false, std::memory_order_release);
		done.store(true, std::memory_order_release);
	}

	inline bool shouldRetryWarmup(bool started, bool done, bool failed) noexcept
	{
		return !started && done && failed;
	}
} // namespace SteamStub
