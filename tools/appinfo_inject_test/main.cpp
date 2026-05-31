// Standalone test for the appinfo.vdf injection pipeline.
// Compile: g++ -std=c++20 -O2 main.cpp -lcrypto -o appinfo_inject_test
// Run:     ./appinfo_inject_test /path/to/input.vdf /path/to/picsbuffer.bin <appid> <change_number> <sha_hex>
//
// Produces /tmp/appinfo_patched.vdf.  Validate by re-reading it and
// inspecting the emitted entry for the given appid.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <string>
#include <unordered_map>
#include <vector>

constexpr uint32_t MAGIC_V41 = 0x07564429;

namespace KV {
    constexpr uint8_t ChildObject = 0;
    constexpr uint8_t String      = 1;
    constexpr uint8_t Int32       = 2;
    constexpr uint8_t UInt64      = 7;
    constexpr uint8_t End         = 8;
}

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
    std::unordered_map<std::string, uint32_t> stringIndex;

    uint32_t intern(const std::string& s) {
        auto it = stringIndex.find(s);
        if (it != stringIndex.end()) return it->second;
        const auto idx = (uint32_t)strings.size();
        strings.push_back(s);
        stringIndex.emplace(s, idx);
        return idx;
    }
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
            std::string s((const char*)p, nul - p);
            out.stringIndex.emplace(s, i);
            out.strings.push_back(std::move(s));
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
    writeLE<uint32_t>(body, 0);
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

class TextLexer {
public:
    TextLexer(const char* p, const char* end) : p_(p), end_(end) {}
    enum class Tok { String, OpenBrace, CloseBrace, End };
    Tok next(std::string& out) {
        out.clear();
        skipWs();
        if (p_ >= end_) return Tok::End;
        char c = *p_;
        if (c == '{') { ++p_; return Tok::OpenBrace; }
        if (c == '}') { ++p_; return Tok::CloseBrace; }
        if (c == '"') return readQuoted(out);
        return readBare(out);
    }
private:
    void skipWs() {
        while (p_ < end_) {
            unsigned char c = (unsigned char)*p_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p_; continue; }
            if (c == '/' && p_+1 < end_ && p_[1] == '/') { while (p_ < end_ && *p_ != '\n') ++p_; continue; }
            break;
        }
    }
    Tok readQuoted(std::string& out) {
        ++p_;
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return Tok::String;
            if (c == '\\' && p_ < end_) {
                char e = *p_++;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    default: out += e; break;
                }
                continue;
            }
            out += c;
        }
        return Tok::End;
    }
    Tok readBare(std::string& out) {
        while (p_ < end_) {
            unsigned char c = (unsigned char)*p_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}' || c == '"') break;
            out += *p_++;
        }
        return Tok::String;
    }
    const char* p_;
    const char* end_;
};

void emitString(std::vector<uint8_t>& out, AppInfoFile& f, const std::string& key, const std::string& val) {
    uint32_t idx = f.intern(key);
    out.push_back(KV::String);
    writeLE<uint32_t>(out, idx);
    writeBytes(out, val.data(), val.size());
    out.push_back(0);
}

bool emitObject(TextLexer& lex, AppInfoFile& f, std::vector<uint8_t>& out, std::string& err);

bool emitPair(TextLexer& lex, AppInfoFile& f, const std::string& key,
              std::vector<uint8_t>& out, std::string& err) {
    std::string tok;
    auto t = lex.next(tok);
    if (t == TextLexer::Tok::String) { emitString(out, f, key, tok); return true; }
    if (t == TextLexer::Tok::OpenBrace) {
        uint32_t idx = f.intern(key);
        out.push_back(KV::ChildObject);
        writeLE<uint32_t>(out, idx);
        return emitObject(lex, f, out, err);
    }
    err = "expected value or '{' after key '" + key + "'";
    return false;
}

bool emitObject(TextLexer& lex, AppInfoFile& f, std::vector<uint8_t>& out, std::string& err) {
    std::string tok;
    while (true) {
        auto t = lex.next(tok);
        if (t == TextLexer::Tok::CloseBrace) { out.push_back(KV::End); return true; }
        if (t == TextLexer::Tok::End) { err = "unterminated object"; return false; }
        if (t != TextLexer::Tok::String) { err = "expected key"; return false; }
        if (!emitPair(lex, f, tok, out, err)) return false;
    }
}

