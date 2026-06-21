// Standalone test for the pure AppInfoPin gid-rewrite logic
// (manifest-pin-NEXT-STEPS.md §3, Approach A).
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_appinfopin.cpp \
//       lib/libyaml-cpp.a -o /tmp/test_appinfopin && /tmp/test_appinfopin
#include "../src/feats/appinfopin.hpp"

#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <string>
#include <unordered_map>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

// Build a minimal SteamCMD-shaped appinfo body with two depots, each with a
// public manifest gid.  Mirrors the depots.<id>.manifests.public.gid shape.
static YAML::Node makeBody()
{
	YAML::Node body(YAML::NodeType::Map);
	body["common"]["name"] = "Test Game";

	YAML::Node depots(YAML::NodeType::Map);

	YAML::Node d1(YAML::NodeType::Map);
	d1["manifests"]["public"]["gid"]      = std::string("6330832861176696160");
	d1["manifests"]["public"]["size"]     = std::string("1497653");
	d1["manifests"]["public"]["download"] = std::string("9000000000");
	depots["3357651"] = d1;

	YAML::Node d2(YAML::NodeType::Map);
	d2["manifests"]["public"]["gid"]  = std::string("4731286747379700304");
	d2["manifests"]["public"]["size"] = std::string("4096");
	depots["3859920"] = d2;

	// branches.public.buildid — what GetAppBuildId reads.
	depots["branches"]["public"]["buildid"]      = std::string("23443442");
	depots["branches"]["public"]["timeupdated"]  = std::string("1700000000");
	depots["branches"]["beta"]["buildid"]        = std::string("99999999");

	body["depots"] = depots;
	return body;
}

static std::string gidOf(const YAML::Node& body, const std::string& depot)
{
	return body["depots"][depot]["manifests"]["public"]["gid"].as<std::string>();
}

int main()
{
	using namespace AppInfoPin;

	// 1) Rewrites only the depot whose gid differs from its pin.
	{
		YAML::Node body = makeBody();
		std::unordered_map<uint32_t, uint64_t> pins;
		pins[3357651] = 2417499809052404547ULL;  // differs -> rewrite
		pins[3859920] = 4731286747379700304ULL;  // already matches -> skip

		const int n = applyDepotGidPins(body, pins);
		CHECK(n == 1, "rewrites exactly the one differing depot");
		CHECK(gidOf(body, "3357651") == "2417499809052404547",
		      "differing depot gets pinned gid");
		CHECK(gidOf(body, "3859920") == "4731286747379700304",
		      "matching depot left unchanged");
	}

	// 2) Sibling fields (size/download) are preserved when only gid changes.
	{
		YAML::Node body = makeBody();
		std::unordered_map<uint32_t, uint64_t> pins;
		pins[3357651] = 2417499809052404547ULL;
		applyDepotGidPins(body, pins);
		const auto pub = body["depots"]["3357651"]["manifests"]["public"];
		CHECK(pub["size"].as<std::string>() == "1497653", "size preserved");
		CHECK(pub["download"].as<std::string>() == "9000000000",
		      "download preserved");
	}

	// 3) A pin for a depot absent from appinfo is ignored (no crash, no add).
	{
		YAML::Node body = makeBody();
		std::unordered_map<uint32_t, uint64_t> pins;
		pins[9999999] = 1234567890123ULL;
		const int n = applyDepotGidPins(body, pins);
		CHECK(n == 0, "unknown depot pin is a no-op");
		CHECK(!body["depots"]["9999999"], "unknown depot not added");
	}

	// 4) Empty pins / non-map body are safe no-ops.
	{
		YAML::Node body = makeBody();
		CHECK(applyDepotGidPins(body, {}) == 0, "empty pins -> 0");

		YAML::Node scalar = YAML::Load("just-a-scalar");
		std::unordered_map<uint32_t, uint64_t> pins;
		pins[3357651] = 1ULL;
		CHECK(applyDepotGidPins(scalar, pins) == 0, "non-map body -> 0");
	}

	// 5) A depot present but missing the manifests/public node is skipped.
	{
		YAML::Node body(YAML::NodeType::Map);
		body["depots"]["3357651"]["dlcappid"] = std::string("3357652");
		std::unordered_map<uint32_t, uint64_t> pins;
		pins[3357651] = 2417499809052404547ULL;
		const int n = applyDepotGidPins(body, pins);
		CHECK(n == 0, "depot without manifests/public is skipped");
	}

	// 6) applyBranchBuildId: rewrites the public branch buildid only.
	{
		YAML::Node body = makeBody();
		const bool ok = applyBranchBuildId(body, 22357085u);
		CHECK(ok, "buildid rewrite reports success");
		CHECK(body["depots"]["branches"]["public"]["buildid"].as<std::string>()
		      == "22357085", "public branch buildid set to pin");
		CHECK(body["depots"]["branches"]["beta"]["buildid"].as<std::string>()
		      == "99999999", "non-public branch buildid left unchanged");
		CHECK(body["depots"]["branches"]["public"]["timeupdated"].as<std::string>()
		      == "1700000000", "sibling timeupdated preserved");
	}

	// 7) applyBranchBuildId: buildId 0 is a no-op; missing branches is safe.
	{
		YAML::Node body = makeBody();
		CHECK(!applyBranchBuildId(body, 0u), "buildId 0 -> no-op false");
		CHECK(body["depots"]["branches"]["public"]["buildid"].as<std::string>()
		      == "23443442", "buildId 0 leaves buildid untouched");

		YAML::Node empty(YAML::NodeType::Map);
		CHECK(!applyBranchBuildId(empty, 22357085u),
		      "no depots/branches -> false, no crash");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
