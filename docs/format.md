# HNXPKG Container Format v1

The source of truth for the on-disk layout shared by `hnxstub` and `hnxpack`.
The binary encoding lives in [`src/common/format.hpp`](../src/common/format.hpp);
this document explains the *why*. When they disagree, the header wins and this
document is a bug.

All integers are **little-endian**. All structures are `#pragma pack(1)` and are
`static_assert`ed to their documented size.

---

## 1. File layout

A packaged application is a normal PE executable with data appended past the end
of the image:

```
offset 0                                                          logical EOF
+-------------------+--------------------+-----------+-----------+
| PE headers        | sections, incl.    | container | footer    | [cert blob]
| + .rsrc (HNXMETA) | code and resources |           | 64 bytes  |  optional
+-------------------+--------------------+-----------+-----------+
                                         ^                       ^
                                    archiveOffset          logicalEof
```

The container is opaque to Windows: the PE loader never reads past the last
section, so the appended bytes do not affect execution.

### 1.1 Locating the footer

The footer is the last 64 bytes **before any Authenticode certificate**, not
necessarily the last 64 bytes of the file. `signtool` appends a certificate blob
after everything else, so `fileSize - 64` is wrong for any signed build.

Resolution order:

1. Parse `IMAGE_DOS_HEADER` then `IMAGE_NT_HEADERS` then
   `OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY]`.
2. If that directory `VirtualAddress` is non-zero, it is a **file offset**, not
   an RVA. This is the one data directory that breaks the usual rule. Take it
   as the logical EOF.
3. Otherwise the logical EOF is the physical file size.
4. Read 64 bytes at `logicalEof - 64`; validate `magic` and `footerCrc32`.
5. **If step 4 finds no magic, retry at `logicalEof - 64 - n` for n in 1..7.**

Step 5 is not defensive padding, it is required. The Authenticode certificate
table must begin on an **8-byte boundary**, so `signtool` inserts up to 7 bytes
of padding between the end of our data and the certificate. The security
directory offset includes that padding; our footer does not. Skipping step 5
means a signed package whose payload happens not to end on an 8-byte boundary is
simply not found.

This was observed, not theorised: a 3,723,628-byte package (≡ 4 mod 8) signed
with a test certificate got `securityDir.VirtualAddress = 3723632` against a real
footer ending at `3723628`, a four-byte miss.

A footer that is *found* but fails its CRC is a corrupt payload, not a misplaced
one, and must be reported rather than searched past: continuing is more likely to
find garbage that happens to validate than to recover anything real.

Only if all of the above fails should a reader scan backwards for `magic` across
the last 64 MB. That is a recovery path for damaged or unusually post-processed
binaries; a healthy build always resolves via steps 2 through 5.

Both the stub and `hnxpack` must use the **same** implementation
(`hnx::findFooter`). An earlier version had the packer reimplement the lookup
without step 5, so `hnxpack verify` reported "no payload" on signed packages that
the stub loaded perfectly — the tool meant to validate a package was not looking
at what the stub would actually read.

---

## 2. Footer, 64 bytes

| Offset | Field | Type | Notes |
| ---: | --- | --- | --- |
| 0 | `magic` | `u8[8]` | `48 4E 58 50 4B 47 1A 00` = `HNXPKG\x1A\0` |
| 8 | `formatVersion` | `u32` | 1 |
| 12 | `flags` | `u32` | see below |
| 16 | `archiveOffset` | `u64` | absolute file offset of container start |
| 24 | `archiveSize` | `u64` | container length, footer excluded |
| 32 | `tocOffset` | `u64` | relative to `archiveOffset` |
| 40 | `tocCompSize` | `u32` | compressed TOC bytes |
| 44 | `tocRawSize` | `u32` | decompressed TOC bytes |
| 48 | `tocCodec` | `u8` | codec id, see section 5 |
| 49 | `reserved` | `u8[3]` | zero |
| 52 | `buildId` | `u64` | random per build; payload identity |
| 60 | `footerCrc32` | `u32` | CRC-32 (IEEE) over bytes 0 through 59 |

