# BanjoCoop

Online multiplayer for **Banjo-Kazooie**, built as a mod for
[BanjoRecompiled](https://github.com/BanjoRecomp/BanjoRecomp).

Play the whole game together. You see each other, you share the world's progress, you can gang up
on the same Grublin, and you can pick each other up and throw each other at a ledge. Worlds are
free-roam — nobody is tethered to anybody, and two players in different worlds both make progress
on the same save.

> **You need your own copy of Banjo-Kazooie.** No game data is included, bundled, or downloaded by
> this project. See [About the ROM](#about-the-rom).
>
> Unofficial fan project. Not affiliated with or endorsed by Nintendo, Rare, Microsoft, or the
> BanjoRecomp project.

---

## Status — read this before playing

Honest state of things, because play-testing time is worth more than a good first impression:

| | |
|---|---|
| **Played, and works** | Two players in the world together, puppets, shared notes/jiggies/flags, the progression mirror, free-roam between worlds |
| **Built, tested off-game, never actually played** | Enemies, pooled damage, projectiles, bosses, the Grunty fight, revive, carrying, party modes, distance-based send rates |
| **Known incomplete** | Party modes have no on-screen UI; hide-and-seek's head start does not restrain the seeker; cutscenes are local so one player watches while others wait; a client joining mid-session must return to the file-select screen once |

`docs/next.md` is the live list of what to test and what is shaky. If you hit something, that file
is where it should get written down.

The transport and the mod's testable logic do have automated coverage — see
[Building from source](#building-from-source) — but "the tests pass" and "the fight works" are
different claims, and only the first one is currently true for the later systems.

---

## Install

### The easy way

> **No release has been published yet.** Until the first one is tagged there is nothing to
> download, and [Building from source](#building-from-source) is the only way in. The rest of this
> section describes what the downloads will be.

Grab the latest release for your platform from the
[**Releases page**](https://github.com/Lebbitheplow/BanjoCoop/releases):

| Download | What it is |
|---|---|
| `banjocoop-<version>-<os>-bundle.zip` | BanjoRecompiled **and** BanjoCoop, ready to go. Start here if you don't already have BanjoRecompiled. |
| `banjocoop-<version>-<os>-mod-only.zip` | Just the two mod files, for people who already run BanjoRecompiled. ~200 KB. |

Then:

1. Unzip it.
2. Run `install.sh` (Linux) or `install.ps1` (Windows). It copies the mod into BanjoRecompiled's
   mods folder and tells you where it put it.
3. Launch **BanjoRecompiled**. The first time, it asks where your Banjo-Kazooie ROM is and copies
   it into its own configuration folder.
4. Open the mod menu, enable **BanjoCoop**, and set **Network Mode** to Host or Join.

### By hand

Drop `banjocoop.nrm` and the native library (`banjocoop_net.so` / `.dll`) — **both files, side by
side** — into:

| OS | Folder |
|---|---|
| Linux | `~/.config/BanjoRecompiled/mods/` |
| Windows | `%LOCALAPPDATA%\BanjoRecompiled\mods\` |
| macOS | `~/Library/Application Support/BanjoRecompiled/mods/` |

The runtime loads the native library from *next to* the `.nrm`, not from inside it, so leaving it
behind means the mod loads and then fails to find its networking.

macOS is not currently built or tested. The transport is portable, so it is likely a matter of
building it — but nobody has.

## About the ROM

You supply your own. BanjoCoop ships none, downloads none, and contains none.

BanjoRecompiled accepts **only the North American v1.0 (NTSC-U rev0) release** and silently
rejects anything else — no rev A, no PAL, no Xbox Live Arcade version. If yours is refused, check
it before assuming the mod is at fault:

```bash
scripts/check_rom.py "path/to/your/rom.z64"
```

It reports the SHA-1, compares it against rev0's (`1fe1632098865f639e22c11b9a81ee8f29c75d7a`),
and with `-o out.z64` converts a `.v64` or `.n64` byte order into the `.z64` the runtime wants.
Byte order is a common and easily fixed problem; the wrong revision is not fixable.

---

## Playing together

One player hosts, everyone else joins. The host is the session's authority: **the host's save file
becomes the session's save**, and clients adopt it on connect. Their own save is untouched on
disk and comes back when they disconnect and reload it.

**Set up through the mod menu**, or use the in-game lobby without leaving the game:

| Input | Does |
|---|---|
| **D-pad Down** (while disconnected) | Open the lobby — host or join |
| **D-pad Up** | Chat |
| **Z** (next to another player) | Pick them up / put them down |

Default port is **34567/UDP**. The host forwards it, or you both use a VPN/tunnel — there is no
matchmaking server, by design.

### Settings

All configurable from BanjoRecompiled's mod menu, live, without restarting.

| Setting | Default | What it does |
|---|---|---|
| Network Mode | Offline | Host a session, join one, or stay offline |
| Host Address | 127.0.0.1 | Address to connect to when joining |
| Port | 34567 | UDP port to host on or connect to |
| Player Name | player | Name shown to other players |
| Show Player List | On | Overlay listing everyone and which world they are in |
| Sync Enemies | On | Share enemies, so you can gang up and see each other's kills |
| Carry Other Players | On | Z next to a player picks them up — good for boosting onto a ledge |
| Respawn Next to a Teammate | On | Reappear beside another player instead of walking back |
| In-Game Lobby | On | The D-pad shortcuts above |
| Party Mode | Off | Race, or Hide and Seek (host sets this) |
| Debug: Simulated Latency | 0 ms | Artificial one-way delay on outgoing packets |
| Debug: Simulated Jitter | 0 ms | Random variation either side of that delay |
| Debug: Simulated Packet Loss | 0 | Outgoing packets dropped, in tenths of a percent |

The three debug sliders exist because replication bugs that only show up under latency are the
norm, not the exception. Setting 150 ms on one side alone already produces a realistic
asymmetric link.

---

## How it works

Deep detail lives in [`docs/symbols.md`](docs/symbols.md) — the engineering log, including every
attachment point into the game and the mistakes that found them. This is the short version.

### Transport — `src/native/`

ENet over UDP in a star topology: the host relays, and is authoritative. Two channels, and the
split is the central design decision of the whole project:

- **Player and object state** goes out unreliable at 30 Hz. A dropped position packet is
  superseded by the next one; re-sending a stale position would be worse than losing it.
- **World events** go out reliable and ordered. A dropped "this note is gone" is never superseded
  — it is a permanent divergence between two players' games.

Everything expensive happens on the transport's own thread. The game thread crosses the
native boundary exactly **once per frame**, exchanging two staging structs, rather than once per
object.

Peers compare protocol version, mod version and a build fingerprint at handshake, and are rejected
with a named reason rather than left to desync mysteriously later.

### Players and puppets — `src/mod/puppet.c`

Position, velocity, yaw, transformation and the animation's asset index *and playback timer*.
Sending the animation rather than inferring one from velocity is what makes a remote player
reproduce the sender's actual pose. Puppets only exist for peers in your map.

### Shared world — `src/mod/world*.c`

Notes, jiggies, empty honeycombs, Mumbo tokens, and flags at three different scopes (file
progress, per-level, per-map).

The host adjudicates every claim, which is what stops two players grabbing the same note from
counting it twice. Both players still *get* the note — it is shared progress — but it is recorded
once. Peers exchange FNV-1a hashes of the state that is supposed to be identical, so divergence is
detected rather than discovered hours later.

A trap worth knowing about, documented at length in `docs/symbols.md` §15: every collectible in
this game is recorded **twice**, as a score bit and as an item counter. Handling one and not the
other produces a save that looks right and behaves wrong.

### Progression mirror — `src/mod/progress.c`

The host publishes its permanent progress once a second and clients mirror it. Continuous, not a
one-shot replay on join — that distinction is the point. A burst on connect can be dropped,
mistimed, or raced against the client's own save load, and when it fails the symptom is a player
standing in front of a world that will not open with no way to recover. A mirror resent every
second is self-healing: whatever went wrong, the next one fixes it.

Deliberately **not** mirrored: health, lives, eggs, feathers. Those are yours.

### Free-roam — `world.c` and the transport

Events route by map and level. Walking into a map replays that map's state to you, which the host
detects straight from the state stream — you are already reporting where you are every frame, so
there is no request protocol.

### Enemies — `src/mod/enemy.c`

Every peer runs enemy AI. Whoever owns a map — the **lowest player id present in it**, so
ownership moves by itself as people come and go — additionally publishes positions, and receivers
snap to them only on genuine divergence rather than fighting their own simulation every frame.

Damage is pooled by replaying each hit through the game's own collision dispatcher. That is what
makes ganging up work: without it, each machine only ever sees its own player's hits, so two
players could work on one enemy forever and never kill it. Death is detected generically, by an
enemy disappearing locally, and broadcast reliably.

Enemy types are an **allowlist**, not a heuristic — 274 marker types cover enemies, scenery,
doors and signs with no field cleanly separating them, and being wrong surfaces far from the
cause.

### Projectiles — `projectile.c`, `spawn.c`

Things enemies throw have no identity two machines can derive independently, so the owner names
each one and everybody else adopts it. Otherwise every peer's Chimpy throws its own orange at
whoever is nearest *there*, and a projectile that hits you may never have existed for the other
player.

`spawn.c` patches the game's single actor-spawn funnel so a non-owner never creates its own copy
in the first place.

### Send rates — `src/mod/tier.h`

Enemies are published nearest-first in three distance bands, phased by net id so a map's whole
band does not land on one frame. Projectiles are deliberately exempt: an enemy missing from a
frame is merely uncorrected, but a projectile missing from a frame is one the receiver *despawns*.

### Bosses — `src/mod/boss.c`

One generic state-machine driver covers roughly 70 actors' phase transitions. Gruntilda needed
her own: her six-phase fight has discrete moments — a Jinjo statue appearing, the Jinjonator's
strike — that no state change describes.

### Revive, carrying, party modes — `revive.c`, `carry.c`, `modes.c`

Respawn beside a teammate instead of walking back. Carrying reuses the game's own carry mechanic.
Race and Hide and Seek are deliberately thin — a rule about data that already exists, which is
also the honest test of the layer underneath: if a mode needs new plumbing, the plumbing was wrong.

### API for other mods — `src/mod/banjocoop_api.h`

Six functions. Send a message under your own tag, scoped to everyone / your map / your level, and
receive everyone else's. Delivery, ordering, and attribution that cannot be spoofed are already
solved; what you send is your business.

```c
RECOMP_IMPORT("banjocoop", u32 banjocoop_is_connected(void));
RECOMP_IMPORT("banjocoop", u32 banjocoop_is_host(void));
RECOMP_IMPORT("banjocoop", void banjocoop_send(u32 tag, u32 scope, u32 a, u32 b));
```

Copy the header, declare BanjoCoop an optional dependency, and check `banjocoop_is_connected()` —
everything returns 0 when BanjoCoop is absent, so your mod still works without it.

---

## Building from source

Needs `clang`, `lld`, `cmake`, `ninja`, `python3`, and a C++20 host compiler.

```bash
git clone --recursive https://github.com/Lebbitheplow/BanjoCoop.git
cd BanjoCoop
scripts/bootstrap.sh     # submodules, RecompModTool, and the BanjoRecompiled runtime
make                     # the mod (.nrm) and the native transport
```

Cloned without `--recursive`? `git submodule update --init --recursive`.

| Command | Does |
|---|---|
| `make` | Build the mod and the native library |
| `make check-imports` | Verify every base-recomp import resolves against the shipped runtime — **run this before trusting a build** |
| `make test-native` | Headless transport tests: real peers, real sockets, no ROM and no game |
| `make test-modlogic` | Mod logic compiled against fake game accessors |
| `make install` | Copy both artifacts into your BanjoRecompiled mods folder |
| `make run` / `make run-p2` | Launch two independent instances with separate configs and saves |
| `make diag` | Read the last session's logs back and answer the questions that matter, in order |

`check-imports` exists because of a specific trap: `vendor/BanjoRecomp` is `main`, while the
runtime that actually loads the mod is release 1.0.1, and the gap between them is precisely in the
mod-facing API. Code written against the vendored source compiles, links, packages — then fails at
game start. See `docs/symbols.md` §16.

**Diagnosing anything starts with the logs, not the screen.** `make run` tees to `run/logs/`, every
subsystem announces itself, and a missing line is usually the whole diagnosis.

---

## Licence and credits

GPL-3.0-or-later — see [`LICENSE`](LICENSE).

BanjoCoop compiles GPL-3 headers from N64ModernRuntime and runs as a plugin inside the GPL-3
BanjoRecompiled process, so a permissive licence was never really available to it.

Credits, the full dependency list, and what the release bundles contain are in
[`NOTICE.md`](NOTICE.md). The short version: this exists because of BanjoRecomp, N64Recomp, the
Banjo-Kazooie decompilation team, and ENet — and it was measured throughout against SM64CoopDX,
which showed what a co-op mod can be.
