"""Optional, bounded Wine startup check; never substitutes for a graphics test."""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Callable

from build_dependencies import BuildError

MARKERS = ('disc region PAL (serial SCES-00163)', 'image=OPENBIOS', 'executing from PC=0xBFC00000')


def wine_smoke(package: Path, work: Path, log: Callable[[str], None] = print, *, seconds: int = 12) -> Path:
    wine = shutil.which('wine')
    server = shutil.which('wineserver')
    if not wine or not server:
        raise BuildError('Wine startup check is optional. Install Wine (Fedora/Nobara: sudo dnf install wine), or run the package on Windows.')
    package = package.resolve()
    executable = package / 'MickeyWildAdventureRecompiled.exe'
    cues = sorted((package / 'disc').glob('*.cue'))
    if not executable.is_file() or len(cues) != 1:
        raise BuildError('Wine startup check needs the complete Windows package with one CUE in disc/.')
    work.mkdir(parents=True, exist_ok=True)
    output = work / 'wine-startup.log'
    with tempfile.TemporaryDirectory(prefix='build-studio-wine-', dir=work) as folder:
        prefix = Path(folder).resolve() / 'prefix'
        env = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH='win64', WINEDEBUG='-all,err+all,warn+loaddll')
        session = Path(folder) / 'session'
        session.mkdir()
        log(f'Checking Windows startup with Wine in an isolated prefix; log: {output}')
        process = None
        try:
            with output.open('w', encoding='utf-8') as stream:
                # Wine initializes this fresh prefix before loading the PE.
                command = [wine, str(executable), '--game', str(package / 'game.toml'), '--disc', str(cues[0]),
                           '--headless', '--no-launcher', '--debug-port', '0', '--memcard-dir', str(session / 'cards')]
                process = subprocess.Popen(command, cwd=session, env=env, stdout=stream, stderr=subprocess.STDOUT)
                try:
                    code = process.wait(timeout=seconds + 45)
                    raise BuildError(f'Wine game exited early (code {code}). See {output}.')
                except subprocess.TimeoutExpired:
                    pass
        except OSError as exc:
            raise BuildError(f'Cannot launch Wine executable {wine}: {exc}. See {output}.') from exc
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            try:
                subprocess.run([server, '-k'], env=env, capture_output=True, timeout=10)
                subprocess.run([server, '-w'], env=env, capture_output=True, timeout=10)
            except (OSError, subprocess.SubprocessError) as exc:
                log(f'Private Wine prefix cleanup warning: {exc}')
        text = output.read_text(encoding='utf-8', errors='replace')
        missing = [marker for marker in MARKERS if marker not in text]
        if missing or 'err:module:import_dll' in text or 'Unhandled exception' in text:
            raise BuildError(f'Wine startup was not verified. Missing runtime markers: {missing}. See {output}.')
    log('Wine startup verified: Windows PE loaded, PAL disc recognized, OpenBIOS execution started. Graphics, audio and gameplay need a graphical test; native Windows remains untested.')
    return output
