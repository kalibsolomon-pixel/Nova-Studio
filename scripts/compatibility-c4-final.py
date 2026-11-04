"""C4 compatibility evidence tooling; independent fixtures, no Nova serializer calls.

All writes are confined to new compatibility-final evidence/work directories.
The historical client installation, original fixtures and demo world are read-only.
"""
import argparse
import collections
import gzip
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import time
import zipfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/phase-2c4/compatibility-final'
WORK = ROOT / 'build/studio-c4-compatibility-final'
OLD = ROOT / 'docs/phase-2c4/acceptance-final'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT).decode('utf-8')


def record(path):
    s = path.stat()
    return {'path': path.relative_to(ROOT).as_posix(), 'bytes': s.st_size,
            'mtime_ns': s.st_mtime_ns, 'sha256': sha(path.read_bytes())}


def baseline():
    assert not OUT.exists(), 'Baseline is write-once'
    tracked = git('ls-files', '-z').split('\0')[:-1]
    untracked = git('ls-files', '--others', '--exclude-standard', '-z').split('\0')[:-1]
    ignored = git('ls-files', '--others', '--ignored', '--exclude-standard', '-z').split('\0')[:-1]
    state = {'branch': git('branch', '--show-current').strip(), 'head': git('rev-parse', 'HEAD').strip(),
             'status': git('status', '--short', '--branch'), 'diff': git('diff', '--name-status'),
             'tracked_count': len(tracked), 'untracked_count': len(untracked), 'ignored_count': len(ignored)}
    OUT.mkdir(parents=True)
    for name, paths in [('tracked', tracked), ('untracked', untracked), ('ignored', ignored)]:
        data = ('\n'.join(paths) + '\n').encode()
        if name == 'ignored':
            (OUT / (name + '-at-start.txt.gz')).write_bytes(gzip.compress(data, mtime=0))
        else:
            (OUT / (name + '-at-start.txt')).write_bytes(data)
    protected = set(p['path'] for p in json.loads((OLD / 'protected-local.json').read_text()))
    protected.update(p for p in tracked if p.startswith('docs/phase-2c4/') or p.startswith('tests/fixtures/'))
    protected.update(p for p in ignored + untracked if
                     p.endswith(('.nvdoc', '.ini')) or '/recovery/' in p or '/animation-exports/' in p
                     or '/minecraft-1.21.11/' in p or '/runtime-captures/' in p)
    entries = [record(ROOT / p) for p in sorted(protected) if (ROOT / p).is_file()]
    write_json(OUT / 'protected-at-start.json', entries)
    write_json(OUT / 'baseline.json', state)
    print(json.dumps({**state, 'protected_count': len(entries)}, indent=2))


COLORS = [(240, 40, 30), (30, 210, 65), (35, 65, 240), (245, 205, 25)]


def pixels(width, height, index, alpha='opaque'):
    """Independent deterministic construction, matching the original C input recipe."""
    data = bytearray()
    for y in range(height):
        for x in range(width):
            rgb = COLORS[index]
            if x == index + 1 or y == index + 1:
                rgb = (255, 255, 255)
            if x == y == 0:
                rgb = (10 + index * 40, 20, 30)
            a = 255
            if alpha == 'binary' and x < width // 3:
                a = 0
            if alpha == 'partial' and x < 2 * width // 3:
                a = 96 if x >= width // 3 else 0
            data.extend((*rgb, a))
    return bytes(data)


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def encode_png(width, height, rgba):
    assert len(rgba) == width * height * 4
    # Plain RGBA8, filter 0, one IDAT; intentionally independent of Qt PNG encoding.
    scan = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b'')


def inspect_png(path, fw, fh):
    from PIL import Image
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, chunks = 8, []
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos+4])[0]
        kind, payload = data[pos+4:pos+8], data[pos+8:pos+8+length]
        crc = struct.unpack('>I', data[pos+8+length:pos+12+length])[0]
        assert zlib.crc32(kind + payload) == crc
        chunks.append({'type': kind.decode(), 'bytes': length, 'crc_valid': True})
        pos += 12 + length
    assert pos == len(data) and chunks[-1]['type'] == 'IEND'
    im = Image.open(path).convert('RGBA'); w, h = im.size
    assert w % fw == h % fh == 0
    cells = [sha(im.crop((x, y, x+fw, y+fh)).tobytes()) for y in range(0, h, fh) for x in range(0, w, fw)]
    return {'sha256': sha(data), 'bytes': len(data), 'width': w, 'height': h,
            'ihdr': list(struct.unpack('>IIBBBBB', data[16:29])), 'chunks': chunks,
            'rgba_sha256': sha(im.tobytes()), 'cell_rgba_sha256': cells,
            'alpha_counts': dict(collections.Counter(im.getchannel('A').tobytes()))}


