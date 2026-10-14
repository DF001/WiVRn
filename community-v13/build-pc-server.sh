#!/usr/bin/env bash
# Build the matching WiVRn Linux PC server from the SAME source revision as the Steam Frame client.
# Compiles and stages into output-v13-pc; does NOT install into the live system.
set -euo pipefail
cd "$(dirname "$(dirname "$(realpath "$0")")")"
grep -Fq 'WIVRN_FRAME_V3' server/driver/wivrn_session.cpp || {
    echo 'ERROR: This checkout does not contain the expected V3 server modifications.' >&2
    exit 1
}
grep -Fq 'Steam Frame gaze v11 active:' client/scenes/stream_tracking.cpp || {
    echo 'ERROR: This checkout does not contain the V13 Steam Frame patch.' >&2
    exit 1
}
cmake -S . -B build-v13-server --preset server -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-v13-server --parallel "$(nproc)"
DESTDIR="$PWD/output-v13-pc" cmake --install build-v13-server
if test -f output-v13-pc/usr/bin/wivrn-server; then
    file output-v13-pc/usr/bin/wivrn-server
else
    echo 'NOTE: Check the install log for the wivrn-server install path.'
fi
printf '\nPC server staged under: %s/output-v13-pc/usr\n' "$PWD"
printf 'NOT installed system-wide. Test in an isolated environment or package appropriately.\n'
