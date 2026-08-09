// SPDX-License-Identifier: AGPL-3.0-only
//
// Synchronization contract for a provisioning pass. Network/provider work
// must run outside the pass mutex; only state snapshots and commits use it.

#pragma once

#include <mutex>
#include <utility>

namespace AppInfoProvision
{

class ProvisionPassCoordinator
{
public:
	explicit ProvisionPassCoordinator(std::mutex& mutex) : mutex_(mutex) {}

	template <typename Fn>
	decltype(auto) snapshot(Fn&& fn)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return std::forward<Fn>(fn)();
	}

	template <typename Fn>
	decltype(auto) commit(Fn&& fn)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return std::forward<Fn>(fn)();
	}

	template <typename Fn>
	decltype(auto) network(Fn&& fn)
	{
		return std::forward<Fn>(fn)();
	}

private:
	std::mutex& mutex_;
};

} // namespace AppInfoProvision
