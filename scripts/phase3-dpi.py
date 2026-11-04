"""Per-process Qt scale emulation; no Windows settings/input changes."""
import argparse
import json
import os
import pathlib
import subprocess
from PIL import Image

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--build', default='studio-phase3-release')
a = p.parse_args()
out = root/a.output
out.mkdir(parents=True, exist_ok=False)
base = root/'build'/a.build/'tests'
env = {**os.environ, 'PATH': str(root/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
def run(factor, label, binary, functions):
    cell = out/label
    cell.mkdir()
    environment = {**env, 'QT_SCALE_FACTOR': str(factor),
                   'NOVA_PHASE3_EVIDENCE_DIR': str(cell/'screenshots'),
                   'NOVA_C8_EVIDENCE_DIR': str(cell/'c8-screenshots')}
    result = subprocess.run([str(base/(binary+'.exe')), *functions, '-o', str(cell/'checks.txt')+',txt',
                             '-o', str(cell/'checks.xml')+',junitxml'], cwd=root, env=environment,
                            capture_output=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
    (cell/'process.log').write_bytes(result.stdout+result.stderr)
    if result.returncode: raise RuntimeError((label, result.returncode))
    return cell
baseline = run(1, 'default-qt-factor', 'nova-phase3-tests', ['compactAndNormalWorkspaceGallery'])
file = next((baseline/'screenshots').glob('welcome-compact*.png'))
with Image.open(file) as image:
    default_dpr = image.width/800
rows = []
for dpr in (1, 1.25, 1.5, 2):
    factor = dpr/default_dpr
    label = f'effective-{dpr:g}'
    cell = run(factor, label, 'nova-phase3-tests',
               ['compactAndNormalWorkspaceGallery', 'helpAndAccessibleAssetControls'])
    files = list((cell/'screenshots').glob('welcome-compact*.png'))
    for file in files:
        with Image.open(file) as image:
            observed = image.width/800
            assert abs(observed-dpr) < .01, (file, observed, dpr)
    run(factor, label+'-c8', 'nova-c8-workspace-tests', [])
    rows.append(dict(target_device_pixel_ratio=dpr, qt_scale_factor=factor,
                     observed_device_pixel_ratio=observed, phase3_and_c8_passed=True))
(out/'summary.json').write_text(json.dumps(dict(default_device_pixel_ratio=default_dpr, rows=rows,
    boundary='Qt process emulation and widget captures; no real Windows DPI or mixed-monitor certification'), indent=2)+'\n', encoding='utf-8')
print(json.dumps(rows, indent=2))
