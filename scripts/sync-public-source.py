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
"""
import argparse
import fnmatch
import hashlib
import pathlib
import re
import subprocess
import sys

DEV = pathlib.Path(__file__).resolve().parents[1]
PUBLIC_REMOTE = 'https://github.com/kalibsolomon-pixel/Nova-Studio.git'
# Private development evidence and developer-only files. Keep in step with
# docs/phase-4h1/SOURCE_SYNCHRONIZATION.md.
EXCLUDED = [
    'docs/phase-*',            # all phase evidence, including Minecraft client screenshots
    'docs/benchmarks',         # benchmark evidence
    'scripts/prepare-phase2c6-shadow-inspection.py',  # local user-profile path
    'scripts/write-phase3-report.py',                 # local user-profile path
    'logs', '.nova', '*.log',  # never tracked, listed for defence in depth
]
# Files the build, licence and corresponding-source obligations need. A sync that would drop
# any of them stops.
REQUIRED = ['CMakeLists.txt', 'LICENSE', 'COPYRIGHT.md', 'README.md', '.gitattributes',
            'packaging/THIRD_PARTY_NOTICES.md', 'packaging/CPackProjectConfig.cmake',
            'cmake/NovaPackaging.cmake', 'docs/release/BUILDING.md', 'src/CMakeLists.txt',
            'tests/CMakeLists.txt', 'assets/fabric-runtime/gradle-wrapper-9.8.2.jar',
            'scripts/build-studio.ps1', 'scripts/package-phase4g.py',
            'scripts/verify-phase4g1-licensing.py', 'scripts/install-qt.py',
            'scripts/prepare-phase3-qt.py', 'scripts/requirements-lock.txt']
GPL3_SHA256 = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
SECRET = re.compile(rb'(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|'
                    rb'sk-[A-Za-z0-9]{20,}|xox[abp]-[A-Za-z0-9-]{10,}|BEGIN [A-Z ]*PRIVATE KEY)')
# Generic patterns, so this file itself names nobody: a user-profile folder, a personal mail
# address, or a path under a drive's Dev folder (the local workspace).
PERSONAL = re.compile(rb'(?i)([a-z]:[/\\]{1,2}users[/\\]{1,2}[^/\\"\s]+[/\\]{1,2}'
                      rb'(appdata|desktop|documents|downloads|onedrive|\.codex|\.claude)|'
                      rb'[a-z0-9._%+-]+@(gmail|outlook|hotmail|yahoo|icloud)\.com|'
                      rb'[a-z]:[/\\]{1,2}dev[/\\])')
IMAGE = re.compile(r'(?i)\.(png|jpe?g|gif|bmp|webp)$')
IMAGE_DIRS = ('assets/branding/', 'fixtures/', 'tests/fixtures/', 'src/')


def git(repo, *args, data=False):
    r = subprocess.run(['git', '-C', str(repo), *args], capture_output=True)
    if r.returncode:
        raise SystemExit(f'git {" ".join(args)} failed in {repo}: {r.stderr.decode(errors="replace")}')
    return r.stdout if data else r.stdout.decode().strip()


def excluded(path):
    for pattern in EXCLUDED:
        if fnmatch.fnmatch(path, pattern) or fnmatch.fnmatch(path, pattern + '/*') \
                or any(fnmatch.fnmatch(part, pattern) for part in path.split('/')[:-1]):
            return True
    return False


def tree(repo, rev):
    out = {}
    for line in git(repo, 'ls-tree', '-r', '-z', rev, data=True).split(b'\0'):
        if not line:
            continue
        meta, path = line.split(b'\t', 1)
        mode, kind, blob = meta.decode().split()
        if kind == 'blob':
            out[path.decode()] = (mode, blob)
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--public', required=True, type=pathlib.Path)
    p.add_argument('--revision', default='HEAD', help='development revision to copy (committed only)')
    p.add_argument('--apply', action='store_true', help='write the files and stage them (no commit)')
    p.add_argument('--check', action='store_true', help='only report whether the trees already match')
    p.add_argument('--allow-deletions', action='store_true')
    p.add_argument('--public-revision', default='HEAD', help='with --check: public revision to compare')
    a = p.parse_args()
    pub = a.public.resolve()
    problems = []

    # Repository identity and safety
    if pub == DEV or DEV in pub.parents or pub in DEV.parents:
        raise SystemExit('The public repository must be a separate folder')
    if git(DEV, 'remote'):
        problems.append(f'development repository has remotes: {git(DEV, "remote")!r}')
    remotes = git(pub, 'remote', '-v').split('\n')
    if {r.split()[1] for r in remotes if r} != {PUBLIC_REMOTE}:
        problems.append(f'public remotes are not exactly {PUBLIC_REMOTE}: {remotes}')
    if (pub / '.git/objects/info/alternates').exists():
        problems.append('public repository uses alternates')
    if git(pub, 'branch', '--show-current') != 'main':
        problems.append('public repository is not on main')
    if not a.check and git(pub, 'status', '--porcelain', '--untracked-files=normal'):
        problems.append('public working tree is not clean')
    rev = git(DEV, 'rev-parse', '--verify', a.revision + '^{commit}')
    dirty = git(DEV, 'status', '--porcelain', '--untracked-files=no')
    if a.revision == 'HEAD' and dirty:
        print('note: development repository has uncommitted changes; only the commit is copied')

    source = {k: v for k, v in tree(DEV, rev).items() if not excluded(k)}
    if a.public_revision != 'HEAD' and not a.check:
        raise SystemExit('--public-revision is only for --check')
    target = tree(pub, a.public_revision)
    added = sorted(set(source) - set(target))
    deleted = sorted(set(target) - set(source))
    changed = sorted(k for k in set(source) & set(target) if source[k] != target[k])

    # Content checks on everything that would be published
    for path in sorted(set(added) | set(changed)):
        blob = git(DEV, 'cat-file', 'blob', source[path][1], data=True)
        if SECRET.search(blob):
            problems.append(f'possible secret in {path}')
        if PERSONAL.search(blob):
            problems.append(f'personal or local machine path in {path}')
        if IMAGE.search(path) and not path.startswith(IMAGE_DIRS):
            problems.append(f'image outside the approved folders: {path}')
        if len(blob) > 5 * 1024 * 1024:
            problems.append(f'file over 5 MB: {path}')
    for path in REQUIRED:
        if path not in source:
            problems.append(f'required file missing: {path}')
    if 'LICENSE' in source and hashlib.sha256(
            git(DEV, 'cat-file', 'blob', source['LICENSE'][1], data=True)).hexdigest() != GPL3_SHA256:
        problems.append('LICENSE is not the unmodified GPL-3.0 text')
    if deleted and not a.allow_deletions:
        problems.append(f'{len(deleted)} deletion(s) need --allow-deletions')

    print(f'development revision {rev}')
    print(f'public revision      {git(pub, "rev-parse", a.public_revision)}')
    print(f'files to copy {len(source)}; added {len(added)}, changed {len(changed)}, deleted {len(deleted)}')
    for label, items in (('A', added), ('M', changed), ('D', deleted)):
        for item in items:
            print(f'  {label} {item}')
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
        dest.write_bytes(git(DEV, 'cat-file', 'blob', source[path][1], data=True))
    git(pub, 'add', '-A')
    staged = tree(pub, git(pub, 'write-tree'))
    if staged != source:
        print('ERROR: staged tree does not equal the source tree (modes or line endings changed?)')
        return 3
    print('Staged. Review with `git -C <public> diff --cached`, then commit. Nothing was pushed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
