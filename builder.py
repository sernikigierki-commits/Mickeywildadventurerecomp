#!/usr/bin/env python3
"""Cross-platform desktop interface for the local source build."""

from __future__ import annotations

import platform
import queue
import shutil
import subprocess
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from builder_core import BuildError, ROOT, build, check_build_dependencies
from build_dependencies import DependencyReport

BG = "#171b1f"
PANEL = "#22282d"
FIELD = "#2c3439"
TEXT = "#edf1f0"
MUTED = "#a5b1b1"
ACCENT = "#45c7b0"


class BuilderApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Recomp Build Studio")
        self.geometry("880x720")
        self.minsize(760, 640)
        self.configure(bg=BG)
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.disc = tk.StringVar()
        self.output = tk.StringVar(value=str(ROOT / "outputs"))
        self.target = tk.StringVar(value=platform.system().lower() if platform.system().lower() in {"windows", "linux"} else "windows")
        self.status = tk.StringVar(value="Checking build tools...")
        self.windows_package: Path | None = None
        self.busy = False
        self.checked_target: str | None = None
        self.dependencies_ready = False
        self._style()
        self._layout()
        self.target.trace_add("write", self._target_changed)
        self.after(80, self._drain)
        self.after(100, self._check)

    def _style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background=BG)
        style.configure("Panel.TFrame", background=PANEL)
        style.configure("TLabel", background=BG, foreground=TEXT, font=("Segoe UI", 10))
        style.configure("Muted.TLabel", background=BG, foreground=MUTED, font=("Segoe UI", 9))
        style.configure("Header.TLabel", background=BG, foreground=TEXT, font=("Segoe UI Semibold", 20))
        style.configure("TRadiobutton", background=PANEL, foreground=TEXT, font=("Segoe UI", 11))
        style.map("TRadiobutton", background=[("active", PANEL)], foreground=[("active", TEXT)])
        style.configure("TEntry", fieldbackground=FIELD, foreground=TEXT, insertcolor=TEXT, padding=8)
        style.configure("TButton", background=FIELD, foreground=TEXT, font=("Segoe UI", 10), padding=(14, 9), borderwidth=0)
        style.map("TButton", background=[("active", "#3a454a"), ("disabled", FIELD)])
        style.configure("Primary.TButton", background=ACCENT, foreground="#10211e", font=("Segoe UI Semibold", 10), padding=(20, 10))
        style.map("Primary.TButton", background=[("active", "#6bdbc8"), ("disabled", "#526e68")])
        style.configure("TProgressbar", background=ACCENT, troughcolor=FIELD, borderwidth=0)

    def _layout(self) -> None:
        root = ttk.Frame(self, padding=(28, 24))
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, weight=1)
        ttk.Label(root, text="Recomp Build Studio", style="Header.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(root, text="LOCAL BUILD  /  SOURCE SNAPSHOT", style="Muted.TLabel").grid(row=1, column=0, sticky="w", pady=(3, 22))

        ttk.Label(root, text="Target").grid(row=2, column=0, sticky="w", pady=(0, 7))
        target_box = ttk.Frame(root, style="Panel.TFrame", padding=(16, 11))
        target_box.grid(row=3, column=0, sticky="ew", pady=(0, 18))
        self.target_buttons = [
            ttk.Radiobutton(target_box, text="Windows x64", value="windows", variable=self.target),
            ttk.Radiobutton(target_box, text="Linux x86_64", value="linux", variable=self.target),
        ]
        for radio in self.target_buttons:
            radio.pack(side="left", padx=(0, 30))

        self._path_row(root, 4, "Original disc folder", self.disc, self._choose_disc)
        self._path_row(root, 6, "Output location", self.output, self._choose_output)

        action = ttk.Frame(root)
        action.grid(row=8, column=0, sticky="ew", pady=(4, 15))
        action.columnconfigure(0, weight=1)
        ttk.Label(action, textvariable=self.status, style="Muted.TLabel", wraplength=430).grid(row=0, column=0, sticky="w")
        self.wine_button = ttk.Button(action, text="Wine startup check", command=self._wine_check, state="disabled")
        if platform.system().lower() == "linux":
            self.wine_button.grid(row=1, column=2, sticky="e", pady=(8, 0))
        self.retry = ttk.Button(action, text="Retry Check", command=self._check)
        self.retry.grid(row=0, column=1, padx=(0, 10))
        self.button = ttk.Button(action, text="Build", style="Primary.TButton", command=self._start, state="disabled")
        self.button.grid(row=0, column=2, sticky="e")
        self.progress = ttk.Progressbar(root, mode="indeterminate")
        self.progress.grid(row=9, column=0, sticky="ew", pady=(0, 18))

        ttk.Label(root, text="Dependencies and build log — missing tools show installation steps below").grid(row=10, column=0, sticky="w", pady=(0, 7))
        log_frame = ttk.Frame(root)
        log_frame.grid(row=11, column=0, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        self.log = tk.Text(log_frame, bg="#101416", fg="#c9d4d2", insertbackground=TEXT,
                           relief="flat", font=("Consolas", 9), wrap="word", padx=12, pady=11,
                           state="disabled")
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)
        root.rowconfigure(11, weight=1)

    def _path_row(self, parent: ttk.Frame, row: int, label: str, value: tk.StringVar, choose) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=(0, 7))
        frame = ttk.Frame(parent)
        frame.grid(row=row + 1, column=0, sticky="ew", pady=(0, 18))
        frame.columnconfigure(0, weight=1)
        ttk.Entry(frame, textvariable=value).grid(row=0, column=0, sticky="ew", padx=(0, 9))
        ttk.Button(frame, text="Browse...", command=choose).grid(row=0, column=1)

    def _choose_disc(self) -> None:
        selected = filedialog.askdirectory(title="Select the folder containing one CUE and all 28 BIN tracks")
        if selected:
            self.disc.set(selected)

    def _choose_output(self) -> None:
        selected = filedialog.askdirectory(title="Select an output location")
        if selected:
            self.output.set(selected)

    def _append(self, line: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _target_changed(self, *_args) -> None:
        self.dependencies_ready = False
        self.button.configure(state="disabled")
        if not self.busy:
            self._check()

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        self.retry.configure(state="disabled" if busy else "normal")
        wine_ready = self.windows_package is not None and platform.system().lower() == "linux" and shutil.which("wine") and shutil.which("wineserver")
        self.wine_button.configure(state="normal" if not busy and wine_ready else "disabled")
        for radio in self.target_buttons:
            radio.configure(state="disabled" if busy else "normal")
        self.button.configure(state="normal" if not busy and self.dependencies_ready else "disabled")
        if busy:
            self.progress.start(10)
        else:
            self.progress.stop()

    def _check(self) -> None:
        if self.busy:
            return
        self.dependencies_ready = False
        self._set_busy(True)
        self.status.set("Checking build tools...")
        target = self.target.get()
        self._append("Checking tools for " + target + ". No packages will be installed automatically.")
        threading.Thread(target=self._check_worker, args=(target,), daemon=True).start()

    def _check_worker(self, target: str) -> None:
        try:
            self.events.put(("checked", check_build_dependencies(target)))
        except Exception as exc:
            self.events.put(("error", f"Dependency check failed: {exc}"))

    def _start(self) -> None:
        if self.busy:
            return
        if not self.dependencies_ready or self.checked_target != self.target.get():
            self._check()
            return
        if not self.disc.get().strip():
            messagebox.showerror("Disc folder required", "Choose the folder containing the original CUE and all 28 BIN tracks.")
            return
        self._set_busy(True)
        self.status.set("Checking tools and building...")
        self._append("Starting " + self.target.get() + " build")
        thread = threading.Thread(target=self._worker, args=(self.target.get(), self.disc.get(), self.output.get()), daemon=True)
        thread.start()

    def _worker(self, target: str, disc: str, output: str) -> None:
        try:
            final = build(target, Path(disc), Path(output), lambda line: self.events.put(("log", line)))
            self.events.put(("done", str(final)))
        except (BuildError, OSError, subprocess.SubprocessError) as exc:
            self.events.put(("error", str(exc)))
        except Exception as exc:
            self.events.put(("error", f"Unexpected build error: {exc}"))

    def _wine_check(self) -> None:
        if self.busy or self.windows_package is None:
            return
        self._set_busy(True)
        self.status.set("Checking Windows startup in a private Wine prefix...")
        threading.Thread(target=self._wine_worker, args=(self.windows_package,), daemon=True).start()

    def _wine_worker(self, package: Path) -> None:
        from wine_validation import wine_smoke
        try:
            result = wine_smoke(package, ROOT / "work/wine-check", lambda line: self.events.put(("log", line)))
            self.events.put(("wine_done", str(result)))
        except (BuildError, OSError, subprocess.SubprocessError) as exc:
            self.events.put(("wine_error", str(exc)))
        except Exception as exc:
            self.events.put(("wine_error", f"Unexpected Wine check error: {exc}"))

    def _drain(self) -> None:
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self._append(str(value))
            elif kind == "checked":
                assert isinstance(value, DependencyReport)
                self.checked_target = value.target
                self.dependencies_ready = value.ready and value.target == self.target.get()
                for line in value.lines():
                    self._append(line)
                self.status.set("Tools ready — choose the disc folder and click Build" if self.dependencies_ready else "Dependencies need attention — follow the log, then Retry Check")
                self._set_busy(False)
            elif kind in {"wine_done", "wine_error"}:
                self._set_busy(False)
                self.status.set("Wine startup checked; graphical test still required" if kind == "wine_done" else "Wine check failed; Windows package is still available")
                self._append(str(value))
                if kind == "wine_error":
                    messagebox.showerror("Wine startup check", value)
            else:
                if kind == "done" and self.target.get() == "windows":
                    self.windows_package = Path(str(value))
                if kind == "error":
                    self.dependencies_ready = False
                self._set_busy(False)
                self.status.set("Ready" if kind == "done" else "Build failed")
                self._append(("Ready: " if kind == "done" else "Error: ") + str(value))
                if kind == "done":
                    messagebox.showinfo("Build complete", value)
                else:
                    messagebox.showerror("Build failed", value)
        self.after(80, self._drain)


if __name__ == "__main__":
    BuilderApp().mainloop()
