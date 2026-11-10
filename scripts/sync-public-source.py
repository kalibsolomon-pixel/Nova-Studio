# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy one committed development revision into the sanitized public repository.

Safe by default: without --apply it only prints the plan. It never commits, pushes, tags,
deletes history or touches .git metadata in either repository.

  python scripts/sync-public-source.py --public <public-checkout>            # dry run
  python scripts/sync-public-source.py --public <public-checkout> --check    # trees equal?
  python scripts/sync-public-source.py --public <public-checkout> --apply    # write + stage

The public tree must equal the development tree at --revision minus EXCLUDED. File bytes are
read from Git blobs, so line endings never change. Deletions stop the run unless
--allow-deletions is given. Content checks refuse secrets and personal paths.
Phase 4-H.2 adds: symbolic links and submodules are refused; nothing is written through a
link or junction (the public checkout's .tools folder links to the development repository) or
into .git; an image byte-identical to excluded private evidence is refused; the public object
store must not contain the development revision or root commit; the current user's profile path
and login are refused as well (derived at run time, so this file names nobody).
RC1 review: the checks are toolchain-agnostic (the MSVC and the GCC/MinGW build, packaging and
licence files are both required) and name no local folder. Local workspace paths are derived at run
time from the repositories themselves (every worktree of the development repository and the public
checkout), so documented toolchain defaults such as a drive's nova-toolchains folder are not
mistaken for personal data. Build outputs (executables, objects, caches, CMake/CTest folders) are
refused.
"""
import argparse
import fnmatch
import hashlib
import os
import pathlib
import re
import subprocess
import sys

DEV = pathlib.Path(__file__).resolve().parents[1]
PUBLIC_REMOTE = 'https://github.com/kalibsolomon-pixel/Nova-Studio.git'
# Private development evidence and developer-only files. Keep in step with
# docs/release/SOURCE_SYNC.md.
EXCLUDED = [
    'docs/phase-*',            # all phase evidence, including Minecraft client screenshots
    'docs/benchmarks',         # benchmark evidence
    'docs/release-review',     # owner-review index of private evidence folders
    'docs/vanilla-baseline',   # qualification report with local evidence locations
    'docs/mcp-opencode-glm',   # agent session evidence
    'scripts/prepare-phase2c6-shadow-inspection.py',  # local user-profile path
    'scripts/write-phase3-report.py',                 # local user-profile path
    'logs', '.nova', '*.log',  # never tracked, listed for defence in depth
]
# Published although inside an excluded folder: the GCC build guide is part of the corresponding
# source (scripts/package-mingw-experimental.py ships it in the source archive).
INCLUDED = ['docs/phase-4i/MINGW_BUILD_GUIDE.md']
# Files the build, licence and corresponding-source obligations need, for every supported
# toolchain. A sync that would drop any of them stops.
REQUIRED_COMMON = ['CMakeLists.txt', 'LICENSE', 'COPYRIGHT.md', 'README.md', '.gitattributes',
                   'packaging/CPackProjectConfig.cmake', 'packaging/nsis/NSIS.template.in',
                   'cmake/NovaPackaging.cmake', 'cmake/NovaDeployBridge.cmake', 'docs/release/BUILDING.md',
                   'docs/release/RELEASE_NOTES.md', 'src/CMakeLists.txt', 'tests/CMakeLists.txt',
                   'assets/fabric-runtime/gradle-wrapper-9.8.2.jar', 'scripts/nova-toolchain.ps1',
                   'scripts/check-release-payload.py', 'scripts/requirements.txt', 'scripts/requirements-lock.txt',
                   'scripts/requirements-mcp-sdk.txt', 'scripts/phase4i2-source-snapshot.py',
                   'scripts/sync-public-source.py']
REQUIRED_MSVC = ['packaging/THIRD_PARTY_NOTICES.md', 'packaging/MICROSOFT-VC-RUNTIME-TERMS.txt',
                 'packaging/README.txt.in', 'packaging/nsis/license-page.txt.in', 'cmake/NovaMsvcRuntime.cmake',
                 'scripts/build-studio.ps1', 'scripts/run-ctest.ps1', 'scripts/check-toolchain.ps1',
                 'scripts/package-phase4g.py', 'scripts/verify-phase4g1-licensing.py', 'scripts/install-qt.py',
                 'scripts/prepare-phase3-qt.py']
REQUIRED_GCC = ['packaging/mingw/THIRD_PARTY_NOTICES.md', 'packaging/mingw/README.txt.in',
                'packaging/mingw/license-page.txt.in', 'packaging/mingw/qtbase-6.12.0-pcre2-jit-stack.patch',
                'packaging/mingw/msys2-ucrt64-toolchain-lock.json', 'packaging/mingw/docs/BUILDING.md',
                'cmake/NovaPackagingMinGW.cmake', 'cmake/NovaMingwHelpers.cmake', 'docs/phase-4i/MINGW_BUILD_GUIDE.md',
                'scripts/nova-mingw-toolchain.ps1', 'scripts/install-mingw-toolchain.ps1',
                'scripts/build-qtbase-mingw.ps1', 'scripts/nova-qt-build-record.ps1', 'scripts/build-mingw.ps1',
                'scripts/check-mingw-toolchain.ps1', 'scripts/check-mingw-payload.py', 'scripts/msys2-resolve-lock.py',
                'scripts/package-mingw-experimental.py']
REQUIRED = REQUIRED_COMMON + REQUIRED_MSVC + REQUIRED_GCC
# Generated files that must come from a build, never from the source tree. The Gradle wrapper jar is
# a pinned upstream file that the Fabric runtime needs (COPYRIGHT.md, THIRD_PARTY_NOTICES.md).
# Folder names compare case-sensitively: CTest writes Testing/, while src/testing is Nova's source.
BUILD_OUTPUT_DIRS = {'build', 'out', 'CMakeFiles', 'Testing', '__pycache__', '.vs', '.cache', 'node_modules'}
BUILD_OUTPUT_NAMES = {'cmakecache.txt', 'build.ninja', '.ninja_log', '.ninja_deps', 'ctesttestfile.cmake',
                      'install_manifest.txt', 'compile_commands.json'}
BUILD_OUTPUT = re.compile(r'(?i)\.(exe|dll|obj|o|a|lib|pdb|ilk|exp|idb|pch|pyc|pyo|so|dylib|msi|msix|nupkg|'
                          r'zip|7z|zst|xz|gz|tgz|tar|jar|class|qm)$')
ALLOWED_BINARIES = {'assets/fabric-runtime/gradle-wrapper-9.8.2.jar'}
# Files over 5 MB are refused unless they are exactly this content (Git blob id), the unmodified
# Material Symbols font pinned in assets/fonts/README.md.
LARGE_ALLOWED = {'assets/fonts/MaterialSymbolsOutlined.ttf': '0a161642346ecf953a515ac43c9fe0f20ce3be2f'}
GPL3_SHA256 = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
SECRET = re.compile(rb'(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|'
                    rb'sk-[A-Za-z0-9]{20,}|xox[abp]-[A-Za-z0-9-]{10,}|BEGIN [A-Z ]*PRIVATE KEY)')
# Generic patterns, so this file itself names nobody: a user-profile folder or a personal mail
# address. Local workspace folders are matched by workspace_patterns(), not by a fixed drive path.
PERSONAL = re.compile(rb'(?i)([a-z]:[/\\]{1,2}users[/\\]{1,2}[^/\\"\s]+[/\\]{1,2}'
                      rb'(appdata|desktop|documents|downloads|onedrive|\.codex|\.claude)|'
                      rb'[a-z0-9._%+-]+@(gmail|outlook|hotmail|yahoo|icloud)\.com)')
IMAGE = re.compile(r'(?i)\.(png|jpe?g|gif|bmp|webp)$')
IMAGE_DIRS = ('assets/branding/', 'fixtures/', 'tests/fixtures/', 'src/')


def machine_patterns():
    """This machine's profile path and login as path segments, built at run time."""
    parts = []
    profile = os.environ.get('USERPROFILE') or os.environ.get('HOME') or ''
    components = [c for c in re.split(r'[/\\]+', profile) if c]
    if len(components) >= 2:
        parts.append(rb'[/\\]{1,2}'.join(re.escape(c.encode()) for c in components))
    login = os.environ.get('USERNAME') or os.environ.get('USER') or ''
    if len(login) >= 3:
        parts.append(rb'users[/\\]{1,2}' + re.escape(login.encode()) + rb'(?=[/\\"\s]|$)')
    return re.compile(rb'(?i)(' + rb'|'.join(parts) + rb')') if parts else None


def path_pattern(path):
    """A local folder written with either separator (doubled in source strings), any letter case."""
    components = [c for c in re.split(r'[/\\]+', str(path)) if c]
    return rb'[/\\]{1,2}'.join(re.escape(c.encode()) for c in components) + rb'(?=[/\\"\'`\s]|$)'


def workspace_patterns(dev, pub):
    """Every checkout of the development repository and the public checkout, read from Git now."""
    folders = {pub, dev, pathlib.Path(git(dev, 'rev-parse', '--path-format=absolute', '--git-common-dir')).parent}
    for line in git(dev, 'worktree', 'list', '--porcelain').splitlines():
        if line.startswith('worktree '):
            folders.add(pathlib.Path(line[len('worktree '):]))
    parts = sorted({path_pattern(f) for f in folders if len(f.parts) >= 2})
    return re.compile(rb'(?i)(' + rb'|'.join(parts) + rb')')


def build_output(path):
    """Why <path> looks like a build output rather than source, or None."""
    if path in ALLOWED_BINARIES:
        return None
    parts = path.split('/')
    if any(part in BUILD_OUTPUT_DIRS for part in parts[:-1]):
        return 'inside a build-output folder'
    if parts[-1].lower() in BUILD_OUTPUT_NAMES or BUILD_OUTPUT.search(path):
        return 'build output or binary archive'
    return None


def git(repo, *args, data=False, check=True):
    r = subprocess.run(['git', '-C', str(repo), *args], capture_output=True)
    if r.returncode and check:
        raise SystemExit(f'git {" ".join(args)} failed in {repo}: {r.stderr.decode(errors="replace")}')
    if not check:
        return r.returncode
    return r.stdout if data else r.stdout.decode().strip()


def excluded(path):
    if path in INCLUDED:
        return False
    for pattern in EXCLUDED:
        if fnmatch.fnmatch(path, pattern) or fnmatch.fnmatch(path, pattern + '/*') \
                or any(fnmatch.fnmatch(part, pattern) for part in path.split('/')[:-1]):
            return True
    return False


def tree(repo, rev):
    """{path: (mode, object)} for every entry, including links (120000) and submodules (160000)."""
    out = {}
    for line in git(repo, 'ls-tree', '-r', '-z', rev, data=True).split(b'\0'):
        if not line:
            continue
        meta, path = line.split(b'\t', 1)
        mode, kind, blob = meta.decode().split()
        out[path.decode()] = (mode, blob)
    return out


def unsafe_destination(pub, path):
    """Why writing or deleting <pub>/<path> is unsafe, or None."""
    parts = path.split('/')
    if any(p in ('', '.', '..') for p in parts) or any(p.lower() == '.git' for p in parts):
        return 'path with ., .. or .git component'
    current = pub
    for part in parts[:-1]:
        current = current / part
        if current.is_symlink() or (hasattr(current, 'is_junction') and current.is_junction()):
            return f'would write through the link {current.relative_to(pub).as_posix()}'
    target = pub / path
    if target.is_symlink():
        return 'target is a link'
    try:
        target.resolve().relative_to(pub)
    except ValueError:
        return 'resolves outside the public checkout'
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--public', required=True, type=pathlib.Path)
    p.add_argument('--dev', type=pathlib.Path, default=DEV, help=argparse.SUPPRESS)  # for the script's own tests
    p.add_argument('--revision', default='HEAD', help='development revision to copy (committed only)')
    p.add_argument('--apply', action='store_true', help='write the files and stage them (no commit)')
    p.add_argument('--check', action='store_true', help='only report whether the trees already match')
    p.add_argument('--allow-deletions', action='store_true')
    p.add_argument('--public-revision', default='HEAD', help='with --check: public revision to compare')
    a = p.parse_args()
    dev = a.dev.resolve()
    pub = a.public.resolve()
    problems, notes = [], []

    # Repository identity and safety
    if pub == dev or dev in pub.parents or pub in dev.parents:
        raise SystemExit('The public repository must be a separate folder')
    if a.apply and a.check:
        raise SystemExit('--apply and --check exclude each other')
    if git(dev, 'remote'):
        problems.append(f'development repository has remotes: {git(dev, "remote")!r}')
    remotes = git(pub, 'remote', '-v').split('\n')
    if {r.split()[1] for r in remotes if r} != {PUBLIC_REMOTE}:
        problems.append(f'public remotes are not exactly {PUBLIC_REMOTE}: {remotes}')
    if (pub / '.git/objects/info/alternates').exists():
        problems.append('public repository uses alternates')
    if git(pub, 'branch', '--show-current') != 'main':
        problems.append('public repository is not on main')
    if not a.check and git(pub, 'status', '--porcelain', '--untracked-files=normal'):
        problems.append('public working tree is not clean')
    rev = git(dev, 'rev-parse', '--verify', a.revision + '^{commit}')
    dirty = git(dev, 'status', '--porcelain', '--untracked-files=no')
    if a.revision == 'HEAD' and dirty:
        print('note: development repository has uncommitted changes; only the commit is copied')
    # Private history must never reach the public object store.
    roots = git(dev, 'rev-list', '--max-parents=0', rev).split()
    for commit in [rev, *roots]:
        if git(pub, 'cat-file', '-e', commit + '^{commit}', check=False) == 0:
            problems.append(f'public repository contains development commit {commit[:12]} (private history)')

    full = tree(dev, rev)
    source = {k: v for k, v in full.items() if not excluded(k)}
    evidence_images = {v[1] for k, v in full.items() if excluded(k) and IMAGE.search(k)}
    if a.public_revision != 'HEAD' and not a.check:
        raise SystemExit('--public-revision is only for --check')
    target = tree(pub, a.public_revision)
    added = sorted(set(source) - set(target))
    deleted = sorted(set(target) - set(source))
    changed = sorted(k for k in set(source) & set(target) if source[k] != target[k])

    # Content checks on everything that would be published
    machine = machine_patterns()
    workspace = workspace_patterns(dev, pub)
    for path in sorted(set(added) | set(changed)):
        mode, obj = source[path]
        if mode == '160000':
            problems.append(f'submodule entry: {path}')
            continue
        if mode == '120000':
            problems.append(f'symbolic link entry: {path}')
            continue
        blob = git(dev, 'cat-file', 'blob', obj, data=True)
        if SECRET.search(blob):
            problems.append(f'possible secret in {path}')
        if PERSONAL.search(blob):
            problems.append(f'personal or local machine path in {path}')
        if machine and machine.search(blob):
            problems.append(f"this machine's user profile path or login in {path}")
        if workspace.search(blob):
            problems.append(f'local workspace folder in {path}')
        reason = build_output(path)
        if reason:
            problems.append(f'{reason}: {path}')
        if IMAGE.search(path) and not path.startswith(IMAGE_DIRS):
            problems.append(f'image outside the approved folders: {path}')
        if IMAGE.search(path) and obj in evidence_images:
            problems.append(f'image identical to excluded private evidence: {path}')
        if len(blob) > 5 * 1024 * 1024 and LARGE_ALLOWED.get(path) != obj:
            problems.append(f'file over 5 MB: {path}')
    for path in REQUIRED:
        if path not in source:
            (notes if a.check else problems).append(f'required file missing: {path}')
    if 'LICENSE' in source and hashlib.sha256(
            git(dev, 'cat-file', 'blob', source['LICENSE'][1], data=True)).hexdigest() != GPL3_SHA256:
        problems.append('LICENSE is not the unmodified GPL-3.0 text')
    if deleted and not a.allow_deletions:
        problems.append(f'{len(deleted)} deletion(s) need --allow-deletions')
    if a.apply:
        for path in added + changed + deleted:
            reason = unsafe_destination(pub, path)
            if reason:
                problems.append(f'unsafe destination {path}: {reason}')

    print(f'development revision {rev}')
    print(f'public revision      {git(pub, "rev-parse", a.public_revision)}')
    print(f'files to copy {len(source)}; added {len(added)}, changed {len(changed)}, deleted {len(deleted)}')
    for label, items in (('A', added), ('M', changed), ('D', deleted)):
        for item in items:
            print(f'  {label} {item}')
    for item in notes:
        print('note: ' + item)
    if problems:
        print('REFUSED:')
        for item in problems:
            print('  - ' + item)
        return 2
    if not (added or changed or deleted):
        print('IN SYNC: the public tree equals the development tree minus the exclusions')
        return 0
    if a.check:
        print('NOT IN SYNC')
        return 1
    if not a.apply:
        print('Dry run. Re-run with --apply to write and stage these files.')
        return 0
    for path in deleted:
        (pub / path).unlink()
    for path in added + changed:
        dest = pub / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(git(dev, 'cat-file', 'blob', source[path][1], data=True))
    git(pub, 'add', '-A')
    staged = tree(pub, git(pub, 'write-tree'))
    if staged != source:
        print('ERROR: staged tree does not equal the source tree (modes or line endings changed?)')
        return 3
    print('Staged. Review with `git -C <public> diff --cached`, then commit. Nothing was pushed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
