"""Owned C4 runtime comparison, preserving all prior evidence and game instances."""
import argparse
import datetime
import gzip
import hashlib
import io
import json
import pathlib
import re
import shutil
import struct
import subprocess
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PREVIOUS = ROOT / 'docs/phase-2c4/compatibility-final'
OUT = PREVIOUS / 'autonomous'
WORK = ROOT / 'build/studio-c4-autonomous-final'
INSTANCE = WORK / 'minecraft-1.21.11'


def digest(data): return hashlib.sha256(data).hexdigest()
def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8', newline='\n')
def manifest(folder):
    return [{'path':p.relative_to(folder).as_posix(),'bytes':p.stat().st_size,'sha256':digest(p.read_bytes())}
            for p in sorted(folder.rglob('*')) if p.is_file()]


class NBT:
    def __init__(self,data): self.stream=io.BytesIO(data)
    def scalar(self,fmt): return struct.unpack('>'+fmt,self.stream.read(struct.calcsize(fmt)))[0]
    def text(self): return self.stream.read(self.scalar('H')).decode('utf-8')
    def payload(self,kind):
        if kind in range(1,7): return self.scalar({1:'b',2:'h',3:'i',4:'q',5:'f',6:'d'}[kind])
        if kind==8: return self.text()
        if kind==9:
            child,count=self.scalar('B'),self.scalar('i')
            return child,[self.payload(child) for _ in range(count)]
        if kind==10:
            result={}
            while (child:=self.scalar('B')):
                name=self.text();result[name]=(child,self.payload(child))
            return result
        if kind in (7,11,12):
            count=self.scalar('i');return self.stream.read(count*{7:1,11:4,12:8}[kind])
        raise ValueError(kind)


def string(value):
    data=value.encode('utf-8');return struct.pack('>H',len(data))+data
