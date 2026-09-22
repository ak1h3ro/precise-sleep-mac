#!/bin/sh
# precise-sleep-mac installer
#   sh precise-sleep-install.sh            install prebuilt binaries (from ./dist if run from a checkout, else download)
#   sh precise-sleep-install.sh --build    compile from ./src instead (needs Xcode Command Line Tools)
#   sh precise-sleep-install.sh --check    only report the current state, change nothing
set -eu

REPO="ak1h3ro/precise-sleep-mac"
RAW_BASE="https://raw.githubusercontent.com/$REPO/main/dist"
RAW_SELF="https://raw.githubusercontent.com/$REPO/main/precise-sleep-install.sh"
RAW_UNINSTALL="https://raw.githubusercontent.com/$REPO/main/precise-sleep-uninstall.sh"
INSTALL_DIR="$HOME/.precise-sleep"
DST_APP="$HOME/Library/Application Support/Steam/steamapps/common/Don't Starve Together/dontstarve_steam.app"
HERE="$(cd "$(dirname "$0")" 2>/dev/null && pwd || echo "")"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }
fail() { printf '  \033[31m✗\033[0m %s\n' "$*"; }

mode="install"
[ "${1:-}" = "--build" ] && mode="build"
[ "${1:-}" = "--check" ] && mode="check"

[ "$(uname -s)" = "Darwin" ] || { fail "This is a macOS-only tool."; exit 1; }

check_dst() {
    bold "Don't Starve Together"
    if [ ! -d "$DST_APP" ]; then
        warn "not found at the default Steam location; the library works with any game that has the entitlements below"
        return 0
    fi
    ok "found: $DST_APP"
    ents="$(codesign -d --entitlements :- "$DST_APP" 2>/dev/null || true)"
    for e in com.apple.security.cs.allow-dyld-environment-variables com.apple.security.cs.disable-library-validation; do
        case "$ents" in *"$e"*) ok "entitlement $e";; *) fail "entitlement $e MISSING — injection will be silently ignored by macOS";; esac
    done
}

check_install() {
    bold "Installation"
    if [ -x "$INSTALL_DIR/precise_run" ] && [ -r "$INSTALL_DIR/precise_sleep.dylib" ]; then
        ok "installed in $INSTALL_DIR"
        codesign -v "$INSTALL_DIR/precise_sleep.dylib" 2>/dev/null && ok "library signature valid" || warn "library signature invalid — run the installer again"
        if xattr -p com.apple.quarantine "$INSTALL_DIR/precise_sleep.dylib" >/dev/null 2>&1; then fail "library is quarantined — run the installer again"; fi
        [ -f "$INSTALL_DIR/precise_run.log" ] && { echo "  last launcher log lines:"; tail -n 3 "$INSTALL_DIR/precise_run.log" | sed 's/^/    /'; }
    else
        warn "not installed"
    fi
    pid="$(pgrep -f 'MacOS/dontstarve_steam' 2>/dev/null | head -n1 || true)"
    if [ -n "$pid" ]; then
        if lsof -p "$pid" 2>/dev/null | grep -q precise_sleep.dylib; then ok "DST is running (pid $pid) WITH the library loaded"
        else fail "DST is running (pid $pid) WITHOUT the library — check the Steam launch option"; fi
    fi
}

if [ "$mode" = "check" ]; then check_dst; check_install; exit 0; fi

bold "Installing to $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"

if [ "$mode" = "build" ]; then
    command -v clang >/dev/null || { fail "clang not found — install Xcode Command Line Tools (xcode-select --install) or run without --build"; exit 1; }
    [ -f "$HERE/src/precise_sleep.c" ] || { fail "run --build from a checkout of the repository"; exit 1; }
    clang -arch x86_64 -arch arm64 -O2 -dynamiclib -o "$INSTALL_DIR/precise_sleep.dylib" "$HERE/src/precise_sleep.c"
    clang -arch x86_64 -arch arm64 -O2 -o "$INSTALL_DIR/precise_run" "$HERE/src/precise_run.c"
    ok "built from source"
elif [ -n "$HERE" ] && [ -f "$HERE/dist/precise_sleep.dylib" ] && [ -f "$HERE/dist/precise_run" ]; then
    cp -f "$HERE/dist/precise_sleep.dylib" "$HERE/dist/precise_run" "$INSTALL_DIR/"
    ok "copied prebuilt binaries from $HERE/dist"
else
    for f in precise_sleep.dylib precise_run; do
        curl -fsSL "$RAW_BASE/$f" -o "$INSTALL_DIR/$f" || { fail "download of $f failed"; exit 1; }
    done
    ok "downloaded prebuilt binaries"
fi

chmod 755 "$INSTALL_DIR/precise_run"
# Downloaded files carry a quarantine flag; macOS refuses to inject quarantined libraries. Clear it and re-sign locally.
xattr -c "$INSTALL_DIR/precise_sleep.dylib" "$INSTALL_DIR/precise_run" 2>/dev/null || true
codesign -s - -f "$INSTALL_DIR/precise_sleep.dylib" "$INSTALL_DIR/precise_run" >/dev/null 2>&1
ok "quarantine cleared, ad-hoc signed"

bold "Self-test (the library should report 16.667 ms for every line)"
if "$INSTALL_DIR/precise_run" --selftest 2>&1 | sed 's/^/  /' | tee /tmp/precise_sleep_selftest.$$ ; then :; fi
grep -q 'got 16.66' /tmp/precise_sleep_selftest.$$ && ok "library loads and is precise" || fail "self-test did not produce timings — the library did not load"
rm -f /tmp/precise_sleep_selftest.$$

check_dst

LAUNCH_OPTION="$INSTALL_DIR/precise_run %command%"
bold "Last step — set the Steam launch option"
cat <<MSG
  Steam → Library → right-click Don't Starve Together → Properties → General → Launch Options:

      $LAUNCH_OPTION

MSG
if command -v pbcopy >/dev/null 2>&1; then printf '%s' "$LAUNCH_OPTION" | pbcopy && ok "copied to clipboard"; fi
echo "  Then start the game."
# The hints must match how this script was invoked: a curl | sh user has no local copy.
if [ -n "$HERE" ] && [ -f "$HERE/precise-sleep-install.sh" ]; then
    echo "  Verify any time with:  sh precise-sleep-install.sh --check"
    echo "  Remove with:           sh precise-sleep-uninstall.sh   (and clear the launch option)"
else
    echo "  Verify any time with:  curl -fsSL $RAW_SELF | sh -s -- --check"
    echo "  Remove with:           curl -fsSL $RAW_UNINSTALL | sh   (and clear the launch option)"
fi
