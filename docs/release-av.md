# Release antivirus investigation (2026-11-12)

The v0.1.2 player ZIP has SHA-256
`b9e0ebf13f022a852a69080f55dc07fe4ee1ffdd3bb36e36038bb98cac1801c2`.
Its original `gt5.exe` has SHA-256
`623b44ad007089bc9e8b1e079bf61dc9d15f297d28449450655196f816e0cb33`.
VirusTotal showed the same `Lazy` detection labels for the ZIP and executable;
local Defender detected `Trojan:Win32/Tecabans.STV!cl` in the ZIP but cleared
each executable/DLL when scanned individually without remediation. These
different results do not establish a source-code cause for those detections.

## Reproduced CAPE warning

The supplied CAPE HTML report for the executable contains one static YARA
warning, `vmdetect` by nex ("Possibly employs anti-virtualization techniques").
It has no process/API trace. Its network section lists a UDP entry to
`172.16.1.255:138` and no HTTP(S) requests; that entry alone does not identify
which process sent it.

The [public rule](https://github.com/Yara-Rules/rules/blob/master/antidebug_antivm/antidebug_antivm.yar)
matches **any** listed pattern anywhere in the file. Two of its MAC-address
patterns match ordinary generated function-name strings in the original EXE:

| Pattern | File offset | Containing string |
| --- | --- | --- |
| `000569` | `0x0E695269` | `gt5p_b0171D980_spu_func_0000569C` |
| `000C29` (case insensitive) | `0x0E738AE9` | `gt5p_b016B5B00_spu_func_0000C298` |

These matches do not demonstrate virtual-machine detection. CAPE does not
include its exact rule version or matched offsets, so reproduction with the
public rule is the evidence here. This rule is separate from the antivirus
vendors' `Lazy` and `Tecabans` detections.

## Generator change

`scripts/lift_spu_jobs.py` now removes the unused `name` column of the private
SPU registration table after lifting each image. Its consumer reads only the
guest address and host function pointer. The transformation preserves those
pairs, their order, the sentinel, all generated function bodies and their
linker symbols. It removes duplicate diagnostic data rather than renaming
functions or changing execution. The pinned runtime and its patch are unchanged.

`scripts/spu_registration.py` checks the expected format and refuses unexpected
rows, names, sentinels or a new `e->name` consumer. It accepts its own output
unchanged. Re-lift SPU images and rebuild to apply it to an existing build;
the already published v0.1.2 ZIP remains unchanged.

Validation: `python scripts/test/spu_registration.py` covers preservation,
idempotence, an empty table and rejected format/consumer changes. Private
v0.1.2 validation compares all 33,948 registration pairs across 52 images,
then compiles the transformed sources and their five FMA variants and links
them with the original release's other objects. No live game/test process or
shared runtime is changed. A validation-link output-path error briefly
replaced the local release build's EXE; it was restored byte-for-byte from
the original ZIP and its original SHA-256 verified. The published ZIP is unchanged.

YARA 4.5.4 reproduces the original `vmdetect` match and reports **no matches**
on the rebuilt validation EXE (244,743,680 bytes, SHA-256
`e7b69087975f73e32d365e28b86ee57c599e501c22d50e546c46770d83f8156d`).
The downloaded rule's SHA-256 is
`121e3c9797db611f08c30175ad693e5afcb974ccea7c598afb2a70bd168c327f`.
Validation artifacts are ignored under `scratch/av-validation/` in the fix
checkout. This is a validation relink of v0.1.2 objects, not a published
release or a gameplay test. No result for this rule guarantees clearance by
Defender or the other vendors; those still need vendor review.

## Bitdefender `Gen:Variant.Lazy` again, on PGO builds (2026-11-18)

Every profile-guided 01.04 `gt5.exe` (releases 0.1.4 to 0.1.10) was flagged
`Gen:Variant.Lazy.746136` by Bitdefender and its OEM engines (ALYac, Arcabit,
Emsisoft, GData, VIPRE, MicroWorld-eScan), plus CTX `exe.unknown.generic`.
0.1.3's `gt5.exe` and every `gt5_211.exe` (neither profile-guided) were clean;
`tourline5.exe` and the DLLs were clean apart from one Trapmine
`suspicious.low.ml.score`. 0.1.2's fix (`apply_env_defaults`) was still in
place: the cause this time was layout.

Located by uploading int3-filled copies of a relink of the 0.1.10 objects
(`scripts/release/vt.py variant`; each copy has a new PE timestamp):

| Filled | Result |
| --- | --- |
| CRT startup objects; `main.cpp.obj` | flagged |
| all of `.text` except those two | clean (only "swollen file" heuristics) |
| `.text` quarters 2, 3, 4 | flagged |
| quarter 1; then its first 7.3 MB; first 64 KB; first 16 KB | clean |
| any other eighth / 64 KB-7 MB slice / 16 KB slice | flagged |

So the score depends on the first 16 KB of `.text`, as in 0.1.2 (where that
was `main.cpp`'s code). In a profile-guided build `main()` runs once and is
cold (`.text$unlikely`), and lld-link's call-graph profile sort, on by
default when objects carry `.llvm.call-graph-profile`, puts hot renderer and
lifted functions at the start of `.text` instead. Neither change alone
cleared it: `main.cpp` compiled without the profile (cg sort still moves it)
and `/call-graph-profile-sort:no` alone (`main()` still cold) were both
flagged. Together they put `main.cpp`'s code first as in the non-PGO builds:
**0/69**.

Build change: the `gt5p` target links with `/call-graph-profile-sort:no`
(`-DGT5P_PGO_CG_SORT=ON` restores the sort) and `scripts/pgo/pgocc.py`
compiles `src/main.cpp` without the profile (`GT5P_PGO_PROFILE_MAIN=1`
profiles it). Speed, High Speed Ring `gt5boot.ps1 -Route race -FrameTest`,
alternating, 0.1.10 release engine vs the fixed relink of the same objects:
57.3 / 58.0 / 58.4 FPS vs 54.3 / 58.3 / 58.2 FPS (last pair back to back:
p99 24.6 vs 24.7 ms). No measurable cost.

Prevention: `clean_build.py --vt-key-file <file>` (or `VT_API_KEY`) scans the
ZIP and each `.exe`/`.dll` in it after packaging and fails on any detection;
without a key it prints `NOT SCANNED`. Scan before publishing. A file clean at
upload can still be flagged later when an engine updates, so rescan
(`vt.py scan --rescan`) when a report comes in. The model looks at the start
of `.text` and the entry point: check `/MAP` that `main.cpp.obj` is first
after any link-order or PGO change.

Allowed detections (`ALLOWED` in `scripts/release/vt.py`, 2026-11-18): only
Trapmine `suspicious.low.ml.score` on `tourline5.exe`, a low-confidence
machine-learning label present since 0.1.10 that no other engine shares. Any
other label, including another one from Trapmine, fails the scan.
