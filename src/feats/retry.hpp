// SPDX-License-Identifier: AGPL-3.0-only
//
// Tiny retry-with-backoff helper for transient network operations.
//
// Motivation: AppInfoProvision's single steamcmd.net GET has no retry, so
// one transient timeout (e.g. a larger multi-depot JSON timing out on a
// cold-network first setup() pass) leaves an AddedApp unprovisioned and
// its install broken.  Wrapping the fetch in a bounded backoff retry
// removes the dependency on Steam happening to re-exec setup().
//
// Pure and dependency-free so it can be unit-tested standalone (see
// tools/test_retry.cpp).  The sleep is injected so tests don't wait.

#pragma once

#include <functional>

namespace AppInfoProvision
{

// Run `op` until it returns true or `maxAttempts` is reached.
//
// - Returns true on the first successful attempt (no further attempts).
// - Sleeps ONLY between attempts (never before the first, never after the
//   last), via `sleepMs`, with linear backoff: baseDelayMs, 2*baseDelayMs,
//   3*baseDelayMs, ...  Linear (not exponential) keeps the worst-case
//   startup wait bounded and predictable.
// - Returns false if every attempt failed.
//
// `maxAttempts < 1` is treated as 1.
inline bool retryWithBackoff(const std::function<bool()>& op,
                             int maxAttempts,
                             int baseDelayMs,
                             const std::function<void(int)>& sleepMs)
{
	if (maxAttempts < 1) maxAttempts = 1;
	for (int attempt = 1; attempt <= maxAttempts; ++attempt)
	{
		if (op())
		{
			return true;
		}
		if (attempt < maxAttempts)
		{
			sleepMs(baseDelayMs * attempt);
		}
	}
	return false;
}

} // namespace AppInfoProvision
