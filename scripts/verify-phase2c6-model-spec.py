"""Record official artifact identities and independent observations; never copy artwork/code."""
import hashlib,json,pathlib,re,zipfile
root=pathlib.Path(__file__).resolve().parents[1];prior=json.loads((root/'docs/phase-2c4/official-spec-evidence.json').read_text());rows=[]
for record in prior['versions']:
 version=record['id'];path=root/'.tools/minecraft-spec'/f'{version}.jar';assert hashlib.sha1(path.read_bytes()).hexdigest()==record['sha1']
 with zipfile.ZipFile(path) as z:
  files={n:hashlib.sha256(z.read(n)).hexdigest() for n in z.namelist() if n.startswith('assets/minecraft/') and n.split('/')[2] in ('models','items','blockstates','textures') and n.endswith(('.json','.png','.mcmeta'))}
  row={'version':version,'official_client':record['client'],'verified_client_sha1':record['sha1'],'resources':len(files),'resource_manifest_sha256':hashlib.sha256(json.dumps(files,sort_keys=True,separators=(',',':')).encode()).hexdigest(),'class_identities':{}}
  for name in ('net.minecraft.client.resources.model.cuboid.ItemModelGenerator','net.minecraft.util.ExtraCodecs','com.mojang.math.Transformation','net.minecraft.client.color.item.Constant','net.minecraft.client.color.item.GrassColorSource'):
   target=name
   if version=='1.21.11':
    mapping=(root/'.tools/minecraft-spec/1.21.11-mappings.txt').read_text();match=re.search('^'+re.escape(name)+r' -> (.*):$',mapping,re.M);target=match[1] if match else name
   entry=target.replace('.','/')+'.class'
   if entry in z.namelist():row['class_identities'][name]=hashlib.sha256(z.read(entry)).hexdigest()
  rows.append(row)
(root/'docs/phase-2c6/official-model-evidence.json').write_text(json.dumps({'checked':'2026-10-30','versions':rows,'observations':['26.1+ subtree transformations compose after display contexts','Matrix serialization uses row/column order; quaternion components x,y,z,w','Generated geometry derives alpha boundary spans','26.3 shade override replaces shade and map_color tint is removed'],'boundary':'Artifact/codec inspection and Nova tests; no in-game certification inferred'},indent=2)+'\n')
print('Verified',len(rows),'official clients; no client artwork or bytecode distributed')
