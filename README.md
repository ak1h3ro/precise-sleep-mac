# precise-sleep-mac

**Restores a flat 60 fps in Don't Starve Together on Apple Silicon Macs and fixes the host lag in fullscreen, without `sudo` and without touching game files.**

## The symptom

- Don't Starve Together (Steam, macOS, Apple Silicon) runs just below 60 fps no matter what you do: 56.9–57 on an M4 Pro, in the menu, in an empty world, windowed and fullscreen. The game targets a flat 60 and the machine is nowhere near its limits.
- When you **host** a world and play in **fullscreen**, your own character rubber-bands and teleports as if the network were lagging.

Both come from the same place: **macOS timer coalescing**. The game's frame limiter asks the kernel to sleep until the next frame, and macOS, to save power, wakes it up late. A foreground app is woken roughly 1 ms late (the exact amount varies by machine), so a 16.67 ms frame takes 17.57 ms and 60 fps becomes 57. The server shards have it worse. Once macOS Game Mode (fullscreen) pushes them into the background band, the permitted lateness grows to **100 ms**: a 33 ms server tick arrives 100 ms late, ticks get skipped, and the host teleports.

Full write-up with measurements: [docs/investigation.md](docs/investigation.md).

## What this does

A tiny library, `precise_sleep.dylib`, is loaded into the game process at start-up via `DYLD_INSERT_LIBRARIES`, the same mechanism Steam uses for its overlay. It replaces `nanosleep()` with a version that waits on a kqueue timer flagged `NOTE_CRITICAL`. The kernel is not allowed to coalesce such a timer in any scheduling band, so the wake-up lands within ~0.05 ms of the requested deadline. The game then runs at a true 60 fps, and the shards tick on time in fullscreen whether Game Mode is on or off.

