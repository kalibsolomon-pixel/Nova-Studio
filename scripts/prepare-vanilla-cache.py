"""Extract only authorized local official resources into the application cache.
No credentials, downloads or bundled artwork. Existing cache files are not replaced.
"""
import argparse, hashlib, json, os, pathlib, zipfile
p=argparse.ArgumentParser();p.add_argument('version',choices=['1.21.11','26.1','26.3']);p.add_argument('client',type=pathlib.Path);p.add_argument('--cache',type=pathlib.Path);a=p.parse_args()
hashes={'1.21.11':'ba2df812c2d12e0219c489c4cd9a5e1f0760f5bd','26.1':'191771837687b766537a8c4607cb6fad79c533a1','26.3':'e877b6a07acd633fb3bb475002175cec036e7b87'}
assert hashlib.sha1(a.client.read_bytes()).hexdigest()==hashes[a.version], 'Client does not match the verified official version'
out=a.cache or pathlib.Path(os.environ['LOCALAPPDATA'])/'Nova Studio'/'Nova Studio'/'vanilla'/a.version
assert not out.exists(), f'Cache already exists; choose a new output: {out}'
out.mkdir(parents=True);files={};total=0
with zipfile.ZipFile(a.client) as z:
 for entry in z.infolist():
  name=entry.filename;parts=pathlib.PurePosixPath(name).parts
  if len(parts)<4 or parts[:2]!=('assets','minecraft') or parts[2] not in ('models','items','blockstates','textures'):continue
  if '..' in parts or entry.is_dir() or not name.endswith(('.json','.png','.mcmeta')):continue
  assert entry.file_size<=16*1024*1024
  total+=entry.file_size;assert total<=512*1024*1024
  data=z.read(entry);dest=out.joinpath(*parts);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data);files[name]=hashlib.sha256(data).hexdigest()
(out/'nova-vanilla.json').write_text(json.dumps({'schema':1,'version':a.version,'client_sha1':hashes[a.version],'files':files},sort_keys=True))
print(json.dumps({'cache':str(out),'resources':len(files),'bytes':total,'client_sha1':hashes[a.version]}))
