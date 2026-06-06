// Standalone test for the pure CM-list cache freshness logic
// (src/feats/cmlist_cache.hpp).
//
// The CM server list HTTP fetch is ~400ms on the startup path (measured
// on the Zorin VM).  Caching the list to disk and reusing it within a TTL
// removes that round-trip from subsequent cold boots.  This pins down the
// PURE decision; the read/write/parse lives in cmclient.cpp.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_cmlist_cache.cpp -o /tmp/t && /tmp/t

#include "../src/feats/cmlist_cache.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

int main()
{
	using CmClient::cache::isCmListReusable;

	const long long ttl = 24 * 3600; // 1 day
	const long long now = 2'000'000;

	CHECK(!isCmListReusable(false, now - 1, now, ttl),
	      "missing list is not reusable");
	CHECK(isCmListReusable(true, now, now, ttl),
	      "list written now is reusable");
	CHECK(isCmListReusable(true, now - (ttl - 1), now, ttl),
	      "list aged just under TTL is reusable");
	CHECK(!isCmListReusable(true, now - ttl, now, ttl),
	      "list aged exactly TTL is not reusable");
	CHECK(!isCmListReusable(true, now - (ttl + 1), now, ttl),
	      "list aged past TTL is not reusable");
	CHECK(!isCmListReusable(true, now, now, 0),
	      "ttl=0 disables reuse");
	CHECK(!isCmListReusable(true, now, now, -1),
	      "negative ttl disables reuse");
	CHECK(!isCmListReusable(true, now + 10, now, ttl),
	      "future mtime is not trusted");

	if (g_failures == 0) std::printf("\nall cmlist-cache checks passed\n");
	else                 std::printf("\n%d cmlist-cache check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
