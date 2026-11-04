"""Read-only capture of native saves while the owner supplies real pan gestures."""
import argparse, datetime, hashlib, json, pathlib, struct, time

parser = argparse.ArgumentParser()
parser.add_argument('native', type=pathlib.Path)
parser.add_argument('--seconds', type=float, default=3600)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
case = root / 'docs/phase-2c4/acceptance-final/processes/B-pan-human'
output = case / 'native-saves'
output.mkdir(exist_ok=False)
rows = []
last_hash = None
started = time.monotonic()
record = {'purpose': 'Read-only save capture; no UI input', 'source': str(args.native),
          'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'poll_seconds': 0.1, 'saves': rows, 'status': 'watching'}

def valid(data):
    if data[:8] != b'NVAN0002' or len(data) < 12:
        return False
    length = struct.unpack_from('<I', data, 8)[0]
    manifest = json.loads(data[12:12+length])
    offset = 12 + length
    for descriptor in manifest['frames']:
        size = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        frame = data[offset:offset+size]
        offset += size
        if hashlib.sha256(frame).hexdigest() != descriptor['sha256']:
            return False
    return offset == len(data)

def persist():
    temporary = output / 'watch.json.partial'
    temporary.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    deadline = time.monotonic()+5
    while True:
        try:
            temporary.replace(output / 'watch.json')
            break
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)

while time.monotonic()-started < args.seconds:
    try:
        data = args.native.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != last_hash and valid(data):
            destination = output / f'{len(rows):03d}.nvdoc'
            destination.write_bytes(data)
            rows.append({'sequence': len(rows), 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                         'path': str(destination), 'sha256': digest, 'bytes': len(data)})
            last_hash = digest
            persist()
        supervisor = json.loads((case / 'supervisor.json').read_text())
        recovery = case / 'exit-recovery.json'
        recovered_exit = json.loads(recovery.read_text()).get('exit_utc') if recovery.exists() else None
        if supervisor.get('exit_utc') or recovered_exit:
            record['status'] = 'application exited'
            break
    except (OSError, ValueError, KeyError, struct.error):
        pass  # Retry an atomic replacement or transient sharing conflict; send no input.
    time.sleep(0.1)
else:
    record['status'] = 'capture time limit reached; application untouched'
record['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
persist()
