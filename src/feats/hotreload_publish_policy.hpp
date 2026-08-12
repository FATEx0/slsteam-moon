// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

namespace HotReloadPublishPolicy
{
inline constexpr bool shouldPublish(
	bool membershipChanged,
	bool forceSourceRefresh) noexcept
{
	return membershipChanged || forceSourceRefresh;
}
} // namespace HotReloadPublishPolicy
