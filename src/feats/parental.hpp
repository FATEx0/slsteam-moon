// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Parental
{

inline bool looksLikeSettingsReceiver(const uint8_t* code, std::size_t len)
{
	static constexpr int expected[] = {
		0x55, 0x89, 0xe5, 0x57, 0x56,
		0xe8,   -1,   -1,   -1,   -1,
		0x81, 0xc6,   -1,   -1,   -1,   -1,
		0x53, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00,
		0x8b, 0x45, 0x08, 0x8b, 0x55, 0x1c,
		0x8b, 0x7d, 0x0c,
	};
	constexpr std::size_t expectedLen = sizeof(expected) / sizeof(expected[0]);
	if (!code || len < expectedLen) return false;
	for (std::size_t i = 0; i < expectedLen; ++i)
	{
		if (expected[i] >= 0 && code[i] != expected[i]) return false;
	}
	return true;
}

namespace Wire
{

inline bool readVarint(const uint8_t* data, std::size_t len,
	                   std::size_t& pos, uint64_t& out)
{
	uint64_t value = 0;
	int shift = 0;
	while (pos < len && shift <= 63)
	{
		const uint8_t byte = data[pos++];
		if (shift == 63 && (byte & 0xfe) != 0) return false;
		value |= static_cast<uint64_t>(byte & 0x7f) << shift;
		if (!(byte & 0x80))
		{
			out = value;
			return true;
		}
		shift += 7;
	}
	return false;
}

struct Field
{
	uint32_t number;
	uint8_t wireType;
	std::size_t start;
	std::size_t end;
	std::size_t valueOff;
	std::size_t valueLen;
};

template<typename Fn>
inline bool walk(const uint8_t* data, std::size_t len, Fn&& fn)
{
	std::size_t pos = 0;
	while (pos < len)
	{
		const std::size_t start = pos;
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag)) return false;

		const uint64_t number64 = tag >> 3;
		if (number64 == 0 || number64 > 0x1fffffffULL) return false;
		const uint32_t number = static_cast<uint32_t>(number64);
		const uint8_t wireType = static_cast<uint8_t>(tag & 7);

		std::size_t valueOff = pos;
		std::size_t valueLen = 0;
		switch (wireType)
		{
		case 0:
		{
			const std::size_t valueStart = pos;
			uint64_t ignored = 0;
			if (!readVarint(data, len, pos, ignored)) return false;
			valueOff = valueStart;
			valueLen = pos - valueStart;
			break;
		}
		case 1:
			if (len - pos < 8) return false;
			valueOff = pos;
			valueLen = 8;
			pos += 8;
			break;
		case 2:
		{
			uint64_t size = 0;
			if (!readVarint(data, len, pos, size)) return false;
			if (size > len - pos) return false;
			valueOff = pos;
			valueLen = static_cast<std::size_t>(size);
			pos += valueLen;
			break;
		}
		case 5:
			if (len - pos < 4) return false;
			valueOff = pos;
			valueLen = 4;
			pos += 4;
			break;
		default:
			return false;
		}

		fn(Field{ number, wireType, start, pos, valueOff, valueLen });
	}
	return true;
}

inline void putVarint(std::vector<uint8_t>& out, uint64_t value)
{
	while (value >= 0x80)
	{
		out.push_back(static_cast<uint8_t>(value) | 0x80);
		value >>= 7;
	}
	out.push_back(static_cast<uint8_t>(value));
}

inline void putVarintField(std::vector<uint8_t>& out,
	                       uint32_t number, uint64_t value)
{
	putVarint(out, static_cast<uint64_t>(number) << 3);
	putVarint(out, value);
}

} // namespace Wire

inline std::optional<std::vector<uint8_t>>
rewriteSettings(const uint8_t* data, std::size_t len)
{
	std::vector<uint8_t> out;
	bool parentalEnabled = false;
	const bool valid = Wire::walk(data, len, [&](const Wire::Field& field) {
		switch (field.number)
		{
		case 9: // is_enabled
		{
			if (field.wireType == 0)
			{
				std::size_t pos = field.valueOff;
				uint64_t value = 0;
				if (Wire::readVarint(data, len, pos, value))
				{
					parentalEnabled = value != 0;
				}
			}
			return;
		}
		case 10: // enabled_features
		case 13: // temporary_enabled_features
		case 15: // playtime_restrictions
		case 16: // temporary_playtime_restrictions
			return;
		default:
			out.insert(out.end(), data + field.start, data + field.end);
			return;
		}
	});
	if (!valid) return std::nullopt;
	if (!parentalEnabled) return std::vector<uint8_t>(data, data + len);

	// Keep parental mode active while allowing every locally unlockable feature.
	// Disabling parental mode makes Steam normalize these masks to all bits,
	// including an unnamed feature value that produces startup assertions.
	constexpr uint64_t unlockableFeatures = 0x7fffULL;
	Wire::putVarintField(out, 9, 1);
	Wire::putVarintField(out, 10, unlockableFeatures);
	Wire::putVarintField(out, 13, unlockableFeatures);
	return out;
}

void setup();
void remove();

} // namespace Parental
