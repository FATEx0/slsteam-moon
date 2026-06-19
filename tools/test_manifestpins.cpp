// Standalone test for the pure ManifestPins container logic (design §3/§4).
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_manifestpins.cpp -o /tmp/test_manifestpins && /tmp/test_manifestpins
#include "../src/feats/manifestpins.hpp"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using namespace ManifestPins;

	// Fixture: app 1054490 locked with two base depots; app 285900 a lone
	// DLC override (not locked).
	PinMap pins;
	pins[1054490].locked = true;
	pins[1054490].depots[1054491] = 4091695229428697509ULL;
	pins[1054490].depots[1054492] = 6006800166866532891ULL;
	pins[285900].locked = false;
	pins[285900].depots[285904] = 123456789012345ULL;

	// 1) flatten: every depot present, gids intact (uint64 precision).
	{
		auto flat = flattenDepots(pins);
		CHECK(flat.size() == 3, "flatten: 3 depots across 2 apps");
		CHECK(flat[1054491] == 4091695229428697509ULL, "flatten: gid1 exact");
		CHECK(flat[1054492] == 6006800166866532891ULL, "flatten: gid2 exact");
		CHECK(flat[285904] == 123456789012345ULL, "flatten: dlc gid exact");
	}

	// 2) lockedAppSet: only the locked app.
	{
		auto locked = lockedAppSet(pins);
		CHECK(locked.count(1054490) == 1, "locked: includes locked app");
		CHECK(locked.count(285900) == 0, "locked: excludes unlocked app");
		CHECK(locked.size() == 1, "locked: exactly one");
	}

	// 3) getPin: hit returns gid, miss returns 0.
	{
		auto flat = flattenDepots(pins);
		CHECK(getPin(flat, 1054491) == 4091695229428697509ULL, "getPin: hit");
		CHECK(getPin(flat, 999999) == 0, "getPin: miss -> 0");
	}

	// 4) isLocked: hit / miss.
	{
		auto locked = lockedAppSet(pins);
		CHECK(isLocked(locked, 1054490), "isLocked: locked app true");
		CHECK(!isLocked(locked, 285900), "isLocked: unlocked app false");
		CHECK(!isLocked(locked, 1), "isLocked: unknown app false");
	}

	// 5) purgeApps: drops only listed apps.
	{
		PinMap p = pins;
		purgeApps(p, {1054490});
		CHECK(p.count(1054490) == 0, "purgeApps: target removed");
		CHECK(p.count(285900) == 1, "purgeApps: other kept");
		CHECK(flattenDepots(p).size() == 1, "purgeApps: index shrinks");
	}

	// 6) purgeOrphans: keeps only apps still present in AdditionalApps.
	{
		PinMap p = pins;
		purgeOrphans(p, {285900});   // 1054490 no longer an AdditionalApp
		CHECK(p.count(1054490) == 0, "purgeOrphans: orphan removed");
		CHECK(p.count(285900) == 1, "purgeOrphans: live app kept");
	}

	// 7) empty inputs: no crash, empty results.
	{
		PinMap empty;
		CHECK(flattenDepots(empty).empty(), "empty: flatten empty");
		CHECK(lockedAppSet(empty).empty(), "empty: locked empty");
		purgeOrphans(empty, {1});      // no-op
		purgeApps(empty, {1});         // no-op
		CHECK(empty.empty(), "empty: purges are no-ops");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
