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
