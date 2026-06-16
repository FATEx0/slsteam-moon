// Exhaustive unit tests for the CEF debug-port rewrite helpers (CefPort).
//
// The rewrite frees TCP 8080 by changing Steam's hard-coded
// `--remote-debugging-port=8080` to a free loopback port in flight (via
// la_symbind in main.cpp) and publishing it to a contract file Lumen reads.
// These tests cover the parts independent of the exec call:
//   - CefPort::rewritePortArg : argv string surgery (incl. the sh -c wrapper)
//   - CefPort::isBindable / pickFreePort / resolveSessionPort : port selection
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_cefport.cpp -o /tmp/test_cefport && /tmp/test_cefport

#include "../src/feats/cefport.hpp"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		++g_checks;                                                          \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
	} while (0)

static std::string readLine(const std::string& p)
{
	std::ifstream f(p);
	std::string s;
	std::getline(f, s);
	return s;
}

static void test_rewrite()
{
	using CefPort::rewritePortArg;

	// Exact argv element, various port widths.
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=8080", 12345);
		CHECK(c && o == "--remote-debugging-port=12345", "exact element");
	}
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=8080", 1024);
		CHECK(c && o == "--remote-debugging-port=1024", "shorter replacement port");
	}
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=8080", 65535);
		CHECK(c && o == "--remote-debugging-port=65535", "max port");
	}

	// Embedded in the sh -c wrapper string (real Steam layout).
	{
		const std::string in =
			"exec '/x/steamwebhelper.sh' '-nocrashdialog' "
			"'--remote-debugging-port=8080' '--enable-smooth-scrolling'";
		auto [o, c] = rewritePortArg(in, 49777);
		CHECK(c, "sh -c: changed");
		CHECK(o.find("'--remote-debugging-port=49777'") != std::string::npos, "sh -c: digits replaced, quotes intact");
		CHECK(o.find("8080") == std::string::npos, "sh -c: no stale 8080");
		CHECK(o.find("'--enable-smooth-scrolling'") != std::string::npos, "sh -c: neighbours intact");
	}

	// Switch at very start and digits ending the string.
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=8080", 2000);
		CHECK(c && o == "--remote-debugging-port=2000", "digits end the string");
	}
	// Digits followed by a space.
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=8080 --foo", 2000);
		CHECK(c && o == "--remote-debugging-port=2000 --foo", "digits followed by space");
	}
	// Digits followed by a quote.
	{
		auto [o, c] = rewritePortArg("x='--remote-debugging-port=8080'", 2000);
		CHECK(c && o == "x='--remote-debugging-port=2000'", "digits followed by quote");
	}
	// Multiple occurrences in one string -> all rewritten.
	{
		auto [o, c] = rewritePortArg(
			"--remote-debugging-port=8080 then --remote-debugging-port=8080", 4444);
		CHECK(c, "multi: changed");
		CHECK(o == "--remote-debugging-port=4444 then --remote-debugging-port=4444", "multi: all replaced");
	}

	// No occurrence -> unchanged.
	{
		auto [o, c] = rewritePortArg("-cachedir=/home/u/.cache", 12345);
		CHECK(!c && o == "-cachedir=/home/u/.cache", "no switch -> verbatim");
	}
	// Empty string.
	{
		auto [o, c] = rewritePortArg("", 12345);
		CHECK(!c && o.empty(), "empty string -> unchanged");
	}
	// Future-proofing: non-8080 default still matched (prefix match).
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=9090", 22222);
		CHECK(c && o == "--remote-debugging-port=22222", "non-8080 default matched");
	}
	// Idempotent: rewriting to the same value keeps it.
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=49777", 49777);
		CHECK(o == "--remote-debugging-port=49777", "idempotent same-port");
	}
	// Sibling switches must NOT match.
	{
		auto [o, c] = rewritePortArg("--remote-debugging-address=127.0.0.1", 12345);
		CHECK(!c && o == "--remote-debugging-address=127.0.0.1", "address not matched");
	}
	{
		auto [o, c] = rewritePortArg("--remote-debugging-pipe", 12345);
		CHECK(!c, "pipe not matched");
	}
	// Prefix present but no digits after '=' -> not a real port arg, untouched.
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=", 12345);
		CHECK(!c && o == "--remote-debugging-port=", "prefix with no digits untouched");
	}
	{
		auto [o, c] = rewritePortArg("--remote-debugging-port=abc", 12345);
		CHECK(!c && o == "--remote-debugging-port=abc", "prefix with non-digit untouched");
	}
}

