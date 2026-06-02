#!/bin/bash

SLSDIR="$HOME/.local/share/SLSsteam"
SLSPATH="$SLSDIR/path"
SLSLIB="$SLSDIR/SLSsteam.so"
SLSAUDIT="LD_AUDIT=\"$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so\""

uninstall()
{
	test -f "$SLSDIR/steam-jupiter.bak" && sudo cp -v "$SLSDIR/steam-jupiter.bak" "$(realpath "$(type -P steam-jupiter)")"
	rm -v "$HOME/.config/fish/conf.d/SLSsteam.fish" 2> /dev/null
	sed -i '/export PATH="$HOME\/.local\/share\/SLSsteam\/path:$PATH"/d' "$HOME/.bashrc" "$HOME/.zshrc" 2> /dev/null
	rm -v "$HOME/.local/share/applications/steam.desktop" 2> /dev/null
	rm -v "$HOME/.local/share/applications/steam-native.desktop" 2> /dev/null
	rm -rvf "$SLSDIR"
	echo "Uninstall done!"
}

install_wrapper()
{
	EXE="$1"
	FPATH="$(type -P $EXE 2>/dev/null)"

	if [ -z "$FPATH" ]; then
		echo "$EXE not found in path! Skipping"
		return 1
	fi

	DIRNAME="$(dirname "$FPATH")"
	if [ "$DIRNAME" = "$SLSPATH" ]; then
		echo "$EXE wrapper already installed! Skipping"
		return 0
	fi

	echo -e "#!/bin/sh\n$SLSAUDIT \"$FPATH\"" > "$SLSPATH/$EXE"
	chmod u+x "$SLSPATH/$EXE"

	echo "Created wrapper for $FPATH at $SLSPATH/$EXE"
	return 0
}

install_desktop_file()
{
	NAME="$1.desktop"
	USR_APP_DIR="$HOME/.local/share/applications"
	
	APP_DIR=""
	for dir in "/usr/share/applications" "/usr/local/share/applications" "$HOME/.local/share/applications"; do
		if [ -f "$dir/$NAME" ]; then
			APP_DIR="$dir"
			break
		fi
	done

	if [ -z "$APP_DIR" ]; then
		echo "$NAME not found in applications! Skipping"
		return 1
	fi

	if [ ! -d "$USR_APP_DIR" ]; then
		mkdir -p "$USR_APP_DIR" || return 1
	fi

	if [ "$APP_DIR" != "$USR_APP_DIR" ] || ! grep -q "$SLSAUDIT" "$USR_APP_DIR/$NAME"; then
		cp "$APP_DIR/$NAME" "$USR_APP_DIR/$NAME.tmp"
		sed -i "s|^Exec=\(.*steam.*\)|Exec=env $SLSAUDIT \1|" "$USR_APP_DIR/$NAME.tmp"
		mv "$USR_APP_DIR/$NAME.tmp" "$USR_APP_DIR/$NAME"
		echo "Created $USR_APP_DIR/$NAME"
	else
		echo "$NAME is already patched! Skipping"
	fi
}

install_path()
{
	SHELL_NAME="$(basename "$SHELL")"
	CMD="export PATH=\"$SLSPATH:\$PATH\""

	if [ "$SHELL_NAME" = "fish" ]; then
		mkdir -p "$HOME/.config/fish/conf.d"
		SLSSTEAM_FISH="$HOME/.config/fish/conf.d/SLSsteam.fish"
		if [ ! -f "$SLSSTEAM_FISH" ]; then
			echo "set -gx PATH \"$SLSPATH\" \$PATH" > "$SLSSTEAM_FISH"
			echo "Wrote path config to $SLSSTEAM_FISH"
			echo "Relog for changes to take effect!"
		fi
	elif [ "$SHELL_NAME" = "bash" ] || [ "$SHELL_NAME" = "zsh" ]; then
		RC_FILE="$HOME/.${SHELL_NAME}rc"
		if ! grep -q "$SLSPATH" "$RC_FILE" 2>/dev/null; then
			echo "" >> "$RC_FILE"
			echo "$CMD" >> "$RC_FILE"
			echo "Wrote path config to $RC_FILE"
			echo "Relog or run 'source $RC_FILE' for changes to take effect!"
		fi
	else
		echo "User is on unsupported shell ($SHELL_NAME)! Please add $SLSPATH to your PATH manually."
	fi
}

install_slssteam()
{
	LIB="./bin/SLSsteam.so"
	if [ ! -f "$LIB" ]; then
		echo "$LIB not found! Did you run setup.sh in the correct directory?"
		exit 1
	fi

	mkdir -p "$SLSDIR" || exit 1
	mkdir -p "$SLSPATH" || exit 1

	cp -v ./bin/* "$SLSDIR/"
}

install_steamstub()
{
	TARGET="$1"
	HELPERSRC="./tools/steamstub-bypass"

	if [ ! -d "$HELPERSRC" ]; then
		echo "Helper scripts not found at $HELPERSRC! Skipping Steam Stub setup"
		return 1
	fi

	mkdir -p "$TARGET/steamstub-bypass"
	cp -v "$HELPERSRC/run-steamless.sh"     "$TARGET/steamstub-bypass/"
	cp -v "$HELPERSRC/install-steamless.sh" "$TARGET/steamstub-bypass/"
	chmod u+x "$TARGET/steamstub-bypass/run-steamless.sh" \
	          "$TARGET/steamstub-bypass/install-steamless.sh"

	bash "$TARGET/steamstub-bypass/install-steamless.sh" \
		--target "$TARGET/steamless-bin"
}

install_all()
{
	install_slssteam
	install_steamstub "$SLSDIR"

	install_path
	
	install_wrapper steam
	install_wrapper steam-runtime
	install_wrapper steam-native

	install_desktop_file steam
	install_desktop_file steam-native

	echo "Install script done! If any wrappers or .desktop files have been created it was successful."
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
