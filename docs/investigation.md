# Don't Starve Together on MacBook Pro M4 Pro — the "57 fps" problem, diagnosis and fix

**Date:** 2026-09-22
**Machine:** MacBook Pro, Apple M4 Pro (14 CPU cores, 24 GB RAM), macOS 26.6.2 (Darwin 25.6.0)
**Displays:** Samsung Odyssey G50A 2560×1440 @ 165 Hz (main) + built-in Liquid Retina XDR 3456×2234 (ProMotion)
**Game:** Don't Starve Together, Steam build 747465 (`OSX_STEAM`), x86_64 binary → runs under Rosetta 2, renderer `GL_VERSION: 2.1 Metal - 90.5` (OpenGL-on-Metal)

---

### 1. Symptom

- FPS counter (Steam overlay and the in-game counter) shows **56.9–57 fps** everywhere: main menu, empty world, modded world, windowed and fullscreen.
- In heavy boss fights fps dropped further, down to ~40.
- The game itself is capped at 60 fps by design, so the goal was a *stable 60*, not more.

### 2. What was ruled out (each one tested, none changed the number)

| Test | Result |
|---|---|
| Fullscreen vs windowed | 57 both |
| Only the built-in display, only the external display | 57 both |
| Built-in display fixed 120 Hz instead of ProMotion | 57 |
| macOS Energy Mode: Automatic → High Power | 57 |
| macOS Game Mode (engages in fullscreen) | 57 |
| Steam launch option `-novsync` (confirmed received: `Command Line Arguments: -novsync` in `client_log.txt`) | 57 |
| All client and server mods disabled, empty world | 57 |
| `use_threaded_renderer = true` in `client.ini` (the Windows-only "Threaded Render" option) | **game crashes on start**: `Threaded Renderer: ENABLED` → `Assert failure 'BREAKPT:' at .../application.cpp(1896)`, `EXC_BAD_ACCESS`. The Mac build does not support it — this is why Klei hides the switch on Mac (`optionsscreen.lua`: `if IsWin32() then ... threadedrenderSpinner`). Reverted. |
| Hosting with caves (2 server processes) | Master and Caves shards idle at 12–20 % CPU during play — not the bottleneck |
| Thermal throttling | `pmset -g therm` → no throttling at any point |

### 3. The measurement that found it

`sample <pid> 4` (macOS stack sampler) on the game's main thread, 4 seconds, in the main menu, **before the fix**:

```
2633 samples on Main Thread
 1187 (45 %)  dontstarve_steam + 0x4191f4 → nanosleep      ← engine sleeping on purpose
  394 (15 %)  -[NSOpenGLContext flushBuffer] → CGLFlushDrawable → Metal present
 ~1050 (40 %) actual work (Lua, UI, audio, …)
```

The game spends almost half of every frame **asleep**. The CPU was never the limit (process at 25–47 % CPU). The engine is deliberately waiting between frames — a sleep-based 60 fps frame limiter.

Then the arithmetic:

```
60.00 fps  →  16.67 ms per frame  (what the engine asks for)
56.92 fps  →  17.57 ms per frame  (what the counter shows)
difference →   0.90 ms, every single frame
```

A constant ~0.9 ms overshoot on every sleep = the OS waking the thread late.

### 4. Root cause: macOS timer coalescing

Every frame DST does:

```
start = now
update game logic + render          (~6 ms on M4 Pro)
sleep until start + 16.67 ms        (frame limiter)
repeat
```

macOS does not wake a thread at the exact moment it asked for. To save battery it uses **timer coalescing**: nearby timer deadlines from all processes are shifted and fired together in one batch, so the CPU can stay in a low-power state longer. A wakeup may arrive up to a few percent late. For normal apps this is invisible. For a frame limiter it is fatal: each sleep ends ~0.9 ms late, the engine measures from the *start* of the frame so it cannot compensate, and every frame is 17.57 ms instead of 16.67 → 57 fps forever. Because the delay varies frame to frame (0.3 ms … 1.5 ms), frame times are also uneven, which is felt as micro-stutter.

