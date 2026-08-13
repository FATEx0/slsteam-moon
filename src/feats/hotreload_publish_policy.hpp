// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

namespace HotReloadPublishPolicy
{
inline constexpr bool shouldEvaluateInputs(
	bool initialPublication,
	bool membershipChanged,
	bool forceSourceRefresh) noexcept
{
	return initialPublication || membershipChanged || forceSourceRefresh;
}

inline constexpr bool shouldPublish(
	bool initialPublication,
	bool membershipChanged,
	bool fingerprintsChanged) noexcept
{
	return initialPublication || membershipChanged || fingerprintsChanged;
}
} // namespace HotReloadPublishPolicy
