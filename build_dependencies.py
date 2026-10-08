"""Dependency discovery and launch diagnostics shared by the GUI and CLI."""
from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path


class BuildError(RuntimeError):
    pass


WINDOWS_INSTALL = (
    "In the MSYS2 MINGW64 terminal run: pacman -S --needed "
    "mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja. "
    "Then restart Build Studio and click Retry Check."
)
HELP = {
    "python": "Install Python 3.10 or newer with Tcl/Tk from https://www.python.org/downloads/windows/.",
    "cmake": "Install CMake 3.20 or newer from https://cmake.org/download/ and add its bin folder to PATH, "
             "or set CMAKE to the full executable path. Then restart Build Studio. " + WINDOWS_INSTALL,
    "ninja": "Install Ninja and add its folder to PATH, or set NINJA to the full executable path. " + WINDOWS_INSTALL,
    "gcc": "Install the 64-bit GCC C compiler. Check CC and MSYS2_ROOT. " + WINDOWS_INSTALL,
    "g++": "Install the 64-bit GCC C++ compiler. Check CXX and MSYS2_ROOT. " + WINDOWS_INSTALL,
    "msys2": "Install MSYS2 from https://www.msys2.org/. For a custom installation set MSYS2_ROOT "
             "to its Windows folder (for example D:\\Tools\\msys64), then restart Build Studio.",
    "wsl": "Install WSL with a Linux distribution, then install python3, cmake, ninja-build, "
           "gcc, g++, pkg-config, libcurl4-openssl-dev and libgl1-mesa-dev inside that distribution.",
    "pkg-config": "Install pkg-config and the Linux development packages for libcurl and OpenGL.",
    "libcurl": "Install the Linux libcurl development package (Ubuntu/Debian: libcurl4-openssl-dev).",
    "OpenGL": "Install the Linux OpenGL development package (Ubuntu/Debian: libgl1-mesa-dev).",
}
LABELS = {"python": "Python", "cmake": "CMake", "ninja": "Ninja", "gcc": "GCC", "g++": "G++", "msys2": "MSYS2", "wsl": "WSL"}


def install_help(tool: str, host: str | None = None, target: str | None = None) -> str:
    host = host or platform.system().lower()
    if host == "linux" and tool in {"cmake", "ninja", "gcc", "g++", "python", "windres", "ar", "ld", "ranlib"}:
        packages = "cmake ninja-build gcc g++ binutils python3 python3-tk"
        if target == "windows":
            packages += " gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64"
        fedora = "sudo dnf install python3 python3-tkinter cmake ninja-build gcc gcc-c++ binutils"
        if target == "windows":
            fedora += " mingw64-gcc mingw64-gcc-c++ mingw64-binutils mingw64-headers mingw64-crt mingw64-winpthreads-static mingw64-zlib-static"
        return (f"Install {LABELS.get(tool, tool)} with your distribution's package manager "
                f"(Fedora/Nobara: {fedora}; Ubuntu/Debian: {packages}). Check PATH and configured tool paths.")
    return HELP.get(tool, "Install the MinGW-w64 binutils build tools. " + WINDOWS_INSTALL)


def launch_error(executable: str, exc: BaseException, *, cwd: Path | None = None) -> str:
    name = Path(executable).name.lower().removesuffix(".exe")
    if name.startswith("x86_64-w64-mingw32-"):
        name = name.removeprefix("x86_64-w64-mingw32-")
    if cwd is not None and not cwd.is_dir():
        return f"Cannot start '{executable}': build working folder does not exist: {cwd}."
    return (f"Cannot start {LABELS.get(name, name)} executable '{executable}': {exc}. "
            "Check that the file and its runtime DLLs exist and are accessible. " + install_help(name))


@dataclass
class Dependency:
    name: str
    path: str | None
    detail: str
    required: bool = True
    ok: bool = True

    def line(self) -> str:
        state = "FOUND" if self.ok else ("MISSING / UNUSABLE" if self.required else "CHECK IN CMAKE")
        return f"[{state}] {LABELS.get(self.name, self.name)}: {self.path or 'not found'} — {self.detail}"


