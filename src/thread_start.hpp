#pragma once

#include <memory>
#include <thread>
#include <utility>
namespace ThreadStart
{
	struct NoopDetachFailure
	{
		void operator()() const noexcept {}
	};

	struct NoopRecoveryFailure
	{
		void operator()() const noexcept {}
	};

	struct StdThreadDetacher
	{
		void operator()(std::thread& worker) const
		{
			worker.detach();
		}
	};

	struct StdThreadJoiner
	{
		void operator()(std::thread& worker) const
		{
			worker.join();
		}
	};

	template <typename Callback>
	void invokeNoThrow(Callback&& callback) noexcept
	{
		try
		{
			callback();
		}
		catch (...)
		{
			// Recovery callbacks must not escape a worker-start boundary.
		}
	}

	// Start a worker while retaining ownership of its std::thread object until
	// detach succeeds.  If detach fails, signal the worker to stop, join it
	// while the object is still alive, and then restore the caller's state.
	template <typename Worker, typename OnFailure,
	          typename OnDetachFailure = NoopDetachFailure,
	          typename Detacher = StdThreadDetacher,
	          typename Joiner = StdThreadJoiner,
	          typename OnRecoveryFailure = NoopRecoveryFailure>
	bool startDetached(Worker&& worker, OnFailure&& onFailure,
	                   OnDetachFailure&& onDetachFailure = OnDetachFailure{},
	                   Detacher&& detacher = Detacher{},
	                   Joiner&& joiner = Joiner{},
	                   OnRecoveryFailure&& onRecoveryFailure = OnRecoveryFailure{}) noexcept
	{
		std::unique_ptr<std::thread> workerThread;
		try
		{
			workerThread = std::make_unique<std::thread>(std::forward<Worker>(worker));
		}
		catch (...)
		{
			invokeNoThrow(std::forward<OnFailure>(onFailure));
			return false;
		}

		try
		{
			detacher(*workerThread);
			workerThread.release();
			return true;
		}
		catch (...)
		{
			invokeNoThrow(std::forward<OnDetachFailure>(onDetachFailure));

			bool joined = false;
			try
			{
				if (workerThread && workerThread->joinable())
				{
					std::forward<Joiner>(joiner)(*workerThread);
					joined = !workerThread->joinable();
				}
			}
			catch (...)
			{
				// A failed join is recovered below with the same detacher
				// seam.  It is not safe to reset caller state yet.
			}

			if (joined)
			{
				invokeNoThrow(std::forward<OnFailure>(onFailure));
				return false;
			}

			bool detached = false;
			try
			{
				if (workerThread && workerThread->joinable())
				{
					detacher(*workerThread);
					detached = !workerThread->joinable();
				}
			}
			catch (...)
			{
				// The wrapper is released below if it is still joinable so its
				// destructor cannot call std::terminate.
			}

			if (!detached && workerThread && workerThread->joinable())
				workerThread.release();
			invokeNoThrow(std::forward<OnRecoveryFailure>(onRecoveryFailure));
			return false;
		}
	}

	// Run a detached-worker starter without allowing a construction failure to
	// escape into Steam.  The failure callback restores the caller's state so
	// a later attempt can retry and waiters cannot block forever.
	template <typename Starter, typename OnFailure>
	bool tryStart(Starter&& starter, OnFailure&& onFailure) noexcept
	{
		try
		{
			starter();
			return true;
		}
		catch (...)
		{
			try
			{
				onFailure();
			}
			catch (...)
			{
				// State recovery must not rethrow from the worker-start boundary.
			}
			return false;
		}
	}

	// Run the body of a detached worker behind a top-level exception barrier.
	// Cleanup runs after both normal return and failure; each callback is
	// isolated so state recovery cannot rethrow into the thread runtime.
	template <typename Worker, typename OnFailure, typename OnExit>
	void runGuarded(Worker&& worker, OnFailure&& onFailure,
	                OnExit&& onExit) noexcept
	{
		try
		{
			worker();
		}
		catch (...)
		{
			try
			{
				onFailure();
			}
			catch (...)
			{
				// Failure reporting must not prevent state recovery.
			}
		}

		try
		{
			onExit();
		}
		catch (...)
		{
			// Cleanup must not escape the detached worker either.
		}
	}
} // namespace ThreadStart