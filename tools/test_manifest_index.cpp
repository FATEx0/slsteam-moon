// TDD regression test for depotcache manifest-name indexing.

#include "manifest_index.hpp"

#include <cstdio>

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
	const auto valid = ManifestIndex::parseManifestName("12345_67890.manifest");
	check(valid.has_value(), "valid manifest name is parsed");
	check(valid && valid->depotId == 12345 && valid->gid == 67890,
	      "parsed depot and gid are preserved");
	check(!ManifestIndex::parseManifestName("12345_67890.bin").has_value(),
	      "non-manifest extension is rejected");
	check(!ManifestIndex::parseManifestName("12345_.manifest").has_value(),
	      "missing gid is rejected");
	check(!ManifestIndex::parseManifestName("_67890.manifest").has_value(),
	      "missing depot is rejected");
	check(!ManifestIndex::parseManifestName("abc_67890.manifest").has_value(),
	      "non-numeric depot is rejected");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_manifest_index: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("manifest index tests passed");
	return 0;
}
