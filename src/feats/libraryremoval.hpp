// SPDX-License-Identifier: AGPL-3.0-only
//
// Optional Steam-UI bridge for live library presentation removal.
#pragma once

#include <cstdint>

namespace LibraryRemoval
{
	// Install the complete Steam-UI capability set. Partial installs are rolled
	// back and return false without affecting package/license hot reload.
	bool setup() noexcept;

	// Close watcher input before restoring every installed UI detour.
	void remove() noexcept;

	bool ready() noexcept;

	// Thread-safe watcher/owner entry points. Steam-owned state is touched only
	// later by the SteamUI RunFrame hook.
	void queue(std::uint32_t appId) noexcept;
	void cancel(std::uint32_t appId) noexcept;
	// Called after the re-added package generation has completed its license
	// refresh; compensates a removal that was already in flight.
	void restore(std::uint32_t appId) noexcept;
	void requestFullReassert() noexcept;
}
