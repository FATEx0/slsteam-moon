#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# run-steamless.sh — strips Steam Stub DRM from a Windows .exe.
#
# Designed to be invoked by the SLSsteam .so right before Steam launches
# an unowned-app Windows binary. Mirrors Accela's flow but without the
# Qt/Python overhead.
#
# Usage:
#   run-steamless.sh <exe-path>
#
# Exit codes:
#   0  unpack succeeded, original.exe.original.exe is the backup,
#      original.exe is now the unpacked (naked) binary
#   1  invalid arguments
#   2  exe doesn't carry the Steam Stub VLV signature — already naked,
#      no work needed
#   3  no usable Wine binary (system wine missing, no Proton installed)
#   4  Steamless run failed (compile/dependency/timeout)
#   5  Steamless ran but produced no .unpacked.exe
#   6  rename step failed
#
# Environment variables (optional):
#   STEAMLESS_HOME    where Steamless.CLI.exe + Plugins/ live;
#                     default: <script dir>/../steamless-bin
#   WINE_BIN          override wine binary path (default: probe Proton then PATH)
#   WINE_PREFIX       override prefix path
#                     (default: ~/.local/share/SLSsteam/steamless-prefix)
#   QUIET             non-empty -> suppress informational stdout
#                     (errors still go to stderr)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE_PATH="${1:-}"

log()  { [ -z "${QUIET:-}" ] && echo "[steamless-bypass] $*"; return 0; }
warn() { echo "[steamless-bypass] WARN: $*" >&2; }
die()  { echo "[steamless-bypass] ERROR: $*" >&2; exit "${2:-1}"; }

# ── 1. validate args ────────────────────────────────────────────────────
[ -n "$EXE_PATH" ] || die "usage: $0 <exe-path>" 1
[ -f "$EXE_PATH" ] || die "exe not found: $EXE_PATH" 1

# ── 2. probe Steam Stub VLV signature at offset 0x40 ────────────────────
# Quick exit if the exe isn't packed. Saves 5-30s of Wine startup.
sig=$(dd if="$EXE_PATH" bs=1 skip=64 count=4 2>/dev/null | xxd -p)
if [ "$sig" != "564c5600" ]; then
    log "exe is not Steam-Stub-wrapped (sig=$sig); skipping"
    exit 2
fi
log "Steam Stub VLV signature detected — proceeding"

# ── 3. resolve Steamless home ────────────────────────────────────────────
STEAMLESS_HOME="${STEAMLESS_HOME:-$SCRIPT_DIR/../steamless-bin}"
STEAMLESS_CLI="$STEAMLESS_HOME/Steamless.CLI.exe"
[ -f "$STEAMLESS_CLI" ] || die "Steamless.CLI.exe not found at $STEAMLESS_CLI" 4

# Steamless's CLI exe loads Steamless.API.dll from the working
# directory. Plugins live in Plugins/. If the API dll is only in
# Plugins/, copy it up so the CLI can find it. Idempotent.
if [ ! -f "$STEAMLESS_HOME/Steamless.API.dll" ] \
   && [ -f "$STEAMLESS_HOME/Plugins/Steamless.API.dll" ]; then
    cp "$STEAMLESS_HOME/Plugins/Steamless.API.dll" "$STEAMLESS_HOME/Steamless.API.dll"
fi

# ── 4. locate wine binary ───────────────────────────────────────────────
# Order: explicit override > Proton-experimental in standard Steam paths >
#        Proton in compatibilitytools.d > system wine.
find_wine() {
    if [ -n "${WINE_BIN:-}" ] && [ -x "$WINE_BIN" ]; then
        echo "$WINE_BIN"; return
    fi

    local candidates=()
    for steam_root in \
        "$HOME/.steam/steam" \
        "$HOME/.steam/debian-installation" \
        "$HOME/.local/share/Steam"
    do
        candidates+=( "$steam_root/steamapps/common/Proton - Experimental/files/bin/wine" )
        candidates+=( "$steam_root/steamapps/common/Proton"*/files/bin/wine )
        candidates+=( "$steam_root/compatibilitytools.d/"*/files/bin/wine )
        candidates+=( "$steam_root/compatibilitytools.d/"*/dist/bin/wine )
    done
    candidates+=( "$(command -v wine 2>/dev/null || true)" )

    for c in "${candidates[@]}"; do
        if [ -n "$c" ] && [ -x "$c" ]; then
            echo "$c"; return
        fi
    done
    return 1
}

