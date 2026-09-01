# HNx portable app system

A packaged application is a single `.exe`: a launcher stub with an entire
application filesystem appended to it as a compressed archive. Running it unpacks
whatever is missing or stale into a cached directory and starts the app.

The design is shaped almost entirely by **repeat launch cost**. A portable app is
launched over and over against the same directory, so the second and subsequent
launches must do essentially nothing. Measured on this machine, warm launch adds
**~2 ms** (`verify=fast`) to **~10 ms** (`verify=normal`, the default) over
starting the application directly, and performs zero writes.

```
hnxstub.exe  +  your app folder   ──hnxpack──►   YourApp.exe
                                                 (one file, nothing to install)
```

## Building

Requires MSVC and the Windows SDK. CMake and Ninja ship with Visual Studio.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Third-party sources (zstd 1.5.6, lz4 1.10.0, xxHash 0.8.3) are fetched by CMake
at configure time and compiled from source — there is nothing to install.

The build produces the x64 stub directly and the x86 stub through a nested CMake
configure using the Visual Studio generator, which resolves the x86 toolset by
itself; doing it with Ninja would mean reproducing the `INCLUDE` and `LIB`
environment for a second target architecture by hand. That sub-build re-enters the
same `CMakeLists.txt` with `HNX_STUB_ONLY=ON`, which is what stops it recursing.
Both stubs are then embedded into `hnxpack`.

Run the tests:

```bash
build\tests\hnx_tests.exe
```

## Packaging an application

Run `hnxpack` with no arguments and it asks three questions:

1. **Which folder?** — the application directory to package.
2. **Which program?** — the entry point, with the dialog opening inside the
   folder you just chose. It must be inside that folder, since it is stored as a
   path relative to the extracted tree.
3. **Where to save?** — defaulting to the folder's own name plus `.exe`, in
   whichever directory you saved to last time (remembered in
   `HKCU\Software\HNx\hnxpack`).

Everything else is derived: the app id from the folder name, the name and version
from the entry point's `VERSIONINFO`, the icon from its resources, and the stub
architecture from its PE machine type. Cancelling any dialog exits without
writing anything.

`hnxpack --src DIR` is the same flow with the first question already answered,
so it asks only for the entry point and the destination. The shorthand is
recognised only when `--src` and its value are the whole command line: anything
else alongside it — a `build` subcommand, an `--out`, a second `--src` — is a
scripted invocation, and dropping one of those into a modal dialog would hang a
build with nothing printed to say why. `--no-console` is the one exception, since
this form implies it anyway.

If there is no desktop — CI, a service — these interactive forms print usage
instead of blocking on a dialog nobody can see.

For scripted builds:

```bash
build\src\packer\hnxpack.exe build --src .\MyApp --out .\MyApp.exe --config app.json
```

`app.json` becomes the `HNXMETA` resource in the output:

```json
{
  "schemaVersion": 1,
  "appId": "my-app",
  "appName": "My App",
  "appVersion": "1.0.0",
  "targetDir": "%LOCALAPPDATA%\\HNx\\%APPID%\\%VERSION%",
  "exec": "bin\\app.exe",
  "workDir": "%TARGET%",
  "env": { "MY_APP_DATA": "%TARGET%\\data" },
  "waitMode": "none",
  "verify": "normal",
  "ui": "progress"
}
```

`%TARGET%`, `%APPDIR%`, `%APPID%` and `%VERSION%` expand, as does anything
`ExpandEnvironmentStringsW` understands.

| Field | Values | Meaning |
| --- | --- | --- |
| `verify` | `fast`, `normal`, `paranoid` | how hard to check the cache before launching |
| `waitMode` | `none`, `wait`, `wait-and-clean` | exit immediately, wait and propagate the exit code, or wait then delete the tree |
| `ui` | `silent`, `progress`, `console` | progress window only appears if extraction runs past ~300 ms |
| `requireAdmin` | bool | route the launch through the shell with the `runas` verb |
| `argPassthrough` | bool | forward the stub's own arguments to the application |

### Console windows

`hnxpack` is a console-subsystem program, deliberately: `cmd.exe` waits for a
console application and returns the prompt immediately for a GUI one, so building
it as a GUI binary would silently break `hnxpack build ... && next-step`.

`--no-console` hides the console window, and the no-argument interactive flow
does it automatically — a double-click cannot pass a flag, and that is exactly
the case where the window is unwanted.

It only ever hides a console **this process owns**. A console Windows created for
us has exactly one process attached; one inherited from a shell has at least two.
Running `hnxpack --no-console` from your terminal therefore leaves that terminal
alone, which matters far more than hiding a stray window does.

With the console hidden there is nowhere for a message to land, so failures are
reported as dialogs instead.

One honest limitation: Windows creates the console before `main` runs, so a brief
flash is unavoidable in a console-subsystem program. This reduces it from a window
that stays up for the whole run to a few milliseconds. Eliminating it entirely
needs a second GUI-subsystem binary (the `pythonw.exe` pattern) — worth doing if
the planned GUI front-end does not supersede the need.

### The stub is inside hnxpack

`hnxpack.exe` carries an x64 and an x86 launcher stub as resources, so it is a
single self-sufficient file — copy it anywhere and it still packages. It does not
look for a `hnxstub.exe` on disk, and refuses to start the interactive flow at all
if it has no stub built in, rather than asking three questions and then failing.

The architecture is **matched to the entry point**: a 32-bit application gets the
32-bit stub, so the package runs anywhere the application would. An entry point
that is not a PE image at all — a `.bat`, a `.lnk` — carries no architecture and
falls back to x64. `--arch x64|x86` overrides the match; `--stub FILE` bypasses
the embedded stubs entirely.

