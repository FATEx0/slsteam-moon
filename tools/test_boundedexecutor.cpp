// Standalone regression test for the manifest-fetch worker pool.
//
// The production bug created one std::async thread per manifest.  A title with
// hundreds of depots therefore exhausted the 32-bit Steam process even when
// every manifest was already cached.  The executor must accept any queue
// length while bounding only simultaneous work.
//
// Build (from repo root):
//   g++ -std=c++20 -pthread tools/test_boundedexecutor.cpp -o /tmp/test_boundedexecutor && /tmp/test_boundedexecutor

#include "../src/utils/boundedexecutor.hpp"
#include "../src/utils/ManifestFetch.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	constexpr std::size_t kWorkers = 4;
	constexpr int kJobs = 2000;

	BoundedExecutor executor(kWorkers);
	std::atomic<int> active{0};
	std::atomic<int> maxActive{0};
	std::atomic<int> completed{0};
	std::mutex doneLock;
	std::condition_variable doneCv;
	bool allAccepted = true;

	for (int i = 0; i < kJobs; ++i)
	{
		allAccepted = executor.submit([&]
		{
			const int now = active.fetch_add(1) + 1;
			int observed = maxActive.load();
			while (now > observed &&
			       !maxActive.compare_exchange_weak(observed, now))
			{
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			active.fetch_sub(1);
			completed.fetch_add(1);
			doneCv.notify_one();
		}) && allAccepted;
	}

	{
		std::unique_lock<std::mutex> lk(doneLock);
		doneCv.wait_for(lk, std::chrono::seconds(10), [&]
		{
			return completed.load() == kJobs;
		});
	}

	CHECK(executor.workerCount() == kWorkers,
	      "executor: starts the configured fixed worker count");
	CHECK(allAccepted, "executor: accepts all jobs without a queue-length cap");
	CHECK(completed.load() == kJobs,
	      "executor: processes every queued manifest job");
	CHECK(maxActive.load() <= static_cast<int>(kWorkers),
	      "executor: never exceeds the configured concurrency");
	CHECK(maxActive.load() > 1,
	      "executor: still runs independent manifest jobs concurrently");

	{
		BoundedExecutor capped(1, 1);
		std::mutex gateLock;
		std::condition_variable gateCv;
		std::atomic<bool> firstStarted{false};
		std::atomic<bool> releaseFirst{false};
		const bool firstAccepted = capped.submit([&]
		{
			firstStarted.store(true, std::memory_order_release);
			gateCv.notify_one();
			while (!releaseFirst.load(std::memory_order_acquire))
				std::this_thread::yield();
		});
		{
			std::unique_lock<std::mutex> lk(gateLock);
			gateCv.wait_for(lk, std::chrono::seconds(1), [&]
			{
				return firstStarted.load(std::memory_order_acquire);
			});
		}
		const bool secondAccepted = capped.submit([] {});
		const bool thirdAccepted = capped.submit([] {});
		CHECK(firstAccepted && secondAccepted && !thirdAccepted,
		      "executor: configured queue cap rejects excess work");
		releaseFirst.store(true, std::memory_order_release);
	}

	ManifestFetch::JobBudget budget(std::chrono::milliseconds(100));
	CHECK(!budget.shouldStop(), "manifest budget starts active");
	budget.cancel();
	CHECK(budget.shouldStop(), "manifest budget cancellation is observable");
	ManifestFetch::JobBudget shortBudget(std::chrono::milliseconds(1));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(shortBudget.shouldStop(), "manifest budget expires deterministically");

	std::mutex waiterLock;
	std::atomic<int> registeredWaiters{0};
	std::thread waiterA([&]
	{
		std::lock_guard<std::mutex> lk(waiterLock);
		ManifestFetch::detail::registerWaiterLocked(registeredWaiters);
	});
	std::thread waiterB([&]
	{
		std::lock_guard<std::mutex> lk(waiterLock);
		ManifestFetch::detail::registerWaiterLocked(registeredWaiters);
	});
	waiterA.join();
	waiterB.join();
	CHECK(registeredWaiters.load(std::memory_order_acquire) == 2,
	      "manifest waiter registration is performed before a joined job is returned");
	CHECK(!ManifestFetch::detail::shouldCancelBlobJob(
	              /*producerActive=*/true, /*lastWaiter=*/true,
	              /*cancelIfLast=*/true),
	      "a timed-out waiter cannot cancel an active background producer");
	CHECK(ManifestFetch::detail::shouldCancelBlobJob(
	              /*producerActive=*/false, /*lastWaiter=*/true,
	              /*cancelIfLast=*/true),
	      "a completed job can be cancelled after its last waiter leaves");
	CHECK(!ManifestFetch::detail::shouldCancelBlobJob(
	              /*producerActive=*/true, /*lastWaiter=*/false,
	              /*cancelIfLast=*/true),
	      "a non-final waiter never cancels a shared job");

	std::mutex generationLock;
	std::atomic<int> generationWaiters{1};
	std::atomic<bool> generationPresent{true};
	std::atomic<bool> generationCancelled{false};
	std::atomic<bool> generationJoined{false};
	std::thread timeoutWaiter([&]
	{
		std::lock_guard<std::mutex> lk(generationLock);
		if (ManifestFetch::detail::releaseWaiterLocked(generationWaiters))
		{
			generationPresent.store(false, std::memory_order_release);
			generationCancelled.store(true, std::memory_order_release);
		}
	});
	std::thread joiningWaiter([&]
	{
		std::lock_guard<std::mutex> lk(generationLock);
		if (generationPresent.load(std::memory_order_acquire))
		{
			ManifestFetch::detail::registerWaiterLocked(generationWaiters);
			generationJoined.store(true, std::memory_order_release);
		}
	});
	timeoutWaiter.join();
	joiningWaiter.join();
	if (generationJoined.load(std::memory_order_acquire))
	{
		CHECK(!generationCancelled.load(std::memory_order_acquire)
		      && generationWaiters.load(std::memory_order_acquire) == 1,
		      "a joined waiter prevents cancellation of its shared job");
	}
	else
	{
		CHECK(generationCancelled.load(std::memory_order_acquire)
		      && generationWaiters.load(std::memory_order_acquire) == 0,
		      "last waiter cancellation removes the job before a later join");
	}

	const pid_t child = fork();
	if (child == 0)
	{
		for (;;) pause();
	}
	CHECK(child > 0, "manifest process cleanup test child starts");
	if (child > 0)
	{
		(void)setpgid(child, child);
		int status = 0;
		ManifestFetch::detail::killAndReap(child, status);
		CHECK(WIFSIGNALED(status),
		      "manifest process cleanup terminates the child");
		errno = 0;
		CHECK(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD,
		      "manifest process cleanup reaps the child");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
