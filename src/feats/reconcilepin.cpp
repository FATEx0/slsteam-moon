// SPDX-License-Identifier: AGPL-3.0-only
//
// See reconcilepin.hpp for the design.

#include "reconcilepin.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>


namespace
{
	// EvaluateConfigChanges calling convention (HANDOFF-v2 §6/§7): a global
	// anchor/manager pointer arrives in EAX (BOTH the manager object the body
	// dereferences AND the PIC anchor for its .rodata string leas); the three
	// real args are on the stack.  regparm(1) models exactly that: arg0 -> EAX,
	// the rest -> stack.  Declaring both the hook and the orig-trampoline
	// pointer regparm(1) keeps EAX intact across the call so the relocated
	// prologue's `mov %eax,-0xb0(%ebp)` reads the right anchor.  (Prologue is a
	// plain `push ebp`, not a get_pc_thunk -> no fixPICThunkCall.)
	typedef void* (__attribute__((regparm(1))) *ReconFn_t)(
	    void* mgr, void* ctx, void* a1, void* a2);

	ReconFn_t    g_orig = nullptr;
	lm_address_t g_addr = LM_ADDRESS_BAD;
	lm_address_t g_tramp = LM_ADDRESS_BAD;
	lm_size_t    g_size = 0;

	bool g_pinActive = false;   // SLSSTEAM_PIN_PLANNER: perform the gid rewrite
	bool g_trace = false;       // SLSSTEAM_RECONCILE_TRACE: per-call logging
	std::atomic<int> g_traceBudget{400};

	// ctx layout (confirmed live on build cfe99f0c).
	constexpr size_t kCtxFlagsOff = 0x04;
	constexpr size_t kCtxAppIdOff = 0x08;
	constexpr size_t kCtxDepotPtrOff = 0x78;
	constexpr size_t kCtxDepotCountOff = 0x84;
	// flag bit marking the TARGET/appinfo ctx (vs the ACTIVE/installed ctx).
	constexpr uint32_t kFlagTargetSide = 0x8;
	// DepotEntry: DepotId @ +0, ManifestGid @ +0x8, stride 0x20.
	constexpr size_t kDepotEntryStride = 0x20;
	constexpr size_t kDepotEntryGidOff = 0x08;
	constexpr int32_t kMaxDepots = 512;

	void traceLog(uint32_t appId, uint32_t flags, void* base, int32_t count)
	{
		if (!g_trace) return;
		const bool added = g_config.isAddedAppId(appId);
		const bool locked = g_config.isAppLocked(appId);
		// Locked apps are the ones we're debugging: never let the global
		// budget (exhausted by the startup batch of all apps) hide their
		// reconcile calls, especially mid-loop.  Non-locked apps still
		// respect the budget so the log doesn't flood.
		if (!locked && g_traceBudget.fetch_sub(1) <= 0) return;
		g_pLog->info("ReconcilePin[trace]: app=%u flags=0x%x added=%d locked=%d "
		             "side=%s depots@%p count=%d\n",
		             appId, flags, static_cast<int>(added),
		             static_cast<int>(locked),
		             (flags & kFlagTargetSide) ? "target" : "active",
		             base, count);
		if (base && count > 0 && count <= kMaxDepots)
		{
			const auto* e = reinterpret_cast<const uint8_t*>(base);
			for (int32_t i = 0; i < count; ++i, e += kDepotEntryStride)
			{
				g_pLog->info("ReconcilePin[trace]:   depot=%u gid=%llu pin=%llu\n",
				    *reinterpret_cast<const uint32_t*>(e),
				    static_cast<unsigned long long>(
				        *reinterpret_cast<const uint64_t*>(e + kDepotEntryGidOff)),
				    static_cast<unsigned long long>(
				        g_config.getManifestPin(
				            *reinterpret_cast<const uint32_t*>(e))));
			}
		}
	}