Under load (boss fight) the work part grows toward 16.67 ms; the overshoot then eats the whole remaining budget and frames that miss the deadline land at 25–33 ms → 30–40 fps.

Windows games avoid this by calling `timeBeginPeriod(1)` (1 ms timer precision). macOS has no per-app equivalent — only a global kernel switch.

### 5. The fix

```bash
sudo sysctl -w kern.timer.coalescing_enabled=0
```

- `sysctl` — reads/writes kernel settings on the running system.
- `-w kern.timer.coalescing_enabled=0` — writes `0` (off) to the timer-coalescing switch.
- `sudo` — kernel settings require admin rights.

Effect: the kernel fires every timer exactly at its deadline. DST's sleep ends at 16.67 ms, frame pacing becomes even, the game runs at a true 60.

Properties:
- Lives only in memory. **Resets to `1` on every reboot.** Nothing on disk is changed.
- Undo immediately: `sudo sysctl -w kern.timer.coalescing_enabled=1`.
- Check current value: `sysctl kern.timer.coalescing_enabled`.
- Only side effect: slightly higher idle power draw (the CPU wakes more often for *everything*). Irrelevant when plugged in.

To apply automatically at every boot, install a LaunchDaemon (paste into Terminal):

```bash
sudo tee /Library/LaunchDaemons/local.timer-coalescing.plist >/dev/null <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>local.timer-coalescing</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/sbin/sysctl</string>
        <string>-w</string>
        <string>kern.timer.coalescing_enabled=0</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
EOF
sudo launchctl load /Library/LaunchDaemons/local.timer-coalescing.plist
```

Remove it later:

```bash
sudo launchctl unload /Library/LaunchDaemons/local.timer-coalescing.plist
sudo rm /Library/LaunchDaemons/local.timer-coalescing.plist
```

### 6. Results

| Scenario | Before (`coalescing_enabled=1`) | After (`coalescing_enabled=0`) |
|---|---|---|
| Main menu | 57 | **60** |
| Empty world, no mods | 56.92 | **59.95–60.3** |
| Fullscreen 2560×1440 @ 144 Hz | 57 | **60** |
| Modded world, 2× Klaus + 2× Deerclops (+ gem deer) | previously dips to ~40 in boss fights | **59.5–60.3 for the whole fight** |

### 7. Logs

#### 7.1 Per-second CPU sampler (`~/dst_sample.sh` → `~/dst_sample.log`)

Columns: client process CPU %, Master shard CPU %, Caves shard CPU %, client RSS MB, thermal state. 100 % = one full core.

**Before the fix — 10:15–10:19, modded world, mini-boss fight (fps stable at 57):**

```
10:16:33–10:16:48   client ~18 %   master 100 %   caves 100 %   ← both servers loading the world
10:17:00–10:17:14   client 100 %   master  ~9 %   caves  ~9 %   ← client loading the world (RSS 1.7 → 3.3 GB)
10:17:20–10:18:28   client 45.6 % avg (max 57 %)   master 14.3 %   caves 9.6 %   ← gameplay
```

Reading: neither server is anywhere near its limit; the client has more than half of its frame budget free — yet fps was 57. A CPU-average view cannot show a per-frame timing overshoot; the stack sample in §3 could.

**After the fix — 10:38:42–10:42:31, modded world (mods list in §8), 2× Klaus + 2× Deerclops (fps 59.5–60.3 throughout):**

```
time      client%  master%  caves%  rss_MB  therm
10:38:42    49.0     16.9    12.1    3304   ok
10:38:44    56.6     18.1    11.6    3304   ok
10:38:50    55.4     17.3    11.9    3304   ok
10:38:59    55.5     17.1    11.3    3304   ok
10:39:07    57.0     17.2    12.0    3304   ok    ← peak, all four bosses + gem deer active
10:39:10    48.4     13.9    11.0    3304   ok
10:39:13    39.0     12.4     9.9    3304   ok    ← fight over
10:42:31    40.7     12.7    11.0    3305   ok

fight window 10:38:42–10:39:10:  client avg 51.5 % (min 43.7, max 57.0) | master avg 17.1 % (max 18.7) | caves avg 11.7 % (max 12.6)
after fight 10:39:12–10:42:31:   client avg 40.0 % | master avg 12.9 % | caves avg 11.0 %
```

