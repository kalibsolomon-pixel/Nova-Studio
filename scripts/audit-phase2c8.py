"""Preserve a starting inventory; verify presence and protected hashes at delivery."""
import gzip, hashlib, json, pathlib, subprocess, sys
from datetime import datetime, timezone
root = pathlib.Path(__file__).resolve().parents[1]
out = root / 'docs/phase-2c8'
def git(*args):
    return subprocess.check_output(['git', *args], cwd=root).decode('utf-8').strip()
def digest(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()
if '--verify' in sys.argv:
    data = json.loads((out/'starting-audit.json').read_text())
    failures = [p for p,h in data['protected_hashes'].items() if not (root/p).is_file() or digest(root/p) != h]
    missing = []
    for kind in ('tracked','untracked','ignored'):
        with gzip.open(out/(kind+'-at-start.txt.gz'), 'rt', encoding='utf-8') as f:
            missing += [p for p in f.read().splitlines() if not (root/p).exists()]
    transient=[p for p in missing if p.startswith(('build/studio-c8-baseline-release/driver/Testing/Temporary/','build/studio-c8-baseline-debug/driver/Testing/Temporary/')) and (pathlib.Path(p).name=='CTestCheckpoint.txt' or pathlib.Path(p).name.startswith('LastTest.log.tmp'))]
    missing=[p for p in missing if p not in transient]
    result = dict(head=git('rev-parse','HEAD'), branch=git('branch','--show-current'),
                  status=git('status','--short'), protected_hash_mismatches=failures,
                  missing_paths=missing, completed_C8_baseline_temporary_paths=transient,
                  deleted=git('diff','--diff-filter=D','--name-only',data['head']).splitlines())
    (root/'build/studio-c8-preservation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2)); assert not failures and not missing and not result['deleted']
else:
    out.mkdir(parents=True, exist_ok=True)
    assert not (out/'starting-audit.json').exists()
    data = dict(utc=datetime.now(timezone.utc).isoformat(),head=git('rev-parse','HEAD'),
                branch=git('branch','--show-current'), status=git('status','--short'),counts={},protected_hashes={})
    for kind,args in [('tracked',('ls-files',)),('untracked',('ls-files','--others','--exclude-standard')),
                      ('ignored',('ls-files','--others','--ignored','--exclude-standard'))]:
        files = git(*args).splitlines(); data['counts'][kind]=len(files)
        with gzip.open(out/(kind+'-at-start.txt.gz'),'wt',encoding='utf-8') as f: f.write('\n'.join(files)+'\n')
        for p in files:
            if p.startswith(('docs/','fixtures/','prototypes/','logs/')) or pathlib.Path(p).suffix in ('.nvdoc','.ini','.log') or 'recovery' in p.lower():
                full=root/p
                if full.is_file(): data['protected_hashes'][p]=digest(full)
    (out/'starting-audit.json').write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps({k:v for k,v in data.items() if k!='protected_hashes'},indent=2))
