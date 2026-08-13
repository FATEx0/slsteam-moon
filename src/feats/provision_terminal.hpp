// SPDX-License-Identifier: AGPL-3.0-only
//
// Persistent, change-number-bound terminal results for AppInfo provisioning.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ProvisionTerminal
{

inline constexpr std::uint32_t kSchema = 1;

enum class Kind { VirtualDlc, NoUsableContent };

struct Record
{
	std::uint32_t schema = kSchema;
	std::uint32_t appId = 0;
	Kind kind = Kind::VirtualDlc;
	std::uint32_t changeNumber = 0;
	std::string inputFingerprint;
	bool operator==(const Record&) const = default;
};

struct LocalInput
{
	std::uint32_t depotId = 0;
	std::string key;
	std::uint64_t manifestGid = 0;
};

enum class LoadStatus { Missing, Valid, Invalid, IoError };
struct LoadResult { LoadStatus status; Record record; };

bool parse(std::string_view text, Record& out) noexcept;
std::string render(const Record& record);
std::string fingerprint(std::vector<LocalInput> inputs);
bool applies(const Record& record, std::uint32_t appId,
             std::uint32_t observedChangeNumber,
             std::string_view currentInputFingerprint) noexcept;

class Store
{
public:
	explicit Store(std::string cacheDir);
	std::string path(std::uint32_t appId) const;
	LoadResult load(std::uint32_t appId) const;
	bool publish(const Record& record) const;
	bool erase(std::uint32_t appId) const;

private:
	std::string cacheDir_;
};

} // namespace ProvisionTerminal