Reading: four bosses at once add ~10 percentage points on the client and ~4 on the Master shard. Nothing is close to saturation; thermals stayed `ok`; RAM flat.

#### 7.2 `client_log.txt` excerpts (hardware / renderer detection)

```
Don't Starve Together: 747465 OSX_STEAM
Mode: 64-bit
Threaded Renderer: DISABLED
HardwareStats: CPU Apple M4 Pro, numCores 14, RAM 24576 MB
               GPU  megsOfRam 0, name "", refreshRate 2      ← engine cannot read GPU/refresh info under Rosetta/GL
GL_RENDERER: Apple M4 Pro
GL_VERSION: 2.1 Metal - 90.5
OpenGL 3.0 not present, detecting extensions the old way
WindowManager::SetFullscreen(2560, 1440, 144)
```

#### 7.3 Threaded-renderer experiment (crash), `client_log.txt` + `~/Library/Logs/DiagnosticReports/dontstarve_steam-2026-09-22-100921.ips`

```
Threaded Renderer: ENABLED
Error during initialization!
Assert failure 'BREAKPT:' at /Users/build/jenkins-buildmaster/workspace/DST_BuildGame_OSX/source/dontstarve/application.cpp(1896)
exception: EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x8, faulting thread 0 (main)
```

#### 7.4 Command-line flags the Mac binary actually parses (`strings` on the executable)

`-alternate_gc -backup_log_count -backup_log_period -backup_logs -bind_ip -clouddir -cluster -conf_dir -disabledatacollection -enable_lua_debugger -lan -monitor_parent_process -nooverlay -nosound -novsync -offline -players -port -shard -tick -token -ugc_directory …`

There is no fps-cap flag (nothing like `+fps_max`); the 60 fps limiter is hard-coded in the engine.

### 8. Mods enabled at the time of the "after" test

Server (Master/Caves): Epic Healthbar, [API] Gem Core, Display Attack Range, 六格装备栏 (6-slot equipment), Display food values, Wormhole Marks, Global Positions, Realistic Placement, Show Me (Origin), Global Pause.

Client: all of the above plus Finder *fixed*, Extended Map Icons, Russification Pack for DST, Nightmare phase indicator, ActionQueue Reborn, Snapping tills, Minimap HUD, Gesture Wheel, Health Info, Combined Status, Waypoint, Ice Fling Range Check.

None of them were involved in the problem — the 57 fps was reproduced with every mod off, and 60 fps holds with all of them on.

### 9. Leftovers on disk

- `~/dst_sample.sh`, `~/dst_sample.log` — CPU sampler; **deleted 2026-09-22 12:50** (results preserved in §7.1).
- `~/Documents/Klei/DoNotStarveTogether/319660262/client.ini.bak` — backup from the threaded-renderer test; `client.ini` was restored; backup safe to delete.
- Steam launch option `-novsync` — harmless, did nothing, can be removed.

---

# Addendum — per-process fix without touching the system

### 10. Second solution: inject a precise `nanosleep` into DST only

The `sysctl` fix is system-wide and resets on reboot. A narrower alternative: make the sleep precise **inside the game process only**, leaving macOS timer coalescing on for everything else.

**How it works.** macOS lets a process load an extra library at start-up via the `DYLD_INSERT_LIBRARIES` environment variable — the same mechanism Steam uses to put its overlay (`gameoverlayrenderer.dylib`) into every game. DST's binary is signed with hardened runtime but Klei granted it `com.apple.security.cs.allow-dyld-environment-variables` and `com.apple.security.cs.disable-library-validation`, so injection is permitted.

