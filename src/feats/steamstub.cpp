// SPDX-License-Identifier: AGPL-3.0-only
//
// See steamstub.hpp for the design notes.

#include "steamstub.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>


namespace
{
	// 4-byte signature at file offset 0x40 in Steam-Stub-wrapped PE
	// files.  Identifies the wrapper before we pay the Wine startup
	// cost.  Big-endian here because we read raw bytes from disk.
	constexpr uint8_t kVlvSig[4] = { 'V', 'L', 'V', 0x00 };
	constexpr off_t   kVlvSigOff = 0x40;

	std::string g_helperScript;        // run-steamless.sh
	std::string g_steamlessHome;       // dir containing Steamless.CLI.exe
	std::atomic<bool> g_enabled{false};

	std::mutex g_processedMu;
	std::unordered_set<std::string> g_processedExes;

	// Returns true when the file at `path` carries the VLV signature
	// at offset 0x40.  Returns false on any I/O error or short reads
	// — stub-detection is best-effort and a missed positive only
	// costs the user the wine round-trip with an early exit.
	bool fileHasVlvSig(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f.is_open()) return false;
		f.seekg(kVlvSigOff, std::ios::beg);
		uint8_t buf[4] = {};
		f.read(reinterpret_cast<char*>(buf), sizeof(buf));
		if (f.gcount() != static_cast<std::streamsize>(sizeof(buf))) return false;
		return std::memcmp(buf, kVlvSig, sizeof(buf)) == 0;
	}

	// Resolve the install dir for `appId` from the Steam appmanifest.
	// We go through the appmanifest rather than IClientAppManager
	// because the manifest is the canonical source — and we don't
	// want to introduce another vfunc dependency that could break on
	// Steam updates.  Walks every Steam library:
	//   1. Default install:  ~/.steam/<distro>/steamapps/
	//   2. libraryfolders.vdf-listed external library paths.
	std::string findInstallDir(uint32_t appId)
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};

		// Probe a handful of well-known Steam roots.  This list
		// matches what feats/depotkey.cpp uses so the behaviour stays
		// consistent across the codebase.
		static const char* steamRoots[] = {
			"/.steam/steam",
			"/.steam/debian-installation",
			"/.local/share/Steam",
		};

		std::vector<std::filesystem::path> libraryRoots;
		for (const char* suffix : steamRoots)
		{
			auto root = std::filesystem::path(home) / (std::string(suffix).substr(1));
			if (std::filesystem::exists(root))
			{
				libraryRoots.push_back(root / "steamapps");
				// External libraries from libraryfolders.vdf
				const auto libVdf = root / "steamapps" / "libraryfolders.vdf";
				if (std::filesystem::exists(libVdf))
				{
					std::ifstream f(libVdf);
					std::string line;
					while (std::getline(f, line))
					{
						// Crude but effective: any "path" key value.
						const auto pos = line.find("\"path\"");
						if (pos == std::string::npos) continue;
						const auto qOpen = line.find('"', pos + 6);
						if (qOpen == std::string::npos) continue;
						const auto qClose = line.find('"', qOpen + 1);
						if (qClose == std::string::npos) continue;
						auto extra = line.substr(qOpen + 1, qClose - qOpen - 1);
						libraryRoots.push_back(std::filesystem::path(extra) / "steamapps");
					}
				}
			}
		}

		for (const auto& libroot : libraryRoots)
		{
			const auto manifest = libroot / ("appmanifest_" + std::to_string(appId) + ".acf");
			if (!std::filesystem::exists(manifest)) continue;

			std::ifstream f(manifest);
			std::string line;
			std::string installdir;
			while (std::getline(f, line))
			{
				const auto pos = line.find("\"installdir\"");
				if (pos == std::string::npos) continue;
				const auto qOpen = line.find('"', pos + 12);
				if (qOpen == std::string::npos) continue;
				const auto qClose = line.find('"', qOpen + 1);
				if (qClose == std::string::npos) continue;
				installdir = line.substr(qOpen + 1, qClose - qOpen - 1);
				break;
			}
			if (!installdir.empty())
			{
				return (libroot / "common" / installdir).string();
			}
		}
		return {};
	}

	// Run `<script> <exePath>` synchronously with the STEAMLESS_HOME
	// env var pointing at our bundled binaries.  Returns the helper's
	// exit code, or -1 on spawn failure.
	int runHelper(const std::string& exePath)
	{
		const pid_t pid = fork();
		if (pid < 0)
		{
			g_pLog->warn("SteamStub: fork() failed (errno=%d)\n", errno);
			return -1;
		}
		if (pid == 0)
		{
			// Child.  Set STEAMLESS_HOME, exec the helper.
			setenv("STEAMLESS_HOME", g_steamlessHome.c_str(), 1);
			// QUIET=1 keeps the SLSsteam log clean of Steamless's
			// banner; the helper still logs its own one-liner per
			// step plus errors via stderr.
			setenv("QUIET", "1", 1);
			execlp("/bin/bash", "bash", g_helperScript.c_str(),
			       exePath.c_str(), nullptr);
			// If exec returns we are in a wedged state; bail loudly.
			_exit(127);
		}

		// Parent.  Wait synchronously — we want the unpacked exe in
		// place before LaunchApp returns and Proton starts.
		int status = 0;
		if (waitpid(pid, &status, 0) < 0)
		{
			g_pLog->warn("SteamStub: waitpid failed (errno=%d)\n", errno);
			return -1;
		}
		if (WIFEXITED(status))
		{
			return WEXITSTATUS(status);
		}
		g_pLog->warn("SteamStub: helper terminated abnormally (status=%d)\n", status);
		return -1;
	}
}