A small launcher, `precise_run`, is what Steam actually starts: it adds the library to the environment (keeping Steam's overlay), resolves the `.app` bundle to the executable and `exec`s the game in place.

The game folder and the system settings stay untouched, and nothing needs `sudo`.

## Install

Requirements: macOS on Apple Silicon (also works on Intel Macs), Steam version of the game.

### 1. Run the installer

Open Terminal (⌘ Space, type Terminal) and paste:

```sh
curl -fsSL https://raw.githubusercontent.com/ak1h3ro/precise-sleep-mac/main/precise-sleep-install.sh | sh
```

Or, from a checkout of this repository:

```sh
sh precise-sleep-install.sh            # prebuilt binaries from dist/
sh precise-sleep-install.sh --build    # compile from src/ yourself (needs Xcode Command Line Tools)
```

The installer puts two files, `precise_run` and `precise_sleep.dylib`, into `~/.precise-sleep/`, clears the download quarantine, signs them locally, runs a self-test (every line should report about 16.667 ms) and checks that the game carries the two entitlements that allow injection. The one thing it cannot do for you is the Steam launch option, so it ends by printing that line and copying it to your clipboard.

### 2. Set the Steam launch option

The line, with your username in place of `<you>`:

```
/Users/<you>/.precise-sleep/precise_run %command%
```

1. Steam → Library → right-click Don't Starve Together → Properties.
2. General tab → Launch Options field → paste the line (⌘V).
3. Close the Properties window. There is no save button; the field is applied immediately.

Steam replaces `%command%` with the game's own launch command, so what actually runs is `precise_run <path to the game .app>`. Keep it in: without it the launcher has nothing to start and the game "closes immediately".

### 3. Start the game

Launch it from Steam as usual. Options → Settings → Show FPS should now read 60. To confirm the library is really inside the game process, see Verify below.

## Verify

From a checkout:

```sh
sh precise-sleep-install.sh --check
```

If you installed with the `curl` one-liner and have no checkout:

```sh
curl -fsSL https://raw.githubusercontent.com/ak1h3ro/precise-sleep-mac/main/precise-sleep-install.sh | sh -s -- --check
```

This reports the state of the installation and the game's entitlements. Run it while the game is open and it also looks up the game's process and tells you whether the library is loaded into it.

## Uninstall

From a checkout:

```sh
sh precise-sleep-uninstall.sh
```

If you installed with the `curl` one-liner and have no checkout:

```sh
curl -fsSL https://raw.githubusercontent.com/ak1h3ro/precise-sleep-mac/main/precise-sleep-uninstall.sh | sh
```

It removes `~/.precise-sleep` and reminds you to clear the Launch Options field in Steam, which is the one step it cannot do for you. The game is back to stock, since nothing was ever placed inside its folder.

## How it works

1. Steam runs `precise_run <path to game.app>`.
2. `precise_run` prepends `precise_sleep.dylib` to `DYLD_INSERT_LIBRARIES` (Steam has already put `steamloader.dylib` and `gameoverlayrenderer.dylib` there, and they stay), resolves `Game.app` → `Game.app/Contents/MacOS/Game`, and `execv`s it. Because the PID does not change, Steam keeps tracking the game.
3. `dyld` loads the library. It carries a `__DATA,__interpose` table that redirects every call to `nanosleep` from the game's code into `precise_nanosleep`.
4. `precise_nanosleep` computes an absolute deadline on `mach_absolute_time()`, arms a kqueue `EVFILT_TIMER` with `NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_CRITICAL` for `deadline − margin`, and spins the last `margin` (adaptive, settles at ~0.1–0.2 ms). If kqueue is unavailable it falls back to `mach_wait_until`.
5. The library is inherited by the dedicated-server shards the client spawns, so they get precise ticks too. That is what fixes fullscreen hosting.

Why a compiled launcher and not a shell script: macOS strips `DYLD_*` variables from the environment of platform binaries such as `/bin/sh`, which would also drop Steam's overlay.

## Measurements (MacBook Pro M4 Pro, macOS 26)

Lateness of a 33 ms sleep (one server tick), 60 samples:

| wait primitive | normal process | Darwin-background band (`pri 4`, where Game Mode puts the shards) |
|---|---|---|
| `nanosleep` | avg 3.0 / max 5.0 ms | avg 70 / max 100 ms |
| `mach_wait_until` | avg 3.5 / max 5.0 ms | avg 79 / max 101 ms |
| realtime thread + `mach_wait_until` | avg 0.01 / max 0.02 ms | avg 76 / max 100 ms |
| kqueue timer + `NOTE_CRITICAL` | avg 0.04 / max 0.08 ms | **avg 0.03 / max 0.08 ms** |

In game: 57 → 60 fps, and no rubber-banding when hosting fullscreen with caves and Game Mode active.

## Limitations and risks

- **It is code injection into the game process.** It works only because Klei signed the game with `com.apple.security.cs.allow-dyld-environment-variables` and `com.apple.security.cs.disable-library-validation`. If a game update drops either, macOS silently ignores the library and the cap comes back. Nothing else breaks; the fix simply stops working, and `precise-sleep-install.sh --check` will tell you so.
- If Klei changes the frame limiter to something other than `nanosleep`, the interposer no longer hits it.
- If Steam changes how `%command%` is passed, the launcher may fail and the game will appear to "close immediately". Remove the launch option to get the stock behaviour back, then look at `~/.precise-sleep/precise_run.log`.
- `NOTE_CRITICAL` is in Apple's public headers and used by libdispatch (`DISPATCH_TIMER_STRICT`), but documented only as "best effort". A future macOS could restrict it for background processes.
- Every `nanosleep` in the process gets a precise wake-up, not just the frame limiter; each costs up to ~0.2 ms of spinning. On battery this means a slightly higher draw, but only for the game's three processes, which is far narrower than the system-wide alternative (`sudo sysctl -w kern.timer.coalescing_enabled=0`).
- The shards stay in the background band in fullscreen (only their *timers* are fixed). On a heavily loaded or small machine they may still starve for CPU; turning Game Mode off is then the remaining option.
- DST has no anti-cheat, and the game files are untouched, so there is nothing for the server to detect. Should an anti-cheat ever be added, injection is the first thing it would flag.
- Security software may flag `DYLD_INSERT_LIBRARIES` usage as an injection technique. It is exactly that; the code is ~150 lines and in `src/` for you to read.

## Other games

The library knows nothing about Don't Starve Together. Any Steam game on macOS that (a) paces frames with `nanosleep` and (b) carries the two entitlements above will behave the same way. `precise_run` resolves any `.app`. If you try it with another game, reports are welcome.

## The proper fix

This library is a workaround. The real fix belongs inside the game: Klei's macOS platform layer should arm the frame and tick timers with `NOTE_CRITICAL` (or `DISPATCH_TIMER_STRICT`), which is about twenty lines of code. If you want to file or support a bug report with Klei, [docs/investigation.md](docs/investigation.md) has the measurements and the explanation it needs.

## Build

```sh
sh build.sh      # universal arm64 + x86_64 binaries into dist/, ad-hoc signed
```

## License

MIT.
