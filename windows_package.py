"""Validate Windows x64 PE imports and stage the non-system DLL closure."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
from pathlib import Path
from typing import Callable

from build_dependencies import BuildError, DependencyReport

# Imports supplied by Windows itself. API-set contracts resolve through the OS.
SYSTEM_DLLS = set("""advapi32 avrt bcrypt cabinet cfgmgr32 comctl32 comdlg32 crypt32
cryptbase d3d9 d3d11 d3d12 d3dcompiler_47 dbghelp dinput8 dnsapi dwmapi dxgi gdi32
hid imm32 iphlpapi kernel32 kernelbase mpr msvcrt msimg32 ncrypt netapi32 normaliz
ntdll ole32 oleaut32 opengl32 powrprof propsys psapi rpcrt4 secur32 setupapi shell32
shcore shlwapi ucrtbase user32 userenv usp10 uxtheme version winhttp wininet winmm
winspool wintrust ws2_32 wtsapi32 xinput1_4 xinput9_1_0""".split())


def system_dll(name: str) -> bool:
    stem = name.casefold().removesuffix(".dll")
    return stem in SYSTEM_DLLS or stem.startswith(("api-ms-win-", "ext-ms-win-"))


def pe_imports(path: Path) -> list[str]:
    """Read normal and delay imports, rejecting non-AMD64/non-PE32+ binaries."""
    try:
        data = path.read_bytes()
        def unpack(fmt: str, offset: int):
            return struct.unpack_from(fmt, data, offset)
        if data[:2] != b"MZ":
            raise ValueError("missing MZ header")
        pe = unpack("<I", 0x3c)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("missing PE header")
        machine, sections = unpack("<HH", pe + 4)
        optional_size = unpack("<H", pe + 20)[0]
        optional = pe + 24
        if machine != 0x8664 or unpack("<H", optional)[0] != 0x20b:
            raise ValueError("expected Windows x86_64 PE32+")
        if optional_size < 112 or optional + optional_size > len(data):
            raise ValueError("truncated optional header")
        image_base = unpack("<Q", optional + 24)[0]
        header_size = unpack("<I", optional + 60)[0]
        directory_count = unpack("<I", optional + 108)[0]
        section_table = optional + optional_size
        def offset(rva: int) -> int:
            if rva < header_size and rva < len(data):
                return rva
            for index in range(sections):
                entry = section_table + index * 40
                virtual_size, address, raw_size, raw = unpack("<IIII", entry + 8)
                if address <= rva < address + max(virtual_size, raw_size):
                    delta = rva - address
                    if delta >= raw_size or raw + delta >= len(data):
                        break
                    return raw + delta
            raise ValueError(f"invalid import RVA {rva:#x}")
        names = set()
        for directory, stride, name_position in ((1, 20, 12), (13, 32, 4)):
            if directory_count <= directory:
                continue
            entry = optional + 112 + directory * 8
            if entry + 8 > optional + optional_size:
                raise ValueError("truncated import directory")
            rva, size = unpack("<II", entry)
            if not rva:
                continue
            for index in range(min(size // stride, 4096)):
                descriptor = offset(rva + index * stride)
                record = data[descriptor:descriptor + stride]
                if len(record) != stride:
                    raise ValueError("truncated import descriptor")
                if not any(record):
                    break
                name_rva = unpack("<I", descriptor + name_position)[0]
                if directory == 13 and not (unpack("<I", descriptor)[0] & 1):
                    name_rva -= image_base
                start = offset(name_rva)
                end = data.find(b"\0", start, start + 260)
                if end < 0:
                    raise ValueError("unterminated DLL name")
                name = data[start:end].decode("ascii")
                if not name.lower().endswith(".dll") or Path(name).name != name or "/" in name or "\\" in name:
                    raise ValueError(f"invalid DLL name {name!r}")
                names.add(name)
            else:
                raise ValueError("unterminated import descriptors")
        return sorted(names, key=str.casefold)
    except (OSError, ValueError, struct.error) as exc:
        raise BuildError(f"Cannot validate Windows executable/DLL {path}: {exc}.") from exc


def dll_directories(executable: Path, report: DependencyReport | None) -> list[Path]:
    directories = [executable.parent]
    if report:
        env = report.env
        directories.extend(Path(value) for value in env.get("WINDOWS_DLL_PATH", "").split(os.pathsep) if value)
        for key in ("gcc", "g++"):
            if key in report.tools:
                bin_dir = Path(report.tools[key]).parent
                prefix = bin_dir.parent
                directories.extend([bin_dir, prefix / "bin",
                    prefix / "x86_64-w64-mingw32/bin",
                    prefix / "x86_64-w64-mingw32/sys-root/mingw/bin"])
        if env.get("MINGW_SYSROOT"):
            root = Path(env["MINGW_SYSROOT"])
            directories.extend([root / "bin", root / "mingw/bin"])
    return list(dict.fromkeys(directories))


def stage_windows_dependencies(executable: Path, destination: Path,
                               report: DependencyReport | None, log: Callable[[str], None],
                               *, packaged_name: str | None = None) -> None:
    directories = dll_directories(executable, report)
    available = {}
    for directory in directories:
        if directory.is_dir():
            for path in directory.iterdir():
                if path.is_file() and path.suffix.lower() == ".dll":
                    available.setdefault(path.name.casefold(), path)
    queue = [executable]
    seen = set()
    manifest = {}
    while queue:
        binary = queue.pop(0)
        key = binary.name.casefold()
        if key in seen:
            continue
        seen.add(key)
        imports = pe_imports(binary)
        name_in_package = packaged_name if binary == executable and packaged_name else binary.name
        manifest[name_in_package] = {"sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "imports": imports}
        for name in imports:
            if system_dll(name) or name.casefold() in seen:
                continue
            source = available.get(name.casefold())
            if source is None:
                raise BuildError(f"Windows package needs {name}, imported by {binary.name}, but it was not found. "
                                 "Install its Windows x64 runtime/development package or set WINDOWS_DLL_PATH "
                                 "to the folder containing the DLL. Searched: " + ", ".join(map(str, directories)))
            target = destination / source.name
            if not target.exists():
                shutil.copy2(source, target)
                log(f"Bundling Windows runtime DLL: {source.name}")
            queue.append(target)
    (destination / "windows-imports.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    log(f"Validated Windows x64 PE imports: {len(manifest)} binaries; all non-system DLLs packaged.")
