#!/usr/bin/env python3
"""Local build and packaging engine. Original media never enters the source tree."""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT / "project"
PRODUCT = "MickeyWildAdventureRecompiled"
SERIAL = b"SCES_001.63"
OPENBIOS_SHA256 = "fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57"
REQUIRED_ASSETS = (
    "frontend/background.png",
    "frontend/logo.png",
    "frontend/preview_test.png",
    "frontend/audio/menu_music.wav",
    "frontend/audio/title_music.wav",
    "frontend/audio/pickup.wav",
    "frontend/audio/enter.wav",
    "i18n/glyphs/manifest.txt",
)
Log = Callable[[str], None]


class BuildError(RuntimeError):
    pass


def within(path: Path, base: Path) -> bool:
    try:
        path.resolve().relative_to(base.resolve())
        return True
    except ValueError:
        return False


def disc_files(folder: Path) -> tuple[Path, list[Path]]:
    folder = folder.expanduser().resolve()
    if not folder.is_dir():
        raise BuildError("The selected disc folder does not exist.")
    cues = sorted(folder.glob("*.cue"))
    if len(cues) != 1:
        raise BuildError("Select a folder containing exactly one CUE file.")
    cue = cues[0]
    files: list[Path] = []
    for line in cue.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        match = re.match(r'^\s*FILE\s+(?:"([^"]+)"|(\S+))\s+', line, re.I)
        if not match:
            continue
        name = (match.group(1) or match.group(2)).replace("\\", "/")
        candidate = (folder / name).resolve()
        if not within(candidate, folder) or not candidate.is_file():
            raise BuildError(f"Missing or unsafe CUE track: {name}")
        if candidate not in files:
            files.append(candidate)
    if not files:
        raise BuildError("The CUE file contains no tracks.")
    if len(files) < 28:
        raise BuildError(f"Expected the complete 28-track disc, found {len(files)} tracks.")
    if not has_serial(files[0]):
        raise BuildError("The selected disc does not match the supported PAL serial.")
    return cue, files


def has_serial(track: Path) -> bool:
    tail = b""
    with track.open("rb") as stream:
        while block := stream.read(4 * 1024 * 1024):
            data = tail + block
            if SERIAL in data:
                return True
            tail = data[-len(SERIAL):]
    return False


def run(command: list[str], log: Log, *, cwd: Path, env: dict[str, str] | None = None) -> None:
    log("$ " + " ".join(shlex.quote(str(part)) for part in command))
    try:
        process = subprocess.Popen(
            command, cwd=cwd, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1,
        )
    except OSError as exc:
        raise BuildError(f"Unable to start build tool: {exc}") from exc
    assert process.stdout is not None
    for line in process.stdout:
        log(line.rstrip())
    if process.wait() != 0:
        raise BuildError(f"Build command failed with exit code {process.returncode}.")


def windows_env() -> dict[str, str]:
    env = os.environ.copy()
    candidates = [Path(os.environ.get("MSYS2_ROOT", "C:/msys64")) / "mingw64/bin"]
    compiler_dir = next((p for p in candidates if (p / "gcc.exe").is_file()), None)
    if compiler_dir is None:
        raise BuildError("Windows build requires MSYS2 MINGW64 at C:/msys64 (or MSYS2_ROOT).")
    env["PATH"] = str(compiler_dir) + os.pathsep + env.get("PATH", "")
    env["CC"] = str(compiler_dir / "gcc.exe")
    env["CXX"] = str(compiler_dir / "g++.exe")
    return env


def wsl_path(path: Path) -> str:
    result = subprocess.run(
        ["wsl.exe", "--exec", "wslpath", "-a", str(path)],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode or not result.stdout.strip():
        raise BuildError("WSL is required to build Linux on a Windows host.")
    return result.stdout.strip()


def prepare_openbios() -> Path:
    source = (PROJECT / "embedded_openbios.cpp").read_text(encoding="ascii")
    marker = "static const unsigned char kEmbeddedOpenBios[] = {"
    if marker not in source:
        raise BuildError("Embedded OpenBIOS source is missing.")
    array = source.split(marker, 1)[1].split("};", 1)[0]
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})\b", array))
    if hashlib.sha256(data).hexdigest() != OPENBIOS_SHA256:
        raise BuildError("Embedded OpenBIOS data failed its integrity check.")
    destination = ROOT / "work/openbios.bin"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file() or destination.read_bytes() != data:
        destination.write_bytes(data)
    return destination


