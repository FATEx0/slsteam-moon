// SPDX-License-Identifier: AGPL-3.0-only
//
// Small pure lazy-index primitive used by DepotKey's on-disk catalog.

#pragma once

#include <map>
#include <utility>

namespace DepotKey
{
	template<typename Key, typename Value>
	class LazyIndex
	{
	public:
		template<typename Loader>
		void loadOnce(Loader&& loader)
		{
			if (loaded_) return;
			std::map<Key, Value> loaded;
			std::forward<Loader>(loader)(loaded);
			entries_ = std::move(loaded);
			loaded_ = true;
		}

		Value* find(const Key& key)
		{
			auto it = entries_.find(key);
			return it == entries_.end() ? nullptr : &it->second;
		}

		const Value* find(const Key& key) const
		{
			auto it = entries_.find(key);
			return it == entries_.end() ? nullptr : &it->second;
		}

		void upsert(const Key& key, Value value)
		{
			entries_[key] = std::move(value);
		}

		const std::map<Key, Value>& entries() const noexcept
		{
			return entries_;
		}

	private:
		bool loaded_ = false;
		std::map<Key, Value> entries_;
	};
}
