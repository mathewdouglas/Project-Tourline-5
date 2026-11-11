# What Tourline Prologue has that Project Tourline 5 does not (2026-11-11)

Compared Project Tourline 5 0.1.0 (`2168f51`) with Tourline Prologue
v0.2.5 (`7d0ab12`): source trees, settings keys, F1 page rows,
`config/targets.json`, scripts, launcher and docs, and Prologue Phases
134 to 237. What was already ported is in
[prologue-sync-review.md](prologue-sync-review.md); this list is what is left.

**Effort**: S = hours, M = a day or two, L = a week or more, with GT5
measurements counted. **Value** is for a GT5 player.

**Follow-up, GT5 Phase 37:** the native setup launcher, storage selection and
game-file pickers described in item 2.1 are now implemented. See
[setup and storage](setup.md). The earlier 0.1.0 ZIP remains a manual-setup
release; starter/catalogue and preparation helpers below are still deferred.

## 1. Features in our code that do nothing on GT5

These ship in PT5 0.1.0 and appear in F1 or as defaults, but are keyed to
Prologue's screen scripts (`.gpb` names), its texture hashes or its
executable's addresses. GT5 has none of those, so on GT5 they are inert or
untested. Each needs a GT5 measurement, not a port. This is the most
important group: a player can switch these on and nothing happens.

| # | Feature (Prologue phase) | Why it is inert on GT5 | What to measure | Effort | Value |
| --- | --- | --- | --- | --- | --- |
| 1.1 | **Skip opening movie** (F1 GAME; Phases 67, 234) | Sets `PAD_SKIP_MOVIE=MovieRoot`, a `.gpb` milestone GT5 never reads. Prologue 0.2.5 instead hooks the game's own Opening Movie option at Prologue addresses. | The file GT5 reads when the movie starts (`PAD_FILE_LOG=2`, as Phase 36 did), or GT5's own option. | S (file marker) / M (native option) | High |
| 1.2 | **Button prompts follow the controller** (Phase 119) | `assets/glyphs/textures.txt` lists BCUS98158 texture hashes. GT5's button textures are different, so Xbox/Nintendo/keyboard art is never drawn. | Dump GT5's button textures with `RSX_TEX_REPL_DUMP`, add their hashes and cell boxes. | M | High |
| 1.3 | **Ultrawide 21:9** (F1 VIDEO; Phases 121 to 126, 130) | The aspect constants (`aspect_16_9_float`, `ui_aspect_16_9_float`, `scene_camera_aspect_16_9_float`) are not known for v104, and `src/main.cpp` skips all ultrawide setup without them, so choosing 21:9 leaves GT5 at 16:9. | Find GT5's aspect table (Phase 121's method), then check race, menus and HUD. | M | High for ultrawide users |
| 1.4 | **F1 pauses a race** (Phase 117) | Needs `RaceRoot.gpb` and `menu_listbox_vtable`; GT5 has neither, so F1 opens over a running race. | Race scope now exists (Phase 36); find GT5's pause-menu widget for the "already paused" check. | S to M | Medium |
| 1.5 | **"SELECT PC settings" hint on the game's Options screen** (Phase 114) | Keyed to `OptionRoot.gpb`. | GT5's Options screen file. | S | Low |
| 1.6 | **Frame-pacer padding skipped** (Phase 106) | No `frame_pacer_sleep_lr` for v104. Prologue measured 55 to 58 FPS and much less late-frame copying from this. | Whether GT5's render thread pads frames the same way (`scripts/diag/rtsplit.py`). | M | Medium to high |
| 1.7 | **Guest spin waits yield** (Phase 101, required post-lift step) | No `spin_waits` for v104; the patch is a no-op. | GT5's spin loops on SPU progress (sampling profiles). | M | Medium |
| 1.8 | **Native SPU hot loops** (Phase 111) | Prologue replaces its race sorted-list walk. GT5 has one hook (`clear_words`) and no profile-led replacements. | `GT5P_SAMPLE_WID` profile of GT5's race workloads. | M to L | Medium |
| 1.9 | **AVX/FMA audio images** (Phase 93) | Fingerprints are Prologue's three mixer images; none match GT5's images, so the portable code always runs. | Profile GT5's sound worker; build FMA copies of its hot images. | M | Medium (audio underruns) |
| 1.10 | **Race scope outside Arcade** (our Phase 36) | Measured for Arcade only. GT Life events, licences, special events and replays are unmeasured. | `PAD_FILE_LOG=2` runs of each mode. | S to M | Medium |
| 1.11 | **My Page label mask fix** (Phase 130) | Prologue-only function; probably not applicable. | Confirm GT5 shows no cut-off labels in ultrawide. | S | Low |