`precise_sleep.dylib` (~60 lines of C, universal arm64+x86_64) interposes `nanosleep()`:
1. wait with the kernel until `deadline − margin` (`mach_wait_until`),
2. busy-wait the remaining `margin` on `mach_absolute_time()`, which is exact,
3. `margin` adapts to the lateness the kernel actually delivers (starts 1.5 ms, floor 0.5 ms, cap 8 ms), so CPU spent spinning is ~1 ms per frame (~6 % of one core).

`run_dst` is a tiny compiled launcher: Steam runs it instead of the game (launch option `run_dst %command%`), it prepends the library to whatever `DYLD_INSERT_LIBRARIES` Steam already set (keeping the overlay), resolves the `.app` bundle to the executable inside, and `exec`s the game. It has to be a compiled program, not a shell script: macOS strips `DYLD_*` variables from the environment of platform binaries such as `/bin/sh`, which would have dropped Steam's overlay.

**Files:** `~/dst_precise_sleep/precise_sleep.c`, `precise_sleep.dylib`, `run_dst.c`, `run_dst`, `run_dst.log` (one entry per launch), `bench`/`bench.c` (test program, deletable).

**Steam launch options:**
```
~/dst_precise_sleep/run_dst %command%
```

**Verification (2026-09-22 11:07, `kern.timer.coalescing_enabled: 1`):**
- Stand-alone benchmark, 120 × `nanosleep(16.667 ms)` as an x86_64 process under Rosetta: plain → avg 19.8 ms (50.4 fps-equiv.); with the library → avg 16.68 ms (59.95 fps-equiv.), self-test rows read `asked 16.667 ms, got 16.667 ms`.
- In game: 60 fps in the menu, 60 fps in the modded world with 2× Klaus. Steam overlay counter still present.
- `lsof -p <pid>` on the game process lists `precise_sleep.dylib`, `gameoverlayrenderer.dylib`, `steamloader.dylib` — all three loaded.
- `run_dst.log`: `DYLD_INSERT_LIBRARIES=…/precise_sleep.dylib:…/steamloader.dylib:…/gameoverlayrenderer.dylib`, `resolved: …/dontstarve_steam.app/Contents/MacOS/dontstarve_steam`.

**Dead ends on the way:** Steam on macOS does not run launch options through a shell, so `VAR=value %command%` fails with "OS Error 260"; and Steam passes the `.app` directory, not the executable, so the launcher must resolve it.

**Comparison:**

| | `sysctl` | `run_dst` + `precise_sleep.dylib` |
|---|---|---|
| Scope | whole system | DST only |
| Battery | slightly worse at idle | unchanged |
| Needs | `sudo` after every reboot | nothing, always on via Steam |
| Breaks if | macOS removes the sysctl | Klei drops the entitlements or Steam changes `%command%` |
| Failure mode | command errors, no change | game starts without the library → 57 again |

Troubleshooting if 57 fps ever returns: `tail ~/dst_precise_sleep/run_dst.log` (was the launcher used?) and `lsof -p $(pgrep -f MacOS/dontstarve_steam) | grep precise` (is the library loaded?).

**Not applicable:** the Steam guide "Разблокировка FPS при помощи LuaJIT" (workshop 3513536655) — Windows-only (`.bat` + VC++ 2022 redistributable) and aimed at lifting the cap above 60, which was never the issue here.

---

# Part 3 — library internals, edge cases, open issue

### 11. `precise_sleep.dylib` — how it works, line by line

**Mechanism: dyld interposing.** A Mach-O library can carry a `__DATA,__interpose` section listing `{replacement, original}` pairs. When the library is loaded via `DYLD_INSERT_LIBRARIES`, dyld rewrites every *other* image's calls to `nanosleep` so they land in `precise_nanosleep` instead. Calls made from inside the interposing library itself are not redirected, which is why `precise_nanosleep` can safely call the real `nanosleep`/`mach_wait_until`.

