"""Build with the installed MSVC/Qt/Ninja toolchain; retain all build directories."""
import argparse
import os
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('--configuration', choices=['Release', 'Debug'], default='Release')
p.add_argument('--targets', nargs='+')
p.add_argument('--probe-only', help='Recompile one test probe against already frozen baseline archives')
p.add_argument('--shutdown-probe', action='store_true', help='Build the retained standalone shutdown probe against this build')
a = p.parse_args()
if not all(c.isalnum() or c in '-_' for c in a.name):
    raise SystemExit('Build name must be a simple directory name')
vswhere = pathlib.Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = pathlib.Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property',
                                         'installationPath'], text=True).strip())
vcvars = vs / 'VC/Auxiliary/Build/vcvars64.bat'
environment = dict(os.environ)
for line in subprocess.check_output(f'cmd /d /s /c ""{vcvars}" >nul && set"', text=True).splitlines():
    key, sep, value = line.partition('=')
    if key and sep:
        environment[key] = value
cmake = vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
ninja = vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
build = root / 'build' / a.name
source = root
extra = []
if a.shutdown_probe:
    if a.probe_only: raise SystemExit('Choose only one probe build option')
    if not (build / 'src/nova-workspace.lib').is_file(): raise SystemExit('Build production libraries first')
    extra = ['-DNOVA_BUILD=' + str(build)]
    source = root / 'scripts/shutdown-probe'
    build = build.with_name(a.name + '-shutdown')
if a.probe_only:
    if not a.probe_only.startswith('nova-') or not all(c.isalnum() or c == '-' for c in a.probe_only):
        raise SystemExit('Invalid probe target')
    cache = (build / 'CMakeCache.txt').read_text()
    import re
    source = pathlib.Path(re.search(r'CMAKE_HOME_DIRECTORY:INTERNAL=(.*)', cache)[1])
    if source.resolve() != root.resolve(): raise SystemExit('Unexpected build source')
    production = list((build / 'src').glob('*.lib'))
    import hashlib
    before = {str(f): hashlib.file_digest(f.open('rb'), 'sha256').hexdigest() for f in production}
    subprocess.run([str(cmake), '-E', 'cmake_autogen',
                    str(build / 'tests/CMakeFiles' / (a.probe_only + '_autogen.dir') / 'AutogenInfo.json'),
                    a.configuration], cwd=build, env=environment, check=True)
    # Source filenames and the link command come from the existing Ninja graph.
    commands = subprocess.check_output([str(ninja), '-t', 'commands', a.probe_only], cwd=build, text=True).splitlines()
    compiles = [c for c in commands if f'tests/CMakeFiles/{a.probe_only}.dir/' in c.replace('\\', '/')
                and '.cpp.obj' in c and (' /c ' in c or ' -c ' in c) and 'mocs_compilation.cpp' not in c]
    if len(compiles) != 1: raise SystemExit(f'Expected one probe translation unit, found {len(compiles)}')
    for command in (compiles[0], commands[-1]):
        subprocess.run(command, cwd=build, env=environment, shell=True, check=True)
    assert before == {str(f): hashlib.file_digest(f.open('rb'), 'sha256').hexdigest() for f in production}
    raise SystemExit(0)
subprocess.run([str(cmake), '-S', str(source), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_MAKE_PROGRAM=' + str(ninja), '-DCMAKE_PREFIX_PATH=' + str(root / '.tools/qt'),
                '-DCMAKE_BUILD_TYPE=' + a.configuration, *extra], env=environment, check=True)
command = [str(cmake), '--build', str(build), '--parallel', '4']
if a.targets:
    command += ['--target', *a.targets]
subprocess.run(command, env=environment, check=True)
