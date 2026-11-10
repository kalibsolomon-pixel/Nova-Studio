# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/sync-public-source.py on throwaway repositories (Phase 4-H.2).

Creates a development and a public repository in a temporary folder, never touches the real
ones and never uses the network (the public remote URL is only recorded, nothing is fetched or
pushed). Usage: python scripts/test-sync-public-source.py [--keep <folder>]
"""
import argparse
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SYNC = ROOT / 'scripts/sync-public-source.py'
sys.path.insert(0, str(ROOT / 'scripts'))
REMOTE = 'https://github.com/kalibsolomon-pixel/Nova-Studio.git'
IDENTITY = ['-c', 'user.name=Sync Test', '-c', 'user.email=sync-test@example.invalid', '-c', 'core.autocrlf=false']
results = []


def git(repo, *args, check=True):
    r = subprocess.run(['git', *IDENTITY, '-C', str(repo), *args], capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f'git {args}: {r.stderr}')
    return r.stdout.strip()


def sync(dev, pub, *args, env=None):
    r = subprocess.run([sys.executable, '-I', str(SYNC), '--dev', str(dev), '--public', str(pub), *args],
                       capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def expect(name, condition, detail=''):
    results.append(bool(condition))
    print(('PASS ' if condition else 'FAIL ') + name + ('' if condition else f'\n{detail}'), flush=True)


def write(repo, path, data):
    target = repo / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data if isinstance(data, bytes) else data.encode())


def required_files():
    import importlib.util
    spec = importlib.util.spec_from_file_location('sync', SYNC)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.REQUIRED


def make_repos(base):
    dev, pub = base / 'dev', base / 'pub'
    for repo in (dev, pub):
        repo.mkdir(parents=True)
        git(repo, 'init', '-q', '-b', 'main')
    for path in required_files():
        write(dev, path, f'stub {path}\n')
    shutil.copyfile(ROOT / 'LICENSE', dev / 'LICENSE')
    write(dev, 'src/app.cpp', 'int main() { return 0; }\n')
    write(dev, 'docs/phase-9/evidence.png', b'\x89PNG private screenshot')
    write(dev, 'docs/phase-9/REPORT.md', 'private evidence\n')
    write(dev, 'docs/phase-4i/REPORT.md', 'private evidence next to the published GCC build guide\n')
    write(dev, 'docs/release-review/README.md', 'private review index\n')
    git(dev, 'add', '-A')
    git(dev, 'commit', '-q', '-m', 'dev 1')
    write(pub, 'README.md', 'initial\n')
    git(pub, 'add', '-A')
    git(pub, 'commit', '-q', '-m', 'public 1')
    git(pub, 'remote', 'add', 'origin', REMOTE)
    return dev, pub


def commit(repo, message='change'):
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--keep', type=pathlib.Path, help='use this empty folder and keep it')
    a = p.parse_args()
    base = a.keep or pathlib.Path(tempfile.mkdtemp(prefix='nova-sync-test-'))
    try:
        dev, pub = make_repos(base)
        public_head = git(pub, 'rev-parse', 'HEAD')

        # 1. dry run by default: nothing written
        code, out = sync(dev, pub, '--allow-deletions')
        expect('dry run is the default and writes nothing',
               code == 0 and 'Dry run' in out and not git(pub, 'status', '--porcelain')
               and git(pub, 'rev-parse', 'HEAD') == public_head, out)
        expect('private evidence is excluded from the plan',
               'docs/phase-9' not in out and 'docs/release-review' not in out and 'A src/app.cpp' in out, out)
        expect('the GCC build guide is published from its evidence folder, nothing else there',
               'A docs/phase-4i/MINGW_BUILD_GUIDE.md' in out and 'docs/phase-4i/REPORT.md' not in out, out)

        # 2. apply stages but never commits or pushes
        code, out = sync(dev, pub, '--apply', '--allow-deletions')
        expect('--apply stages without committing', code == 0 and 'Staged' in out
               and git(pub, 'rev-parse', 'HEAD') == public_head and git(pub, 'diff', '--cached', '--name-only'), out)
        expect('--apply never pushes (no remote-tracking ref appears)',
               git(pub, 'for-each-ref', 'refs/remotes') == '', git(pub, 'for-each-ref'))
        expect('excluded evidence is not written', not (pub / 'docs/phase-9').exists())
        commit(pub, 'public 2')
        code, out = sync(dev, pub, '--check')
        expect('--check reports IN SYNC after the public commit', code == 0 and 'IN SYNC' in out, out)

        # 3. refusals; each change is reverted afterwards
        def refused(name, setup, needle, *args, env=None, undo=True):
            before = git(dev, 'rev-parse', 'HEAD')
            setup()
            code, out = sync(dev, pub, *args, env=env)
            expect(name, code == 2 and 'REFUSED' in out and needle in out, out)
            if undo:
                git(dev, 'reset', '-q', '--hard', before)

        def add(path, data):
            def go():
                write(dev, path, data)
                commit(dev)
            return go

        def accepted(name, setup):
            before = git(dev, 'rev-parse', 'HEAD')
            setup()
            code, out = sync(dev, pub)
            expect(name, code == 0 and 'REFUSED' not in out and 'Dry run' in out, out)
            git(dev, 'reset', '-q', '--hard', before)

        refused('a secret is refused', add('src/token.txt', 'token = ghp_' + 'A' * 36 + '\n'), 'possible secret')
        # Workspace folders come from the repositories at run time, so this file names none.
        refused('the development checkout folder is refused',
                add('scripts/build-mingw.ps1', f"$Source = '{dev}\\build\\x.tar.xz'\n"), 'local workspace folder')
        refused('the public checkout folder is refused (forward slashes)',
                add('src/path.txt', 'see ' + pub.as_posix() + '/README.md\n'), 'local workspace folder')
        worktree = base / 'second checkout'
        git(dev, 'worktree', 'add', '-q', '--detach', str(worktree))
        refused('another worktree of the development repository is refused',
                add('docs/release/BUILDING.md', f'cd {worktree}\n'), 'local workspace folder')
        toolchains = 'C:' + '\\Dev\\nova-toolchains\\'
        accepted('GCC sources with documented toolchain defaults are accepted (no false rejection)',
                 add('scripts/nova-mingw-toolchain.ps1', f"DefaultRoot = '{toolchains}msys64-20260927'\n"
                     f"DefaultQt = '{toolchains}qt-6.12.0-ucrt64-gcc16-r3'\n"
                     f"expect(\"{toolchains.replace(chr(92), '/')}qt-6.12.0-ucrt64-gcc16/bin/Qt6Core.dll\" ON)\n"))
        accepted('source folders named like build folders in another case are accepted',
                 add('src/testing/runtime.cpp', 'int runtime() { return 0; }\n'))
        for path, data in (('build/gcc/src/nova-studio.exe', b'MZ'), ('src/ui/window.obj', b'\x64\x86'),
                           ('scripts/__pycache__/sync.cpython-312.pyc', b'\x00'),
                           ('Testing/Temporary/CTestCostData.txt', 'x\n'), ('CMakeCache.txt', 'x\n'),
                           ('packaging/payload.zip', b'PK')):
            refused(f'a build output is refused: {path}', add(path, data), path)
        refused('a large file is refused, even at the pinned font path with other content',
                add('assets/fonts/MaterialSymbolsOutlined.ttf', b'\0' * (6 * 1024 * 1024)),
                'file over 5 MB: assets/fonts/MaterialSymbolsOutlined.ttf')
        env = dict(os.environ, USERPROFILE=r'C:\Users\synctestperson', USERNAME='synctestperson')
        refused("this machine's profile path is refused", add('src/p.txt', 'C:/Users/synctestperson/projects/x\n'),
                "user profile path or login", env=env)
        refused('an image outside the approved folders is refused', add('docs/release/shot.png', b'\x89PNG x'),
                'image outside the approved folders')
        refused('an image identical to private evidence is refused',
                add('src/evidence.png', b'\x89PNG private screenshot'), 'identical to excluded private evidence')

        def delete():
            (dev / 'src/app.cpp').unlink()
            commit(dev)
        refused('a deletion needs --allow-deletions', delete, 'deletion(s) need --allow-deletions')

        def drop_required():
            (dev / 'scripts/run-ctest.ps1').unlink()
            commit(dev)
        refused('a missing required file is refused', drop_required, 'required file missing: scripts/run-ctest.ps1',
                '--allow-deletions')

        def drop_gcc():
            (dev / 'packaging/mingw/qtbase-6.12.0-pcre2-jit-stack.patch').unlink()
            commit(dev)
        refused('a missing GCC corresponding-source file is refused', drop_gcc,
                'required file missing: packaging/mingw/qtbase-6.12.0-pcre2-jit-stack.patch', '--allow-deletions')
        refused('a modified GPL text is refused', add('LICENSE', 'not the GPL\n'), 'LICENSE is not the unmodified')

        def symlink_entry():
            blob = subprocess.run(['git', '-C', str(dev), 'hash-object', '-w', '--stdin'], input='../outside',
                                  capture_output=True, text=True).stdout.strip()
            git(dev, 'update-index', '--add', '--cacheinfo', f'120000,{blob},src/link')
            git(dev, 'commit', '-q', '-m', 'link')
        refused('a symbolic link entry is refused', symlink_entry, 'symbolic link entry: src/link')

        # writing through the public checkout's linked folder (like .tools -> development repository)
        outside = base / 'outside'
        outside.mkdir()
        made = subprocess.run(['cmd', '/c', 'mklink', '/J', str(pub / 'linked'), str(outside)], capture_output=True)
        if made.returncode == 0:
            with open(pub / '.git/info/exclude', 'a') as f:
                f.write('linked\n')
            refused('nothing is written through a link or junction', add('linked/x.txt', 'x\n'),
                    'would write through the link linked', '--apply')
            expect('the linked folder stayed untouched', not any(outside.iterdir()))
            os.rmdir(pub / 'linked')  # removes the junction only, never its target
        else:
            expect('junction created for the link test', False, made.stderr)

        def dirty_public():
            write(pub, 'stray.txt', 'x')
        refused('a dirty public tree is refused', dirty_public, 'public working tree is not clean', undo=False)
        (pub / 'stray.txt').unlink()

        def dev_remote():
            git(dev, 'remote', 'add', 'leak', 'https://example.invalid/x.git')
        refused('a development remote is refused', dev_remote, 'development repository has remotes', undo=False)
        git(dev, 'remote', 'remove', 'leak')

        def leak_history():
            git(pub, 'fetch', '-q', str(dev), 'main:refs/leak')
        refused('private history in the public object store is refused', leak_history,
                'public repository contains development commit', undo=False)

        code, out = sync(dev, pub, '--check')
        expect('the public repository was never committed to or pushed by the script',
               git(pub, 'rev-list', '--count', 'main') == '2' and git(pub, 'for-each-ref', 'refs/remotes') == '', out)
    finally:
        if not a.keep:
            subprocess.run(['cmd', '/c', 'rmdir', str(base / 'pub' / 'linked')], capture_output=True)

            def writable(function, path, _):
                os.chmod(path, stat.S_IWRITE)  # Git stores objects read-only on Windows
                function(path)
            shutil.rmtree(base, onexc=writable)
    print(f'{sum(results)}/{len(results)} sync-script tests passed')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
