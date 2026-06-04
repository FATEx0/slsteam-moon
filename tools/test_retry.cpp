// Standalone test for AppInfoProvision::retryWithBackoff.
//
// Bug #2: AppInfoProvision's steamcmd.net GET has no retry.  A single
// transient timeout (observed on Outlast 238320: the larger 8-depot JSON
// timed out on the first setup() pass while the smaller 2262770 succeeded
// in the same pass) leaves the app unprovisioned -> install breaks.  The
// only thing that "saved" it was Steam happening to re-exec setup().
//
// retryWithBackoff(op, maxAttempts, baseDelayMs, sleepFn) must:
//   - return true as soon as op() returns true, doing no more attempts
//   - call op() up to maxAttempts times if it keeps failing, then false
//   - sleep with growing backoff *between* attempts only (never after the
//     last attempt, never before the first)
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_retry.cpp -o /tmp/test_retry && /tmp/test_retry

#include "../src/feats/retry.hpp"

#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	// 1) Succeeds on the first try -> exactly one attempt, no sleeps.
	{
		int attempts = 0;
		std::vector<int> sleeps;
		const bool ok = AppInfoProvision::retryWithBackoff(
			[&] { ++attempts; return true; },
			/*maxAttempts=*/3, /*baseDelayMs=*/100,
			[&](int ms) { sleeps.push_back(ms); });
		CHECK(ok, "first-try success returns true");
		CHECK(attempts == 1, "first-try success makes exactly 1 attempt");
		CHECK(sleeps.empty(), "first-try success never sleeps");
	}

	// 2) Fails twice then succeeds -> 3 attempts, 2 backoff sleeps that grow.
	{
		int attempts = 0;
		std::vector<int> sleeps;
		const bool ok = AppInfoProvision::retryWithBackoff(
			[&] { ++attempts; return attempts >= 3; },
			/*maxAttempts=*/5, /*baseDelayMs=*/100,
			[&](int ms) { sleeps.push_back(ms); });
		CHECK(ok, "recovers after transient failures");
		CHECK(attempts == 3, "stops attempting once it succeeds");
		CHECK(sleeps.size() == 2, "sleeps only between the 3 attempts");
		CHECK(sleeps.size() == 2 && sleeps[0] == 100 && sleeps[1] == 200,
		      "backoff grows (100ms, 200ms)");
	}

	// 3) Always fails -> exactly maxAttempts attempts, false, no trailing sleep.
	{
		int attempts = 0;
		std::vector<int> sleeps;
		const bool ok = AppInfoProvision::retryWithBackoff(
			[&] { ++attempts; return false; },
			/*maxAttempts=*/3, /*baseDelayMs=*/100,
			[&](int ms) { sleeps.push_back(ms); });
		CHECK(!ok, "all-fail returns false");
		CHECK(attempts == 3, "all-fail makes exactly maxAttempts attempts");
		CHECK(sleeps.size() == 2, "no sleep after the final failed attempt");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
