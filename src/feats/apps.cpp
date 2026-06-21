#include "apps.hpp"

#include "../sdk/CAppOwnershipInfo.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/EReleaseState.hpp"
#include "../sdk/IClientApps.hpp"
#include "../sdk/IClientAppManager.hpp"

#include "../config.hpp"
#include "../globals.hpp"

#include "fakeappid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace
{
	// --- pin-aware update suppression helpers (apps.cpp-local) -------------
	//
	// A locked, pinned app must let Steam run the update ONCE to install the
	// pinned build, then freeze.  If we suppress unconditionally Steam never
	// applies the pin (it stays on the installed/public build); if we never
	// suppress Steam loops forever reconciling the pinned gid against appinfo's
	// public gid.  So suppress IFF the app's installed depots already match its
	// pins.  That needs the installed manifest gids, read from the app's
	// appmanifest_<appId>.acf across all Steam library folders.

	std::string steamRootForManifests()
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};
		const std::string roots[] = {
			std::string(home) + "/.steam/steam",
			std::string(home) + "/.steam/debian-installation",
			std::string(home) + "/.local/share/Steam",
		};
		for (const auto& r : roots)
		{
			struct stat st{};
			if (stat((r + "/steamapps/libraryfolders.vdf").c_str(), &st) == 0)
				return r;
		}
		return {};
	}

	// All library steamapps dirs (main root + every "path" in libraryfolders).
	std::vector<std::string> librarySteamappsDirs()
	{
		std::vector<std::string> dirs;
		const std::string root = steamRootForManifests();
		if (root.empty()) return dirs;
		dirs.push_back(root + "/steamapps");

		std::ifstream f(root + "/steamapps/libraryfolders.vdf");
		if (!f) return dirs;
		std::string line;
		while (std::getline(f, line))
		{
			// "path"  "/some/library"
			const auto k = line.find("\"path\"");
			if (k == std::string::npos) continue;
			const auto q1 = line.find('"', k + 6);
			if (q1 == std::string::npos) continue;
			const auto q2 = line.find('"', q1 + 1);
			if (q2 == std::string::npos) continue;
			dirs.push_back(line.substr(q1 + 1, q2 - q1 - 1) + "/steamapps");
		}
		return dirs;
	}

	std::string findAppManifestPath(uint32_t appId)
	{
		const std::string name = "/appmanifest_" + std::to_string(appId) + ".acf";
		for (const auto& d : librarySteamappsDirs())
		{
			const std::string p = d + name;
			struct stat st{};
			if (stat(p.c_str(), &st) == 0) return p;
		}
		return {};
	}

	// depot -> installed manifest gid, parsed from the .acf InstalledDepots
	// block.  Each depot block opens with `"<depot>" {` and (per Steam's
	// writer) lists `"manifest" "<gid>"` first; no nested braces inside.
	std::unordered_map<uint32_t, uint64_t> installedDepotGids(uint32_t appId)
	{
		std::unordered_map<uint32_t, uint64_t> out;
		const std::string path = findAppManifestPath(appId);
		if (path.empty()) return out;

		std::ifstream f(path);
		if (!f) return out;
		std::stringstream ss;
		ss << f.rdbuf();
		const std::string s = ss.str();

		const auto idStart = s.find("\"InstalledDepots\"");
		if (idStart == std::string::npos) return out;
		// Scope to the InstalledDepots block: from its '{' to the matching '}'.
		auto pos = s.find('{', idStart);
		if (pos == std::string::npos) return out;
		int depth = 1;
		size_t i = pos + 1;
		uint32_t curDepot = 0;
		while (i < s.size() && depth > 0)
		{
			const char c = s[i];
			if (c == '{') { ++depth; ++i; continue; }
			if (c == '}') { --depth; ++i; continue; }
			if (c == '"')
			{
				const auto end = s.find('"', i + 1);
				if (end == std::string::npos) break;
				const std::string tok = s.substr(i + 1, end - i - 1);
				i = end + 1;
				if (depth == 1)
				{
					// depot id key
					try { curDepot = static_cast<uint32_t>(std::stoul(tok)); }
					catch (...) { curDepot = 0; }
				}
				else if (depth == 2 && tok == "manifest" && curDepot)
				{
					const auto v1 = s.find('"', i);
					if (v1 == std::string::npos) break;
					const auto v2 = s.find('"', v1 + 1);
					if (v2 == std::string::npos) break;
					try {
						out[curDepot] =
						    std::stoull(s.substr(v1 + 1, v2 - v1 - 1));
					} catch (...) {}
					i = v2 + 1;
				}
				continue;
			}
			++i;
		}
		return out;
	}

	// True iff every pinned depot of `appId` is installed at its pinned gid.
	bool appAtPinnedGids(uint32_t appId)
	{
		const auto pins = g_config.getAppPinnedDepots(appId);
		if (pins.empty()) return false;
		const auto installed = installedDepotGids(appId);
		if (installed.empty()) return false;
		for (const auto& [depot, gid] : pins)
		{
			const auto it = installed.find(depot);
			if (it == installed.end() || it->second != gid) return false;
		}
		return true;
	}
}

