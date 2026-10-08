from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pe_fixture import pe_image
from build_dependencies import BuildError, DependencyReport, check_dependencies, install_help
from builder_core import build_commands, generator_dependencies, package
from windows_package import pe_imports, stage_windows_dependencies, system_dll


class WindowsCrossTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='Windows package with spaces ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.exe = self.root / 'game.exe'
        self.exe.write_bytes(pe_image(['KERNEL32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll']))
        self.output = self.root / 'output'
        self.output.mkdir()

    def test_pe_normal_and_delay_imports(self):
        self.exe.write_bytes(pe_image(['USER32.dll'], delay=['libwinpthread-1.dll']))
        self.assertEqual(pe_imports(self.exe), ['libwinpthread-1.dll', 'USER32.dll'])

    def test_invalid_binary_is_rejected(self):
        for data in (b'fake executable', pe_image(machine=0x14c), b'MZ'):
            self.exe.write_bytes(data)
            with self.assertRaisesRegex(BuildError, 'Cannot validate Windows'):
                pe_imports(self.exe)

    def test_system_imports_need_no_bundled_dlls(self):
        stage_windows_dependencies(self.exe, self.output, None, print)
        manifest = json.loads((self.output / 'windows-imports.json').read_text())
        self.assertIn('game.exe', manifest)
        self.assertEqual(list(self.output.glob('*.dll')), [])
        self.assertFalse(system_dll('SDL3.dll'))
        self.assertFalse(system_dll('libstdc++-6.dll'))

    def test_dll_closure_case_insensitive_and_paths_with_spaces(self):
        dll_dir = self.root / 'Custom DLL folder'
        dll_dir.mkdir()
        self.exe.write_bytes(pe_image(['SDL3.dll']))
        (dll_dir / 'sdl3.DLL').write_bytes(pe_image(['libwinpthread-1.dll', 'USER32.dll']))
        (dll_dir / 'libwinpthread-1.dll').write_bytes(pe_image(['KERNEL32.dll']))
        report = DependencyReport('linux', 'windows', {'WINDOWS_DLL_PATH': str(dll_dir)})
        stage_windows_dependencies(self.exe, self.output, report, lambda _line: None)
        self.assertEqual(len(list(self.output.glob('*.[dD][lL][lL]'))), 2)
        self.assertEqual(len(json.loads((self.output / 'windows-imports.json').read_text())), 3)

    def test_missing_runtime_dll_has_actionable_message(self):
        self.exe.write_bytes(pe_image(['SDL3.dll']))
        with self.assertRaisesRegex(BuildError, 'Windows package needs SDL3.dll') as error:
            stage_windows_dependencies(self.exe, self.output, None, print)
        self.assertIn('WINDOWS_DLL_PATH', str(error.exception))

    def test_wrong_architecture_dll_is_rejected(self):
        self.exe.write_bytes(pe_image(['SDL3.dll']))
        (self.root / 'SDL3.dll').write_bytes(pe_image(machine=0x14c))
        with self.assertRaisesRegex(BuildError, 'expected Windows x86_64'):
            stage_windows_dependencies(self.exe, self.output, None, print)

    def test_fedora_sysroot_dll_directory(self):
        compiler_bin = self.root / 'usr/bin'
        compiler_bin.mkdir(parents=True)
        dll_dir = self.root / 'usr/x86_64-w64-mingw32/sys-root/mingw/bin'
        dll_dir.mkdir(parents=True)
        self.exe.write_bytes(pe_image(['libstdc++-6.dll']))
        (dll_dir / 'libstdc++-6.dll').write_bytes(pe_image())
        report = DependencyReport('linux', 'windows', {}, tools={'gcc': str(compiler_bin / 'gcc')})
        stage_windows_dependencies(self.exe, self.output, report, print)
        self.assertTrue((self.output / 'libstdc++-6.dll').is_file())

    def test_windows_failure_does_not_publish_partial_package(self):
        self.exe.write_bytes(pe_image(['unavailable-runtime.dll']))
        with self.assertRaises(BuildError):
            package(self.exe, 'windows', self.root / 'disc.cue', [], self.output, print)
        self.assertEqual(list((self.output / 'Windows').iterdir()), [])

    def test_native_generator_ignores_target_compiler(self):
        report = DependencyReport('linux', 'windows', {'CC': '/cross/gcc', 'CXX': '/cross/g++', 'AR': '/cross/ar', 'MINGW_SYSROOT': str(self.root)})
        with mock.patch('builder_core.check_dependencies') as check:
            generator_dependencies(report)
        env = check.call_args.kwargs['environ']
        self.assertEqual(env['CC'], 'gcc')
        self.assertEqual(env['CXX'], 'g++')
        self.assertEqual(env['AR'], 'ar')
        self.assertEqual(check.call_args.args, ('linux',))

    def test_cross_toolchain_only_used_for_windows_runtime(self):
        tools = {name: '/custom tools/' + name for name in ('cmake','ninja','gcc','g++','ar','ld','ranlib','windres')}
        report = DependencyReport('linux', 'windows', {}, tools=tools)
        command, _, _ = build_commands('windows', self.root, self.root / 'bios.bin', report)
        self.assertIn('-DCMAKE_C_COMPILER=/custom tools/gcc', command)
        self.assertTrue(any(arg.endswith('toolchains/mingw64.cmake') for arg in command))
        report.target = 'linux'
        command, _, _ = build_commands('linux', self.root, self.root / 'bios.bin', report)
        self.assertFalse(any('CMAKE_TOOLCHAIN_FILE' in arg for arg in command))

    def test_target_library_link_failure_prevents_build(self):
        def probe(command, **kwargs):
            if '-x' in command:
                self.assertIn('-static', command)
                self.assertIn('-lwinpthread', command)
                return subprocess.CompletedProcess(command, 1, '', 'cannot find -lwinpthread')
            text = 'x86_64-w64-mingw32' if '-dumpmachine' in command else 'cmake version 3.29.0'
            return subprocess.CompletedProcess(command, 0, text, '')
        with mock.patch('build_dependencies.shutil.which', side_effect=lambda name, **kwargs: '/tools/' + name), \
             mock.patch('build_dependencies.subprocess.run', side_effect=probe):
            report = check_dependencies('windows', environ={'PATH': '/tools'}, host='linux')
        with self.assertRaisesRegex(BuildError, 'cannot find -lwinpthread'):
            report.require()
        self.assertIn('mingw64-winpthreads-static', '\n'.join(report.lines()))

    def test_invalid_explicit_sysroot_is_not_ignored(self):
        with mock.patch('build_dependencies.shutil.which', side_effect=lambda name, **kwargs: '/tools/' + name), \
             mock.patch('build_dependencies.subprocess.run', return_value=subprocess.CompletedProcess([], 0, 'x86_64-w64-mingw32 cmake version 3.29.0', '')):
            report = check_dependencies('windows', environ={'PATH': '/tools', 'MINGW_SYSROOT': str(self.root / 'missing')}, host='linux')
        with self.assertRaisesRegex(BuildError, 'MINGW_SYSROOT does not exist'):
            report.require()

    def test_fedora_installation_help(self):
        help_text = install_help('g++', 'linux', 'windows')
        self.assertIn('sudo dnf install', help_text)
        self.assertIn('mingw64-gcc-c++', help_text)
        self.assertIn('Ubuntu/Debian', help_text)


if __name__ == '__main__':
    unittest.main()