def build_commands(target: str, build_dir: Path, bios: Path) -> tuple[list[str], list[str], dict[str, str] | None]:
    host = platform.system().lower()
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPSX_SDL_BACKEND=SDL3",
              f"-DPSX_DEBUG_TOOLS={'OFF' if target == 'linux' else 'ON'}"]
    if host == "windows" and target == "windows":
        env = windows_env()
        return (["cmake", "-S", str(PROJECT), "-B", str(build_dir), *common,
                 f"-DPSXRECOMP_BUNDLED_BIOS_SOURCE={bios}"],
                ["cmake", "--build", str(build_dir), "--parallel", "4"], env)
    if host == "linux" and target == "linux":
        return (["cmake", "-S", str(PROJECT), "-B", str(build_dir), *common,
                 f"-DPSXRECOMP_BUNDLED_BIOS_SOURCE={bios}"],
                ["cmake", "--build", str(build_dir), "--parallel", "4"], None)
    if host == "windows" and target == "linux":
        source, build = wsl_path(PROJECT), wsl_path(build_dir)
        configure = ["cmake", "-S", source, "-B", build, *common,
                     f"-DPSXRECOMP_BUNDLED_BIOS_SOURCE={wsl_path(bios)}"]
        compile_cmd = ["cmake", "--build", build, "--parallel", "4"]
        return (["wsl.exe", "--exec", *configure], ["wsl.exe", "--exec", *compile_cmd], None)
    if host == "linux" and target == "windows":
        compiler = shutil.which("x86_64-w64-mingw32-gcc")
        if not compiler:
            raise BuildError("Windows cross-build requires the MinGW-w64 cross compiler.")
        toolchain = ROOT / "toolchains/mingw64.cmake"
        return (["cmake", "-S", str(PROJECT), "-B", str(build_dir), *common,
                 f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                 f"-DPSXRECOMP_BUNDLED_BIOS_SOURCE={bios}"],
                ["cmake", "--build", str(build_dir), "--parallel", "4"], None)
    raise BuildError(f"Unsupported host/target combination: {host}/{target}.")


def find_executable(build_dir: Path, target: str) -> Path:
    marker = build_dir / "psxrecomp_exe_name-psx-runtime.txt"
    suffix = ".exe" if target == "windows" else ""
    if marker.is_file():
        name = marker.read_text(encoding="utf-8").strip()
        candidate = build_dir / (name + suffix)
        if candidate.is_file():
            return candidate
    candidates = [p for p in build_dir.glob("*") if p.is_file() and
                  (p.suffix.lower() == ".exe" if target == "windows" else os.access(p, os.X_OK)) and
                  ("mickey" in p.name.lower() or p.name == "psx-runtime")]
    if not candidates:
        raise BuildError("The linker finished but no game executable was found.")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def copy_tree(source: Path, destination: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, destination, ignore=shutil.ignore_patterns("*.before_*"))


def validate_assets() -> None:
    missing = [name for name in REQUIRED_ASSETS if not (PROJECT / "assets" / name).is_file()]
    if missing:
        raise BuildError("Frontend assets are incomplete: " + ", ".join(missing))
    asset_count = sum(path.is_file() for path in (PROJECT / "assets").rglob("*"))
    if asset_count < 680:
        raise BuildError(f"Frontend assets are incomplete: {asset_count}/680 files.")
    missing_borders = [f"{number:02d}.png" for number in range(1, 5)
                       if not (PROJECT / "Border" / f"{number:02d}.png").is_file()]
    if missing_borders:
        raise BuildError("Display frames are incomplete: " + ", ".join(missing_borders))


