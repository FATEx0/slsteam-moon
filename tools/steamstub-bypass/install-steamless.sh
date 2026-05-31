#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# install-steamless.sh — fetch Steamless from GitHub releases and
# place it where run-steamless.sh expects it. Idempotent.
#
# Default install dir matches the path that run-steamless.sh probes
# when STEAMLESS_HOME is not set:
#   <script dir>/../steamless-bin
#
# Usage:
#   install-steamless.sh [--user-local]
#
# Flags:
#   --user-local   install to ~/.local/share/SLSsteam/steamless-bin
#                  instead of next to this script. Use this when the
#                  SLSsteam install dir is read-only (system package).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="$SCRIPT_DIR/../steamless-bin"

if [ "${1:-}" = "--user-local" ]; then
    TARGET_DIR="$HOME/.local/share/SLSsteam/steamless-bin"
fi

mkdir -p "$TARGET_DIR"

if [ -f "$TARGET_DIR/Steamless.CLI.exe" ] && [ -f "$TARGET_DIR/Steamless.API.dll" ]; then
    echo "[install-steamless] already installed at $TARGET_DIR"
    exit 0
fi

# Resolve latest release URL from the GitHub API.
echo "[install-steamless] resolving latest Steamless release..."
ASSET_URL="$(
    curl -fsSL https://api.github.com/repos/atom0s/Steamless/releases/latest \
    | grep -oE 'https://[^"]+\.zip' \
    | head -1
)"
if [ -z "$ASSET_URL" ]; then
    echo "[install-steamless] could not resolve latest release URL" >&2
    exit 1
fi
echo "[install-steamless] downloading $ASSET_URL"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/steamless.zip" "$ASSET_URL"
unzip -q -o "$TMP/steamless.zip" -d "$TMP/extract"

# Move artifacts. Layout is:
#   Steamless.CLI.exe   (and .config)
#   Steamless.exe       (and .config; we keep but don't use)
#   Plugins/*.dll
cp -r "$TMP/extract/." "$TARGET_DIR/"

# run-steamless.sh expects Steamless.API.dll alongside the CLI exe;
# upstream ships it inside Plugins/. Mirror it up.
if [ ! -f "$TARGET_DIR/Steamless.API.dll" ] \
   && [ -f "$TARGET_DIR/Plugins/Steamless.API.dll" ]; then
    cp "$TARGET_DIR/Plugins/Steamless.API.dll" "$TARGET_DIR/Steamless.API.dll"
fi

echo "[install-steamless] installed at $TARGET_DIR"
