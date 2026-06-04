// Integration-ish timing test: drives retryWithBackoff with a REAL sleep
// and a flaky op to confirm the wall-clock backoff matches the contract
// (no sleep before first / after last; linear growth between attempts).
//
// Build: g++ -std=c++20 -I include tools/test_retry_timing.cpp -o /tmp/test_retry_timing && /tmp/test_retry_timing

#include "../src/feats/retry.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
	using clk = std::chrono::steady_clock;

	// Fails twice then succeeds; base 50ms => expected sleeps 50 + 100 = 150ms.
	int attempts = 0;
	const auto t0 = clk::now();
	const bool ok = AppInfoProvision::retryWithBackoff(
		[&] { ++attempts; return attempts >= 3; },
		/*maxAttempts=*/5, /*baseDelayMs=*/50,
		[](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
	const auto elapsedMs =
		std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();

	std::printf("ok=%d attempts=%d elapsed=%lldms (expect >=150, <300)\n",
	            ok, attempts, static_cast<long long>(elapsedMs));

	const bool pass = ok && attempts == 3 && elapsedMs >= 150 && elapsedMs < 300;
	std::printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
