# GT5 native Modern controls

Implemented 2026-11-15, adapted from Tourline Prologue `1c7cb3e`. Source and
CPU-only tests are complete. **Not yet linked or tested in game**: the existing
2.11 test session was left running, with its executable, runtime and data intact.

F1 > Controls > Modern controls changes player one's native SIXAXIS assignments:
Yes sets R2/RT accelerate, L2/LT brake, Cross/A gear up and Square/X gear down.
No restores Cross accelerate, Square brake, R2 gear up and L2 gear down. Other
bindings, right-stick driving, other devices/modes and player two stay unchanged.
Physical menu buttons and their pressure values are unchanged. Controller-family
support comes from the existing SDL/XInput paths.

The row follows the current game mapping, including edits made in Options. A
custom mapping displays No; explicitly picking No restores those four defaults.
Use the game's normal Save to persist native assignments. `input.modern_controls`
is the last F1 choice only, never a command at startup. An explicit environment
`PAD_MODERN_CONTROLS=0|1` queues a one-shot command after profile load and overrides
F1. `GT5P_NATIVE_CONTROLS=0` or `PAD_CONTROLS_POLISH=0` disables the link. Unsupported
targets display Unavailable; unloaded/invalid mappings display Waiting for game.

## Executable evidence

These facts were derived separately from each private ELF's script registrations,
controller virtual methods, constructors and default tables; no Prologue addresses
were copied. Hex addresses below are guest addresses.

| Fact | BCUS98114-v104 | BCUS98114-v211 |
| --- | --- | --- |
| ELF SHA256 | `1b0ebcd676bf4bb782fa598e97fa594c0e772249d4cb0a570ee984b5af1264d1` | `e3b93d86312d7f4fcc069bc1454366b0a4bcd377cc0fe9d414fd5aa4a0da63ad` |
| Player-one wrapper | `019AFEC0` | `017FDF70` |
| Wrapper vtable | `01867A28` | `016EB0B0` |
| Script getConfig | `0009AE80` | `0009ADA0` |
| Script setConfig | `0009AD50` | `0009A394` |
| Profile import | `0009AC38` | `0009A29C` |
| Profile export | `00099F44` | `0009AC64` |
| Default configuration descriptor | `01834454` | `016B6720` |

For 2.11, the getConfig/setConfig script registrations at `0009972C`/`00099744`
name descriptors `01723F08`/`01723EF8`. The controller implementation's
`00489EC4` selects wrapper zero at global `017FDF60 + 0x10`; initialization at
`0048A000`/`0048A170` attaches it to the first mode vector at global + 0x24.
Profile export/import descriptors `01723F00`/`01723EF0` occupy adjacent virtual
slots at `016C2834`/`016C2838`. For 01.04, the matching wrapper getter and
initializer use the module TOC (`019096F0`, derived from its call thunk), global
`019AFEB0` and wrapper vtable `01867A28` (`0054C888`). The native importer and
exporter were identified from their controller set/get paths, not address deltas.

Both default descriptors independently yield PLAY_NORMAL mode 1, SIXAXIS device
7 and identical pressure entries `(action,key)`:
`(4,29)` Cross, `(5,31)` Square, `(10,34)` L2, `(9,35)` R2.
The modern actions are respectively 9/10/5/4. Their default digital tables omit
these keys, so missing digital entries are allowed; existing corresponding entries
are cleared when applying a preset. Prologue's requirement that all four digital
entries exist would reject GT5's default configuration.

GT5 wrappers contain a **pointer** at +4 to a mode vector, unlike Prologue's inline
layout. Vector fields are begin/end/capacity at +4/+8/+12. Modes have stride 20,
devices stride 36, and mappings are two-byte action/key pairs. Each device has
pressure and digital vectors at +4 and +20. The link verifies the wrapper vtable,
committed memory, vector bounds/strides and unique required keys before any write.
It modifies four action bytes plus any present corresponding digital actions.

## Synchronization and validation

The UI publishes an atomic request. Execution applies it from guest control hooks or the guest
pad poll after profile import (or the controller Options getter/setter), never
from the UI thread. getConfig, setConfig and profile import/export serialize
against mapping access. The pad callback uses try-lock so a guest hook calling
cellPad cannot deadlock against its poll lock. Export flushes a pending request
before the game's own save representation is assembled. No save file is patched.
The runtime already exports the callback; this change does not edit its sources
or regenerate the cumulative patch. No post-lift step is added.

Run `python scripts/test/modern_controls.py` (Windows clang-cl/MSVC tools; optional
`PS3RECOMP_DIR`). It creates unique ignored scratch outputs, compiles production
controls for both target definitions, compiles the settings/UI consumers and
runs native fixtures. Checks cover Yes/No round trips, game-side/custom edits,
preserved unrelated bytes, absent digital keys, malformed layouts rejected before
writes with requests retained, deferred profile load, callback installation,
explicit startup command, disabled link, poll-lock contention and TOML parse/store
without a synthesized boot command. Explicit preload-off remains false.

Final passed evidence: `scratch/modern-controls-8wsczqsv`. The private disassembly/default-table
inspection is in `scratch/modern-controls` and contains no tracked game data.

Still required when the separate test session releases the game slot:

- Build both engines normally; enter Options and compare the native assignments
  with F1 Yes/No, including a pre-existing custom mapping.
- Confirm graded R2/L2 throttle/brake and face-button gear shifts in an Arcade
  race, without changing menu navigation, with Xbox and PlayStation pads.
- Save/restart each version and verify that native assignments persist and the
  row reflects them. Check a fresh profile as well as an existing save.
