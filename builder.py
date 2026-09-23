#!/usr/bin/env python3
"""Cross-platform desktop interface for the local source build."""

from __future__ import annotations

import platform
import queue
import subprocess
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from builder_core import BuildError, ROOT, build

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
        self.geometry("780x590")
        self.minsize(700, 520)
        self.configure(bg=BG)
        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self.disc = tk.StringVar()
        self.output = tk.StringVar(value=str(ROOT / "outputs"))
        self.target = tk.StringVar(value=platform.system().lower() if platform.system().lower() in {"windows", "linux"} else "windows")
        self.status = tk.StringVar(value="Ready")
        self._style()
        self._layout()
        self.after(80, self._drain)

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
        ttk.Radiobutton(target_box, text="Windows x64", value="windows", variable=self.target).pack(side="left", padx=(0, 30))
        ttk.Radiobutton(target_box, text="Linux x86_64", value="linux", variable=self.target).pack(side="left")

        self._path_row(root, 4, "Original disc folder", self.disc, self._choose_disc)
        self._path_row(root, 6, "Output location", self.output, self._choose_output)

        action = ttk.Frame(root)
        action.grid(row=8, column=0, sticky="ew", pady=(4, 15))
        action.columnconfigure(0, weight=1)
        ttk.Label(action, textvariable=self.status, style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        self.button = ttk.Button(action, text="Build", style="Primary.TButton", command=self._start)
        self.button.grid(row=0, column=1, sticky="e")
        self.progress = ttk.Progressbar(root, mode="indeterminate")
        self.progress.grid(row=9, column=0, sticky="ew", pady=(0, 18))

        ttk.Label(root, text="Build log").grid(row=10, column=0, sticky="w", pady=(0, 7))
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
        selected = filedialog.askdirectory(title="Select the original disc folder")
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

    def _start(self) -> None:
        if not self.disc.get().strip():
            messagebox.showerror("Disc folder required", "Choose the folder containing the original CUE and tracks.")
            return
        self.button.configure(state="disabled")
        self.status.set("Building...")
        self.progress.start(10)
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

    def _drain(self) -> None:
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self._append(value)
            else:
                self.progress.stop()
                self.button.configure(state="normal")
                self.status.set("Ready" if kind == "done" else "Build failed")
                self._append(("Ready: " if kind == "done" else "Error: ") + value)
                if kind == "done":
                    messagebox.showinfo("Build complete", value)
                else:
                    messagebox.showerror("Build failed", value)
        self.after(80, self._drain)


if __name__ == "__main__":
    BuilderApp().mainloop()
