#!/bin/sh
# Remove precise-sleep-mac. Also clear the Steam launch option by hand:
#   Steam → Library → Don't Starve Together → Properties → General → Launch Options → empty
set -eu
INSTALL_DIR="$HOME/.precise-sleep"
if [ -d "$INSTALL_DIR" ]; then
    rm -rf "$INSTALL_DIR"
    echo "Removed $INSTALL_DIR"
else
    echo "Nothing to remove ($INSTALL_DIR does not exist)"
fi
echo "Now clear the launch option in Steam: Library → Don't Starve Together → Properties → General → Launch Options."
