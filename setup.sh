#!/bin/bash

SLSDIR="$HOME/.local/share/SLSsteam"
SLSLIB="$SLSDIR/SLSsteam.so"

# ============================================================================
# Pretty output (colors + box-drawing). Falls back to plain text when stdout
# is not a TTY or the terminal does not advertise colour support.
#
# Palette: "moonlit night" — cool blues for structure, silver-white for the
# moon glyph, standard semantic colours for status. Uses 256-colour escapes
# when the terminal supports them, otherwise degrades to 8-colour ANSI.
# ============================================================================

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != "dumb" ]; then
	# Detect 256-colour support. tput is the reliable check; default to true
	# if tput is unavailable but COLORTERM looks modern.
	if command -v tput >/dev/null 2>&1 && [ "$(tput colors 2>/dev/null || echo 0)" -ge 256 ]; then
		HAS_256=1
	elif [ "${COLORTERM:-}" = "truecolor" ] || [ "${COLORTERM:-}" = "24bit" ]; then
		HAS_256=1
	else
		HAS_256=0
	fi

	BOLD=$'\033[1m'
	DIM=$'\033[2m'
	NC=$'\033[0m'

	if [ "$HAS_256" = 1 ]; then
		# Moon-themed accents.
		MOON=$'\033[38;5;153m'      # pale silver-blue — primary brand colour
		NIGHT=$'\033[38;5;75m'      # cool steel blue  — section headers / info
		HALO=$'\033[38;5;231m'      # bright white     — moon glyph highlight
		MUTED=$'\033[38;5;110m'     # dusk blue        — hints / separators
		# Semantic status colours stay standard so users read them instinctively.
		GREEN=$'\033[38;5;114m'     # soft sage green
		YELLOW=$'\033[38;5;221m'    # warm amber
		RED=$'\033[38;5;203m'       # muted coral red
	else
		MOON=$'\033[1;34m'          # bold blue
		NIGHT=$'\033[0;36m'         # cyan
		HALO=$'\033[1;37m'          # bold white
		MUTED=$'\033[0;34m'         # blue
		GREEN=$'\033[0;32m'
		YELLOW=$'\033[0;33m'
		RED=$'\033[0;31m'
	fi
else
	BOLD=""
	DIM=""
	NC=""
	MOON=""
	NIGHT=""
	HALO=""
	MUTED=""
	GREEN=""
	YELLOW=""
	RED=""
fi

print_banner() {
	echo ""
	echo -e "${MOON}${BOLD}"
	echo "┌─────────────────────────────────────────────────────────┐"
	printf "│             ${HALO}${BOLD}◯${NC}${MOON}${BOLD}  slsteam-moon installer                   │\n"
	echo "└─────────────────────────────────────────────────────────┘"
	echo -e "${NC}"
}

print_section() {
	echo ""
	echo -e "${NIGHT}─────────────────────────────────────────────────────────${NC}"
	echo -e "${NIGHT}${BOLD}❯ $1${NC}"
	echo -e "${NIGHT}─────────────────────────────────────────────────────────${NC}"
}

log_info()    { echo -e "${NIGHT}→${NC} $1"; }
log_success() { echo -e "${GREEN}✓${NC} $1"; }
log_warn()    { echo -e "${YELLOW}⚠${NC} $1"; }
log_error()   { echo -e "${RED}✗${NC} $1"; }
log_step()    { echo -e "${MOON}•${NC} $1"; }

# Make sure no Steam process is running so the wrapper / desktop entry takes
# effect on next launch. Tries graceful shutdown first, falls back to SIGKILL.
kill_steam() {
	# Only do anything if Steam is actually running.
	if ! pgrep -x steam >/dev/null 2>&1 \
	   && ! pgrep -f '/usr/games/steam' >/dev/null 2>&1 \
	   && ! pgrep -f 'steamwebhelper' >/dev/null 2>&1; then
		log_success "No running Steam process detected"
		return 0
	fi

	log_info "Stopping running Steam processes"

	# Graceful shutdown via Steam's own IPC, if available.
	if command -v steam >/dev/null 2>&1; then
		steam -shutdown >/dev/null 2>&1 || true
	fi

	# Give it a moment to exit on its own.
	for _ in 1 2 3 4 5; do
		if ! pgrep -x steam >/dev/null 2>&1 \
		   && ! pgrep -f 'steamwebhelper' >/dev/null 2>&1; then
			log_success "Steam stopped"
			return 0
		fi
		sleep 1
	done

	# Polite SIGTERM.
	pkill -TERM -x steam 2>/dev/null || true
	pkill -TERM -f 'steamwebhelper' 2>/dev/null || true
	pkill -TERM -f '/usr/games/steam' 2>/dev/null || true
	sleep 2

	# Force kill anything still lingering.
	if pgrep -x steam >/dev/null 2>&1 \
	   || pgrep -f 'steamwebhelper' >/dev/null 2>&1 \
	   || pgrep -f '/usr/games/steam' >/dev/null 2>&1; then
		log_warn "Steam still running — sending SIGKILL"
		pkill -KILL -x steam 2>/dev/null || true
		pkill -KILL -f 'steamwebhelper' 2>/dev/null || true
		pkill -KILL -f '/usr/games/steam' 2>/dev/null || true
		sleep 1
	fi

	log_success "Steam stopped"
}