WINE_BIN="$(find_wine)" || die "no usable Wine binary found" 3
log "using wine: $WINE_BIN"

# Wine and wineserver have to come from the same install or the
# version mismatch crashes the prefix. Always pair them.
WINE_DIR="$(dirname "$WINE_BIN")"
WINESERVER_BIN="$WINE_DIR/wineserver"
[ -x "$WINESERVER_BIN" ] || warn "wineserver not at $WINESERVER_BIN; relying on PATH"

# ── 5. set up environment ───────────────────────────────────────────────
WINE_PREFIX="${WINE_PREFIX:-$HOME/.local/share/SLSsteam/steamless-prefix}"
mkdir -p "$WINE_PREFIX"

# If using Proton, point the dynamic linker at Proton's bundled libs.
# Otherwise let the system loader resolve it. The Proton lib paths
# follow the standard layout: <proton_root>/files/{lib,lib64}.
if [[ "$WINE_BIN" == */Proton*/files/bin/wine ]]; then
    PROTON_FILES="$(dirname "$(dirname "$WINE_DIR")")"
    LD_LIBRARY_PATH="$PROTON_FILES/lib64:$PROTON_FILES/lib:${LD_LIBRARY_PATH:-}"
    WINEDLLPATH="$PROTON_FILES/lib64/wine:$PROTON_FILES/lib/wine"
    export LD_LIBRARY_PATH WINEDLLPATH
    log "configured Proton library paths"
fi

export WINEPREFIX="$WINE_PREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINESERVER="${WINESERVER:-$WINESERVER_BIN}"
export WINEARCH="${WINEARCH:-win64}"

# Different Steam runtime ships overlapping wineservers. Kill any
# stragglers before kicking off our own — version mismatch otherwise
# crashes immediately.
pkill -9 -f wineserver 2>/dev/null || true

# ── 6. lazy prefix init (~30s on first run) ─────────────────────────────
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    log "initializing Wine prefix at $WINEPREFIX (one-time, ~30s)"
    "$WINE_BIN" wineboot --init >/dev/null 2>&1 || die "wineboot --init failed" 4
    "$WINESERVER" -w 2>/dev/null || true
fi

# ── 7. invoke Steamless ─────────────────────────────────────────────────
# Steamless writes <exe>.unpacked.exe next to the input. Wine paths
# need to be drive-mapped — Z:\ maps to /. Everything else is just
# slash-flipping.
WIN_PATH="Z:${EXE_PATH//\//\\}"
log "running Steamless on $EXE_PATH"

cd "$STEAMLESS_HOME"

# 90s timeout is generous; Skyrim's 37 MB exe takes ~3s on a warm prefix.
if ! timeout 90 "$WINE_BIN" Steamless.CLI.exe \
        --quiet --realign --recalcchecksum -f "$WIN_PATH" \
        > /tmp/steamless-bypass.$$.log 2>&1
then
    warn "Steamless invocation failed; log follows:"
    sed 's/^/  /' /tmp/steamless-bypass.$$.log >&2
    rm -f /tmp/steamless-bypass.$$.log
    die "Steamless failed" 4
fi
[ -z "${QUIET:-}" ] && cat /tmp/steamless-bypass.$$.log
rm -f /tmp/steamless-bypass.$$.log

UNPACKED="$EXE_PATH.unpacked.exe"
[ -f "$UNPACKED" ] || die "no unpacked output at $UNPACKED" 5

# ── 8. atomic-ish rename ────────────────────────────────────────────────
BACKUP="$EXE_PATH.original.exe"
log "swapping unpacked into place (backup: $BACKUP)"
if [ -f "$BACKUP" ]; then
    rm -f "$BACKUP"
fi
mv "$EXE_PATH" "$BACKUP" || die "backup move failed" 6
mv "$UNPACKED" "$EXE_PATH" || {
    warn "swap-in failed; rolling back"
    mv "$BACKUP" "$EXE_PATH"
    die "swap failed" 6
}
chmod +x "$EXE_PATH"

# Marker so we don't reprocess the same exe forever.
touch "$EXE_PATH.steamless_done"

log "done — $EXE_PATH is now Stub-free"
exit 0