`buildId` is what lets the stub decide, in one comparison, whether a target
directory was written by this exact payload. It must be freshly random for every
`hnxpack build` and never derived from content: two builds of identical input are
still distinct payloads as far as the cache is concerned.

### Footer flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `FOOTER_FLAG_HAS_DIRS` | TOC carries explicit directory entries |
| 1 | `FOOTER_FLAG_RESERVED_DICT` | reserved for zstd dictionaries, see 4.3 |

---

## 3. TOC

Stored at `archiveOffset + tocOffset`, compressed with `tocCodec`. Decompresses
to exactly `tocRawSize` bytes in this order:

```
TocHeader          24 bytes
BlockDesc[]        blockCount * 24 bytes
EntryDesc[]        entryCount * 48 bytes
pathPool           pathPoolSize bytes
```

### 3.1 TocHeader, 24 bytes

| Offset | Field | Type | Notes |
| ---: | --- | --- | --- |
| 0 | `entryCount` | `u32` | files plus directories |
| 4 | `blockCount` | `u32` | |
| 8 | `pathPoolSize` | `u32` | |
| 12 | `reserved` | `u32` | zero |
| 16 | `totalRawBytes` | `u64` | sum of entry sizes, drives the progress UI |

The `reserved` word at offset 12 is padding that keeps `totalRawBytes` and the
following `BlockDesc` array 8-byte aligned, so the stub can point structs
directly at the decompressed buffer instead of copying field by field.

### 3.2 BlockDesc, 24 bytes

| Offset | Field | Type | Notes |
| ---: | --- | --- | --- |
| 0 | `compOffset` | `u64` | relative to `archiveOffset` |
| 8 | `compSize` | `u32` | |
| 12 | `rawSize` | `u32` | at most `MAX_BLOCK_RAW` |
| 16 | `codec` | `u8` | |
| 17 | `flags` | `u8` | |
| 18 | `dictId` | `u16` | 0 = none, reserved for v2 |
| 20 | `reserved` | `u32` | zero |

`dictId` is reserved now precisely so that turning on zstd dictionaries later is
not a format break.

### 3.3 EntryDesc, 48 bytes

| Offset | Field | Type | Notes |
| ---: | --- | --- | --- |
| 0 | `pathOffset` | `u32` | byte offset into `pathPool` |
| 4 | `blockIndex` | `u32` | first block; `U32_MAX` for directories |
| 8 | `offsetInBlock` | `u64` | 0 when `ENTRY_FLAG_SPANS_BLOCKS` |
| 16 | `size` | `u64` | uncompressed bytes |
| 24 | `mtime` | `u64` | Windows `FILETIME`, 100ns ticks since 1601 UTC |
| 32 | `hash` | `u64` | XXH3-64 of contents, 0 for directories |
| 40 | `attrs` | `u32` | filtered `FILE_ATTRIBUTE_*`, see 3.5 |
| 44 | `type` | `u8` | 0 = file, 1 = directory, 2 = symlink (rejected in v1) |
| 45 | `flags` | `u8` | |
| 46 | `blockSpan` | `u16` | block count when `SPANS_BLOCKS`, else 0 |

### 3.4 Entry flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `ENTRY_FLAG_SPANS_BLOCKS` | occupies `blockSpan` consecutive exclusive blocks |
| 1 | `ENTRY_FLAG_EXECUTABLE` | hint only, marks the primary exec target |

### 3.5 Attributes

Only these survive a round trip: `READONLY`, `HIDDEN`, `SYSTEM`, `ARCHIVE`.
Everything else (`DIRECTORY`, `REPARSE_POINT`, `COMPRESSED`, `ENCRYPTED`,
`TEMPORARY`) is stripped at pack time. Restoring `REPARSE_POINT` or `ENCRYPTED`
on extract would either fail outright or do something surprising, and `DIRECTORY`
is already implied by `type`.

`READONLY` needs specific care: apply it **after** writing the file and **after**
`SetFileTime`, otherwise both the write and the timestamp update fail.

### 3.6 pathPool