**Algorithm of `precise_nanosleep(req, rem)`:**
1. Convert the request to nanoseconds, compute `deadline = mach_absolute_time() + want` (absolute clock ticks, exact).
2. If `want > margin`: call `mach_wait_until(deadline − margin)` — a kernel wait with an absolute deadline. This is the part the kernel may deliver late.
3. Measure `late = now − (deadline − margin)`, i.e. how late the kernel actually woke us. Adapt: `target = late + 0.4 ms`; if `target > margin` jump up immediately, otherwise decay toward it by 1/16 per call. Clamp to [0.5 ms, 8 ms]. Initial value 1.5 ms.
4. Busy-wait until `mach_absolute_time() >= deadline` using `pause` (x86) / `yield` (arm64) so the core is not hammered.
5. Return 0 with `rem` zeroed (we never return early, so no remaining time).

**Why adaptive.** Lateness depends on the process's scheduling tier: ~1 ms for a foreground app, 3–5 ms for a background one (kernel: `kern.timer_coalesce_tier0_ns_max = 1 ms`, `tier1 = 5 ms`, `tier2 = 20 ms`). A fixed 1.5 ms margin was proven insufficient for a background process in the benchmark (got 18–19 ms); adaptive settled at ~3.7 ms there and delivers 16.667 ms exactly.

**Cost.** CPU spent spinning = `margin` per call. Observed: the client process rose from ~45–57 % (before the library) to ~72–80 % CPU in the same scenes. That is the spin. It is CPU that would otherwise idle; on a 14-core M4 Pro it has no measurable effect on fps or thermals, but it is not free on battery.

**Edge cases / known behaviour:**
- `DYLD_INSERT_LIBRARIES` is inherited by child processes. DST's client spawns the two dedicated-server shards, so **the library is also loaded into Master and Caves**. Their tick loops also sleep, so they also spin (observed: shards ~30 % CPU vs ~17 % / ~11 % before). Harmless, arguably makes server ticks more regular, but wasteful. Possible improvement not implemented: in the constructor check `getprogname()` and set a pass-through flag when it contains `nullrenderer`, so `precise_nanosleep` just calls the original there.
- Applies to *every* `nanosleep` in the process, not only the frame limiter: audio/network/loader threads that sleep get precise wake-ups too (each costing ≤ `margin` of spin). No issue observed.
- `usleep()`/`sleep()` inside libc call `nanosleep` internally — those internal calls are *not* interposed (same image). DST's frame limiter calls `nanosleep` directly (seen in the stack sample), so it is covered.
- Only one adaptive `margin` shared by all threads (no locking; a torn update would just cause one imprecise sleep). Fine for this use.
- Universal binary (arm64 + x86_64) so it loads harmlessly into any process in the launch chain; in the x86_64 game it is the x86_64 slice, which Rosetta AOT-compiles (`/private/var/db/oah/…/precise_sleep.dylib.aot` visible in `lsof`).
- Ad-hoc signed (`codesign -s -`). Accepted because DST has `disable-library-validation`. If Klei removes that entitlement or `allow-dyld-environment-variables`, dyld silently skips the library and the game starts normally at 57 fps.
- Self-test: `DST_PRECISE_SLEEP_TEST=1` in the environment makes the constructor print 8 timed sleeps to stderr at load.
- Rebuild: `clang -arch x86_64 -arch arm64 -O2 -dynamiclib -o precise_sleep.dylib precise_sleep.c && codesign -s - -f precise_sleep.dylib`.

### 12. `run_dst` — the launcher

Compiled C, universal. Steam runs it with the game path as `argv[1]` (launch option `run_dst %command%`).
1. Reads the incoming `DYLD_INSERT_LIBRARIES` (Steam sets `steamloader.dylib:gameoverlayrenderer.dylib`), prepends `precise_sleep.dylib`, re-exports.
2. Appends a line to `run_dst.log` with the final variable and the arguments.
3. If `argv[1]` ends in `.app`, rewrites it to `<app>/Contents/MacOS/<name>` (Steam passes the bundle directory, `execv` needs the executable).
4. `execv` the game — same PID, so Steam still tracks it as the game process.

