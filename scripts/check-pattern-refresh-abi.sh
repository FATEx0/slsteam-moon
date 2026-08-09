#!/usr/bin/env bash
# Reject release helpers that cannot start on the project's glibc 2.34 floor.

set -euo pipefail

BINARY="${1:-bin/pattern-refresh}"
READELF_BIN="${READELF:-readelf}"

if [ ! -s "$BINARY" ]; then
	echo "pattern refresh helper is missing or empty: $BINARY" >&2
	exit 1
fi
if [ ! -x "$READELF_BIN" ] && ! command -v "$READELF_BIN" >/dev/null 2>&1; then
	echo "readelf is required to verify the pattern refresh ABI" >&2
	exit 1
fi

ABI_OUTPUT="$(mktemp)"
cleanup() { rm -f "$ABI_OUTPUT"; }
trap cleanup EXIT

if ! "$READELF_BIN" --version-info "$BINARY" >"$ABI_OUTPUT" 2>&1; then
	echo "could not inspect pattern refresh ABI: $BINARY" >&2
	exit 1
fi

VERSIONS="$(sed -n \
	's/.*Name: GLIBC_\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2/p' \
	"$ABI_OUTPUT")"
if [ -z "$VERSIONS" ]; then
	echo "pattern refresh helper exposes no verifiable GLIBC requirements" >&2
	exit 1
fi

while read -r major minor; do
	case "$major:$minor" in
		*[!0-9:]*|:*)
			echo "invalid GLIBC requirement in $BINARY" >&2
			exit 1
			;;
	esac
	if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 34 ]; }; then
		echo "$BINARY requires GLIBC_${major}.${minor}; maximum allowed is GLIBC_2.34" >&2
		exit 1
	fi
done <<< "$VERSIONS"

echo "pattern refresh ABI is compatible with GLIBC_2.34"