@dataclass
class DependencyReport:
    host: str
    target: str
    env: dict[str, str]
    dependencies: list[Dependency] = field(default_factory=list)
    tools: dict[str, str] = field(default_factory=dict)
    launcher: list[str] = field(default_factory=list)

    @property
    def ready(self) -> bool:
        return all(item.ok or not item.required for item in self.dependencies)

    def lines(self) -> list[str]:
        return [item.line() for item in self.dependencies]

    def require(self) -> None:
        failures = [item.line() for item in self.dependencies if item.required and not item.ok]
        if failures:
            raise BuildError("Build tools need attention. Compilation has not started.\n" + "\n".join(failures))


def _resolve(name: str, env: dict[str, str], directories: list[Path], *, windows: bool,
             configured: str | None = None) -> str | None:
    """Explicit settings are authoritative; never silently replace a bad setting."""
    if configured:
        configured = configured.strip().strip('"')
        path = Path(configured).expanduser()
        if path.is_file():
            return str(path.resolve())
        found = shutil.which(configured, path=env.get("PATH", ""))
        return str(Path(found).resolve()) if found else None
    suffix = ".exe" if windows else ""
    found = shutil.which(name + suffix, path=env.get("PATH", ""))
    if found:
        return str(Path(found).resolve())
    for directory in directories:
        path = directory / (name + suffix)
        if path.is_file():
            return str(path.resolve())
    return None


def _windows_locations(env: dict[str, str]) -> tuple[list[Path], list[Path]]:
    roots: list[Path] = []
    if env.get("MSYS2_ROOT"):
        roots.append(Path(env["MSYS2_ROOT"].strip('"')).expanduser())
    else:
        for key in ("MINGW_PREFIX", "MSYSTEM_PREFIX"):
            value = env.get(key, "")
            if value and not value.startswith("/"):
                roots.append(Path(value).parent)
        # Infer a custom installation from PATH or explicit compiler settings.
        for value in (env.get("CC", ""), env.get("CXX", ""),
                      shutil.which("gcc.exe", path=env.get("PATH", "")) or ""):
            path = Path(value.strip('"'))
            if path.parent.name.lower() == "bin" and path.parent.parent.name.lower() in {"mingw64", "ucrt64"}:
                roots.append(path.parent.parent.parent)
        roots.extend([Path(env.get("SYSTEMDRIVE", "C:") + "/msys64"), Path("C:/msys64"), Path("C:/tools/msys64")])
        for key in ("PROGRAMFILES", "LOCALAPPDATA"):
            if env.get(key):
                roots.append(Path(env[key]) / "msys64")
    roots = list(dict.fromkeys(roots))
    bins: list[Path] = []
    for key in ("MINGW_PREFIX", "MSYSTEM_PREFIX"):
        value = env.get(key, "")
        if value:
            # MSYS shells may export /mingw64; map it to the selected Windows root.
            bins.append((roots[0] / value.lstrip("/\\") if value.startswith("/") else Path(value)) / "bin")
    for root in roots:
        bins.extend([root / "mingw64/bin", root / "ucrt64/bin"])
    return roots, list(dict.fromkeys(bins))