def fixtures():
    dest = OUT / 'fixtures'
    dest.mkdir(exist_ok=True)
    source = OLD / 'minecraft/exported-pack/assets/minecraft/textures/block/bricks.png'
    original_meta = json.loads(source.with_suffix('.png.mcmeta').read_text())
    variants = [('nova_original',16,8,2,'opaque',None,None),
                ('reference',16,8,2,'opaque',None,None),
                ('square',16,16,2,'opaque',None,None),
                ('tall',8,16,2,'opaque',None,None),
                ('wide4',32,8,2,'opaque',None,None),
                ('vertical',16,8,1,'opaque',None,None),
                ('horizontal',16,8,4,'opaque',None,None),
                ('binary_auto',16,8,2,'binary',None,None),
                ('partial_auto',16,8,2,'partial',None,None)]
    for strategy in ['auto','mean','cutout','strict_cutout','dark_cutout']:
        variants.append(('opaque_strategy_'+strategy,16,8,2,'opaque',strategy,None))
        variants.append(('binary_strategy_'+strategy,16,8,2,'binary',strategy,None))
        variants.append(('partial_strategy_'+strategy,16,8,2,'partial',strategy,None))
    for bias in [-0.1, 0.1]:
        variants.append(('binary_bias_'+str(bias),16,8,2,'binary','cutout',bias))
    manifest = []
    for name, fw, fh, columns, alpha, strategy, bias in variants:
        folder = dest / name; folder.mkdir(exist_ok=True)
        w, h = fw*columns, fh*((4+columns-1)//columns)
        cells = [pixels(fw, fh, i, alpha) for i in range(4)]
        sheet = bytearray(w*h*4)
        for i, cell in enumerate(cells):
            for y in range(fh):
                at = (((i//columns)*fh+y)*w+(i%columns)*fw)*4
                sheet[at:at+fw*4] = cell[y*fw*4:(y+1)*fw*4]
        meta = {'animation': {'width':fw,'height':fh,'frametime':10,'interpolate':False,'frames':[0,1,2,3]},
                'texture': {'blur':False}, 'nova_acceptance': {'fixture':'C'}}
        if strategy is not None: meta['texture']['mipmap_strategy'] = strategy
        if bias is not None: meta['texture']['alpha_cutoff_bias'] = bias
        path = folder / 'texture.png'
        if name == 'nova_original':
            shutil.copy2(source, path)
            shutil.copy2(source.with_suffix('.png.mcmeta'), path.with_suffix('.png.mcmeta'))
        else:
            path.write_bytes(encode_png(w,h,bytes(sheet)))
            write_json(path.with_suffix('.png.mcmeta'), meta)
        actual = inspect_png(path,fw,fh)
        assert actual['cell_rgba_sha256'] == [sha(c) for c in cells]
        if name == 'reference': assert meta == original_meta
        manifest.append({'name':name,'frame':[fw,fh],'columns':columns,'alpha':alpha,
                         'strategy':strategy or 'default(auto)','bias':bias or 0,
                         'png':actual,'metadata':meta,
                         'metadata_sha256':sha(path.with_suffix('.png.mcmeta').read_bytes()),
                         'runtime_status':'not run'})
    assert manifest[0]['png']['rgba_sha256'] == manifest[1]['png']['rgba_sha256']
    write_json(OUT / 'fixture-manifest.json', manifest)
    print(json.dumps({'variants':len(manifest),'original':manifest[0]['png'],'reference':manifest[1]['png']},indent=2))


def prepare():
    old = json.loads((OLD/'minecraft/instance.json').read_text())
    original = pathlib.Path(old['instance'])
    instance = WORK/'minecraft-1.21.11'
    assert not instance.exists()
    instance.mkdir(parents=True)
    shutil.copytree(original/'natives',instance/'natives')
    shutil.copytree(original/'saves',instance/'saves')
    shutil.copytree(original/'resourcepacks',instance/'resourcepacks')
    shutil.copy2(original/'options.txt',instance/'options.txt')
    shutil.copy2(original/'options.txt',WORK/'options.original.txt')
    args = [arg.replace(old['instance'],str(instance)) for arg in old['args']]
    write_json(OUT/'instance.json',{**old,'instance':str(instance),'args':args,
               'source_instance':str(original),'pack':str(instance/'resourcepacks/Nova-C4-AE'),
               'source':'historical isolated instance; new copy for all experiments'})
    print(str(instance))


def packs():
    """Uninstalled, deterministic packs for resumed controlled game trials."""
    manifest = json.loads((OUT/'fixture-manifest.json').read_text())
    records=[]
    for row in manifest:
        name=row['name']; source=OUT/'fixtures'/name
        content={'pack.mcmeta':(json.dumps({'pack':{'description':'C4 compatibility '+name,
                  'min_format':[75,0],'max_format':[75,0]}},indent=2)+'\n').encode()}
        # Vanilla blockstates/models provide known contexts; item/apple belongs to items atlas.
        for context in ['block/bricks','block/oak_leaves','block/white_stained_glass','item/apple']:
            for suffix in ['.png','.png.mcmeta']:
                content['assets/minecraft/textures/'+context+suffix]=(source/('texture'+suffix)).read_bytes()
        # Same square control in every pack; no rectangular control contaminates a square-only trial.
        for suffix in ['.png','.png.mcmeta']:
            content['assets/minecraft/textures/block/stone'+suffix]=(OUT/'fixtures/square'/('texture'+suffix)).read_bytes()
        archive=OUT/'packs'/('Nova-C4-'+name+'.zip'); archive.parent.mkdir(exist_ok=True)
        with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
            for path,data in sorted(content.items()):
                entry=zipfile.ZipInfo(path,(2026,10,30,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED
                z.writestr(entry,data)
        records.append({'variant':name,'archive':archive.relative_to(ROOT).as_posix(),
                        'sha256':sha(archive.read_bytes()),'installed':False,'runtime_status':'not run',
                        'files':[{'path':p,'bytes':len(d),'sha256':sha(d)} for p,d in sorted(content.items())]})
    write_json(OUT/'pack-manifest.json',records)
    print(json.dumps({'prepared_packs':len(records),'installed':False}))


def clientcheck():
    info=json.loads((OUT/'instance.json').read_text())
    cp=info['args'][info['args'].index('-cp')+1]
    java=pathlib.Path(info['java']); classes=WORK/'client-check';classes.mkdir(exist_ok=True)
    source=ROOT/'scripts/c4-compatibility-probe/ClientCheck.java'
    compiled=subprocess.run([str(java.with_name('javac.exe')),'-cp',cp,'-d',str(classes),str(source)],capture_output=True)
    (OUT/'client-check-build.txt').write_bytes(compiled.stdout+compiled.stderr)
    compiled.check_returncode()
    result=subprocess.run([str(java),'-Djava.library.path='+str(pathlib.Path(info['instance'])/'natives'),
                           '-cp',str(classes)+';'+cp,'c4.ClientCheck',str(OUT/'fixtures'),str(OUT/'client-check.json')],
                          cwd=classes,capture_output=True)
    (OUT/'client-check-stdout.txt').write_bytes(result.stdout)
    (OUT/'client-check-stderr.txt').write_bytes(result.stderr)
    result.check_returncode()
    rows=json.loads((OUT/'client-check.json').read_text())
    expected={r['name']:r for r in json.loads((OUT/'fixture-manifest.json').read_text())}
    for r in rows:
        e=expected[r['variant']]
        assert r['decoded']['rgba_sha256']==e['png']['rgba_sha256']
        assert r['frame']==e['frame']
        assert r['animation_codec_accepted'] and r['texture_codec_accepted']
    originals=[r for r in rows if r['variant'] in ['nova_original','reference']]
    assert originals[0]['cpu_mips']==originals[1]['cpu_mips']
    print(json.dumps({'official_client_codecs_and_decodes_verified':len(rows),'original_reference_cpu_mips_equal':True}))


def focused():
    build=ROOT/'build/studio-c4-acceptance-release'
    dest=OUT/'focused-release'; dest.mkdir(exist_ok=False)
    environment={**os.environ,'PATH':str(ROOT/'.tools/qt/bin')+os.pathsep+os.environ['PATH']}
    generated=(build/'tests/CTestTestfile.cmake').read_text()
    tests=dict(re.findall(r'add_test\(\[=\[(.*?)\]=\] "(.*?)"\)',generated))
    summary=[]
    for name in ['animation','animation-storage','animation-export','integrity']:
        exe=pathlib.Path(tests[name])
        result=subprocess.run([str(exe),'-o',str(dest/(name+'.txt'))+',txt','-o',str(dest/(name+'.xml'))+',xunitxml'],
                              env=environment,cwd=build/'tests',capture_output=True,timeout=180)
        (dest/(name+'-stdout.txt')).write_bytes(result.stdout+result.stderr)
        totals=re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted',(dest/(name+'.txt')).read_text())
        row={'suite':name,'exit':result.returncode,'binary_sha256':sha(exe.read_bytes()),
             **dict(zip(['passed','failed','skipped','blacklisted'],map(int,totals.groups())))}
        summary.append(row)
    write_json(dest/'summary.json',summary)
    assert all(r['exit']==r['failed']==r['skipped']==0 for r in summary)
    print(json.dumps(summary,indent=2))


def analysis():
    historical=json.loads((ROOT/'docs/phase-2c4/acceptance/official-spec-evidence.json').read_text())['versions'][0]
    official=ROOT/'.tools/minecraft-spec/1.21.11.jar'
    game_args=json.loads((OUT/'instance.json').read_text())['args']
    installed=pathlib.Path(game_args[game_args.index('-cp')+1].split(';')[-1])
    expected=historical['sha1']
    assert hashlib.sha1(official.read_bytes()).hexdigest()==hashlib.sha1(installed.read_bytes()).hexdigest()==expected
    assert hashlib.sha1((ROOT/'.tools/minecraft-spec/1.21.11-mappings.txt').read_bytes()).hexdigest()==historical['client_mappings_sha1']
    classes=['imz','inj','ilk','ilk$a','ilk$b','ill','ilo','ilp','ild','ile','fxe','fxl','fxm','fzb']
    resources=['assets/minecraft/shaders/core/animate_sprite.vsh',
               'assets/minecraft/shaders/core/animate_sprite_blit.fsh',
               'assets/minecraft/shaders/core/animate_sprite_interpolate.fsh',
               'assets/minecraft/atlases/blocks.json','assets/minecraft/atlases/items.json']
    with zipfile.ZipFile(official) as jar:
        hashes={name:sha(jar.read(name)) for name in [*(c+'.class' for c in classes),*resources]}
        contexts=[]
        for block in ['bricks','stone','oak_leaves','white_stained_glass']:
            path='assets/minecraft/blockstates/'+block+'.json'
            model='assets/minecraft/models/block/'+block+'.json'
            data=json.loads(jar.read(model))
            contexts.append({'block':block,'blockstate':path,'blockstate_sha256':sha(jar.read(path)),
                             'model':model,'model_sha256':sha(jar.read(model)),
                             'parent':data.get('parent'),'textures':data.get('textures'),
                             'atlas':'blocks','render_context':'held and placed; not observed in new variant trials'})
        contexts.append({'item':'apple','definition':'assets/minecraft/items/apple.json',
                         'definition_sha256':sha(jar.read('assets/minecraft/items/apple.json')),
                         'model':'assets/minecraft/models/item/apple.json','atlas':'items (no mipmaps)',
                         'model_sha256':sha(jar.read('assets/minecraft/models/item/apple.json')),
                         'parent':json.loads(jar.read('assets/minecraft/models/item/apple.json')).get('parent')})
        atlas_sources={name:[s for s in json.loads(jar.read('assets/minecraft/atlases/'+name+'.json'))['sources']
                             if s.get('type')=='minecraft:directory' and s.get('source') in ['block','item']]
                       for name in ['blocks','items']}
    write_json(OUT/'source-artifacts.json',{'version':'1.21.11','client_url':historical['client'],
        'client_sha1':expected,'metadata_url':historical['version_metadata'],
        'mappings_url':historical['client_mappings'],'mappings_sha1':historical['client_mappings_sha1'],
        'release_notes':'https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-11',
        'client_class_and_resource_sha256':hashes,'contexts':contexts,'atlas_sources':atlas_sources,
        'private_analysis':[{'path':str(WORK/p),'sha256':sha((WORK/p).read_bytes())} for p in ['client-bytecode.txt','backend-bytecode.txt']]})
    probe=json.loads((OUT/'backend-probe.json').read_text(encoding='utf-8-sig'))
    for row in probe['rows']:
        rectangular=row['frame'][0]!=row['frame'][1]
        black=rectangular and row['effective']==3 and row['extra_allocation']
        assert row['sampled_rgba']==([0,0,0,255] if black else [255,40,30,255])
        assert row['allocation_gl_error']==row['render_gl_error']==0
    manifest=json.loads((OUT/'fixture-manifest.json').read_text())
    client=json.loads((OUT/'client-check.json').read_text())
    assert len(manifest)==len(client)==26
    for row in client:
        fixture=next(f for f in manifest if f['name']==row['variant'])
        assert row['decoded']['rgba_sha256']==fixture['png']['rgba_sha256']
        assert row['frame']==fixture['frame']
    alpha=[]
    for row in client:
        if '_strategy_' in row['variant'] or '_bias_' in row['variant']:
            alpha.append({'variant':row['variant'],'strategy':row['strategy'],'bias':row['bias'],
                          'mip3_alpha_counts':row['cpu_mips'][3]['alpha_counts'],
                          'mip3_rgba_sha256':row['cpu_mips'][3]['rgba_sha256']})
    write_json(OUT/'analysis-summary.json',{'client_decoded_variants':26,'backend_probe_rows_verified':40,
        'backend_black_rows':sum(r['sampled_rgba']==[0,0,0,255] for r in probe['rows']),
        'alpha_cpu_results':alpha,'game_variant_render_trials':'unobserved after interruption',
        'production_code_changed':False,'acceptance':'Outcome C; backend mechanism strongly supported, required in-game comparisons incomplete'})
    print(json.dumps({'verified_official_client_sha1':expected,'codec_variants':26,'backend_rows':40}))


def snapshot():
    import datetime
    import psutil
    runtime=OUT/'runtime'
    options=WORK/'options.original.txt'
    shutil.copy2(options,runtime/'options-at-start.txt')
    selected={}
    for line in options.read_text().splitlines():
        if ':' in line:
            key,value=line.split(':',1)
            if key in ['mipmapLevels','textureFiltering','graphicsPreset','renderDistance','simulationDistance','resourcePacks']:
                selected[key]=value
    process=json.loads((runtime/'baseline/process.json').read_text())
    live={'pid':process['pid'],'query_only':True}
    try:
        proc=psutil.Process(process['pid'])
        live.update(running=proc.is_running(),exe=proc.exe(),created_unix=proc.create_time())
    except psutil.NoSuchProcess:
        live.update(running=False)
    write_json(runtime/'controls.json',{'snapshot_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'initial_options_file':selected,'options_sha256':sha(options.read_bytes()),
        'gpu':json.loads((WORK/'gpu.json').read_text(encoding='utf-8-sig')),
        'current_process_query':live,'original_instance_settings_untouched':True,
        'live_copied_instance_settings_not_overwritten_after_stop':True})
    write_json(runtime/'interaction-boundary.json',{
        'date_local':'2026-10-30','timezone':'America/New_York',
        'first_observation':'C held yellow in running demo world; screenshot saved',
        'last_completed_action':'Escape opened pause menu; C red backdrop screenshot saved',
        'interrupted_action':'Options click returned Computer Use stopped by physical Escape; action outcome unknown',
        'owner_continuation':'i confirm working textures in the demo world. continue with the rest of the task',
        'resume_query':'Read-only list_windows also returned the stopped status; no further computer use called',
        'owner_clarification':{'question':'Which mipmap level; did Fixture C animate held, placed or both?',
                               'answer':'i dont know, yes in both,','mipmap_level':'unknown','held':True,'placed':True},
        'alternate_input_or_process_control_used':False,
        'game_variant_trials_after_boundary':'not observed',
        'independent_offscreen_and_cpu_analysis_continued':True})
    captures=[]
    for path in sorted((runtime/'captures').glob('*.png')):
        captures.append(record(path))
    archive=runtime/'prior-root-latest.log.gz'
    write_json(runtime/'snapshot-manifest.json',{'screenshots':captures,
        'prior_root_log_archive':record(archive),'prior_root_log_decoded_sha256':sha(gzip.decompress(archive.read_bytes())),
        'log_snapshots':[record(runtime/'baseline'/p) for p in ['stdout-observed.txt','stderr-observed.txt']],
        'offscreen_executable_sha256':sha((WORK/'probe/c4-compatibility-probe.exe').read_bytes())})
    print(json.dumps(live,indent=2))


def verify():
    manifest=json.loads((OUT/'fixture-manifest.json').read_text())
    for item in manifest:
        path=OUT/'fixtures'/item['name']/'texture.png'
        assert json.loads(json.dumps(inspect_png(path,*item['frame'])))==item['png']
        assert sha(path.with_suffix('.png.mcmeta').read_bytes())==item['metadata_sha256']
        assert json.loads(path.with_suffix('.png.mcmeta').read_text())==item['metadata']
    packs=json.loads((OUT/'pack-manifest.json').read_text())
    for pack in packs:
        path=ROOT/pack['archive'];assert sha(path.read_bytes())==pack['sha256']
        with zipfile.ZipFile(path) as z:
            assert sorted(z.namelist())==sorted(p['path'] for p in pack['files'])
            for item in pack['files']:assert sha(z.read(item['path']))==item['sha256']
    links=[]
    for path in OUT.glob('*.md'):
        for match in re.finditer(r'\[[^\]]*\]\(([^)]+)\)',path.read_text(encoding='utf-8')):
            target=match.group(1)
            if target.startswith('https:'):continue
            assert (path.parent/target.split('#')[0]).exists(),(str(path),target)
            links.append(target)
    comparison=(OUT/'EXPORT_COMPARISON.md').read_text()
    for item in manifest[:2]:
        assert item['png']['sha256'] in comparison
        assert all(h in comparison for h in item['png']['cell_rgba_sha256'])
    difference=git('diff','c8350fc7bdb156abc93fe4ac217eb23da1ec8375','--','src','tests','CMakeLists.txt')
    assert not difference
    result={'fixture_pairs':len(manifest),'pack_archives':len(packs),'valid_local_document_links':len(links),
            'production_and_existing_tests_diff':'empty','original_reference_comparison_hashes_valid':True}
    staged=git('diff','--cached','--name-status').splitlines()
    if staged:
        for line in staged:
            action,path=line.split('\t',1)
            assert action=='A' and (path.startswith('docs/phase-2c4/compatibility-final/') or
                                   path.startswith('scripts/c4-compatibility-probe/') or path=='scripts/compatibility-c4-final.py'),line
        paths=[OUT/'fixtures'/item['name']/('texture'+suffix) for item in manifest for suffix in ['.png','.png.mcmeta']]
        paths.extend(ROOT/pack['archive'] for pack in packs)
        for path in paths:
            blob=subprocess.check_output(['git','show',':'+path.relative_to(ROOT).as_posix()],cwd=ROOT)
            assert blob==path.read_bytes(),str(path)
        result.update(staged_fixture_and_pack_files_byte_exact=len(paths),staged_changes_only_new_scoped_files=len(staged))
    write_json(OUT/'evidence-verification.json',result)
    print(json.dumps(result,indent=2))


def launch():
    info = json.loads((OUT/'instance.json').read_text())
    run = OUT/'runtime'/'baseline'
    run.mkdir(parents=True,exist_ok=False)
    command = [info['java'],*info['args']]
    with (run/'stdout.txt').open('wb') as stdout, (run/'stderr.txt').open('wb') as stderr:
        proc = subprocess.Popen(command,cwd=pathlib.Path(info['instance']),stdout=stdout,stderr=stderr)
        write_json(run/'process.json',{'pid':proc.pid,'started_unix':time.time(),'command':command})
        code = proc.wait()
    write_json(run/'exit.json',{'exit_code':code,'ended_unix':time.time()})


def preserve():
    entries = json.loads((OUT/'protected-at-start.json').read_text())
    missing, changed = [], []
    for entry in entries:
        p = ROOT/entry['path']
        if not p.is_file(): missing.append(entry['path']); continue
        if record(p) != entry: changed.append(entry['path'])
    inventories = {}
    for label in ['tracked','untracked','ignored']:
        p = OUT/(label+'-at-start.txt'+('.gz' if label=='ignored' else ''))
        paths = gzip.decompress(p.read_bytes()).decode().splitlines() if label=='ignored' else p.read_text().splitlines()
        inventories[label] = {'count':len(paths),'missing':[x for x in paths if not (ROOT/x).exists()]}
    result = {'protected_count':len(entries),'missing':missing,'changed':changed,'inventories':inventories}
    write_json(OUT/'preservation-final.json',result)
    assert not missing and not changed and not any(x['missing'] for x in inventories.values()), result
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('mode',choices=['baseline','fixtures','packs','prepare','launch','preserve','clientcheck','focused','analysis','snapshot','verify'])
    globals()[parser.parse_args().mode]()
