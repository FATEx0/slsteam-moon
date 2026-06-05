// SPDX-License-Identifier: AGPL-3.0-only

#include "prewarm.hpp"

#include "depotkey.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"

#include "../utils/ManifestFetch.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Prewarm
{

namespace
{

// Only ever flips false -> true.  The background worker runs for the rest
// of the Steam session, so once it is up we never start a second one.
std::atomic<bool> g_started{false};

std::string bufferPath(uint32_t appId)
{
	std::stringstream ss;
	ss << g_config.getDir() << "/cache/picsbuffer_" << appId << ".bin";
	return ss.str();
}

// Read an AddedApp's provisioned appinfo buffer (written by
// AppInfoProvision at startup).  Same on-disk path feats/pics.cpp mines
// for synchronous install staging.
std::string readBuffer(uint32_t appId)
{
	const auto path = bufferPath(appId);
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return {};
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (64LL << 20)) return {};
	std::string out;
	out.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	if (!ifs.read(out.data(), sz)) return {};
	return out;
}

void runLoop()
{
	using namespace std::chrono_literals;

	// Re-stage interval: short enough that a post-commit purge is healed
	// before the next planning pass (Steam's own auto-retry is ~30s), yet
	// not a busy loop.  A steady-state pass with everything already on disk
	// is just stat() calls — ManifestFetch's (gid,depotId) dedup returns the
	// cached success future after re-checking the file is still present, and
	// only re-fetches a manifest Steam actually purged.
	constexpr auto kPassInterval = 30s;
	// Small gap between depots so a cold first pass doesn't fire every CDN
	// request at once.  We block on our OWN thread, so this just paces us.
	constexpr auto kPerDepotGap = 200ms;

	for (;;)
	{
		const auto added = g_config.addedAppIds.get();
		if (!added.empty())
		{
			std::vector<std::string> buffers;
			buffers.reserve(added.size());
			for (uint32_t appId : added)
			{
				std::string buf = readBuffer(appId);
				if (!buf.empty())
				{
					buffers.push_back(std::move(buf));
				}
			}

			const auto hasKey = [](uint32_t depotId) {
				return !DepotKey::getCachedKey(depotId).key.empty();
			};
			const auto targets = planStageTargets(buffers, hasKey);

			if (!targets.empty())
			{
				g_pLog->debug(
				    "Prewarm: keeping %zu manifest(s) warm across %zu AddedApp(s)\n",
				    targets.size(), added.size());
			}

			for (const auto& [depotId, gid] : targets)
			{
				// Blocking await ON OUR DEDICATED THREAD serialises the
				// fetches (no thread storm) and reuses ManifestFetch's
				// on-disk re-check: present -> returns instantly, purged ->
				// re-fetched so the next planning pass finds it and skips
				// BYldRequestDepotManifest entirely.
				ManifestFetch::awaitManifestBlob(
				    gid, depotId, ManifestFetch::getTimeoutSec());
				std::this_thread::sleep_for(kPerDepotGap);
			}
		}

		std::this_thread::sleep_for(kPassInterval);
	}
}

} // namespace

void ensureStarted()
{
	bool expected = false;
	if (!g_started.compare_exchange_strong(expected, true))
	{
		return; // already running
	}

	// No AddedApps -> nothing to warm.  Reset the flag so a later call
	// (after config / AddedApps are fully loaded) can still start it.
	if (g_config.addedAppIds.get().empty())
	{
		g_started.store(false);
		return;
	}

	g_pLog->info("Prewarm: starting background manifest pre-warm worker\n");

	// Detached: lives for the Steam session.  MUST only be reached from a
	// real Steam worker thread (the PICS recv path) — never the LD_AUDIT
	// load()/setup() path, which crashed Steam twice (HANDOFF DEAD END #2).
	std::thread(runLoop).detach();
}

} // namespace Prewarm