bool Apps::applistRequested;
std::map<uint32_t, int> Apps::appIdOwnerOverride;

bool Apps::unlockApp(uint32_t appId, CAppOwnershipInfo* info, uint32_t ownerId)
{
	info->owner = ownerId;
	info->realOwner = 0;
	info->familyShared = ownerId != g_currentSteamId;

	info->licensePermanent = !info->familyShared;
	info->retailLicense = false;
	info->licenseExpired = false;
	info->licensePending = false;
	info->licenseLocked = false;

	info->releaseState = ERELEASESTATE_RELEASED;
	info->ownsLicense = true;

	info->lowViolence = false;
	info->regionRestricted = false;

	info->autoGrant = false;
	info->trialTime = 0;
	info->fromFreeWeekend = false;
	info->freeLicense = info->familyShared;
	info->siteLicense = false;

	g_pLog->infoOnce("Unlocked %u\n", appId);
	return true;
}

bool Apps::unlockApp(uint32_t appId, CAppOwnershipInfo* info)
{
	return unlockApp(appId, info, g_currentSteamId);
}

bool Apps::checkAppOwnership(uint32_t appId, CAppOwnershipInfo* pInfo)
{
	if (!applistRequested || !pInfo || !g_currentSteamId)
	{
		return false;
	}

	const uint32_t denuvoOwner = g_config.getDenuvoGameOwner(appId);

	if (denuvoOwner && denuvoOwner != g_currentSteamId)
	{
		g_pLog->infoOnce("Skipping %u because it's a Denuvo game from someone else\n", appId);
		return false;
	}

	if (g_config.shouldExcludeAppId(appId))
	{
		return false;
	}

	if (pInfo->lowViolence)
	{
		pInfo->lowViolence = false;
		g_pLog->infoOnce("Decensoring %u\n", appId);
	}
	if (pInfo->regionRestricted)
	{
		pInfo->regionRestricted = false;
		g_pLog->infoOnce("Bypassing region restriction for %u\n", appId);
	}

	const auto times = g_config.subscriptionTimestamps.get();
	if (times.contains(appId))
	{
		pInfo->purchaseTime = times.at(appId);
	}

	const bool manualUnlock = g_config.isAddedAppId(appId);
	if (!manualUnlock && (!g_config.playNotOwnedGames.get() || pInfo->ownsLicense))
	{
		return false;
	}

	if (!manualUnlock && g_config.automaticFilter.get())
	{
		if (!g_pClientApps)
		{
			return false;
		}

		auto type = g_pClientApps->getAppType(appId);
		if (type == APPTYPE_DLC) //Don't touch DLC here, otherwise downloads might break. Hopefully this won't decrease compatibility
		{
			return false;
		}

		switch(type)
		{
			case APPTYPE_APPLICATION:
			case APPTYPE_GAME:
				break;

			default:
				return false;
		}
	}

	unlockApp(appId, pInfo);

	return true;
}

void Apps::getSubscribedApps(uint32_t* appList, size_t size, uint32_t& count)
{
	if (!size || !appList)
	{
		count = count + g_config.addedAppIds.get().size();
		return;
	}

	for(auto& appId : g_config.addedAppIds.get())
	{
		appList[count++] = appId;
	}

	applistRequested = true;
}

bool Apps::shouldDisableCloud(uint32_t appId)
{
	if (!g_config.disableCloud.get())
	{
		return false;
	}

	// AdditionalApps are injected into package 0 so Steam treats them as
	// owned — which means isSubscribed() returns true for them.  Cloud
	// saves still can't sync: Valve's cloud backend validates ownership
	// server-side and rejects the upload with "Access Denied" (visible in
	// cloud_log.txt).  Disable cloud for AddedApps explicitly so Steam
	// doesn't attempt the doomed sync and surface a cloud error to the
	// user; the isSubscribed() check below would otherwise be defeated by
	// our own ownership injection.
	if (g_config.isAddedAppId(appId))
	{
		return true;
	}

	CUser* user = getLocalUser();
	if (user == nullptr)
	{
		return false;
	}
	return !user->isSubscribed(appId);
}

bool Apps::shouldDisableCDKey(uint32_t appId)
{
	CUser* user = getLocalUser();
	if (user == nullptr)
	{
		return false;
	}
	return !user->isSubscribed(appId);
}

