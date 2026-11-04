"""Read saved native manifests after GUI actions; class-A inspection, never UI input."""
import argparse, hashlib, json, pathlib, struct
parser = argparse.ArgumentParser()
parser.add_argument('native', type=pathlib.Path)
parser.add_argument('output', type=pathlib.Path)
args = parser.parse_args()
payload = args.native.read_bytes()
assert payload[:8] in (b'NVAN0002', b'NVLD0001')
length = struct.unpack_from('<I', payload, 8)[0]
manifest = json.loads(payload[12:12+length])
result = {'path': str(args.native.resolve()), 'sha256': hashlib.sha256(payload).hexdigest(), 'manifest': manifest}
if payload[:8] == b'NVAN0002':
    offset = 12+length; frames = []
    for descriptor in manifest['frames']:
        size = struct.unpack_from('<I', payload, offset)[0]; offset += 4
        frame = payload[offset:offset+size]; offset += size
        assert hashlib.sha256(frame).hexdigest() == descriptor['sha256']
        inner_length = struct.unpack_from('<I', frame, 8)[0]
        frames.append({'id': descriptor['id'], 'name': descriptor['name'], 'manifest': json.loads(frame[12:12+inner_length])})
    assert offset == len(payload)
    result['source_frames'] = frames
    ids = [f['id'] for f in frames]
    result['playback_indices'] = [ids.index(e['frame']) for e in manifest['entries']]
    result['active_source'] = ids.index(manifest['active'])
args.output.parent.mkdir(parents=True, exist_ok=True)
assert not args.output.exists()
args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
