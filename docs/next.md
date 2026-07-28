# BanjoCoop — what's left

The 9-phase plan in `~/.claude/plans/i-want-to-make-gleaming-hinton.md` is implemented end to end.
This is what to do next, ordered by what would actually cost you a playthrough.

The honest summary: **Phases 0–4 have been played and work. Phases 5–9 are built, tested where they
can be tested off-game, and have never been played.** That gap is the whole of this document.

---

## 1. Play-test backlog, most likely to bite first

Each of these has a cheap version — do the cheap one before the expensive one.

| What | Cheap test | Expensive test |
|---|---|---|
| Boss phase sync | Conga, in Mumbo's Mountain | Any later boss |
| Pooled damage | Two players hitting one Grublin | A boss with several phases |
| Projectiles | Chimpy's oranges in MM | Grunty's spells |
| Carrying | Z next to a teammate in Spiral Mountain | Carrying across a level transition |
| Revive | Die in a world with a teammate present | Die during a boss fight |
| Enemy ownership away from the host | Two clients in a world, host in the lair | Three players in three worlds |
| Distance send rates (§5) | Walk apart in MM, read `objtier` | A full map at 150 ms and 3% loss |
| Spawn suppression (§5) | Chimpy's oranges — one, not two | Sir Slush, the unchecked caller |
| The Grunty fight | — | The only test there is |

**Watch the logs, not the screen.** `make run` / `make run-p2` tee to `run/logs/`; `make diag` reads
them back. Every subsystem announces itself, and a missing line is usually the whole diagnosis.

## 2. Known-shaky, in the order I would fix them

1. **Grunty's fight is the single biggest unknown.** Six phases, the Jinjonator, a shifting arena,
   and no way to reach it quickly for testing. If one thing gets a dedicated session, this.
2. **Carrying across a map transition.** The carried player is dropped by design when the carrier
   leaves the map, but "dropped" has not been watched happen.
3. **Party modes have no UI.** The round runs, the winner is logged, and nothing appears on screen.
   The overlay already has rows to spare.
4. **Hide-and-seek head start does not restrain the seeker.** It only delays judging, so the seeker
   can simply follow somebody. Needs the seeker actually held still.
5. **`s_finish` for Race is wherever the host stood.** Fine for a party, useless for a rematch —
   nothing persists it.

## 3. Genuinely unfinished from earlier phases

- **The volatile-flag allowlist** (§15). Volatile flags are not replicated because most are
  per-player session state; a handful are world state and are therefore silently missing. Needs
  someone to go through them by name.
- **A shared cutscene skip vote** (Phase 8). Cutscenes are local, so one player watches while the
  others wait. Harmless, but not what the plan asked for.
- **Late-join mid-session still needs a file reload.** The host's save arrives on connect, but the
  game only reads a save at file select. A client already in-game must return there once.
- **No CI, because there is no repository.** The plan's Verification §5 asks for builds on every
  commit. Nothing is under version control, so every fix in this project is unbisectable. This is
  the cheapest large win available and it needs one `git init`.

## 4. Where the seams are, if something breaks

Written up in `symbols.md`, but the four that have caused the most trouble:

- **Actors latch their state at spawn** (§21). Notes, jiggies and the lair entrances all bit us.
  Anything gated on replicated progress needs a way to re-evaluate.
- **`vendor/BanjoRecomp` is not what we run** (§16). It is `main`; the runtime is 1.0.1, and the
  API differs. `make check-imports` guards this.
- **Only entry hooks may read arguments** (§14). Return hooks get a clobbered context.
- **Every collectible is recorded twice** (§15) — a score bit *and* an item counter.

## 5. If you want to go further than the plan

In rough order of value:

- ~~**Distance-based send rates.**~~ **Done, untested in game.** Enemies are published nearest-first
  in three distance bands (`src/mod/tier.h`), the host now routes its own object frames by map
  instead of broadcasting them, and projectiles hold a reserved share of the budget. Two
  pre-existing defects fell out of it — enemies could starve projectiles out of the object frame
  entirely, and enemies never cleared `actor_id`, which would have had receivers spawn phantom
  projectiles. Written up in `symbols.md` §35.
  **The distance constants are guesses and need a tuning pass** — the `objtier` log line and
  `make diag` exist for exactly that.
- ~~**Spawn suppression for dynamic actors.**~~ **Done, untested in game.** `src/mod/spawn.c`
  patches `__actor_spawnWithYaw_s32`, the single funnel every spawn goes through. Suppression is a
  despawn rather than a NULL return, because at least one caller dereferences the result unchecked.
  This is the project's only `RECOMP_PATCH` and it now holds that exclusive slot — read
  `symbols.md` §36 before adding a second.
- **Lua or scripting on top of the mod API.** The API is six functions; CoopDX's platform is a
  scripting layer above something similar.
- **Extracting the transport as a cross-recomp library** (plan §1.10). Deliberately deferred until
  BanjoCoop works, so the abstraction is drawn from something real rather than guessed at.
