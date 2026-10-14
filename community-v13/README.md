# Star Citizen VR on Linux — Steam Frame Eye Tracking V13 (Gaze Fix V11)

> **UNOFFICIAL EXPERIMENTAL SOURCE PATCH — NOT A TESTED RELEASE.** V13 was prepared in response to a report that V12 feels significantly worse than V11. The source transformation and an isolated C++20 tracking-policy test passed. The **full native/ARM64 WiVRn build, runtime integration, and headset behaviour have not been validated here**. Do not treat it as a guaranteed fix for Star Citizen MFD/HUD wobble.

**Purpose:** Preserve the better-feeling V11 gaze baseline for the **Valve Steam Frame** while allowing an explicitly optional, more conservative test of one-eye fallback when playing **Star Citizen through WiVRn on a Linux PC**.

## How V13 differs from V12

| Feature | V12 (Gaze V10) | V13 (Gaze V11) |
|---|---|---|
| Gaze source by default | Binocular, automatically switches to one healthy eye | **Binocular only (V11-style)** |
| Monocular fallback | Always enabled when gaze fix is on | **Off by default; explicit opt-in** |
| Monocular source-transition validation | No dedicated angular guard | **Three fresh consecutive source samples; reject >2° jump on entry to a mono source** |
| Small gaze movement confirmation | 3 fresh samples | **5 fresh samples by default** |
| Micro / medium / large bands | 1.15° / 2.40° / 5.00° | Same |
| Stale gaze cutoff | 250 ms | Same |
| Linux PC V3 adaptive cadence | Inherited | Inherited; no V13 server algorithm change |

**Important:** V13 safe-default policy is intended to approximate V11 behaviour, but it is built from the V12 codebase. It has not yet been shown to perform identically to V11 in the headset. The optional mono guard may reject real eye movements as well as erroneous ones. Star Citizen's independent MFD-text wobble is a separate investigation.

## Required source pairing

Build the **Steam Frame AArch64 client** and the **Linux x86_64 server** from **the same V13 Git commit**. The client contains gaze handling; the host server retains earlier V3 adaptive 144/72/48 pacing. Do not mix a V13 client with an unrelated WiVRn version/Flatpak. Save `git rev-parse HEAD` with reports.

```bash
git clone --branch steam-frame-v13-gaze-v11-community-test --single-branch https://github.com/DF001/WiVRn.git
cd WiVRn
git rev-parse HEAD
python3 community-v13/test-v13-gaze-policy.py
```

The separate C++20 test requires `g++` and checks only the V13 selection policy. It is **not** a full WiVRn build or hardware test.

## 1 — Build the Linux PC server locally

Install the normal WiVRn build dependencies for your distribution (CMake 3.25+, Ninja, C++20, OpenXR/Monado, Vulkan and codec/encoder development dependencies). See upstream WiVRn build documentation for your Linux distro.

```bash
bash community-v13/build-pc-server.sh
```

The script stages output under `output-v13-pc/usr` and **does not install or activate** the host server. Package/deploy the staged server and its Monado/OpenXR runtime libraries/manifests using your distro's normal method. A staged binary alone may not be runnable without the correct library paths and runtime registration. Make sure the active server is the matching V13 build rather than a stock instance from `$PATH`.

Enable the inherited V3 server cadence in the actual host server environment (or systemd service environment):

```bash
WIVRN_FRAME_V3=1 wivrn-server
```

This command is an example for an already correctly installed V13 `wivrn-server`, not a substitute for installing the staged libraries/runtime. To disable the *host pacing experiment* only: `WIVRN_FRAME_V3=0 wivrn-server`. These are separate from the Frame gaze settings.

## 2 — Build the Steam Frame ARM64 client locally

From that **same checkout** on the Linux PC, with Docker, QEMU/binfmt support for `linux/arm64`, and access to the pinned Steam Runtime 4 ARM64 SDK:

```bash
bash community-v13/build-frame-arm64.sh
file output-frame-v13-arm64/usr/bin/wivrn
strings output-frame-v13-arm64/usr/bin/wivrn | grep -F 'Steam Frame gaze v11 active:'
```

The first command builds and stages an **ARM64 native SteamOS client** under `output-frame-v13-arm64/usr`. It does not install anything onto the headset. `file` must say `ELF ... ARM aarch64`. **Do not install the x86_64 test binary on the Frame.** The build script is experimental and may install dependencies inside the build container; review it before running.

## 3 — Copy to SteamOS, back up, install, verify

**Close WiVRn on the Frame first.** `FRAME_IP` is a placeholder for *your* headset IP. The example installation folder below is *not universal*: verify the existing `wivrn` path on the headset, and adjust it before copying. Commands deliberately use direct paths, with no shell variables.

From the Linux PC:

