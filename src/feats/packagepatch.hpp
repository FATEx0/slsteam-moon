// SPDX-License-Identifier: AGPL-3.0-only
//
// Package patch feature.
//
// Hooks Steam's LoadPackage so that, when Steam loads PackageId 0
// (the default anonymous package every account has), our
// AdditionalApps from the SLSsteam config get appended to
// `pInfo->AppIdVec` via the resolved `CUtlMemoryGrow` helper.
//
// Why: Steam's depot eligibility filter walks every package and
// looks for an appid in `AppIdVec`. Without an entry in package 0
// the filter can return "no eligible depots" — the install dialog
// then shows 0 B and Steam declares the app installed without
// downloading anything. Adding the appid to package 0 makes the
// filter return the real depot list, the install size becomes
// correct, and existing depot-key + manifest paths finish the
// pipeline.

#pragma once

#include <cstdint>
#include <vector>

namespace PackagePatch
{
	// Set up the LoadPackage detour and resolve CUtlMemoryGrow.  Returns
	// false if either pattern failed to resolve; in that case the hook
	// is not installed and the feature is a no-op.
	bool setup();

	// Tear the hooks back down (called from Hooks::remove).
	void remove();

	// Manual injection entry point.  Steam normally calls LoadPackage
	// for package 0 once, very early at boot, after the cached
	// packageinfo.vdf is read from disk.  If our setup() ran *after*
	// that call (e.g. config parser loaded extra appids late), this
	// reinjects them.  Idempotent against the same id-set.
	bool injectIntoPackage0(const std::vector<uint32_t>& appIds);
}
