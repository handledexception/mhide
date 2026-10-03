# mhide

[![Build](https://github.com/handledexception/mhide/actions/workflows/build.yml/badge.svg)](https://github.com/handledexception/mhide/actions/workflows/build.yml)

Auto-hide the mouse cursor on Windows.

Why? Modern OLED displays have a penchant for screen burn. To help prevent this I like to keep my desktop background pure black and would like it if my mouse cursor would just disappear when I'm not at the PC, or after a period of inactivity.

## What it does

mhide sits in the system tray. After a configurable number of seconds without mouse or keyboard input it hides the cursor system-wide. Any mouse movement, click, or key press shows the cursor again and restarts the countdown.

- **Double-click** the tray icon, or right-click and choose **Settings...**, to open the settings window.
- Settings: inactivity delay in seconds, enabled on/off, and start with Windows.
- The tray menu also offers **Enabled** (toggle), **Hide cursor now**, and **Exit**.
- Settings are stored under `HKCU\Software\mhide`. "Start with Windows" adds an entry under the user's `Run` key.

## Design

- Plain C against Win32 only. Statically linked, so the exe has no runtime dependencies beyond `user32`, `shell32`, `advapi32`, `comctl32`, and `kernel32`.
- The cursor is hidden by replacing every system cursor with a transparent one via `SetSystemCursor`, and restored by reloading the user's cursor scheme via `SystemParametersInfo(SPI_SETCURSORS)`.
- Minimal resource use. While the cursor is visible there is no polling at all, just one coalescable timer armed for the idle deadline. While the cursor is hidden the app registers for Raw Input so the first mouse or keyboard event wakes it, with a 250 ms fallback check for anything Raw Input does not report, such as pen, touch, or synthetic input. Steady-state footprint is around 1 MB private memory and effectively zero CPU.
- Safety net. Blanked system cursors are shared state that would outlive a crash, so mhide spawns a tiny watchdog copy of itself (`mhide.exe --watchdog <pid>`) that sleeps on the main process handle and restores the cursor scheme the moment the main process exits for any reason. mhide also restores the cursor scheme on startup and on normal exit, log-off, and shutdown.

If the cursor ever does get stuck hidden, run:

```
mhide.exe --restore
```

To trace what the app sees (hide/show decisions, Raw Input events, fallback checks), set `MHIDE_LOG` to a file path before starting it:

```
set MHIDE_LOG=%TEMP%\mhide.log
mhide.exe
```

## Building

Requires Visual Studio 2022 (Community or Build Tools) with the "Desktop development with C++" workload. From the repo root:

```
build.cmd
```

This produces `build\mhide.exe`.

### Continuous integration

[.github/workflows/build.yml](.github/workflows/build.yml) builds on a `windows-latest` runner for every push to `main`, every pull request, and on manual dispatch. It runs `build.cmd`, fails if the exe imports any CRT runtime DLL (it must stay statically linked), and uploads `mhide.exe` as a workflow artifact. Pushing a tag like `v1.0.0` additionally creates a GitHub release with the exe attached:

```
git tag v1.0.0
git push origin v1.0.0
```

To regenerate the icon:

```
powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
```

## Layout

- [src/main.c](src/main.c): the whole application.
- [src/mhide.rc](src/mhide.rc), [src/resource.h](src/resource.h): settings dialog, icon, version info, and manifest.
- [src/mhide.manifest](src/mhide.manifest): common controls v6 and per-monitor DPI awareness.
- [tools/make-icon.ps1](tools/make-icon.ps1): generates `src/mhide.ico`.
- [build.cmd](build.cmd): one-step MSVC build.
