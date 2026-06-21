// SPDX-License-Identifier: AGPL-3.0-only
//
// See manifestbind.hpp for design notes.

#include "manifestbind.hpp"

#include "depotkey.hpp"
#include "depotkey_scope.hpp"
#include "manifeststore.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"
#include "../utils/ManifestFetch.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>


namespace
{
	// Two cooperating detours in CDepotDownloadMgr (see patterns.cpp
	// CDepotDownloadMgr for the full RE).  Both share this 7-dword cdecl
	// signature:
	//   (ctx, a0C, appId, depotId, uint64 manifestId, a20) -> (eax)
	//
	//   * ProcessDepotManifest — the manifest-acquisition LEAF: builds
	//     "<steamRoot>/depotcache/<depot>_<gid>.manifest", checks it on disk,
	//     calls BYldRequestDepotManifest only when missing.  Redirecting the
	//     gid here makes the on-disk check find the locally-staged (zip)
	//     manifest and skip the request-code fetch (lets a providers-down
	//     install proceed).  5 callers go through it; hooking the leaf covers
	//     them all for the BYld decision.
	//
	//   * PrepareDepotDownload — a LATER pipeline stage (one of those callers)
	//     that, after the leaf returns, looks the depot up in the per-download
	//     table BY the gid it was called with and derefs the per-manifest
	//     state pointer.  Must be redirected too, else it looks up the public
	//     gid the leaf no longer staged -> NULL deref -> SIGSEGV.
	//
	// Both are self-contained PIC.  ProcessDepotManifest's get_pc_thunk is its
	// first instruction (in the relocated tramp -> fixPICThunkCall repairs
	// it); PrepareDepotDownload's is at +5 (not relocated -> fixPICThunkCall
	// is a harmless no-op).
	using DepotFn_t = void*(*)(void*, uint32_t, uint32_t, uint32_t,
	                           uint64_t, uint32_t);

	// FUNC_1141 / CDepotDownloadMgr::BuildDepotDependency — the install-plan
	// consumer (manifest-pin-planner-port.md §12).  cdecl, 4 dwords:
	//   (void* ctx, uint32_t flag, CUtlVector<DepotEntry>* depots, uint32_t a3)
	using BuildDepFn_t = void*(*)(void*, uint32_t, void*, uint32_t);

	// DepotEntry layout (confirmed §11/§12, OpenSteamTool Structs.h verbatim):
	//   +0x00 u32 DepotId   +0x08 u64 ManifestGid   +0x10 u64 ManifestSize
	//   +0x18 u32 DlcAppId   +0x1c u8 Lcs   +0x1d u8 bNotNewTarget
	//   +0x1e u8 SharedInstall ; stride 0x20.
	// CUtlVector<DepotEntry>: element base @ +0x00 (m_Memory.m_pMemory),
	//   count (m_Size) @ +0x0c.
	constexpr size_t kDepotEntryStride = 0x20;
	constexpr size_t kDepotEntryGidOff = 0x08;
	constexpr size_t kDepotEntrySizeOff = 0x10;
	constexpr size_t kVecBaseOff = 0x00;
	constexpr size_t kVecCountOff = 0x0c;

	struct Detour
	{
		DepotFn_t    orig = nullptr;
		lm_address_t addr = LM_ADDRESS_BAD;
		lm_address_t tramp = LM_ADDRESS_BAD;
		lm_size_t    size = 0;
	};

	Detour g_leaf;     // ProcessDepotManifest
	Detour g_planner;  // PrepareDepotDownload

	// BuildDepotDependency uses its own typed orig (different arity).
	struct BuildDetour
	{
		BuildDepFn_t orig = nullptr;
		lm_address_t addr = LM_ADDRESS_BAD;
		lm_address_t tramp = LM_ADDRESS_BAD;
		lm_size_t    size = 0;
	};
	BuildDetour g_builder;  // CDepotDownloadMgr::BuildDepotDependency

