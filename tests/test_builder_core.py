from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pe_fixture import pe_image
from build_dependencies import DependencyReport
from builder_core import BuildError, PROJECT, build_commands, check_build_dependencies, disc_files, package, prepare_openbios, validate_assets


class BuilderCoreTests(unittest.TestCase):
    def test_linux_release_disables_debug_tools(self) -> None:
        tools = {name: "/tools/" + name for name in ("cmake", "ninja", "gcc", "g++", "windres", "ar", "ld", "ranlib")}
        for target in ("linux", "windows"):
            report = DependencyReport(target, target, {}, tools=tools)
            configure, _, _ = build_commands(target, Path("build"), Path("bios.bin"), report)
            self.assertIn("-DPSX_DEBUG_TOOLS=" + ("OFF" if target == "linux" else "ON"), configure)

    def test_snapshot_and_openbios(self) -> None:
        self.assertTrue((PROJECT / "generated/SCES_001.63_dispatch.c").is_file())
        self.assertTrue((PROJECT / "assets/frontend/audio/menu_music.wav").is_file())
        validate_assets()
        self.assertEqual(prepare_openbios().stat().st_size, 524288)

    def test_missing_generator_inputs_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as folder, \
             mock.patch("builder_core.PROJECT", Path(folder)), \
             mock.patch("builder_core.check_dependencies", side_effect=lambda _target: DependencyReport("linux", "linux", {})):
            report = check_build_dependencies("linux")
            with self.assertRaisesRegex(BuildError, "generator inputs are missing"):
                report.require()

    def test_backend_absence_does_not_block_automatic_generation(self) -> None:
        with mock.patch("builder_core.check_dependencies", return_value=DependencyReport("linux", "linux", {})):
            self.assertTrue(check_build_dependencies("linux").ready)

    def test_complete_disc_validation(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            lines = []
            for index in range(1, 29):
                name = f"track{index:02d}.bin"
                (root / name).write_bytes(b"SCES_001.63" if index == 1 else b"test")
                lines.extend((f'FILE "{name}" BINARY', f"  TRACK {index:02d} MODE2/2352"))
            (root / "game.cue").write_text("\n".join(lines), encoding="ascii")
            cue, tracks = disc_files(root)
            self.assertEqual(cue.name, "game.cue")
            self.assertEqual(len(tracks), 28)

    def test_rejects_incomplete_or_wrong_version_disc(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            lines = []
            for index in range(1, 29):
                name = f"track{index:02d}.bin"
                (root / name).write_bytes(b"SCUS_999.99")
                lines.append(f'FILE "{name}" BINARY')
            cue = root / "game.cue"
            cue.write_text("\n".join(lines[:27]))
            with self.assertRaisesRegex(BuildError, "complete 28-track disc"):
                disc_files(root)
            cue.write_text("\n".join(lines))
            with self.assertRaisesRegex(BuildError, "supported PAL serial"):
                disc_files(root)
            (root / "track28.bin").unlink()
            with self.assertRaisesRegex(BuildError, "Missing or unsafe CUE track"):
                disc_files(root)

    def test_rejects_cue_escape(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "game.cue").write_text('FILE "../outside.bin" BINARY\n', encoding="ascii")
            with self.assertRaises(BuildError):
                disc_files(root)

    def test_local_package_is_clean(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            executable = root / "linked.exe"
            executable.write_bytes(pe_image(["KERNEL32.dll"]))
            disc = root / "input"
            disc.mkdir()
            cue = disc / "game.cue"
            tracks = []
            for index in range(1, 29):
                track = disc / f"track{index:02d}.bin"
                track.write_bytes(b"SCES_001.63" if index == 1 else bytes([index]))
                tracks.append(track)
            cue.write_text("\n".join(f'FILE "{track.name}" BINARY' for track in tracks), encoding="ascii")
            disc_files(disc)
            (disc / "SCES_001.63").write_bytes(b"local boot image")
            result = package(executable, "windows", cue, tracks, root / "output", lambda _line: None)
            self.assertTrue((result / "MickeyWildAdventureRecompiled.exe").is_file())
            self.assertEqual((result / "bios/openbios.bin").read_bytes(), prepare_openbios().read_bytes())
            self.assertTrue((result / "bios/OpenBIOS.LICENSE").is_file())
            self.assertTrue((result / "assets/frontend/audio/menu_music.wav").is_file())
            self.assertTrue((result / "assets/frontend/preview_test.png").is_file())
            self.assertTrue((result / "disc/game.cue").is_file())
            self.assertTrue((result / "disc/SCES_001.63").is_file())
            self.assertIn("bezel=4", (result / "mickey_frontend.ini").read_text())
            self.assertFalse((result / "retroachievements.ini").exists())
            self.assertFalse((result / "card1.mcd").exists())
            self.assertFalse((result / "profiles").exists())
            linux = package(executable, "linux", cue, tracks, root / "output", lambda _line: None)
            self.assertTrue((linux / "assets/frontend/preview_test.png").is_file())
            self.assertIn("bezel=1", (linux / "mickey_frontend.ini").read_text())
            self.assertTrue((linux / "RUN_GAME.sh").is_file())
            for packaged in (result, linux):
                self.assertEqual(len(list((packaged / "disc").glob("*.bin"))), 28)
                for track in tracks:
                    self.assertEqual((packaged / "disc" / track.name).read_bytes(), track.read_bytes())


if __name__ == "__main__":
    unittest.main()
