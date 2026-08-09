// TDD regression test for bounded PIC-thunk instruction formatting.

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
	char instruction[64] = {};
	check(MemHlp::formatPICThunkInstruction(
	              instruction, sizeof(instruction), "mov", "eax", 0x401234),
	      "valid instruction is formatted");
	check(std::string(instruction) == "mov eax, 0x401234",
	      "formatted instruction preserves operands");

	char tiny[8] = {};
	check(!MemHlp::formatPICThunkInstruction(
	              tiny, sizeof(tiny), "mov", "eax", 0x401234),
	      "oversized instruction is rejected");
	check(!MemHlp::formatPICThunkInstruction(
	              instruction, sizeof(instruction), "", "eax", 0x401234),
	      "empty mnemonic is rejected");
	check(!MemHlp::formatPICThunkInstruction(
	              instruction, sizeof(instruction), "mov", "", 0x401234),
	      "empty operand is rejected");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_memhlp_pic: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("memhlp PIC tests passed");
	return 0;
}
