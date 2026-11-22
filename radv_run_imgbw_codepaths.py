#!/usr/bin/env python3
"""
Run imgbw configurations for RADV and save the CSV results in a directory.

Recommended options:
- navi48: -llc-size=64M -exec-size=256M
- navi33: -llc-size=32M -exec-size=128M
- navi23: -llc-size=32M -exec-size=128M
- vega10: -llc-size=4M -exec-size=128M
- polaris10: -llc-size=2M -exec-size=128M
- bonaire: -llc-size=512K -exec-size=64M
- pitcairn: -llc-size=512K -exec-size=64M
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build/gpu-ratemeter')
    parser.add_argument('--output-dir', type=Path, default=Path('/tmp/imgbw'))
    parser.add_argument('benchmark_args', nargs=argparse.REMAINDER,
                        help='Shared benchmark options after --, e.g. -llc-size=32M')
    args = parser.parse_args()

    benchmark_args = args.benchmark_args
    if benchmark_args[:1] == ['--']:
        benchmark_args = benchmark_args[1:]

    if any(arg.startswith('-output=') for arg in benchmark_args):
        parser.error('Use --output-dir instead of -output=')

    configurations = [
        ('0-default', {}, [], 'vkd.imgbw'),
        ('1-fragment', {'radv_image_meta_path': '1'}, [], 'vkd.imgbw'),
        ('2-compute', {'radv_image_meta_path': '2'}, [], 'vkd.imgbw'),
        ('3-fast-clear', {'radv_image_meta_path': '3'}, [], 'vkd.imgbw'),
        ('4-default-concurrent', {'RADV_EXPERIMENTAL': 'transfer_queue'}, ['-concurrent=gfx,compute,transfer'], 'vkd.imgbw'),
        ('5-default-general', {}, ['-general'], 'vkd.imgbw'),
        #('6-radeonsi', {}, [], 'gl.imgbw'),
    ]

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = args.output_dir.resolve()
    allow_overwrite = output_dir == Path('/tmp') or Path('/tmp') in output_dir.parents

    for name, _, _, _ in configurations:
        path = args.output_dir / f'{name}.csv'
        if path.exists() and not allow_overwrite:
            parser.error(f'Refusing to overwrite {path}; choose a new output directory')

    base_env = os.environ.copy()

    # Do not let an inherited override change the default configurations.
    base_env.pop('radv_image_meta_path', None)
    base_env.pop('RADV_EXPERIMENTAL', None)
    failures = []

    # Run the tests
    for name, overrides, options, test in configurations:
        env = base_env.copy()
        env.update(overrides)

        result_path = args.output_dir / f'{name}.csv'
        command = [args.binary]

        if test.startswith('vkd.'):
            command.append('-no-validator')

        command.extend(options)
        command.extend(benchmark_args)
        command.extend([f'-output={result_path}', test])

        print(f'Running {name}: {shlex.join(command)}', flush=True)

        try:
            result = subprocess.run(command, env=env, check=False)
        except OSError as error:
            print(f'Could not launch benchmark: {error}', file=sys.stderr)
            return 1

        if result.returncode:
            failures.append(name)
            print(f'  Failed (exit {result.returncode})', file=sys.stderr)
        else:
            print(f'  Saved {result_path}', flush=True)

    if failures:
        print('Failed configurations: ' + ', '.join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
