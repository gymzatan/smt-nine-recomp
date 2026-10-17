"""Extract a user-supplied retail disc with xboxrecomp's existing XDVDFS reader."""
from pathlib import Path, PurePosixPath
import argparse
import json
import sys
from common import ROOT, digest

sys.path.insert(0, str(ROOT/'vendor/xboxrecomp'))
from tools.xiso.xdvdfs import Xiso


def extract(iso_path, destination):
    destination = Path(destination)
    identity = json.loads((ROOT/'config/retail.json').read_text())
    if digest(iso_path) != identity['iso_sha256']:
        raise ValueError('Unsupported retail ISO identity; see docs/BUILDING.md.')
    expected = {row['path']: row for row in identity['files']}
    seen = set()
    with Xiso(str(iso_path)) as image:
        for directory, entry in image.walk():
            relative = PurePosixPath(directory)/entry.name
            if relative.is_absolute() or any(part in ('.', '..') or ':' in part or '\\' in part for part in relative.parts):
                raise ValueError('Unsafe ISO entry: '+str(relative))
            name = relative.as_posix()
            if name not in expected or name in seen:
                raise ValueError('Unexpected or repeated ISO entry: '+name)
            target = destination.joinpath(*relative.parts)
            image.extract(entry, str(target))
            if digest(target) != expected[name]['sha256']:
                raise ValueError('ISO resource identity differs: '+name)
            seen.add(name)
    if seen != set(expected):
        raise ValueError('ISO extraction did not produce the complete retail file set')
    print(f'Extracted and verified {len(seen)} retail files.', flush=True)
    return identity


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    extract(args.iso, args.destination)
