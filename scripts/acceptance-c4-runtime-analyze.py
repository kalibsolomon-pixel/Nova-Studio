"""Summarize captured runtime pixels; keep software sampling limits explicit."""
import datetime, hashlib, json, pathlib, shutil
from PIL import Image, ImageDraw
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-2c4/acceptance-final/minecraft'
def stamp(value): return datetime.datetime.fromisoformat(value).timestamp()
def color(rgb):
    r,g,b = rgb[:3]
    if max(r,g,b) < 30: return 'black'
    if r > 80 and g > 60 and b < min(r,g)*0.45: return 'yellow'
    if r > max(g,b)*1.4: return 'red'
    if g > max(r,b)*1.4: return 'green'
    if b > max(r,g)*1.4: return 'blue'
    return 'mixed'
summaries = []
for manifest in sorted(out.glob('*-captures.json')):
    label = manifest.name.removesuffix('-captures.json'); rows = json.loads(manifest.read_text())
    frames, runs, samples, seen = [], [], [], set()
    first = stamp(rows[0]['begin_utc'])
    for row in rows:
        path = pathlib.Path(row['path']); image = Image.open(path).convert('RGB')
        rgb = image.getpixel((820,480)); state = color(rgb); time = stamp(row['end_utc'])-first
        samples.append({**row,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(), 'probe_820_480':rgb,
                        'probe_750_560':image.getpixel((750,560)), 'classified_color':state, 'seconds':time})
        if not runs or runs[-1]['color'] != state: runs.append({'color':state,'first_seconds':time,'last_seconds':time,'samples':1})
        else: runs[-1]['last_seconds']=time; runs[-1]['samples']+=1
        if state not in seen:
            shutil.copy2(path,out/f'{label}-{state}.png'); seen.add(state)
        crop = image.crop((590,425,882,591)); canvas = Image.new('RGB',(292,190),'#181b22'); canvas.paste(crop,(0,0))
        ImageDraw.Draw(canvas).text((5,171),f'{label}  {time:.2f}s  {rgb}',fill='white')
        frames.append(canvas)
    durations = [max(20,round((samples[i+1]['seconds']-row['seconds'])*1000)) if i+1 < len(samples) else 80 for i,row in enumerate(samples)]
    frames[0].save(out/f'{label}.gif',save_all=True,append_images=frames[1:],duration=durations,loop=0)
    summary = {'label':label,'count':len(rows),'capture_seconds':samples[-1]['seconds'], 'colors':sorted(seen),'runs':runs,
               'limits':'Software screenshot samples of a held block model. Capture latency and display cadence prevent physical timing precision. First/last runs can be partial. Probe colors classify broad fixture colors, not exact GPU/CPU equality.'}
    (out/f'{label}-analysis.json').write_text(json.dumps({'summary':summary,'samples':samples},indent=2)+'\n')
    summaries.append(summary)
(out/'runtime-observations.json').write_text(json.dumps(summaries,indent=2)+'\n')
print(json.dumps(summaries,indent=2))
