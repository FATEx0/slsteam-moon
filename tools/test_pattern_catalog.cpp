#include "pattern_catalog.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace
{
	int failures = 0;

	void expect(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	const PatternCatalog::ModuleIdentity expected
	{
		"steamclient",
		"steamclient.so",
		std::string(64, 'a'),
		8192,
		std::string(40, 'b'),
	};

	std::string validToml()
	{
		return
			"schema = 1\n"
			"platform = \"linux32\"\n"
			"component = \"steamclient\"\n"
			"module_name = \"steamclient.so\"\n"
			"module_sha256 = \"" + std::string(64, 'a') + "\"\n"
			"module_size = 8192\n"
			"gnu_build_id = \"" + std::string(40, 'b') + "\"\n"
			"steam_version = 1785347151\n"
			"consumer_commit = \"" + std::string(40, 'c') + "\"\n"
			"minimum_consumer_schema = 1\n"
			"revision = 1\n"
			"source = \"deterministic\"\n"
			"\n"
			"[[locators]]\n"
			"symbol = \"Patterns::Root\"\n"
			"name = \"Root\"\n"
			"target_rva = 4660\n"
			"match_rva = 4656\n"
			"signature = \"55 89 E5\"\n"
			"follow_mode = \"None\"\n"
			"resolver = \"signature\"\n"
			"required = true\n"
			"match_count = 1\n"
			"target_count = 1\n";
	}

	void replaceOnce(std::string& text, std::string_view from, std::string_view to)
	{
		const auto position = text.find(from);
		expect(position != std::string::npos, "test mutation anchor exists");
		if (position != std::string::npos)
			text.replace(position, from.size(), to);
	}

	void expectRejected(std::string body, std::string_view label)
	{
		std::string error;
		auto parsed = PatternCatalog::parseCanonical(body, expected, &error);
		expect(!parsed.has_value(), label);
		expect(!error.empty(), "rejection includes a bounded diagnostic");
	}
}

int main()
{
	std::string error;
	auto parsed = PatternCatalog::parseCanonical(validToml(), expected, &error);
	expect(parsed.has_value(), "literal canonical TOML parses");
	expect(error.empty(), "valid parse has no diagnostic");
	if (parsed)
	{
		expect(parsed->component() == "steamclient", "component is retained");
		expect(parsed->revision() == 1, "revision is retained");
		auto target = parsed->target("Patterns::Root");
		expect(target.has_value() && *target == 4660, "symbol maps to exact RVA");
		expect(!parsed->target("Patterns::Missing").has_value(),
		       "unknown symbol has no target");

		const PatternCatalog::CompiledLocator allowed[] =
		{
			{"Patterns::Root", true},
		};
		expect(parsed->validatePolicy(allowed),
		       "metadata matches compiled symbol and required policy");
		const PatternCatalog::CompiledLocator weakened[] =
		{
			{"Patterns::Root", false},
		};
		expect(!parsed->validatePolicy(weakened),
		       "metadata cannot change compiled optionality");
		const PatternCatalog::CompiledLocator unrelated[] =
		{
			{"Patterns::Other", true},
		};
		expect(!parsed->validatePolicy(unrelated),
		       "metadata cannot introduce an uncompiled symbol");
		const PatternCatalog::ExecutableRange executable[] =
		{
			{4096, 5000},
		};
		expect(parsed->executableTarget("Patterns::Root", true, 8192, executable) == 4660,
		       "compiled locator may use an RVA in an executable range");
		expect(!parsed->executableTarget("Patterns::Root", false, 8192, executable),
		       "runtime lookup cannot change optionality");
		const PatternCatalog::ExecutableRange nonExecutable[] =
		{
			{0, 4096},
		};
		expect(!parsed->executableTarget("Patterns::Root", true, 8192, nonExecutable),
		       "RVA outside executable segments is ignored");
		expect(!parsed->executableTarget("Patterns::Root", true, 4096, executable),
		       "RVA outside the current module is ignored");
	}

	{
		auto body = validToml();
		replaceOnce(body, "schema = 1", "schema = 2");
		expectRejected(body, "wrong schema is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "component = \"steamclient\"", "component = \"steamui\"");
		expectRejected(body, "wrong component is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "module_size = 8192", "module_size = 8193");
		expectRejected(body, "wrong module identity is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "target_rva = 4660", "target_rva = 4294967296");
		expectRejected(body, "RVA overflow is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "signature = \"55 89 E5\"", "signature = \"55 89 e5\"");
		expectRejected(body, "noncanonical lowercase signature bytes are rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "target_rva = 4660\n", "");
		expectRejected(body, "missing required locator value is rejected");
	}
	{
		auto body = validToml();
		const auto locator = body.substr(body.find("\n[[locators]]"));
		body += locator;
		expectRejected(body, "duplicate locator symbol is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "name = \"Root\"", "name = \"Root\\nInjected\"");
		expectRejected(body, "string escapes are rejected");
	}
	{
		auto body = validToml();
		body.insert(body.find("platform"), "unknown = 1\n");
		expectRejected(body, "unknown top-level field is rejected");
	}
	{
		auto body = validToml();
		body.insert(body.find("platform"), "schema = 1\n");
		expectRejected(body, "duplicate top-level field is rejected");
	}
	{
		auto body = validToml();
		body.insert(body.end() - 1, static_cast<char>(0xFF));
		expectRejected(body, "malformed UTF-8 is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "target_rva = 4660", "target_rva = 8192");
		expectRejected(body, "RVA outside the declared module is rejected");
	}
	{
		auto body = validToml();
		body += "trailing = 1\n";
		expectRejected(body, "trailing garbage is rejected");
	}

	if (failures != 0)
	{
		std::cerr << failures << " pattern catalog test(s) failed\n";
		return 1;
	}
	std::cout << "pattern catalog tests passed\n";
	return 0;
}
