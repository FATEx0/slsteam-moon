#pragma once

// Pure, side-effect-free helper for reading the user's default Steam Play
// compatibility tool out of a `config/config.vdf` text body.
//
// Steam stores the global "Default compatibility tool" (the dropdown under
// Settings -> Compatibility) as a CompatToolMapping entry keyed by the
// special app-id "0":
//
//     "CompatToolMapping"
//     {
//         "0"
//         {
//             "name"      "proton-cachyos"
//             "config"    ""
//             "priority"  "75"
//         }
//         ...
//     }
//
// When we register a windows-only AddedApp for Proton we want to honour that
// user choice instead of hard-coding Proton Experimental.  This header holds
// the text parsing so it can be unit-tested in isolation (see
// tools/test_compattool.cpp); the config.vdf read/write lives in
// appinfo_provision.cpp.

#include <cstddef>
#include <string>

namespace CompatTool
{
	// Find the matching close brace for the block whose opening '{' is at
	// `open`.  Returns the index of the closing '}', or std::string::npos if
	// unbalanced.
	inline std::size_t matchBrace(const std::string& s, std::size_t open)
	{
		if (open >= s.size() || s[open] != '{') return std::string::npos;
		int depth = 0;
		for (std::size_t i = open; i < s.size(); ++i)
		{
			if (s[i] == '{') ++depth;
			else if (s[i] == '}')
			{
				if (--depth == 0) return i;
			}
		}
		return std::string::npos;
	}

	// Read the quoted value that follows the `"name"` key inside the object
	// bracketed by [open, close].  Empty if absent.
	inline std::string readName(const std::string& s, std::size_t open, std::size_t close)
	{
		std::size_t namePos = s.find("\"name\"", open);
		if (namePos == std::string::npos || namePos >= close) return {};
		std::size_t q1 = s.find('"', namePos + 6); // opening quote of the value
		if (q1 == std::string::npos || q1 >= close) return {};
		std::size_t q2 = s.find('"', q1 + 1);
		if (q2 == std::string::npos || q2 >= close) return {};
		return s.substr(q1 + 1, q2 - q1 - 1);
	}

	// Return the user's default compatibility-tool name from a config.vdf
	// body, or "" if Steam Play has no default tool set (or the file shape is
	// unexpected).  Robust against partial/malformed input.
	inline std::string parseDefaultTool(const std::string& content)
	{
		const std::size_t mapPos = content.find("\"CompatToolMapping\"");
		if (mapPos == std::string::npos) return {};
		const std::size_t mapBrace = content.find('{', mapPos);
		if (mapBrace == std::string::npos) return {};
		const std::size_t mapEnd = matchBrace(content, mapBrace);
		if (mapEnd == std::string::npos) return {};

		// Locate the standalone "0" key (the global default) within the block.
		static const std::string zeroKey = "\"0\"";
		std::size_t k = mapBrace;
		while ((k = content.find(zeroKey, k)) != std::string::npos && k < mapEnd)
		{
			// Must be a key: the next non-whitespace char is its object '{'.
			std::size_t b = content.find_first_not_of(" \t\r\n", k + zeroKey.size());
			if (b != std::string::npos && b < mapEnd && content[b] == '{')
			{
				std::size_t objEnd = matchBrace(content, b);
				if (objEnd == std::string::npos) objEnd = mapEnd;
				return readName(content, b, objEnd);
			}
			k += zeroKey.size();
		}
		return {};
	}
}
