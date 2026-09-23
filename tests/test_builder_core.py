from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from builder_core import BuildError, PROJECT, build_commands, disc_files, package, prepare_openbios, validate_assets


class BuilderCoreTests(unittest.TestCase):
    def test_linux_release_disables_debug_tools(self) -> None:
        with mock.patch("builder_core.platform.system", return_value="linux"):
            configure, _, _ = build_commands("linux", Path("build"), Path("bios.bin"))
        self.assertIn("-DPSX_DEBUG_TOOLS=OFF", configure)
        with mock.patch("builder_core.platform.system", return_value="windows"), \
             mock.patch("builder_core.windows_env", return_value={}):
            configure, _, _ = build_commands("windows", Path("build"), Path("bios.bin"))
        self.assertIn("-DPSX_DEBUG_TOOLS=ON", configure)

    def test_snapshot_and_openbios(self) -> None:
        self.assertTrue((PROJECT / "generated/SCES_001.63_dispatch.c").is_file())
        self.assertTrue((PROJECT / "assets/frontend/audio/menu_music.wav").is_file())
        validate_assets()
        self.assertEqual(prepare_openbios().stat().st_size, 524288)

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
            executable.write_bytes(b"test executable")
            disc = root / "input"
            disc.mkdir()
            cue = disc / "game.cue"
            cue.write_text('FILE "track.bin" BINARY\n', encoding="ascii")
            track = disc / "track.bin"
            track.write_bytes(b"SCES_001.63")
            (disc / "SCES_001.63").write_bytes(b"local boot image")
            result = package(executable, "windows", cue, [track], root / "output", lambda _line: None)
            self.assertTrue((result / "MickeyWildAdventureRecompiled.exe").is_file())
            self.assertTrue((result / "assets/frontend/audio/menu_music.wav").is_file())
            self.assertTrue((result / "assets/frontend/preview_test.png").is_file())
            self.assertTrue((result / "disc/game.cue").is_file())
            self.assertTrue((result / "disc/SCES_001.63").is_file())
            self.assertIn("bezel=4", (result / "mickey_frontend.ini").read_text())
            self.assertFalse((result / "retroachievements.ini").exists())
            self.assertFalse((result / "card1.mcd").exists())
            self.assertFalse((result / "profiles").exists())
            linux = package(executable, "linux", cue, [track], root / "output", lambda _line: None)
            self.assertTrue((linux / "assets/frontend/preview_test.png").is_file())
            self.assertIn("bezel=1", (linux / "mickey_frontend.ini").read_text())
            self.assertTrue((linux / "RUN_GAME.sh").is_file())


if __name__ == "__main__":
    unittest.main()
