# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed

- The README is now a short introduction. The full reference moved to
  `docs/reference.md`.
- `--out` may no longer name one of the files being packaged. The build refuses
  instead of overwriting a source file.
- Sources whose paths differ only in letter case (`App.dll` and `app.dll`) are
  reported as a collision, because they are the same file on Windows.

### Fixed

- `HNxPacker` no longer hangs when a source file cannot be read during hashing.
- Re-extracting over a hidden or system file in the cache no longer fails.
- Folder and file names outside the system's ANSI code page no longer fail the
  build or crash `HNxPacker` on Windows versions without UTF-8 code page support.
- Building without `--config` works when `--src` ends in a backslash or the
  folder name is not a valid app id. The app id is now sanitized, and the app
  name keeps non-ASCII characters.
- A missing or non-numeric value for `--threads`, `--level` or `--block-size`
  now prints an error and exits with code 2 instead of crashing.
- A package whose application has non-English version info no longer shows the
  launcher's name and version in Explorer.
- A file too large for its block size (more than 65,535 blocks) now fails the
  build with a clear message instead of producing a package that will not launch.
- After an identical rebuild, or if the cache's state file was deleted, launches
  go back to the fast path instead of re-checking the whole tree every time.

## [0.1.0] - 2026-11-09

### Added

- `hnxpack` packs an application folder into a single self-extracting `.exe`.
  Launches after the first unpack only check the cache and write nothing.
- With no arguments, `hnxpack` asks for the folder, the program to start and the
  output file. `hnxpack --src DIR` starts with the folder already chosen.
- Scripted builds with `hnxpack build`: JSON metadata through `--config`, codec,
  level and block size options, `--exclude`, `--icon-from` and `--no-icon`.
- `hnxpack inspect` and `hnxpack verify` to examine and check a built package.
- `HNxPacker`, a GUI for building packages.
- Packages take their icon and version info from the program they contain.
- 64-bit and 32-bit launchers built into `hnxpack`, chosen to match the
  application, with `--arch` to override.
- Launch options: `verify` modes, `waitMode`, `ui`, `requireAdmin`, argument
  passthrough, environment variables and `singleInstance`.
- `--hnx-*` launch flags for packaged apps: info, extract only, verify mode,
  target folder, clean and help.
- Temporary firewall rules that block a packaged application's inbound or
  outbound connections while it runs, and are removed when it exits.
- Packaged applications run under WinPE. The cache moves to a temporary folder
  when the profile folders don't exist, and free space is checked before
  unpacking.
- Folders can be packaged while their application is running. A file that
  changes during the build fails it.
- Long paths, Unicode names and Authenticode-signed packages are supported.
- `hnxpack --version` prints the version. `hnxpack.exe`, `HNxPacker.exe` and the
  launcher carry it in their file properties, and `HNxPacker` shows it in its
  window title.

[Unreleased]: https://github.com/hydranix/HNxApp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/hydranix/HNxApp/releases/tag/v0.1.0