print_install_complete() {
	echo ""
	echo -e "${GREEN}${BOLD}"
	echo "┌─────────────────────────────────────────────────────────┐"
	echo "│        ✓ slsteam-moon Installation Completed!           │"
	echo "└─────────────────────────────────────────────────────────┘"
	echo -e "${NC}"
}

print_uninstall_complete() {
	echo ""
	echo -e "${GREEN}${BOLD}"
	echo "┌─────────────────────────────────────────────────────────┐"
	echo "│               ✓ Uninstall Complete!                     │"
	echo "└─────────────────────────────────────────────────────────┘"
	echo -e "${NC}"
	echo ""
	echo -e "   Restart your terminal and Steam for changes to take effect."
	echo ""
}

# ============================================================================
# Core actions
# ============================================================================

uninstall()
{
	print_banner
	print_section "Uninstalling SLSsteam"

	kill_steam

	# Remove from bashrc
	if [ -f "$HOME/.bashrc" ]; then
		if grep -q "SLSsteam/path" "$HOME/.bashrc"; then
			log_info "Removing wrapper PATH entry from ~/.bashrc"
			sed -i '/# SLSsteam: Add wrapper to PATH/d' "$HOME/.bashrc"
			sed -i '\|SLSsteam/path|d' "$HOME/.bashrc"
		fi
	fi

	# Remove local .desktop file if exists
	if [ -f "$HOME/.local/share/applications/steam.desktop" ]; then
		if grep -q "SLSsteam" "$HOME/.local/share/applications/steam.desktop"; then
			log_info "Removing local steam.desktop"
			rm -f "$HOME/.local/share/applications/steam.desktop"
		fi
	fi

	# Restore system-wide .desktop if modified
	if [ -f "/usr/share/applications/steam.desktop" ] && grep -q "SLSsteam" "/usr/share/applications/steam.desktop" 2>/dev/null; then
		if [ -f "/usr/share/applications/steam.desktop.slssteam-backup" ]; then
			log_info "Restoring system steam.desktop (requires sudo)"
			sudo cp "/usr/share/applications/steam.desktop.slssteam-backup" \
			        "/usr/share/applications/steam.desktop"
			sudo rm "/usr/share/applications/steam.desktop.slssteam-backup"
			log_success "Restored system steam.desktop"
		else
			log_warn "System steam.desktop is modified but no backup found"
			echo "    You may need to reinstall Steam to restore it"
		fi
	fi

	# Check if /usr/games/steam was modified (legacy method)
	if [ -f "/usr/games/steam" ] && grep -q "SLSsteam" "/usr/games/steam" 2>/dev/null; then
		log_info "Found legacy Steam script modification"
		if [ -f "/usr/games/steam.slsteam-backup" ]; then
			log_info "Restoring original Steam script (requires sudo)"
			sudo cp "/usr/games/steam.slsteam-backup" "/usr/games/steam"
			sudo rm "/usr/games/steam.slsteam-backup"
			log_success "Restored /usr/games/steam"
		else
			log_warn "Legacy modification found but no backup exists"
		fi
	fi

	# Remove SLSsteam directory
	if [ -d "$SLSDIR" ]; then
		log_info "Removing $SLSDIR"
		rm -rf "$SLSDIR"
	fi

	print_uninstall_complete
}

