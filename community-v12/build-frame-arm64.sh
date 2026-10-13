#!/usr/bin/env bash
# ARM64 Steam Frame build for the v10 development branch. Does not install to headset.
set -euo pipefail

cd "${1:-$(dirname "$(dirname "$(realpath "$0")")")}"

CURRENT_BRANCH="$(git branch --show-current)"
if [[ "$CURRENT_BRANCH" != "steam-frame-v12-gaze-v10-monocular-fallback" && "$CURRENT_BRANCH" != "steam-frame-v12-gaze-v10-community-test" ]]; then
    echo "ERROR: checkout an original V12 or community V12 branch first" >&2
    exit 1
fi
if ! grep -Fq 'Steam Frame gaze v10 active:' client/scenes/stream_tracking.cpp; then
    echo 'ERROR: gaze v10 source marker missing' >&2
    exit 1
fi

LOG="$PWD/wivrn-v12-gaze-v10-arm64-build.log"
echo "Building WiVRn gaze-v10 ARM64; log: $LOG"

docker run --rm \
  --name wivrn-frame-v10-builder \
  --platform linux/arm64 \
  --user root \
  -e HOST_UID="$(id -u)" \
  -e HOST_GID="$(id -g)" \
  -v "$PWD:/src" \
  -w /src \
  --entrypoint /bin/bash \
  registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk/arm64:4.0.20260805.254769 \
  -lc '
set -euo pipefail
git config --global --add safe.directory /src

echo "=== ARCHITECTURE ==="
uname -m

echo "=== SDK DEPENDENCIES ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends librsvg2-bin npm wget ca-certificates
wget -q https://github.com/KhronosGroup/KTX-Software/releases/download/v4.4.2/KTX-Software-4.4.2-Linux-arm64.deb -O /tmp/KTX-arm64.deb
dpkg -i /tmp/KTX-arm64.deb
npm install --global @gltf-transform/cli@4.1.1

echo "=== CONFIGURE ==="
cmake -S /src \
  -B /src/build-frame-v10-arm64 \
  --preset frame \
  -DCMAKE_BUILD_TYPE=Release \
  -DWIVRN_CSS=images/wivrn-nightly.css \
  -DFETCHCONTENT_BASE_DIR=/src/cmake-fetch-arm64 \
  -DCMAKE_INSTALL_PREFIX=/usr

echo "=== BUILD + STAGE INSTALL ==="
DESTDIR=/src/output-frame-v10-arm64 \
  cmake --build /src/build-frame-v10-arm64 --target install --parallel 4

echo "=== VERIFY ARM64 GAZE V10 ==="
file /src/output-frame-v10-arm64/usr/bin/wivrn
strings /src/output-frame-v10-arm64/usr/bin/wivrn | grep -F "Steam Frame gaze v10 active"

echo "=== RESTORE OWNERSHIP ==="
chown -R "$HOST_UID:$HOST_GID" \
  /src/build-frame-v10-arm64 \
  /src/output-frame-v10-arm64

echo "=== BUILD COMPLETE ==="
' 2>&1 | tee "$LOG"

echo "ARM64 package staged at: $PWD/output-frame-v10-arm64/usr"
echo "Build log: $LOG"
