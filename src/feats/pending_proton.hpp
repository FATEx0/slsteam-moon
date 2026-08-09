// SPDX-License-Identifier: AGPL-3.0-only
//
// Strict parsing for the deferred Proton mapping cache record.

#pragma once

#include <charconv>
#include <cstdint>
#include <set>
#include <string_view>
#include <system_error>

namespace AppInfoProvision
{

enum class PendingProtonParseStatus
{
	Valid,
	Invalid,
};

struct PendingProtonParseResult
{
	PendingProtonParseStatus status = PendingProtonParseStatus::Invalid;
	std::set<uint32_t> ids;
};

// Parse one complete proton-mappings.pending record. The file format is one
// non-zero uint32 appid per whitespace-delimited token. A malformed token
// invalidates the entire record so callers never replace a valid file with a
// partial interpretation of corrupt or truncated input.
inline PendingProtonParseResult parsePendingProtonText(std::string_view text)
{
	PendingProtonParseResult result;
	std::size_t pos = 0;
	while (pos < text.size())
	{
		while (pos < text.size() &&
		       (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
	        text[pos] == '\r' || text[pos] == '\f' || text[pos] == '\v'))
		{
			++pos;
		}
		if (pos == text.size()) break;

		const std::size_t tokenStart = pos;
		while (pos < text.size() &&
		       text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n' &&
		       text[pos] != '\r' && text[pos] != '\f' && text[pos] != '\v')
		{
			++pos;
		}

		uint32_t id = 0;
		const auto first = text.data() + tokenStart;
		const auto last = text.data() + pos;
		const auto parsed = std::from_chars(first, last, id, 10);
		if (parsed.ec != std::errc{} || parsed.ptr != last || id == 0)
		{
			result.ids.clear();
			return result;
		}
		result.ids.insert(id);
	}

	result.status = PendingProtonParseStatus::Valid;
	return result;
}

} // namespace AppInfoProvision