install_slssteam()
{
	LIB="./bin/SLSsteam.so"

	if [ ! -f "$LIB" ]; then
		log_error "$LIB not found"
		echo ""
		echo "   If you're a developer, build it first:"
		echo -e "     ${GREEN}./build-docker.sh${NC}  ${MUTED}# For releases (requires Podman/Docker)${NC}"
		echo -e "     ${GREEN}make${NC}               ${MUTED}# For local testing${NC}"
		echo ""
		exit 1
	fi

	log_info "Installing SLSsteam libraries"
	mkdir -p "$SLSDIR" || exit 1
	cp -v ./bin/* "$SLSDIR/" | sed "s|^|   ${MUTED}${NC}|"
	log_success "Libraries installed at $SLSDIR"
	echo ""
}

create_steam_wrapper()
{
	log_info "Creating Steam wrapper with SLSsteam injection"

	# Create wrapper directory
	mkdir -p "$SLSDIR/path"

	# Create wrapper script
	cat > "$SLSDIR/path/steam" << 'EOF'
#!/bin/sh
# SLSsteam wrapper - injects via LD_AUDIT (rtld-audit).
# Loading SLSsteam.so as an audit module keeps it (and the protobuf /
# yaml-cpp / libstdc++ symbols it statically links) in the linker's
# separate auditing namespace, so they cannot interpose on the copies
# Steam's own libraries use. library-inject.so redirects libcurl to a
# system copy and must come first in the list.
SLSDIR="$HOME/.local/share/SLSsteam"
LD_AUDIT="$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so${LD_AUDIT:+:$LD_AUDIT}" exec /usr/games/steam "$@"
EOF

	chmod +x "$SLSDIR/path/steam"

	log_success "Steam wrapper created at $SLSDIR/path/steam"
	echo ""
	return 0
}

setup_path_and_desktop()
{
	log_info "Setting up PATH and desktop integration"

	# Add to bashrc if not already there
	if ! grep -q "SLSsteam/path" "$HOME/.bashrc" 2>/dev/null; then
		echo '' >> "$HOME/.bashrc"
		echo '# SLSsteam: Add wrapper to PATH' >> "$HOME/.bashrc"
		echo 'export PATH="$HOME/.local/share/SLSsteam/path:$PATH"' >> "$HOME/.bashrc"
		log_success "Added wrapper to ~/.bashrc"
	else
		log_success "Already in ~/.bashrc"
	fi

	# Modify system-wide desktop file for better DE compatibility
	if [ -f "/usr/share/applications/steam.desktop" ]; then
		# Check if already modified
		if grep -q "SLSsteam" /usr/share/applications/steam.desktop 2>/dev/null; then
			log_success "System steam.desktop already configured"
		else
			# Create backup if doesn't exist
			if [ ! -f "/usr/share/applications/steam.desktop.slssteam-backup" ]; then
				log_info "Creating backup of system steam.desktop"
				sudo cp /usr/share/applications/steam.desktop \
				        /usr/share/applications/steam.desktop.slssteam-backup
			fi

			log_info "Modifying system steam.desktop (requires sudo)"
			sudo sed -i "s|Exec=/usr/games/steam|Exec=$HOME/.local/share/SLSsteam/path/steam|g" \
				/usr/share/applications/steam.desktop
			log_success "Modified system steam.desktop"
		fi
	fi

	echo ""
	return 0
}

install_steamstub()
{
	TARGET="$1"
	HELPERSRC="./tools/steamstub-bypass"

	if [ ! -d "$HELPERSRC" ]; then
		log_warn "Helper scripts not found at $HELPERSRC — skipping Steam Stub setup"
		return 1
	fi

	log_info "Installing Steamless helper"
	mkdir -p "$TARGET/steamstub-bypass"
	cp -v "$HELPERSRC/run-steamless.sh"     "$TARGET/steamstub-bypass/"
	cp -v "$HELPERSRC/install-steamless.sh" "$TARGET/steamstub-bypass/"
	chmod u+x "$TARGET/steamstub-bypass/run-steamless.sh" \
	          "$TARGET/steamstub-bypass/install-steamless.sh"

	echo ""
	bash "$TARGET/steamstub-bypass/install-steamless.sh" \
		--target "$TARGET/steamless-bin"
	echo ""
}

install_all()
{
	print_banner

	print_section "Stopping Steam"
	kill_steam

	print_section "Installing libraries"
	install_slssteam

	print_section "Creating Steam wrapper"
	create_steam_wrapper

	print_section "Configuring PATH & desktop entry"
	setup_path_and_desktop

	print_section "Installing Steamless helper"
	install_steamstub "$SLSDIR"

	print_install_complete
}

# ============================================================================
# Entry point
# ============================================================================

if [[ $# -lt 1 ]]; then
	print_banner
	echo -e "${BOLD}Usage:${NC}  $0 ${GREEN}install${NC} | ${GREEN}uninstall${NC}"
	echo ""
	exit 0
fi

if [ "$1" == "install" ]; then
	install_all
elif [ "$1" == "uninstall" ]; then
	uninstall
else
	log_error "Unknown command: $1"
	echo -e "${BOLD}Usage:${NC}  $0 ${GREEN}install${NC} | ${GREEN}uninstall${NC}"
	exit 1
fi