static void test_ports()
{
	// pickFreePort -> usable, bindable; isBindable(0) is false.
	{
		const uint16_t p = CefPort::pickFreePort();
		CHECK(p >= 1024, "pickFreePort in unprivileged range");
		CHECK(CefPort::isBindable(p), "pickFreePort is bindable");
		CHECK(!CefPort::isBindable(0), "port 0 is not bindable");
	}

	// isBindable reflects an occupied port: false while held, true after close.
	{
		int s = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
		::bind(s, (sockaddr*)&a, sizeof(a)); ::listen(s, 1);
		socklen_t al = sizeof(a); ::getsockname(s, (sockaddr*)&a, &al);
		const uint16_t held = ntohs(a.sin_port);
		CHECK(!CefPort::isBindable(held), "occupied port not bindable");
		::close(s);
		CHECK(CefPort::isBindable(held), "released port bindable again");
	}

	const std::string tag = std::to_string(getpid());

	// Missing file -> fresh bindable port, persisted.
	{
		const std::string path = "/tmp/cefport_missing_" + tag;
		::unlink(path.c_str());
		const uint16_t p = CefPort::resolveSessionPort(path);
		CHECK(p >= 1024 && CefPort::isBindable(p), "missing-file -> fresh bindable");
		CHECK(readLine(path) == std::to_string(p), "missing-file -> persisted");
		::unlink(path.c_str());
	}

	// Valid free port in file -> reused (session/restart stability).
	{
		const std::string path = "/tmp/cefport_reuse_" + tag;
		const uint16_t free = CefPort::pickFreePort();
		{ std::ofstream(path) << free << "\n"; }
		CHECK(CefPort::resolveSessionPort(path) == free, "valid free port reused");
		::unlink(path.c_str());
	}

	// Garbage / out-of-range in file -> fresh valid port.
	for (const char* bad : { "not-a-port", "70000", "1023", "-5", "" })
	{
		const std::string path = "/tmp/cefport_bad_" + tag;
		{ std::ofstream(path) << bad << "\n"; }
		const uint16_t p = CefPort::resolveSessionPort(path);
		CHECK(p >= 1024 && CefPort::isBindable(p), (std::string("garbage '") + bad + "' -> fresh").c_str());
		::unlink(path.c_str());
	}

	// Occupied port in file -> rotates to a different bindable port.
	{
		const std::string path = "/tmp/cefport_busy_" + tag;
		int s = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
		::bind(s, (sockaddr*)&a, sizeof(a)); ::listen(s, 1);
		socklen_t al = sizeof(a); ::getsockname(s, (sockaddr*)&a, &al);
		const uint16_t busy = ntohs(a.sin_port);
		{ std::ofstream(path) << busy << "\n"; }
		const uint16_t p = CefPort::resolveSessionPort(path);
		CHECK(p != busy && p >= 1024 && CefPort::isBindable(p), "occupied port rotates");
		::close(s);
		::unlink(path.c_str());
	}

	// Contract file in a not-yet-existing directory -> dir created, persisted.
	{
		const std::string dir = "/tmp/cefport_dir_" + tag + "/a/b";
		const std::string path = dir + "/cef_port";
		std::error_code ec; std::filesystem::remove_all("/tmp/cefport_dir_" + tag, ec);
		const uint16_t p = CefPort::resolveSessionPort(path);
		CHECK(p >= 1024, "nested-dir resolve yields a port");
		CHECK(readLine(path) == std::to_string(p), "nested-dir resolve persisted (dir created)");
		std::filesystem::remove_all("/tmp/cefport_dir_" + tag, ec);
	}

	// Empty path (HOME unset case) -> still returns a usable port, no crash,
	// just can't persist.
	{
		const uint16_t p = CefPort::resolveSessionPort("");
		CHECK(p >= 1024 && CefPort::isBindable(p), "empty path -> port without persistence");
	}
}

int main()
{
	test_rewrite();
	test_ports();

	if (g_failures == 0) { std::printf("test_cefport: ALL PASS (%d checks)\n", g_checks); return 0; }
	std::printf("test_cefport: %d/%d CHECK(S) FAILED\n", g_failures, g_checks);
	return 1;
}
