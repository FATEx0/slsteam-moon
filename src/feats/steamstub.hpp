// SPDX-License-Identifier: AGPL-3.0-only
//
// Steamtools-Linux: Steam Stub DRM bypass.
//
// Wraps the run-steamless.sh helper that strips Steam Stub from
// AdditionalApps' Windows executables before Steam launches them via
// Proton. Runs synchronously inside the IClientAppManager::LaunchApp
// hook so the unpack completes before the Stub-protected exe gets a
// chance to start.
//
// Why a shell helper instead of porting Steamless to C++:
//   - Steamless covers 5 stub variants (1.0/2.0/2.1/3.0/3.1, x86+x64),
//     each ~600-800 LOC of C# unpacker logic. Porting and keeping
//     them in sync as Valve revs the stub is a multi-session ongoing
//     cost we don't want to take on.
//   - Steamless is widely deployed under Wine on Linux (Accela,
//     SteaMidra) and updates land upstream automatically.
//   - The .so just needs to know "is this exe stub-wrapped, and is
//     the helper available" — both are cheap reads.
//
// Layout expected at runtime:
//   <ssl-install-root>/tools/steamstub-bypass/run-steamless.sh
//   <ssl-install-root>/tools/steamless-bin/Steamless.CLI.exe
//   <ssl-install-root>/tools/steamless-bin/Plugins/*.dll
//
// `<ssl-install-root>` is the directory that contains SLSsteam.so —
// resolved at startup via dladdr().

#pragma once

#include <cstdint>

namespace SteamStub
{
	// Configure helper paths from `installRoot` (typically the
	// directory holding SLSsteam.so).  Idempotent.  Logs a warning
	// at debug level and disables the feature if the helper script
	// or Steamless binaries are missing — Steam still runs, the
	// Stub-protected app just fails like it would without us.
	void setup(const char* installRoot);

	// Called from the LaunchApp hook for every app launch.  No-ops
	// for owned apps and apps not in AdditionalApps.  For
	// AdditionalApps, scans the install dir for .exe files, runs
	// the helper script on each Stub-wrapped one, blocks until the
	// helper exits.  Re-entry safe via on-disk marker files.
	void onLaunchApp(uint32_t appId);
}
