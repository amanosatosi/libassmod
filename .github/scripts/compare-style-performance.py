"""Run already-built public-API harnesses; never build in this script."""
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys


def main():
    baseline, candidate, myanmar, latin = sys.argv[1:]
    affinity = None
    if hasattr(os, 'sched_getaffinity'):
        affinity = min(os.sched_getaffinity(0))
        os.sched_setaffinity(0, {affinity})
    frames, trials = 1000, 7
    results = {}
    for workload in ('ordinary', 'ordinary-overrides', 'furi-inline', 'furi-metadata',
                     'vertical-inline', 'border-karaoke'):
        samples = {'baseline': [], 'candidate': []}
        for trial in range(trials):
            order = ('baseline', 'candidate') if trial % 2 == 0 else ('candidate', 'baseline')
            for revision in order:
                binary = baseline if revision == 'baseline' else candidate
                run = subprocess.check_output([binary, myanmar, latin, str(frames), workload], text=True)
                samples[revision].append(json.loads(run))
        checksums = {s['checksum'] for runs in samples.values() for s in runs}
        if len(checksums) != 1:
            raise RuntimeError(f'{workload}: baseline/candidate render geometry differs')
        medians = {key: statistics.median(s['cpu_seconds'] for s in runs)
                   for key, runs in samples.items()}
        mad_percent = {key: 100 * statistics.median(abs(s['cpu_seconds'] - medians[key])
                                                   for s in runs) / medians[key]
                       for key, runs in samples.items()}
        results[workload] = {'samples': samples, 'median_cpu_seconds': medians,
                            'median_absolute_deviation_percent': mad_percent,
                            'change_percent': 100 * (medians['candidate'] / medians['baseline'] - 1)}
    inherited = results['furi-metadata']['samples']['candidate']
    inline = results['furi-inline']['samples']['candidate'][0]
    if any(s['checksum'] != inline['checksum'] for s in inherited):
        raise RuntimeError('Inherited ruby differs from inline ruby benchmark geometry')
    report = {'platform': platform.platform(), 'compiler': subprocess.check_output(['cc', '--version'], text=True),
              'baseline_sha': '5a1ed3d1ee2a3a0837af5d1c395d301de800046b',
              'candidate_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
              'cpu_affinity': affinity, 'warmup_frames': 100, 'frames': frames, 'trials': trials,
              'results': results, 'inherited_ruby': inherited}
    Path('style-performance.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = ['| Workload | Baseline median CPU s | Candidate median CPU s | Change | Baseline/candidate MAD |',
             '| --- | ---: | ---: | ---: | ---: |']
    for name, result in results.items():
        medians = result['median_cpu_seconds']
        mad = result['median_absolute_deviation_percent']
        lines.append(f"| {name} | {medians['baseline']:.6f} | {medians['candidate']:.6f} | {result['change_percent']:+.2f}% | {mad['baseline']:.2f}%/{mad['candidate']:.2f}% |")
    lines.append(f"\nInherited ruby median CPU s: {statistics.median(s['cpu_seconds'] for s in inherited):.6f}")
    lines.append('\nSeven alternating trials, one CPU, 100 warmup frames, 1000 timed frames, 12 active events; raw samples attached. Timing is informational because shared runners are noisy.')
    summary = '\n'.join(lines) + '\n'
    print(summary)
    Path('style-performance.md').write_text(summary)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as output:
            output.write(summary)


if __name__ == '__main__':
    main()
