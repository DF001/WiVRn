# WiVRn Steam Frame V12 / Gaze Fix V10 — community test

> **Experimental, unofficial, and not an upstream WiVRn release.** This is the **original V12 Monocular Fallback** implementation (250 ms stale timeout, 3-sample micro fixation, 1.15° / 2.40° / 5.00° bands), **NOT** the separately circulated "V12 Monocular Recovery" (400 ms / 2-sample) experiment. **This is not a confirmed fix for Star Citizen's independently wobbling MFD/HUD.**

This branch packages **both sides** of a paired Linux PC/Steam Frame test:

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

## 2. Linux PC: build the patched server (x86_64)

Install the standard WiVRn source build dependencies for your distribution, including CMake (>=3.25), Ninja, C++ toolchain, Vulkan, OpenXR/Monado and the required codec/encoder dependencies. Arch/CachyOS may need AUR/build-time dependencies beyond `base-devel`. See [upstream build instructions](https://github.com/WiVRn/WiVRn/blob/master/docs/building.md).

```bash
bash community-v12/build-pc-server.sh
```

The script configures with **`--preset server`**, builds the matching Linux PC server, and **stages** an install tree under `output-v12-pc/usr`. It does **not** run `sudo`, replace your system's existing WiVRn installation, install an OpenXR manifest or switch your session. Use a test package/environment to activate this build and verify its libraries and OpenXR runtime manifest point to the staged version. Running a staged executable directly may fail if its library/manifest paths are not set up.

For systems where you have already installed the exact matching server through your usual packaging method, ensure its launch environment contains:

```text
WIVRN_FRAME_V3=1
```

E.g. when starting a matching `wivrn-server` directly in a properly configured runtime environment:

```bash
WIVRN_FRAME_V3=1 wivrn-server
```

If using the dashboard/systemd, configure the environment for the actual service instead: setting a variable in one shell won't affect an already-running service. Stop other WiVRn server instances before testing. **Do not accidentally launch a stock server from `$PATH`**; confirm the running binary path and build revision.

## 3. Steam Frame: build native ARM64 client

On a Linux PC with Docker, QEMU/binfmt AArch64 emulation, disk space and network access for the Steam Runtime SDK:

```bash
bash community-v12/build-frame-arm64.sh
file output-frame-v10-arm64/usr/bin/wivrn
strings output-frame-v10-arm64/usr/bin/wivrn | grep -F 'Steam Frame gaze v10 active:'
```

The script uses a Steam Runtime 4 ARM64 SDK container and stages the ARM64 installation tree under `output-frame-v10-arm64/usr`. It may download build-time dependencies; **review it before running**. It does not install anything on the headset. In the output of `file`, expect `ELF 64-bit ... ARM aarch64`; **never install the native x86_64 test binary onto the Frame**.

This Steam Frame native Linux client is **not an Android APK**.

## 4. Steam Frame: stage safely, back up, install

WiVRn installation paths vary across Steam Frame setups. The example below uses `~/Applications/WiVRn-v3-gaze/bin` as a **placeholder installation layout**. Adjust to the *real* native executable path on your device, determined before proceeding. Replace `FRAME_IP` with the address of your own Frame.

Close WiVRn on the Frame. From the PC, copy to a new name (do **not** overwrite yet):

```bash
scp output-frame-v10-arm64/usr/bin/wivrn steamos@FRAME_IP:~/Applications/WiVRn-v3-gaze/bin/wivrn.v12-staged
sha256sum output-frame-v10-arm64/usr/bin/wivrn
ssh steamos@FRAME_IP 'sha256sum ~/Applications/WiVRn-v3-gaze/bin/wivrn.v12-staged'
```

Hashes should match. SSH into the headset, verify that the old executable exists, then take a backup and activate V12:

```bash
ssh steamos@FRAME_IP
cd ~/Applications/WiVRn-v3-gaze/bin || exit 1
cp -p wivrn "wivrn.before-v12.$(date +%Y%m%d-%H%M%S)"
chmod +x wivrn.v12-staged
mv -n wivrn.v12-staged wivrn.new-v12
# After WiVRn is stopped and the backup exists:
mv -f wivrn.new-v12 wivrn
strings wivrn | grep -F 'Steam Frame gaze v10 active:'
```

**Rollback:** close WiVRn, copy the saved `wivrn.before-v12.TIMESTAMP` binary back to `wivrn`, and restart. If the app fails to start due to incompatible shared libraries, also restore matching client assets from your previous working installation; this test does not cover replacement of platform runtime dependencies.

Launch options for the Frame Steam shortcut:

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 %command%
```

To change the V12 micro-confirmation count (default 3, allowed 2–8), e.g.:

```text
WIVRN_FRAME_V3=1 WIVRN_FRAME_GAZE_FIX=1 WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES=4 %command%
```

## 5. Compare baseline and experimental results

Keep the **same headset refresh rate (144 Hz where supported), codec, bitrate, render resolution, game scene, and server commit** for meaningful comparisons.

1. **Dashboard with head still:** look at fixed points, check if the foveated/high-quality zone moves with the gaze rather than freezing.
2. **Cover one eye / blink:** verify `COMBINED POST → LEFT ONLY / RIGHT ONLY` where the remaining eye is good, rather than repeated `HOLD`.
3. **Rotate head slowly:** distinguish world/cockpit stability from eye-gaze cursor instability.
4. **Star Citizen MFD:** check if MFD **text/UI wobbles independently of the stable physical MFD frame**. That may be a separate engine/Wine/VR UI-rendering issue, even if V12 gaze is improved.
5. **Revert to baseline:** compare against your previous known-working client, preferably with the same PC server. Avoid simultaneously changing other VR settings.

Extract logs on the Frame (path depends on how Steam starts WiVRn):

```bash
grep -E 'Steam Frame gaze v10 (active:|source:|world:|requests=|:)' ~/.local/share/Steam/logs/steam_output.log | tail -n 100
```

Useful telemetry: `COMBINED POST`, `LEFT ONLY`, `RIGHT ONLY`, `HOLD`, `source_hz`, `stale_events`, `confirmed`, `reacquisitions`. Record whether the foveation zone freezes, whether a monocular transition introduces a sudden gaze jump, and whether the *game's* MFD text jitters independently. If anything becomes uncomfortable, stop testing and restore the backup.

### Test report template

```text
WiVRn V12 original / Gaze V10:
Client+server commit:
PC distro/kernel/GPU:
Headset software version and panel Hz:
Codec / resolution / bitrate / WiVRn options:
Gaze results (COMBINED, LEFT, RIGHT, HOLD; stale):
Did the gaze-directed sharp region freeze?:
Did Star Citizen MFD text wobble relative to the MFD frame?:
Client/server logs (personal identifiers removed):
```

## Scope / limitations

- Unofficial and experimental; no reproducible release binaries or GitHub CI artifacts are provided.
- Gaze v10 only targets source-selection and gaze stability. **It does not fix confirmed engine-side MFD/UI wobble**, and it does not change the headset eye-server producer (`/dev/shm/eye-server.mmap`, ABI v5).
- WiVRn's server must be built from the same revision. The Linux server retains earlier V3 144/72/48 cadence behaviour; V12 is a headset-client source patch.
- No private device IP addresses, credentials, vendor tokens or user logs should be committed to the repository.
- Not affiliated with the upstream WiVRn project, Valve or CIG.