Why compiled and not `#!/bin/sh`: macOS strips all `DYLD_*` variables from the environment of platform binaries (`/bin/sh`, `zsh`, `/usr/bin/env`). Verified: `env DYLD_INSERT_LIBRARIES=x /bin/sh -c 'echo $DYLD_INSERT_LIBRARIES'` prints empty. A shell wrapper would therefore lose Steam's overlay.

Edge cases: paths are hard-coded to `~/dst_precise_sleep/`; moving the folder means rebuilding with new `LIB`/`LOG` defines. `snprintf` buffers are 4096 bytes — fine for these paths. If `execv` fails the launcher prints `run_dst: execv: <reason>` to stderr and exits 1; Steam then shows nothing (observed as "game closes immediately") — check `run_dst.log` first.

### 13. Can this be a Steam Workshop mod for DST?

**No.** DST mods are Lua scripts running inside the engine's sandbox: they can add prefabs, UI, recipes, hook Lua functions. They cannot load native code, set environment variables, change process scheduling, or touch `nanosleep` — the frame limiter is C++ inside the engine, unreachable from Lua. That is also why the Windows "LuaJIT / unlock FPS" project needs a `.bat` that replaces DLLs in the game folder: it is not a mod, it is a binary patch distributed through the Workshop page as instructions.

What is possible instead:
- Distribute `precise_sleep.c` + `run_dst.c` as a small open-source project (e.g. GitHub) with a build/install script and the one-line Steam launch option. Anyone on Apple Silicon with the same symptom can use it.
- File a bug report with Klei (Klei bug tracker, "Don't Starve Together" → macOS) containing §3–§4 of this document: the sample showing 45 % of the main thread in `nanosleep`, the 16.67→17.57 ms arithmetic, and the fact that both `sysctl` and the interposer restore 60. The proper fix is ~20 lines in their macOS platform layer (sleep until deadline − 1 ms, then spin, or use a realtime thread policy).

### 14. Open issue: rubber-banding ("ping") in fullscreen while hosting — RESOLVED in Part 4 (§16–§18)

> **Update 2026-09-22 12:40:** the hypothesis below was confirmed by measurement and fixed by `precise_sleep.dylib` v2. See §16–§18. The text below is kept as the state of knowledge at the time.

**Symptom:** in fullscreen the host's own character rubber-bands/teleports as if there were network lag; windowed is fine. Present before the library was introduced (reported at the 57-fps stage), unchanged after.

**Tested, did not help:** fullscreen refresh rate 165 / 120 / 60 Hz (the game applies it — `SetFullscreen(2560,1440,165|120|60)` in `client_log.txt` — but does not persist it; it reverts to 60 on the next fullscreen switch).

**Hypothesis (unverified): macOS Game Mode deprioritises the server shards.** Game Mode engages automatically for a fullscreen game and lowers the scheduling priority of all *other* processes to favour the game. The Master and Caves shards are separate background processes, so their simulation ticks would arrive late and the client's movement prediction gets corrected → rubber-banding. Supporting data: windowed, the client main thread runs at priority 97 and both shards at 63; the fullscreen priorities were not captured (three sampler attempts: one ended before fullscreen, one lost the shard columns to a `pgrep` collision with its predecessor, one ran entirely windowed).

**How to test (5 minutes):**
1. In fullscreen, menu-bar game-controller icon → **Game Mode → Off**. If rubber-banding disappears, confirmed; leave it Off for DST.
2. Alternatively run the shards outside the client: a dedicated server started separately is not a child of the game and can be given normal priority; or simply play in a borderless window at screen size (Game Mode does not engage).
3. To measure: `ps -o pri,%cpu -p <client> <master> <caves>` once per second while switching to fullscreen; a drop of the shards' `pri` (63 → lower) with a corresponding CPU dip confirms the hypothesis. Master log (`master_server_log.txt`) shows only `Server Autopaused/Unpaused` — no tick warnings, so any lateness is scheduling, not simulation load.

### 15. Final state on disk

