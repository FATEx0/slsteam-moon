// SPDX-License-Identifier: AGPL-3.0-only
//
// ReconcilePin — the downgrade-loop fix.
//
// Hooks CDepotDownloadMgr::EvaluateConfigChanges (entry VA 0xfe425a on build
// cfe99f0c) — the post-commit reconcile that diffs an app's ACTIVE (installed)
// depot config against its TARGET (appinfo) config and emits "config changed :
// updated depots" / "Update Required".  It is invoked once per side; the ctx's
// flag bit 0x8 marks the TARGET/appinfo side (confirmed live: active call
// carried the installed gid, target call carried the public appinfo gid,
// matching content_log "active: ... target: ...").
//
// The fix: for a LOCKED app, on the TARGET call (flags & 0x8) only, rewrite the
// depot ManifestGid in the ctx vector (ptr @ ctx+0x78, count @ ctx+0x84,
// stride 0x20, gid @ +0x8) to the configured pin.  Then a still-public install
// (active=public != target=pin) triggers the one downgrade, and afterwards
// (active=pin == target=pin) the reconcile finds "no changes" -> the perpetual
// "Update Required" loop never starts.  Patching the TARGET side in memory is
// downstream of appinfo, so an in-session RequestAppInfoUpdate cannot defeat it
// (unlike the appinfo.vdf provision pin).  The active side is left untouched so
// a real pending downgrade is never masked.
//
// Calling convention (REd): a global anchor/manager pointer arrives in EAX; the
// three args are on the stack (ctx = arg0 @ ebp+0x8).  Modelled with a
// regparm(1) detour; the prologue is a plain `push ebp` (no get_pc_thunk), so
// no fixPICThunkCall is needed.
//
// Gating: the gid rewrite acts only for locked apps and only when the pin
// feature is enabled (SLSSTEAM_PIN_PLANNER, shared with the FUNC_1141 plan
// patch).  Optional per-call diagnostic logging is gated separately on
// SLSSTEAM_RECONCILE_TRACE.  The hook installs when either is on.

#pragma once

namespace ReconcilePin
{
	// Resolve EvaluateConfigChanges and install the detour.  No-op (returns
	// false) when neither gate is on or the pattern/hook fails to resolve.
	bool setup();

	// Tear the detour back down (called from Hooks::remove).
	void remove();
}
