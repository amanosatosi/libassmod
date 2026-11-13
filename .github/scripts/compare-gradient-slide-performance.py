"""Compare warmed, single-CPU release rendering and allocation measurements."""
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys


def median(samples, field):
    return statistics.median(sample[field] for sample in samples)


def compare(binaries, fonts, workload, height, events, frames, trials, variants):
    samples = {name: [] for name in variants}
    for trial in range(trials):
        order = variants if trial % 2 == 0 else variants[::-1]
        for name in order:
            version, mode = name.split('/')
            args = [binaries[version], *fonts, str(frames), workload,
                    str(height), str(events), mode]
            samples[name].append(json.loads(subprocess.check_output(args, text=True)))

    # Compare identical paint modes across revisions. The deliberately changed
    # mixed-coordinate edge case is covered by a separate pixel regression.
    for mode in ('legacy', 'slide'):
        checksums = {trial['checksum'] for key, values in samples.items()
                     if key.endswith('/' + mode) for trial in values}
        if len(checksums) > 1:
            raise RuntimeError(f'{workload}/{height}/{events}/{mode}: pixels or geometry differ')
    return samples


def main():
    pinned, before, after, *fonts = sys.argv[1:]
    binaries = {'pinned': pinned, 'before': before, 'after': after}
    cpu = min(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {cpu})
    control = ('pinned/legacy', 'before/legacy', 'after/legacy')
    active = ('before/legacy', 'before/slide', 'after/legacy', 'after/slide')
    cases = [(name, 1080, 12, 1000, 15, control)
             for name in ('ordinary', 'ordinary-overrides')]
    cases += [(name, height, 1, 60, 7, active)
              for height in (720, 1080, 2160)
              for name in ('fill', 'stops', 'rgb-alpha', 'outlines', 'positioned', 'overlap')]
    cases += [('rgb-alpha', 1080, count, 60, 7, active) for count in (4, 8, 12)]
    rows = [
        '| Workload | Output height | Events | Variant | Median CPU ms/frame | MAD % | '
        'Peak RSS MiB | Alloc/frame | Alloc KiB/frame |',
        '| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |',
    ]
    raw = []
    for name, height, count, frames, trials, variants in cases:
        samples = compare(binaries, fonts, name, height, count, frames, trials, variants)
        raw.append(dict(workload=name, height=height, events=count, samples=samples))
        for variant, runs in samples.items():
            seconds = median(runs, 'cpu_seconds')
            mad = statistics.median(abs(run['cpu_seconds'] - seconds) for run in runs)
            rows.append(f'| {name} | {height} | {count} | {variant} | {seconds * 1000 / frames:.4f} '
                        f'| {100 * mad / seconds:.2f} | {median(runs, "peak_rss_kib") / 1024:.2f} '
                        f'| {median(runs, "allocations_per_frame"):.2f} '
                        f'| {median(runs, "requested_bytes_per_frame") / 1024:.2f} |')
    report = dict(cpu_affinity=cpu, pinned_sha='014ed134c36b45f5ce2d4173964f02a221f0ad3b',
                  before_sha='9a68654e56c189c67d94987b1c79c209216ade41',
                  after_sha=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                  cases=raw)
    Path('gradient-slide-performance.json').write_text(json.dumps(report, indent=2) + '\n')
    summary = '\n'.join(rows) + '\n\nPeak RSS includes process startup and shared libraries. '
    summary += 'Allocation counts and requested bytes come from a separate warmed pass over direct '
    summary += 'malloc, calloc, and realloc calls in the harness and static libass.\n'
    Path('gradient-slide-performance.md').write_text(summary)
    print(summary)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as output:
            output.write(summary)


if __name__ == '__main__':
    main()
