"""Audit continuation evidence and preservation without touching historical resources."""
import datetime, hashlib, json, pathlib, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-2c4/acceptance-final'
def read(path): return json.loads((out/path).read_text(encoding='utf-8-sig'))
def save(path,value):
    target = out/path
    text = json.dumps(value,indent=2)+'\n'
    newline = '\r\n' if target.exists() and b'\r\n' in target.read_bytes() else '\n'
    target.write_text(text,encoding='utf-8',newline=newline)
inventories = {}
for name in ('tracked','untracked','ignored'):
    paths = [p for p in (out/f'{name}-at-start.txt').read_text(encoding='utf-8-sig').splitlines() if p]
    inventories[name] = {'count':len(paths),'missing':[p for p in paths if not (root/p).exists()]}
protected = read('protected-local.json'); changed = []
for entry in protected:
    path = root/entry['path']
    if not path.exists(): changed.append({'path':entry['path'],'reason':'missing'}); continue
    stat = path.stat()
    now = {'bytes':stat.st_size,'mtime_ns':stat.st_mtime_ns,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    if any(now[key] != entry[key] for key in now): changed.append({'path':entry['path'],'now':now,'before':entry})
deleted = subprocess.check_output(['git','diff','--name-only','--diff-filter=D',read('baseline.json')['head']],cwd=root,text=True).splitlines()
preservation = {'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'inventories':inventories,
    'protected_count':len(protected),'protected_changed':changed,'deleted_from_start':deleted}
save('preservation-final.json',preservation)
assert not changed and not deleted and all(not item['missing'] for item in inventories.values())
processes = []
for path in sorted((out/'processes').glob('*/supervisor.json')):
    record = json.loads(path.read_text())
    recovery = path.parent/'exit-recovery.json'
    if record['case'] == 'B-pan-human' and recovery.exists():
        recovered = json.loads(recovery.read_text())
        assert recovered['pid'] == record['pid'] and recovered['exe_sha256'] == record['exe_sha256']
        record.update({key:recovered[key] for key in ('exit_code','exit_utc','timeout','classification')})
        record['wall_seconds'] = (datetime.datetime.fromisoformat(record['exit_utc'])-datetime.datetime.fromisoformat(record['launch_utc'])).total_seconds()
        record['supervision_gap'] = True
        record['exit_record'] = str(recovery)
        observations = [json.loads(line) for line in (path.parent/'observations.jsonl').read_text().splitlines()]
        record['last_screenshot'] = observations[-1]['screenshots']
    assert record['exit_code'] == 0 and not record['timeout'], record['case']
    processes.append({key:record[key] for key in ('case','pid','exe','exe_sha256','launch_utc','exit_utc','exit_code','timeout','classification','windows','wall_seconds','children','last_screenshot')})
    if record.get('supervision_gap'):
        processes[-1].update(supervision_gap=True,exit_record=record['exit_record'])
expected = {'A-navigation','B-pan-setup','C-static','D-restore-create','D-restore-reopen','E-large','F-pack-export','MC-1_21_11'}
human = out/'processes/B-pan-human/owner-verification.json'
if human.exists():
    expected.add('B-pan-human')
assert {p['case'] for p in processes} == expected
save('process-summary.json',processes)
before = read('processes/D-restore-create/saved-state.json'); after = read('processes/D-restore-reopen/restored-state.json')
assert before['sha256'] == after['sha256'] and before['active_source'] == after['active_source'] == 2
assert after['playback_indices'] == [0,1,2]
layers = [len(frame['manifest']['layers']) for frame in after['source_frames']]; assert layers == [1,2,2]
def observation(case,tag):
    rows = [json.loads(line) for line in (out/f'processes/{case}/observations.jsonl').read_text().splitlines()]
    return next(row for row in rows if row['tag'] == tag)
old_layout = observation('D-restore-create','18-timeline-floating')['screenshots']
new_layout = observation('D-restore-reopen','01-native-only-restored')['screenshots']
geometry = lambda rows:[{k:row[k] for k in ('originX','originY','width','height')} for row in rows]
assert geometry(old_layout) == geometry(new_layout)
project = root/'build/studio-c4-acceptance-final/D-restore'
assert not (project/'native-only.png').exists() and not (project/'native-only.png.mcmeta').exists()
save('processes/D-restore-reopen/assertions.json',{'passed':True,'same_native_sha256':after['sha256'],
    'playback_indices':after['playback_indices'],'active_source':2,'source_layer_counts':layers,
    'same_main_and_floating_geometry':geometry(new_layout),'png_exists':False,'mcmeta_exists':False})
native = next((root/'build/studio-c4-acceptance-final/B-pan/.nova/documents').glob('*.nvdoc'))
bstate = out/'processes/B-pan-setup/saved-state.json'
if not bstate.exists(): subprocess.run([sys.executable,str(root/'scripts/acceptance-c4-native-state.py'),str(native),str(bstate)],check=True)
b = json.loads(bstate.read_text()); a = read('processes/A-navigation/initial-state.json')
assert [f['manifest']['layers'] for f in a['source_frames']] == [f['manifest']['layers'] for f in b['source_frames']]
initial_pan = {'status':'blocked','held_pan_gestures_performed':False,
    'reason':'The available native drag API has no mouse-button or held-modifier argument. No human gesture reply was received.',
    'ready_zoom_percent':5337,'onion_enabled':True,'source_layer_pixels_unchanged':True,
    'unverified':['horizontal pan','vertical pan','diagonal pan','stable zoom during pan','mapping after pan','no paint during pan','onion alignment after pan']}
if not (out/'processes/B-pan-setup/outcome.json').exists():
    save('processes/B-pan-setup/outcome.json',initial_pan)
pan_status = 'blocked'
if human.exists():
    owner = read('processes/B-pan-human/owner-verification.json')
    assert owner['owner_accepted_functional_panning'] and owner['source_layers_and_pixel_hashes_unchanged']
    pan_status = 'owner verified; captured pan and source integrity evidence'
pack = pathlib.Path(read('minecraft/instance.json')['pack'])
for entry in read('minecraft/pack-verification.json')['files']:
    assert hashlib.sha256((pack/entry['path']).read_bytes()).hexdigest() == entry['sha256']
save('minecraft/post-runtime-pack.json',{'all_11_exported_files_unchanged':True,'editor_metadata_absent':not (pack/'.nova').exists()})
print(json.dumps({'preservation':preservation,'processes':[{'case':p['case'],'pid':p['pid'],'exit':p['exit_code'],'window':p['windows']} for p in processes],'D_restore':'passed','B_pan':pan_status},indent=2))