namespace SteamStub
{

void setup(const char* installRoot)
{
	if (!installRoot || !installRoot[0])
	{
		g_pLog->debug("SteamStub: install root missing; feature disabled\n");
		return;
	}

	std::filesystem::path root(installRoot);

	// Helper script and binary kit live under tools/. Probe two
	// locations for both:
	//   1. Next to SLSsteam.so (dev tree, bundled releases).
	//   2. User-local mirror under ~/.local/share/SLSsteam/ for
	//      system-packaged installs where the .so is read-only.
	const std::filesystem::path scriptCandidates[] = {
		root / "tools" / "steamstub-bypass" / "run-steamless.sh",
		std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/")
			/ ".local" / "share" / "SLSsteam" / "steamstub-bypass"
			/ "run-steamless.sh",
	};
	const std::filesystem::path binCandidates[] = {
		root / "tools" / "steamless-bin",
		std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/")
			/ ".local" / "share" / "SLSsteam" / "steamless-bin",
	};

	bool haveScript = false;
	for (const auto& cand : scriptCandidates)
	{
		if (std::filesystem::exists(cand))
		{
			g_helperScript = cand.string();
			haveScript = true;
			break;
		}
	}
	bool haveBinary = false;
	for (const auto& cand : binCandidates)
	{
		if (std::filesystem::exists(cand / "Steamless.CLI.exe"))
		{
			g_steamlessHome = cand.string();
			haveBinary = true;
			break;
		}
	}

	if (!haveScript || !haveBinary)
	{
		g_pLog->debug
		(
			"SteamStub: helper missing (script=%d binary=%d, root=%s); "
			"feature disabled — run tools/steamstub-bypass/install-steamless.sh "
			"to enable\n",
			haveScript, haveBinary, root.c_str()
		);
		return;
	}

	g_enabled.store(true, std::memory_order_release);
	g_pLog->debug
	(
		"SteamStub: enabled (script=%s, steamless=%s)\n",
		g_helperScript.c_str(), g_steamlessHome.c_str()
	);
}

void onLaunchApp(uint32_t appId)
{
	if (!g_enabled.load(std::memory_order_acquire)) return;
	if (!appId) return;
	if (!g_config.isAddedAppId(appId)) return;

	const auto installDir = findInstallDir(appId);
	if (installDir.empty() || !std::filesystem::exists(installDir))
	{
		g_pLog->debug("SteamStub: no install dir resolved for %u; skip\n", appId);
		return;
	}

	g_pLog->debug("SteamStub: scanning %s for %u\n", installDir.c_str(), appId);

	// Walk a small recursion (most stub-wrapped exes live at the
	// install dir root, but a few games tuck them in subdirs like
	// Bin/ or Binaries/).  Cap depth to avoid runaway.
	for (auto it = std::filesystem::recursive_directory_iterator(installDir);
	     it != std::filesystem::recursive_directory_iterator();
	     ++it)
	{
		if (it.depth() > 3)
		{
			it.disable_recursion_pending();
			continue;
		}
		if (!it->is_regular_file()) continue;

		const auto& p = it->path();
		// Cheap extension filter before opening the file.
		auto ext = p.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c){ return std::tolower(c); });
		if (ext != ".exe") continue;

		const auto pathStr = p.string();

		// Marker file lets us skip already-processed exes without
		// re-reading them on every launch.
		if (std::filesystem::exists(pathStr + ".steamless_done"))
		{
			continue;
		}

		// Avoid redoing the same exe twice in one Steam session
		// (e.g. user clicks Stop then Play again).
		{
			std::lock_guard<std::mutex> lk(g_processedMu);
			if (g_processedExes.count(pathStr))
			{
				continue;
			}
		}

		if (!fileHasVlvSig(pathStr))
		{
			continue;
		}

		g_pLog->info("SteamStub: stripping %s\n", pathStr.c_str());
		const int rc = runHelper(pathStr);
		switch (rc)
		{
			case 0:
				g_pLog->info("SteamStub: stripped successfully (%s)\n", pathStr.c_str());
				break;
			case 2:
				// Already naked — race between sig check and helper
				// invocation.  Harmless.
				g_pLog->debug("SteamStub: helper reported already-naked (%s)\n", pathStr.c_str());
				break;
			default:
				g_pLog->warn
				(
					"SteamStub: helper failed for %s (rc=%d); launch may fail at error 6\n",
					pathStr.c_str(), rc
				);
				break;
		}

		std::lock_guard<std::mutex> lk(g_processedMu);
		g_processedExes.insert(pathStr);
	}
}

}  // namespace SteamStub
