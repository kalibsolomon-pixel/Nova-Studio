"""Check official cached client identities and inspect model codec classes, without distributing game code."""
import hashlib
import json
import pathlib
import re
import subprocess
import zipfile
root = pathlib.Path(__file__).resolve().parents[1]
previous = json.loads((root/'docs/phase-2c4/official-spec-evidence.json').read_text())
javap = pathlib.Path(r'C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot\bin\javap.exe')
classes = ['net.minecraft.client.renderer.block.model.BlockElement',
           'net.minecraft.client.renderer.block.model.BlockElementRotation',
           'net.minecraft.client.renderer.block.model.BlockElementFace',
           'net.minecraft.client.renderer.block.model.BlockFaceUV',
           'net.minecraft.client.renderer.block.model.ItemTransform',
           'net.minecraft.client.renderer.FaceInfo',
           'net.minecraft.client.renderer.item.ItemModel']
classes += ['net.minecraft.client.resources.model.cuboid.'+name for name in
            ('CuboidModelElement','CuboidModelElement$Deserializer','CuboidRotation','CuboidRotation$EulerXYZRotation',
             'CuboidRotation$SingleAxisRotation','CuboidFace','CuboidFace$Deserializer','CuboidFace$UVs',
             'ItemTransform','ItemTransform$Deserializer','FaceBakery')]
mapping = (root/'.tools/minecraft-spec/1.21.11-mappings.txt').read_text()
rows=[]
for record in previous['versions']:
    version=record['id']; jar=root/f'.tools/minecraft-spec/{version}.jar'
    assert hashlib.sha1(jar.read_bytes()).hexdigest()==record['sha1']
    row={'version':version,'official_client':record['client'],'verified_sha1':record['sha1'],'classes':[]}
    with zipfile.ZipFile(jar) as archive:
        for name in classes:
            target=name
            if version=='1.21.11':
                found=re.search('^'+re.escape(name)+r' -> (.*):$',mapping,re.M)
                if found: target=found[1]
            path=target.replace('.','/')+'.class'
            if path not in archive.namelist(): continue
            data=archive.read(path)
            fields=[s for s in ('origin','axis','angle','rescale','rotation','uv','shade','shade_direction_override','translation','scale','from','to') if s.encode() in data]
            item={'name':name,'sha256':hashlib.sha256(data).hexdigest(),'field_strings':fields}
            if javap.exists():
                result=subprocess.run([str(javap),'-classpath',str(jar),'-c','-p',target],capture_output=True,text=True)
                item['javap_exit']=result.returncode
                item['bytecode_sha256']=hashlib.sha256(result.stdout.encode()).hexdigest()
                # Kept locally, ignored; only identities/observations are committed.
                (root/'.tools/minecraft-spec'/f'{version}-{name.rsplit(".",1)[-1]}-javap.txt').write_text(result.stdout)
            row['classes'].append(item)
        row['counts']={kind:sum(n.startswith(f'assets/minecraft/{kind}/') and n.endswith('.json') for n in archive.namelist()) for kind in ('models','items','blockstates','atlases')}
    rows.append(row)
output=root/'docs/phase-2c5/official-model-evidence.json'
output.write_text(json.dumps({'checked':'2026-10-30','versions':rows,'notes':'No official client assets or bytecode are distributed. Preview compatibility is independently scoped.'},indent=2)+'\n')
print(output)
