"""Create or verify a local qualification tree without staging or committing changes."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATHS = ['.clang-format', '.gitattributes', '.gitignore', 'CMakeLists.txt', 'LICENSE', 'COPYRIGHT.md',
         'LICENSE-DECISION.md', 'README.md', 'src', 'tests', 'cmake', 'packaging', 'assets/branding',
         'assets/fabric-runtime', 'assets/fonts', 'fixtures', 'docs/release', 'docs/phase-4i/MINGW_BUILD_GUIDE.md', 'scripts']


def git(*arguments, env=None):
    return subprocess.check_output(['git', *arguments], cwd=ROOT, env=env)


def verify(tree):
    expected = {}
    for entry in git('ls-tree', '-rz', tree, '--', *PATHS).split(b'\0'):
        if not entry:
            continue
        metadata, name = entry.split(b'\t', 1)
        mode, kind, digest = metadata.decode().split()
        if kind != 'blob' or mode not in ('100644', '100755'):
            raise SystemExit(f'Unsupported snapshot entry: {name!r}')
        expected[name.decode('utf-8')] = digest
    actual_names = set(git('ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', *PATHS)
                       .decode('utf-8').rstrip('\0').split('\0'))
    differences = []
    for name, digest in expected.items():
        path = ROOT / name
        if not path.is_file() or path.is_symlink():
            differences.append(name)
            continue
        data = path.read_bytes()
        actual = hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()
        if actual != digest:
            differences.append(name)
    differences.extend(sorted(actual_names - set(expected)))
    if differences:
        raise SystemExit('Snapshot mismatch: ' + ', '.join(differences))
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify')
    parser.add_argument('--report', type=pathlib.Path)
    arguments = parser.parse_args()
    if arguments.verify:
        tree = arguments.verify
    else:
        temporary = ROOT / 'build/phase4i2-evidence'
        temporary.mkdir(parents=True, exist_ok=True)
        descriptor, index = tempfile.mkstemp(prefix='source-index-', dir=temporary)
        os.close(descriptor)
        pathlib.Path(index).unlink()
        environment = dict(os.environ, GIT_INDEX_FILE=index)
        try:
            git('read-tree', 'HEAD', env=environment)
            # Include tracked removals outside PATHS (for example archived private evidence),
            # so the identity describes the release tree rather than retaining deleted files.
            git('-c', 'core.autocrlf=false', 'add', '-u', '--', env=environment)
            git('-c', 'core.autocrlf=false', 'add', '--', *PATHS, env=environment)
            tree = git('write-tree', env=environment).decode().strip()
        finally:
            pathlib.Path(index).unlink(missing_ok=True)
    expected = verify(tree)
    report = {'head': git('rev-parse', 'HEAD').decode().strip(), 'source_tree': tree,
              'files': expected, 'verified': True, 'creates_commit': False}
    if arguments.report:
        arguments.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'source_tree': tree, 'files': len(expected), 'verified': True}))


if __name__ == '__main__':
    main()
