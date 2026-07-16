#include "parental.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Parental
{
namespace
{

std::atomic<bool> g_warnedMalformed{ false };

using SettingsReceivedFn = bool(*)(void*, const uint8_t*, uint32_t,
	                               const uint8_t*, uint32_t,
	                               uintptr_t, uintptr_t);

lm_address_t g_receiver = LM_ADDRESS_BAD;
lm_address_t g_trampoline = LM_ADDRESS_BAD;
lm_size_t g_hookSize = 0;
lm_address_t g_signatureCheck = LM_ADDRESS_BAD;
uint8_t g_originalSignatureBranch = 0;
bool g_signaturePatched = false;

bool writeByte(lm_address_t address, uint8_t value)
{
	lm_prot_t oldProt;
	if (!LM_ProtMemory(address, 1, LM_PROT_XRW, &oldProt)) return false;
	const lm_size_t written = LM_WriteMemory(address, &value, 1);
	LM_ProtMemory(address, 1, oldProt, LM_NULL);
	return written == 1 && *reinterpret_cast<const uint8_t*>(address) == value;
}

bool hkSettingsReceived(void* instance,
	                    const uint8_t* settings, uint32_t settingsLen,
	                    const uint8_t* signature, uint32_t signatureLen,
	                    uintptr_t a6, uintptr_t a7)
{
	const auto original = reinterpret_cast<SettingsReceivedFn>(g_trampoline);
	if (!original) return false;
	if (!settings || settingsLen == 0)
	{
		return original(instance, settings, settingsLen, signature, signatureLen,
		                a6, a7);
	}

	auto rewritten = rewriteSettings(settings, settingsLen);
	if (!rewritten || rewritten->size() > std::numeric_limits<uint32_t>::max())
	{
		if (!g_warnedMalformed.exchange(true))
		{
			g_pLog->warn("Parental: received malformed settings; leaving them unchanged\n");
		}
		return original(instance, settings, settingsLen, signature, signatureLen,
		                a6, a7);
	}

	g_pLog->info("Parental: unlocked settings before Steam applied them (%u -> %zu bytes)\n",
	             settingsLen, rewritten->size());
	return original(instance, rewritten->data(),
	                static_cast<uint32_t>(rewritten->size()),
	                signature, signatureLen, a6, a7);
}

} // namespace

void setup()
{
	g_warnedMalformed = false;
	if (!g_config.disableParentalRestrictions.get())
	{
		return;
	}

	const lm_address_t receiver = Patterns::ParentalSettingsReceived.address;
	if (receiver == 0 || receiver == LM_ADDRESS_BAD
	    || !looksLikeSettingsReceiver(reinterpret_cast<const uint8_t*>(receiver), 32))
	{
		g_pLog->warn("Parental: internal settings receiver unavailable; local unlock disabled\n");
		return;
	}

	const lm_address_t check = Patterns::ParentalSignatureCheck.address;
	if (check == 0 || check == LM_ADDRESS_BAD)
	{
		g_pLog->warn("Parental: signature check pattern unavailable; local unlock disabled\n");
		return;
	}

	const auto* bytes = reinterpret_cast<const uint8_t*>(check);
	if (bytes[0] != 0x84 || bytes[1] != 0xc0
	    || (bytes[2] != 0x75 && bytes[2] != 0xeb))
	{
		g_pLog->warn("Parental: signature check has unexpected instructions; local unlock disabled\n");
		return;
	}

	g_signatureCheck = check;
	g_originalSignatureBranch = bytes[2];
	if (bytes[2] == 0x75)
	{
		if (!writeByte(check + 2, 0xeb))
		{
			g_pLog->warn("Parental: could not patch signature branch\n");
			return;
		}
		g_signaturePatched = true;
	}

	g_receiver = receiver;
	g_hookSize = LM_HookCode(g_receiver,
	                        reinterpret_cast<lm_address_t>(&hkSettingsReceived),
	                        &g_trampoline);
	if (!g_hookSize || g_trampoline == 0 || g_trampoline == LM_ADDRESS_BAD)
	{
		if (g_signaturePatched)
		{
			writeByte(g_signatureCheck + 2, g_originalSignatureBranch);
			g_signaturePatched = false;
		}
		g_receiver = LM_ADDRESS_BAD;
		g_trampoline = LM_ADDRESS_BAD;
		g_pLog->warn("Parental: could not hook the internal settings receiver\n");
		return;
	}

	g_pLog->info("Parental: internal pre-cache restrictions unlock enabled\n");
}

void remove()
{
	if (g_hookSize && g_receiver != LM_ADDRESS_BAD && g_trampoline != LM_ADDRESS_BAD)
	{
		LM_UnhookCode(g_receiver, g_trampoline, g_hookSize);
	}
	g_hookSize = 0;
	g_receiver = LM_ADDRESS_BAD;
	g_trampoline = LM_ADDRESS_BAD;

	if (g_signaturePatched && g_signatureCheck != LM_ADDRESS_BAD)
	{
		writeByte(g_signatureCheck + 2, g_originalSignatureBranch);
	}
	g_signaturePatched = false;
	g_signatureCheck = LM_ADDRESS_BAD;
}

} // namespace Parental
