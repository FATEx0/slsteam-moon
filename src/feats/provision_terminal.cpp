// SPDX-License-Identifier: AGPL-3.0-only

#include "provision_terminal.hpp"

#include "../utils/atomic_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <utility>

namespace ProvisionTerminal
{
namespace
{

constexpr std::size_t kMaxStateBytes = 4096;

bool parseUnsigned(std::string_view text, std::uint32_t& out) noexcept
{
	if (text.empty()) return false;
	std::uint32_t value = 0;
	const char* first = text.data();
	const char* last = first + text.size();
	const auto result = std::from_chars(first, last, value, 10);
	if (result.ec != std::errc{} || result.ptr != last ||
	    std::to_string(value) != text)
	{
		return false;
	}
	out = value;
	return true;
}

bool isFingerprint(std::string_view value) noexcept
{
	if (value.size() != 16) return false;
	for (const char c : value)
	{
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

bool isValid(const Record& record) noexcept
{
	if (record.schema != kSchema || record.appId == 0 ||
	    record.changeNumber == 0)
	{
		return false;
	}
	switch (record.kind)
	{
	case Kind::VirtualDlc:
		return record.inputFingerprint == "-";
	case Kind::NoUsableContent:
		return isFingerprint(record.inputFingerprint);
	}
	return false;
}

void fnvByte(std::uint64_t& hash, unsigned char byte) noexcept
{
	hash ^= byte;
	hash *= UINT64_C(1099511628211);
}

template <typename UInt>
void fnvUnsigned(std::uint64_t& hash, UInt value) noexcept
{
	for (std::size_t shift = sizeof(UInt) * 8; shift != 0; shift -= 8)
		fnvByte(hash, static_cast<unsigned char>(value >> (shift - 8)));
}

} // namespace

bool parse(std::string_view text, Record& out) noexcept
{
	if (text.size() > kMaxStateBytes || text.find('\r') != std::string_view::npos)
		return false;

	constexpr std::array<std::string_view, 5> prefixes = {
		"schema: ", "appid: ", "kind: ", "change_number: ",
		"input_fingerprint: "};
	std::array<std::string_view, 5> values{};
	std::size_t position = 0;
	for (std::size_t index = 0; index < prefixes.size(); ++index)
	{
		const std::size_t newline = text.find('\n', position);
		if (newline == std::string_view::npos) return false;
		const std::string_view line = text.substr(position, newline - position);
		if (!line.starts_with(prefixes[index])) return false;
		values[index] = line.substr(prefixes[index].size());
		if (values[index].empty()) return false;
		position = newline + 1;
	}
	if (position != text.size()) return false;

	Record parsed;
	if (!parseUnsigned(values[0], parsed.schema) ||
	    !parseUnsigned(values[1], parsed.appId) ||
	    !parseUnsigned(values[3], parsed.changeNumber))
	{
		return false;
	}
	if (values[2] == "virtual_dlc")
		parsed.kind = Kind::VirtualDlc;
	else if (values[2] == "no_usable_content")
		parsed.kind = Kind::NoUsableContent;
	else
		return false;
	parsed.inputFingerprint = values[4];
	if (!isValid(parsed)) return false;
	out = std::move(parsed);
	return true;
}

std::string render(const Record& record)
{
	if (!isValid(record)) return {};
	const std::string_view kind = record.kind == Kind::VirtualDlc
		? "virtual_dlc" : "no_usable_content";
	return "schema: " + std::to_string(record.schema) + "\n" +
	       "appid: " + std::to_string(record.appId) + "\n" +
	       "kind: " + std::string(kind) + "\n" +
	       "change_number: " + std::to_string(record.changeNumber) + "\n" +
	       "input_fingerprint: " + record.inputFingerprint + "\n";
}

std::string fingerprint(std::vector<LocalInput> inputs)
{
	std::sort(inputs.begin(), inputs.end(), [](const LocalInput& left,
	                                            const LocalInput& right) {
		if (left.depotId != right.depotId) return left.depotId < right.depotId;
		if (left.key != right.key) return left.key < right.key;
		return left.manifestGid < right.manifestGid;
	});

	std::uint64_t hash = UINT64_C(14695981039346656037);
	for (const auto& input : inputs)
	{
		fnvUnsigned(hash, input.depotId);
		fnvUnsigned(hash, static_cast<std::uint64_t>(input.key.size()));
		for (const unsigned char byte : input.key) fnvByte(hash, byte);
		fnvUnsigned(hash, input.manifestGid);
	}

	constexpr char hex[] = "0123456789abcdef";
	std::string result(16, '0');
	for (std::size_t index = 0; index < result.size(); ++index)
	{
		const std::size_t shift = (result.size() - 1 - index) * 4;
		result[index] = hex[(hash >> shift) & 0x0f];
	}
	return result;
}

bool applies(const Record& record, std::uint32_t appId,
             std::uint32_t observedChangeNumber,
             std::string_view currentInputFingerprint) noexcept
{
	if (!isValid(record) || record.appId != appId ||
	    record.changeNumber != observedChangeNumber)
	{
		return false;
	}
	return record.kind == Kind::VirtualDlc ||
	       record.inputFingerprint == currentInputFingerprint;
}

Store::Store(std::string cacheDir) : cacheDir_(std::move(cacheDir)) {}

std::string Store::path(std::uint32_t appId) const
{
	return (std::filesystem::path(cacheDir_) /
	        ("terminal_" + std::to_string(appId) + ".state")).string();
}

LoadResult Store::load(std::uint32_t appId) const
{
	if (appId == 0) return {LoadStatus::Invalid, {}};
	const std::string statePath = path(appId);
	std::error_code ec;
	const bool exists = std::filesystem::exists(statePath, ec);
	if (ec) return {LoadStatus::IoError, {}};
	if (!exists) return {LoadStatus::Missing, {}};

	std::ifstream file(statePath, std::ios::binary);
	if (!file.is_open()) return {LoadStatus::IoError, {}};
	std::string text(kMaxStateBytes + 1, '\0');
	file.read(text.data(), static_cast<std::streamsize>(text.size()));
	const std::streamsize count = file.gcount();
	if (file.bad())
		return {LoadStatus::IoError, {}};
	if (count > static_cast<std::streamsize>(kMaxStateBytes))
		return {LoadStatus::Invalid, {}};
	text.resize(static_cast<std::size_t>(count));

	Record record;
	if (!parse(text, record) || record.appId != appId)
		return {LoadStatus::Invalid, {}};
	return {LoadStatus::Valid, std::move(record)};
}

bool Store::publish(const Record& record) const
{
	if (!isValid(record)) return false;
	const std::string text = render(record);
	if (text.empty()) return false;
	std::string error;
	return AtomicFile::write(path(record.appId), text, error);
}

bool Store::erase(std::uint32_t appId) const
{
	if (appId == 0) return false;
	std::error_code ec;
	const bool removed = std::filesystem::remove(path(appId), ec);
	return !ec && (removed || !std::filesystem::exists(path(appId), ec));
}

} // namespace ProvisionTerminal
