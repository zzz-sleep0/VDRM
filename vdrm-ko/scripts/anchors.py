#!/usr/bin/env python3
"""Extract cross-check anchors from the snapshot's real target module."""
import argparse
from pathlib import Path
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('module', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
text = subprocess.check_output(['modprobe', '--show-modversions', str(a.module)], text=True)
values = {n: c for c, n in (line.split() for line in text.splitlines())}
for name in ['module_layout', 'fget', 'mutex_lock']:
    if name not in values:
        raise SystemExit('Reference module must import module_layout, fget and mutex_lock; choose msm_drm.ko or original working vdrm.ko')
if a.output.exists():
    raise SystemExit('Refusing to overwrite ' + str(a.output))
a.output.write_text(''.join(f'{n}\t{c}\n' for n, c in sorted(values.items())))
