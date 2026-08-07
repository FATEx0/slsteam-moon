# launcher-shim.lib.sh — safe, sourceable helpers for the distro Steam launcher.
#
# This wraps package-owned launchers (/usr/bin/steam, /usr/games/steam, or
# /usr/local/bin/steam). It deliberately never touches Steam's own steam.sh,
# which is verified and re-extracted by Steam's bootstrapper.
#
# Callers may override LS_* variables before sourcing this file. No top-level
# command mutates the filesystem.
: "${LS_TAG:=# slsteam-moon system launcher shim}"
: "${LS_SLSDIR:=$HOME/.local/share/SLSsteam}"
: "${LS_BACKUP_ROOT:=$LS_SLSDIR/system-launcher-backup}"
: "${LS_SUDO:=}"

if ! declare -p LS_LAUNCHER_DIRS >/dev/null 2>&1; then
	LS_LAUNCHER_DIRS=(/usr/bin /usr/games /usr/local/bin)
fi

# Resolve the initialized Steam data root. Production callers use Valve's
# canonical ~/.steam/steam symlink; tests and diagnostics may inject a root
# directly so eligibility never depends on the host's Steam installation.
ls_steam_root() {
	local root
	if [ -n "${LS_STEAM_ROOT:-}" ]; then
		printf '%s\n' "$LS_STEAM_ROOT"
		return 0
	fi
	# In production, Valve owns this path as a symlink into the actual Steam
	# installation. Do not accept a merely matching directory: a partial or
	# foreign tree can contain an executable steam.sh without being bootstrapped.
	[ -L "$HOME/.steam/steam" ] || return 1
	root="$(readlink -e -q "$HOME/.steam/steam" 2>/dev/null || true)"
	[ -n "$root" ] || return 1
	printf '%s\n' "$root"
}

# A distro launcher is eligible only after Steam has bootstrapped its real data
# root. This is the guard that keeps Debian's pre-bootstrap `Install Steam`
# package stub usable: an absent/non-executable steam.sh means no replacement.
ls_steam_bootstrapped() {
	local root
	root="$(ls_steam_root)" || return 1
	[ -d "$root" ] && [ -f "$root/steam.sh" ] && [ -x "$root/steam.sh" ]
}

ls_launcher_eligible() {
	[ -f "$1" ] && [ -x "$1" ] && ls_steam_bootstrapped
}

ls_detect_launchers() {
	local dir launcher
	for dir in "${LS_LAUNCHER_DIRS[@]}"; do
		launcher="${dir%/}/steam"
		[ -f "$launcher" ] && printf '%s\n' "$launcher"
	done
}

ls_is_our_shim() {
	[ -f "$1" ] && head -3 "$1" 2>/dev/null | grep -qF "$LS_TAG"
}

ls_is_injected_wrapper() {
	local candidate="$1" wrapper="$HOME/.local/share/SLSsteam/path/steam"
	local resolved_candidate resolved_wrapper
	resolved_candidate="$(readlink -f "$candidate" 2>/dev/null || true)"
	resolved_wrapper="$(readlink -f "$wrapper" 2>/dev/null || true)"
	[ -n "$resolved_candidate" ] && [ -n "$resolved_wrapper" ] && \
		[ "$resolved_candidate" = "$resolved_wrapper" ]
}

# The generated shim records the exact original it captured. Older installs did
# not emit SLSM_ORIG, but embedded the same absolute path in their literal
# fallback command; accept that historical form only when it matches exactly.
ls_shim_references_backup() {
	local launcher="$1" backup="$2"
	[ -f "$launcher" ] || return 1
	grep -Fqx -- "SLSM_ORIG=\"$backup\"" "$launcher" 2>/dev/null && return 0
	grep -Fqx -- "exec \"$backup\" \"\$@\"" "$launcher" 2>/dev/null
}

ls_backup_is_usable() {
	[ -f "$1" ] && [ -x "$1" ] || return 1
	! ls_is_our_shim "$1" || return 1
	! ls_is_injected_wrapper "$1"
}

# Run backup-tree mutations with the configured privilege prefix. The live
# launcher writer has its own privilege handling; this helper covers mirrored
# backup migration, refresh, and mode normalization as well.
ls_run_privileged() {
	if [ -n "${LS_SUDO:-}" ]; then
		$LS_SUDO "$@"
	else
		"$@"
	fi
}

ls_wrapped_paths() {
	local launcher
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" && printf '%s\n' "$launcher"
	done < <(ls_detect_launchers)
}

ls_detected_launcher_count() {
	local launcher count=0
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		count=$((count + 1))
	done < <(ls_detect_launchers)
	printf '%s\n' "$count"
}

ls_wrapped_launcher_count() {
	local launcher count=0
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" || continue
		count=$((count + 1))
	done < <(ls_detect_launchers)
	printf '%s\n' "$count"
}

