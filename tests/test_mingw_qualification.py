import importlib.util
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class QualificationTests(unittest.TestCase):
    def test_system32_is_not_a_third_party_dependency_escape(self):
        checker = load('mingw_payload', 'check-mingw-payload.py')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/phase4i2-evidence') as folder:
            system = pathlib.Path(folder).resolve()
            self.assertTrue(system.is_relative_to(ROOT / 'build/phase4i2-evidence'))
            for name in ('KERNEL32.dll', 'vcruntime140.dll', 'thirdparty.dll'):
                (system / name).touch()
            self.assertTrue(checker.is_windows_system_import('KERNEL32.dll', system))
            self.assertFalse(checker.is_windows_system_import('vcruntime140.dll', system))
            self.assertFalse(checker.is_windows_system_import('thirdparty.dll', system))

    def test_snapshot_detects_changed_deleted_and_extra_source(self):
        snapshot = load('source_snapshot', 'phase4i2-source-snapshot.py')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/phase4i2-evidence') as folder:
            snapshot.ROOT = pathlib.Path(folder).resolve()
            self.assertTrue(snapshot.ROOT.is_relative_to(ROOT / 'build/phase4i2-evidence'))
            snapshot.PATHS = ['src']
            subprocess.run(['git', 'init', '-q', folder], check=True)
            (snapshot.ROOT / 'src').mkdir()
            original = snapshot.ROOT / 'src/example.cpp'
            original.write_bytes(b'int value = 1;\n')
            snapshot.git('add', 'src')
            tree = snapshot.git('write-tree').decode().strip()
            self.assertEqual(len(snapshot.verify(tree)), 1)
            original.write_bytes(b'int value = 2;\n')
            with self.assertRaises(SystemExit):
                snapshot.verify(tree)
            original.write_bytes(b'int value = 1;\n')
            extra = snapshot.ROOT / 'src/extra.cpp'
            extra.touch()
            with self.assertRaises(SystemExit):
                snapshot.verify(tree)
            extra.unlink()
            original.unlink()
            with self.assertRaises(SystemExit):
                snapshot.verify(tree)

    def test_powershell_build_record_uses_plain_strings_and_path_placeholders(self):
        script = ROOT / 'scripts/nova-qt-build-record.ps1'
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/phase4i2-evidence') as folder:
            base = pathlib.Path(folder).resolve()
            self.assertTrue(base.is_relative_to(ROOT / 'build/phase4i2-evidence'))
            for scenario in ('Users/Example User', 'Users/Jos\u00e9 \u674e', 'Temporary Directory/Build Space'):
                target = base / scenario
                target.mkdir(parents=True)
                manifest = target / 'manifest.txt'
                manifest.write_text('mingw-w64-ucrt-x86_64-gcc 16.2.0-6\n', encoding='utf-8')
                command = ("[Console]::OutputEncoding=[Text.UTF8Encoding]::new(); . '" + str(script) + "'; "
                           "$record=[ordered]@{packages=@(Get-NovaQtPackageLines '" + str(manifest) + "'); "
                           "options=@(ConvertTo-NovaQtBuildOptions @('-DCMAKE_INSTALL_PREFIX=" + str(target) + "',"
                           "'-DCMAKE_C_COMPILER=" + str(target / 'gcc.exe') + "','-DFEATURE_schannel=ON'))}; "
                           "$record|ConvertTo-Json -Depth 4")
                output = subprocess.check_output(['powershell', '-NoProfile', '-Command', command], encoding='utf-8')
                record = json.loads(output.lstrip('\ufeff'))
                self.assertEqual(record['packages'], ['mingw-w64-ucrt-x86_64-gcc 16.2.0-6'])
                self.assertIn('-DCMAKE_INSTALL_PREFIX=<qt-prefix>', record['options'])
                self.assertNotIn(str(base), output)
                for forbidden in ('PSPath', 'PSDrive', 'PSProvider', os.environ['USERPROFILE']):
                    self.assertNotIn(forbidden, output)


if __name__ == '__main__':
    unittest.main()
