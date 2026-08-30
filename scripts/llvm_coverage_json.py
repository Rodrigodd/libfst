#!/usr/bin/env python3
"""Merge clang profraw files and export an llvm-cov JSON coverage report.

Invoked by the `coverage-json` target of a build directory configured with
-Dllvm_coverage=true, after `meson test` has produced the raw profiles.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-profdata', default='llvm-profdata')
    parser.add_argument('--llvm-cov', default='llvm-cov')
    parser.add_argument('--profraw-dir', type=Path, required=True,
                        help='directory the tests wrote their .profraw files to')
    parser.add_argument('--output', type=Path, required=True,
                        help='path of the JSON report to write')
    parser.add_argument('--ignore-filename-regex', action='append', default=[],
                        help='exclude matching source files, may be repeated')
    parser.add_argument('--keep-stale', action='store_true',
                        help='merge profiles older than the instrumented binaries too')
    parser.add_argument('objects', nargs='+',
                        help='instrumented binaries to read the coverage mapping from')
    return parser.parse_args()


def collect_profraws(profraw_dir, objects, keep_stale):
    """Return the profraw files to merge, dropping the ones left over from an
    earlier build. Stale profiles do not match the coverage mapping of the
    binaries they get exported against, which makes llvm-cov fail with a
    function hash mismatch."""
    profraws = sorted(profraw_dir.glob('*.profraw'))
    if not profraws:
        sys.exit(f'no .profraw files in {profraw_dir}, run `meson test` first')

    if keep_stale:
        return profraws

    newest_object = max(os.path.getmtime(obj) for obj in objects)
    fresh = [p for p in profraws if p.stat().st_mtime >= newest_object]
    stale = len(profraws) - len(fresh)
    if stale:
        print(f'ignoring {stale} profile(s) predating the current build')
    if not fresh:
        sys.exit('every .profraw predates the current build, re-run `meson test`')
    return fresh


def run(command):
    try:
        return subprocess.run(command, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr)
        sys.exit(f'{command[0]} failed with exit code {error.returncode}')


def print_summary(report):
    files = report['data'][0]['files']
    width = max(len(Path(entry['filename']).name) for entry in files)
    for entry in sorted(files, key=lambda e: e['summary']['lines']['percent']):
        summary = entry['summary']
        print('  {:<{}}  lines {:5.1f}%  regions {:5.1f}%  branches {:5.1f}%'.format(
            Path(entry['filename']).name, width,
            summary['lines']['percent'],
            summary['regions']['percent'],
            summary['branches']['percent'],
        ))
    totals = report['data'][0]['totals']
    print('  total: lines {:.1f}%  regions {:.1f}%  branches {:.1f}%'.format(
        totals['lines']['percent'],
        totals['regions']['percent'],
        totals['branches']['percent'],
    ))


def main():
    args = parse_args()

    profraws = collect_profraws(args.profraw_dir, args.objects, args.keep_stale)

    profdata = args.output.with_suffix('.profdata')
    run([args.llvm_profdata, 'merge', '-sparse', '-o', str(profdata),
         *(str(p) for p in profraws)])

    command = [args.llvm_cov, 'export', '--format=text',
               f'--instr-profile={profdata}']
    for regex in args.ignore_filename_regex:
        command.append(f'--ignore-filename-regex={regex}')
    for obj in args.objects:
        command += ['--object', obj]

    export = run(command)
    args.output.write_text(export.stdout)

    print(f'wrote {args.output}')
    print_summary(json.loads(export.stdout))


if __name__ == '__main__':
    main()