ls_legacy_backup_is_unambiguous() {
	local legacy="$1" references=0 launcher
	ls_backup_is_usable "$legacy" || return 1
	# A flat backup is safe only when exactly one surviving shim explicitly
	# references this exact file. Other mirrored shims do not make that owner
	# ambiguous; they are unrelated paths that can be reconciled first.
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" || continue
		ls_shim_references_backup "$launcher" "$legacy" || continue
		references=$((references + 1))
	done < <(ls_detect_launchers)
	[ "$references" -eq 1 ]
}

ls_legacy_backup_is_unambiguous_restore() {
	local legacy="$1" references=0 launcher
	ls_backup_is_usable "$legacy" || return 1
	# Ambiguity is candidate-specific: unrelated surviving shims do not own this
	# flat backup. Reject only when zero or multiple shims reference this path.
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" || continue
		ls_shim_references_backup "$launcher" "$legacy" || continue
		references=$((references + 1))
	done < <(ls_detect_launchers)
	[ "$references" -eq 1 ]
}

ls_backup_path() {
	local launcher="$1" relative
	case "$launcher" in
		/*) relative="${launcher#/}" ;;
		*)  relative="$launcher" ;;
	esac
	printf '%s/%s.orig\n' "${LS_BACKUP_ROOT%/}" "$relative"
}

ls_legacy_backup_path() {
	printf '%s/%s.orig\n' "${LS_BACKUP_ROOT%/}" "$(basename -- "$1")"
}

ls_shim_content() {
	local backup="$1"
	cat <<EOF
#!/bin/sh
$LS_TAG
# Managed by slsteam-moon. Run the uninstaller to restore the package launcher.
SLSM_WRAPPER="\${HOME}/.local/share/SLSsteam/path/steam"
SLSM_ORIG="$backup"
SLSM_TAG="$LS_TAG"
SLSM_BOOTSTRAP_ROOT=""
SLSM_BOOTSTRAPPED=0
if [ -L "\${HOME}/.steam/steam" ]; then
	SLSM_BOOTSTRAP_ROOT="\$(readlink -e "\${HOME}/.steam/steam" 2>/dev/null || true)"
	if [ -n "\$SLSM_BOOTSTRAP_ROOT" ] && \
	   [ -f "\$SLSM_BOOTSTRAP_ROOT/steam.sh" ] && \
	   [ -x "\$SLSM_BOOTSTRAP_ROOT/steam.sh" ]; then
		SLSM_BOOTSTRAPPED=1
	fi
fi
SLSM_ORIG_USABLE=0
SLSM_ORIG_REAL=""
SLSM_WRAPPER_REAL=""
if [ -f "\$SLSM_ORIG" ] && [ -x "\$SLSM_ORIG" ]; then
	SLSM_ORIG_REAL="\$(readlink -f "\$SLSM_ORIG" 2>/dev/null || true)"
	SLSM_WRAPPER_REAL="\$(readlink -f "\$SLSM_WRAPPER" 2>/dev/null || true)"
	if [ -n "\$SLSM_ORIG_REAL" ] && \
	   [ "\$SLSM_ORIG_REAL" != "\$SLSM_WRAPPER_REAL" ] && \
	   ! head -3 "\$SLSM_ORIG" 2>/dev/null | grep -qF "\$SLSM_TAG"; then
		SLSM_ORIG_USABLE=1
	fi
fi
if [ -x "\$SLSM_WRAPPER" ] && [ "\$SLSM_BOOTSTRAPPED" = 1 ]; then
	if [ "\$SLSM_ORIG_USABLE" = 1 ]; then
		export SLSM_STEAM_BIN="\$SLSM_ORIG"
	fi
	exec "\$SLSM_WRAPPER" "\$@"
fi
if [ "\$SLSM_ORIG_USABLE" = 1 ]; then
	exec "\$SLSM_ORIG" "\$@"
fi
for _s in "\${HOME}/.local/share/Steam/steam.sh" \
          "\${HOME}/.steam/steam/steam.sh" \
          "\${HOME}/.steam/debian-installation/steam.sh"; do
	[ -f "\$_s" ] && [ -x "\$_s" ] && exec "\$_s" "\$@"
done
echo "steam: no usable launcher found (slsteam-moon shim)" >&2
exit 127
EOF
}

ls_write_shim() {
	local launcher="$1" backup="$2" temp privilege rc
	if [ -w "$launcher" ]; then
		privilege=""
	else
		privilege="$LS_SUDO"
		[ -n "$privilege" ] || return 1
	fi
	temp="$(mktemp 2>/dev/null)" || return 1
	if ! ls_shim_content "$backup" > "$temp"; then
		rm -f -- "$temp"
		return 1
	fi
	if [ -n "$privilege" ]; then
		$privilege install -m 0755 "$temp" "$launcher" 2>/dev/null
		rc=$?
	else
		install -m 0755 "$temp" "$launcher" 2>/dev/null
		rc=$?
	fi
	rm -f -- "$temp"
	[ "$rc" -eq 0 ] || return 1
	ls_is_our_shim "$launcher"
}

ls_install_one_shim() {
	local launcher="$1" backup legacy migrated=0
	# Never capture or replace a package installer stub before Steam has created
	# its real data root. The caller can rerun after bootstrap on the same path.
	ls_launcher_eligible "$launcher" || return 1
	backup="$(ls_backup_path "$launcher")"
	legacy="$(ls_legacy_backup_path "$launcher")"
	ls_run_privileged mkdir -p "$(dirname "$backup")" 2>/dev/null || return 1

	# Migrate the pre-mirrored flat backup before an already-installed shim can
	# be mistaken for the current package launcher. Consume the flat source only
	# after the mirrored copy is safely in place so a failed migration is retryable.
	if [ ! -e "$backup" ] && ls_backup_is_usable "$legacy" && \
	   ls_shim_references_backup "$launcher" "$legacy" && \
	   ls_legacy_backup_is_unambiguous "$legacy"; then
		if ! ls_run_privileged mv -- "$legacy" "$backup" 2>/dev/null; then
			ls_run_privileged cp -- "$legacy" "$backup" 2>/dev/null || return 1
			ls_run_privileged rm -f -- "$legacy" 2>/dev/null || return 1
		fi
		ls_run_privileged chmod 0755 "$backup" 2>/dev/null || return 1
		migrated=1
	fi

	if ls_is_our_shim "$launcher"; then
		# Refresh an existing shim only from the exact backup path embedded in
		# that shim. An executable mirrored file is not authoritative merely
		# because it has the expected basename.
		if [ "$migrated" -eq 0 ] && ! ls_shim_references_backup "$launcher" "$backup"; then
			# A legacy reference is safe only when the initial migration moved
			# that exact file into an absent mirror. If migration did not happen,
			# an existing mirror is an association conflict; do not overwrite it.
			return 1
		fi
		ls_run_privileged chmod 0755 "$backup" 2>/dev/null || return 1
		ls_backup_is_usable "$backup" || return 1
		ls_write_shim "$launcher" "$backup"
		return $?
	fi

	# A package update can replace our shim with a new genuine launcher. Keep the
	# captured original in sync, but never copy a shim into the backup.
	if [ ! -f "$backup" ] || ! cmp -s "$launcher" "$backup" 2>/dev/null; then
		ls_run_privileged cp -- "$launcher" "$backup" 2>/dev/null || return 1
	fi
	# The launcher candidate was executable, so its captured original must stay
	# executable even when a mirrored backup predates the current install.
	ls_run_privileged chmod 0755 "$backup" 2>/dev/null || return 1
	ls_backup_is_usable "$backup" || return 1
	ls_write_shim "$launcher" "$backup"
}

ls_install_shims() {
	local launcher detected=0 failures=0
	# Existing shims must be reconciled first. This lets an unambiguous flat
	# backup be assigned to the one surviving shim before a vanilla path is
	# captured as a new launcher.
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		detected=$((detected + 1))
		ls_is_our_shim "$launcher" || continue
		if ! ls_install_one_shim "$launcher"; then
			failures=$((failures + 1))
		fi
	done < <(ls_detect_launchers)
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" && continue
		if ! ls_install_one_shim "$launcher"; then
			failures=$((failures + 1))
		fi
	done < <(ls_detect_launchers)
	[ "$detected" -gt 0 ] && [ "$failures" -eq 0 ]
}

ls_restore_one() {
	local launcher="$1" backup="$2" privilege
	if [ -w "$launcher" ]; then
		privilege=""
	else
		privilege="$LS_SUDO"
		[ -n "$privilege" ] || return 1
	fi
	if [ -n "$privilege" ]; then
		$privilege install -m 0755 "$backup" "$launcher" 2>/dev/null || return 1
	else
		install -m 0755 "$backup" "$launcher" 2>/dev/null || return 1
	fi
	! ls_is_our_shim "$launcher"
}

ls_restore_shims() {
	local launcher backup legacy left progress
	while :; do
		left=0
		progress=0
		while IFS= read -r launcher; do
			[ -n "$launcher" ] || continue
			ls_is_our_shim "$launcher" || continue
			backup="$(ls_backup_path "$launcher")"
			# Never install a managed or otherwise unusable mirrored file over the
			# live launcher. If one exists, clear it and try an exactly-associated
			# legacy flat backup instead.
			if [ -f "$backup" ] && \
			   { ! ls_backup_is_usable "$backup" || \
			     ! ls_shim_references_backup "$launcher" "$backup"; }; then
				backup=""
			fi
			if [ -z "$backup" ] || [ ! -f "$backup" ]; then
				legacy="$(ls_legacy_backup_path "$launcher")"
				if ls_legacy_backup_is_unambiguous_restore "$legacy" && \
				   ls_shim_references_backup "$launcher" "$legacy"; then
					backup="$legacy"
				else
					backup=""
				fi
			fi
			if [ -n "$backup" ] && [ -f "$backup" ] && \
			   ls_backup_is_usable "$backup" && ls_restore_one "$launcher" "$backup"; then
				progress=1
				continue
			fi
			left=1
		done < <(ls_detect_launchers)
		[ "$left" -eq 0 ] && return 0
		# A mirrored restore can make a previously ambiguous flat backup
		# unambiguous. Re-scan until no shim remains or no progress is possible.
		[ "$progress" -eq 1 ] || return 1
	done
}