	bool g_fallbackEnabled = true;
	// Patch the depot gid in the install plan so Steam commits the pinned build
	// for LOCKED apps (§13).  DEFAULT ON: the pin is config-driven (ManifestPins
	// locked apps), no longer env-gated; a non-pinned depot is a no-op because
	// getManifestPin returns 0.  The appinfo depot-gid pin (appinfopin.hpp) keeps
	// update-check/plan/commit/reconcile agreeing on the pinned gid so it no
	// longer loops.  SLSSTEAM_PIN_PLANNER=0 is an explicit opt-out for testing.
	bool g_pinPlanner = true;

	// --- DIAGNOSTIC: install-planner runtime trace (env SLSSTEAM_PLAN_TRACE) -
	//
	// §11 of manifest-pin-planner-port.md: static analysis could not tie the
	// depot-list BUILDER (the LumaCore `BuildDepotDependency` equivalent) back
	// to an external caller — the consumer `sub_FDFD00` (VA 0xfdfd00) appears
	// to have no static caller, no vtable slot, no GOT entry.  The unblocking
	// move is to recover the dynamic CALL CHAIN at the two frames we already
	// hook (ProcessDepotManifest @0xfa8b40, PrepareDepotDownload @0xfa9050).
	//
	// We do NOT use backtrace()/libgcc _Unwind: it SIGABRTs in this 32-bit
	// client when invoked from inside a libmem trampoline (no eh_frame for the
	// tramp frame -> unwinder aborts; confirmed: client Abort at the exact
	// Reconfiguring tick the hook first fired).  Instead we scan our own stack
	// for dwords that point INTO steamclient's mapped range AND are preceded by
	// a `call` instruction (0xE8 rel32 at V-5, or 0xFF /2..3 at V-2/V-3) — a
	// safe, allocation-free, unwinder-free "poor man's backtrace".  Each hit is
	// logged as a steamclient-relative VA (V - base), directly comparable to
	// the §11 ELF VAs.  The consumer sub_FDFD00's frame and, crucially, ITS
	// caller (the builder/scheduler) appear in the scan window.
	//
	// Gated behind the env var so a normal session pays nothing; capped per
	// frame so a many-depot install doesn't flood ~/.SLSsteam.log.
	bool g_planTrace = false;
	std::atomic<int> g_traceLeftLeaf{12};
	std::atomic<int> g_traceLeftPlanner{12};

	// Is `v` a plausible return address: inside steamclient .text and preceded
	// by a call instruction?  Reads only mapped code (safe).
	bool looksLikeRetAddr(uintptr_t v, uintptr_t base, uintptr_t end)
	{
		if (v < base + 16 || v >= end) return false;
		const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
		// call rel32: E8 xx xx xx xx  -> return addr is at insn+5
		if (p[-5] == 0xE8) return true;
		// call r/m32: FF /2 (modrm reg field == 2), 2- or 3-byte forms
		if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;
		if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;
		// call r/m32 with disp8/disp32 + SIB: be lenient, accept FF in window
		if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;
		if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return true;
		return false;
	}

	void logPlanStack(const char* tag, std::atomic<int>& budget,
	                  uint32_t appId, uint32_t depotId, uint64_t gid)
	{
		if (!g_planTrace) return;
		if (budget.fetch_sub(1) <= 0) return;

		const auto base = reinterpret_cast<uintptr_t>(g_modSteamClient.base);
		const auto end = reinterpret_cast<uintptr_t>(g_modSteamClient.end);

		// Start just above our own frame and scan upward (stack grows down).
		volatile int anchor = 0;
		uintptr_t sp = reinterpret_cast<uintptr_t>(&anchor);
		const uintptr_t scanWords = 320;  // ~1.25 KiB window

		g_pLog->info("PlanTrace[%s]: app=%u depot=%u gid=%llu base=%p sp=%p\n",
		             tag, appId, depotId,
		             static_cast<unsigned long long>(gid),
		             g_modSteamClient.base, reinterpret_cast<void*>(sp));

		int shown = 0;
		for (uintptr_t i = 0; i < scanWords && shown < 24; ++i)
		{
			const uintptr_t slot = sp + i * sizeof(uintptr_t);
			const uintptr_t v = *reinterpret_cast<volatile uintptr_t*>(slot);
			if (looksLikeRetAddr(v, base, end))
			{
				g_pLog->info("PlanTrace[%s]:   +0x%lx  steamclient+0x%lx\n",
				             tag,
				             static_cast<unsigned long>(i * sizeof(uintptr_t)),
				             static_cast<unsigned long>(v - base));
				++shown;
			}
		}
	}


