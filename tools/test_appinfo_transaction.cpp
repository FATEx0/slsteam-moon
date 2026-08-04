// Integration regression test for the appinfo.vdf transaction boundary.

#include "../src/feats/appinfo_vdf.hpp"
#include "../src/log.hpp"
#include "../src/utils/atomic_file.hpp"
#include "../include/base64/base64.hpp"

#include <openssl/sha.h>
#include <yaml-cpp/emitter.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{

template <typename T>
void appendLE(std::vector<uint8_t>& out, T value)
{
	const auto* p = reinterpret_cast<const uint8_t*>(&value);
	out.insert(out.end(), p, p + sizeof(value));
}

void createEmptyAppInfo(const std::string& path)
{
	std::vector<uint8_t> bytes;
	appendLE<uint32_t>(bytes, 0x07564429u);
	appendLE<uint32_t>(bytes, 1u);
	appendLE<int64_t>(bytes, 20);
	appendLE<uint32_t>(bytes, 0u); // app footer
	appendLE<uint32_t>(bytes, 0u); // empty string table
	std::string error;
	assert(AtomicFile::write(path,
		reinterpret_cast<const char*>(bytes.data()), bytes.size(), error));
}

std::string wireFor(uint64_t gid)
{
	return "\"appinfo\"\n{\n"
	       "\t\"depots\"\n\t{\n"
	       "\t\t\"123\"\n\t\t{\n"
	       "\t\t\t\"manifests\"\n\t\t\t{\n"
	       "\t\t\t\t\"public\"\n\t\t\t\t{\n"
	       "\t\t\t\t\t\"gid\"\t\"" + std::to_string(gid) + "\"\n"
	       "\t\t\t\t}\n\t\t\t}\n"
	       "\t\t}\n"
	       "\t}\n"
	       "}\n";
}

std::string sha1Of(const std::string& data)
{
	unsigned char digest[SHA_DIGEST_LENGTH]{};
	SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
	return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

void writeCache(const std::string& cacheDir, uint32_t appid,
		uint32_t change, const std::string& wire)
{
	const auto sha = sha1Of(wire);
	const auto stem = cacheDir + "/picsbuffer_" + std::to_string(appid);
	std::string error;
	assert(AtomicFile::write(stem + ".bin", wire, error));

	YAML::Emitter emitter;
	emitter << YAML::BeginMap
	         << YAML::Key << "appid" << YAML::Value << appid
	         << YAML::Key << "change_number" << YAML::Value << change
	         << YAML::Key << "wire_size" << YAML::Value << wire.size()
	         << YAML::Key << "sha_b64" << YAML::Value << base64::to_base64(sha)
	         << YAML::EndMap;
	const std::string metadata(emitter.c_str(), emitter.size());
	assert(AtomicFile::write(stem + ".yaml", metadata, error));
}

std::string readAll(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

int main()
{
	const std::string root =
		"/tmp/slssteam-appinfo-transaction." + std::to_string(getpid());
	const std::string configHome = root + "/config";
	const std::string cacheDir = configHome + "/SLSsteam/cache";
	const std::string appinfo = root + "/appcache/appinfo.vdf";
	std::error_code ec;
	std::filesystem::create_directories(cacheDir, ec);
	std::filesystem::create_directories(root + "/appcache", ec);
	assert(!ec);

	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((root + "/test.log").c_str()));

	createEmptyAppInfo(appinfo);
	const auto first = wireFor(456);
	const auto second = wireFor(789);
	writeCache(cacheDir, 1001, 10, first);
	writeCache(cacheDir, 1002, 20, second);

	assert(AppInfoVdf::injectAllCached(appinfo) == 2);
	const auto published = readAll(appinfo);
	assert(!published.empty());
	assert(std::filesystem::exists(appinfo + ".slssteam-previous"));

	// A second pass is idempotent and must not produce another visible rewrite.
	assert(AppInfoVdf::injectAllCached(appinfo) == 2);
	assert(readAll(appinfo) == published);

	// Simulate a torn/corrupt v41 file.  The previous good snapshot is restored
	// and the cache transaction can continue without exposing the bad bytes.
	{
		std::ofstream out(appinfo, std::ios::binary | std::ios::trunc);
		out << "\x29\x44\x56\x07broken";
	}
	assert(AppInfoVdf::injectAllCached(appinfo) == 2);
	assert(readAll(appinfo) == published);

	// No fixed appinfo.vdf.tmp may remain after either success or recovery.
	assert(!std::filesystem::exists(appinfo + ".tmp"));
	return 0;
}
