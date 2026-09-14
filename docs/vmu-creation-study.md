# Creating a blank VMU: what exists, what the format is, and what it takes (study)

Written 2026-09-13 from a reading pass over `runtime/src/maple/maple.cpp`, `runtime/boot/boot_main.cpp`
and the Flycast checkout in `build/flycast-src` (ADR 1 reference, GPL-2.0, so porting is allowed),
plus eight headless runs of the existing `build/dream-dev/games/crazytaxi/crazytaxi_boot`. No source
was changed. Every claim below is either a `file:line` citation or the output of a run that is
reproduced in full so it can be re-run.

**Headline: a blank card works today.** Crazy Taxi accepts a correctly formatted blank image,
enumerates it, and writes a real 23-block `CRAZYTAXI_DC` save to it — allocating exactly the blocks
and the FAT chain the owner's real Flycast card carries. The only thing missing is that nothing in
the project can *make* that image, so a first-time user has nothing to point `--vmu` at. The
smallest honest change is about half a day.

**And one thing is worse than "missing feature": `--vmu` will silently destroy whatever file you
point it at.** Section 6.1 has a run that proves it.

---

## 1. What exists already

### 1.1 The flag

`--vmu FILE` is declared at `runtime/boot/boot_main.cpp:967-969`, parsed at
`runtime/boot/boot_main.cpp:1042-1043`, documented in the help at
`runtime/boot/boot_main.cpp:907`, and wired at `runtime/boot/boot_main.cpp:1270-1281`:

```cpp
// runtime/boot/boot_main.cpp:1270
    dream::maple::MemoryCard* card_ptr = nullptr;
    if (!vmu.empty()) {
        auto card = std::make_unique<dream::maple::MemoryCard>();
        if (!card->load(vmu)) {
            std::fprintf(stderr, "cannot read the memory card image %s\n", vmu.c_str());
            return 2;
        }
        std::printf("memory card %s: %s\n", vmu.c_str(),
                    card->formatted() ? "formatted" : "blank (the title may offer to format it)");
        card_ptr = card.get();
        maple.attach_expansion(0, 0, std::move(card));
    }
```

With no `--vmu`, no card object is created and nothing is attached to the expansion slot: the guest
sees an empty slot. There is no default path and no environment variable — `git grep getenv` over
`runtime/` and `render/` finds only the `DREAM_*` trace switches.

### 1.2 The device

`dream::maple::MemoryCard` is declared at `runtime/include/dream/runtime/maple/maple.h:117-139`
(256 blocks × 512 bytes = 131,072 bytes, `kImageSize` at `:120`) and implemented at
`runtime/src/maple/maple.cpp:20-152`. It answers Device Request / All Status Request, Get Media
Info, Block Read, Block Write (storage and LCD), Get Last Error and Set Condition.

### 1.3 Does it create a missing file? No.

`runtime/src/maple/maple.cpp:20-27`:

```cpp
bool MemoryCard::load(const std::string& path) {
    path_ = path;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return true;  // no file yet: an unformatted card, which is what a blank one looks like
    in.read(reinterpret_cast<char*>(flash_.data()), static_cast<std::streamsize>(kImageSize));
    return in.gcount() > 0;
}
```

A missing file yields an all-zero image: a card that is *present* but *unformatted*. Nothing writes
the file at that point; `save()` is only called from the Block Write handler
(`runtime/src/maple/maple.cpp:140-141`), so if the title never writes, no file ever appears.
Verified — see 3.3, where a run against a non-existent path left nothing on disk.

### 1.4 What it does with a wrong-size or corrupt file

- **Wrong size is not detected at all.** `load()` reads up to `kImageSize` bytes into a buffer that
  is already zero-filled and returns true for any non-empty read. A 64 KB file becomes "the first
  64 KB of a card, the rest blank". A 29-byte text file becomes "a card whose first 29 bytes are
  that text". Both were accepted by the launcher in runs (3.4).
- **Zero-length file is the only rejected case**: `in.gcount() > 0` is false, so the launcher prints
  `cannot read the memory card image ...` and exits 2. Verified.
- **There is no corruption check beyond the format marker** (1.5). Nothing validates the FAT, the
  directory chain, or that the geometry in the root block matches a 128 KB card.
- **Any accepted file is rewritten in full on the first block write**, truncating anything past
  131,072 bytes, because `save()` opens a plain `std::ofstream` (truncating by default) and writes
  exactly `kImageSize`, `runtime/src/maple/maple.cpp:29-38`.

### 1.5 What `memory card <path>: formatted` means

That line is `runtime/boot/boot_main.cpp:1278-1279`. The predicate is
`runtime/src/maple/maple.cpp:41-47`:

```cpp
// A formatted card has its system area in the last block, starting with a run of 0x55.
bool MemoryCard::formatted() const noexcept {
    const std::uint8_t* root = flash_.data() + 0xFF * kBlockSize;
    for (unsigned i = 0; i < 16; ++i)
        if (root[i] != 0x55)
            return false;
    return true;
}
```

