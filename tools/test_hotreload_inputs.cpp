#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/hotreload_inputs.hpp"
#include "../src/feats/hotreload_publish_policy.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}
}

int main()
{
	using HotReloadInputs::AppInput;

	const std::vector<AppInput> inputs{
		{20, false, {}, {220}},
		{10, true, {1000, 10, 1000}, {999, 110, 999}},
	};
	const auto built = HotReloadInputs::build(7, inputs);
	check(built.valid, "bounded fixture builds successfully");
	check(built.snapshot.generation == 7, "generation is preserved");
	check(built.snapshot.appIds ==
		std::vector<std::uint32_t>({10, 20, 1000}),
		"base and planner app ids are deterministic");
	check(built.snapshot.depotIds ==
		std::vector<std::uint32_t>({110, 220, 999}),
		"depot ids are deterministic");
	check(!built.snapshot.metadataComplete,
		"missing or malformed cache is unresolved");

	const auto empty = HotReloadInputs::build(8, {});
	check(empty.valid && empty.snapshot.metadataComplete &&
		empty.snapshot.appIds.empty() && empty.snapshot.depotIds.empty(),
		"an empty managed source is a complete empty snapshot");

	const auto bounded = HotReloadInputs::build(9, inputs, 2);
	check(!bounded.valid && !bounded.snapshot.metadataComplete &&
		bounded.snapshot.appIds.empty() && bounded.snapshot.depotIds.empty(),
		"oversized input fails closed without a destructive partial snapshot");

	check(HotReloadPublishPolicy::shouldEvaluateInputs(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*forceSourceRefresh=*/true),
		"a forced source event evaluates local inputs");
	check(!HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*fingerprintsChanged=*/false),
		"an unchanged duplicate forced event does not publish");
	check(HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/true, /*membershipChanged=*/false,
		/*fingerprintsChanged=*/false),
		"the initial state always publishes");
	check(HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/false, /*membershipChanged=*/true,
		/*fingerprintsChanged=*/false),
		"a membership transition always publishes");
	check(HotReloadPublishPolicy::shouldPublish(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*fingerprintsChanged=*/true),
		"a key or archived fallback change publishes");
	check(!HotReloadPublishPolicy::shouldEvaluateInputs(
		/*initialPublication=*/false, /*membershipChanged=*/false,
		/*forceSourceRefresh=*/false),
		"an ordinary unchanged event skips fingerprint evaluation");

	const std::vector<ProvisionTerminal::LocalInput> localInputs{
		{330, "", 999},
		{110, "alpha", 999},
		{220, "beta", 999},
	};
	const ManifestStore::ArchivedGidIndex archivedGids{
		{110, 19},
		{220, 3},
	};
	check(AppInfoProvision::fingerprintIndexedLocalInputs(
		localInputs, archivedGids) == "baba8bfd866131f5",
		"indexed gids preserve terminal fingerprint semantics");

	return failures == 0 ? 0 : 1;
}
