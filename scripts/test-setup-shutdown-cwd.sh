#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Regression test for setup.sh::kill_steam. The shutdown launcher must be
# invoked from a stable directory rather than the installer's temporary
# extraction directory. This matters for sandboxed launchers such as NixOS's
# bwrap wrapper, which reproduces the caller's cwd inside an isolated /tmp.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SETUP="$SCRIPT_DIR/../setup.sh"
PASS=0
FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL + 1)); }

FN="$(awk '
  /^kill_steam\(\)/ { f=1 }
  f { print }
  f && /^}/ { exit }
' "$SETUP")"
[ -n "$FN" ] || { echo "could not extract kill_steam from setup.sh" >&2; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
HOME_DIR="$TMP_DIR/home"
BIN_DIR="$TMP_DIR/bin"
EXTRACT_DIR="$TMP_DIR/extracted"
PGREP_STATE="$TMP_DIR/pgrep.state"
PWD_CAPTURE="$TMP_DIR/shutdown.cwd"
mkdir -p "$HOME_DIR" "$BIN_DIR" "$EXTRACT_DIR"
export HOME="$HOME_DIR" PGREP_STATE PWD_CAPTURE

cat > "$BIN_DIR/pgrep" <<'PG'
#!/bin/sh
# Report one running Steam process, then report that it stopped.
if [ ! -f "$PGREP_STATE" ]; then
	touch "$PGREP_STATE"
	exit 0
fi
exit 1
PG
cat > "$BIN_DIR/steam" <<'STEAM'
#!/bin/sh
pwd > "$PWD_CAPTURE"
exit 0
STEAM
chmod +x "$BIN_DIR/pgrep" "$BIN_DIR/steam"

(
	cd "$EXTRACT_DIR" || exit 1
	PATH="$BIN_DIR:/usr/bin:/bin"
	export PATH
	log_info() { :; }
	log_success() { :; }
	log_warn() { :; }
	eval "$FN"
	kill_steam
)

if [ "$(cat "$PWD_CAPTURE" 2>/dev/null)" = "$HOME_DIR" ]; then
	ok "shutdown launcher runs from HOME"
else
	bad "shutdown launcher ran from $(cat "$PWD_CAPTURE" 2>/dev/null || echo '<unknown>')"
fi

echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
