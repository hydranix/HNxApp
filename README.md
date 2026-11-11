# HNx App

Turn a Windows application folder into a single portable `.exe`. Nothing to
install.

**[Download the latest release](https://github.com/hydranix/HNxApp/releases/latest)**:
`hnxpack.exe` (command line) and `HNxPacker.exe` (GUI). Each one is a single file
that runs on its own.

The first time a packaged application runs, it unpacks itself into a cache folder
and starts the app. After that, a launch only checks the cache, which adds a few
milliseconds and writes nothing.

## Usage

Run `hnxpack.exe` with no arguments, or open `HNxPacker.exe`. You pick the
application folder, the program to start, and where to save the package. The
name, version and icon are taken from the program itself.

For scripts and CI:

```bash
hnxpack build --src .\MyApp --out .\MyApp.exe --config app.json
```

`app.json` sets the app's id, name, version, entry point and launch behaviour.
Leave out `--config` and hnxpack generates one. Run `hnxpack --help` for every
option, and `hnxpack verify .\MyApp.exe` to check a built package.

Full reference: [docs/reference.md](docs/reference.md)

## Building

You need Visual Studio with MSVC and the Windows SDK. CMake and Ninja come with
Visual Studio. Build from a Developer Command Prompt:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build\src\packer\hnxpack.exe` and `build\src\gui\HNxPacker.exe`.
Third-party libraries are downloaded and compiled during the build. Add
`-DHNX_BUILD_X86_STUB=OFF` if you don't have the x86 toolset; packages are then
always 64-bit.