So `formatted` means exactly one thing: **bytes 0x1FE00..0x1FE0F of the loaded image are all 0x55**.
It is not a validity check. It says nothing about the file's size (a 200 KB file with the marker in
the right place reports "formatted"; a truncated 64 KB copy of a perfectly good card reports
"blank", because the root block was cut off — verified in 3.4). It is also the switch that decides
what Get Media Info answers: a formatted card returns its own 24-byte geometry block copied from
root+0x40 (`runtime/src/maple/maple.cpp:85-87`), an unformatted one returns a hard-coded standard
geometry so the title could format it (`:88-101`).

### 1.6 Status in the plan

`docs/progress.md:35` records WP2.4 as done including the card; `docs/progress.md:46` has WP3.3
("Katana specifics (Manatee, ADX streaming, VMU)") as `todo`. Card *creation* is not named in any
work package, so this is new scope — small, but it should get a line in WP3.3.

---

## 2. The on-disc format, precisely

### 2.1 How this was verified

Three independent sources, and they agree byte-for-byte where they overlap:

1. **The owner's real card**, `~/Library/Application Support/Flycast/data/MK-51035_vmu_save_A1.bin`,
   copied to a scratch directory and only read. 131,072 bytes; formatted by Flycast's default image,
   which is itself a dump of a BIOS-formatted card.
2. **Flycast's built-in blank card**: `build/flycast-src/core/hw/maple/maple_devs.cpp:329-352` is
   `u8 vmu_default[]`, a 276-byte zlib stream, inflated into the 128 KB image by
   `maple_sega_vmu::initializeVmu()` at `build/flycast-src/core/hw/maple/maple_devs.cpp:418-429`.
   I inflated it and dumped it.
3. **Flycast's field names** for the geometry block, from the Get Media Info handler at
   `build/flycast-src/core/hw/maple/maple_devs.cpp:529-560` (`total_size`, `partition_number`,
   `system_area_block`, `fat_area_block`, `number_fat_areas_block`, `file_info_block`,
   `number_info_blocks`, `volume_icon`, `save_area_block`, `number_of_save_blocks`).

**KallistiOS is not present on this machine** (searched the filesystem for `vmufs*`/`kallistios*`;
`tests/kos/` holds only the `hello` program). So the field *names* below come from Flycast, and the
field *values* from two real images. Where I could not name a field, I say so rather than guess.

