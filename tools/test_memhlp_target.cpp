// TDD regression test for safe direct-call/jump operand parsing.

#include "memhlp.hpp"

#include <cstdio>
#include <string>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}
}

int main()
{
	using MemHlp::parseJumpTargetOperand;

	check(parseJumpTargetOperand("0x401234") == static_cast<lm_address_t>(0x401234),
	      "hexadecimal target with 0x prefix");
	check(parseJumpTargetOperand("7f00") == static_cast<lm_address_t>(0x7f00),
	      "hexadecimal target without prefix");
	check(parseJumpTargetOperand("eax") == LM_ADDRESS_BAD,
	      "register operand is rejected");
	check(parseJumpTargetOperand("dword ptr [ebx+0x10]") == LM_ADDRESS_BAD,
	      "indirect memory operand is rejected");
	check(parseJumpTargetOperand("") == LM_ADDRESS_BAD,
	      "empty operand is rejected");
	check(parseJumpTargetOperand(nullptr) == LM_ADDRESS_BAD,
	      "null operand is rejected");
	check(parseJumpTargetOperand("0x") == LM_ADDRESS_BAD,
	      "prefix without digits is rejected");
	check(parseJumpTargetOperand("0x123g") == LM_ADDRESS_BAD,
	      "partially numeric operand is rejected");

	const std::string tooLarge = sizeof(lm_address_t) <= 4
		? "0x100000000"
		: "0x10000000000000000";
	check(parseJumpTargetOperand(tooLarge.c_str()) == LM_ADDRESS_BAD,
	      "out-of-range target is rejected");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_memhlp_target: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("memhlp target tests passed");
	return 0;
}
