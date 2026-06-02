#!/bin/bash

SLSDIR="$HOME/.local/share/SLSsteam"
SLSLIB="$SLSDIR/SLSsteam.so"

uninstall()
{
	echo "Uninstalling SLSsteam..."
	
	# Remove from bashrc
	if [ -f "$HOME/.bashrc" ]; then
		if grep -q "SLSsteam/path" "$HOME/.bashrc"; then
			echo "Removing from ~/.bashrc..."
			sed -i '/# SLSsteam: Add wrapper to PATH/d' "$HOME/.bashrc"
			sed -i '\|SLSsteam/path|d' "$HOME/.bashrc"
		fi
	fi
	
	# Remove local .desktop file if exists
	if [ -f "$HOME/.local/share/applications/steam.desktop" ]; then
		if grep -q "SLSsteam" "$HOME/.local/share/applications/steam.desktop"; then
			echo "Removing local steam.desktop..."
			rm -f "$HOME/.local/share/applications/steam.desktop"
		fi
	fi
	
	# Restore system-wide .desktop if modified
	if [ -f "/usr/share/applications/steam.desktop" ] && grep -q "SLSsteam" "/usr/share/applications/steam.desktop" 2>/dev/null; then
		if [ -f "/usr/share/applications/steam.desktop.slssteam-backup" ]; then
			echo "Restoring system steam.desktop (requires sudo)..."
			sudo cp "/usr/share/applications/steam.desktop.slssteam-backup" \
			        "/usr/share/applications/steam.desktop"
			sudo rm "/usr/share/applications/steam.desktop.slssteam-backup"
			echo "✓ Restored system steam.desktop"
		else
			echo "⚠️  System steam.desktop is modified but no backup found"
			echo "    You may need to reinstall Steam to restore it"
		fi
	fi
	
	# Check if /usr/games/steam was modified (legacy method)
	if [ -f "/usr/games/steam" ] && grep -q "SLSsteam" "/usr/games/steam" 2>/dev/null; then
		echo "Found legacy Steam script modification..."
		if [ -f "/usr/games/steam.slsteam-backup" ]; then
			echo "Restoring original Steam script (requires sudo)..."
			sudo cp "/usr/games/steam.slsteam-backup" "/usr/games/steam"
			sudo rm "/usr/games/steam.slsteam-backup"
			echo "✓ Restored /usr/games/steam"
		else
			echo "⚠️  Legacy modification found but no backup exists"
		fi
	fi
	
	# Remove SLSsteam directory
	if [ -d "$SLSDIR" ]; then
		echo "Removing $SLSDIR..."
		rm -rf "$SLSDIR"
	fi
	
	echo ""
	echo "✓ Uninstall complete!"
	echo "  Restart your terminal and Steam for changes to take effect."
	echo ""
}

install_slssteam()
{
	LIB="./bin/SLSsteam.so"
	
	if [ ! -f "$LIB" ]; then
		echo "ERROR: $LIB not found!"
		echo ""
		echo "If you're a developer, build it first:"
		echo "  ./build-docker.sh  # For releases (requires Podman/Docker)"
		echo "  make               # For local testing"
		echo ""
		exit 1
	fi

	echo "Installing SLSsteam libraries..."
	mkdir -p "$SLSDIR" || exit 1
	cp -v ./bin/* "$SLSDIR/"
	echo ""
}

create_steam_wrapper()
{
	echo "Creating Steam wrapper with SLSsteam injection..."
	
	# Create wrapper directory
	mkdir -p "$SLSDIR/path"
	
	# Create wrapper script
	cat > "$SLSDIR/path/steam" << 'EOF'
#!/bin/sh
# SLSsteam wrapper - injects library via LD_AUDIT
LD_AUDIT="$HOME/.local/share/SLSsteam/library-inject.so:$HOME/.local/share/SLSsteam/SLSsteam.so" exec /usr/games/steam "$@"
EOF
	
	chmod +x "$SLSDIR/path/steam"
	
	echo "✓ Steam wrapper created at $SLSDIR/path/steam"
	echo ""
	return 0
}

setup_path_and_desktop()
{
	echo "Setting up PATH and desktop integration..."
	
	# Add to bashrc if not already there
	if ! grep -q "SLSsteam/path" "$HOME/.bashrc" 2>/dev/null; then
		echo '' >> "$HOME/.bashrc"
		echo '# SLSsteam: Add wrapper to PATH' >> "$HOME/.bashrc"
		echo 'export PATH="$HOME/.local/share/SLSsteam/path:$PATH"' >> "$HOME/.bashrc"
		echo "✓ Added to ~/.bashrc"
	else
		echo "✓ Already in ~/.bashrc"
	fi
	
	# Modify system-wide desktop file for better DE compatibility
	if [ -f "/usr/share/applications/steam.desktop" ]; then
		# Check if already modified
		if grep -q "SLSsteam" /usr/share/applications/steam.desktop 2>/dev/null; then
			echo "✓ System steam.desktop already configured"
		else
			# Create backup if doesn't exist
			if [ ! -f "/usr/share/applications/steam.desktop.slssteam-backup" ]; then
				echo "Creating backup of system steam.desktop..."
				sudo cp /usr/share/applications/steam.desktop \
				        /usr/share/applications/steam.desktop.slssteam-backup
			fi
			
			echo "Modifying system steam.desktop (requires sudo)..."
			sudo sed -i "s|Exec=/usr/games/steam|Exec=$HOME/.local/share/SLSsteam/path/steam|g" \
				/usr/share/applications/steam.desktop
			echo "✓ Modified system steam.desktop"
		fi
	fi
	
	echo ""
	echo "Launch Steam from a new terminal or application menu."
	echo ""
	return 0
}

install_steamstub()
{
	TARGET="$1"
	HELPERSRC="./tools/steamstub-bypass"

	if [ ! -d "$HELPERSRC" ]; then
		echo "Helper scripts not found at $HELPERSRC! Skipping Steam Stub setup"
		return 1
	fi

	echo "Installing Steamless helper..."
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
	echo "========================================="
	echo "  SLSsteam Installation"
	echo "========================================="
	echo ""
	
	# Install libraries (no sudo needed)
	install_slssteam
	
	# Create wrapper instead of patching system files
	create_steam_wrapper
	
	# Setup PATH and desktop integration
	setup_path_and_desktop
	
	# Install steamless helper
	install_steamstub "$SLSDIR"

	echo "========================================="
	echo "  Installation Complete!"
	echo "========================================="
	echo ""
	echo "SLSsteam has been installed using the PATH wrapper method."
	echo ""
	echo "To use SLSsteam:"
	echo "  1. Open a NEW terminal (to load the updated PATH)"
	echo "  2. Launch Steam normally: 'steam' or use the application menu"
	echo ""
	echo "Or run directly: ~/.local/share/SLSsteam/path/steam"
	echo ""
}

if [[ $# -lt 1 ]]; then
	echo "Usage: $0 install|uninstall"
	exit 0
fi

if [ "$1" == "install" ]; then
	install_all
elif [ "$1" == "uninstall" ]; then
	uninstall
else
	echo "Unknown command $1!"
	exit 1
fi
