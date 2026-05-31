// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone smoke test for ManifestId::rewriteDepotGid logic.
// Builds outside the main .so so we can exercise the regex/walker
// against captured PICS buffers without spinning up Steam.
//
// Usage: ./test_rewrite <picsbuffer.bin> <depotid> <newgid>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

// Inline copy of ManifestId::rewriteDepotGid — the exact same logic
// from feats/manifestid.cpp, kept in sync by hand for this smoke
// test.  Intentional duplication; if the real code changes shape,
// rerun this with the new version pasted in.
namespace
{
bool rewriteDepotGid(std::string& buf, uint32_t depotId,
                     const std::string& newGid)
{
	std::string depotKey = "\"" + std::to_string(depotId) + "\"";
	size_t pos = 0;
	while (pos < buf.size())
	{
		size_t depotPos = buf.find(depotKey, pos);
		if (depotPos == std::string::npos) return false;

		bool isKey = (depotPos == 0);
		if (!isKey)
		{
			size_t scan = depotPos;
			while (scan > 0 && (buf[scan - 1] == ' ' || buf[scan - 1] == '\t'))
				--scan;
			isKey = (scan == 0 || buf[scan - 1] == '\n');
		}
		if (!isKey)
		{
			pos = depotPos + depotKey.size();
			continue;
		}

		size_t openBrace = buf.find('{', depotPos + depotKey.size());
		if (openBrace == std::string::npos) return false;

		int depth = 1;
		size_t scan = openBrace + 1;
		size_t depotEnd = std::string::npos;
		while (scan < buf.size() && depth > 0)
		{
			char c = buf[scan];
			if (c == '"')
			{
				size_t end = buf.find('"', scan + 1);
				if (end == std::string::npos) return false;
				scan = end + 1;
				continue;
			}
			if (c == '{') ++depth;
			else if (c == '}')
			{
				--depth;
				if (depth == 0) { depotEnd = scan; break; }
			}
			++scan;
		}
		if (depotEnd == std::string::npos) return false;

		const std::string scopeView(buf.data() + openBrace,
		                            depotEnd - openBrace);
		size_t pubKey = scopeView.find("\"public\"");
		if (pubKey == std::string::npos)
		{
			pos = depotEnd + 1;
			continue;
		}

		size_t pubKeyAbs = openBrace + pubKey;
		size_t pubOpen = buf.find('{', pubKeyAbs + 8);
		if (pubOpen == std::string::npos || pubOpen > depotEnd)
		{
			pos = depotEnd + 1;
			continue;
		}

		int pubDepth = 1;
		size_t pubScan = pubOpen + 1;
		size_t pubEnd = std::string::npos;
		while (pubScan < buf.size() && pubDepth > 0)
		{
			char c = buf[pubScan];
			if (c == '"')
			{
				size_t end = buf.find('"', pubScan + 1);
				if (end == std::string::npos) return false;
				pubScan = end + 1;
				continue;
			}
			if (c == '{') ++pubDepth;
			else if (c == '}')
			{
				--pubDepth;
				if (pubDepth == 0) { pubEnd = pubScan; break; }
			}
			++pubScan;
		}
		if (pubEnd == std::string::npos) return false;

		const std::string pubView(buf.data() + pubOpen, pubEnd - pubOpen);
		static const std::regex gidRe("(\"gid\"[\\s]*\")([0-9]+)(\")");
		std::smatch m;
		if (!std::regex_search(pubView, m, gidRe))
		{
			pos = depotEnd + 1;
			continue;
		}

		const size_t valStartInView = m.position(2);
		const size_t valLen = m.length(2);
		const size_t valStart = pubOpen + valStartInView;
		if (buf.compare(valStart, valLen, newGid) == 0) return true;
		buf.replace(valStart, valLen, newGid);
		return true;
	}
	return false;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 4)
	{
		std::fprintf(stderr,
			"Usage: %s <picsbuffer.bin> <depotid> <newgid>\n", argv[0]);
		return 2;
	}

	std::ifstream ifs(argv[1], std::ios::binary);
	if (!ifs.is_open())
	{
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	std::stringstream buf;
	buf << ifs.rdbuf();
	std::string content = buf.str();

	const uint32_t depotId = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
	const std::string newGid = argv[3];

	const auto before = content.size();
	const bool changed = rewriteDepotGid(content, depotId, newGid);
	const auto after = content.size();

	std::printf("rewriteDepotGid: depot=%u new_gid=%s changed=%s "
	            "before=%zu after=%zu\n",
	            depotId, newGid.c_str(),
	            changed ? "yes" : "no", before, after);

	if (changed)
	{
		// Print the snippet around the (possibly new) gid for visual
		// inspection.  Find the depot key, then dump ~300 chars of
		// surrounding context.
		const std::string depotKey = "\"" + std::to_string(depotId) + "\"";
		size_t pos = content.find(depotKey);
		if (pos != std::string::npos)
		{
			size_t start = pos > 50 ? pos - 50 : 0;
			size_t end = std::min(content.size(), pos + 350);
			std::fwrite(content.data() + start, 1, end - start, stdout);
			std::printf("\n");
		}
	}
	return 0;
}
