# Star Citizen VR on Linux — Steam Frame Eye Tracking (WiVRn V12 / Gaze Fix V10)

> **⚠️ COMMUNITY REGRESSION REPORT (2026-10-14):** The original V12/Gaze V10 has been reported to feel **significantly worse than V11/Gaze V9** on an actual Steam Frame. The possible causes include unconditional binocular/mono source switching and reduced micro-fixation confirmation (3 vs V11's 5), but neither has been confirmed. **V12 is not recommended as the stable baseline.** Restore V11 for daily use, and test V12 only with backups. An experimental V13 follow-up uses V11-style binocular defaults with opt-in guarded mono fallback.

**Community testing project: experimental Steam Frame eye tracking and gaze-driven foveated streaming for Star Citizen on Linux.**

## What is this test for?

Star Citizen is the primary test game for this WiVRn patch. The goal is to improve **Steam Frame eye-gaze reliability** while playing Star Citizen through a Linux PC, especially when blinking or temporarily losing tracking in one eye. Instead of immediately freezing the gaze-directed sharp region, original V12 can use the remaining healthy eye (monocular fallback).

We want testers to compare **gaze updates, eye-tracking freezes, sharp-region movement, reacquisition, and any gaze jumps** in the cockpit. We are also collecting separate observations about the **Star Citizen MFD/HUD wobble** reported on Linux VR: interface text may shake while the physical cockpit display stays steady. That is a different symptom and **this patch is not a confirmed fix for it**.

> **Experimental, unofficial, and not an upstream WiVRn release.** This is the **original V12 Monocular Fallback** implementation (250 ms stale timeout, 3-sample micro fixation, 1.15° / 2.40° / 5.00° bands), **NOT** the separately circulated "V12 Monocular Recovery" (400 ms / 2-sample) experiment. **This is not a confirmed fix for Star Citizen's independently wobbling MFD/HUD.**

To reproduce the Star Citizen eye-tracking test, build **both sides** from the same commit:

- **Steam Frame:** ARM64 WiVRn native headset client with Gaze Fix V10 (V12) in `client/scenes/stream_tracking.cpp`.
- **Linux PC:** x86_64 WiVRn server compiled from **the same Git commit**. The server inherits the V3 144/72/48 pacing modifications from the base branch; the V12 gaze update changes only headset-client tracking source, **not** the Linux server's gaze implementation.

**Source provenance:** branch derived from `DF001/WiVRn` branch `steam-frame-v10-gaze-v8-confirmed-upstream`. The V12 source is taken from the author's already-compiled local original V12 checkout, which contains V11 adaptive fixation plus original V12 monocular fallback. The `community-v12/` scripts and docs are additional testing tools. It is *not* a reproduction of the later V12 package. No precompiled binary is included here, and these scripts haven't been independently run on another person's Frame.

## 1. Download the same source for both machines

On the Linux PC:

```bash
git clone --branch steam-frame-v12-gaze-v10-community-test --single-branch https://github.com/DF001/WiVRn.git
cd WiVRn
git rev-parse HEAD
grep -F 'Steam Frame gaze v10 active:' client/scenes/stream_tracking.cpp
grep -F 'WIVRN_FRAME_V3' server/driver/wivrn_session.cpp | head
```

Save the `git rev-parse HEAD` result when reporting test results. If you build or update one component later, rebuild the other from this exact commit. **Do not mix this client with an unrelated stock WiVRn server or Flatpak**. Client/server protocol compatibility requires matching versions.

## 2. Build both components locally on a Linux PC

**Clone the community-test branch as shown in section 1 first.** Use the same exact commit for both builds (`git rev-parse HEAD`). Do not mix an updated headset client with an older installed Linux WiVRn server. The build scripts stage output; they do **not** install anything on your PC or headset.

### 2A. Linux PC — x86_64 patched server

Install the standard WiVRn source dependencies for your Linux distribution: CMake 3.25+, Ninja, C/C++ build tools, Vulkan/OpenXR/Monado development libraries and WiVRn's encoding dependencies. On Arch/CachyOS, review the [upstream Linux build instructions](https://github.com/WiVRn/WiVRn/tree/master/docs) for current requirements.

From the root of your cloned community branch:

```bash
bash community-v12/build-pc-server.sh

# Inspect the staged files (nothing has been installed system-wide):
find output-v12-pc/usr -maxdepth 3 -type f | head -n 30
file output-v12-pc/usr/bin/wivrn-server
```

The script configures `--preset server`, builds locally, and stages the server under `output-v12-pc/usr`. If the server binary has a different installation path on your build, locate it with the `find` command above. **This staged tree is not by itself a working installed OpenXR runtime**: package/install the matching components and runtime manifest using your distribution's WiVRn packaging method or a self-contained test environment. Check that you are running the patched server, not a stock `/usr/bin/wivrn-server`.

To turn on the original V3 Linux server pacing for a server instance you have installed correctly:

```bash
WIVRN_FRAME_V3=1 wivrn-server
```

If the server is started by a dashboard or systemd user service, set its environment on **that service**, rather than in an unrelated terminal, and restart the service. Stop conflicting WiVRn instances when testing. To disable the experimental V3 server pacing, use `WIVRN_FRAME_V3=0` instead. This switch does not control V12 headset gaze processing.

### 2B. Steam Frame — ARM64 native WiVRn client (built on the Linux PC)

Install and start Docker; enable AArch64 execution (`binfmt_misc`/QEMU) if your Linux PC is x86_64. The included script uses Steam Runtime 4's ARM64 SDK container, can download build dependencies, and requires network access and disk space. Review it before running.

From the **same cloned community branch**:

```bash
bash community-v12/build-frame-arm64.sh

# Confirm this is for the headset, NOT the Linux PC:
file output-frame-v10-arm64/usr/bin/wivrn
strings output-frame-v10-arm64/usr/bin/wivrn | grep -F 'Steam Frame gaze v10 active:'
sha256sum output-frame-v10-arm64/usr/bin/wivrn
```

Expected architecture: **`ARM aarch64`**. The client output is `output-frame-v10-arm64/usr/bin/wivrn` relative to the repository root; despite its `v10` directory name, it is the **V12 / Gaze Fix V10** build. The script does not install it. This is a native SteamOS/Linux binary, **not an Android APK**. Do not copy an x86_64 executable to the headset.

## 3. Copy the ARM64 client from Linux to SteamOS

The following is an **example only**: replace `FRAME_IP` with your Steam Frame IP address and `/home/steamos/Applications/WiVRn-v3-gaze/bin` with the **real directory where your Frame launches the native WiVRn `wivrn` binary**. The headset must be reachable by SSH as `steamos`, and the destination directory must already exist. Nothing in these commands uses shell variables for the paths.

First **exit WiVRn on the headset**, then from the Linux PC (in the cloned repository root) run:

```bash
# Make sure the existing installation path is correct before copying:
ssh steamos@FRAME_IP 'test -f /home/steamos/Applications/WiVRn-v3-gaze/bin/wivrn && echo "Existing WiVRn executable found"'

# Transfer as a staged filename; do not overwrite the running/working binary:
scp output-frame-v10-arm64/usr/bin/wivrn \
  steamos@FRAME_IP:/home/steamos/Applications/WiVRn-v3-gaze/bin/wivrn.v12-staged

# Check file integrity: local and remote SHA-256 hashes must match:
sha256sum output-frame-v10-arm64/usr/bin/wivrn
ssh steamos@FRAME_IP 'sha256sum /home/steamos/Applications/WiVRn-v3-gaze/bin/wivrn.v12-staged'
```

**If the original executable check fails, STOP** and find the actual install path. Do not blindly create that directory or replace an unrelated executable. If copying fails, check SSH access/permissions before proceeding.

## 4. Activate V12 on Steam Frame, verify, and roll back

Connect to your headset:

```bash
ssh steamos@FRAME_IP
```

Then run **on SteamOS**, with WiVRn fully closed:

```bash
cd /home/steamos/Applications/WiVRn-v3-gaze/bin || exit 1

test -f wivrn && test -f wivrn.v12-staged || exit 1

# Preserve the previous working executable before replacing it:
cp -p wivrn "wivrn.before-v12.$(date +%Y%m%d-%H%M%S)"

# Activate the copied V12 executable:
chmod +x wivrn.v12-staged
mv -f wivrn.v12-staged wivrn

# Optional marker check; this verifies the build identity, not that the runtime option is ON:
strings wivrn | grep -F 'Steam Frame gaze v10 active:'
```

Restart WiVRn from Steam. Check that your Steam shortcut still points to this `wivrn` and not a different installation.

**To restore the previous binary:** close WiVRn, SSH to the Frame, `cd` into the installation directory, then run:

```bash
ls -lt wivrn.before-v12.*
# Replace TIMESTAMP below with the exact saved backup filename suffix:
cp -p wivrn.before-v12.TIMESTAMP wivrn
```

Restart WiVRn. A binary-only backup will not repair missing or incompatible runtime libraries or assets; restore your earlier matching client installation if necessary.

## 5. Enable or disable the experimental gaze fix — no rebuild necessary

These are **Steam launch options for the WiVRn client app on the Steam Frame**, not Linux terminal commands or launch options for Star Citizen. On the Frame, open **Steam → Library → WiVRn → Properties → Launch Options**. Apply the desired option, then **fully close and restart WiVRn** (the gaze switch is read at startup).

**Enable V12 eye tracking and keep V3 enabled (normal test mode):**

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 %command%
```

**Disable the V12 experimental eye-tracking processing, but keep V3 enabled (A/B comparison):**

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=0 %command%
```

**Disable both experimental options on the headset (troubleshooting):**

```text
WIVRN_FRAME_V3=0 WIVRN_FRAME_GAZE_FIX=0 %command%
```

To disable the V3 server's experimental pacing too, start the **matching installed Linux PC server** with:

```bash
WIVRN_FRAME_V3=0 wivrn-server
```

For a systemd/dashboard-managed server, put `WIVRN_FRAME_V3=0` in the **service's** environment and restart that service. A variable placed in your current terminal will not affect an already-running server process.

**Important:** The original V12 has **no independent runtime switch for *only* monocular fallback**. `WIVRN_FRAME_GAZE_FIX=0` disables the broader experimental gaze-processing path and uses the client's alternate gaze handling; it does not uninstall the patch or turn off the headset's hardware eye tracker. A compiled marker may remain in the binary even when the feature is disabled. Reinstall the backed-up executable to return to the previous build completely.

## 6. Test Steam Frame eye tracking in Star Citizen (compare baseline and V12)

Keep the **same headset refresh rate (144 Hz where supported), codec, bitrate, render resolution, game scene, and server commit** for meaningful comparisons.

1. **Baseline outside Star Citizen:** in the Steam Frame/WiVRn dashboard, look between fixed points and verify that the foveated/high-quality region follows your gaze. Note any freezing.
2. **Star Citizen cockpit, eyes only:** while keeping your head still, look between two MFDs or cockpit indicators. Record whether the sharp region follows your eyes smoothly or sticks in one place.
3. **Blink / one-eye loss:** blink naturally, then briefly cover either eye if comfortable. Look for transitions from `COMBINED POST` to `LEFT ONLY` / `RIGHT ONLY` rather than persistent `HOLD`; watch for sudden position jumps.
4. **Slow head movement:** fix your eyes on MFD text and rotate your head slowly. Compare gaze focus stability and cockpit stability independently.
5. **Separate MFD/HUD wobble observation:** check whether UI text shakes independently of the stable physical MFD frame. Do not score this as an eye-tracking fix: similar wobble has been observed on SteamVR for Linux.
6. **A/B comparison:** return to your known-working client and repeat the same Star Citizen scene with the same matching Linux server, headset refresh rate and streaming settings.

Extract logs on the Frame (path depends on how Steam starts WiVRn):

```bash
grep -E 'Steam Frame gaze v10 (active:|source:|world:|requests=|:)' ~/.local/share/Steam/logs/steam_output.log | tail -n 100
```

Useful telemetry: `COMBINED POST`, `LEFT ONLY`, `RIGHT ONLY`, `HOLD`, `source_hz`, `stale_events`, `confirmed`, `reacquisitions`. Record whether the foveation zone freezes, whether a monocular transition introduces a sudden gaze jump, and whether the *game's* MFD text jitters independently. If anything becomes uncomfortable, stop testing and restore the backup.

### Test report template

```text
Star Citizen Linux VR / Steam Frame Eye Tracking — WiVRn V12 original (Gaze V10):
Client+server commit:
PC distro/kernel/GPU:
Headset software version and panel Hz:
Codec / resolution / bitrate / WiVRn options:
Gaze results (COMBINED, LEFT, RIGHT, HOLD; stale):
Star Citizen: did the gaze-directed sharp region freeze or lag behind eye movement?:
Star Citizen: did blinking / monocular fallback restore gaze updates?:
Star Citizen: any jumps when switching between binocular and monocular tracking?:
Did Star Citizen MFD text wobble relative to the MFD frame?:
Client/server logs (personal identifiers removed):
```

## V12 regression note: V11 comparison and follow-up V13

This is a **user-reported V12 regression**, not a proven upstream defect. V12 switches directly to a valid single-eye direction when binocular tracking degrades. Since monocular and binocular rays can differ, the foveated region may jump. V12 also accepts small gaze movement after three fresh samples rather than V11's five.

For a focused A/B test, restore V11's confirmation count *without changing V12 mono fallback* by setting this **Steam Frame WiVRn shortcut** launch option, then restarting:

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES=5 %command%
```

**Disable the entire V12 experimental gaze fix:**

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=0 %command%
```

This is **not** the same as restoring the V11 executable. To return to the actual V11 implementation, stop WiVRn and restore the saved V11 ARM64 client and compatible assets. Retain the matching host server build.

**V13 / Gaze Fix V11:** Follow-up experimental branch `steam-frame-v13-gaze-v11-community-test` defaults to five-sample confirmation and binocular-only tracking; guarded monocular fallback is independently opt-in. Neither V12 nor V13 is a confirmed fix for the separate Star Citizen MFD texture wobble.

## 7. Scope / limitations

- Unofficial and experimental; no reproducible release binaries or GitHub CI artifacts are provided.
- Gaze v10 targets eye-gaze source selection and stability specifically for Steam Frame use cases such as Star Citizen. **No fix for Star Citizen MFD/HUD wobble is claimed or verified.** It does not change the headset eye-server producer (`/dev/shm/eye-server.mmap`, ABI v5).
- WiVRn's server must be built from the same revision. The Linux server retains earlier V3 144/72/48 cadence behaviour (controlled independently with `WIVRN_FRAME_V3`); V12 is a headset-client gaze-processing patch (controlled by `WIVRN_FRAME_GAZE_FIX`).
- No private device IP addresses, credentials, vendor tokens or user logs should be committed to the repository.
- Not affiliated with the upstream WiVRn project, Valve or CIG.