UTF-8, NUL-terminated strings, concatenated. Paths are **relative**, use forward
slashes, and carry no leading or trailing separator. Directory entries sort
before any file they contain, so one forward pass can create the tree.

Every path is validated at both pack time and unpack time. See section 6.

---

## 4. Blocks

### 4.1 Sizing

Target `DEFAULT_BLOCK_RAW` = 8 MiB of uncompressed content per block. Blocks are
compressed independently, which is the entire point: a block whose files are all
unchanged is never read from disk and never decompressed, and blocks decompress
in parallel across cores.

The cost is compression ratio. A single solid stream compresses better, but it
forces every launch to decompress everything, which defeats the skip logic this
format exists to serve.

### 4.2 File placement

A file lives in exactly one block whenever it fits. Files are never split across
a block boundary for packing convenience, which keeps the extractor inner loop a
straight copy out of one decompressed buffer.

A file larger than `MAX_BLOCK_RAW` gets a run of `blockSpan` **exclusive**
consecutive blocks containing nothing else. The extractor decompresses those one
at a time and streams them to disk, which is what bounds worker memory to
`MAX_BLOCK_RAW` per thread. Without this rule a single 4 GB file inside an
archive would demand a 4 GB buffer.

### 4.3 Pack ordering

Files are sorted by **(extension, parent directory, name)** before grouping.
Clustering files of the same type materially improves the solid ratio within a
block: 200 DLLs together compress far better than one DLL next to one PNG next
to one JSON.

---

## 5. Codecs

| Id | Codec | Stub | Packer |
| ---: | --- | --- | --- |
| 0 | store (raw copy) | yes | yes |
| 1 | LZ4 block | yes | yes |
| 2 | Zstandard | yes | yes |
| 3 | LZMA | reserved | reserved |
| 4 | Brotli | reserved | reserved |

The stub links **decompress-only** builds. The packer links the full libraries.
Every codec is reached through a single dispatch function, so adding one touches
exactly one file.

A block records its own codec, so one archive may mix them. Already-compressed
content (PNG, ZIP, MP4) is best stored; text and code go to zstd.

---

## 6. Path validation

The archive controls where bytes land on disk, so path handling is the one place
where a bug becomes a code-execution vector. Validation runs at pack time **and**
unpack time, and a violation rejects the **entire archive** rather than skipping
or sanitising the offending entry. A partially applied archive is worse than a
refused one.

Rejected:

- absolute paths, drive letters, leading `/` or backslash
- any `..` path **component**, compared per component and not as a substring, so
  a legitimate file named `..foo` still packs
- `:` anywhere, which blocks NTFS alternate data streams
- reserved DOS device names, with or without extension, case-insensitive:
  `CON`, `PRN`, `AUX`, `NUL`, `COM1` through `COM9`, `LPT1` through `LPT9`
- components ending in a dot or a space, because Windows silently strips those,
  which would let two distinct entries collide on one real filename
- empty components, non-UTF-8 bytes, embedded NULs
- `type == symlink` in v1

After joining to the target directory, the canonicalised result must still sit
inside the target. That containment check is the backstop for anything the
component rules missed.

Structural bounds are checked with the same severity:

- `tocRawSize` against `MAX_TOC_RAW` before allocating
- `blockIndex < blockCount`
- `offsetInBlock + size <= block.rawSize`, computed without overflow
- `compOffset + compSize <= archiveSize`

---

## 7. State file

Written to `<targetDir>/.hnx/state.bin` **after** all extraction completes. A
launch that finds nothing to extract also writes it when the existing file is
missing or names a different buildId; that write is best effort.

```
magic        u8[8]   "HNXSTATE"
version      u32     1
reserved     u32     zero
buildId      u64     copied from the footer
entryCount   u32
reserved2    u32     zero
records[]    entryCount * { size u64, mtime u64, hash u64 }
```

Records follow TOC entry order, so no path strings are stored. The TOC supplies
them.

Writing this file last is deliberate. A crash mid-extraction leaves no state file
at all, so the next run falls back to full verification instead of trusting a
half-written tree. It is written to `state.tmp` and renamed with
`MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, so it is never observed partially
written.