	// The fix: on the TARGET call for a LOCKED app, force each pinned depot's
	// target gid to the pin so the reconcile sees active(pinned)==target(pin)
	// once the downgrade has committed (no loop), while a still-public install
	// (active=public != target=pin) still triggers the one downgrade.
	void applyTargetPin(void* ctxv, uint32_t appId, uint32_t flags)
	{
		if (!g_pinActive) return;
		if (!(flags & kFlagTargetSide)) return;       // target/appinfo side only
		if (!g_config.isAppLocked(appId)) return;

		auto* ctx = reinterpret_cast<uint8_t*>(ctxv);
		auto* base = *reinterpret_cast<uint8_t* const*>(ctx + kCtxDepotPtrOff);
		const int32_t count =
		    *reinterpret_cast<const int32_t*>(ctx + kCtxDepotCountOff);
		if (!base || count <= 0 || count > kMaxDepots) return;

		auto* e = base;
		for (int32_t i = 0; i < count; ++i, e += kDepotEntryStride)
		{
			const uint32_t depotId = *reinterpret_cast<const uint32_t*>(e);
			const uint64_t pin = g_config.getManifestPin(depotId);
			if (!pin) continue;
			auto* gidp = reinterpret_cast<uint64_t*>(e + kDepotEntryGidOff);
			if (*gidp != pin)
			{
				g_pLog->info("ReconcilePin: app=%u target depot=%u gid=%llu -> "
				             "pinned gid=%llu\n",
				             appId, depotId,
				             static_cast<unsigned long long>(*gidp),
				             static_cast<unsigned long long>(pin));
				*gidp = pin;
			}
		}
	}

	__attribute__((regparm(1)))
	void* hkEvaluate(void* mgr, void* ctx, void* a1, void* a2)
	{
		if (ctx)
		{
			const auto* c = reinterpret_cast<const uint8_t*>(ctx);
			const uint32_t appId = *reinterpret_cast<const uint32_t*>(c + kCtxAppIdOff);
			const uint32_t flags = *reinterpret_cast<const uint32_t*>(c + kCtxFlagsOff);

			if (g_trace)
			{
				void* base =
				    *reinterpret_cast<void* const*>(c + kCtxDepotPtrOff);
				const int32_t count =
				    *reinterpret_cast<const int32_t*>(c + kCtxDepotCountOff);
				traceLog(appId, flags, base, count);
			}
			applyTargetPin(ctx, appId, flags);
		}
		return g_orig(mgr, ctx, a1, a2);
	}
}


namespace ReconcilePin
{
	bool setup()
	{
		// The gid rewrite (the loop fix) is DEFAULT ON now: the pin is
		// config-driven (ManifestPins locked apps), so it acts for any locked
		// app without an env var.  SLSSTEAM_RECONCILE_PIN=0 is an explicit
		// opt-out for isolating/debugging.
		g_pinActive = true;
		if (const char* e = std::getenv("SLSSTEAM_RECONCILE_PIN"))
		{
			g_pinActive = !(e[0] == '0' && e[1] == '\0');
		}
		if (const char* e = std::getenv("SLSSTEAM_RECONCILE_TRACE"))
		{
			g_trace = !(e[0] == '0' && e[1] == '\0');
		}
		if (!g_pinActive && !g_trace)
		{
			return false;  // nothing to do; don't touch the hot reconcile path
		}

		auto& pat = Patterns::CDepotDownloadMgr::EvaluateConfigChanges;
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ReconcilePin: EvaluateConfigChanges pattern not found; "
			             "loop fix disabled\n");
			return false;
		}

		g_addr = pat.address;
		g_size = LM_HookCode(g_addr,
		                     reinterpret_cast<lm_address_t>(&hkEvaluate),
		                     &g_tramp);
		if (!g_size || g_tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ReconcilePin: failed to install hook\n");
			g_addr = LM_ADDRESS_BAD;
			g_tramp = LM_ADDRESS_BAD;
			g_size = 0;
			return false;
		}
		g_orig = reinterpret_cast<ReconFn_t>(g_tramp);
		g_pLog->info("ReconcilePin: hooked EvaluateConfigChanges at %p "
		             "(pin=%d trace=%d)\n",
		             reinterpret_cast<void*>(g_addr),
		             static_cast<int>(g_pinActive), static_cast<int>(g_trace));
		return true;
	}

	void remove()
	{
		if (g_size && g_addr != LM_ADDRESS_BAD && g_tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(g_addr, g_tramp, g_size);
		}
		g_orig = nullptr;
		g_addr = LM_ADDRESS_BAD;
		g_tramp = LM_ADDRESS_BAD;
		g_size = 0;
	}
}