bool Apps::shouldDisableUpdates(uint32_t appId)
{
	const bool added = g_config.isAddedAppId(appId);
	if (!added)
	{
		CUser* user = getLocalUser();
		if (user == nullptr)
		{
			return false;
		}
		return !user->isSubscribed(appId);
	}

	// For AdditionalApps we want to suppress UPDATES (so Steam doesn't
	// re-fetch and overwrite the staged build) — but NOT suppress the
	// initial INSTALL.  Returning false from GetUpdateInfo
	// unconditionally made Steam think a not-yet-installed AddedApp had
	// "nothing to download", so the install hung in "Reconfiguring"
	// and got Suspended.  Only disable updates once the app is already
	// fully installed; while it's uninstalled / update-required, let
	// the real update info through so the download proceeds.
	if (g_pClientAppManager != nullptr)
	{
		const EAppState state = g_pClientAppManager->getAppInstallState(appId);
		if (!(state & APPSTATE_FULLY_INSTALLED))
		{
			return false;  // allow the install/download to start
		}
	}

	// Locked apps freeze on their pinned build.  But suppressing updates
	// UNCONDITIONALLY means Steam never installs the pinned build in the first
	// place (it stays on whatever is installed); allowing them unconditionally
	// makes Steam loop forever reconciling the pinned depot gid against
	// appinfo's public gid (commit pinned -> "Update Required" -> re-plan ->
	// commit -> ...).  Resolve both: suppress IFF the app's installed depots
	// already match its pins.  Not-yet-pinned -> allow the ONE downgrade to
	// run; once installed==pinned -> suppress so it freezes without looping.
	if (!g_config.isAppLocked(appId))
	{
		return false;  // unlocked AddedApp: updates enabled (grab latest)
	}

	const bool atPinned = appAtPinnedGids(appId);
	g_pLog->infoOnce("Pin-lock %u: installed%s at pinned build -> updates %s\n",
	                 appId, atPinned ? "" : " NOT",
	                 atPinned ? "frozen" : "allowed (apply pin)");
	return atPinned;
}

void Apps::sendGamesPlayed(CMsgClientGamesPlayed* msg)
{
	auto titles = g_config.gameTitles.get();
	bool owned = false;

	for(int i = 0; i < msg->games_played_size(); i++)
	{
		auto game = CMsgClientGamesPlayed_GamePlayed(msg->games_played(i));

		if (!game.game_id())
		{
			continue;
		}

		if(!owned)
		{
			CUser* user = getLocalUser();
			if (user != nullptr && user->isSubscribed(game.game_id()))
			{
				owned = true;
			}
		}

		if (g_config.disableFamilyLock.get())
		{
			game.set_owner_id(1);
		}

		if (titles.contains(game.game_id()))
		{
			game.set_game_extra_info(titles[game.game_id()]);
		}
		else if (!owned || FakeAppIds::getFakeAppId(game.game_id()))
		{
			char name[256] {}; //No clue how long titles can get
			if (g_pClientApps)
			{
				g_pClientApps->getAppData(game.game_id(), "common/name", name, sizeof(name));
				g_pLog->debug("AppName %s\n", name);
				game.set_game_extra_info(name);
			}
		}

		msg->mutable_games_played(i)->ParseFromString(game.SerializeAsString());

		g_pLog->debug("Playing game %llu with flags %u & pid %u\n", game.game_id(), game.game_flags(), game.process_id());
	}

	if (owned || msg->games_played_size() > 0)
	{
		return;
	}

	const auto statusApp = g_config.idleStatus.get();
	if (statusApp.appId)
	{
		auto game = msg->add_games_played();
		game->set_game_id(statusApp.appId);
		game->set_game_extra_info(statusApp.title);
		game->set_game_flags(0);

		if (g_config.disableFamilyLock.get())
		{
			game->set_owner_id(1);
		}
	}
}

void Apps::sendPICSInfoRequest(CMsgClientPICSProductInfoRequest* msg)
{
	const auto tokens = g_config.appTokens.get();

	// We intentionally do NOT add AdditionalApps to Steam's outgoing PICS
	// product-info requests.  This mirrors the upstream LumaCore design:
	// ownership is established purely by the package-0 AppIdVec injection in
	// PackagePatch plus the CheckAppOwnership patch, and Steam fetches the
	// product info for those apps through its own normal request/response
	// handshake.  Injecting appids here (or forcing meta_data_only=false)
	// makes Steam follow up forever for buffers it never asked for, which
	// hangs the client at "Loading user data".  We only attach an access
	// token to apps Steam is ALREADY asking about, so the CM returns a real
	// product-info buffer for them.
	for (int i = 0; i < msg->apps_size(); i++)
	{
		auto app = msg->mutable_apps(i);
		if (tokens.contains(app->appid()))
		{
			app->set_access_token(tokens.at(app->appid()));
			g_pLog->debug("PICS-request: attached access token for %u\n", app->appid());
		}
	}

	std::stringstream sentIds;
	for (int i = 0; i < msg->apps_size(); ++i)
	{
		if (i) sentIds << ',';
		sentIds << msg->apps(i).appid();
	}
	g_pLog->debug("PICS-request: apps=%d packages=%d ids=[%s]\n",
	              msg->apps_size(), msg->packages_size(), sentIds.str().c_str());
}

void Apps::sendMsg(CProtoBufMsgBase *msg)
{
	switch(msg->type)
	{
		case EMSG_PICS_PRODUCTINFO_REQUEST:
			sendPICSInfoRequest(msg->getBody<CMsgClientPICSProductInfoRequest>());
			break;

		case EMSG_GAMESPLAYED:
		case EMSG_GAMESPLAYED_NO_DATABLOB:
		case EMSG_GAMESPLAYED_WITH_DATABLOB:
			sendGamesPlayed(msg->getBody<CMsgClientGamesPlayed>());
			break;
	}
}
