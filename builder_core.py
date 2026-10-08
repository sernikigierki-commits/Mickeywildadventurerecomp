#!/usr/bin/env python3
"""Local build and packaging engine. Original media never enters the source tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Callable

from build_dependencies import BuildError, Dependency, DependencyReport, check_dependencies, launch_error
from windows_package import stage_windows_dependencies

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


def check_build_dependencies(target: str) -> DependencyReport:
    report = check_dependencies(target)
    framework = PROJECT / "psxrecomp"
    required = [PROJECT / "embedded_openbios.cpp", framework / "bios/OpenBIOS.toml",
                framework / "recompiler/seeds/openbios_elf_seeds.json",
                framework / "recompiler/CMakeLists.txt", framework / "recompiler/src/main_bios.cpp",
                framework / "recompiler/cmake/generate_rabbitizer_table.cmake"]
    missing = [str(path) for path in required if not path.is_file()]
    detail = ("Sources ready. Build Studio prepares and generates the backend automatically on Build."
              if not missing else "OpenBIOS generator inputs are missing: " + ", ".join(missing) +
              ". Restore these files from a complete project checkout; no Sony BIOS is needed.")
    report.dependencies.append(Dependency("OpenBIOS generation", str(framework), detail, ok=not missing))
    if report.host == "linux" and target == "windows":
        native = generator_dependencies(report)
        report.dependencies.extend(Dependency("BIOS generator " + item.name, item.path, item.detail,
                                              required=item.required, ok=item.ok) for item in native.dependencies)
    return report


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
    effective_env = os.environ.copy() if env is None else env
    executable = shutil.which(str(command[0]), path=effective_env.get("PATH", ""))
    if executable is None:
        raise BuildError(launch_error(str(command[0]), FileNotFoundError("executable was not found"), cwd=cwd))
    command = [str(Path(executable).resolve()), *map(str, command[1:])]
    log("$ " + (subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)))
    try:
        process = subprocess.Popen(
            command, cwd=cwd, env=effective_env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace", bufsize=1,
        )
    except OSError as exc:
        raise BuildError(launch_error(command[0], exc, cwd=cwd)) from exc
    assert process.stdout is not None
    for line in process.stdout:
        log(line.rstrip())
    if process.wait() != 0:
        raise BuildError(f"Build command '{command[0]}' failed with exit code {process.returncode}. See the build log above.")


def windows_env() -> dict[str, str]:
    report = check_dependencies("windows")
    report.require()
    return report.env


def wsl_path(path: Path, report: DependencyReport) -> str:
    try:
        result = subprocess.run(
            [*report.launcher, "wslpath", "-a", str(path)], env=report.env,
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise BuildError(launch_error(report.tools["wsl"], exc)) from exc
    if result.returncode or not result.stdout.strip():
        raise BuildError(f"WSL could not convert the path '{path}'. Check that the default Linux distribution can access it.")
    return result.stdout.strip()


def prepare_openbios() -> Path:
    try:
        source = (PROJECT / "embedded_openbios.cpp").read_text(encoding="ascii")
    except (OSError, UnicodeError) as exc:
        raise BuildError(f"Cannot read the bundled OpenBIOS source: {exc}. Restore project/embedded_openbios.cpp.") from exc
    marker = "static const unsigned char kEmbeddedOpenBios[] = {"
    if marker not in source:
        raise BuildError("Embedded OpenBIOS source is missing.")
    array = source.split(marker, 1)[1].split("};", 1)[0]
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})\b", array))
    if len(data) != 524288 or hashlib.sha256(data).hexdigest() != OPENBIOS_SHA256:
        raise BuildError("Embedded OpenBIOS data failed its integrity check.")
    destination = PROJECT / "psxrecomp/bios/openbios.bin"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file() or destination.read_bytes() != data:
        destination.write_bytes(data)
    return destination


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def generator_dependencies(runtime: DependencyReport) -> DependencyReport:
    if runtime.host != "linux" or runtime.target != "windows":
        return runtime
    # Cross-compilers produce Windows executables, which cannot be run by the
    # Linux build host. The BIOS emitter must itself be a native host tool.
    env = runtime.env.copy()
    env["CC"] = env.get("BIOS_CC", "gcc")
    env["CXX"] = env.get("BIOS_CXX", "g++")
    for key in ("AR", "LD", "RANLIB"):
        env[key] = env.get("BIOS_" + key, key.lower())
    return check_dependencies("linux", environ=env, host="linux")


def openbios_fingerprint(env: dict[str, str]) -> str:
    framework = PROJECT / "psxrecomp"
    digest = hashlib.sha256(b"Build Studio OpenBIOS pipeline v1\n" + OPENBIOS_SHA256.encode())
    for name in ("PSX_CPS", "PSX_CODEGEN_CYCLE_PER_INSN"):
        digest.update((name + "=" + env.get(name, "") + "\n").encode())
    paths = [framework / "bios/OpenBIOS.toml", framework / "recompiler/seeds/openbios_elf_seeds.json"]
    for directory in (framework / "recompiler", framework / "runtime/include"):
        paths.extend(path for path in directory.rglob("*") if path.is_file() and
                     (path.suffix in {".cpp", ".c", ".h", ".hpp", ".inc", ".template", ".cmake"} or path.name == "CMakeLists.txt"))
    for path in sorted(set(paths)):
        digest.update(str(path.relative_to(framework)).replace("\\", "/").encode())
        digest.update(file_sha256(path).encode())
    return digest.hexdigest()


def validate_openbios_generated(directory: Path) -> dict[str, str]:
    hashes = {}
    for name in ("OpenBIOS_full.c", "OpenBIOS_dispatch.c"):
        path = directory / name
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise BuildError(f"OpenBIOS generation did not produce a readable {path}: {exc}.") from exc
        if f"BIOS SHA256: {OPENBIOS_SHA256}" not in source or '#include "cpu_state.h"' not in source:
            raise BuildError(f"Generated {name} has invalid OpenBIOS provenance or runtime headers. Regenerate it with Build Studio.")
        if name.endswith("dispatch.c") and not re.search(r"const\s+PsxBiosBackend\s+OpenBIOS_psx_bios_backend\s*=\s*\{", source):
            raise BuildError("OpenBIOS_dispatch.c does not define the backend descriptor required by runtime.cmake.")
        if name.endswith("full.c") and not re.search(r"void\s+OpenBIOS_\w+\s*\(CPUState\*", source):
            raise BuildError("OpenBIOS_full.c contains no generated OpenBIOS functions.")
        hashes[name] = file_sha256(path)
    return hashes


def prepare_openbios_backend(bios: Path, dependencies: DependencyReport, log: Log) -> None:
    framework = PROJECT / "psxrecomp"
    generated = framework / "generated"
    stamp = generated / ".build-studio-openbios.json"
    try:
        valid_rom = bios.stat().st_size == 524288 and file_sha256(bios) == OPENBIOS_SHA256
    except OSError as exc:
        raise BuildError(f"Cannot verify recovered OpenBIOS image {bios}: {exc}. Retry Build to recover it from the embedded source.") from exc
    if not valid_rom:
        raise BuildError("OpenBIOS binary failed SHA-256 validation. Restore the embedded source and retry Build.")
    generator = generator_dependencies(dependencies)
    generator.require()
    try:
        fingerprint = openbios_fingerprint(generator.env)
    except OSError as exc:
        raise BuildError(f"Cannot read OpenBIOS generator inputs: {exc}. Restore the profile, seeds and emitter sources, then retry Build.") from exc
    try:
        cached = json.loads(stamp.read_text(encoding="utf-8"))
        if cached["inputs"] == fingerprint and cached["outputs"] == validate_openbios_generated(generated):
            log("Reusing verified generated OpenBIOS backend (ROM, profile, seeds and emitter unchanged).")
            return
    except (OSError, ValueError, KeyError, TypeError, BuildError):
        pass
    log("Preparing the MIT-licensed OpenBIOS backend; first build compiles the BIOS generator.")
    mode = "wsl-linux" if generator.launcher else generator.target
    # Separate native toolchain caches: CMake can discard command-line options
    # when an existing cache switches compilers. Keep that from re-enabling CHD.
    toolchain_key = hashlib.sha256((generator.tools["gcc"] + "\n" + generator.tools["g++"]).encode()).hexdigest()[:10]
    build_dir = ROOT / "work" / ("bios-generator-" + mode + "-" + toolchain_key)
    build_dir.mkdir(parents=True, exist_ok=True)

    def tool_path(path: Path) -> str:
        return wsl_path(path, generator) if generator.launcher else str(path)

    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
              "-DPSXRECOMP_ENABLE_CHD=OFF", "-DCMAKE_TOOLCHAIN_FILE=",
              f"-DCMAKE_MAKE_PROGRAM={generator.tools['ninja']}",
              f"-DCMAKE_C_COMPILER={generator.tools['gcc']}",
              f"-DCMAKE_CXX_COMPILER={generator.tools['g++']}",
              f"-DCMAKE_AR={generator.tools['ar']}",
              f"-DCMAKE_LINKER={generator.tools['ld']}",
              f"-DCMAKE_RANLIB={generator.tools['ranlib']}"]
    if generator.target == "windows":
        common.append("-DPSXRECOMP_STATIC_CLI=ON")
    work = ROOT / "work"
    try:
        log("Configuring BIOS code generator (C++20, bundled fmt/Rabbitizer, no CHD download)...")
        run([*generator.launcher, generator.tools["cmake"], "-S", tool_path(framework / "recompiler"),
             "-B", tool_path(build_dir), *common], log, cwd=ROOT, env=generator.env)
        log("Building psxrecomp-bios and generating Rabbitizer headers...")
        run([*generator.launcher, generator.tools["cmake"], "--build", tool_path(build_dir),
             "--target", "psxrecomp-bios", "--parallel", "4"], log, cwd=ROOT, env=generator.env)
        suffix = ".exe" if generator.target == "windows" else ""
        executable = build_dir / ("psxrecomp-bios" + suffix)
        if not executable.is_file():
            raise BuildError(f"BIOS generator executable was not produced: {executable}.")
        with tempfile.TemporaryDirectory(prefix="openbios-", dir=work) as staging:
            stage = Path(staging)
            log("Generating OpenBIOS C from the pinned profile and verified ROM...")
            run([*generator.launcher, tool_path(executable), "--config", tool_path(framework / "bios/OpenBIOS.toml"),
                 "--rom", tool_path(bios), "--out-dir", tool_path(stage)], log, cwd=ROOT, env=generator.env)
            hashes = validate_openbios_generated(stage)
            log("Checking generated OpenBIOS C against the runtime headers...")
            # Validate with the target compiler as well as the emitter's identity
            # gate. The runtime build later performs the actual compile/link.
            for name in hashes:
                source = wsl_path(stage / name, dependencies) if dependencies.launcher else str(stage / name)
                include = wsl_path(framework / "runtime/include", dependencies) if dependencies.launcher else str(framework / "runtime/include")
                run([*dependencies.launcher, dependencies.tools["gcc"], "-std=c99", "-fsyntax-only", "-I", include, source],
                    log, cwd=ROOT, env=dependencies.env)
            generated.mkdir(parents=True, exist_ok=True)
            stamp.unlink(missing_ok=True)
            for path in stage.glob("OpenBIOS_*"):
                destination = generated / path.name
                if not destination.is_file() or file_sha256(destination) != file_sha256(path):
                    path.replace(destination)
            stamp_tmp = generated / ".build-studio-openbios.tmp"
            stamp_tmp.write_text(json.dumps({"inputs": fingerprint, "outputs": hashes}, indent=2), encoding="utf-8")
            stamp_tmp.replace(stamp)
    except (BuildError, OSError) as exc:
        raise BuildError(f"OpenBIOS preparation failed: {exc}\nCheck the BIOS stage in the build log, restore missing project sources, "
                         "and use a C++20-capable GCC/G++ toolchain. Then click Retry Check and Build again.") from exc
    log("OpenBIOS backend verified and ready for runtime compilation.")


def build_commands(target: str, build_dir: Path, bios: Path,
                   dependencies: DependencyReport | None = None) -> tuple[list[str], list[str], dict[str, str]]:
    report = dependencies if dependencies is not None else check_dependencies(target)
    report.require()
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPSX_SDL_BACKEND=SDL3",
              f"-DPSX_DEBUG_TOOLS={'OFF' if target == 'linux' else 'ON'}",
              f"-DCMAKE_MAKE_PROGRAM={report.tools['ninja']}",
              f"-DCMAKE_C_COMPILER={report.tools['gcc']}",
              f"-DCMAKE_CXX_COMPILER={report.tools['g++']}",
              f"-DCMAKE_AR={report.tools['ar']}",
              f"-DCMAKE_LINKER={report.tools['ld']}",
              f"-DCMAKE_RANLIB={report.tools['ranlib']}"]
    if target == "windows":
        common.append(f"-DCMAKE_RC_COMPILER={report.tools['windres']}")
    if report.host == "linux" and target == "windows":
        common.append(f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'toolchains/mingw64.cmake'}")
    source, build, bios_source = str(PROJECT), str(build_dir), str(bios)
    if report.launcher:
        source, build, bios_source = (wsl_path(path, report) for path in (PROJECT, build_dir, bios))
    cmake = report.tools["cmake"]
    return ([*report.launcher, cmake, "-S", source, "-B", build, *common,
             f"-DPSXRECOMP_BUNDLED_BIOS_SOURCE={bios_source}"],
            [*report.launcher, cmake, "--build", build, "--parallel", "4"], report.env)


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


def package(executable: Path, target: str, cue: Path, tracks: list[Path], output_root: Path, log: Log,
            dependencies: DependencyReport | None = None) -> Path:
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
        if target == "windows":
            stage_windows_dependencies(executable, temporary, dependencies, log, packaged_name=output_exe.name)
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
        bios_dir = temporary / "bios"
        bios_dir.mkdir()
        shutil.copy2(prepare_openbios(), bios_dir / "openbios.bin")
        shutil.copy2(PROJECT / "psxrecomp/bios/OpenBIOS.LICENSE", bios_dir / "OpenBIOS.LICENSE")
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
    log("Checking build tools...")
    dependencies = check_build_dependencies(target)
    for line in dependencies.lines():
        log(line)
    dependencies.require()
    if not (PROJECT / "generated/SCES_001.63_dispatch.c").is_file():
        raise BuildError("The generated source snapshot is incomplete.")
    validate_assets()
    cue, tracks = disc_files(disc_folder)
    log(f"Verified disc: {cue.name} ({len(tracks)} tracks)")
    build_dir = ROOT / "work" / f"build-{target}"
    build_dir.mkdir(parents=True, exist_ok=True)
    log("Recovering and verifying the bundled OpenBIOS ROM...")
    bios = prepare_openbios()
    prepare_openbios_backend(bios, dependencies, log)
    configure, compile_cmd, env = build_commands(target, build_dir, bios, dependencies)
    log("Configuring native runtime...")
    run(configure, log, cwd=ROOT, env=env)
    log("Compiling and linking the complete game...")
    run(compile_cmd, log, cwd=ROOT, env=env)
    executable = find_executable(build_dir, target)
    log(f"Linked executable: {executable.name}")
    final = package(executable, target, cue, tracks, output_root, log, dependencies)
    log(f"Ready: {final}")
    return final


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a local game package from a user-owned disc")
    parser.add_argument("--target", choices=("windows", "linux"), required=True)
    parser.add_argument("--check-dependencies", action="store_true", help="Check build tools without a disc or compilation")
    parser.add_argument("--wine-smoke", action="store_true", help="Optional Wine headless startup check after a Linux-to-Windows build")
    parser.add_argument("--disc-folder", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.check_dependencies:
            report = check_build_dependencies(args.target)
            for line in report.lines():
                print(line)
            report.require()
        else:
            if args.disc_folder is None or args.output is None:
                parser.error("--disc-folder and --output are required for a build")
            final = build(args.target, args.disc_folder, args.output)
            if args.wine_smoke:
                if args.target != "windows" or os.name == "nt":
                    raise BuildError("--wine-smoke is available for Windows packages on Linux.")
                from wine_validation import wine_smoke
                wine_smoke(final, ROOT / "work/wine-check")
    except BuildError as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