```bash
ssh steamos@FRAME_IP 'test -f ~/Applications/WiVRn-v3-gaze/bin/wivrn && ls -l ~/Applications/WiVRn-v3-gaze/bin/wivrn'
scp output-frame-v13-arm64/usr/bin/wivrn steamos@FRAME_IP:~/Applications/WiVRn-v3-gaze/bin/wivrn.v13-staged
sha256sum output-frame-v13-arm64/usr/bin/wivrn
ssh steamos@FRAME_IP 'sha256sum ~/Applications/WiVRn-v3-gaze/bin/wivrn.v13-staged'
```

The two hashes must be identical. SSH to the headset:

```bash
ssh steamos@FRAME_IP
cd ~/Applications/WiVRn-v3-gaze/bin || exit 1
# Ensure WiVRn is stopped and this is the correct install location.
test -f wivrn && test -f wivrn.v13-staged || exit 1
cp -p wivrn "wivrn.before-v13.$(date +%Y%m%d-%H%M%S)" || exit 1
chmod +x wivrn.v13-staged
mv -f wivrn.v13-staged wivrn
strings wivrn | grep -F 'Steam Frame gaze v11 active:'
```

Start WiVRn again from Steam. Native client assets/libraries may also need to match the built source. If it fails to start, restore the previous known-working binary **and its compatible assets**.

### Roll back to V11

Stop WiVRn. If the version in place *before V13* was V11, select its timestamped backup:

```bash
cd ~/Applications/WiVRn-v3-gaze/bin || exit 1
ls -lt wivrn.before-v13.*
# Replace TIMESTAMP with the exact displayed filename suffix.
cp -p wivrn.before-v13.TIMESTAMP wivrn
```

If you had V12 installed before V13, this backup restores **V12, not V11**; use your previously saved V11 executable or a fresh V11 ARM64 build to actually return to V11.

## 4 — On/off switches (Frame's WiVRn Steam launch options)

The runtime flags are read when the WiVRn client launches. Restart after changing them.

**Recommended V13 test (V11-like stable default; no mono):**

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 WIVRN_FRAME_V13_MONOCULAR=0 %command%
```

**Experimental guarded one-eye fallback:**

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 WIVRN_FRAME_V13_MONOCULAR=1 %command%
```

**Turn OFF the entire experimental gaze fix (same V13 binary):**

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=0 %command%
```

**Optional tuning** — change the default of 5 small-movement confirmations to 4 (may reduce latency but increase noise):

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 WIVRN_FRAME_V13_MONOCULAR=0 WIVRN_FRAME_GAZE_V11_MICRO_SAMPLES=4 %command%
```

`WIVRN_FRAME_V13_MONOCULAR=0` disables **only** monocular fallback, retaining V13 binocular smoothing. `WIVRN_FRAME_GAZE_FIX=0` disables the whole experimental gaze path and may select a different OpenXR tracking route. Those are not equivalent tests.

## 5 — Star Citizen / Steam Frame eye-tracking comparison

1. Use the same Star Citizen version, cockpit, head movement, codec, bitrate, resolution and headset rate on each run.
2. Run the V13 **binocular default** for 30 seconds of fixed gaze and intentional small gaze movements. Compare against your known-working V11 on the same host build.
3. If comfortable, enable `WIVRN_FRAME_V13_MONOCULAR=1`. Look between MFDs, blink, and briefly cover one eye. Watch for sharp-region jumps, holds and reacquisition. Stop if uncomfortable.
4. If the MFD **text wobbles relative to its physical frame** while the world remains stable, log it **separately** from foveated-region gaze jitter. This V13 patch does not claim to fix it.
5. Collect logs, identifying startup marker, `COMBINED POST`, `LEFT ONLY`, `RIGHT ONLY`, `HOLD`, `source_hz`, `stale_events`, `confirmed`, and any `mono transition rejected` events.

```bash
# On the Steam Frame, if WiVRn is launched via Steam and logs here:
grep -E 'Steam Frame gaze v11 (active:|source:|world:|:)' ~/.local/share/Steam/logs/steam_output.log | tail -n 100
```

### Report template

```text
Patch: V13 / Gaze V11 — Steam Frame / Star Citizen on Linux
Client+server git commit:
Linux distro/kernel/GPU / WiVRn build environment:
Steam Frame OS revision / refresh rate:
Streaming codec / bitrate / resolution:
Runtime mode: mono=0 or mono=1 / micro sample count:
Fixed gaze jitter compared to V11:
Eye-motion response lag compared to V11:
Blink/one-eye fallback jumps or freezes:
MFD text wobble relative to physical MFD frame (separate):
Relevant V13 logs (remove personal info/IPs):
```

## Scope

This is an independent community experiment, **not an upstream WiVRn release**, and not affiliated with Valve or Cloud Imperium Games. The patch does not modify `/dev/shm/eye-server.mmap` producer ABI v5 or the game's Wine/OpenXR UI render-to-texture path. All binaries must be built/tested by participants; keep backups. **A passing isolated test does not imply VR hardware validation.**
