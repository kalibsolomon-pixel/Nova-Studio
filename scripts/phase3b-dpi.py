"""Phase 3-B DPI acceptance: gallery captures and workspace tests at effective
device pixel ratios 1.0, 1.25, 1.5 and 2.0 using per-process QT_SCALE_FACTOR.

No Windows display settings or input are changed. The machine's own scaling is
measured from a baseline capture and divided out, so each cell renders at the
requested effective ratio. Run while no build or benchmark is active.
"""
import argparse
import json
import os
import pathlib
import subprocess

from PIL import Image

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--build', default='studio-phase3b')
a = p.parse_args()
out = root / a.output
out.mkdir(parents=True, exist_ok=False)
tests = root / 'build' / a.build / 'tests'
env = {**os.environ, 'PATH': str(root / '.tools/qt/bin') + os.pathsep + os.environ['PATH']}


def run(binary, factor, cell, functions=()):
    cell.mkdir(parents=True, exist_ok=True)
    environment = {**env, 'QT_SCALE_FACTOR': str(factor), 'NOVA_PHASE3B_GALLERY_DIR': str(cell / 'screenshots')}
    result = subprocess.run([str(tests / (binary + '.exe')), *functions, '-o', str(cell / (binary + '.txt')) + ',txt'],
                            cwd=root, env=environment, capture_output=True, timeout=600,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    (cell / (binary + '.log')).write_bytes(result.stdout + result.stderr)
    return result.returncode


probe = out / 'probe'
run('nova-phase3b-gallery', 1, probe, ['welcome'])
with Image.open(probe / 'screenshots' / '01-welcome-compact-light.png') as image:
    native = image.width / 800
rows = []
for dpr in (1.0, 1.25, 1.5, 2.0):
    factor = dpr / native
    cell = out / f'effective-{dpr:g}'
    gallery = run('nova-phase3b-gallery', factor, cell)
    workspace = run('nova-phase3b-tests', factor, cell)
    with Image.open(cell / 'screenshots' / '01-welcome-compact-light.png') as image:
        observed = image.width / 800
    measurements = json.loads((cell / 'screenshots' / 'measurements.json').read_text(encoding='utf-8'))
    rows.append(dict(target_device_pixel_ratio=dpr, qt_scale_factor=round(factor, 4),
                     observed_device_pixel_ratio=round(observed, 4),
                     gallery_exit=gallery, workspace_tests_exit=workspace,
                     texture_1280_canvas_share=measurements['05-layers-dark'].get('central_share'),
                     minimum_canvas=[measurements['14-compact-minimum-dark'].get('central_width'),
                                     measurements['14-compact-minimum-dark'].get('central_height')]))
    print(json.dumps(rows[-1]), flush=True)
(out / 'summary.json').write_text(json.dumps(dict(native_device_pixel_ratio=native, rows=rows,
    boundary='Per-process Qt scale emulation on one Windows 11 monitor at its native scaling; '
             'widget captures via QWidget::grab. Mixed-DPI multi-monitor movement was not available.'),
    indent=2) + '\n', encoding='utf-8')
