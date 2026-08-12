// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure helpers for reconciling the package vector used by the hot-reload
// path.  The seeded set is explicit provenance: an id is removable only
// when it was seeded by the plugin and is no longer desired.

#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <set>
#include <unordered_set>
#include <vector>

namespace HotReloadPackage
{
	using IdSet = std::unordered_set<uint32_t>;

	struct Contribution
	{
		uint32_t baseAppId = 0;
		std::vector<uint32_t> appIds;
		std::vector<uint32_t> depotIds;
	};

	struct Aggregate
	{
		IdSet appIds;
		IdSet depotIds;
	};

	namespace detail
	{
		template <typename Range>
		inline bool contains(const Range& values, uint32_t id)
		{
			return std::find(std::begin(values), std::end(values), id) !=
			       std::end(values);
		}

		inline bool containsLive(const uint32_t* data, uint32_t size,
		                        uint32_t id)
		{
			if (!data) return false;
			for (uint32_t index = 0; index < size; ++index)
			{
				if (data[index] == id) return true;
			}
			return false;
		}
	} // namespace detail

	// Union contributions only from bases that are active in the current
	// runtime snapshot.  Set insertion deduplicates shared DLC and depot ids.
	template <typename ActiveBases>
	inline Aggregate aggregate(
		const ActiveBases& activeBases,
		const std::vector<Contribution>& contributions)
	{
		Aggregate out;
		for (const Contribution& contribution : contributions)
		{
			if (!detail::contains(activeBases, contribution.baseAppId)) continue;
			out.appIds.insert(contribution.appIds.begin(),
			                  contribution.appIds.end());
			out.depotIds.insert(contribution.depotIds.begin(),
			                    contribution.depotIds.end());
		}
		return out;
	}

	// Keep the convenient aggregate({1, 2}, contributions) call form while
	// retaining the generic range overload above for vectors and sets.
	inline Aggregate aggregate(
		std::initializer_list<uint32_t> activeBases,
		const std::vector<Contribution>& contributions)
	{
		const IdSet active(activeBases);
		return aggregate(active, contributions);
	}

	// Stable-compacts a live uint32 vector in place.  Non-seeded ids are
	// naturally present and therefore survive regardless of desired state.
	template <typename Seeded, typename Desired>
	inline void compactInjected(uint32_t* data, uint32_t& size,
	                            const Seeded& seeded,
	                            const Desired& desired)
	{
		if (!data || size == 0) return;

		uint32_t writeIndex = 0;
		for (uint32_t readIndex = 0; readIndex < size; ++readIndex)
		{
			const uint32_t id = data[readIndex];
			if (detail::contains(seeded, id) &&
			    !detail::contains(desired, id))
			{
				continue;
			}

			data[writeIndex] = id;
			++writeIndex;
		}
		size = writeIndex;
	}

	// Return the desired ids absent from the live vector.  std::set makes the
	// result deterministic and deduplicates repeated desired inputs.
	template <typename Desired>
	inline std::set<uint32_t> missingFromVector(const uint32_t* data,
	                                             uint32_t size,
	                                             const Desired& desired)
	{
		std::set<uint32_t> missing;
		for (const uint32_t id : desired)
		{
			if (!detail::containsLive(data, size, id)) missing.insert(id);
		}
		return missing;
	}

	// A failed package-vector transaction must not trigger a live appinfo
	// request for ids Steam never accepted. The successful path preserves the
	// deterministic order supplied by missingFromVector.
	template <typename Missing>
	inline std::vector<uint32_t> idsToRequestAfterApply(
		bool applied,
		const Missing& missing)
	{
		if (!applied) return {};
		return std::vector<uint32_t>(std::begin(missing), std::end(missing));
	}
}
