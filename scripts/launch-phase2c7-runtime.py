"""Launch only the newly prepared owned C7 offline demo instance."""
import json,pathlib,subprocess,sys,hashlib,re
from datetime import datetime,timezone
root=pathlib.Path(__file__).resolve().parents[1];out=root/'docs/phase-2c7/runtime'
r=json.loads((out/'instance.json').read_text());instance=pathlib.Path(r['instance']).resolve();assert instance==(root/'build/studio-c7-runtime/minecraft-1.21.11').resolve()
args=r['args'];assert '--demo' in args and args[args.index('--accessToken')+1] in ('0','none','null','')
label=sys.argv[1] if len(sys.argv)>1 else ''
assert re.fullmatch('[a-z0-9-]*',label);suffix='-'+label if label else ''
assert pathlib.Path(args[args.index('--gameDir')+1]).resolve()==instance
pack=instance/'resourcepacks/Nova-C7'
(out/('pack-manifest'+suffix+'.json')).write_text(json.dumps([{'path':p.relative_to(pack).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(pack.rglob('*')) if p.is_file()],indent=2)+'\n')
with (out/('stdout'+suffix+'.txt')).open('x') as stdout,(out/('stderr'+suffix+'.txt')).open('x') as stderr:
 child=subprocess.Popen([r['java'],*args],cwd=instance,creationflags=subprocess.CREATE_NO_WINDOW,stdout=stdout,stderr=stderr)
 (out/('launch'+suffix+'.json')).write_text(json.dumps(dict(pid=child.pid,utc=datetime.now(timezone.utc).isoformat(),instance=str(instance)),indent=2)+'\n')
 print(child.pid,flush=True);code=child.wait()
 (out/('exit'+suffix+'.json')).write_text(json.dumps(dict(exit_code=code,utc=datetime.now(timezone.utc).isoformat()),indent=2)+'\n');print('exit',code)
