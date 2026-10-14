#!/usr/bin/env python3
"""Experimental V13/Gaze V11: V11-safe defaults, opt-in guarded monocular fallback.

Patches only an ORIGINAL V12/Gaze V10 `client/scenes/stream_tracking.cpp`.
By default this checks anchors and prints changes; --apply writes the file.
Build, runtime, and headset tests are separate. No git or network mutation.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import sys

SOURCE = Path('client/scenes/stream_tracking.cpp')


def once(src: str, old: str, new: str, label: str) -> str:
    hits = src.count(old)
    if hits != 1:
        raise ValueError(f'{label}: expected exactly one anchor, found {hits}')
    return src.replace(old, new, 1)


POLICY_V12 = '''\t\t\t// The eye-server reports head-relative POST rays for both eyes. A
\t\t\t// failing eye must not freeze tracking while the other eye remains
\t\t\t// healthy. Prefer fused binocular gaze; otherwise choose one healthy
\t\t\t// POST ray. Preserve conservative per-eye covariance hysteresis.
\t\t\tstd::optional<glm::vec3> candidate = combined;
\t\t\tsource selected = source::combined;
\t\t\tif (not candidate and left_good and left)
\t\t\t{
\t\t\t\tcandidate = *left;
\t\t\t\tselected = source::left;
\t\t\t}
\t\t\telse if (not candidate and right_good and right)
\t\t\t{
\t\t\t\tcandidate = *right;
\t\t\t\tselected = source::right;
\t\t\t}

\t\t\tif (candidate)
\t\t\t{
\t\t\t\tif (not last_good_direction)
\t\t\t\t{
\t\t\t\t\t// No previous gaze to hold: accept first usable eye.
\t\t\t\t\tsource_ready = true;
\t\t\t\t\trecovery_samples = recovery_samples_required;
\t\t\t\t}
\t\t\t\telse if (not source_ready)
\t\t\t\t{
\t\t\t\t\t++recovery_samples;
\t\t\t\t\tif (recovery_samples >= recovery_samples_required)
\t\t\t\t\t{
\t\t\t\t\t\tsource_ready = true;
\t\t\t\t\t}
\t\t\t\t}

\t\t\t\tif (source_ready)
\t\t\t\t{
\t\t\t\t\tlast_good_direction = *candidate;
\t\t\t\t\tlast_good_sample_time_raw = timestamp;
\t\t\t\t\tlast_good_sequence = sequence;
\t\t\t\t\tcurrent_source = selected;
\t\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t\t}
\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tcurrent_source = source::held;
\t\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t\t}
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tsource_ready = false;
\t\t\t\trecovery_samples = 0;
\t\t\t\tcurrent_source = source::held;
\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t}
'''

POLICY_V13 = '''\t\t\t// Safe by default: accept fresh binocular gaze only (V11 policy).
\t\t\t// Experimental monocular selection requires explicit opt-in, and
\t\t\t// a source transition must pass a short angular/coherence check.
\t\t\tstd::optional<glm::vec3> candidate = combined;
\t\t\tsource selected = source::combined;
\t\t\tif (v13_monocular_enabled() and not candidate and left_good and left)
\t\t\t{
\t\t\t\tcandidate = *left;
\t\t\t\tselected = source::left;
\t\t\t}
\t\t\telse if (v13_monocular_enabled() and not candidate and right_good and right)
\t\t\t{
\t\t\t\tcandidate = *right;
\t\t\t\tselected = source::right;
\t\t\t}

\t\t\tbool transition_pending = false;
\t\t\tif (candidate and last_good_direction and selected != last_accepted_source and
\t\t\t    selected != source::combined)
\t\t\t{
\t\t\t\t// Only transitions INTO a single-eye source need this guard.
\t\t\t\t// A rejected jump must not move the foveated center.
\t\t\t\tconst float d = std::clamp(glm::dot(*candidate, *last_good_direction), -1.0f, 1.0f);
\t\t\t\tconst float jump_degrees = std::acos(d) * (180.0f / 3.14159265358979323846f);
\t\t\t\tif (jump_degrees > v13_mono_transition_max_degrees)
\t\t\t\t{
\t\t\t\t\ttransition_pending = true;
\t\t\t\t\tpending_transition.reset();
\t\t\t\t\tpending_transition_samples = 0;
\t\t\t\t\t++rejected_mono_jumps;
\t\t\t\t\tif (rejected_mono_jumps <= 3 or rejected_mono_jumps % 64 == 0)
\t\t\t\t\t{
\t\t\t\t\t\tspdlog::warn("Steam Frame gaze v11: mono transition rejected: {:.2f} deg, rejects={}",
\t\t\t\t\t\t             jump_degrees, rejected_mono_jumps);
\t\t\t\t\t}
\t\t\t\t}
\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tif (not pending_transition or *pending_transition != selected)
\t\t\t\t\t{
\t\t\t\t\t\tpending_transition = selected;
\t\t\t\t\t\tpending_transition_samples = 1;
\t\t\t\t\t}
\t\t\t\t\telse
\t\t\t\t\t{
\t\t\t\t\t\t++pending_transition_samples;
\t\t\t\t\t}
\t\t\t\t\ttransition_pending = pending_transition_samples < v13_mono_transition_samples_required;
\t\t\t\t}
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tpending_transition.reset();
\t\t\t\tpending_transition_samples = 0;
\t\t\t}

\t\t\tif (transition_pending)
\t\t\t{
\t\t\t\t// Keep the last valid gaze during the guard window. Do not
\t\t\t\t// mark this as a fresh accepted source timestamp.
\t\t\t\tcurrent_source = source::held;
\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t}
\t\t\telse if (candidate)
\t\t\t{
\t\t\t\tif (not last_good_direction)
\t\t\t\t{
\t\t\t\t\tsource_ready = true;
\t\t\t\t\trecovery_samples = recovery_samples_required;
\t\t\t\t}
\t\t\t\telse if (not source_ready)
\t\t\t\t{
\t\t\t\t\t++recovery_samples;
\t\t\t\t\tif (recovery_samples >= recovery_samples_required)
\t\t\t\t\t{
\t\t\t\t\t\tsource_ready = true;
\t\t\t\t\t}
\t\t\t\t}

\t\t\t\tif (source_ready)
\t\t\t\t{
\t\t\t\t\tif (selected != last_accepted_source)
\t\t\t\t\t{
\t\t\t\t\t\tspdlog::info("Steam Frame gaze v11: accepted source transition {} -> {}",
\t\t\t\t\t\t             int(last_accepted_source), int(selected));
\t\t\t\t\t}
\t\t\t\t\tlast_good_direction = *candidate;
\t\t\t\t\tlast_good_sample_time_raw = timestamp;
\t\t\t\t\tlast_good_sequence = sequence;
\t\t\t\t\tlast_accepted_source = selected;
\t\t\t\t\tcurrent_source = selected;
\t\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t\t}
\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tcurrent_source = source::held;
\t\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t\t}
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tsource_ready = false;
\t\t\t\trecovery_samples = 0;
\t\t\t\tcurrent_source = source::held;
\t\t\t\tlog_source(current_source, left_unc, right_unc);
\t\t\t}
'''


def patch(src: str) -> str:
    if 'Steam Frame gaze v11 active:' in src or 'WIVRN_FRAME_V13_MONOCULAR' in src:
        raise ValueError('V13 already present; refusing to patch twice')
    if src.count('Steam Frame gaze v10 active:') != 1:
        raise ValueError('Original V12 source marker missing or duplicated')
    src = once(src, POLICY_V12, POLICY_V13, 'original V12 single-eye policy')
    src = once(src,
        'static constexpr uint32_t recovery_samples_required = 3;',
        '''static constexpr uint32_t recovery_samples_required = 3;
\tstatic constexpr uint32_t v13_mono_transition_samples_required = 3;
\tstatic constexpr float v13_mono_transition_max_degrees = 2.0f;

\tstatic bool v13_monocular_enabled()
\t{
\t\tstatic const bool enabled = [] {
\t\t\tconst char * value = std::getenv("WIVRN_FRAME_V13_MONOCULAR");
\t\t\treturn value != nullptr and std::strcmp(value, "1") == 0;
\t\t}();
\t\treturn enabled;
\t}''', 'mono opt-in guard')
    src = once(src,
        '\tsource current_source = source::held;\n',
        '''\tsource current_source = source::held;
\tsource last_accepted_source = source::combined;
\tstd::optional<source> pending_transition;
\tuint32_t pending_transition_samples = 0;
\tuint64_t rejected_mono_jumps = 0;
''', 'transition state')
    src = once(src, 'WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES',
               'WIVRN_FRAME_GAZE_V11_MICRO_SAMPLES', 'micro sample env')
    src = once(src, 'return uint32_t(3);\n\t\t}();\n\t\treturn samples;',
               'return uint32_t(5);\n\t\t}();\n\t\treturn samples;', 'micro default')
    src = src.replace('Steam Frame gaze v10', 'Steam Frame gaze v11')
    src = once(src,
        'timestamp-corrected binocular+monocular world-space mmap with quality hysteresis, adaptive 3-sample micro fixation (1.15/2.40/5.00 deg), 144Hz smoothing and 250ms stale timeout',
        'V11-safe binocular default, optional guarded monocular fallback, adaptive 5-sample micro fixation (1.15/2.40/5.00 deg), 144Hz smoothing and 250ms stale timeout',
        'V13 runtime identity')
    src = once(src,
        'mmap active: ABI v5 COMBINED POST, hold on single-eye loss, 3-sample recovery',
        'mmap active: ABI v5, binocular default, optional guarded monocular fallback',
        'mmap description')
    for token in ['Steam Frame gaze v11 active:', 'WIVRN_FRAME_V13_MONOCULAR',
                  'WIVRN_FRAME_GAZE_V11_MICRO_SAMPLES', 'rejected_mono_jumps',
                  'glm::conjugate(*current_head) * stabilized_world',
                  'frame_world_stale_timeout_ns = 250', '/dev/shm/eye-server.mmap']:
        if token not in src:
            raise ValueError(f'required V13 invariant missing: {token}')
    return src


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--apply', action='store_true', help='write patched source (default: dry run)')
    args = parser.parse_args()
    before = args.source.read_text(encoding='utf-8')
    after = patch(before)
    print(f'PASS V13 source anchors: {len(before)} -> {len(after)} bytes')
    print('Defaults: binocular-only, 5 micro samples, monocular fallback OFF')
    if args.apply:
        args.source.write_text(after, encoding='utf-8')
        print(f'WROTE: {args.source}')
    else:
        print('DRY RUN: no modifications')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