	std::string findSteamRoot()
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
			if (stat((r + "/steam.sh").c_str(), &st) == 0) return r;
		}
		return {};
	}

	bool manifestOnDisk(const std::string& depotcacheDir,
	                    uint32_t depotId, uint64_t gid)
	{
		const std::string p = depotcacheDir + "/" + std::to_string(depotId)
		                      + "_" + std::to_string(gid) + ".manifest";
		struct stat st{};
		return stat(p.c_str(), &st) == 0 && st.st_size > 0;
	}

	// Find a locally-staged manifest for `depotId` whose gid differs from
	// `planned` (i.e. the LuaTools zip's own manifest).  Picks the most
	// recently written one when several exist.  Returns 0 if none.
	uint64_t findLocalAltGid(const std::string& depotcacheDir,
	                         uint32_t depotId, uint64_t planned)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(depotcacheDir, ec)) return 0;

		const std::string prefix = std::to_string(depotId) + "_";
		uint64_t best = 0;
		long bestMtime = -1;
		for (const auto& entry :
		     std::filesystem::directory_iterator(depotcacheDir, ec))
		{
			if (ec) break;
			if (!entry.is_regular_file(ec)) continue;
			const auto name = entry.path().filename().string();
			if (name.rfind(prefix, 0) != 0) continue;
			const auto dot = name.rfind(".manifest");
			if (dot == std::string::npos || dot + 9 != name.size()) continue;
			const auto gidStr = name.substr(prefix.size(),
			                                dot - prefix.size());
			if (gidStr.empty()) continue;

			uint64_t gid = 0;
			try { gid = std::stoull(gidStr); } catch (...) { continue; }
			if (!gid || gid == planned) continue;

			struct stat st{};
			if (stat(entry.path().c_str(), &st) != 0 || st.st_size <= 0)
			{
				continue;
			}
			if (static_cast<long>(st.st_mtime) > bestMtime)
			{
				bestMtime = static_cast<long>(st.st_mtime);
				best = gid;
			}
		}
		return best;
	}

	bool depotInScope(uint32_t appId, uint32_t depotId)
	{
		// Only content WE manage.  A merely-observed key (owned game / Proton
		// runtime) must NOT pull the depot in here: redirectGid would archive
		// it into the ManifestStore and could redirect an owned depot to a
		// stale local gid.  The store holds LuaTools depots only.
		return DepotKey::depotInManifestScope(
		    appId   && g_config.isAddedAppId(appId),
		    depotId && g_config.isAddedAppId(depotId),
		    DepotKey::isManagedDepot(depotId),
		    /*depotHasPin=*/g_config.getManifestPin(depotId) != 0);
	}

	// Shared redirect decision: when the planned (public) gid's manifest is
	// not on disk but a different gid for the same depot IS available
	// (depotcache or the persistent ManifestStore), return that local gid;
	// otherwise return the planned gid unchanged.
	uint64_t redirectGid(const char* site, uint32_t appId, uint32_t depotId,
	                     uint64_t manifestId)
	{
		// Explicit pin (design §4.2): the pin is TARGET-ONLY.  Ensure the
		// pinned manifest is STAGED on disk (so the FUNC_1141-patched plan and
		// ReconcilePin's reconcile target can fetch it) but DO NOT redirect the
		// gid here.
		//
		// Returning `pin` unconditionally (the old behaviour) contaminated the
		// ACTIVE/installed manifest read: when Steam loads the installed
		// (public) manifest to compute the public->pinned chunk delta, serving
		// the pinned manifest instead made it diff pinned-vs-pinned -> 0 chunks
		// -> a 0-file commit -> the real content (e.g. the build-locked exe a
		// crack validates) was NEVER downloaded, even though Steam recorded the
		// pinned gid and reported "Fully Installed" (proven live, HANDOFF-v2
		// §8; predicted by NEXT-STEPS §3.1).  The pin belongs ONLY on the
		// TARGET (FUNC_1141 plan DepotEntry + feats/reconcilepin.cpp's reconcile
		// target pass), never on this acquisition/active path.  Leaving the
		// real gid here lets Steam load the genuine public active manifest,
		// compute a true delta against the pinned target, and download it.
		const uint64_t pin = g_config.getManifestPin(depotId);
		if (pin)
		{
			// Stage the pinned manifest if absent (harmless when present: just
			// an on-disk check).  Needed because a pinned build is commonly one
			// the user never installed, so it is NOT in depotcache/the store
			// yet, and Steam's own BYldRequestDepotManifest is "Access Denied"
			// for a managed (non-purchased) app.  Restore from the persistent
			// store if we archived it before, else fetch+stage via ManifestFetch
			// (same request-code provider + CDN path pics.cpp uses) and persist.
			const auto steamRoot = findSteamRoot();
			if (!steamRoot.empty())
			{
				const std::string dc = steamRoot + "/depotcache";
				if (!manifestOnDisk(dc, depotId, pin)
				    && !ManifestStore::restoreToDepotcache(depotId, pin))
				{
					ManifestFetch::fetchManifestBlobSync(pin, depotId);
					ManifestStore::archiveDepot(depotId);
				}
			}
			g_pLog->debug("ManifestBind[%s]: depot=%u pin gid=%llu staged "
			              "(target-only; active read left intact)\n",
			              site, depotId,
			              static_cast<unsigned long long>(pin));
			return manifestId;
		}

		if (!(g_fallbackEnabled && manifestId && depotId
		      && depotInScope(appId, depotId)))
		{
			return manifestId;
		}

		const auto steamRoot = findSteamRoot();
		if (steamRoot.empty()) return manifestId;

		const std::string dc = steamRoot + "/depotcache";

		// Capture whatever manifests are currently staged for this depot
		// into the purge-proof store BEFORE Steam can purge them.
		// Idempotent. (Bulk capture of all of an app's depots happens at
		// PICS install-plan time in pics.cpp; this is belt-and-suspenders.)
		ManifestStore::archiveDepot(depotId);

		// Gate: if the planned (public) gid IS already in depotcache, the
		// providers worked (or it's still staged) -> install it as-is.
		if (manifestOnDisk(dc, depotId, manifestId)) return manifestId;

		// The planned gid is NOT in depotcache.  Case 1 (the common one,
		// e.g. a Proton-switch after Steam purged the windows depot's
		// manifest): we archived this EXACT gid earlier -> restore it from
		// the store so Steam finds it on disk and skips the request-code
		// fetch.  No redirect needed; Steam installs the gid it planned.
		if (ManifestStore::restoreToDepotcache(depotId, manifestId))
		{
			g_pLog->info("ManifestBind[%s]: depot=%u restored planned gid=%llu "
			             "from store (no internet needed)\n",
			             site, depotId,
			             static_cast<unsigned long long>(manifestId));
			return manifestId;
		}

		// Case 2: the planned gid is unavailable anywhere (the live public
		// build is newer than anything we hold).  Fall back to a DIFFERENT
		// gid for the depot -- newest in depotcache, else newest archived
		// in the store (restored into depotcache).
		uint64_t alt = findLocalAltGid(dc, depotId, manifestId);
		if (!alt)
		{
			alt = ManifestStore::bestArchivedGid(depotId, manifestId);
			if (alt) ManifestStore::restoreToDepotcache(depotId, alt);
		}
		if (!alt) return manifestId;

		g_pLog->info(
		    "ManifestBind[%s]: depot=%u public gid=%llu not staged; "
		    "installing local manifest gid=%llu instead (resilience fallback)\n",
		    site, depotId,
		    static_cast<unsigned long long>(manifestId),
		    static_cast<unsigned long long>(alt));
		return alt;
	}

	void* hkProcessDepot(void* ctx, uint32_t a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, uint32_t a20)
	{
		logPlanStack("leaf", g_traceLeftLeaf, appId, depotId, manifestId);
		const uint64_t useGid = redirectGid("leaf", appId, depotId, manifestId);
		return g_leaf.orig(ctx, a0C, appId, depotId, useGid, a20);
	}

	void* hkPrepareDepot(void* ctx, uint32_t a0C, uint32_t appId,
	                     uint32_t depotId, uint64_t manifestId, uint32_t a20)
	{
		logPlanStack("plan", g_traceLeftPlanner, appId, depotId, manifestId);
		const uint64_t useGid = redirectGid("plan", appId, depotId, manifestId);
		return g_planner.orig(ctx, a0C, appId, depotId, useGid, a20);
	}

	// CDepotDownloadMgr::BuildDepotDependency — patch the PLAN in place.
	//
	// This is the real manifest-pin lever (manifest-pin-planner-port.md §12).
	// Steam decides the depot gid + build it installs/commits from the
	// already-built DepotEntry vector this function consumes; redirecting the
	// gid downstream (ProcessDepotManifest/PrepareDepotDownload) only changes
	// which .manifest is fetched, NOT the committed gid (proven, §3).  Here we
	// overwrite depots[i].ManifestGid for any pinned depot BEFORE the original
	// runs, so the planned-gid copy (ctx+0x664) and the commit see the pin.
	//
	// Defensive: bail on an implausible vector (null base / out-of-range count)
	// so a signature/ABI drift degrades to a harmless pass-through.
	void* hkBuildDepot(void* ctx, uint32_t flag, void* depots, uint32_t a3)
	{
		if (depots)
		{
			const auto p = reinterpret_cast<char*>(depots);
			char* const base = *reinterpret_cast<char* const*>(p + kVecBaseOff);
			const int32_t count =
			    *reinterpret_cast<const int32_t*>(p + kVecCountOff);

			if (base && count > 0 && count <= 4096)
			{
				for (int32_t i = 0; i < count; ++i)
				{
					char* e = base + static_cast<size_t>(i) * kDepotEntryStride;
					const uint32_t depotId =
					    *reinterpret_cast<const uint32_t*>(e);
					const uint64_t pin = g_config.getManifestPin(depotId);
					if (!pin) continue;

					auto* gidp =
					    reinterpret_cast<uint64_t*>(e + kDepotEntryGidOff);
					if (g_pinPlanner && *gidp != pin)
					{
						g_pLog->info(
						    "ManifestBind[build]: depot=%u plan gid=%llu -> pinned "
						    "gid=%llu (DepotEntry patch)\n",
						    depotId,
						    static_cast<unsigned long long>(*gidp),
						    static_cast<unsigned long long>(pin));
						*gidp = pin;
					}
				}
			}
			else
			{
				g_pLog->debugOnce(
				    "ManifestBind[build]: implausible depot vector "
				    "(base=%p count=%d); passing through\n",
				    static_cast<void*>(base), count);
			}
		}
		return g_builder.orig(ctx, flag, depots, a3);
	}

	// Install one detour; returns false (and leaves the Detour cleared) on
	// any failure so a single missing signature degrades to a safe no-op.
	bool installDetour(Detour& d, Pattern_t& pat, void* hookFn)
	{
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: %s pattern not found; that hook disabled\n",
			             pat.name.c_str());
			return false;
		}
		d.addr = pat.address;
		d.size = LM_HookCode(d.addr, reinterpret_cast<lm_address_t>(hookFn),
		                     &d.tramp);
		if (!d.size || d.tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: failed to install %s hook\n",
			             pat.name.c_str());
			d = Detour{};
			return false;
		}
		MemHlp::fixPICThunkCall(pat.name.c_str(), d.addr, d.tramp);
		d.orig = reinterpret_cast<DepotFn_t>(d.tramp);
		g_pLog->debug("ManifestBind: %s detour at %p, tramp at %p\n",
		              pat.name.c_str(),
		              reinterpret_cast<void*>(d.addr),
		              reinterpret_cast<void*>(d.tramp));
		return true;
	}

	void removeDetour(Detour& d)
	{
		if (d.size && d.addr != LM_ADDRESS_BAD && d.tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(d.addr, d.tramp, d.size);
		}
		d = Detour{};
	}

	// Install the BuildDepotDependency detour (distinct orig arity).
	bool installBuilder(Pattern_t& pat, void* hookFn)
	{
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: %s pattern not found; planner patch disabled\n",
			             pat.name.c_str());
			return false;
		}
		g_builder.addr = pat.address;
		g_builder.size = LM_HookCode(g_builder.addr,
		                             reinterpret_cast<lm_address_t>(hookFn),
		                             &g_builder.tramp);
		if (!g_builder.size || g_builder.tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ManifestBind: failed to install %s hook\n",
			             pat.name.c_str());
			g_builder = BuildDetour{};
			return false;
		}
		MemHlp::fixPICThunkCall(pat.name.c_str(), g_builder.addr, g_builder.tramp);
		g_builder.orig = reinterpret_cast<BuildDepFn_t>(g_builder.tramp);
		g_pLog->debug("ManifestBind: %s detour at %p, tramp at %p\n",
		              pat.name.c_str(),
		              reinterpret_cast<void*>(g_builder.addr),
		              reinterpret_cast<void*>(g_builder.tramp));
		return true;
	}
}


