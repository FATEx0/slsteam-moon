// Standalone roundtrip test for the v41 appinfo.vdf parser/writer.
// Compile: g++ -std=c++20 -O2 main.cpp -lcrypto -o appinfo_roundtrip
// Run:     ./appinfo_roundtrip /path/to/appinfo.vdf
//
// Reads appinfo.vdf v41, writes it back to /tmp/appinfo_out.vdf, then diffs.
// If the parser and writer are exact inverses, the output should be
// byte-identical to the input.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

constexpr uint32_t MAGIC_V41 = 0x07564429;

template <typename T> T readLE(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
template <typename T> void writeLE(std::vector<uint8_t>& out, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
static void writeBytes(std::vector<uint8_t>& out, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

struct AppEntry {
    uint32_t appid = 0, info_state = 2, last_updated = 0, change_number = 0;
    uint64_t pics_token = 0;
    uint8_t sha[20] = {0}, binary_hash[20] = {0};
    std::vector<uint8_t> binary_vdf;
};

struct AppInfoFile {
    uint32_t universe = 1;
    std::vector<AppEntry> apps;
    std::vector<std::string> strings;
};

bool readV41(const std::string& path, AppInfoFile& out, std::string& err) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) { err = "open"; return false; }
    auto sz = (size_t)ifs.tellg();
    ifs.seekg(0);
    std::vector<uint8_t> buf(sz);
    ifs.read((char*)buf.data(), sz);
    if (sz < 16) { err = "too small"; return false; }
    uint32_t m = readLE<uint32_t>(buf.data());
    if (m != MAGIC_V41) { err = "bad magic"; return false; }
    out.universe = readLE<uint32_t>(buf.data() + 4);
    auto sto = (size_t)readLE<int64_t>(buf.data() + 8);
    if (sto > sz) { err = "bad sto"; return false; }
    {
        const uint8_t* p = buf.data() + sto;
        const uint8_t* end = buf.data() + sz;
        if (end - p < 4) { err = "st truncated"; return false; }
        uint32_t cnt = readLE<uint32_t>(p);
        p += 4;
        out.strings.reserve(cnt);
        for (uint32_t i = 0; i < cnt; ++i) {
            const uint8_t* nul = (const uint8_t*)std::memchr(p, 0, end - p);
            if (!nul) { err = "st missing nul"; return false; }
            out.strings.emplace_back((const char*)p, nul - p);
            p = nul + 1;
        }
    }
    size_t pos = 16;
    while (pos < sto) {
        uint32_t appid = readLE<uint32_t>(buf.data() + pos); pos += 4;
        if (appid == 0) break;
        uint32_t es = readLE<uint32_t>(buf.data() + pos); pos += 4;
        size_t entryEnd = pos + es;
        AppEntry e;
        e.appid = appid;
        e.info_state    = readLE<uint32_t>(buf.data() + pos); pos += 4;
        e.last_updated  = readLE<uint32_t>(buf.data() + pos); pos += 4;
        e.pics_token    = readLE<uint64_t>(buf.data() + pos); pos += 8;
        std::memcpy(e.sha, buf.data() + pos, 20);              pos += 20;
        e.change_number = readLE<uint32_t>(buf.data() + pos); pos += 4;
        std::memcpy(e.binary_hash, buf.data() + pos, 20);      pos += 20;
        e.binary_vdf.assign(buf.data() + pos, buf.data() + entryEnd);
        pos = entryEnd;
        out.apps.push_back(std::move(e));
    }
    return true;
}

bool writeV41(const std::string& path, const AppInfoFile& f, std::string& err) {
    std::vector<uint8_t> body;
    for (const auto& e : f.apps) {
        writeLE<uint32_t>(body, e.appid);
        size_t sizePos = body.size();
        writeLE<uint32_t>(body, 0);
        size_t entryStart = body.size();
        writeLE<uint32_t>(body, e.info_state);
        writeLE<uint32_t>(body, e.last_updated);
        writeLE<uint64_t>(body, e.pics_token);
        writeBytes(body, e.sha, 20);
        writeLE<uint32_t>(body, e.change_number);
        writeBytes(body, e.binary_hash, 20);
        writeBytes(body, e.binary_vdf.data(), e.binary_vdf.size());
        uint32_t es = (uint32_t)(body.size() - entryStart);
        std::memcpy(body.data() + sizePos, &es, 4);
    }
    writeLE<uint32_t>(body, 0);  // footer
    int64_t sto = 16 + (int64_t)body.size();
    std::vector<uint8_t> out;
    writeLE<uint32_t>(out, MAGIC_V41);
    writeLE<uint32_t>(out, f.universe);
    writeLE<int64_t>(out, sto);
    writeBytes(out, body.data(), body.size());
    writeLE<uint32_t>(out, (uint32_t)f.strings.size());
    for (const auto& s : f.strings) {
        writeBytes(out, s.data(), s.size());
        out.push_back(0);
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) { err = "open out"; return false; }
    ofs.write((const char*)out.data(), out.size());
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s appinfo.vdf\n", argv[0]); return 2; }
    std::string err;
    AppInfoFile f;
    if (!readV41(argv[1], f, err)) {
        std::fprintf(stderr, "read failed: %s\n", err.c_str()); return 1;
    }
    std::printf("Read OK: %zu apps, %zu strings, universe=%u\n",
                f.apps.size(), f.strings.size(), f.universe);
    const std::string out = "/tmp/appinfo_out.vdf";
    if (!writeV41(out, f, err)) {
        std::fprintf(stderr, "write failed: %s\n", err.c_str()); return 1;
    }
    std::printf("Wrote %s\n", out.c_str());

    // Diff
    std::ifstream a(argv[1], std::ios::binary | std::ios::ate);
    std::ifstream b(out, std::ios::binary | std::ios::ate);
    auto sa = (size_t)a.tellg(), sb = (size_t)b.tellg();
    if (sa != sb) {
        std::printf("Size mismatch: input=%zu output=%zu (delta=%zd)\n", sa, sb, (ssize_t)sb - (ssize_t)sa);
    }
    a.seekg(0); b.seekg(0);
    std::vector<uint8_t> ba(sa), bb(sb);
    a.read((char*)ba.data(), sa);
    b.read((char*)bb.data(), sb);
    size_t firstDiff = std::min(sa, sb);
    for (size_t i = 0; i < std::min(sa, sb); ++i) {
        if (ba[i] != bb[i]) { firstDiff = i; break; }
    }
    if (sa == sb && firstDiff == sa) {
        std::printf("MATCH: byte-identical roundtrip\n");
        return 0;
    }
    std::printf("DIFF: first byte differs at offset %zu (in=%02x out=%02x)\n",
                firstDiff, firstDiff < sa ? ba[firstDiff] : 0,
                firstDiff < sb ? bb[firstDiff] : 0);
    return 1;
}
