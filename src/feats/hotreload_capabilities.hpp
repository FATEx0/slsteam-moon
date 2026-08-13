// SPDX-License-Identifier: AGPL-3.0-only
//
// Independent capability gates for restart-less library transitions.
#pragma once

namespace HotReloadCapabilities
{
struct Matrix
{
	bool markLicenseChanged = false;
	bool processLicenseUpdates = false;
	bool unresolvedAppGuard = false;

	bool uiRunFrame = false;
	bool uiGetAppById = false;
	bool uiMarkAppChange = false;
	bool uiOwnershipLayout = false;

	constexpr bool canProcessLicenseChange() const noexcept
	{
		return markLicenseChanged && processLicenseUpdates;
	}

	constexpr bool canProcessUnresolvedAdd() const noexcept
	{
		return canProcessLicenseChange() && unresolvedAppGuard;
	}

	constexpr bool canRemoveVisually() const noexcept
	{
		return uiRunFrame && uiGetAppById && uiMarkAppChange &&
			uiOwnershipLayout;
	}
};
} // namespace HotReloadCapabilities