## 2. Player experience and distribution

| # | Prologue has | PT5 0.1.0 has | Notes | Effort | Value |
| --- | --- | --- | --- | --- | --- |
| 2.1 | **Launcher** (`tourline-prologue.exe`): the player picks a dump, it verifies the build, decrypts `EMAIN.SELF` through RPCS3 on a temporary copy, remembers setup, chooses storage (`Change storage.cmd`), edits settings outside the game | A ZIP with `gt5.exe` and manual steps in `START-HERE.txt` | GT5 setup has more steps: install the 01.04 update, decrypt the update's `EBOOT.BIN`, provide the `PDIPFS` install. The launcher's hashing, storage and RPCS3 parts transfer; its build checks and layout are Prologue's. | L | Very high |
| 2.2 | **Bundled shader starter**: pipeline templates (105 files) and a shader enumeration, so a first launch has most pipelines ready | Nothing: every pipeline is learned in play | Needs the GT5 equivalent of Phases 137 to 216 (material/shader recovery from GT5's assets). GT5's catalogue would be built from GT5 data, never Prologue's. | L (largest item) | Very high (first-run stutter) |
| 2.3 | **Shader preparation helper** (`GT5P-Prepare.exe`, `Prepare shaders.cmd`, quick and full modes, resume, F1 Debug > Prepare shaders) | `scripts/precompile_shaders.py` (developer tool, needs Python and unpacked assets) | Depends on 2.2 for useful coverage. | L | High |
| 2.4 | **Background preparation**: race pipelines first, trickled into the live cache, paused during races, status and "Ready for next launch" notice (Phases 225, 226, 236) | No | Depends on 2.1 and 2.3. Race pausing can use our race scope. | M after 2.3 | High |
| 2.5 | **Prepared pipelines at the player's MSAA sample count** (Phase 223) | No | Depends on 2.3. | M | Medium |
| 2.6 | **First-run/upgrade offers**: one-time preparation offer, DXC migration notice, optional cleanup of old caches (Phases 219, 233) | No. 0.1.0's DXC switch leaves the FXC cache in place with no notice. | Matters from PT5 0.2 onwards, when players have 0.1.0 caches. | M | Medium |
| 2.7 | **Settings defaults migration** (`meta.defaults_version`) | No | Lets a release change a default for existing settings files deliberately. PT5 has already changed two defaults by hand. | S | Medium |
| 2.8 | **Community pipeline contributions** (collect, verify, contribute; F1 Debug rows, Phases 196 to 204) | No | Depends on 2.2 and a GT5 catalogue format. | L | Medium |
| 2.9 | **Release CI**: self-hosted GitHub Actions workflow that builds the player ZIP on version tags with a lift cache (`player-release.yml`, `ci_build.py`, `ci_cache.py`) | `scripts/release/clean_build.py`, run by hand | Our script already does the clean clone, private inputs, pinned downloads and reproducibility check; CI adds a runner and caching. | M | Medium |
| 2.10 | **RecompLauncher manifest** (`recomplauncher.json`, Phase 222) | No | Needs 2.1 first. | S after 2.1 | Low to medium |
| 2.11 | **`VERSION` file and versioned release notes** (`docs/release-v0.2.x.md`, release roadmap) | Version in `CMakeLists.txt`; notes in `CHANGELOG.md` | Cosmetic. | S | Low |
| 2.12 | **F1 "Test controller vibration"** button | Feedback options exist, no test button | Small UI port. | S | Low to medium |
| 2.13 | **Native Modern Controls** (F1 Controls; `input.modern_controls`) | No | Prologue hooks its game's control-assignment profile at measured addresses. GT5's controller settings must be found first. | M to L | Medium |
| 2.14 | **Race-set loading bar** (Phase 230) | No | Not needed: GT5 draws its own loading bar, and our race gate finishes in under a second. | — | None |

## 3. Renderer and runtime

| # | Prologue has | Notes | Effort | Value |
| --- | --- | --- | --- | --- |
| 3.1 | **ps3recomp v0.18.0** (Phases 134, 135; `docs/ps3recomp-upgrade.md`) | 542 upstream files changed. Prologue carries its own renderer, FIFO and SPURS scheduler as overrides; GT5's pager/mmapper, syscall, SPURS, DMA, movie and FIFO fixes would all need re-applying, then a re-lift and full GT5 acceptance. | L | Medium (maintenance, upstream fixes) |
| 3.2 | **Texture allocation work** (Phase 221): placed textures in 64 MB heaps, a texture pool, a persistent 64 MB staging ring; LRU slot eviction **on** by default | Prologue's race-start frame went from about 370 ms to 73 ms with these and 3.3. PT5 has the LRU port but off. Touches GT5's border cubes and copy/volatile texture lifetimes. | M to L | High (race-start hitch) |
| 3.3 | **Persistent fast pipeline lookup** (`fp-lookup-v2-<tag>.bin`, Phase 221) | Skips rebuilding shader text for pipelines already resident. Prologue measured pipeline lookup in the race-start frame at 86 ms falling to 2 ms. Its key must be re-derived from GT5's pipeline key. | M | High (race-start hitch) |
| 3.4 | **Shader interpreter fallback** with per-frame GPU budget and prepared interpreter pipelines (Phases 216, 220, 224) | Draws an approximate picture instead of skipping a draw while its shader compiles. Prologue keeps it opt-in; parity is incomplete. | L | Medium |
| 3.5 | **Translated preparation modes** for ultrawide and native-scaled settings (Phase 221 continued) | Depends on 2.3. | M | Low to medium |
| 3.6 | **Hitch tooling**: `diag/hitch_attribution.py`, `hitch_list.py`, `hitch_summary.py` | PT5 has the hitch trace (`RSX_HITCH_LOG`) and `frame_hitches.py`, but not these readers. Direct ports. | S | Medium (diagnosis) |
| 3.7 | **Runtime pipeline-key coverage** and shader coverage tools (Phases 136, 192) | Measures how much of play a prepared set covers; needed to judge 2.2. | M | Medium |

## 4. Not wanted for PT5

- Prologue's release notes, publication proofs, route sweeps and titles.
- Prologue's shader catalogue, templates, volume maps and executable
  checksums: these describe BCUS98158 and would mean nothing for GT5.
- Any Prologue guest address, vtable or texture hash.

## Suggested order

1. **Make the inert features work (group 1).** Most are small marker or
   hash measurements: skip movie (1.1), button prompts (1.2), F1 race pause
   (1.4), race scope outside Arcade (1.10). Then ultrawide (1.3) and
   frame pacing (1.6). Until then, hide or label the inert F1 rows on GT5,
   so a player does not switch on something that does nothing.
2. **Race-start hitches:** fast pipeline lookup (3.3), then the texture
   allocation work (3.2), measured with the hitch tools (3.6).
3. **Settings defaults migration (2.7)**, before 0.2 changes any default.
4. **Launcher (2.1)**, starting with the dump picker and RPCS3 decryption,
   since GT5 setup is the hardest part for a player.
5. **GT5 shader starter and preparation (2.2 to 2.6)**: the largest
   project, and the one that removes first-run stutter.
6. **Runtime v0.18 (3.1)** when a reason appears: it is maintenance, not a
   player feature.
