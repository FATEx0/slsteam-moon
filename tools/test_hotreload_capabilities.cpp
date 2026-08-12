#include "../src/feats/hotreload_capabilities.hpp"

#include <iostream>
#include <string_view>

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

HotReloadCapabilities::Matrix complete()
{
	return {
		true, true, true,
		true, true, true, true, true,
	};
}
}

int main()
{
	auto value = complete();
	check(value.canProcessUnresolvedAdd(),
	      "complete matrix enables unresolved live add");
	check(value.canProcessLicenseChange(),
	      "complete matrix enables runtime license processing");
	check(value.canRemoveVisually(),
	      "complete matrix enables live visual removal");

	auto withoutGetOrAdd = value;
	withoutGetOrAdd.unresolvedAppGuard = false;
	check(!withoutGetOrAdd.canProcessUnresolvedAdd(),
	      "missing appinfo guard defers an unsafe unresolved add");
	check(withoutGetOrAdd.canProcessLicenseChange(),
	      "missing appinfo guard does not disable resolved license changes");

	auto withoutUi = value;
	withoutUi.uiRunFrame = false;
	check(withoutUi.canProcessLicenseChange() &&
	      !withoutUi.canRemoveVisually(),
	      "missing UI keeps license refresh while disabling visual removal");

	auto withoutMark = value;
	withoutMark.markLicenseChanged = false;
	check(!withoutMark.canProcessLicenseChange(),
	      "missing license mark function requires restart");
	check(!withoutMark.canProcessUnresolvedAdd(),
	      "missing license mark function also defers unresolved add");

	auto withoutProcess = value;
	withoutProcess.processLicenseUpdates = false;
	check(!withoutProcess.canProcessLicenseChange(),
	      "missing license process function requires restart");

	auto withoutOwnershipLayout = value;
	withoutOwnershipLayout.uiOwnershipLayout = false;
	check(!withoutOwnershipLayout.canRemoveVisually(),
	      "invalid derived ownership layout disables visual removal");
	check(withoutOwnershipLayout.canProcessUnresolvedAdd(),
	      "invalid UI layout does not disable live add");

	return failures == 0 ? 0 : 1;
}
