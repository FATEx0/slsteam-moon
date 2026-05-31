// SPDX-License-Identifier: AGPL-3.0-only
//
// Steamtools-Linux: package patch feature.
//
// Mirrors LumaCore's `PackagePatch` (see
// .kiro/research/lumacore/PackagePatch.cpp): hooks Steam's LoadPackage
// so that, when Steam loads PackageId 0 (the default anonymous package
// every account has), our AdditionalApps from the SLSsteam config get
// appended to `pInfo->AppIdVec` via the resolved `CUtlMemoryGrow`
// helper.
//
// Why: Steam's depot eligibility filter walks every package the user
// has and looks for the appid in `AppIdVec`. For unowned apps the
// filter returns "no eligible depots", which is what makes the
// install-dialog show 0 B and Steam declare "Fully Installed" without
// downloading anything. By making package 0 contain Skyrim-AE's depot
// list, the filter returns the real depots, the download size becomes
// non-zero, and our existing depot-key + manifest hooks finish the
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