One consequence worth knowing: a 32-bit stub memory-maps the whole payload into a
32-bit address space, so it cannot carry an archive near 2 GB. The stub is linked
`/LARGEADDRESSAWARE`, which lifts that to 3 GB (4 GB under WOW64). Pack a very
large 32-bit application with `--arch x64` if its users are all on 64-bit Windows.

Building the 32-bit stub needs the x86 MSVC toolset and SDK libraries. Configure
with `-DHNX_BUILD_X86_STUB=OFF` to skip it; hnxpack then embeds only x64 and says
so when asked for x86.

### Icons and version info

By default a package takes its icons **and** its `VERSIONINFO` from the entry
point, so it looks like the application it contains rather than like a generic
stub — the right icon in Explorer and the right name and version in Properties.
The two travel together on purpose: copying only the icon produces something that
looks right until someone opens Properties and sees the stub's details.

`--icon-from FILE` takes them from somewhere else; `--no-icon` copies neither.
An entry point with no resources of its own (a `.bat`, a bare console tool) is
fine and simply contributes nothing.

Inspect and check a built package — worth doing in CI, since a packaged exe is
otherwise opaque:

```bash
hnxpack inspect .\MyApp.exe    # footer, metadata, block and entry counts
hnxpack verify  .\MyApp.exe    # decompress every block, check every hash
```

## Runtime flags

Any argument starting with `--hnx-` is consumed by the stub and not forwarded
(disable with `"allowStubFlags": false`):

```
--hnx-info            payload details
--hnx-extract-only    unpack without launching
--hnx-verify=MODE     override the configured verify mode
--hnx-target=DIR      unpack somewhere else
--hnx-help
```

## How the fast path works

1. Read a 64-byte footer at the end of the file to find the archive.
2. Decompress the TOC (a few KB).
3. Read `<target>\.hnx\state.bin`; if its build id matches this payload, stat
   every entry in parallel and compare size plus mtime.
4. If nothing needs writing, launch. No block is read, no window is created.

Anything stale drops to a hash comparison before being rewritten, so a file whose
timestamp moved but whose contents did not is skipped rather than rewritten.
Blocks are ~8 MB and independently compressed, so a block whose files are all
unchanged is never even decompressed.

Two rules keep this correct and are easy to break:

- Extracted files get the **TOC's** mtime, not the wall clock. Otherwise the
  timestamp check never matches and every launch re-hashes the whole tree.
- `state.bin` is written **last**, and removed before the first write. A crash
  mid-extraction therefore leaves no state file, and the next run falls back to
  full verification instead of trusting a half-written tree.

## Layout

```
docs/format.md        the container format — the contract between the two binaries
src/common/           format.hpp, path validation, hashing
src/stub/             the launcher (ships inside every package)
src/packer/           hnxpack
tests/                unit tests
```

`src/common/format.hpp` is the single definition of the on-disk layout, and the
footer lookup (`hnx::findFooter`) is shared rather than reimplemented — the packer
must validate exactly what the stub will load.

## Notes worth knowing

**Nothing under `src/stub/` may include `<print>`, `<format>` or `<iostream>`.**
One `std::println` call grew the stub from ~101 KB to ~391 KB on this toolchain.
A post-build check in `cmake/CheckStubSize.cmake` warns if this regresses. The
packer has no such restriction.

**Signing works, and the footer lookup depends on getting it right.** The
Authenticode certificate table must be 8-byte aligned, so `signtool` inserts up
to 7 bytes of padding between the payload and the certificate — the security
directory points past the real end of the data. Sign *after* packing; the
signature covers the payload.

**Antivirus.** Self-extracting stubs get flagged. Extracting to `%LOCALAPPDATA%`
rather than `%TEMP%`, signing the result, and not packing or obfuscating the stub
are all deliberate choices here.

**`CreateProcessW` handles `.bat` on current Windows** — the shell fallback exists
for `.lnk` shortcuts and documents, which fail with `ERROR_BAD_EXE_FORMAT`. A
process started through the shell is detached from the console, so its output
does not reach a terminal the stub was launched from.

## Status

Working: the container format, packer, stub, extraction, the skip ladder,
launching with shell fallback, cross-process locking, the progress UI,
Authenticode signing, long paths, Unicode, `--hnx-clean` orphan pruning,
`singleInstance`, the interactive flow and its `--src DIR` shorthand,
icon/version branding, and
x64/x86 stubs embedded in the packer with the architecture matched to the entry
point.

The three-dialog chain is the one part not covered by an automated test — dialogs
cannot be driven headlessly. Its pure logic is unit tested — `sanitizeAppId`, and
which command lines select the flow at all (`presetSourceFromArgs`, the guard
that keeps a scripted build out of a modal dialog) — and the first dialog and the
cancel path are verified, but a full pick-pick-save run is a manual check.

Not yet done: ARM64 stubs, LZMA and Brotli codecs (ids are reserved), and zstd
dictionaries — the `dictId` field exists in the block descriptor specifically so
that enabling them later is not a format break.

ARM64 is the notable gap now that architecture matching exists: an ARM64 entry
point is detected as "not x64 or x86" and falls back to the x64 stub, which runs
under emulation on ARM64 Windows and launches the native application correctly,
just less efficiently than a native stub would. The toolset and SDK libraries for
it are present on this machine, so adding a third stub is mostly a matter of
another `ExternalProject_Add` block and one more resource.

One design question is still open and worth deciding before this is used in
anger: `targetDir` includes `%VERSION%` by default, which gives clean
side-by-side versions but means upgrading rewrites the whole tree. Dropping
`%VERSION%` would let the build-id and per-file hashes do their job across
versions, so a 1.0 → 1.1 upgrade rewrites only the files that actually changed.