def check_dependencies(target: str, *, environ: dict[str, str] | None = None,
                       host: str | None = None) -> DependencyReport:
    host = (host or platform.system()).lower()
    env = dict(os.environ if environ is None else environ)
    if host == "windows":
        env = {key.upper(): value for key, value in env.items()}
    report = DependencyReport(host, target, env)
    if target not in {"windows", "linux"} or host not in {"windows", "linux"}:
        report.dependencies.append(Dependency("platform", None, f"Unsupported host/target: {host}/{target}.", ok=False))
        return report
    python_ok = sys.version_info >= (3, 10) and Path(sys.executable).is_file()
    report.dependencies.append(Dependency("python", sys.executable, platform.python_version() if python_ok else HELP["python"], ok=python_ok))
    windows = host == "windows"
    bins: list[Path] = []
    if windows and target == "windows":
        roots, bins = _windows_locations(env)
        root = next((p for p in roots if (p / "usr/bin/bash.exe").is_file()), None)
        report.dependencies.append(Dependency("msys2", str(root) if root else env.get("MSYS2_ROOT"),
                                             "Installation found" if root else HELP["msys2"], ok=root is not None))
        # Explicit MSYS2_ROOT owns compiler discovery, even if another MSYS2 is in PATH.
        if env.get("MSYS2_ROOT"):
            bins = [p for p in bins if p.is_relative_to(roots[0])]
        preferred = env.get("CC") or (None if env.get("MSYS2_ROOT") else shutil.which("gcc.exe", path=env.get("PATH", "")))
        if preferred:
            resolved = _resolve("gcc", env, bins, windows=True, configured=preferred)
            if resolved:
                compiler_bin = Path(resolved).parent
                bins = [compiler_bin, *[p for p in bins if p != compiler_bin]]
        for directory in bins:
            if (directory / "gcc.exe").is_file():
                env["PATH"] = str(directory) + ";" + env.get("PATH", "")
                break
    tool_dirs = list(bins)
    if windows:
        tool_dirs.extend([Path(env.get("PROGRAMFILES", "C:/Program Files")) / "CMake/bin",
                          Path(env.get("PROGRAMFILES(X86)", "C:/Program Files (x86)")) / "CMake/bin"])
        for variable in ("CMAKE_ROOT", "NINJA_ROOT"):
            if env.get(variable):
                tool_dirs.extend([Path(env[variable]), Path(env[variable]) / "bin"])
    if windows and target == "linux":
        wsl = _resolve("wsl", env, [Path(env.get("SYSTEMROOT", "C:/Windows")) / "System32"], windows=True)
        if not wsl:
            report.dependencies.append(Dependency("wsl", None, HELP["wsl"], ok=False))
            return report
        report.tools["wsl"] = wsl
        report.launcher = [wsl, "--exec"]
        try:
            result = subprocess.run([*report.launcher, "sh", "-c", "command -v sh"], env=env,
                                    capture_output=True, text=True, timeout=30)
            ok = result.returncode == 0 and bool(result.stdout.strip())
            report.dependencies.append(Dependency("wsl", wsl, "Default Linux distribution ready" if ok else HELP["wsl"], ok=ok))
            if not ok:
                return report
        except (OSError, subprocess.SubprocessError) as exc:
            report.dependencies.append(Dependency("wsl", wsl, f"{exc}. {HELP['wsl']}", ok=False))
            return report
    cross = host == "linux" and target == "windows"
    names = {name: name for name in ("cmake", "ninja", "gcc", "g++", "ar", "ld", "ranlib")}
    if report.launcher:
        names["python"] = "python3"
    if target == "windows":
        names["windres"] = "windres"
        if cross:
            names.update({key: "x86_64-w64-mingw32-" + key for key in ("gcc", "g++", "windres", "ar", "ld", "ranlib")})
    else:
        names["pkg-config"] = "pkg-config"
    settings = {"cmake": "CMAKE", "ninja": "NINJA", "gcc": "CC", "g++": "CXX", "windres": "RC", "ar": "AR", "ld": "LD", "ranlib": "RANLIB"}
    for key, name in names.items():
        path: str | None = None
        configured = env.get(settings.get(key, "")) if not report.launcher else None
        required = key != "pkg-config"
        try:
            if report.launcher:
                result = subprocess.run([*report.launcher, "sh", "-c", 'command -v "$1"', "sh", name],
                                        env=env, capture_output=True, text=True, timeout=30)
                path = result.stdout.strip() if result.returncode == 0 else None
            else:
                search_dirs = tool_dirs
                search_env = env
                if windows and target == "windows" and key in {"gcc", "g++", "windres", "ar", "ld", "ranlib"}:
                    # Select all helpers from the chosen compiler installation.
                    search_dirs = [Path(report.tools["gcc"]).parent] if "gcc" in report.tools else bins
                    search_env = {**env, "PATH": ""}
                    if not configured:
                        path = _resolve(name, search_env, search_dirs, windows=True)
                    else:
                        path = _resolve(name, env, search_dirs, windows=True, configured=configured)
                else:
                    path = _resolve(name, search_env, search_dirs, windows=windows, configured=configured)
            if not path:
                detail = f"{LABELS.get(key, key)} was not found (executable: {configured or name}). " + install_help(key, "linux" if report.launcher else host, target)
                report.dependencies.append(Dependency(key, None, detail, required=required, ok=False))
                continue
            args = ["-dumpmachine"] if key in {"gcc", "g++"} else ["--version"]
            result = subprocess.run([*report.launcher, path, *args], env=env, capture_output=True, text=True, timeout=15)
            output = (result.stdout or result.stderr).strip()
            detail = output.splitlines()[0] if output else "No version output"
            ok = result.returncode == 0 and bool(output)
            if key in {"cmake", "python"} and ok:
                version = re.search(r"(\d+)\.(\d+)", output)
                minimum = (3, 20) if key == "cmake" else (3, 10)
                ok = version is not None and tuple(map(int, version.groups())) >= minimum
                if not ok:
                    detail += f"; {LABELS[key]} {minimum[0]}.{minimum[1]} or newer is required"
            if key in {"gcc", "g++"} and ok:
                ok = ("x86_64" in output and ("mingw" in output if target == "windows" else "linux" in output))
                if not ok:
                    detail += f"; expected a {target} x86_64 compiler (use MINGW64 or UCRT64 for Windows)"
            if windows and target == "windows" and key in {"cmake", "ninja"}:
                if Path(path).parent.parent.name.lower() == "usr":
                    ok = False
                    detail += "; use a native Windows/MinGW tool, not the MSYS usr/bin version"
            if not ok:
                detail += f" (probe exit code {result.returncode}). " + install_help(key, "linux" if report.launcher else host, target)
            report.dependencies.append(Dependency(key, path, detail, required=required, ok=ok))
            if ok:
                report.tools[key] = path
                if not report.launcher:
                    # Ensure compiler helpers and tool DLLs are visible to every child.
                    parent = str(Path(path).parent)
                    separator = ";" if windows else os.pathsep
                    if parent not in env.get("PATH", "").split(separator):
                        env["PATH"] = parent + separator + env.get("PATH", "")
        except (OSError, subprocess.SubprocessError) as exc:
            report.dependencies.append(Dependency(key, path, f"Cannot run {name}: {exc}. " + install_help(key, "linux" if report.launcher else host, target), required=required, ok=False))
    if target == "windows" and {"gcc", "g++"} <= report.tools.keys():
        if Path(report.tools["gcc"]).parent != Path(report.tools["g++"]).parent:
            report.dependencies.append(Dependency("toolchain", None, "CC and CXX must select the same MinGW installation. Check both settings.", ok=False))
    if target == "linux" and "pkg-config" in report.tools:
        for name, module in (("libcurl", "libcurl"), ("OpenGL", "gl")):
            try:
                result = subprocess.run([*report.launcher, report.tools["pkg-config"], "--modversion", module],
                                        env=env, capture_output=True, text=True, timeout=15)
                ok = result.returncode == 0
                report.dependencies.append(Dependency(name, module, result.stdout.strip() if ok else HELP[name] + " CMake will also search configured library locations.", required=False, ok=ok))
            except (OSError, subprocess.SubprocessError) as exc:
                report.dependencies.append(Dependency(name, module, f"{exc}. {HELP[name]} CMake will also search configured library locations.", required=False, ok=False))
    if target == "windows" and {"gcc", "g++"} <= report.tools.keys():
        # Compile and link, never execute: this also verifies the Windows CRT,
        # C++20 headers and the static pthread runtime needed by runtime.cmake.
        try:
            sysroot = env.get("MINGW_SYSROOT") if cross else None
            if sysroot and not Path(sysroot).is_dir():
                raise BuildError(f"MINGW_SYSROOT does not exist: {sysroot}")
            with tempfile.TemporaryDirectory(prefix="Build Studio Windows probe ") as folder:
                command = [report.tools["g++"], "-x", "c++", "-", "-std=c++20", "-static",
                           "-o", str(Path(folder) / "probe.exe"), "-lwinpthread"]
                if sysroot:
                    command.append("--sysroot=" + sysroot)
                result = subprocess.run(command, input='#include <windows.h>\n#include <thread>\n#include <filesystem>\nint main(){std::thread t([]{});t.join();return GetCurrentProcessId()==0;}\n',
                                        env=env, capture_output=True, text=True, timeout=60)
                ok = result.returncode == 0
                detail = "Windows CRT, C++20 and static pthread compile/link succeeded" if ok else result.stderr.strip()
                if not ok:
                    detail += "\n" + install_help("g++", host, target)
                report.dependencies.append(Dependency("Windows target libraries", report.tools["g++"], detail, ok=ok))
        except (BuildError, OSError, subprocess.SubprocessError) as exc:
            report.dependencies.append(Dependency("Windows target libraries", None,
                f"{exc}. " + install_help("g++", host, target), ok=False))
    if not report.launcher:
        for key, variable in (("gcc", "CC"), ("g++", "CXX")):
            if key in report.tools:
                env[variable] = report.tools[key]
    return report