def payload(kind,value):
    if kind in range(1,7): return struct.pack('>'+{1:'b',2:'h',3:'i',4:'q',5:'f',6:'d'}[kind],value)
    if kind==8:return string(value)
    if kind==9:
        child,items=value;return struct.pack('>Bi',child,len(items))+b''.join(payload(child,x) for x in items)
    if kind==10:return b''.join(bytes([child])+string(name)+payload(child,item) for name,(child,item) in value.items())+b'\0'
    if kind in (7,11,12):return struct.pack('>i',len(value)//{7:1,11:4,12:8}[kind])+value
    raise ValueError(kind)


def seed_inventory():
    world=INSTANCE/'saves/Demo_World';changes=[]
    for path in [world/'level.dat',*list((world/'playerdata').glob('*.dat'))]:
        raw=gzip.decompress(path.read_bytes());reader=NBT(raw)
        kind,name=reader.scalar('B'),reader.text();value=reader.payload(kind)
        assert bytes([kind])+string(name)+payload(kind,value)==raw
        player=value['Data'][1]['Player'][1] if path.name=='level.dat' else value
        before=repr({k:v for k,v in player.items() if k not in ('Inventory','SelectedItemSlot')})
        shutil.copy2(path,WORK/('inventory-before-'+path.name))
        player['Inventory']=(9,(10,[{'Slot':(1,i),'id':(8,'minecraft:'+block),'count':(3,64)}
            for i,block in enumerate(['bricks','stone','apple','white_stained_glass','oak_leaves'])]))
        player['SelectedItemSlot']=(3,0)
        assert before==repr({k:v for k,v in player.items() if k not in ('Inventory','SelectedItemSlot')})
        path.write_bytes(gzip.compress(bytes([kind])+string(name)+payload(kind,value)))
        changes.append({'path':str(path),'changed_fields':['Inventory','SelectedItemSlot'],
                        'other_player_fields_unchanged':True,'original_nbt_roundtrip_exact':True})
    save(OUT/'inventory-fixture.json',{'changes':changes,'demo_time_rules_authentication_unchanged':True})


def install(variant,tag):
    assert tag.replace('-','').replace('_','').isalnum()
    rows=json.loads((PREVIOUS/'pack-manifest.json').read_text())
    row=next(r for r in rows if r['variant']==variant)
    archive=ROOT/row['archive'];assert digest(archive.read_bytes())==row['sha256']
    active=INSTANCE/'resourcepacks/C4-active';active.mkdir(parents=True,exist_ok=True)
    # Primary comparisons override only the opaque block, square control and item.
    # Retain all previous disposable bytes rather than leaving stale material overrides.
    existing=list(active.rglob('*'))
    for path in existing:
        if not path.is_file():continue
        target=WORK/'retained-active'/tag/path.relative_to(active)
        assert path.resolve().is_relative_to(WORK.resolve()) and target.resolve().is_relative_to(WORK.resolve())
        target.parent.mkdir(parents=True,exist_ok=True);path.rename(target)
    selected=[item for item in row['files'] if '/block/oak_leaves.' not in item['path'] and '/block/white_stained_glass.' not in item['path']]
    with zipfile.ZipFile(archive) as z:
        assert sorted(z.namelist())==sorted(r['path'] for r in row['files'])
        for item in selected:
            relative=pathlib.PurePosixPath(item['path'])
            assert not relative.is_absolute() and '..' not in relative.parts
            target=active.joinpath(*relative.parts);data=z.read(item['path'])
            assert digest(data)==item['sha256']
            target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    assert manifest(active)==[{'path':x['path'],'bytes':x['bytes'],'sha256':x['sha256']} for x in sorted(selected,key=lambda x:x['path'])]
    save(OUT/'runtime'/tag/'installed-pack.json',{'utc':utc(),'variant':variant,'archive_sha256':row['sha256'],
         'active':str(active),'files':manifest(active),'previous_stdout_bytes':(WORK/'live/stdout.txt').stat().st_size if (WORK/'live/stdout.txt').exists() else 0})
    print(json.dumps({'variant':variant,'tag':tag,'files_verified':len(selected)}))


def prepare():
    old=json.loads((PREVIOUS/'instance.json').read_text());source=pathlib.Path(old['instance'])
    assert not INSTANCE.exists();INSTANCE.mkdir()
    for name in ['baseline.json','protected-at-start.json','tracked-at-start.txt.gz','untracked-at-start.txt.gz','ignored-at-start.txt.gz']:
        shutil.copy2(WORK/name,OUT/name)
    shutil.copytree(source/'natives',INSTANCE/'natives')
    shutil.copytree(source/'saves',INSTANCE/'saves')
    shutil.copy2(source/'options.txt',INSTANCE/'options.txt')
    shutil.copy2(source/'options.txt',WORK/'options.original.txt')
    save(OUT/'saved-world-before.json',{'source':str(source/'saves'),'files':manifest(source/'saves')})
    seed_inventory()
    text=(INSTANCE/'options.txt').read_text()
    replacements={'mipmapLevels':'2','resourcePacks':'["vanilla","file/C4-active"]','incompatibleResourcePacks':'[]'}
    lines=[line.split(':',1)[0]+':'+replacements[line.split(':',1)[0]] if line.split(':',1)[0] in replacements else line for line in text.splitlines()]
    (INSTANCE/'options.txt').write_text('\n'.join(lines)+'\n')
    arguments=[arg.replace(old['instance'],str(INSTANCE)) for arg in old['args']]
    save(OUT/'instance.json',{**old,'instance':str(INSTANCE),'args':arguments,'pack':str(INSTANCE/'resourcepacks/C4-active'),
                             'source':str(source),'preparation':'Fresh copy after normal Save and Quit; Inventory/SelectedItemSlot only; options mip2 and one experimental pack'})
    install('nova_original','A-nova-mip2')


def launch():
    info=json.loads((OUT/'instance.json').read_text());live=WORK/'live';live.mkdir(exist_ok=False)
    started=utc();command=[info['java'],*info['args']]
    with (live/'stdout.txt').open('wb') as stdout,(live/'stderr.txt').open('wb') as stderr:
        p=subprocess.Popen(command,cwd=INSTANCE,stdout=stdout,stderr=stderr)
        save(OUT/'process.json',{'pid':p.pid,'launch_utc':started,'command':command,'cwd':str(INSTANCE),'java_sha256':digest(pathlib.Path(info['java']).read_bytes())})
        code=p.wait()
    shutil.copy2(live/'stdout.txt',OUT/'stdout-complete.txt');shutil.copy2(live/'stderr.txt',OUT/'stderr-complete.txt')
    save(OUT/'exit.json',{'pid':p.pid,'exit_code':code,'exit_utc':utc(),'forced_exit':False})


def snapshot(tag):
    dest=OUT/'runtime'/tag;dest.mkdir(exist_ok=True,parents=True)
    stdout=WORK/'live/stdout.txt'
    data=stdout.read_bytes();installed=dest/'installed-pack.json'
    start=json.loads(installed.read_text())['previous_stdout_bytes'] if installed.exists() else 0
    (dest/'stdout-reload-and-observation.txt').write_bytes(data[start:])
    shutil.copy2(INSTANCE/'options.txt',dest/'options-observed.txt')
    save(dest/'post-observation.json',{'utc':utc(),'stdout_begin_offset':start,'stdout_end_offset':len(data),
                                     'pack_files':manifest(INSTANCE/'resourcepacks/C4-active')})
    print(json.dumps({'tag':tag,'stdout_end_offset':len(data)}))


def allocation():
    info=json.loads((PREVIOUS/'instance.json').read_text());java=pathlib.Path(info['java'])
    cp=info['args'][info['args'].index('-cp')+1]
    expected=json.loads((PREVIOUS/'source-artifacts.json').read_text())['client_sha1']
    assert hashlib.sha1(pathlib.Path(cp.split(';')[-1]).read_bytes()).hexdigest()==expected
    classes=WORK/'allocation-trace';classes.mkdir(exist_ok=True)
    source=ROOT/'scripts/c4-runtime-verification/AllocationTrace.java'
    compile_result=subprocess.run([str(java.with_name('javac.exe')),'-cp',cp,'-d',str(classes),str(source)],capture_output=True)
    (OUT/'allocation-build.txt').write_bytes(compile_result.stdout+compile_result.stderr)
    if compile_result.returncode:raise RuntimeError('Allocation harness compile failed; see allocation-build.txt')
    result=subprocess.run([str(java),'-Djava.library.path='+str(pathlib.Path(info['instance'])/'natives'),'-cp',str(classes)+';'+cp,
                           'c4.AllocationTrace',str(PREVIOUS/'fixtures'),str(OUT/'allocation-trace.json')],cwd=classes,capture_output=True)
    (OUT/'allocation-stdout.txt').write_bytes(result.stdout);(OUT/'allocation-stderr.txt').write_bytes(result.stderr)
    if result.returncode:raise RuntimeError('Allocation harness failed; see allocation-stderr.txt')
    rows=json.loads((OUT/'allocation-trace.json').read_text())
    for row in rows:
        assert len(row['actual_client_allocations'])==4
        assert all(a['requested_levels']==row['effective']+2 for a in row['actual_client_allocations'])
        assert len(row['actual_client_uploads'])==4*(row['effective']+1)
        assert set(u['mip_level'] for u in row['actual_client_uploads'])==set(range(row['effective']+1))
    print(json.dumps({'actual_official_client_allocation_cases':len(rows),'all_extra_level_requests_confirmed':True}))


def audit(tag):
    import psutil
    baseline=json.loads((OUT/'baseline.json').read_text());entries=json.loads((OUT/'protected-at-start.json').read_text())
    changed=[];unexpected=[];missing=[];unhashed=[]
    append_names={'EXPERIMENT_MATRIX.md','ROOT_CAUSE.md','CORRECTIONS.md','FINAL_ACCEPTANCE.md'}
    mutable='build/studio-c4-compatibility-final/minecraft-1.21.11/'
    for old in entries:
        path=ROOT/old['path']
        if not path.is_file():missing.append(old['path']);continue
        stat=path.stat();data=path.read_bytes()
        now={'bytes':stat.st_size,'mtime_ns':stat.st_mtime_ns,'sha256':digest(data)}
        if old.get('sha256') is None:
            unhashed.append({'path':old['path'],'now_sha256':now['sha256'],'reason':old.get('reason')});continue
        if all(old[k]==now[k] for k in now):continue
        reason=None
        if old['path'].startswith(mutable+'saves/') or old['path']==mutable+'options.txt':reason='normal Save and Quit / Quit Game of initially live isolated instance; saved source is retained and copied for experiments'
        if path.parent==PREVIOUS and path.name in append_names:
            previous=(OUT/'prior-reports'/path.name).read_bytes()
            if data.startswith(previous):reason='authorized append-only continuation; prior report archived byte-exact'
        item={'path':old['path'],'before':old,'after':now,'reason':reason}
        changed.append(item)
        if reason is None:unexpected.append(item)
    inventory={}
    for name in ['tracked','untracked','ignored']:
        paths=gzip.decompress((OUT/(name+'-at-start.txt.gz')).read_bytes()).decode().splitlines()
        paths=[p for p in paths if p]
        inventory[name]={'count':len(paths),'missing':[p for p in paths if not (ROOT/p).exists()]}
    originals=ROOT/'docs/phase-2c4/acceptance-final/minecraft/exported-pack'
    old_manifest=json.loads((ROOT/'docs/phase-2c4/acceptance-final/minecraft/pack-verification.json').read_text())['files']
    assert all(digest((originals/r['path']).read_bytes())==r['sha256'] for r in old_manifest)
    # Recheck every original prior-investigation protected file without rewriting its report.
    prior=json.loads((PREVIOUS/'protected-at-start.json').read_text());prior_changes=[]
    for old in prior:
        p=ROOT/old['path'];s=p.stat()
        if p.parent==PREVIOUS and p.name in append_names:
            data=(OUT/'prior-reports'/p.name).read_bytes()
            assert digest(data)==old['sha256']
            continue
        if s.st_size!=old['bytes'] or s.st_mtime_ns!=old['mtime_ns'] or digest(p.read_bytes())!=old['sha256']:prior_changes.append(old['path'])
    artifacts=json.loads((OUT/'nova-roundtrip/results.json').read_text())
    assert artifacts['passed'] and len(artifacts['variants'])==26 and all(r['passed'] for r in artifacts['variants'])
    trace=json.loads((OUT/'allocation-trace.json').read_text())
    assert len(trace)==12 and all(all(a['requested_levels']==r['effective']+2 for a in r['actual_client_allocations']) for r in trace)
    process=json.loads((OUT/'process.json').read_text())
    status={'pid':process['pid'],'query_only':True}
    try:
        p=psutil.Process(process['pid']);status.update(running=p.is_running(),created_unix=p.create_time(),exe=p.exe())
    except psutil.NoSuchProcess:status['running']=False
    core=ROOT/'build/studio-c4-acceptance-release/src/nova-core.lib'
    save(OUT/('audit-'+tag+'.json'),{'utc':utc(),'starting_head':baseline['head'],'inventories':inventory,
        'protected_count':len(entries),'expected_changes':changed,'unexpected_changes':unexpected,'missing':missing,
        'initially_unreadable_files':unhashed,'previous_3522_protected_files_changed':prior_changes,
        'original_11_pack_files_unchanged':True,'new_nova_roundtrip_variants_passed':26,'official_allocation_trace_cases':12,
        'nova_core_library_sha256':digest(core.read_bytes()),'game_process':status,
        'production_diff':subprocess.check_output(['git','diff',baseline['head'],'--','src','tests','CMakeLists.txt'],cwd=ROOT,text=True)})
    assert not unexpected and not missing and not prior_changes and all(not v['missing'] for v in inventory.values())
    print(json.dumps({'protected':len(entries),'expected_changes':len(changed),'unexpected_changes':len(unexpected),
                      'prior_protected_changed':len(prior_changes),'original_pack_unchanged':True,'client_running':status['running']}))


def verify():
    docs=[*PREVIOUS.glob('*.md'),OUT/'BASELINE.md']
    links=[]
    for path in docs:
        for match in re.finditer(r'\[[^\]]*\]\(([^)]+)\)',path.read_text(encoding='utf-8')):
            target=match.group(1)
            if target.startswith('https:'):continue
            assert (path.parent/target.split('#')[0]).exists(),(str(path),target)
            links.append(target)
    for name in ['EXPERIMENT_MATRIX.md','ROOT_CAUSE.md','CORRECTIONS.md','FINAL_ACCEPTANCE.md']:
        assert (PREVIOUS/name).read_bytes().startswith((OUT/'prior-reports'/name).read_bytes())
    inputs=json.loads((PREVIOUS/'fixture-manifest.json').read_text())
    for row in inputs:
        base=PREVIOUS/'fixtures'/row['name'];assert digest((base/'texture.png').read_bytes())==row['png']['sha256']
        assert digest((base/'texture.png.mcmeta').read_bytes())==row['metadata_sha256']
    owner=json.loads((OUT/'owner-case-manifest.json').read_text())
    for case in owner:
        row=next(r for r in inputs if r['name']==case['variant'])
        assert next(f['sha256'] for f in case['pack_files'] if f['path'].endswith('/bricks.png'))==row['png']['sha256']
        assert next(f['sha256'] for f in case['pack_files'] if f['path'].endswith('/bricks.png.mcmeta'))==row['metadata_sha256']
    results=json.loads((OUT/'nova-roundtrip/results.json').read_text())
    assert len(results['variants'])==26 and all(r['passed'] and r['sheet_exact'] and r['metadata_exact'] for r in results['variants'])
    suites=json.loads((OUT/'focused-release/summary.json').read_text())
    assert len(suites)==4 and sum(s['passed'] for s in suites)==43 and all(s['exit']==s['failed']==s['skipped']==0 for s in suites)
    save(OUT/'evidence-verification.json',{'local_links_valid':len(links),'prior_report_prefixes_exact':4,
          'original_fixture_pairs_unchanged':26,'owner_templates_match_canonical_inputs':4,
          'Nova_exact_import_export_reimport_variants':26,'focused_suites_passed':4,'focused_checks':43,
          'new_production_code':False,'new_claimed_game_B_C_D_passes':False})
    print('Evidence links, preserved reports, original hashes, owner cases and test results verified.')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=['prepare','launch','install','snapshot','allocation','audit','verify'])
    parser.add_argument('--variant');parser.add_argument('--tag');args=parser.parse_args()
    if args.mode=='install':install(args.variant,args.tag)
    elif args.mode=='snapshot':snapshot(args.tag)
    elif args.mode=='audit':audit(args.tag)
    else:globals()[args.mode]()