namespace ManifestBind
{
	bool setup()
	{
		if (const char* env = std::getenv("SLSSTEAM_MANIFEST_FALLBACK"))
		{
			g_fallbackEnabled = !(env[0] == '0' && env[1] == '\0');
		}

		if (const char* env = std::getenv("SLSSTEAM_PLAN_TRACE"))
		{
			g_planTrace = !(env[0] == '0' && env[1] == '\0');
		}

		if (const char* env = std::getenv("SLSSTEAM_PIN_PLANNER"))
		{
			g_pinPlanner = !(env[0] == '0' && env[1] == '\0');
		}

		// Both hooks cooperate; install independently so one missing
		// signature doesn't disable the other.  The leaf alone lets the
		// install skip BYld but crashes the planner's table lookup; the
		// planner alone never runs because BYld aborts first.  Both together
		// keep the gid consistent end-to-end.
		const bool leaf = installDetour(
		    g_leaf, Patterns::CDepotDownloadMgr::ProcessDepotManifest,
		    reinterpret_cast<void*>(&hkProcessDepot));
		const bool planner = installDetour(
		    g_planner, Patterns::CDepotDownloadMgr::PrepareDepotDownload,
		    reinterpret_cast<void*>(&hkPrepareDepot));

		// The planner patch (the real manifest-pin lever): rewrites the depot
		// gid in the install plan so Steam commits the pinned build.  Optional;
		// a missing signature degrades to the leaf/planner redirect only.
		const bool builder = installBuilder(
		    Patterns::CDepotDownloadMgr::BuildDepotDependency,
		    reinterpret_cast<void*>(&hkBuildDepot));

		g_pLog->debug("ManifestBind: leaf=%d planner=%d builder=%d fallback=%d pinPlanner=%d\n",
		              static_cast<int>(leaf), static_cast<int>(planner),
		              static_cast<int>(builder),
		              static_cast<int>(g_fallbackEnabled),
		              static_cast<int>(g_pinPlanner));
		return leaf || planner || builder;
	}

	void remove()
	{
		removeDetour(g_leaf);
		removeDetour(g_planner);
		if (g_builder.size && g_builder.addr != LM_ADDRESS_BAD
		    && g_builder.tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(g_builder.addr, g_builder.tramp, g_builder.size);
		}
		g_builder = BuildDetour{};
	}
}
