# Notices, credits, and what is and is not distributed

## No game content is distributed

**BanjoCoop contains no Banjo-Kazooie game data, and never downloads any.**

Banjo-Kazooie is the property of Nintendo and Rare. You supply your own copy. BanjoRecompiled
asks for it the first time you run it and copies it into its own configuration folder; nothing in
this project reads, ships, or fetches it. The only ROM-related tool here is
`scripts/check_rom.py`, which tells you whether a ROM *you already have* is the revision
BanjoRecompiled accepts.

BanjoCoop is an unofficial fan project. It is not affiliated with, endorsed by, or connected to
Nintendo, Rare, Microsoft, or the BanjoRecomp project.

## BanjoCoop's own licence

GPL-3.0-or-later. See `LICENSE`.

This is not an arbitrary choice. `src/native/src/mod_abi.cpp` compiles headers from
N64ModernRuntime (`librecomp/helpers.hpp`, `recomp.h`), which is GPL-3 and whose headers carry
substantive inline template code rather than bare declarations; and the mod is loaded as a plugin
into the BanjoRecompiled process, sharing its data structures. Either fact on its own points at
GPL-3, so a permissive licence would have been wishful thinking.

## Bundled with the release archives

The release bundles include **BanjoRecompiled**, unmodified, alongside BanjoCoop.

- Upstream: <https://github.com/BanjoRecomp/BanjoRecomp>
- Licence: GPL-3.0 — full text in `third_party/BanjoRecompiled/COPYING`
- Version bundled: see `BUNDLED_VERSION` in the archive's `README.txt`

GPL-3 §6 requires that the corresponding source accompany a binary or be offered alongside it.
That obligation is discharged here by pointing at the upstream repository, from which both the
release binaries and their exact corresponding source are available. The bundled binary is
upstream's own release artifact, byte-for-byte, with no modification by this project.

Upstream states that its prebuilt binaries contain no game assets. That is consistent with what
the release actually holds: fonts, SVG icons, and a stylesheet.

Bundling BanjoRecompiled beside BanjoCoop in one archive is aggregation under GPL-3 §5. Both are
GPL-3 in any case, so nothing turns on that here.

## Credits

BanjoCoop stands on other people's work:

- **[BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp)** — the static recompilation of
  Banjo-Kazooie that BanjoCoop is a mod for, and without which none of this exists. GPL-3.
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** — the recompiler, and `RecompModTool`,
  which packages this mod. MIT.
- **[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)** — the runtime and its mod
  support, including the native-library ABI this project's transport rides on. GPL-3.
- **[BKRecompModTemplate](https://github.com/BanjoRecomp/BKRecompModTemplate)** and
  **[BanjoRecompSyms](https://github.com/BanjoRecomp/BanjoRecompSyms)** — the mod scaffolding and
  the symbol tables every game function here is resolved through. CC0.
- **[Banjo-Kazooie decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie)** — the reverse
  engineering that makes it possible to know what any of these functions do. Every attachment
  point in `docs/symbols.md` was found by reading their work.
- **[ENet](https://github.com/lsalzman/enet)** by Lee Salzman — the reliable-UDP transport.
  MIT-style licence; full text in `vendor/enet/LICENSE`.
- **[SM64CoopDX](https://github.com/coop-deluxe/sm64coopdx)** — not used as code, but its design
  is the reference this project measured itself against throughout: ownership by lowest player id,
  distance-based send rates, and the idea that a co-op mod should be a platform others can build
  on. See `docs/symbols.md` §20.
