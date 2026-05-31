#include "apps.hpp"

#include "../sdk/CAppOwnershipInfo.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/EReleaseState.hpp"
#include "../sdk/IClientApps.hpp"

#include "../config.hpp"
#include "../globals.hpp"

#include "fakeappid.hpp"

#include <sstream>

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

	return !g_pSteamEngine->getUser(0)->isSubscribed(appId);
}

bool Apps::shouldDisableCDKey(uint32_t appId)
{
	return !g_pSteamEngine->getUser(0)->isSubscribed(appId);
}

bool Apps::shouldDisableUpdates(uint32_t appId)
{
	return g_config.isAddedAppId(appId) || !g_pSteamEngine->getUser(0)->isSubscribed(appId);
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

		if(!owned && g_pSteamEngine->getUser(0)->isSubscribed(game.game_id()))
		{
			owned = true;
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
			g_pClientApps->getAppData(game.game_id(), "common/name", name, sizeof(name));
			g_pLog->debug("AppName %s\n", name);
			game.set_game_extra_info(name);
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
	const auto added = g_config.addedAppIds.get();

	std::unordered_set<uint32_t> alreadyRequested;
	for (int i = 0; i < msg->apps_size(); ++i)
	{
		alreadyRequested.insert(msg->apps(i).appid());
	}

	int injected = 0;
	for (uint32_t appId : added)
	{
		if (!appId)
		{
			g_pLog->debug("PICS-request: skip injection for appId=0\n");
			continue;
		}
		if (alreadyRequested.count(appId))
		{
			g_pLog->debug("PICS-request: skip injection for %u (already in request)\n", appId);
			continue;
		}

		auto* entry = msg->add_apps();
		entry->set_appid(appId);
		if (tokens.contains(appId))
		{
			entry->set_access_token(tokens.at(appId));
		}
		++injected;
		g_pLog->debug("PICS-request: injected %u\n", appId);
	}
	g_pLog->debug("PICS-request: addedAppIds.size=%zu, injected=%d\n", added.size(), injected);
	if (injected > 0)
	{
		g_pLog->debug("PICS-request: injected %d AdditionalApps into outgoing request\n", injected);

		if (msg->meta_data_only())
		{
			msg->set_meta_data_only(false);
			g_pLog->debug("PICS-request: forced meta_data_only=false to get full buffers\n");
		}
	}

	std::stringstream sentIds;
	for (int i = 0; i < msg->apps_size(); ++i)
	{
		if (i) sentIds << ',';
		sentIds << msg->mutable_apps(i)->appid();
	}
	g_pLog->debug("PICS-request: apps=%d packages=%d ids=[%s]\n",
	              msg->apps_size(), msg->packages_size(), sentIds.str().c_str());

	for(int i = 0; i < msg->apps_size(); i++)
	{
		auto app = msg->mutable_apps(i);
		if (tokens.contains(app->appid()))
		{
			app->set_access_token(tokens.at(app->appid()));
			g_pLog->debug("Used access token from config for %u\n", app->appid());
		}
	}
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
