# Mickey's Wild Adventure Recompiled

Native recompilation of **Mickey's Wild Adventure** (PlayStation) for modern PCs, powered by [PSXRecomp](https://github.com/RetroPortingToolKit/psxrecomp).

This project aims to preserve the original game while making it easier and more enjoyable to play on modern hardware with quality-of-life improvements, a custom frontend, modern display options and extra features.

> This is **not an emulator** and **not a remake**.  
> The original PlayStation game code is statically recompiled into native code using PSXRecomp.

---

## Screenshot

![Mickey's Wild Adventure Recompiled gameplay](docs/images/screenshot-gameplay.png)

---

## Features

- Native **Windows** and **Linux** builds
- Powered by **PSXRecomp**
- Custom frontend and startup flow
- Custom main menu
- Story profiles and continue system
- Improved save handling
- Practice Mode protected from overwriting Story progress
- Custom pause menu
- Restart Level option
- Custom Game Over screen
- Resolution options
- Windowed, borderless and fullscreen display modes
- CRT filter presets
- Optional bezels
- Keyboard and controller support
- Rebindable controls
- Automatic input prompt switching
- Multiple UI languages
- Custom loading screens
- Gameplay tips during loading
- Saving overlay
- RetroAchievements integration
- OpenBIOS integration
- Dedicated build workflow for supported game dumps

---

## Supported Game Version

This project currently targets:

- **Mickey's Wild Adventure (Europe)**
- **SCES-00163**

A proper **CUE/BIN dump with all 28 tracks** is required.

---

## Quick Start

### For players

1. Obtain your own legal copy of **Mickey's Wild Adventure (Europe)**.
2. Dump the disc as a proper **CUE/BIN** image with all audio tracks intact.
3. Launch **Recomp Build Studio**.
4. Select your game `.cue` file.
5. Choose your target platform:
   - Windows x64
   - Linux x86_64
6. Build the project.
7. Run the generated executable.

---

## Recomp Build Studio

This repository includes **Recomp Build Studio**, a guided build tool intended to make the process easier.

It handles the main setup steps such as:

- verifying the disc image
- checking for the correct game version
- validating the track layout
- preparing the runtime
- building a runnable native version of the game

This is the recommended way to build the project.

---

## Manual Build

Advanced users can also build the project manually.

### Requirements

- **CMake**
- **Ninja**
- A C/C++ toolchain
- **SDL3**
- **OpenGL**
- Python (for some helper/build scripts)

Depending on your platform, additional dependencies may be required.

### Basic build flow

```bash
cmake -S project -B work/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build work/build
```

> Exact setup may differ depending on platform and local environment.

---

## Controls and Input

The project supports:

- keyboard
- gamepads/controllers
- automatic prompt switching
- rebinding controls

Input prompts change dynamically depending on the active device.

---

## Save System

The recomp includes a custom save/profile layer on top of the original game flow.

### Story Mode

- uses profile-based progress
- supports continue/checkpoint behavior
- is protected from accidental overwrite issues

### Practice Mode

- designed as a **no-save mode**
- does **not** overwrite Story progress

---

## Display Features

The project includes several modern display options:

- multiple resolutions
- windowed mode
- borderless fullscreen
- exclusive fullscreen
- CRT filter presets
- bezel artwork options

These are intended to keep the original game's look and feel while improving presentation on modern displays.

---

## RetroAchievements

The project includes **RetroAchievements** support.

Features include:

- login support
- achievement list
- badge display
- unlock notifications

RetroAchievements are optional, but supported directly through the project frontend.

---

## Languages

The interface supports multiple languages, including:

- English
- Polish
- Italian
- Japanese
- German

---

## Project Structure

A simplified overview:

```text
project/                 Main project files
psxrecomp/               PSXRecomp runtime and related code
frontend/                Custom frontend systems
assets/                  UI/audio/art assets
work/                    Build output and temporary build data
```

> Exact structure may change as the project evolves.

---

## About PSXRecomp

This project is built on top of **PSXRecomp**, a static recompilation framework for original PlayStation games.

PSXRecomp repository:  
https://github.com/RetroPortingToolKit/psxrecomp

Without PSXRecomp, this project would not be possible.

---

## Legal

**No copyrighted game files are included with this repository.**

You must provide your own legally obtained copy of the supported PlayStation game.

This is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by, or sponsored by Disney, Sony, Traveller's Tales, or any other rights holder.

All trademarks, characters, game assets and original game content belong to their respective owners.

---

## Credits

- **Original game:** Traveller's Tales
- **Based on:** Disney's Mickey Mouse
- **Static recompilation framework:** PSXRecomp
- **Project / additional PC features:** Kacperek

---

## Disclaimer

This project is a fan-made tribute created for preservation, experimentation and accessibility on modern systems.

Please support the original creators and rights holders whenever possible.
