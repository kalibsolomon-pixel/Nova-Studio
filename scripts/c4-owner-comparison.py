"""Prepare/verify four deterministic owner-run demo comparisons; never sends UI input.

Run mode is for an owner to invoke after closing the current isolated client.
Every run uses a new copy, official --demo, fixed test identity and token 0.
No reset of demo time, game mode, authentication, rules or personal installation.
"""
import argparse
import datetime
import gzip
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import uuid
import zipfile
import psutil

ROOT=pathlib.Path(__file__).resolve().parents[1]
PREVIOUS=ROOT/'docs/phase-2c4/compatibility-final'
OUT=PREVIOUS/'autonomous'
WORK=ROOT/'build/studio-c4-autonomous-final'
CASES={'A':('nova_original',2),'B':('nova_original',4),'C':('reference',4),'D':('square',4)}

def sha(data):return hashlib.sha256(data).hexdigest()
def save(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8',newline='\n')
def files(folder):
    return [{'path':p.relative_to(folder).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p.read_bytes())}
            for p in sorted(folder.rglob('*')) if p.is_file()]
def parser_module():
    spec=importlib.util.spec_from_file_location('c4runtime',ROOT/'scripts/compatibility-c4-runtime.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def seed(instance):
    rt=parser_module();world=instance/'saves/Demo_World'
    for path in [world/'level.dat',*list((world/'playerdata').glob('*.dat'))]:
        raw=gzip.decompress(path.read_bytes());r=rt.NBT(raw)
        kind,name=r.scalar('B'),r.text();value=r.payload(kind)
        assert bytes([kind])+rt.string(name)+rt.payload(kind,value)==raw
        player=value['Data'][1]['Player'][1] if path.name=='level.dat' else value
        other=repr({k:v for k,v in player.items() if k not in ['Inventory','SelectedItemSlot']})
        player['Inventory']=(9,(10,[{'Slot':(1,i),'id':(8,'minecraft:'+block),'count':(3,64)}
                                  for i,block in enumerate(['bricks','stone','apple','white_stained_glass','oak_leaves'])]))
        player['SelectedItemSlot']=(3,0)
        assert other==repr({k:v for k,v in player.items() if k not in ['Inventory','SelectedItemSlot']})
        path.write_bytes(gzip.compress(bytes([kind])+rt.string(name)+rt.payload(kind,value),mtime=0))

def prepare():
    source_info=json.loads((PREVIOUS/'instance.json').read_text())
    source=pathlib.Path(source_info['instance'])
    assert files(source/'saves')==json.loads((OUT/'saved-world-before.json').read_text())['files']
    records=[];pack_rows=json.loads((PREVIOUS/'pack-manifest.json').read_text())
    for case,(variant,mip) in CASES.items():
        template=WORK/'owner-cases'/case/'template'
        assert not template.exists();template.mkdir(parents=True)
        shutil.copytree(source/'natives',template/'natives');shutil.copytree(source/'saves',template/'saves')
        seed(template)
        changes={'mipmapLevels':str(mip),'resourcePacks':'["vanilla","file/C4-active"]','incompatibleResourcePacks':'[]'}
        lines=[line.split(':',1)[0]+':'+changes[line.split(':',1)[0]] if line.split(':',1)[0] in changes else line
               for line in (source/'options.txt').read_text().splitlines()]
        (template/'options.txt').write_text('\n'.join(lines)+'\n',newline='\n')
        row=next(r for r in pack_rows if r['variant']==variant);archive=ROOT/row['archive']
        assert sha(archive.read_bytes())==row['sha256']
        with zipfile.ZipFile(archive) as z:
            selected=[r for r in row['files'] if '/block/oak_leaves.' not in r['path'] and '/block/white_stained_glass.' not in r['path']]
            for item in selected:
                p=pathlib.PurePosixPath(item['path']);assert not p.is_absolute() and '..' not in p.parts
                data=z.read(item['path']);assert sha(data)==item['sha256']
                target=template/'resourcepacks/C4-active'/pathlib.Path(*p.parts);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        arguments=[a.replace(source_info['instance'],str(template)) for a in source_info['args']]
        record={'case':case,'variant':variant,'mipmap_setting':mip,'expected_effective_mip':min(mip,3) if variant!='square' else mip,
                'template':str(template),'java':source_info['java'],'args':arguments,
                'java_sha256':source_info['java_sha256'],'pack_files':files(template/'resourcepacks/C4-active'),
                'world_files':files(template/'saves'),'options_sha256':sha((template/'options.txt').read_bytes()),
                'source_archive_sha256':row['sha256'],'runtime_status':'prepared, not run'}
        save(template.parent/'case.json',record);records.append(record)
        launcher=WORK/'owner-cases'/('Run-'+case+'.cmd')
        launcher.write_text('@"'+str(ROOT/'.tools/python/Scripts/python.exe')+'" "'+str(pathlib.Path(__file__).resolve())+'" run '+case+'\r\n@pause\r\n',newline='')
    save(OUT/'owner-case-manifest.json',records)
    print(json.dumps({'prepared_cases':list(CASES),'launched':False}))

def verify():
    records=json.loads((OUT/'owner-case-manifest.json').read_text())
    for row in records:
        template=pathlib.Path(row['template'])
        assert files(template/'resourcepacks/C4-active')==row['pack_files']
        assert files(template/'saves')==row['world_files']
        assert sha((template/'options.txt').read_bytes())==row['options_sha256']
        assert sha(pathlib.Path(row['java']).read_bytes())==row['java_sha256']
        options=dict(line.split(':',1) for line in (template/'options.txt').read_text().splitlines() if ':' in line)
        assert int(options['mipmapLevels'])==row['mipmap_setting']
        assert json.loads(options['resourcePacks'])==['vanilla','file/C4-active']
        assert '--demo' in row['args'] and row['args'][row['args'].index('--accessToken')+1]=='0'
    # All worlds begin with exactly the same terrain/player/time/rules; artwork/level are deliberate variables.
    assert all(r['world_files']==records[0]['world_files'] for r in records)
    a,b,c,d=records
    assert a['pack_files']==b['pack_files']
    assert next(f['sha256'] for f in a['pack_files'] if f['path'].endswith('/bricks.png'))!=next(f['sha256'] for f in c['pack_files'] if f['path'].endswith('/bricks.png'))
    save(OUT/'owner-case-verification.json',{'cases_verified':4,'matching_worlds':True,'A_B_pack_byte_exact':True,
           'source_reference_encoded_png_different':True,'one_experimental_pack_only':True,'demo_and_placeholder_authentication':True,'game_launched_by_verification':False})
    print('Four owner cases verified; no game launched.')

def running_owned_client():
    for path in [OUT/'process.json',*list((OUT/'owner-runs').glob('*/process.json'))]:
        if not path.exists():continue
        row=json.loads(path.read_text())
        try:
            p=psutil.Process(row['pid']);expected=datetime.datetime.fromisoformat(row['launch_utc']).timestamp()
            if p.is_running() and abs(p.create_time()-expected)<3:return row['pid']
        except psutil.NoSuchProcess:pass
    return None

def run(case):
    verify()
    if (pid:=running_owned_client()):raise RuntimeError(f'Owned Minecraft process {pid} is still running. Save and Quit, then Quit Game before starting a case. No running session was changed.')
    row=json.loads((WORK/'owner-cases'/case/'case.json').read_text());template=pathlib.Path(row['template'])
    run_id=case+'-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]
    session=WORK/'owner-sessions'/run_id;shutil.copytree(template,session)
    result=OUT/'owner-runs'/run_id;result.mkdir(parents=True)
    command=[row['java'],*[a.replace(str(template),str(session)) for a in row['args']]]
    before=files(session/'resourcepacks/C4-active')
    with (result/'stdout.txt').open('wb') as out,(result/'stderr.txt').open('wb') as err:
        started=datetime.datetime.now(datetime.timezone.utc).isoformat();p=subprocess.Popen(command,cwd=session,stdout=out,stderr=err)
        save(result/'process.json',{'case':case,'pid':p.pid,'launch_utc':started,'command':command,'session':str(session)})
        print('Case',case,'started. Use Play Demo World, Continue Playing, select 1 for bricks; capture held/placed playback. Finish with Save and Quit, then Quit Game.')
        code=p.wait()
    images=[]
    for image in sorted((session/'screenshots').glob('*.png')):
        dest=result/'screenshots'/image.name;dest.parent.mkdir(exist_ok=True);shutil.copy2(image,dest)
        images.append({'path':str(dest),'sha256':sha(dest.read_bytes())})
    after=files(session/'resourcepacks/C4-active')
    shutil.copy2(session/'options.txt',result/'options-after.txt')
    save(result/'result.json',{'case':case,'exit_code':code,'forced_exit':False,'pack_unchanged':before==after,
          'screenshots':images,'rendering_classification':'owner observations required; no automatic pass inferred',
          'expected_mipmap_setting':row['mipmap_setting'],'expected_effective_mip':row['expected_effective_mip']})
    save(result/'observations-to-complete.json',{'case':case,'mipmap_UI_value':None,'held_frames_advance':None,
         'placed_frames_advance':None,'black_texture':None,'F3_target_block':None,'square_stone_control':None,'apple_item_atlas_control':None,'notes':''})
    print('Evidence preserved:',result)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('mode',choices=['prepare','verify','run']);parser.add_argument('case',nargs='?',choices=list(CASES))
    args=parser.parse_args()
    if args.mode=='run':
        if not args.case:parser.error('run requires A, B, C or D')
        run(args.case)
    else:globals()[args.mode]()
