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

	check(!HotReloadPublishPolicy::shouldPublish(false, false),
		"an unchanged membership event may coalesce normally");
	check(HotReloadPublishPolicy::shouldPublish(false, true),
		"a source-write event forces a rebuilt package snapshot");
	check(HotReloadPublishPolicy::shouldPublish(true, false),
		"a membership transition always publishes");

	return failures == 0 ? 0 : 1;
}