Result: `flycast_blank.bin` (inflated) and `real.bin` (owner's card) have **identical** block 255,
and their FATs differ only in the 45 bytes covering entries 177..199 — the owner's save.

### 2.2 A blank card, field by field

Image is 131,072 bytes: 256 blocks of 512. All multi-byte fields are **little-endian**.
Everything not listed is zero.

**Block 255 (root / system area), file offset 0x1FE00:**

| Offset in block | Size | Value | Field | Where verified |
|---|---|---|---|---|
| 0x00 | 16 | `55` ×16 | format marker | identical in both references; it is what `MemoryCard::formatted()` reads (`runtime/src/maple/maple.cpp:41-47`) |
| 0x10 | 1 | `01` | custom volume colour flag | both references |
| 0x11 | 1 | `FF` | colour, blue | both references |
| 0x12 | 1 | `FF` | colour, green | both references |
| 0x13 | 1 | `FF` | colour, red | both references |
| 0x14 | 1 | `64` | colour, alpha (100) | both references |
| 0x15..0x2F | 27 | 0 | reserved | both references |
| 0x30..0x37 | 8 | `19 98 11 27 00 00 59 04` | BCD timestamp: century, year, month, day, hour, minute, second, weekday | both references, identical — it is the stamp baked into Flycast's default image (1998-11-27), not a live clock read |
| 0x38..0x3F | 8 | 0 | reserved | both references |
| 0x40 | 2 | `FF 00` (255) | `total_size` (last block) | flycast `maple_devs.cpp:534-537`; both images |
| 0x42 | 2 | `00 00` | `partition_number` | same |
| 0x44 | 2 | `FF 00` (255) | `system_area_block` | same |
| 0x46 | 2 | `FE 00` (254) | `fat_area_block` | same |
| 0x48 | 2 | `01 00` | `number_fat_areas_block` | same |
| 0x4A | 2 | `FD 00` (253) | `file_info_block` (directory) | same |
| 0x4C | 2 | `0D 00` (13) | `number_info_blocks` | same |
| 0x4E | 1 | `05` | `volume_icon` | both images carry 5; Flycast's *unformatted* reply sends 0 (`maple_devs.cpp:548`), as does ours (`runtime/src/maple/maple.cpp:95`) — so 5 is what a real format writes, 0 is what a card-less reply claims. Cosmetic. |
| 0x4F | 1 | `00` | reserved | both |
| 0x50 | 2 | `C8 00` (200) | `save_area_block` — where VMU mini-games (executable files) live, blocks 200..230 | flycast field name; both images |
| 0x52 | 2 | `1F 00` (31) | `number_of_save_blocks` | same |
| 0x54 | 4 | `00 00 80 00` | Flycast's comment calls it "reserved0 (something for execution files?)" (`maple_devs.cpp:558`). **I could not determine what the 0x80 at +0x56 means.** Both real images carry it; Flycast's *unformatted* reply sends four zero bytes. Reproduce it. |
| 0x58..0x1FF | — | 0 | unused | both |

The 24 bytes at root+0x40..0x57 are exactly what our Get Media Info returns for a formatted card
(`runtime/src/maple/maple.cpp:85-87` copies 24 bytes from that offset), so getting them right is
what makes the geometry self-consistent.

**Block 254 (FAT), file offset 0x1FC00:** 256 little-endian 16-bit entries, entry *n* describing
block *n*.

| Entries | Value | Meaning |
|---|---|---|
| 0..240 | `FC FF` (0xFFFC) | unallocated / free — this is the whole 200-block user area plus 200..240 |
| 241 | `FA FF` (0xFFFA) | allocated, last block of a chain (the directory's tail) |
| 242..253 | *n*−1 | the directory chain: 253→252→…→242→241 |
| 254 | `FA FF` | the FAT block itself: allocated, end of chain |
| 255 | `FA FF` | the root block: allocated, end of chain |

Verified against both references; the only difference between them is that the owner's card also has
177 = 0xFFFA and 178..199 = *n*−1, the 23-block Crazy Taxi save.

**Blocks 241..253 (directory), file offsets 0x1E200..0x1FBFF:** all zero on a blank card (checked
every byte of all thirteen blocks in both references). Entries are 32 bytes; the first one lives at
**block 253 offset 0** and the area grows downward toward 241. For reference (not needed to create a
blank card), the entry layout derived from the two live entries I observed is:

```
+0x00 u8   file type      0x33 data, 0xCC game
+0x01 u8   copy protect   0x00 copyable, 0xFF protected
+0x02 u16  first block    (LE)
+0x04 12   filename       ASCII, space/NUL padded  e.g. "CRAZYTAXI_DC"
+0x10 8    BCD timestamp  century, year, month, day, hour, minute, second, weekday
+0x18 u16  size in blocks (LE)
+0x1A u16  header offset in blocks (LE)
+0x1C 4    reserved, zero
```

**Blocks 0..199 (user area):** zero.

Note that Flycast's `vmu_default[]` is *not* perfectly blank: after inflation, block 0 offsets
0/2/5/6 and sixteen bytes around block 11 offset 0x188 hold leftover junk from whatever card it was
made from. It is harmless (the FAT marks those blocks free and the directory is empty) and should
not be reproduced.

### 2.3 Reconstruction check

I wrote the layout above from scratch in Python and diffed it against the inflated Flycast image:
**20 differing bytes, all of them in blocks 0 and 11** — i.e. only the junk. The spec above is
therefore complete and exact.

### 2.4 What to port, and what *not* to confuse it with

The thing to port is `initializeVmu()` +`vmu_default[]`
(`build/flycast-src/core/hw/maple/maple_devs.cpp:329-352` and `:418-429`), but **port the meaning,
not the blob**: writing the fields explicitly avoids a zlib dependency in `dream_runtime`, avoids
carrying the junk blocks, and is reviewable. The field names should be taken from the Get Media Info
handler at `build/flycast-src/core/hw/maple/maple_devs.cpp:529-560`.

**`runtime/src/hle/flash.cpp` is a different filesystem and must not be used as a model.** It is the
*console's* flash — also 128 KB, which is the trap. It has 64-byte blocks, a `KATANA_FLASH____`
magic, partition headers, CRC-16-protected user blocks and free bitmaps
(`runtime/include/dream/runtime/hle/flash.h:1-45`, `Flash::format` at `runtime/src/hle/flash.cpp:62-83`,
`Flash::crc` declared at `runtime/include/dream/runtime/hle/flash.h:41`). **The VMU filesystem has
no CRC anywhere.** Do not reach for `hle::Flash::crc` by analogy.

---

## 3. Does a blank card actually satisfy Crazy Taxi? Yes — tested

All runs used the already-built `build/dream-dev/games/crazytaxi/crazytaxi_boot` (mtime 2026-09-13
15:59). Nothing was built. The owner's card was copied to a scratch directory first and every run
used a fresh copy.

### 3.1 600 frames, title screen only

```
crazytaxi_boot --config games/crazytaxi/crazytaxi.toml --vmu <card> --max-frames 600 --rtc-seed 1000000
```

| Card | launcher line | `memory card:` report line | maple commands |
|---|---|---|---|
| my blank image | `formatted` | 15 block reads, 0 block writes, 1 media-info reads, 0 screen writes | `01:1733 09:576 0a:1 0b:15` |
| copy of owner's real card | `formatted` | 38 block reads, 0 block writes, 1 media-info reads, 20 screen writes | `01:1733 09:576 0a:1 0b:38 0c:20` |
| no `--vmu` at all | (no line) | (no line) | `01:1732 09:576` |

Read that arithmetic: **15 = root (255) + FAT (254) + 13 directory blocks.** The title read the
geometry once and then walked the entire directory — which is what a filesystem driver does when it
finds a valid card and is looking for its save. **38 = those 15 plus the 23 blocks of the owner's
save.** With no card, the storage commands (`0a` Get Media Info, `0b` Block Read, `0c` Block Write)
never appear at all and there is one fewer Device Request.

So the blank card is not merely accepted, it is *parsed*.

### 3.2 1800 frames with scripted input — the game saves

```
crazytaxi_boot --config games/crazytaxi/crazytaxi.toml --vmu <blank copy> --max-frames 1800 \
  --rtc-seed 1000000 --press start@120,start@400,start@700,a@1000,start@1300
```

```
memory card <path>: formatted
memory card: 38 block reads, 100 block writes, 1 media-info reads, 27 screen writes
maple frames by command: 01:5333 09:1776 0a:1 0b:38 0c:127 0d:25
```

Inspecting the image afterwards:

- FAT entries 177 = 0xFFFA and 178..199 = *n*−1 — **the same 23-block chain, in the same blocks, as
  the owner's real card.**
- A directory entry at block 253 offset 0: type `0x33`, first block 199, name `CRAZYTAXI_DC`,
  size 23 blocks, timestamp `19 70 01 12 13 46 56 00` — 1970-01-12 13:46:56, which is exactly what
  `--rtc-seed 1000000` should produce. The RTC path is right too.
- User blocks 177..199 non-zero, everything else still zero.

**The game created its save on a card this project has never been able to produce.** That is the
whole answer to question 3.

### 3.3 The same run against a path that does not exist

```
memory card <path>/missing.bin: blank (the title may offer to format it)
memory card: 1 block reads, 0 block writes, 1 media-info reads, 103 screen writes
maple frames by command: 01:5333 09:1776 0a:1 0b:1 0c:103
```

and afterwards `ls` reports no such file. So an unformatted card is a hard stop: the title reads
**one** block (the root), sees no `0x55` marker, and gives up. It does not offer to format — the
comment at `runtime/src/maple/maple.cpp:24` and the launcher's "the title may offer to format it"
at `runtime/boot/boot_main.cpp:1279` are, for Crazy Taxi, **wrong**. It also drives the card's LCD
about four times as much as in the working case (103 vs 27 screen writes), which is consistent with
an animated warning, though I cannot prove that from counters alone.

**What I could not verify:** the actual on-screen text. `--screenshot-at` is only honoured on the
windowed path (`runtime/boot/boot_main.cpp:1332` assigns it to the `Live` object, and the headless
path never constructs one), so there is no way to capture the "No VMU found" dialog headless, and I
did not open a window in this session. The block-read/block-write counters above are the evidence;
if the owner wants the dialog confirmed visually, one `--window` run with a blank card and one with
a missing path will do it in a minute.

### 3.4 Wrong-size and junk files

| Input | Launcher says | Outcome |
|---|---|---|
| 0 bytes | `cannot read the memory card image ...`, exit 2 | the only rejection that exists |
| 64 KB (first half of the owner's real card) | `blank (the title may offer to format it)` | wrong size undetected; the root block was cut off so the card reads as blank. 1 block read, 0 writes. The file survived only because Crazy Taxi refuses to format. |
| 29-byte text file | `blank (the title may offer to format it)` | accepted as a card |
| 131,109 bytes (a valid card + 37 trailing bytes) | `formatted` | game saved; **the file came back 131,072 bytes and the trailing bytes were gone** |

That last row is a real run, not a thought experiment. See 6.1.

---

## 4. Where the file should live, and what it should be called

### 4.1 Per-game, not shared

Recommend **per-game**, matching what the owner is already using. Flycast does this: slot A1 uses
`<gameId>_vmu_save_A1.bin` when `PerGameVmu` is on (`build/flycast-src/core/oslib/oslib.cpp:50-72`),
which is precisely how the owner's `MK-51035_vmu_save_A1.bin` got its name. Reasons, in order:

1. A 200-block card is ample for one title (Crazy Taxi takes 23) but the owner runs several; a
   shared card eventually needs a card-management UI this project does not have and should not grow.
2. A card corrupted by one title costs one title's saves.
3. Two titles running at once cannot interleave whole-file rewrites into each other (see 6.3).

The cost is that a title which reads another title's save (Sonic Adventure-style unlocks) would not
see it. No title in the owner's library is known to do that, and `--vmu` remains an override for
anyone who wants one shared card.

### 4.2 Under the platform's per-user directory, never beside the TOML

`docs/future-enhancements.md:53-58` already settled the principle for input bindings:

> **A per-user file gets the actual bindings**, keyed by game and by gamepad identifier, under the
> platform's configuration directory. This is the part that must not be committed: the game TOML is
> a tracked file, so putting a player's keys in it would mean every rebind dirties the repository.

A save file is the same argument only stronger — it changes every time the player plays. So: **not**
`games/<id>/`, and **not** a path in `games/<id>/<id>.toml`. Be explicit in the commit message that
this follows that section, and that the ADR `docs/future-enhancements.md:65-70` asks for (a runtime
configuration loader separate from `GameConfig`, which must stay free of runtime dependencies per
ADR 1) should cover the save path when it is written.

Note the distinction the freedesktop spec draws: a save is *data*, not *configuration*. Proposed:

| Platform | Directory |
|---|---|
| macOS | `$HOME/Library/Application Support/dream-recomp` |
| Windows | `%APPDATA%\dream-recomp` |
| Linux | `$XDG_DATA_HOME/dream-recomp`, else `$HOME/.local/share/dream-recomp` |

and within it: **`saves/<game.id>/vmu_a1.bin`** — `a1` because the hardware address is port A slot 1,
leaving room for a second card without renaming anything. `<game.id>` (`crazytaxi`) rather than the
product code, because that is the key the rest of the project already uses (`docs/game-config.md`).

There is no path helper in the project today (`git grep` for `XDG_CONFIG_HOME`, `Application
Support`, `APPDATA` over `runtime/`, `render/`, `translator/`, `tools/` returns nothing), so this
adds one — and input binding will need the same one, which is a point in its favour.

### 4.3 .gitignore

`.gitignore` already covers this three ways: `*.bin`, `*.vmu`, `*.vms`/`*.vmi`. A card written under
the recommended per-user directory is outside the repository and cannot be committed at all.

**One trap worth knowing:** `.gitignore` has `!tests/**/*.bin`, which un-ignores `.bin` files under
`tests/`. A card written to `tests/…/vmu.bin` by a stray `--vmu` would be committable. So: never
default a path inside the repo, and if a unit test writes a card it must write to
`std::filesystem::temp_directory_path()`, not into the source tree.

---

## 5. Implementation plan

Five changes. The first two are the minimum that fixes the user-facing problem; 5.3 is the
protection that should not ship separately from them; 5.4 is the polish; 5.5 is tests.

### 5.1 `MemoryCard::format()` — the new code

**`runtime/include/dream/runtime/maple/maple.h`, after line 126 (`bool save() const;`), add:**

```cpp
    // Overwrites the image in memory with a blank, formatted card: the root block's format marker
    // and geometry, a FAT with the directory chain allocated and every user block free, and an
    // empty directory. Byte-identical to what the console's own format writes (verified against a
    // BIOS-formatted card and Flycast's default image; docs/vmu-creation-study.md). Does not save.
    void format() noexcept;
```

**`runtime/src/maple/maple.cpp`, insert after `formatted()` ends at line 47:**

```cpp
// A blank card exactly as the console formats one. The VMU filesystem has no checksums anywhere;
// do not reach for hle::Flash::crc, which belongs to the console's own flash and is unrelated.
// Ported from Flycast's vmu_default image and its media-info field names (GPL-2.0, ADR 1:
// core/hw/maple/maple_devs.cpp initializeVmu() and the MDCF_GetMediaInfo handler).
void MemoryCard::format() noexcept {
    std::fill(flash_.begin(), flash_.end(), std::uint8_t{0});
    std::uint8_t* const root = flash_.data() + 0xFF * kBlockSize;
    // Explicit byte stores, not u16 casts: the on-card fields are little-endian whatever the host
    // is, and a memcpy of a host u16 would be wrong on a big-endian build.
    const auto put16 = [](std::uint8_t* p, unsigned v) {
        p[0] = static_cast<std::uint8_t>(v & 0xFF);
        p[1] = static_cast<std::uint8_t>(v >> 8);
    };
    std::memset(root, 0x55, 16);  // format marker; this is what formatted() reads
    root[0x10] = 0x01;            // custom volume colour, opaque white
    root[0x11] = root[0x12] = root[0x13] = 0xFF;
    root[0x14] = 0x64;
    // Creation stamp in BCD (century, year, month, day, hour, minute, second, weekday). The
    // console stamps the format date; both reference images carry 1998-11-27, and no title has
    // been seen to read it, so it is a constant rather than a call into the RTC.
    static constexpr std::uint8_t kStamp[8] = {0x19, 0x98, 0x11, 0x27, 0x00, 0x00, 0x59, 0x04};
    std::memcpy(root + 0x30, kStamp, sizeof kStamp);
    put16(root + 0x40, 0x00FF);  // total size: last block
    put16(root + 0x42, 0x0000);  // partition number
    put16(root + 0x44, 0x00FF);  // system area block
    put16(root + 0x46, 0x00FE);  // FAT block
    put16(root + 0x48, 0x0001);  // FAT blocks
    put16(root + 0x4A, 0x00FD);  // directory block
    put16(root + 0x4C, 0x000D);  // directory blocks
    root[0x4E] = 0x05;           // volume icon
    put16(root + 0x50, 0x00C8);  // first block of the executable-file area
    put16(root + 0x52, 0x001F);  // blocks in it
    put16(root + 0x56, 0x0080);  // unidentified; both reference cards carry it
    std::uint8_t* const fat = flash_.data() + 0xFE * kBlockSize;
    for (unsigned b = 0; b < kBlocks; ++b) put16(fat + b * 2, 0xFFFC);  // free
    put16(fat + 0xFF * 2, 0xFFFA);  // root: allocated, end of chain
    put16(fat + 0xFE * 2, 0xFFFA);  // FAT: allocated, end of chain
    put16(fat + 0xF1 * 2, 0xFFFA);  // directory tail (block 241)
    for (unsigned b = 0xF2; b <= 0xFD; ++b) put16(fat + b * 2, b - 1);  // 253 -> 252 -> ... -> 241
    // The directory (241..253) and the 200 user blocks stay zero.
}
```

`<algorithm>` needs adding to the includes at `runtime/src/maple/maple.cpp:3-4` for `std::fill`
(or use `std::memset`, which is already included).

### 5.2 `load()` tells the caller what it found

The current `bool` cannot distinguish "missing" from "loaded", which is why the launcher cannot
decide anything. Replace it.

**`runtime/include/dream/runtime/maple/maple.h:122-124`, before:**

```cpp
    // Loads an image. A missing file leaves the card unformatted rather than absent, which is what
    // a blank card does. Returns false only when the file exists but cannot be read.
    bool load(const std::string& path);
```

**after:**

```cpp
    // What load() found. Missing means the path is free to create; WrongSize and Unreadable mean
    // the file is something else and the card must never write over it.
    enum class Load { Loaded, Missing, WrongSize, Unreadable };
    // Loads an image and remembers the path for save(). On WrongSize and Unreadable the path is
    // NOT remembered, so no later block write can touch the file.
    Load load(const std::string& path);
```

**`runtime/src/maple/maple.cpp:20-27`, before:** (quoted in 1.3) **after:**

```cpp
MemoryCard::Load MemoryCard::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        path_ = path;  // nothing there yet: the caller may format and save to it
        return Load::Missing;
    }
    const std::streamoff size = in.tellg();
    if (size != static_cast<std::streamoff>(kImageSize)) {
        path_.clear();  // not a card: never write to it
        return Load::WrongSize;
    }
    in.seekg(0);
    in.read(reinterpret_cast<char*>(flash_.data()), static_cast<std::streamsize>(kImageSize));
    if (in.gcount() != static_cast<std::streamsize>(kImageSize)) {
        path_.clear();
        return Load::Unreadable;
    }
    path_ = path;
    return Load::Loaded;
}
```

Note `save()` already no-ops on an empty `path_` (`runtime/src/maple/maple.cpp:30-31`), so clearing
the path is a complete interlock — but it makes Block Write return `kFileError` to the guest, which
is the honest answer.

### 5.3 The launcher: create when missing, never overwrite

**`runtime/boot/boot_main.cpp:1272-1281`, before:** (quoted in 1.1) **after:**

```cpp
    if (!vmu.empty()) {
        auto card = std::make_unique<dream::maple::MemoryCard>();
        switch (card->load(vmu)) {
            case dream::maple::MemoryCard::Load::Loaded:
                if (!card->formatted()) {
                    std::fprintf(stderr,
                                 "memory card %s: 128 KB but not formatted. Refusing to touch it; "
                                 "a title that formats it would erase whatever it holds. Move it "
                                 "aside and let the launcher create a new card.\n",
                                 vmu.c_str());
                    return 2;
                }
                std::printf("memory card %s: formatted\n", vmu.c_str());
                break;
            case dream::maple::MemoryCard::Load::Missing:
                if (no_create_vmu) {
                    std::fprintf(stderr, "memory card %s does not exist\n", vmu.c_str());
                    return 2;
                }
                card->format();
                if (!card->save()) {
                    std::fprintf(stderr, "cannot create the memory card image %s\n", vmu.c_str());
                    return 2;
                }
                std::printf("memory card %s: created, blank and formatted\n", vmu.c_str());
                break;
            case dream::maple::MemoryCard::Load::WrongSize:
                std::fprintf(stderr,
                             "%s is not a memory card image: a card is exactly %zu bytes. "
                             "Refusing to overwrite it.\n",
                             vmu.c_str(), dream::maple::MemoryCard::kImageSize);
                return 2;
            case dream::maple::MemoryCard::Load::Unreadable:
                std::fprintf(stderr, "cannot read the memory card image %s\n", vmu.c_str());
                return 2;
        }
        card_ptr = card.get();
        maple.attach_expansion(0, 0, std::move(card));
    }
```

**Create-if-missing, not `--create-vmu`.** The reasoning: creating a file that does not exist cannot
destroy anything, it is what the reference implementation does
(`build/flycast-src/core/hw/maple/maple_devs.cpp:437-476` — open, and if that fails, create), and a
flag the first-time user has to know about does not solve the first-time user's problem. The escape
hatch goes the other way: add `--no-create-vmu` (a `bool no_create_vmu = false;` beside the other
flags near `runtime/boot/boot_main.cpp:967`, parsed beside `--vmu` at `:1042`, one help line at
`:907`) for scripts that want a missing card to be an error.

Two things the switch deliberately does **not** do:

- It never formats a file that exists. A 128 KB file with no `0x55` marker could be a card from a
  tool we do not understand, or a card mid-corruption with a recoverable directory. Refusing is
  right; if the owner wants "format this existing file" later, that is an explicit `--format-vmu`
  with its own confirmation, not a default.
- It does not create parent directories. If `--vmu` names a path in a directory that does not exist,
  `save()` fails and the launcher now says so at startup rather than letting the first in-game save
  fail. For the default path in 5.4, `std::filesystem::create_directories` is required.

### 5.4 A default card so the flag is not needed

**Add `runtime/include/dream/runtime/host_paths.h` + `runtime/src/host_paths.cpp`** (and the source
to the list at `runtime/CMakeLists.txt:4-26`):

```cpp
namespace dream::host {
// The per-user directory for data this project writes on the player's behalf: saves now, input
// bindings later (docs/future-enhancements.md). Empty if the platform's variables are unset.
std::string data_dir();
// <data_dir>/saves/<game_id>/vmu_a1.bin, with the directories created. Empty on failure.
std::string default_vmu_path(const std::string& game_id);
}
```

with the three platform branches from 4.2. Then in the launcher, after the config is loaded and
before the block in 5.3:

```cpp
    // No --vmu on a play session: give the player the per-game card so a first run can save.
    if (vmu.empty() && window)
        vmu = dream::host::default_vmu_path(cfg.id);
```

**Guard it on `--window`.** A headless run that suddenly has a card attached answers more Maple
frames, so its report counters and its `--write-hash` stream change — see `docs/differential-harness.md`.
Defaulting a card into headless runs would silently invalidate every golden trace. Headless keeps
requiring an explicit `--vmu`.

Docs to update in the same commit: `README.md:183-184` (the `--vmu` line), `docs/runtime-maple.md`
(the "Memory cards (2026-09-12)" section — and fix the claim that an unformatted card leads to the
title offering to format, which 3.3 disproves for Crazy Taxi), the help text, and a note under
WP3.3 in `docs/progress.md:46`.

### 5.5 Tests

There are **no memory-card tests today** — `runtime/tests/test_maple.cpp` (119 lines) covers only the
bus and the controller. Add `runtime/tests/test_memcard.cpp` and add it to the source list in
`runtime/tests/CMakeLists.txt:2-17`. It needs no Maple rig for most of it; `MemoryCard` can be
driven directly through `handle()`.

1. **`format()` writes the exact bytes.** The 16 `0x55`; each geometry field at its offset
   (0x40=255, 0x44=255, 0x46=254, 0x48=1, 0x4A=253, 0x4C=13, 0x50=200, 0x52=31); FAT[0]=0xFFFC,
   FAT[241]=0xFFFA, FAT[242]=241, FAT[253]=252, FAT[254]=FAT[255]=0xFFFA; every byte of blocks
   241..253 zero; every byte of blocks 0..199 zero; `formatted()` true. **This is the test that
   catches a silent-corruption format** (6.5) — pin the FAT, not just the marker.
2. **Get Media Info on a formatted blank card** returns the 24 bytes at root+0x40, and they match
   the geometry in (1). Guards against the media-info path and `format()` drifting apart.
3. **Write/read round trip**: format, write the four phases of a block through `handle(kBlockWrite)`,
   read it back through `handle(kBlockRead)`, compare. Covers the big-endian block/phase word
   (`runtime/src/maple/maple.cpp:111` and `:128`) staying right.
4. **`load()` on a missing path returns `Missing`; format + save then produces a file of exactly
   131,072 bytes; loading it again returns `Loaded` and `formatted()`.** Round-trips the whole
   feature. Use `std::filesystem::temp_directory_path()` — never a path inside the repo (4.3).
5. **`load()` on a 64 KB file returns `WrongSize`, and a subsequent block write leaves the file's
   size and contents unchanged.** This is the regression test for 6.1 and the most valuable one here.
6. **`load()` on a 0-byte file returns `WrongSize`** (not `Unreadable`; either is defensible, pin
   whichever is implemented).

### 5.6 Effort

Engineer-days, in the project's units:

| Piece | Days |
|---|---|
| 5.1 `format()` + constants | 0.25 |
| 5.2 `load()` status enum | 0.25 |
| 5.3 launcher cases, `--no-create-vmu`, messages | 0.25 |
| 5.4 host paths + default per-game card + docs | 0.5 |
| 5.5 six unit tests | 0.5 |
| clang-format, CI on three platforms, review | 0.25 |
| **Total** | **2.0** |

**Minimum viable slice: 0.5 days** — 5.1 plus the `Missing` arm of 5.3, keeping `load()`'s `bool`.
That alone lets `--vmu ~/ct.bin` work for a user who has never had a card. But 5.2 and the
`WrongSize` arm are the ones that stop the launcher destroying a file, and shipping the convenience
without the guard would be the wrong order.

---

## 6. Risks

### 6.1 `--vmu` overwrites whatever you point it at (present-day, proven)

`save()` opens a truncating `std::ofstream` and writes exactly 131,072 bytes
(`runtime/src/maple/maple.cpp:29-38`) on **every block write** (`:140-141`), and `load()` accepts any
non-empty file (`:20-27`). A run against a 131,109-byte file (a valid card plus 37 bytes of trailing
text) came back 131,072 bytes with the text gone. A one-character typo in a `--vmu` path — the game
TOML, a screenshot, a disc image — is a destroyed file the moment the title saves. This is the
biggest risk in the area and it exists **today, before any of this work**. 5.2 plus the `WrongSize`
arm of 5.3 closes it; it is the reason to do this change at all, more than the convenience.

Related, smaller: creating a card by default (5.4) is only safe because it is confined to paths that
do not exist. Any future "format this" path must require an explicit flag.

### 6.2 Wrong size reads as "blank", which is the dangerous direction

A truncated card (interrupted copy, half-finished cloud sync) loses block 255, so `formatted()` says
no and the launcher reports `blank`. Crazy Taxi happens to refuse to touch an unformatted card, so
the file survived my run — but a title that *does* format would write a fresh card over a file that
still held 64 KB of the owner's save. Size checking fixes it; the format-marker check alone does not.

### 6.3 Torn writes

A 23-block save is 100 Block Write frames, and each one rewrites the whole 128 KB file. A crash or a
kill between them leaves a half-updated card, and the window is 100 times wider than it needs to be.
Cheap improvements, in order: write to `<path>.tmp` and `std::filesystem::rename` over the original
(atomic on all three platforms for same-directory renames), or only save on a phase-3 write. Not
required for this change, but worth a line in the commit so it is not forgotten. A shared card
across simultaneously running titles makes this much worse, which is another argument for 4.1.

### 6.4 Endianness

Two byte orders meet in this file and they are not the same one:

- **The VMU filesystem is little-endian** — the FAT's u16s, the geometry fields, the directory's
  block numbers.
- **The Maple frame's block-and-phase word is big-endian**, while the function selector before it is
  in the frame's own order. This is already documented at `runtime/src/maple/maple.cpp:76-79` and is
  the bug that made the card look absent once before (`docs/runtime-maple.md`, "Memory cards").

Write the filesystem fields with explicit byte stores (as in 5.1), not `memcpy` of a host `u16`, so
the code is correct by construction rather than by accident of the host. It will never be exercised
on a big-endian host, but the three-platform rule in ADR 16 is about not leaving that kind of trap.

### 6.5 No CRC — and the wrong CRC is a foot-gun

The VMU filesystem has no checksums. `runtime/src/hle/flash.cpp` does (`Flash::crc`,
`runtime/include/dream/runtime/hle/flash.h:41`), it is also a 128 KB image, and it is also called
"flash" — and it is a completely different format belonging to the console, not the card. A reviewer
skimming for prior art will land on it. Say so in the comment above `format()` (5.1 does).

### 6.6 What a wrong format looks like in-game

Measured, not assumed:

- **Missing the format marker: a clear refusal.** One block read, zero writes, no file created
  (3.3). The failure is loud and nothing is damaged. So the *coarse* failure mode is safe.
- **A nearly-correct card is the dangerous case.** If the marker and geometry are right but the FAT
  is wrong — say blocks 241..253 left as `0xFFFC` (free) instead of the directory chain — the card
  passes every check the title makes and then the allocator hands out directory blocks as user
  blocks, so a save overwrites its own directory. That is silent corruption discovered a week later
  when the save will not load. It is exactly the shape of error a from-scratch `format()` makes and
  a byte-diff against a known-good card catches, which is why test 5.5(1) pins every FAT entry and
  why 2.3's reconstruction diff is the acceptance evidence for the constants.

### 6.7 Reproducibility

A card that is written to is run state. Any test or differential run that passes `--vmu` must copy
the image to a temp file first, or the second run of a "reproducible" `--rtc-seed` pair starts from
a card the first one changed. This bit me while measuring section 3 and it will bite the harness.
5.4's `--window` guard keeps it out of the golden-trace path.

---

## Appendix: reproducing section 3

```sh
# 1. Rebuild the blank image (no repo files touched):
python3 - <<'EOF'
import struct
BLK=512; img=bytearray(BLK*256); root=255*BLK
for i in range(16): img[root+i]=0x55
img[root+0x10]=1; img[root+0x11]=img[root+0x12]=img[root+0x13]=0xFF; img[root+0x14]=0x64
for i,v in enumerate((0x19,0x98,0x11,0x27,0,0,0x59,0x04)): img[root+0x30+i]=v
w=lambda o,v: struct.pack_into('<H',img,o,v)
for o,v in ((0x40,255),(0x42,0),(0x44,255),(0x46,254),(0x48,1),(0x4A,253),(0x4C,13),
            (0x4E,5),(0x50,200),(0x52,31),(0x54,0),(0x56,0x80)): w(root+o,v)
fat=254*BLK
for i in range(256): w(fat+i*2,0xFFFC)
w(fat+254*2,0xFFFA); w(fat+255*2,0xFFFA); w(fat+241*2,0xFFFA)
for b in range(242,254): w(fat+b*2,b-1)
open('/tmp/blank.bin','wb').write(img)
EOF

# 2. Title screen only: 15 block reads = root + FAT + 13 directory blocks.
build/dream-dev/games/crazytaxi/crazytaxi_boot --config games/crazytaxi/crazytaxi.toml \
    --vmu /tmp/blank.bin --max-frames 600 --rtc-seed 1000000

# 3. Far enough in to save: 100 block writes, and a CRAZYTAXI_DC entry appears.
build/dream-dev/games/crazytaxi/crazytaxi_boot --config games/crazytaxi/crazytaxi.toml \
    --vmu /tmp/blank.bin --max-frames 1800 --rtc-seed 1000000 \
    --press start@120,start@400,start@700,a@1000,start@1300
python3 -c "d=open('/tmp/blank.bin','rb').read(); print(hex(d.find(b'CRAZYTAXI_DC')))"   # 0x1fa04
```

Comparisons against the owner's card were made on a copy, never the original.
