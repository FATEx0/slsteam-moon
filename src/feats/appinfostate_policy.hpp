#pragma once

#include <cstdint>

namespace AppInfoStatePolicy
{
enum class Action : std::uint8_t
{
	None,
	MarkSkip,
	SignalResolved,
};

constexpr Action decide(
	bool managed,
	bool create,
	bool shaEmpty,
	bool skipSet
) noexcept
{
	if (!managed || create)
		return Action::None;

	if (!shaEmpty)
		return Action::SignalResolved;

	return skipSet ? Action::None : Action::MarkSkip;
}
}
