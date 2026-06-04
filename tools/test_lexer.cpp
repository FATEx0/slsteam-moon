#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <iostream>

namespace KV { constexpr uint8_t ChildObject=0, String=1, End=8; }

class TextLexer {
public:
    TextLexer(const char* p, const char* end) : p_(p), end_(end) {}
    enum class Tok { String, OpenBrace, CloseBrace, End };
    Tok next(std::string& out) {
        out.clear(); skipWs();
        if (p_ >= end_) return Tok::End;
        const char c = *p_;
        if (c == '{') { ++p_; return Tok::OpenBrace; }
        if (c == '}') { ++p_; return Tok::CloseBrace; }
        if (c == '"') return readQuoted(out);
        return readBare(out);
    }
private:
    void skipWs() {
        while (p_ < end_) {
            unsigned char c = *p_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p_; continue; }
            if (c == '/' && p_+1 < end_ && p_[1] == '/') { while (p_<end_ && *p_!='\n') ++p_; continue; }
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
                switch (e) { case 'n':out.push_back('\n');break; case 't':out.push_back('\t');break; case 'r':out.push_back('\r');break; case '"':out.push_back('"');break; case '\\':out.push_back('\\');break; default:out.push_back(e); }
                continue;
            }
            out.push_back(c);
        }
        return Tok::End;
    }
    Tok readBare(std::string& out) {
        while (p_ < end_) {
            unsigned char c = *p_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}' || c == '"') break;
            out.push_back(*p_++);
        }
        return Tok::String;
    }
    const char* p_; const char* end_;
};

int main() {
    std::string wire = "\"appinfo\"\n{\n\t\"appid\"\t\t\"638510\"\n\t\"common\"\n\t{\n\t\t\"name\"\t\t\"dotAGE\"\n\t}\n}\n";
    TextLexer lex(wire.data(), wire.data() + wire.size());
    std::string tok;
    while (true) {
        auto t = lex.next(tok);
        if (t == TextLexer::Tok::End) break;
        std::cout << (t==TextLexer::Tok::String?"S ":t==TextLexer::Tok::OpenBrace?"{ ":"} ") << "[" << tok << "] (len=" << tok.size() << ")\n";
    }
}
