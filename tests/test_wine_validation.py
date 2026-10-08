from __future__ import annotations
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_dependencies import BuildError
from wine_validation import MARKERS, wine_smoke


class WineValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='Wine check with spaces ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'Windows package'
        (self.package / 'disc').mkdir(parents=True)
        (self.package / 'MickeyWildAdventureRecompiled.exe').touch()
        (self.package / 'disc' / "Mickey's game.cue").touch()
        self.work = self.root / 'work'
        self.which = mock.patch('wine_validation.shutil.which', side_effect=lambda name: '/usr/bin/' + name).start()
        self.cleanup = mock.patch('wine_validation.subprocess.run').start()
        self.addCleanup(mock.patch.stopall)

    def process(self, text, *, code=None):
        def start(command, **kwargs):
            kwargs['stdout'].write(text)
            self.command, self.options = command, kwargs
            process = mock.Mock()
            process.poll.return_value = 0
            if code is None:
                process.wait.side_effect = subprocess.TimeoutExpired(command, 10)
            else:
                process.wait.return_value = code
            return process
        return mock.patch('wine_validation.subprocess.Popen', side_effect=start)

    def test_isolated_prefix_and_argument_paths(self):
        with mock.patch.dict(os.environ, {'WINEPREFIX': '/user/prefix'}), self.process('\n'.join(MARKERS)):
            result = wine_smoke(self.package, self.work, lambda _line: None)
        self.assertTrue(result.is_file())
        env = self.options['env']
        self.assertNotEqual(env['WINEPREFIX'], '/user/prefix')
        self.assertTrue(Path(env['WINEPREFIX']).is_relative_to(self.work))
        self.assertIn(str(self.package / 'disc' / "Mickey's game.cue"), self.command)
        self.assertIn('--headless', self.command)
        self.assertEqual(self.cleanup.call_args.kwargs['env']['WINEPREFIX'], env['WINEPREFIX'])
        self.assertFalse(Path(env['WINEPREFIX']).parent.exists())

    def test_missing_wine_is_optional_actionable(self):
        self.which.return_value = None
        self.which.side_effect = None
        with self.assertRaisesRegex(BuildError, 'optional'):
            wine_smoke(self.package, self.work)

    def test_early_crash_is_not_success(self):
        with self.process('\n'.join(MARKERS), code=5):
            with self.assertRaisesRegex(BuildError, 'exited early \(code 5\)'):
                wine_smoke(self.package, self.work, lambda _line: None)
        self.assertTrue(self.cleanup.called)

    def test_missing_runtime_markers_fail(self):
        with self.process('process started'):
            with self.assertRaisesRegex(BuildError, 'Missing runtime markers'):
                wine_smoke(self.package, self.work, lambda _line: None)

    def test_unresolved_dll_is_not_hidden(self):
        with self.process('\n'.join(MARKERS) + '\nerr:module:import_dll SDL3.dll'):
            with self.assertRaisesRegex(BuildError, 'not verified'):
                wine_smoke(self.package, self.work, lambda _line: None)

    def test_launch_failure_identifies_wine(self):
        with mock.patch('wine_validation.subprocess.Popen', side_effect=FileNotFoundError('missing loader')):
            with self.assertRaisesRegex(BuildError, 'Cannot launch Wine executable /usr/bin/wine'):
                wine_smoke(self.package, self.work, lambda _line: None)


if __name__ == '__main__':
    unittest.main()