def package(executable: Path, target: str, cue: Path, tracks: list[Path], output_root: Path, log: Log) -> Path:
    validate_assets()
    parent = output_root.expanduser().resolve() / target.capitalize()
    parent.mkdir(parents=True, exist_ok=True)
    final = parent / PRODUCT
    if final.exists():
        raise BuildError(f"Output already exists; choose a new destination: {final}")
    temporary = Path(tempfile.mkdtemp(prefix=".staging-", dir=parent))
    try:
        output_exe = temporary / (PRODUCT + (".exe" if target == "windows" else ""))
        shutil.copy2(executable, output_exe)
        if target == "linux":
            output_exe.chmod(output_exe.stat().st_mode | 0o111)
        for name in ("assets", "Border", "mods"):
            copy_tree(PROJECT / name, temporary / name)
        source_assets = {path.relative_to(PROJECT / "assets")
                         for path in (PROJECT / "assets").rglob("*") if path.is_file()}
        output_assets = {path.relative_to(temporary / "assets")
                         for path in (temporary / "assets").rglob("*") if path.is_file()}
        if output_assets != source_assets:
            raise BuildError("Asset staging failed: output does not match source snapshot.")
        for name in ("game.toml", "input.ini", "keybinds.ini"):
            shutil.copy2(PROJECT / name, temporary / name)
        shutil.copy2(PROJECT / "config" / target / "mickey_frontend.ini",
                     temporary / "mickey_frontend.ini")
        licenses = temporary / "licenses"
        licenses.mkdir()
        shutil.copy2(PROJECT / "psxrecomp/bios/OpenBIOS.LICENSE", licenses / "OpenBIOS.LICENSE")
        shutil.copy2(PROJECT / "psxrecomp/third_party/rcheevos/LICENSE", licenses / "rcheevos-LICENSE.txt")
        disc_out = temporary / "disc"
        disc_out.mkdir()
        log("Copying the selected disc into the local game package...")
        for source in [cue, *tracks]:
            destination = disc_out / source.relative_to(cue.parent)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        boot_exe = cue.parent / "SCES_001.63"
        if boot_exe.is_file():
            shutil.copy2(boot_exe, disc_out / boot_exe.name)
        if target == "windows":
            shutil.copy2(ROOT / "launchers/RUN_GAME.bat", temporary / "RUN_GAME.bat")
        else:
            launcher = temporary / "RUN_GAME.sh"
            shutil.copy2(ROOT / "launchers/RUN_GAME.sh", launcher)
            launcher.chmod(launcher.stat().st_mode | 0o111)
        (temporary / "README.txt").write_text(
            "Local build. Original disc files were copied from the folder chosen by the user.\n"
            "Do not redistribute this package unless you have the necessary rights.\n"
            "Launch with RUN_GAME.bat (Windows) or RUN_GAME.sh (Linux).\n",
            encoding="utf-8")
        temporary.rename(final)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return final


def build(target: str, disc_folder: Path, output_root: Path, log: Log = print) -> Path:
    if target not in {"windows", "linux"}:
        raise BuildError("Choose Windows or Linux.")
    if not (PROJECT / "generated/SCES_001.63_dispatch.c").is_file():
        raise BuildError("The generated source snapshot is incomplete.")
    validate_assets()
    cue, tracks = disc_files(disc_folder)
    log(f"Verified disc: {cue.name} ({len(tracks)} tracks)")
    build_dir = ROOT / "work" / f"build-{target}"
    build_dir.mkdir(parents=True, exist_ok=True)
    bios = prepare_openbios()
    configure, compile_cmd, env = build_commands(target, build_dir, bios)
    log("Configuring native runtime...")
    run(configure, log, cwd=ROOT, env=env)
    log("Compiling and linking the complete game...")
    run(compile_cmd, log, cwd=ROOT, env=env)
    executable = find_executable(build_dir, target)
    log(f"Linked executable: {executable.name}")
    final = package(executable, target, cue, tracks, output_root, log)
    log(f"Ready: {final}")
    return final


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a local game package from a user-owned disc")
    parser.add_argument("--target", choices=("windows", "linux"), required=True)
    parser.add_argument("--disc-folder", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        build(args.target, args.disc_folder, args.output)
    except BuildError as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