bool translateText(const std::string& wire, AppInfoFile& f, std::vector<uint8_t>& out, std::string& err) {
    size_t len = wire.size();
    while (len > 0 && (wire[len-1] == '\0' || wire[len-1] == '\n' || wire[len-1] == ' ' || wire[len-1] == '\t' || wire[len-1] == '\r')) --len;
    TextLexer lex(wire.data(), wire.data() + len);
    std::string topKey;
    auto t = lex.next(topKey);
    if (t != TextLexer::Tok::String) { err = "no top key"; return false; }
    if (!emitPair(lex, f, topKey, out, err)) return false;
    out.push_back(KV::End);  // root sentinel End
    return true;
}

std::string fromHex(const std::string& hex) {
    std::string out;
    if (hex.size() % 2) return out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        int hi = nib(hex[i]), lo = nib(hex[i+1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((char)((hi << 4) | lo));
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s in.vdf pics.bin appid change_number sha_hex\n", argv[0]);
        return 2;
    }
    const std::string inPath = argv[1];
    const std::string picsPath = argv[2];
    uint32_t appid = (uint32_t)std::stoul(argv[3]);
    uint32_t change = (uint32_t)std::stoul(argv[4]);
    std::string sha = fromHex(argv[5]);
    if (sha.size() != 20) { std::fprintf(stderr, "sha must be 40 hex chars\n"); return 2; }

    std::string err;
    AppInfoFile f;
    if (!readV41(inPath, f, err)) { std::fprintf(stderr, "read: %s\n", err.c_str()); return 1; }
    std::printf("Read: %zu apps, %zu strings\n", f.apps.size(), f.strings.size());

    std::ifstream ifs(picsPath, std::ios::binary | std::ios::ate);
    if (!ifs) { std::fprintf(stderr, "open pics\n"); return 1; }
    size_t bsz = (size_t)ifs.tellg();
    ifs.seekg(0);
    std::string wire(bsz, '\0');
    ifs.read(wire.data(), bsz);

    std::vector<uint8_t> indexed;
    if (!translateText(wire, f, indexed, err)) { std::fprintf(stderr, "translate: %s\n", err.c_str()); return 1; }
    std::printf("Translated: wire=%zu bytes -> indexed=%zu bytes (strings now %zu)\n",
                wire.size(), indexed.size(), f.strings.size());

    AppEntry e;
    e.appid = appid;
    e.info_state = 2;
    e.change_number = change;
    std::memcpy(e.sha, sha.data(), 20);
    SHA1((const unsigned char*)indexed.data(), indexed.size(), e.binary_hash);
    e.binary_vdf = std::move(indexed);

    bool replaced = false;
    for (auto& a : f.apps) if (a.appid == appid) { a = std::move(e); replaced = true; break; }
    if (!replaced) f.apps.push_back(std::move(e));
    std::printf("%s entry for app=%u\n", replaced ? "Replaced" : "Appended", appid);

    const std::string out = "/tmp/appinfo_patched.vdf";
    if (!writeV41(out, f, err)) { std::fprintf(stderr, "write: %s\n", err.c_str()); return 1; }
    std::printf("Wrote %s (%zu apps, %zu strings)\n", out.c_str(), f.apps.size(), f.strings.size());

    // Sanity: re-read and verify we can parse our own output
    AppInfoFile f2;
    if (!readV41(out, f2, err)) { std::fprintf(stderr, "re-read: %s\n", err.c_str()); return 1; }
    std::printf("Re-read: %zu apps, %zu strings (round-trip OK)\n", f2.apps.size(), f2.strings.size());

    // Find our entry in the re-read file
    for (const auto& a : f2.apps) {
        if (a.appid == appid) {
            std::printf("Our entry: change=%u binary_vdf_size=%zu\n", a.change_number, a.binary_vdf.size());
            break;
        }
    }
    return 0;
}
