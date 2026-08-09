// TDD regression test for retrying a failed SteamStub warmup.

#include "feats/steamstub_warmup.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

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
	std::ifstream steamstubSource("src/feats/steamstub.cpp");
	const std::string steamstubText(
		(std::istreambuf_iterator<char>(steamstubSource)),
		std::istreambuf_iterator<char>());
	const auto victim = steamstubText.find("if (!fileHasStubMarker(pathStr))");
	const auto retry = steamstubText.find("warmupAsync();", victim);
	const auto wait = steamstubText.find(
		"std::unique_lock<std::mutex> lk(g_warmupMu);", victim);
	const auto retryAfterWait = steamstubText.find("warmupAsync();", wait);
	const auto generationState = steamstubText.find("g_warmupGeneration", victim);
	const auto generationTransition = steamstubText.find(
		"g_warmupGeneration != observedGeneration", wait);
	check(steamstubSource.is_open() && victim != std::string::npos &&
	      retry != std::string::npos && wait != std::string::npos && retry < wait &&
	      retryAfterWait != std::string::npos &&
	      generationState != std::string::npos &&
	      generationTransition != std::string::npos,
	      "real SteamStub victims retry and wait by generation");

	std::atomic<bool> started{false};
	std::atomic<bool> done{false};
	std::atomic<bool> failed{false};
	std::mutex stateMutex;
	auto tryStart = [&]
	{
		return SteamStub::tryBeginWarmup(stateMutex, started, done, failed);
	};

	check(tryStart(), "first warmup claims the start gate");
	check(!tryStart(), "duplicate warmup is suppressed while running");

	failed.store(true, std::memory_order_release);
	check(!tryStart(), "failed warmup cannot be retried before worker exit");
	SteamStub::finishWarmup(stateMutex, started, done, failed);
	check(done.load(std::memory_order_acquire),
	      "failed warmup releases launch waiters");
	check(!started.load(std::memory_order_acquire),
	      "failed warmup releases the start gate after exit");
	check(SteamStub::shouldRetryWarmup(
		      started.load(std::memory_order_acquire),
		      done.load(std::memory_order_acquire),
		      failed.load(std::memory_order_acquire)),
	      "a waiter can identify a completed failed generation for retry");

	done.store(false, std::memory_order_release);
	check(tryStart(), "failed warmup can be retried after worker exit");
	SteamStub::finishWarmup(stateMutex, started, done, failed);
	check(done.load(std::memory_order_acquire),
	      "successful retry completes normally");
	check(started.load(std::memory_order_acquire),
	      "successful warmup remains one-shot");
	check(!tryStart(), "successful warmup is not retried");

	std::atomic<bool> numberedStarted{false};
	std::atomic<bool> numberedDone{false};
	std::atomic<bool> numberedFailed{false};
	std::mutex numberedMutex;
	std::uint64_t generation = 0;
	check(SteamStub::tryBeginWarmup(
			numberedMutex, numberedStarted, numberedDone, numberedFailed, generation),
	      "generation-aware warmup claims the first generation");
	check(generation == 1, "first warmup generation is numbered one");
	SteamStub::recordWarmupResult(numberedFailed, /*exitCode=*/9);
	SteamStub::finishWarmup(numberedMutex, numberedStarted, numberedDone,
	                        numberedFailed);
	check(SteamStub::tryBeginWarmup(
			numberedMutex, numberedStarted, numberedDone, numberedFailed, generation),
	      "generation-aware warmup claims the replacement generation");
	check(generation == 2, "replacement warmup gets a distinct generation");
	SteamStub::finishWarmup(numberedMutex, numberedStarted, numberedDone,
	                        numberedFailed);

	std::atomic<bool> exitStarted{false};
	std::atomic<bool> exitDone{false};
	std::atomic<bool> exitFailed{false};
	std::mutex exitMutex;
	check(SteamStub::tryBeginWarmup(exitMutex, exitStarted, exitDone, exitFailed),
	      "normal-exit generation claims the start gate");
	SteamStub::recordWarmupResult(exitFailed, /*exitCode=*/9);
	SteamStub::finishWarmup(exitMutex, exitStarted, exitDone, exitFailed);
	check(exitDone.load(std::memory_order_acquire),
	      "non-zero prewarm exit wakes launch waiters");
	check(!exitStarted.load(std::memory_order_acquire),
	      "non-zero prewarm exit releases the retry gate");
	check(SteamStub::tryBeginWarmup(exitMutex, exitStarted, exitDone, exitFailed),
	      "a normal-exit failure can claim a later retry");
	check(!SteamStub::tryBeginWarmup(exitMutex, exitStarted, exitDone, exitFailed),
	      "the retry generation still suppresses duplicate starts");
	SteamStub::finishWarmup(exitMutex, exitStarted, exitDone, exitFailed);

	std::mutex generationMutex;
	std::atomic<bool> generationStarted{true};
	std::atomic<bool> generationDone{false};
	std::atomic<bool> generationFailed{true};
	for (int i = 0; i < 1000; ++i)
	{
		generationStarted.store(true, std::memory_order_release);
		generationDone.store(false, std::memory_order_release);
		generationFailed.store(true, std::memory_order_release);
		std::atomic<bool> retried{false};
		std::thread oldExit([&]
		{
			SteamStub::finishWarmup(generationMutex,
			                        generationStarted,
			                        generationDone,
			                        generationFailed);
		});
		std::thread retry([&]
		{
			retried.store(
				SteamStub::tryBeginWarmup(generationMutex,
				                           generationStarted,
				                           generationDone,
				                           generationFailed),
				std::memory_order_release);
		});
		oldExit.join();
		retry.join();
		if (retried.load(std::memory_order_acquire))
		{
			check(generationStarted.load(std::memory_order_acquire)
			      && !generationDone.load(std::memory_order_acquire),
			      "warmup retry cannot inherit stale completion from the prior generation");
		}
		else
		{
			check(!generationStarted.load(std::memory_order_acquire)
			      && generationDone.load(std::memory_order_acquire),
			      "warmup exit completes before a retry can claim the gate");
		}
	}

	if (failures != 0) {
		std::fprintf(stderr, "test_steamstub_warmup: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("steamstub warmup tests passed");
	return 0;
}