| Path | Purpose | Keep? |
|---|---|---|
| `~/dst_precise_sleep/precise_sleep.c`, `precise_sleep.dylib` | the fix | yes |
| `~/dst_precise_sleep/run_dst.c`, `run_dst` | Steam launcher | yes |
| `~/dst_precise_sleep/run_dst.log` | one entry per launch; first thing to check if 57 fps returns | yes (can be truncated) |
| `~/dst_sample.sh`, `~/dst_sample.log` | per-second CPU sampler used in this investigation | deleted 2026-09-22 12:50 |
| `~/Desktop/DST_Mac_57fps_fix.md` | this document | yes |
| Steam launch options | `~/dst_precise_sleep/run_dst %command%` | yes |
| `kern.timer.coalescing_enabled` | back to default `1`; not needed while the library works | — |

Deleted: `bench`, `bench.c`, `client.ini.bak`.

---

# Part 4 — fullscreen rubber-banding: cause found and fixed

### 16. Measurement: Game Mode puts the server shards into the Darwin-background band

**Date:** 2026-09-22 12:23. Library v1 loaded, hosting with caves. One `ps` line per second while switching windowed → fullscreen → windowed (was `~/dst_pri.log`, since deleted):

```
time     | client (dontstarve_steam)  | master (nullrenderer)   | caves (nullrenderer)
         |  pri nice %cpu             |  pri nice %cpu          |  pri nice %cpu
12:23:38 |  97   0   80.3             |  63   0   36.3          |  63   0   28.5     ← windowed
12:23:39 |  97   0   82.8             |   4   0   10.7          |   4   0    7.6     ← entered fullscreen
   …     |  97                        |   4                     |   4
12:23:56 |  97   0   83.6             |   4   0    5.7          |   4   0    3.2
12:23:57 |  97   0   58.5             |  63   0    4.4          |  63   0    2.1     ← back to windowed
12:23:58 |  97   0   86.7             |  63   0   29.3          |  63   0   30.0
```

- `pri 63 → 4` for both shards, for exactly the duration of fullscreen. Priority 4 is `MAXPRI_THROTTLE`, the Darwin-background band (the one App Nap uses). macOS Game Mode — indicated by the rocket icon in the menu bar — moves every process except the game into it. The client stays at 97.
- `nice` is unchanged: the demotion is an external task policy (RunningBoard), not `nice`. A process cannot remove an externally applied darwinbg from inside (`setpriority(PRIO_DARWIN_PROCESS, 0, 0)` only clears a self-imposed one).
- Shard CPU fell from ~30 % to 3–10 %: they were woken less often, i.e. ticks were being skipped, and the client then corrected the host's position → the "teleport".

Why this is *timer* trouble and not *CPU* trouble: the kernel's timer-coalescing parameters (`sysctl -a | grep timer_coalesce`) give each scheduling band its own permitted lateness:

```
tier0 (active app):  scale 3  → interval/8,  cap   1 ms     ← client; 16.67/8 = 2.08 → capped 1 ms = the 0.9 ms seen in §3
tier1:               scale 2  → interval/4,  cap   5 ms
tier2:               scale 1  → interval/2,  cap  20 ms
tier3:               scale -2 → interval×4,  cap  75 ms
bg (darwinbg):       scale -5 → interval×32, cap 100 ms     ← shards in fullscreen
```

The shard tick loop sleeps ~33 ms (30 Hz). In the `bg` band the kernel may deliver that wake-up **up to 100 ms late**. Library v1 could compensate at most 8 ms (`MARGIN_MAX_NS`), and spinning for 100 ms is longer than the tick itself. `sysctl coalescing_enabled=0` fixed fullscreen because it disables coalescing for *every* band including `bg` — which is also why it was the only fix that worked there.

### 17. Which wait primitive survives darwinbg — benchmark

60 waits of 33.3 ms each, measured lateness, once as a normal process and once under `taskpolicy -b` (Darwin background, `pri 4` — the same band as the shards in fullscreen):

