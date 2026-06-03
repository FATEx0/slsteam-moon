#pragma once

#include "EResult.hpp"

#include <cstdint>

class CAppOwnershipInfo;

enum class ECallbackType : uint32_t
{
	LicensesUpdate_t = 0x7d,
	AppOwnershipTicketReceived_t = 0xf907c,
};

struct AppOwnershipTicketReceived_t
{
	EResult result;
	uint32_t appId;
};

class CUser
{
public:
	bool checkAppOwnership(uint32_t appId, CAppOwnershipInfo* pInfo);
	bool isSubscribed(uint32_t appId);

	void postCallback(ECallbackType type, void* pCallback, uint32_t callbackSize);
	void updateAppOwnershipTicket(uint32_t appId, void* pTicket, uint32_t len);

	// Broadcast a LicensesUpdated_t callback rebuilt from this user's
	// current license state.  Used after injecting AdditionalApps into
	// package 0 to force Steam's ownership/library layer to re-read
	// licenses (and package 0).  No-op if the underlying pattern did
	// not resolve.  Returns true if the call was made.
	bool notifyLicensesUpdated();
};
