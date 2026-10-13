#!/usr/bin/env python3
"""Experimental Steam Frame gaze v10: tolerate a single high-confidence eye.

Apply only to the local WiVRn v9 branch/source. Does not commit, push, compile,
or install on the headset. This does not address Star Citizen frame/reprojection jitter.
"""
from __future__ import annotations
import argparse
import pathlib
import subprocess
import sys

FROM_BRANCH = 'steam-frame-v11-gaze-v9-adaptive-fixation'
TO_BRANCH = 'steam-frame-v12-gaze-v10-monocular-fallback'
SOURCE = pathlib.Path('client/scenes/stream_tracking.cpp')


def git(*args):
    p = subprocess.run(['git', *args], text=True, capture_output=True)
    if p.returncode:
        raise RuntimeError(f'git {" ".join(args)} failed: {p.stderr.strip()}')
    return p.stdout.strip()


def replace_once(src, old, new, name):
    count = src.count(old)
    if count != 1:
        raise RuntimeError(f'{name}: expected exactly one match, found {count}')
    return src.replace(old, new, 1)


SINGLE_EYE_POLICY = '''\t\t\t// The eye-server reports head-relative POST rays for both eyes. A
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


def patch(src):
    if 'Steam Frame gaze v10' in src:
        raise RuntimeError('Already contains gaze v10')
    if 'Steam Frame gaze v9 active:' not in src:
        raise RuntimeError('Gaze v9 marker missing. Do not apply to v8 or upstream.')
    if 'no fresh binocular gaze for >250ms' not in src:
        raise RuntimeError('Expected v9 timeout code missing')
    src = replace_once(src,
        '\tenum class source\n\t{\n\t\tcombined,\n\t\theld,\n\t};',
        '\tenum class source\n\t{\n\t\tcombined,\n\t\tleft,\n\t\tright,\n\t\theld,\n\t};',
        'source enum')
    src = replace_once(src,
        '''\t\t\tcase source::held:
\t\t\t\tspdlog::warn(''',
        '''\t\t\tcase source::left:
\t\t\t\tspdlog::info(
\t\t\t\t        "Steam Frame gaze v9 source: LEFT ONLY (left_unc={:.6f}, right_unc={:.6f})",
\t\t\t\t        left_unc,
\t\t\t\t        right_unc);
\t\t\t\tbreak;
\t\t\tcase source::right:
\t\t\t\tspdlog::info(
\t\t\t\t        "Steam Frame gaze v9 source: RIGHT ONLY (left_unc={:.6f}, right_unc={:.6f})",
\t\t\t\t        left_unc,
\t\t\t\t        right_unc);
\t\t\t\tbreak;
\t\t\tcase source::held:
\t\t\t\tspdlog::warn(''',
        'source transition logging')
    # Preserve existing covariance classification, calculation of combined,
    # mutex, producer ABI offsets, and all world-space conversion code.
    start = '\t\t\tif (combined)\n\t\t\t{\n\t\t\t\tif (not last_good_direction)'
    end = '\n\t\t}\n\n\t\tif (last_good_direction)'
    if src.count(start) != 1:
        raise RuntimeError(f'reader policy start is ambiguous ({src.count(start)} matches)')
    a = src.index(start)
    b = src.index(end, a)
    old = src[a:b]
    for flag in ['last_good_direction = *combined;', 'source_ready = false;',
                 'last_good_sample_time_raw = timestamp;', 'current_source = source::held;']:
        if flag not in old:
            raise RuntimeError(f'expected v9 reader guard missing: {flag}')
    src = src[:a] + SINGLE_EYE_POLICY + src[b:]
    # The single-eye fallback should drastically reduce starvation, so use a
    # 3-sample micro-fixation default. This remains tunable at runtime.
    src = replace_once(src, 'WIVRN_FRAME_GAZE_V9_MICRO_SAMPLES',
                       'WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES', 'micro env')
    src = replace_once(src, 'return uint32_t(5);', 'return uint32_t(3);', 'micro default')
    src = src.replace('Steam Frame gaze v9', 'Steam Frame gaze v10')
    src = replace_once(src,
        'timestamp-corrected binocular world-space mmap + adaptive micro/medium/large sample fixation (1.15/2.40/5.00 deg), 144Hz temporal smoothing and 250ms stale-source timeout',
        'timestamp-corrected binocular+monocular world-space mmap with quality hysteresis, adaptive 3-sample micro fixation (1.15/2.40/5.00 deg), 144Hz smoothing and 250ms stale timeout',
        'startup marker')
    src = src.replace('no fresh binocular gaze for >250ms',
                      'no fresh usable eye gaze for >250ms')
    src = src.replace('HOLD last stable combined gaze', 'HOLD last stable gaze')
    checks = {
        'v10 marker': 'Steam Frame gaze v10 active:' in src,
        'only v10 logs': 'Steam Frame gaze v9' not in src,
        'single-eye logs': 'LEFT ONLY' in src and 'RIGHT ONLY' in src,
        'stale guard preserved': 'frame_world_stale_timeout_ns' in src,
        'head compensation preserved': 'glm::conjugate(*current_head) * stabilized_world' in src,
        'mmap ABI preserved': 'eye-server.mmap' in src,
        'runtime tuning': 'WIVRN_FRAME_GAZE_V10_MICRO_SAMPLES' in src,
        'no old combined-only acceptance': 'last_good_direction = *combined;' not in src,
    }
    for k, ok in checks.items():
        if not ok:
            raise RuntimeError(f'validation failed: {k}')
    return src


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apply', action='store_true', help='create new branch and write client patch')
    a = p.parse_args()
    if pathlib.Path.cwd().resolve() != pathlib.Path(git('rev-parse','--show-toplevel')).resolve():
        raise RuntimeError('Run from repository root')
    if git('branch','--show-current') != FROM_BRANCH:
        raise RuntimeError(f'First switch to {FROM_BRANCH}')
    # Existing v9 may be uncommitted. Allow only the v9 tracking source as a
    # tracked change; untracked build dirs are not modified.
    tracked = [x for x in git('diff','--name-only','HEAD','--').splitlines() if x]
    if any(x != SOURCE.as_posix() for x in tracked):
        raise RuntimeError(f'Unexpected modified tracked files: {tracked}')
    if not SOURCE.is_file():
        raise RuntimeError(f'{SOURCE} not found')
    before = SOURCE.read_text(encoding='utf-8')
    after = patch(before)
    print(f'OK: V9 source anchors verified; V10 patch prepared')
    print(f'Branch: {FROM_BRANCH} -> {TO_BRANCH}')
    print(f'File: {SOURCE}; bytes: {len(before)} -> {len(after)}')
    if not a.apply:
        print('DRY RUN: no files changed')
        return
    if subprocess.run(['git','show-ref','--verify','--quiet',f'refs/heads/{TO_BRANCH}']).returncode == 0:
        raise RuntimeError('Target branch already exists; refusing to overwrite')
    git('switch','-c',TO_BRANCH)
    SOURCE.write_text(after,encoding='utf-8')
    print('V10 source applied. No build, install, commit, or push was performed.')

if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError) as e:
        print(f'ERROR: {e}',file=sys.stderr)
        sys.exit(1)
