from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_dependencies import BuildError, check_dependencies
from builder_core import build, build_commands, run, wsl_path


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="Build tools with spaces ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "Custom MSYS2"
        self.bin = self.root / "mingw64/bin"
        self.bin.mkdir(parents=True)
        bash = self.root / "usr/bin/bash.exe"
        bash.parent.mkdir(parents=True)
        bash.touch()
        self.env = {"PATH": "", "MSYS2_ROOT": str(self.root),
                    "ProgramFiles": str(Path(self.temp.name) / "Program Files"),
                    "ProgramFiles(x86)": str(Path(self.temp.name) / "Program Files x86")}
        self.executables = {name: self.bin / (name + ".exe")
                            for name in ("cmake", "ninja", "gcc", "g++", "windres", "ar", "ld", "ranlib")}
        for path in self.executables.values():
            path.touch()
        self.which = mock.patch("build_dependencies.shutil.which", return_value=None).start()
        self.addCleanup(mock.patch.stopall)
        self.probe = mock.patch("build_dependencies.subprocess.run", side_effect=self.probe_result).start()

    def probe_result(self, command, **_kwargs):
        name = Path(command[0]).name
        if "-dumpmachine" in command:
            output = "x86_64-w64-mingw32\n"
        elif name == "cmake.exe":
            output = "cmake version 3.29.0\n"
        else:
            output = "1.12.0\n"
        return subprocess.CompletedProcess(command, 0, output, "")

    def check(self):
        return check_dependencies("windows", environ=self.env, host="windows")

    def test_successful_validation_and_environment(self):
        report = self.check()
        self.assertTrue(report.ready, report.lines())
        self.assertEqual(report.env["CC"], str(self.executables["gcc"]))
        self.assertEqual(report.env["CXX"], str(self.executables["g++"]))
        self.assertIn(str(self.bin), report.env["PATH"].split(";"))
        self.assertIn(str(self.root), "\n".join(report.lines()))
        self.assertEqual(self.env["PATH"], "")  # caller environment is not mutated

    def test_missing_cmake(self):
        self.executables["cmake"].unlink()
        report = self.check()
        self.assertFalse(report.ready)
        with self.assertRaisesRegex(BuildError, "CMake was not found") as error:
            report.require()
        self.assertIn("Install CMake", str(error.exception))
        self.assertIn("restart Build Studio", str(error.exception))

    def test_missing_ninja(self):
        self.executables["ninja"].unlink()
        with self.assertRaisesRegex(BuildError, "Ninja was not found") as error:
            self.check().require()
        self.assertIn("pacman", str(error.exception))

    def test_missing_gxx_is_not_hidden_by_gcc(self):
        self.executables["g++"].unlink()
        report = self.check()
        self.assertIn("gcc", report.tools)
        with self.assertRaisesRegex(BuildError, r"G\+\+ was not found"):
            report.require()

    def test_missing_compilers_are_all_reported(self):
        for name in ("gcc", "g++"):
            self.executables[name].unlink()
        with self.assertRaises(BuildError) as error:
            self.check().require()
        self.assertIn("GCC was not found", str(error.exception))
        self.assertIn("G++ was not found", str(error.exception))
        self.assertIn("CXX", str(error.exception))

    def test_invalid_msys2_root_is_not_ignored(self):
        self.env["MSYS2_ROOT"] = str(self.root / "does not exist")
        with self.assertRaisesRegex(BuildError, "MSYS2_ROOT"):
            self.check().require()

    def test_cmake_in_program_files(self):
        path = Path(self.env["ProgramFiles"]) / "CMake/bin/cmake.exe"
        path.parent.mkdir(parents=True)
        self.executables["cmake"].rename(path)
        self.assertEqual(self.check().tools["cmake"], str(path))

    def test_windows_environment_keys_are_case_insensitive(self):
        env = {key.lower(): value for key, value in self.env.items()}
        report = check_dependencies("windows", environ=env, host="windows")
        self.assertTrue(report.ready, report.lines())
        self.assertEqual(report.env["MSYS2_ROOT"], str(self.root))

    def test_tools_in_arbitrary_directories(self):
        for name, variable in (("cmake", "CMAKE"), ("ninja", "NINJA")):
            path = Path(self.temp.name) / "Other tools" / (name + ".exe")
            path.parent.mkdir(exist_ok=True)
            self.executables[name].rename(path)
            self.env[variable] = str(path)
        report = self.check()
        self.assertTrue(report.ready, report.lines())
        self.assertEqual(report.tools["cmake"], self.env["CMAKE"])
        self.assertEqual(report.tools["ninja"], self.env["NINJA"])

    def test_bad_explicit_cmake_setting_does_not_use_default(self):
        self.env["CMAKE"] = str(self.root / "wrong cmake.exe")
        report = self.check()
        self.assertFalse(report.ready)
        self.assertIn("wrong cmake.exe", "\n".join(report.lines()))

    def test_path_and_compiler_settings_infer_custom_msys2(self):
        del self.env["MSYS2_ROOT"]
        self.which.side_effect = lambda name, **_kwargs: str(self.executables["gcc"]) if name == "gcc.exe" else None
        self.assertTrue(self.check().ready)
        self.which.side_effect = None
        self.env.update(CC=str(self.executables["gcc"]), CXX=str(self.executables["g++"]))
        self.assertTrue(self.check().ready)

    def test_msys_shell_prefix_maps_to_windows_root(self):
        self.env["MINGW_PREFIX"] = "/mingw64"
        self.assertTrue(self.check().ready)

    def test_ucrt64_installation(self):
        ucrt = self.root / "ucrt64/bin"
        ucrt.parent.mkdir()
        self.bin.rename(ucrt)
        self.env["MINGW_PREFIX"] = str(ucrt.parent)
        self.assertTrue(self.check().ready)

    def test_msys_cmake_is_rejected_for_native_windows_build(self):
        path = self.root / "usr/bin/cmake.exe"
        path.touch()
        self.env["CMAKE"] = str(path)
        with self.assertRaisesRegex(BuildError, "native Windows/MinGW tool"):
            self.check().require()

    def test_linux_library_metadata_does_not_block_custom_cmake_locations(self):
        self.which.side_effect = lambda name, **_kwargs: "/tools/" + name
        def probe(command, **_kwargs):
            if "--modversion" in command:
                return subprocess.CompletedProcess(command, 1, "", "not found")
            output = "x86_64-linux-gnu" if "-dumpmachine" in command else "cmake version 3.29.0"
            return subprocess.CompletedProcess(command, 0, output, "")
        self.probe.side_effect = probe
        report = check_dependencies("linux", environ={"PATH": "/tools", "CMAKE_PREFIX_PATH": "/custom/libs"}, host="linux")
        self.assertTrue(report.ready, report.lines())
        self.assertIn("CHECK IN CMAKE", "\n".join(report.lines()))
        self.assertEqual(report.env["CMAKE_PREFIX_PATH"], "/custom/libs")

    def test_wrong_compiler_architecture_is_rejected(self):
        def wrong(command, **kwargs):
            if "-dumpmachine" in command:
                return subprocess.CompletedProcess(command, 0, "i686-pc-msys", "")
            return self.probe_result(command, **kwargs)
        self.probe.side_effect = wrong
        with self.assertRaisesRegex(BuildError, "expected a windows x86_64 compiler"):
            self.check().require()

    def test_mixed_compiler_installations_are_rejected(self):
        other = self.root / "ucrt64/bin/g++.exe"
        other.parent.mkdir(parents=True)
        other.touch()
        self.env["CXX"] = str(other)
        with self.assertRaisesRegex(BuildError, "same MinGW installation"):
            self.check().require()

    def test_old_cmake_is_rejected(self):
        def old(command, **kwargs):
            if Path(command[0]).name == "cmake.exe":
                return subprocess.CompletedProcess(command, 0, "cmake version 3.19.0", "")
            return self.probe_result(command, **kwargs)
        self.probe.side_effect = old
        with self.assertRaisesRegex(BuildError, "3.20 or newer"):
            self.check().require()

    def test_unlaunchable_tool_is_reported_without_hiding_others(self):
        def broken(command, **kwargs):
            if Path(command[0]).name == "cmake.exe":
                raise FileNotFoundError("[WinError 2] The system cannot find the file specified")
            return self.probe_result(command, **kwargs)
        self.probe.side_effect = broken
        report = self.check()
        self.assertFalse(report.ready)
        self.assertIn("ninja", report.tools)
        self.assertIn("Cannot run cmake", "\n".join(report.lines()))
        self.assertIn("Install CMake", "\n".join(report.lines()))

    def test_timeout_is_actionable(self):
        self.probe.side_effect = subprocess.TimeoutExpired("cmake", 15)
        with self.assertRaisesRegex(BuildError, "timed out"):
            self.check().require()

    def test_missing_binutils(self):
        self.executables["ld"].unlink()
        with self.assertRaisesRegex(BuildError, "binutils"):
            self.check().require()

    def test_build_commands_preserve_paths_with_spaces(self):
        report = self.check()
        configure, compile_cmd, env = build_commands("windows", self.root / "Build dir", self.root / "BIOS file.bin", report)
        self.assertEqual(configure[0], str(self.executables["cmake"]))
        self.assertEqual(compile_cmd[0], configure[0])
        self.assertIn(str(self.root / "Build dir"), configure)
        self.assertIn("-DCMAKE_MAKE_PROGRAM=" + str(self.executables["ninja"]), configure)
        self.assertIn("-DCMAKE_C_COMPILER=" + str(self.executables["gcc"]), configure)
        self.assertEqual(env, report.env)

    def test_missing_dependencies_block_before_disc_or_compilation(self):
        self.executables["cmake"].unlink()
        with mock.patch("builder_core.check_dependencies", return_value=self.check()), \
             mock.patch("builder_core.disc_files") as disc, mock.patch("builder_core.run") as command:
            with self.assertRaises(BuildError):
                build("windows", self.root, self.root, lambda _line: None)
        disc.assert_not_called()
        command.assert_not_called()

    def test_popen_launch_failure_identifies_executable(self):
        executable = str(self.executables["cmake"])
        self.which.return_value = executable
        with mock.patch("builder_core.subprocess.Popen", side_effect=FileNotFoundError("[WinError 2] missing")):
            with self.assertRaises(BuildError) as error:
                run([executable, "--version"], lambda _line: None, cwd=self.root, env=self.env)
        self.assertIn(executable, str(error.exception))
        self.assertIn("Install CMake", str(error.exception))

    def test_popen_receives_argument_list_and_environment(self):
        executable = str(self.executables["cmake"])
        self.which.return_value = executable
        process = mock.Mock(stdout=["configured\n"], returncode=0)
        process.wait.return_value = 0
        with mock.patch("builder_core.subprocess.Popen", return_value=process) as popen:
            run([executable, "-B", str(self.root / "Build dir")], lambda _line: None, cwd=self.root, env=self.env)
        args, kwargs = popen.call_args
        self.assertEqual(args[0], [executable, "-B", str(self.root / "Build dir")])
        self.assertEqual(kwargs["env"], self.env)
        self.assertNotIn("shell", kwargs)

    def test_missing_working_directory_has_distinct_message(self):
        self.which.return_value = str(self.executables["cmake"])
        with mock.patch("builder_core.subprocess.Popen", side_effect=FileNotFoundError("missing")):
            with self.assertRaisesRegex(BuildError, "working folder does not exist"):
                run(["cmake"], lambda _line: None, cwd=self.root / "missing")

    def test_linux_cross_compilers_are_resolved(self):
        self.which.side_effect = lambda name, **_kwargs: "/tools/" + name
        def cross(command, **_kwargs):
            output = "x86_64-w64-mingw32" if "-dumpmachine" in command else "cmake version 3.29.0"
            return subprocess.CompletedProcess(command, 0, output, "")
        self.probe.side_effect = cross
        report = check_dependencies("windows", environ={"PATH": "/tools"}, host="linux")
        self.assertTrue(report.ready, report.lines())
        configure, _, _ = build_commands("windows", self.root, self.root / "bios", report)
        self.assertIn("-DCMAKE_CXX_COMPILER=/tools/x86_64-w64-mingw32-g++", configure)
        self.assertTrue(any("mingw64.cmake" in arg for arg in configure))

    def test_wsl_tools_are_checked_inside_linux(self):
        self.which.side_effect = lambda name, **_kwargs: str(self.root / "wsl.exe") if name == "wsl.exe" else None
        def wsl(command, **_kwargs):
            if "sh" in command:
                output = "/usr/bin/" + command[-1]
            elif "-dumpmachine" in command:
                output = "x86_64-linux-gnu"
            elif "/usr/bin/python3" in command:
                output = "Python 3.12.0"
            elif "/usr/bin/cmake" in command:
                output = "cmake version 3.29.0"
            else:
                output = "1.2.3"
            return subprocess.CompletedProcess(command, 0, output, "")
        self.probe.side_effect = wsl
        report = check_dependencies("linux", environ=self.env, host="windows")
        self.assertTrue(report.ready, report.lines())
        self.assertEqual(report.tools["cmake"], "/usr/bin/cmake")
        self.assertTrue(all(command.args[0][0] == report.tools["wsl"] for command in self.probe.call_args_list))

    def test_missing_wsl_is_actionable(self):
        report = check_dependencies("linux", environ=self.env, host="windows")
        with self.assertRaisesRegex(BuildError, "Install WSL"):
            report.require()

    def test_wsl_path_launch_failure(self):
        report = self.check()
        report.tools["wsl"] = "C:/Windows/System32/wsl.exe"
        report.launcher = [report.tools["wsl"], "--exec"]
        self.probe.side_effect = FileNotFoundError("missing")
        with self.assertRaisesRegex(BuildError, "wsl.exe"):
            wsl_path(self.root, report)


if __name__ == "__main__":
    unittest.main()
