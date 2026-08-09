// Regression tests for the IClientRemoteStorage::RunIPCFrame fingerprint
// against both the OLD (2026-07-21 stable) and NEW (2026-08-05) 32-bit
// steamclient.so modules supplied as argv[1] and argv[2].
//
// WHY THIS EXISTS
// ---------------
// The 2026-08-05 client moved every message id in the RemoteStorage dispatch
// tree: the median/root drifted from 0x8712DD4B to 0x8712DD54 and each of the
// sibling comparison pivots moved ~-8..+5. The pre-update fingerprint literals
// (0x5DB4729A, 0x7F3F5645, 0x84692E78) matched the OLD build but missed the NEW
// one, so the required locator regressed to "missing". The repaired literals
// sit at the midpoints so the same bounded fingerprint resolves exactly one
// candidate on BOTH builds.
//
// Build + run (from repo root):
//   g++ -std=c++20 -I include tools/test_autorepair_cmpfingerprint.cpp -o /tmp/tacf \
//     && /tmp/tacf /path/to/OLD/steamclient.so /path/to/NEW/steamclient.so

#include "../src/feats/ipcframe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                      \
	do {                                                                      \
		++g_checks;                                                           \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }        \
	} while (0)

struct TextSection
{
	std::vector<uint8_t> bytes;
	bool ok = false;
};

template <class T> static T rd(const std::vector<uint8_t>& d, size_t off)
{
	T v{}; std::memcpy(&v, d.data() + off, sizeof(T)); return v;
}

static TextSection loadText(const std::string& path)
{
	TextSection ts;
	std::ifstream f(path, std::ios::binary);
	if (!f) return ts;
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
	if (d.size() < 0x34 || std::memcmp(d.data(), "\x7f""ELF", 4) != 0) return ts;

	const uint32_t e_shoff    = rd<uint32_t>(d, 0x20);
	const uint16_t e_shentsz  = rd<uint16_t>(d, 0x2e);
	const uint16_t e_shnum    = rd<uint16_t>(d, 0x30);
	const uint16_t e_shstrndx = rd<uint16_t>(d, 0x32);

	auto shName   = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x00); };
	auto shOffset = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x10); };
	auto shSize   = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x14); };

	const uint32_t strOff = shOffset(e_shstrndx);
	for (int i = 0; i < e_shnum; ++i)
	{
		const char* nm = reinterpret_cast<const char*>(d.data() + strOff + shName(i));
		if (std::strcmp(nm, ".text") == 0)
		{
			const uint32_t off = shOffset(i), sz = shSize(i);
			ts.bytes.assign(d.begin() + off, d.begin() + off + sz);
			ts.ok = true;
			return ts;
		}
	}
	return ts;
}

// Midpoints of the two shipped builds; these are the literals embedded in
// src/patterns.cpp and MUST stay in sync with them (guard re-derives the
// fingerprint from src/patterns.cpp and independently verifies the candidates).
static constexpr uint32_t kRepairedFingerprint[] = {
	0x5DB47296, 0x7F3F564A, 0x84692E73,
};
static constexpr uint32_t kStaleFingerprint[] = {
	0x5DB4729A, 0x7F3F5645, 0x84692E78,
};

static size_t countFingerprintMatches(
	const std::vector<IpcFrame::Cand>& cands,
	const uint8_t* text,
	const uint32_t* pivots)
{
	size_t hits = 0;
	for (const auto& cand : cands)
	{
		if (IpcFrame::matchesCmpFingerprint(
			text + cand.offset, cand.available,
			pivots, 3, 4))
		{
			++hits;
		}
	}
	return hits;
}

static void run(const std::string& label, const TextSection& ts,
	            const std::vector<IpcFrame::Cand>& cands,
	            uint32_t expectedRoot, uint32_t staleHits, uint32_t repairedHits)
{
	// The whole .text must still yield the generic dispatch tails.
	CHECK(cands.size() >= 4, (label + ": generic RunIPCFrame tails found").c_str());

	// The repaired midpoints must identify EXACTLY ONE generated dispatcher on
	// BOTH builds, and it must be the RemoteStorage tree whose root is stable.
	CHECK(repairedHits == 1, (label + ": repaired fingerprint unique").c_str());
	if (repairedHits != 1) return;

	size_t idx = SIZE_MAX;
	for (size_t i = 0; i < cands.size(); ++i)
	{
		if (IpcFrame::matchesCmpFingerprint(
			ts.bytes.data() + cands[i].offset, cands[i].available,
			kRepairedFingerprint, 3, 4))
		{
			idx = i;
		}
	}
	if (idx == SIZE_MAX) return;
	CHECK(cands[idx].root == expectedRoot,
	      (label + ": resolved RemoteStorage dispatch-tree root").c_str());

	// Pre-repair literals never need to fail on the OLD build; the NEW build
	// carries the regression proof (stale == 0 there).
	std::printf("%s: stale=%u repaired=%u\n",
	            label.c_str(), staleHits, repairedHits);
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: %s <old steamclient.so> <new steamclient.so>\n", argv[0]);
		return 1;
	}

	TextSection oldTs = loadText(argv[1]);
	TextSection newTs = loadText(argv[2]);
	CHECK(oldTs.ok && newTs.ok, "both supplied steamclient.so load");

	if (oldTs.ok)
	{
		auto cands = IpcFrame::scan(oldTs.bytes.data(), oldTs.bytes.size());
		size_t stale = countFingerprintMatches(cands, oldTs.bytes.data(), kStaleFingerprint);
		size_t rep   = countFingerprintMatches(cands, oldTs.bytes.data(), kRepairedFingerprint);
		run("OLD", oldTs, cands, 0x8712DD4B, (uint32_t)stale, (uint32_t)rep);
	}

	if (newTs.ok)
	{
		auto cands = IpcFrame::scan(newTs.bytes.data(), newTs.bytes.size());
		size_t stale = countFingerprintMatches(cands, newTs.bytes.data(), kStaleFingerprint);
		size_t rep   = countFingerprintMatches(cands, newTs.bytes.data(), kRepairedFingerprint);

		// The OLD literals must NOT resolve on the NEW binary: this is the
		// regression that the repair fixes.
		CHECK(stale == 0, "NEW: stale fingerprint misses (drift, pre-repair)");
		run("NEW", newTs, cands, 0x8712DD54, (uint32_t)stale, (uint32_t)rep);
	}

	if (g_failures == 0)
	{
		std::printf("\ntest_autorepair_cmpfingerprint: ALL PASS (%d checks)\n", g_checks);
		return 0;
	}
	std::printf("\ntest_autorepair_cmpfingerprint: %d/%d CHECK(S) FAILED\n", g_failures, g_checks);
	return 1;
}