```
primitive                              normal process          darwinbg (pri 4)
nanosleep                              avg  3.0  max   5.0 ms  avg 69.7  max 100.0 ms
mach_wait_until                        avg  3.5  max   5.0 ms  avg 78.9  max 101.1 ms
kevent EVFILT_TIMER (default)          avg  4.2  max   5.0 ms  avg 100.0 max 100.2 ms
kevent EVFILT_TIMER + NOTE_LEEWAY 0    avg  4.2  max   5.0 ms  avg 100.0 max 101.6 ms
latency-QoS tier0 + mach_wait_until    avg  0.9  max   1.2 ms  avg 79.1  max 100.0 ms
realtime thread + mach_wait_until      avg  0.01 max  0.02 ms  avg 75.6  max 100.0 ms
kevent EVFILT_TIMER + NOTE_CRITICAL    avg  0.04 max  0.08 ms  avg 0.03  max   0.08 ms  ← the only survivor
```

- Thread-level fixes (latency QoS tier 0, realtime time-constraint policy) work for a normal process but are **overridden** by an external darwinbg — the kernel demotes realtime threads in throttled tasks.
- `NOTE_CRITICAL` on a kqueue timer selects a zero-leeway timer tier in XNU that is independent of the process's scheduling policy. No entitlement, no `sudo`.

Then the real interposer, as an x86_64 process under Rosetta (like the game), 60 × `nanosleep(33.3 ms)` and 60 × `nanosleep(16.67 ms)`:

```
                             normal (pri 31)             darwinbg (pri 4)
no library                   avg 3.3   max 5.2 ms        avg 70   max 100 ms
v1 (mach_wait_until)         avg 0.08  max 3.5 ms        avg 66   max  92 ms
v2 (kevent NOTE_CRITICAL)    avg 0.00  max 0.06 ms       avg 0.00 max 0.03 ms
```

### 18. `precise_sleep.dylib` v2

Change relative to v1 (§11): the coarse wait `mach_wait_until(deadline − margin)` is replaced by a `kevent64()` wait on an `EVFILT_TIMER` armed with `NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_CRITICAL` and the absolute mach-time deadline. One kqueue per thread, created lazily (`pthread_key` with a destructor that closes it when the thread exits). `EINTR` re-arms and keeps waiting; any other `kevent64` failure falls back to the v1 `mach_wait_until` path. Because the kernel now wakes the thread on time in every band, the adaptive spin margin was lowered: start 0.3 ms, floor 0.1 ms (v1: 1.5 / 0.5 ms), cap 8 ms unchanged as a safety net. Everything else — interposing table, adaptation logic, `DST_PRECISE_SLEEP_TEST=1` self-test — is unchanged. kqueue descriptors are not inherited across `exec`, so the shards create their own.

Consequences:
- Fullscreen with Game Mode **on**: shards stay at `pri 4` (not touched), but their timers fire on time → no rubber-banding. Game Mode remains beneficial for the client.
- Spin is now ~0.1–0.2 ms per sleep instead of ~1–4 ms: client CPU drops from ~75–85 % back toward ~50 %, shards from ~30 % to ~10–15 %.
- Verified in game 2026-09-22 ~12:40: fullscreen, hosting with caves, Game Mode active — 60 fps, no teleporting.

Rollback: `cp -p ~/dst_precise_sleep/precise_sleep.dylib.v1 ~/dst_precise_sleep/precise_sleep.dylib`.
Rebuild: same command as §11 (`clang -arch x86_64 -arch arm64 -O2 -dynamiclib … && codesign -s - -f …`); build to a temporary name and `mv` it into place so a running game keeps its mapped copy.

Files added: `~/dst_precise_sleep/precise_sleep.c.v1`, `precise_sleep.dylib.v1` (previous version). The priority capture (`~/dst_pri.log`), the benchmarks and the CPU sampler were deleted after their data was written into this document.

Note for the Klei bug report (§13): the macOS platform layer should arm its frame/tick timers with `NOTE_CRITICAL` (or `DISPATCH_TIMER_STRICT`, which maps to it) — that alone fixes both the 57 fps cap and the fullscreen shard starvation without any user-side workaround.
