"""Unit tests of orchestration/cache controls, with subprocesses mocked.

The C text below tests validation and staging only; it is never used as a BIOS
backend in a real build. Real generation and compile/link are tested separately.
"""
from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import builder_core as core
from build_dependencies import BuildError, DependencyReport

FULL_TEXT = f'/* BIOS SHA256: {core.OPENBIOS_SHA256} */\n#include "cpu_state.h"\nvoid OpenBIOS_test(CPUState* cpu) {{}}\n'
DISPATCH_TEXT = f'/* BIOS SHA256: {core.OPENBIOS_SHA256} */\n#include "cpu_state.h"\nconst PsxBiosBackend OpenBIOS_psx_bios_backend = {{}};\n'


class OpenBiosPipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Use the actual bundled redistributable image; no disc files needed.
        cls.rom = core.prepare_openbios().read_bytes()
        cls.embedded_source = (core.PROJECT / "embedded_openbios.cpp").read_text(encoding="ascii")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="BIOS paths with spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project"
        self.framework = self.project / "psxrecomp"
        (self.framework / "bios").mkdir(parents=True)
        self.profile = self.framework / "bios/OpenBIOS.toml"
        self.profile.write_text('rom = "bios/openbios.bin"')
        seed = self.framework / "recompiler/seeds/openbios_elf_seeds.json"
        seed.parent.mkdir(parents=True)
        seed.write_text("{}")
        (self.project / "embedded_openbios.cpp").write_text(self.embedded_source, encoding="ascii")
        for name, value in (("ROOT", self.root), ("PROJECT", self.project)):
            patcher = mock.patch.object(core, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.report = DependencyReport("linux", "linux", {"PATH": "/tools"},
            tools={name: "/tools/" + name for name in ("cmake", "ninja", "gcc", "g++", "ar", "ld", "ranlib", "windres")})
        self.messages = []

    def fake_run(self, command, log, **_kwargs):
        if "--build" in command:
            directory = Path(command[command.index("--build") + 1])
            name = "psxrecomp-bios.exe" if self.report.target == "windows" and not self.report.launcher else "psxrecomp-bios"
            (directory / name).touch()
        elif "--config" in command:
            stage = Path(command[command.index("--out-dir") + 1])
            (stage / "OpenBIOS_full.c").write_text(FULL_TEXT)
            (stage / "OpenBIOS_dispatch.c").write_text(DISPATCH_TEXT)

    def pipeline(self, run=None):
        with mock.patch.object(core, "run", side_effect=run or self.fake_run) as runner:
            core.prepare_openbios_backend(core.prepare_openbios(), self.report, self.messages.append)
        return runner

    def test_recovers_rom_to_profile_location_and_verifies_hash(self):
        bios = core.prepare_openbios()
        self.assertEqual(bios, self.framework / "bios/openbios.bin")
        self.assertEqual(len(bios.read_bytes()), 524288)
        self.assertEqual(hashlib.sha256(bios.read_bytes()).hexdigest(), core.OPENBIOS_SHA256)
        original_mtime = bios.stat().st_mtime_ns
        core.prepare_openbios()
        self.assertEqual(bios.stat().st_mtime_ns, original_mtime)

    def test_repairs_corrupted_local_rom_from_embedded_source(self):
        bios = core.prepare_openbios()
        bios.write_bytes(b"corrupt")
        self.assertEqual(core.prepare_openbios().read_bytes(), self.rom)

    def test_rejects_corrupted_embedded_rom(self):
        source = self.project / "embedded_openbios.cpp"
        source.write_text(self.embedded_source.replace("0x", "0y", 1), encoding="ascii")
        with self.assertRaisesRegex(BuildError, "integrity check"):
            core.prepare_openbios()
        self.assertFalse((self.framework / "bios/openbios.bin").exists())

    def test_rejects_missing_embedded_source(self):
        (self.project / "embedded_openbios.cpp").unlink()
        with self.assertRaisesRegex(BuildError, "Restore project/embedded_openbios.cpp"):
            core.prepare_openbios()

    def test_generation_commands_and_staging_preserve_spaces(self):
        runner = self.pipeline()
        calls = [call.args[0] for call in runner.call_args_list]
        self.assertEqual(len(calls), 5)  # configure, build emitter, emit, two syntax checks
        self.assertIn("-DPSXRECOMP_ENABLE_CHD=OFF", calls[0])
        self.assertIn("-DBUILD_TESTING=OFF", calls[0])
        self.assertIn("psxrecomp-bios", calls[1])
        self.assertIn(str(self.profile), calls[2])
        self.assertIn(str(self.framework / "bios/openbios.bin"), calls[2])
        self.assertIn("-fsyntax-only", calls[3])
        self.assertEqual(len(core.validate_openbios_generated(self.framework / "generated")), 2)
        self.assertIn("backend verified", self.messages[-1])

    def test_valid_cache_skips_generator_and_preserves_mtime(self):
        self.pipeline()
        full = self.framework / "generated/OpenBIOS_full.c"
        original_mtime = full.stat().st_mtime_ns
        runner = self.pipeline()
        runner.assert_not_called()
        self.assertEqual(full.stat().st_mtime_ns, original_mtime)
        self.assertIn("Reusing verified", self.messages[-1])

    def test_profile_change_invalidates_cache(self):
        self.pipeline()
        self.profile.write_text('rom = "bios/openbios.bin"\n# updated profile')
        self.assertEqual(self.pipeline().call_count, 5)

    def test_seed_emitter_header_and_env_changes_invalidate_fingerprint(self):
        seed = self.framework / "recompiler/seeds/openbios_elf_seeds.json"
        seed.parent.mkdir(parents=True, exist_ok=True)
        seed.write_text('{}')
        emitter = self.framework / "recompiler/src/main_bios.cpp"
        emitter.parent.mkdir()
        emitter.write_text('// emitter')
        header = self.framework / "runtime/include/cpu_state.h"
        header.parent.mkdir(parents=True)
        header.write_text('// header')
        for path in (seed, emitter, header):
            before = core.openbios_fingerprint({})
            path.write_text(path.read_text() + '\n// changed')
            self.assertNotEqual(before, core.openbios_fingerprint({}))
        self.assertNotEqual(core.openbios_fingerprint({}), core.openbios_fingerprint({"PSX_CPS": "0"}))

    def test_missing_generated_output_regenerates(self):
        self.pipeline()
        (self.framework / "generated/OpenBIOS_full.c").unlink()
        self.assertEqual(self.pipeline().call_count, 5)

    def test_changed_output_even_with_valid_provenance_regenerates(self):
        self.pipeline()
        path = self.framework / "generated/OpenBIOS_full.c"
        path.write_text(path.read_text() + '\n/* changed */')
        self.assertEqual(self.pipeline().call_count, 5)

    def test_corrupt_cache_manifest_regenerates(self):
        self.pipeline()
        (self.framework / "generated/.build-studio-openbios.json").write_text('not JSON')
        self.assertEqual(self.pipeline().call_count, 5)

    def test_generator_failure_preserves_previous_outputs(self):
        self.pipeline()
        full = self.framework / "generated/OpenBIOS_full.c"
        before = full.read_bytes()
        self.profile.write_text('# force regeneration')
        with self.assertRaisesRegex(BuildError, r"C\+\+20-capable"):
            self.pipeline(run=lambda *_args, **_kwargs: (_ for _ in ()).throw(BuildError("generator failed")))
        self.assertEqual(full.read_bytes(), before)

    def test_missing_or_invalid_emission_never_becomes_cached(self):
        def bad_run(command, log, **kwargs):
            self.fake_run(command, log, **kwargs)
            if "--config" in command:
                stage = Path(command[command.index("--out-dir") + 1])
                (stage / "OpenBIOS_dispatch.c").write_text("invalid output")
        with self.assertRaisesRegex(BuildError, "provenance"):
            self.pipeline(run=bad_run)
        self.assertFalse((self.framework / "generated/.build-studio-openbios.json").exists())

    def test_rejects_descriptor_reference_without_definition(self):
        stage = self.root / "stage"
        stage.mkdir()
        (stage / "OpenBIOS_full.c").write_text(FULL_TEXT)
        (stage / "OpenBIOS_dispatch.c").write_text(DISPATCH_TEXT.replace('const PsxBiosBackend', 'extern PsxBiosBackend').replace(' = {}', ''))
        with self.assertRaisesRegex(BuildError, "backend descriptor"):
            core.validate_openbios_generated(stage)

    def test_syntax_failure_is_not_cached(self):
        def bad_run(command, log, **kwargs):
            self.fake_run(command, log, **kwargs)
            if "-fsyntax-only" in command:
                raise BuildError("target compiler rejected generated C")
        with self.assertRaisesRegex(BuildError, "target compiler rejected"):
            self.pipeline(run=bad_run)
        self.assertFalse((self.framework / "generated/.build-studio-openbios.json").exists())

    def test_pipeline_failure_prevents_runtime_configuration(self):
        game_source = self.project / "generated/SCES_001.63_dispatch.c"
        game_source.parent.mkdir()
        game_source.touch()  # file-presence fixture; compilation is never invoked
        with mock.patch.object(core, 'check_build_dependencies', return_value=self.report), \
             mock.patch.object(core, 'validate_assets'), \
             mock.patch.object(core, 'disc_files', return_value=(self.root / 'disc.cue', [])), \
             mock.patch.object(core, 'prepare_openbios_backend', side_effect=BuildError('BIOS failed')), \
             mock.patch.object(core, 'run') as runtime:
            with self.assertRaisesRegex(BuildError, 'BIOS failed'):
                core.build('linux', self.root, self.root, lambda _line: None)
        runtime.assert_not_called()

    def test_native_windows_builds_exe_generator(self):
        self.report.host = self.report.target = "windows"
        runner = self.pipeline()
        calls = [call.args[0] for call in runner.call_args_list]
        self.assertIn("-DPSXRECOMP_STATIC_CLI=ON", calls[0])
        self.assertTrue(calls[2][0].endswith('psxrecomp-bios.exe'))

    def test_cross_build_uses_native_generator_compilers(self):
        runtime = DependencyReport("linux", "windows", {"CC": "cross-gcc", "CXX": "cross-g++"})
        with mock.patch.object(core, 'check_dependencies', return_value=self.report) as check:
            self.assertIs(core.generator_dependencies(runtime), self.report)
        env = check.call_args.kwargs['environ']
        self.assertEqual(env['CC'], 'gcc')
        self.assertEqual(env['CXX'], 'g++')
        self.assertEqual(env['AR'], 'ar')

    def test_wsl_uses_launcher_and_linux_generator(self):
        self.report.host = 'windows'
        self.report.launcher = ['C:/Windows/System32/wsl.exe', '--exec']
        # Identity mapping tests orchestration without requiring a WSL host.
        def wsl_run(command, log, **kwargs):
            self.fake_run(command[2:], log, **kwargs)
        with mock.patch.object(core, 'wsl_path', side_effect=lambda path, _report: str(path)):
            runner = self.pipeline(run=wsl_run)
        for call in runner.call_args_list:
            self.assertEqual(call.args[0][:2], self.report.launcher)
        self.assertTrue(runner.call_args_list[2].args[0][2].endswith('psxrecomp-bios'))
        self.assertFalse(runner.call_args_list[2].args[0][2].endswith('.exe'))


if __name__ == '__main__':
    unittest.main()
