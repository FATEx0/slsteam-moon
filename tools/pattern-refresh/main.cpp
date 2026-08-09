#include "catalog.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>


#ifndef PATTERN_PUBLIC_KEY_HEX
#define PATTERN_PUBLIC_KEY_HEX ""
#endif


int main(int argc, char** argv)
{
	std::vector<std::string_view> arguments;
	arguments.reserve(static_cast<std::size_t>(argc));
	for (int index = 0; index < argc; ++index)
		arguments.emplace_back(argv[index] != nullptr ? argv[index] : "");
	const auto invocation = PatternRefresh::parseInvocation(arguments);
	if (!invocation)
	{
		std::cerr << "usage: pattern-refresh [--cache-only] --steam-root PATH "
		             "--config-root PATH\n";
		return 2;
	}
	const auto publicKey = PatternRefresh::parsePublicKeyHex(PATTERN_PUBLIC_KEY_HEX);
	if (!publicKey)
	{
		std::cerr << "pattern-refresh: embedded public key is invalid\n";
		return 2;
	}

	std::string diagnostic;
	const int result = PatternRefresh::refreshInstallation(
		invocation->steamRoot,
		invocation->configRoot,
		*publicKey,
		invocation->mode,
		{},
		&diagnostic
	);
	if (result == 0)
		std::cerr << "pattern-refresh: exact signed metadata active\n";
	else if (result == 3)
		std::cerr << "pattern-refresh: using validated cache or embedded fallback\n";
	else
		std::cerr << "pattern-refresh: invalid invocation\n";
	return result;
}
