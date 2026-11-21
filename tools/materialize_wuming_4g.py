#!/usr/bin/env python3
"""Restore the supplied Wuming 4G 3.0.0 source from a pinned base and overlay."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile


def materialize(output):
    root = Path(__file__).resolve().parents[1]
    variant = root / 'variants' / 'wuming-4g'
    manifest = json.loads((variant / 'manifest.json').read_text(encoding='utf-8'))
    output = Path(output).resolve()
    if output == root or root.is_relative_to(output):
        raise ValueError('Output must be a separate empty directory')
    if output.is_relative_to(root) and output.relative_to(root).parts[0] != 'build':
        raise ValueError('Inside the repository, output must be under build/')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Output must be empty; choose a new directory')
    files = []
    for entry in manifest['files']:
        relative = Path(entry['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Invalid overlay path')
        data = (variant / 'overlay' / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError('Overlay checksum mismatch: ' + entry['path'])
        files.append((relative, data))
    archive = subprocess.run(
        ['git', '-C', str(root), 'archive', '--format=tar', manifest['base_commit']],
        check=True, capture_output=True).stdout
    output.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        source.extractall(output, filter='data')
    for relative, data in files:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print(f'Restored {manifest["variant"]} {manifest["version"]}: {output}')
    print('Board: metalio47; ESP-IDF v6.1; see docs/WUMING_4G.md in the repository')
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    materialize(args.output)
