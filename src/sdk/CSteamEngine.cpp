#include "CSteamEngine.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"


CUser* CSteamEngine::getUser(uint32_t index)
{
	const static auto offset = *reinterpret_cast<lm_address_t*>(Patterns::CSteamEngine::Offset_User.address + 0x2);
	const auto ppUserMap = *reinterpret_cast<uint8_t**>(this + offset);

	// The user map is populated asynchronously during early bootstrap.
	// Hooks that fire before login (e.g. LoadPackage for package 0 on a
	// cold cache) can reach getUser(0) while the map pointer is still
	// null; indexing it would deref ~address 4 and segfault.  Bail out
	// so callers fall back to the CheckAppOwnership-captured user.
	if (ppUserMap == nullptr)
	{
		return nullptr;
	}

	const auto ppUser = ppUserMap + index * 8;

	return *reinterpret_cast<CUser**>(ppUser + 4);
}

void CSteamEngine::setAppIdForCurrentPipe(uint32_t appId)
{
	//Last argument needs to be 0, otherwise steam crashes.
	//Might be only 1 when steam first sets it, then 0
	Hooks::CSteamEngine_SetAppIdForCurrentPipe.tramp.fn(this, appId, 0);
}

CSteamEngine* g_pSteamEngine = nullptr;
CUser* g_pLocalUser = nullptr;

CUser* getLocalUser()
{
	// Prefer the engine-resolved user when the Init hook caught the
	// engine pointer.  Otherwise fall back to the user captured from
	// CheckAppOwnership.  Both point at the same pipe-0 CUser.
	if (g_pSteamEngine != nullptr)
	{
		CUser* user = g_pSteamEngine->getUser(0);
		if (user != nullptr)
		{
			return user;
		}
	}

	return g_pLocalUser;
}
