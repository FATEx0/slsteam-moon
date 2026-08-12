// SPDX-License-Identifier: AGPL-3.0-only
//
// Watcher-side coordinator for live managed-library state.
#pragma once

#include "hotreload_state.hpp"

#include <cstdint>
#include <unordered_set>

namespace HotReload
{
	// Bind the installed appinfo guard to the coordinator's process-lifetime
	// store and publish the first managed-source snapshot.
	void initialize() noexcept;

	// Publish the complete stplug-in/luaappids source union. Equivalent source
	// sets are a no-op; legacy compatibility ids never enter this API.
	void publish(
		const std::unordered_set<std::uint32_t>& managedAppIds,
		bool forceSourceRefresh = false) noexcept;

	// Process-lifetime lock-free membership store used by GetOrAddAppData.
	HotReloadState::Store& store() noexcept;

	// Stop accepting watcher publications before Steam hooks are removed.
	void shutdown() noexcept;
}
