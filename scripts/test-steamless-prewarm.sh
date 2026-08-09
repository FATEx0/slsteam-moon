#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Regression test for bounded Steamless prewarm subprocesses.  The harness
# supplies fake Wine binaries and a fake timeout command so no Wine install,
# network, or real wineserver is needed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HELPER="$REPO_ROOT/tools/steamstub-bypass/run-steamless.sh"

PASS=0
FAIL=0
ok()   { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

make_harness() {
    local root="$1"
    mkdir -p "$root/bin" "$root/prefix"

    cat > "$root/bin/wine" <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "wineboot" ]; then
    mkdir -p "$WINEPREFIX"
    : > "$WINEPREFIX/system.reg"
fi
exit 0
EOF
    cat > "$root/bin/wineserver" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
    cat > "$root/bin/pkill" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
    cat > "$root/bin/timeout" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[ "${1:-}" = "90" ] || exit 99
shift
printf '%s\n' "$*" >> "$TIMEOUT_LOG"
if [ "${TIMEOUT_FAIL_WINESERVER:-0}" = 1 ] && [ "$(basename "$1")" = wineserver ]; then
    exit 124
fi
exec "$@"
EOF
    chmod +x "$root/bin/wine" "$root/bin/wineserver" \
        "$root/bin/pkill" "$root/bin/timeout"
}

run_prewarm() {
    local root="$1"
    TIMEOUT_LOG="$root/timeout.log" \
    WINE_BIN="$root/bin/wine" \
    WINESERVER="$root/bin/wineserver" \
    WINE_PREFIX="$root/prefix" \
    PATH="$root/bin:$PATH" \
    QUIET=1 \
    bash "$HELPER" --prewarm
}

run_prewarm_with_path_wineserver() {
    local root="$1"
    mkdir -p "$root/fallback"
    mv "$root/bin/wineserver" "$root/fallback/wineserver"
    env -u WINESERVER \
    TIMEOUT_LOG="$root/timeout.log" \
    WINE_BIN="$root/bin/wine" \
    WINE_PREFIX="$root/prefix" \
    PATH="$root/bin:$root/fallback:$PATH" \
    QUIET=1 \
    bash "$HELPER" --prewarm
}

run_prewarm_without_timeout() {
    local root="$1"
    rm -f "$root/bin/timeout"
    ln -s /usr/bin/dirname "$root/bin/dirname"
    ln -s /usr/bin/mkdir "$root/bin/mkdir"
    TIMEOUT_LOG="$root/timeout.log" \
    WINE_BIN="$root/bin/wine" \
    WINESERVER="$root/bin/wineserver" \
    WINE_PREFIX="$root/prefix" \
    PATH="$root/bin" \
    QUIET=1 \
    /bin/bash "$HELPER" --prewarm
}

echo "== test-steamless-prewarm =="

root="$(mktemp -d)"
failed_root=""
missing_root=""
fallback_root=""
trap 'rm -rf "$root" "$failed_root" "$missing_root" "$fallback_root"' EXIT
make_harness "$root"
if run_prewarm "$root" >/dev/null 2>&1; then
    count="$(wc -l < "$root/timeout.log")"
    [ "$count" -eq 2 ] \
        && ok "wineboot and wineserver prewarm waits use timeout" \
        || bad "expected two bounded prewarm commands, saw $count"
else
    bad "successful prewarm harness exited non-zero"
fi

fallback_root="$(mktemp -d)"
make_harness "$fallback_root"
if run_prewarm_with_path_wineserver "$fallback_root" >/dev/null 2>&1; then
    count="$(wc -l < "$fallback_root/timeout.log")"
    [ "$count" -eq 2 ] \
        && ok "wineserver is resolved from PATH when the paired binary is absent" \
        || bad "PATH wineserver fallback did not run both bounded commands"
else
    bad "prewarm failed with a PATH-only wineserver"
fi

failed_root="$(mktemp -d)"
make_harness "$failed_root"
if TIMEOUT_FAIL_WINESERVER=1 run_prewarm "$failed_root" >/dev/null 2>&1; then
    bad "wineserver timeout was reported as successful"
else
    ok "wineserver timeout fails the prewarm"
fi

missing_root="$(mktemp -d)"
make_harness "$missing_root"
if output="$(run_prewarm_without_timeout "$missing_root" 2>&1)"; then
    bad "prewarm succeeded without timeout"
else
    printf '%s\n' "$output" | grep -Fq "required command 'timeout' not found in PATH" \
        && ok "missing timeout is rejected with a clear diagnostic" \
        || bad "missing timeout diagnostic was not reported"
fi

echo "== test-steamless-prewarm: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
