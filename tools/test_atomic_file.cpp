// Regression test for atomic cache/appinfo replacement.

#include "../src/utils/atomic_file.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

int main()
{
	const std::string path =
		"/tmp/slssteam-test-atomic-file." + std::to_string(getpid());
	std::error_code ec;
	std::filesystem::remove(path, ec);

	std::string error;
	assert(AtomicFile::write(path, "first", error));
	assert(error.empty());
	assert(AtomicFile::write(path, "second", error));

	std::ifstream in(path, std::ios::binary);
	std::string contents((std::istreambuf_iterator<char>(in)),
	                     std::istreambuf_iterator<char>());
	assert(contents == "second");

	for (const auto& entry : std::filesystem::directory_iterator(
			std::filesystem::path(path).parent_path()))
	{
		assert(entry.path().filename().string().find(
			"slssteam-test-atomic-file.") == std::string::npos
			|| entry.path() == path);
	}

	std::filesystem::remove(path, ec);
	return 0;
}
