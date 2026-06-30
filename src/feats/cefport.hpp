#pragma once

// Pure, side-effect-light helpers for the CEF debug-port rewrite shim.
//
// Steam's client launches the CEF webhelper with a HARD-CODED
// `--remote-debugging-port=8080` (the value of the .cef-enable-remote-debugging
// flag file is ignored; verified on the Zorin VM). That squats on TCP 8080,
// which collides with common dev servers. We can't change it by editing Steam's
// launcher scripts — Steam restores them from bootstrap on every boot — so the
// shim interposes the exec family inside the client and rewrites the argument
// in flight to a free loopback port, then publishes the chosen port to a
// contract file the Lumen sidecar reads.
//
// This header holds the parts worth unit-testing in isolation:
//   - rewritePortArg : the string surgery on one argv element
//   - isBindable / pickFreePort / resolveSessionPort : free-port selection
// The actual exec interposition lives in main.cpp.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace CefPort
{
	inline constexpr const char* kSwitchPrefix = "--remote-debugging-port=";

	// Replace the digit run after every "--remote-debugging-port=" occurrence
	// in `arg` with `port`. Returns {rewritten, changed}. Matches by prefix
	// (not the literal 8080) so a Steam build defaulting to another port is
	// still caught, and is idempotent when the value already equals `port`.
	// Handles both a bare argv element and the switch embedded inside a
	// `sh -c "exec ... '--remote-debugging-port=8080' ..."` wrapper string.
	inline std::pair<std::string, bool> rewritePortArg(const std::string& arg, uint16_t port)
	{
		const std::string prefix = kSwitchPrefix;
		const std::string portStr = std::to_string(port);
		std::string out;
		out.reserve(arg.size() + 8);
		bool changed = false;

		size_t i = 0;
		while (i < arg.size())
		{
			if (arg.compare(i, prefix.size(), prefix) == 0)
			{
				out += prefix;
				i += prefix.size();

				size_t start = i;
				while (i < arg.size() && std::isdigit(static_cast<unsigned char>(arg[i])))
				{
					++i;
				}

				if (i > start)
				{
					out += portStr;
					changed = true;
				}
				// No digits after the prefix: leave as-is (prefix already copied).
			}
			else
			{
				out += arg[i];
				++i;
			}
		}

		return {out, changed};
	}

	// True if 127.0.0.1:port can be bound right now (plain bind, no REUSEADDR,
	// to mirror how the webhelper itself binds).
	inline bool isBindable(uint16_t port)
	{
		if (port == 0)
		{
			return false;
		}

		const int s = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		{
			return false;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port);

		const bool ok = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
		::close(s);
		return ok;
	}

	// Ask the kernel for a free loopback port (bind :0). Returns 0 on failure.
	inline uint16_t pickFreePort()
	{
		const int s = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		{
			return 0;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;

		uint16_t port = 0;
		if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
		{
			socklen_t len = sizeof(addr);
			if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
			{
				port = ntohs(addr.sin_port);
			}
		}

		::close(s);
		return port;
	}

	inline uint16_t readPortFile(const std::string& path)
	{
		std::ifstream f(path);
		if (!f)
		{
			return 0;
		}
		long v = 0;
		f >> v;
		if (v < 1024 || v > 65535)
		{
			return 0;
		}
		return static_cast<uint16_t>(v);
	}

	inline bool writePortFile(const std::string& path, uint16_t port)
	{
		std::error_code ec;
		const auto parent = std::filesystem::path(path).parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec); // best effort
		}
		std::ofstream f(path, std::ios::trunc);
		if (!f)
		{
			return false;
		}
		f << port << "\n";
		return static_cast<bool>(f);
	}

	// Decide the port to use this session WITHOUT persisting it: reuse the
	// file's port if it is a valid, currently-bindable port; otherwise pick a
	// fresh free port. Returns 0 only if no port could be obtained at all.
	//
	// This is the variant the client uses at la_preinit (setup()) to choose the
	// session port early, WITHOUT touching the contract file. The file is
	// written later, only when this client tree actually launches the webhelper
	// (see main.cpp). That matters at login autostart, where two Steam instances
	// can start concurrently: both run setup(), but only the one that wins the
	// single-instance race goes on to spawn a webhelper. If setup() itself wrote
	// the contract, the LOSING instance (which picks a different free port, then
	// exits before spawning anything) would clobber it with a port nothing ends
	// up listening on, and the Lumen sidecar would connect to a dead port.
	inline uint16_t resolveSessionPortNoPersist(const std::string& path)
	{
		const uint16_t existing = readPortFile(path);
		if (existing != 0 && isBindable(existing))
		{
			return existing;
		}
		return pickFreePort();
	}

	// Resolve the port to use this session: reuse the file's port if it is a
	// valid, currently-bindable port; otherwise pick a fresh free port and
	// persist it. Returns 0 only if no port could be obtained at all (caller
	// then leaves the argv untouched -> Steam keeps 8080).
	inline uint16_t resolveSessionPort(const std::string& path)
	{
		const uint16_t existing = readPortFile(path);
		if (existing != 0 && isBindable(existing))
		{
			return existing;
		}

		const uint16_t fresh = pickFreePort();
		if (fresh != 0)
		{
			writePortFile(path, fresh);
		}
		return fresh;
	}

	// Contract file shared with the Lumen sidecar:
	// $HOME/.local/share/Lumen/cef_port  ("" if HOME is unset).
	inline std::string contractPath()
	{
		const char* home = std::getenv("HOME");
		if (!home || !*home)
		{
			return "";
		}
		return std::string(home) + "/.local/share/Lumen/cef_port";
	}
}
