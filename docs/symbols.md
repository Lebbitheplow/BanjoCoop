# BanjoCoop — Engine Attachment Points

Findings from reading `vendor/BanjoRecomp` @ depth-1 clone of `main`.
This is the reference doc for what we hook and how. Update as we learn more.

§1–13 are Phase 0–2, §14–16 Phase 3, §17 Phase 4, §18 Phase 5, §19 Phase 6.
Read §16 before calling any `bkrecomp_*` export.

## 1. The mod boundary is already well-defined

`patches/patch_helpers.h` gives the exact MIPS↔native calling convention:

```c
#ifdef MIPS
#    define DECLARE_FUNC(type, name, ...)  EXTERNC type name(__VA_ARGS__)
#else
#    define DECLARE_FUNC(type, name, ...)  EXTERNC void name(uint8_t* rdram, recomp_context* ctx)
#endif
```

So a native function is always `void f(uint8_t* rdram, recomp_context* ctx)`, with args pulled
off `ctx` and results pushed via `_return<T>(ctx, val)` (see `src/game/recomp_extension_api.cpp`
for worked examples). This confirms the Phase 1 boundary design — declare each native entry point
once with `DECLARE_FUNC` and it compiles correctly on both sides.

## 2. BanjoRecomp already solved per-object extension data

**Do not build a side table keyed by `actrArrayIdx`.** `patches/bk_api.h` exposes a public,
mod-facing API for attaching arbitrary data to markers and props:

```c
MarkerExtensionId bkrecomp_extend_marker_all(u32 size);
void* bkrecomp_get_extended_marker_data(ActorMarker* marker, MarkerExtensionId ext);
u32   bkrecomp_get_marker_spawn_index(ActorMarker* marker);

PropExtensionId   bkrecomp_extend_prop_all(u32 size);
void* bkrecomp_get_extended_prop_data(Cube* cube, Prop* prop, PropExtensionId ext);
u32   bkrecomp_get_prop_spawn_index(Cube* cube, Prop* prop);
```

We register one extension for markers and one for props at startup, sized to our per-object
network state (net ID, owner, dirty flags, last-sent snapshot). Lifetime is handled for us —
`marker_init` creates the data and `func_80332B2C` destroys it
(`patches/marker_extension_patches.c:69,88`).

## 3. Object identity — corrected

The plan's §1.5 proposed hashing `(map, cube coords, prop index)`. Partly unnecessary, and the
distinction between props and markers matters more than the plan stated.

`recomp_get_object_spawn_index` returns `context.spawn_count++` at creation
(`src/game/recomp_extension_api.cpp:162`) — **spawn order, not a content hash**. Whether that is
safe as a network ID depends entirely on whether spawn order is deterministic:

| Object class | Identity source | Cross-machine stable? |
|---|---|---|
| **Props** (notes, static collectibles, scenery) | `bkrecomp_get_prop_spawn_index` | **Yes** — see below |
| **Markers** (enemies, dynamic actors, dropped items) | `bkrecomp_get_marker_spawn_index` | **No** — spawn-order dependent |

**Props are safe** because prop extension data is cleared wholesale on cube-list teardown at map
unload (`prop_extension_patches.c:382`) and re-created by iterating cubes and their `prop2Ptr`
arrays in fixed order at map load (`:156,186`). Spawn index therefore restarts at 0 each map load
and counts up in deterministic static-level-data order. `(map_id, prop_spawn_index)` is a valid
network ID with no hashing needed.

**Markers are not safe.** Markers are created as actors spawn, and under multiplayer spawn order
diverges between machines (different players trigger different spawns at different times).
Dynamic actors need **host-assigned network IDs** allocated at spawn and replicated in the spawn
packet. Plan accordingly in Phase 5.

## 4. Strong precedent: `note_saving.c`

`patches/note_saving.c` (498 lines) already implements persistent per-note identity — the exact
shape of the collectible-sync problem:

- Registers `note_saving_prop_extension_id` and stores a `note_index` per prop.
- Maintains `map_note_data[512]` / `level_note_counts[256]` keyed by map and level.
- Distinguishes **static** notes (props in level data) from **dynamic** notes
  (`note_saving_handle_dynamic_note`, called from `marker_init` when
  `marker->id == MARKER_5F_MUSIC_NOTE`) — which maps exactly onto the prop/marker identity split above.

Read this file in full before writing the Phase 3 collectible registry. It is the closest thing
to a worked example of what we're building.

Related precedent: `jinjo_saving.c`, `mumbo_token_patches.c`.

## 5. Scale of the existing patch surface

`patches/` uses **146 `RECOMP_PATCH`**, **29 `RECOMP_EXPORT`**, **4 `RECOMP_DECLARE_EVENT`**.
Relevant existing patches to read before hooking the same areas:

| File | Why it matters |
|---|---|
| `marker_extension_patches.c` | marker lifetime + extension API |
| `prop_extension_patches.c` | prop/cube handles, spawn index, map load/unload |
| `note_saving.c`, `jinjo_saving.c`, `mumbo_token_patches.c` | collectible identity + persistence |
| `overlay_loading.c` | overlay/map load entry points — our per-map state handoff hooks |
| `load_patches.c`, `init_patches.c` | startup ordering for registering extensions |
| `specific_actor_patches.c` | per-actor special-casing precedent |
| `pause_menu_patches.c`, `ui_funcs.h`, `recompui_event_structs.h` | UI integration for Phase 7 |

**Conflict risk:** `RECOMP_PATCH` is exclusive — two mods patching the same function are
incompatible, and BanjoRecomp itself already patches 146 functions. Prefer `RECOMP_HOOK` where
possible, and check this list before choosing a patch target.

## 6. Submodules worth having locally

```
lib/bk-decomp        → gitlab.com/banjo.decomp/banjo-kazooie   (struct defs, symbol names)
BanjoRecompSyms      → github.com/BanjoRecomp/BanjoRecompSyms   (symbol map)
lib/N64ModernRuntime → recompui.h / recomputils.h / recompdata.h / recompconfig.h
lib/RecompFrontend   → menus, input
```

The decomp is a first-party submodule of BanjoRecomp, so decomp struct headers (`prop.h`,
`actor.h`, `functions.h`, `enums.h`) are already what the patches `#include`. Our mod includes the
same headers.

## 7. Native libraries — confirmed in code

`RecompModTool/main.cpp:280-301` parses an optional `native_libraries` manifest array. Schema:

```toml
[[manifest.native_libraries]]
name  = "banjocoop_net"                    # base name; runtime appends .dll/.so/.dylib
funcs = ["bcnet_init", "bcnet_pump", "bcnet_shutdown"]   # required, explicit export list
```

Each entry is a table with a required `name` and a required `funcs` string array. Only listed
functions are callable from mod code. **This validates the Phase 1 architecture** — ENet, threads,
and serialization live in a real native `.so`/`.dll`/`.dylib` and are called from MIPS-side mod
code, with no fork of BanjoRecomp.

Each exported native function is implemented with the `DECLARE_FUNC` convention from §1
(`void f(uint8_t* rdram, recomp_context* ctx)`).

## 8. Hooks exist (macros live in the mod template)

`patches/` uses 0 `RECOMP_HOOK`, which initially looked like hooks might be unavailable. They are
not — hooks are a first-class recompiler feature:

- `include/recompiler/live_recompiler.h:81-85` — `run_hook`, `entry_func_hooks`, `return_func_hooks`
- `src/mod_symbols.cpp:98,464-472` — `HookV1`, `N64Recomp::FunctionHook` serialized into mod symbols

BanjoRecomp's own `patches/` are *base-recomp patches*, compiled into the recomp itself, which is
why they use `RECOMP_PATCH` exclusively — a different mechanism from third-party `.nrm` mods.
Entry and return hooks are non-destructive and stack across mods — strongly prefer them over
`RECOMP_PATCH`.

Confirmed in `vendor/BKRecompModTemplate/include/modding.h` — the full macro surface is implemented
as linker-section attributes the mod tool scans:

| Macro | Section |
|---|---|
| `RECOMP_HOOK(func)` / `RECOMP_HOOK_RETURN(func)` | `.recomp_hook.<func>` / `.recomp_hook_return.<func>` |
| `RECOMP_PATCH` / `RECOMP_FORCE_PATCH` | `.recomp_patch` / `.recomp_force_patch` |
| `RECOMP_EXPORT` | `.recomp_export` |
| `RECOMP_IMPORT(mod, func)` | `.recomp_import.<mod>` (`*` = base recomp) |
| `RECOMP_DECLARE_EVENT(func)` / `RECOMP_CALLBACK(mod, event)` | `.recomp_event` / `.recomp_callback.<mod>:<event>` |

## 9. Mod template — our starting point

`vendor/BKRecompModTemplate` is the scaffold to copy. It ships:

- `include/` — `modding.h`, `recompui.h`, `recomputils.h`, `recompdata.h`, `recompconfig.h`,
  `bkrecomp_api.h`, `recompui_event_structs.h`, `rt64_extended_gbi.h`
- `mod.toml` — manifest with `native_libraries = [{ name = ..., funcs = [...] }]` already stubbed
  at line 51-54, plus `config_options` (Enum/Number/String) for our host/join settings
- `Makefile`, `mod.ld`, `generate_symbols.toml` — the MIPS build
- `src/always_tumble_jump.c` — a minimal worked example
- Reference syms wired to `BanjoRecompSyms/bk.us.rev0.syms.toml` +
  `bk.us.rev0.datasyms.toml` — this is how we resolve vanilla symbols by name.

`game_id = "bk"`. `recomp_is_dependency_met` (recomputils.h) is available for optional deps.

## 10. Player state — named accessors already exist

Good news for Phase 2: we do not need to scrape raw globals. The decomp exposes a clean player API,
so the local-state extractor is mostly calls to existing functions. The player subsystem uses a
**`ba`** prefix ("Banjo") — `baanim`, `baphysics`, `bamodel` — that's the namespace to explore.

| State | Symbol | Location |
|---|---|---|
| Position | `player_getPosition(f32 dst[3])`, `_player_getPosition` | `include/functions.h:62-63` |
| Y position | `player_getYPosition()` / `player_setYPosition(f32)` | `functions.h:72,421` |
| Rotation | `player_getRotation(f32 *dst)` | `functions.h:64` |
| Yaw | `player_getYaw()` | `src/core2/playerutils.c:11` |
| **Transformation form** | `player_getTransformation()` | `functions.h:58` |
| Animation controller | `player_getAnimCtrlPtr()` | `src/core2/code_7060.c:259` |
| Animation controller (alt) | `baanim_getAnimCtrlPtr()` | `functions.h:420` |
| Velocity / physics | `baphysics_set_target_velocity(f32 src[3])`, `baphysics_reset_horizontal_velocity()` | `include/core2/ba/physics.h:26-29` |

`player_getTransformation()` directly answers "which model should the puppet render", which is the
Phase 2 + Phase 4 transformation problem largely solved.

Velocity getters do exist — `baphysics_get_velocity(f32 dst[3])`,
`baphysics_get_horizontal_velocity()`, `baphysics_get_vertical_velocity()` — so we send real
velocity rather than deriving it from successive positions. Full API in
`include/core2/ba/physics.h`.

### Frame hook — chosen: `baphysics_update`

`baphysics_update(void)` (vram `0x80297744`) runs once per frame in the player physics step. This
is our `pump()` attachment point and is what the Phase 0 mod hooks.

Rejected alternative: the VI manager timing functions (`viMgr_func_8024BFA0`,
`viMgr_func_8024C1B4`, `viMgr_func_8024C1DC`). BanjoRecomp already `RECOMP_PATCH`es all three
(`patches/timing_patches.c:111-129`), and they tick outside gameplay too. `baphysics_update` only
runs during actual gameplay, which is what we want.

Either way the rule from §5 holds: attach with `RECOMP_HOOK`, never `RECOMP_PATCH`.

Verified present in `bk.us.rev0.syms.toml`:

| Symbol | vram |
|---|---|
| `baphysics_update` | `0x80297744` |
| `baphysics_get_velocity` | `0x80297A88` |
| `player_getPosition` | `0x8028E9A4` |
| `player_getYaw` | `0x8028EBA4` |
| `player_getTransformation` | `0x8028E7CC` |

`player_getYaw` is declared in `src/core2/playerutils.c` but **not** in `functions.h` — declare it
yourself (`extern f32 player_getYaw(void);`) or you get an implicit-declaration warning.

## 11. Native library ABI (Phase 1, all verified in code)

- The library must export `uint32_t recomp_api_version = 1`. Without it the runtime reports
  `NoSpecifiedApiVersion` and no export resolves (`librecomp/src/mods.cpp:185`).
- Every export has the signature `void f(uint8_t* rdram, recomp_context* ctx)` (`recomp_func_t`,
  `N64Recomp/include/recomp.h:443`). Arguments come off `ctx` via `_arg<N, T>`, results go back
  through `_return<T>` (`librecomp/include/librecomp/helpers.hpp`).
- **`helpers.hpp` uses C++20 `requires` clauses** — the native library must build as C++20.
- The `.so`/`.dll`/`.dylib` sits **next to the `.nrm`**, not inside it. The runtime builds the path
  as `mod_root_path.parent_path() / (name + PlatformExtension)` (`mods.cpp:308`). The mod scanner
  logs `Skipping non-mod banjocoop_net.so` for it, which is expected and harmless.
- Only functions listed in the manifest's `native_libraries[].funcs` are resolvable.
- MIPS-side imports use `RECOMP_IMPORT(".", ...)` for the mod's own exports **including its native
  library** — `.` is `DependencySelf` and `*` is `DependencyBaseRecomp`
  (`N64Recomp/include/recompiler/context.h:92-93`).

### RDRAM byte order — the thing most likely to burn you

RDRAM stores 32-bit words in **host-native** order, not big-endian:

```c
#define MEM_W(offset, reg) (*(int32_t*)(rdram + (addr - 0xFFFFFFFF80000000)))   // plain load
#define MEM_B(offset, reg) (*(int8_t*) (rdram + ((addr ^ 3) - ...)))            // note the ^3
```

`MEM_W` is a plain native load with no byteswap; the `^ 3` on byte access is the tell that the
layout is word-swapped. Consequences:

- A struct of all-4-byte fields can be read from rdram through `TO_PTR` as an ordinary native
  struct — no conversion. This is what `bcnet_pump` does.
- **Sub-word types cannot.** Reading a C string requires `MEM_BU(i, addr)` to apply the `^3`;
  `TO_PTR(char, p)` plus `strlen` silently returns scrambled text. See `read_mips_string` in
  `src/native/src/mod_abi.cpp`.
- Anything crossing the network is converted to big-endian explicitly in `transport.cpp`, so the
  protocol does not depend on both peers being little-endian.

## 12. Running two instances

The runtime resolves its config directory from `HOME`. Verified by testing:

| Mechanism | Works? |
|---|---|
| `runtime/.config/portable.txt` marker | no — release build ignores it |
| `XDG_CONFIG_HOME` | no — creates nothing |
| `HOME` override | **yes** |

`make run-p2` uses a `HOME` override pointing at `build/instance2`, seeding it from player 1's
config (ROM, graphics/sound settings, mod config) so there is no first-run prompt.

## 13. Engine rules that cost us three crashes (read before touching actors)

BK's engine predates every assumption a modern C programmer brings to it. Nothing below is
checked at runtime; all of it fails silently, later, and somewhere else. Each one below actually
happened during Phase 2.

### Never cache an `Actor*`

`actor_new` grows the actor array with `realloc` (`code_9E370.c:773`). Any `Actor*` held across a
frame dangles as soon as *some other* actor spawns.

Symptoms when we did this: writes to `actor->position` vanished into freed memory, so the puppet
looked **frozen while receiving perfectly good network data** — which masqueraded as a networking
bug for a full debugging round. Then reading `actor->anctrl` from the same freed memory returned
garbage and crashed inside `anctrl_getIndex`.

**Rule:** hold `ActorMarker*` and call `marker_getActor()` each frame. The marker pool is a fixed
`0xE0`-entry allocation made once (`code_A5BC0.c:2223`) and never reallocated — that is exactly
what markers are for.

### Actor and marker ids are array indices, not free-choice values

| Array | Indexed by | Size | Source |
|---|---|---|---|
| `modelCache` | `actor->modelCacheIndex`, which `actor_new` sets to **actorId** | `AssetCacheSize` = **0x3D5** | `code_A5BC0.c:1390`, `code_9E370.c:809` |
| `sMarkerToBitfield` | `marker->id` | **0x1BC** | `code_7AF80.c:68,2031` |

Neither is bounds-checked. `marker_loadModelBin` *writes* through `modelCache[...]`, so an
out-of-range actor id corrupts the heap past the array. We used actor id `0x3F0` (1008) against a
981-entry array; the damage surfaced as a crash in the **player's** `baMarker_update`, nowhere
near our code, and only after entering gameplay.

The 10-bit width of the `modelCacheIndex` / `id` bitfields (max `0x3FF`) is **not** the relevant
bound — the array sizes are. Checking the bitfield width and stopping there is what let this bug
through.

Highest vanilla actor id is `0x3CB`, so `0x3CC..0x3D4` is the only legal free range. We use
`0x3D0` and marker `0x1BA`.

### Assume no function pointer is NULL-checked

`ActorInfo.draw_func` goes straight into `marker_init` as the marker's `drawFunc` and the renderer
calls it unconditionally — NULL segfaults on the first draw. `actorUpdate2Func` is likewise called
without a check (`code_9E370.c:546`), merely gated by a bitfield that happens to init to 0.

**Rule:** copy the shape of a real vanilla `ActorInfo` (there are 562 of them) rather than
reasoning about what a field probably means. Two of our three `ActorInfo` bugs would never have
happened that way — including `startAnimation`, which is an **index into the animations table**,
not an asset id, and `animations = NULL`, which silently means the engine never creates
`actor->anctrl` at all.

### Dynamically spawned markers have `propPtr == NULL`

Markers created via `actor_spawnWithYaw_f32` are not backed by a cube prop. Engine paths that
dereference `marker->propPtr` (e.g. `codeA5BC0_getActorPosition`, and `func_80325934` which is a
*different* draw function) will crash on them.

### Debugging

`make debug` / `make debug-p2` run under gdb and dump a backtrace to `build/crash-p*.log`. Core
dumps are unavailable (`ulimit -c` is 0, apport owns `core_pattern`), so this is the only way to
get a stack. Note stdout is block-buffered through the pipe and is **lost on a segfault** — a
crashed instance's log will have the backtrace but no `[banjocoop]` lines, while a cleanly-exited
one has the full history.

## 14. Only ENTRY hooks may read arguments (found in Phase 3)

`RECOMP_HOOK_RETURN` looks like the safe way to do something *after* a function runs. It is not
safe to give it parameters.

`recomp::mods::run_hook` calls the hook with whatever `recomp_context` exists at the hook site
(`N64ModernRuntime/librecomp/src/mod_hooks.cpp:28`). For an entry hook that context is the one the
function was just called with, so `a0`–`a3` still hold the arguments and declaring the hook with
the original signature works. At a **return** site those registers are caller-saved and long since
reused — a return hook declared as `void f(Actor *this)` receives garbage and dereferences it.

This is why the existing `RECOMP_HOOK_RETURN("spawnableActorList_new")` takes `void`, and why the
"despawn a jiggy someone else collected" path is an *entry* hook on `chjiggy_update` that queues
the marker for the frame hook to despawn, rather than a return hook that despawns directly. The
deferral is needed anyway: despawning from an entry hook would leave the function we are hooking
to run on a freed actor.

Return hooks can read the *return value* via `recomphook_get_return_*` (mod_hooks.cpp:96-140).
That is the only thing at a return site that is reliable.

## 15. World-state attachment points (Phase 3)

### Collectibles — hook the collision, derive the note index yourself

`__baMarker_resolveMusicNoteCollision(Prop *arg0)` is the note pickup. BanjoRecomp
`RECOMP_PATCH`es it, so it cannot be *patched* — but it can be **hooked**: hooks attach by symbol
and apply to the patched version, and hooking a base-patched function is supported (`ModContext::
regenerate_with_hooks` takes a `BasePatchedFunction` map; confirmed present in the 1.0.1 binary).
Attempting a `RECOMP_PATCH` instead gets you "Attempted to replace a function that's been patched
by the base recomp".

Inside the hook, `arg0->is_actor` distinguishes an enemy-dropped (dynamic) note from a static one;
`arg0->is_3d` rules out model props.

> BanjoRecomp's `main` branch declares `bkrecomp_note_collected_event` /
> `bkrecomp_dynamic_note_collected_event`, which hand you a ready-made note index and would be
> tidier. **They do not exist in the 1.0.1 release.** See §16.

| What | Symbol | Notes |
|---|---|---|
| Note collected | hook `__baMarker_resolveMusicNoteCollision(Prop*)` | base-patched, so hook it; do not patch it |
| Jiggy collected | `jiggyscore_setCollected(s32 index, s32 value)` | authoritative and idempotent; hook at entry |
| Jiggy id from actor | `chjiggy_getJiggyId(Actor*)` | 0 until the actor's INIT state has run |
| Note score | `item_inc(ITEM_C_NOTE)` / `item_adjustByDiffWithoutHud` | the Vile minigame takes the second path, gated on `func_802FADD4` |

**Dynamic notes have no shared identity.** They are spawned per-machine by enemies, so their
spawn order diverges — the marker half of the §3 split. They can be relayed but not deduplicated
until Phase 5 introduces host-assigned ids for dynamic actors.

### Note identity without any `bkrecomp_*` API

Walk `sCubeList` in fixed array order — cube `0..cubeCnt`, then the two fallback cubes
`unk3C`/`unk40` — numbering music-note sprite props as you go. That ordinal is the network id.

It is stable across machines because `sCubeList` is built from the same static level data
everywhere, so the walk visits the same props in the same order. No runtime API, no spawn order,
and nothing that varies between BanjoRecomp versions.

Identify notes the way the game does: a sprite prop's asset id is `spriteProp.sprite_index + 0x572`
(`core2/ba/marker.c:840`), compared against `ASSET_6D6_MODEL_MUSIC_NOTE`.

Two traps:

- **Count collected notes too.** Their props stay in the array with only the alive bit cleared, so
  skipping them renumbers every note after them and desyncs the peers.
- **Guard on `sCubeList.cubes != NULL && cubeCnt > 0`.** Matching map ids does not prove the cube
  list is populated — an event can land inside a map transition.

`sCubeList` is a data symbol at vram `0x80381FA0`; the struct layout is in
`prop_extension_patches.c`.

### Removing an object someone else collected

Clearing a sprite prop's alive bit is how the game itself retires a collected note —
`core2/ba/marker.c:845` does `other_prop->spriteProp.unk8_4 = 0` immediately before calling the
collision handler. So the same write works mid-session from mod code.

### Every collectible is recorded TWICE — score bit *and* item counter

The single most repeatable mistake in this layer. `core2/ba/marker.c` is the ground truth; its
jiggy case is:

```c
jiggyscore_setCollected(jiggy_id, TRUE);
item_adjustByDiffWithoutHud(ITEM_26_JIGGY_TOTAL, 1);
```

Two independent stores. Replicating only the score bit makes the object vanish correctly for the
other player while their total never moves — which reads as "sync is broken" even though the
event arrived and was applied. Replicating only the counter leaves the object collectable, so it
gets counted twice.

| Collectible | Permanent record | Counter the pickup also bumps | Extra side effect |
|---|---|---|---|
| Jiggy | `jiggyscore_setCollected` | `ITEM_26_JIGGY_TOTAL` | — |
| Empty honeycomb | `honeycombscore_set` | `ITEM_13_EMPTY_HONEYCOMB` | at 6, `gcpausemenu_80314AC8(0)` grants the health upgrade |
| Mumbo token | `mumboscore_set` | `ITEM_1C_MUMBO_TOKEN` | — |
| Note | note bits | `ITEM_C_NOTE` | Vile minigame takes the no-HUD path |

Apply both, guarded on the score bit having actually changed — the guard is what makes a
re-delivered event harmless. And when adding a collectible, read its whole case in `marker.c` and
mirror *every* side effect, not just the obvious one.

**There are exactly three score bitfields** (`ls src/core2/*score*`): `jiggyscore`,
`honeycombscore`, `mumboscore`. Hooking notes, jiggies and flags misses the latter two entirely —
which is how hex pieces in Spiral Mountain replicated nowhere. Spiral Mountain in particular has
**no notes and no jiggies** but **four honeycombs** (`HONEYCOMB_13_SM_STUMP`, `14_SM_WATERFALL`,
`15_SM_UNDERWATER`, `16_SM_TREE`), which makes it a misleading place to test note/jiggy sync.

### Jinjos are deliberately NOT synced

`__chJinjo_802CDBA8` (the jinjo collision handler) credits a jinjo with

```c
item_adjustByDiffWithHud(ITEM_12_JINJOS, 1 << (this->id + 6))
```

`this->id` is the marker id (`MARKER_5A_JINJO_BLUE` = 0x5A), so the shift is 96..100 — which lands
on bits 0..4 only because MIPS masks shift counts to 5 bits. `== 0x1f` is the all-five test.

The important part is that this is an **add, not an OR**. Setting a jinjo bit that is already set
carries into the next bit and corrupts the mask. So a half-done sync — credit the bit remotely,
leave the local jinjo standing — actively breaks the save the moment the second player walks into
their still-present jinjo. Doing it safely needs the same remote-despawn treatment the jiggies
get, keyed on marker id.

Leaving them unsynced is benign: each player collects their own five, and the jiggy that spawns at
five is deduplicated by `jiggyscore` anyway, so it still only counts once. Worth doing properly
later; not worth a save-corruption risk now.

### Flags — three categories, three different scopes

| Category | Set | Get | Scope |
|---|---|---|---|
| File progress | `fileProgressFlag_set` | `fileProgressFlag_get` | permanent, save-backed, world-wide |
| Level-specific | `levelSpecificFlags_set` | `levelSpecificFlags_get` | one level |
| Map-specific | `mapSpecificFlags_set` | `mapSpecificFlags_get` | one map — switches, doors, gates |
| Volatile | `volatileFlag_set` | `volatileFlag_get` | **not replicated, see below** |

Highest vanilla file-progress flag is `FILEPROG_123_CHEAT_ENTERED`.

Two things that matter when hooking these:

- **Compare before sending.** Game code re-sets flags to the value they already hold constantly.
  Without a change check the reliable channel carries a continuous stream of no-ops.
- **Volatile flags are mostly per-player, not world state.** Reading through their uses
  (`VOLATILE_FLAG_2_FF_IN_MINIGAME`, dialog-shown and camera-mode flags) they describe the local
  player's session, and replicating them breaks the receiving player. The plan's §1.8 lists them
  as syncable; that is too coarse. The few genuinely world-scoped ones — e.g. the BGS jiggy-missed
  dialogs — need naming individually. That allowlist is not written yet.

## 16. `vendor/BanjoRecomp` is NOT what we run against

This cost a failed launch and is the easiest mistake to repeat.

| | What it is | Where |
|---|---|---|
| `vendor/BanjoRecomp` | depth-1 clone of **`main`** | source we read during recon |
| `runtime/BanjoRecompiled` | prebuilt **1.0.1** release | the binary that actually loads our mod |

`main` is ahead of 1.0.1, and the gap is exactly in the mod-facing API. Reading a
`RECOMP_EXPORT` or `RECOMP_DECLARE_EVENT` in `vendor/BanjoRecomp/patches/` proves **nothing**
about whether we can call it. Code written against the vendored source compiles, links, and
packages into a valid `.nrm` — then fails at game start with:

```
Failed to load mod code (Imported function not found:*:<name>)
```

Present in 1.0.1 (verified against the binary):

```
bkrecomp_extend_marker[_all]      bkrecomp_get_extended_marker_data   bkrecomp_get_marker_spawn_index
bkrecomp_extend_prop_all          bkrecomp_get_extended_prop_data     bkrecomp_get_prop_spawn_index
bkrecomp_note_saving_enabled/active   bkrecomp_notesaving_force_disabled
bkrecomp_notesaving_set_map_{static,dynamic}_note_count   bkrecomp_notesaving_clear_all_map_note_counts
bkrecomp_set_mumbo_token_fixes_enabled   bkrecomp_setup_custom_skinning
bkrecomp_{get,set}_drawn_model_transform_id / _skip_interpolation
```

**Absent from 1.0.1**, despite being in `main` — `bkrecomp_notesaving_get_note_saving_prop_extension_id`,
`bkrecomp_is_note_collected`, `bkrecomp_set_note_collected`, `bkrecomp_collect_dynamic_note`,
`bkrecomp_dynamic_note_collected_count`, and both note-collected **events**. (`is_note_collected`
and friends appear in the binary only as internal, unexported function names — a `strings` match
on the bare name is not evidence the export exists. Match on the `bkrecomp_`-prefixed name.)

**`make check-imports`** compares the built mod's base-recomp imports against what the shipped
runtime can resolve, and fails the build on a mismatch. Run it before trusting a build; it
reproduces the loader's resolution step without needing to start the game.

Vanilla game symbols are a separate matter and are fine — those resolve from
`bk.us.rev0.syms.toml` by address, not from the runtime's export table.

## 18. Enemies (Phase 5) — foundation only

### Why enemies are not collectibles

A note is one discrete fact that must never be lost, so it travels reliably as an event and is
deduplicated. An enemy is a continuous signal — position, facing, animation, every tick — where a
dropped packet is superseded by the next and re-sending a stale one is worse than useless. So
object state rides the *unreliable* channel next to player state, and only the newest frame is
kept on either side (`bc_object_frame`, `BCNET_MSG_OBJECTS`).

Frames carry the map they describe. Without that, a frame arriving just after a transition would
drive whatever happens to be standing in the new map to another map's coordinates.

### Identity does not work the way it does for props

This is the trap. A prop's spawn index is fixed static-level-data order, identical on every
machine — which is exactly why notes work. Actors spawn as play happens, so their order diverges
the moment two players trigger different things (§3). Only objects from a map's *static spawn
list* can use their spawn index directly; anything spawned dynamically needs an id assigned by the
owner and carried in a spawn event.

`bkrecomp_extend_marker_all` / `bkrecomp_get_extended_marker_data` /
`bkrecomp_get_marker_spawn_index` are all present in 1.0.1 (checked against the binary, per §16).

### Suppressing a client's enemy AI

`code_9E370.c:551` calls `marker->actorUpdateFunc` only `if (marker->actorUpdateFunc != NULL)`.
So a non-owner can silence an enemy's AI by nulling that pointer and stashing the original in
marker extension data — no patch, no fighting the owner's simulation. That is the intended hook
for "owner simulates, everyone else applies".

### Iterating live actors

`suBaddieActorArray` (data symbol, vram `0x8036E560`) is `{ s32 cnt; s32 max_cnt; Actor data[]; }`
from `prop.h`. Walking it is how the owner finds what to publish and how a receiver finds what to
apply. Actor pointers from that walk are used **within the call only** — `actor_new` reallocates
the array, and holding one across a frame is the §13 bug.

### Why nobody's AI is silenced

The first design silenced non-owners with `marker->actorUpdateFunc = NULL`. It was wrong, and the
reason is worth keeping: an enemy's **damage handler** lives in per-actor local state with no
generic dispatch point (`func_803889A0` for the grublin, a different one per enemy), while its
**death sequence** lives in the update function. Silencing the update function therefore leaves a
client able to hit an enemy but never able to kill it — exactly backwards for ganging up on one
together.

So every peer runs enemy AI. The owner additionally publishes position and animation, and
receivers snap to it only past `ENEMY_SNAP_DISTANCE`. Not a per-frame lerp: both machines run the
same AI, so small differences are the two simulations tracking each other, and correcting them
constantly would fight the local AI and read as jitter. Only a real divergence — an enemy chasing
a different player — is worth overriding.

The failure mode if any of this is wrong is enemies disagreeing about where they are, never
enemies frozen or unkillable. That is why it can be on by default.

### Death is detected generically, by disappearance

Any peer may declare an enemy dead: whoever lands the killing blow watches it die locally first.
Rather than hooking each enemy's death path — there is no common one — the layer diffs the set of
live enemy net ids against last frame's. Anything that dropped out died here, and gets reported as
a reliable `BC_EV_ENEMY_DEAD`.

Reliable, and deliberately not a flag in the object frame: death is a discrete fact, so a dropped
"it's gone" is never superseded and just leaves that enemy alive forever on one machine. Receivers
record the id and remove their copy on sight, which also covers a copy that has not caught up yet.

### Classification is an allowlist, not a heuristic

274 marker types cover enemies, scenery, doors, signs and collectibles, and no field cleanly
separates them. Silencing the AI of something that is not an enemy breaks it in ways that surface
far from the cause. `k_enemy_markers` in `src/mod/enemy.c` starts narrow — Grublin, Chimpy,
Snippet, Quarrie, and Bottles' tutorial vegetables — and widens as each type is seen working.
Spiral Mountain's vegetables matter because they are the only enemies a fresh save meets.

### Still to do

Ownership transfer by proximity — the owner is currently always the host, so enemy positions only
agree for players in the host's map. Respawn on map re-entry. Dynamically spawned actors
(projectiles, enemy-dropped items) are skipped entirely; they need owner-assigned ids carried in a
spawn event.

Health is not replicated. Each peer tracks its own copy's hits, so an enemy needing several hits
can be killed by either player independently rather than by their combined damage. Whoever
finishes it first reports the death and it vanishes for everyone, which is the behaviour that
matters most; genuinely shared health pools need damage-as-a-request to the owner.

## 19. Progression mirror (Phase 6)

### The gate is a single flag

`lair/code_0.c:1025` — `if (!fileProgressFlag_get(FILEPROG_31_MM_OPEN) && jigsawPicture_isJigsawPictureComplete(1))`.
Every world has the same shape: a `FILEPROG_xx_OPEN` flag, plus a puzzle-completion test over
`PICTURE_INFO[world-1].progressFlag`. World access is therefore entirely file-progress bits, and
it is checked live — mirror the bits and the door opens without re-entering the lair.

### Why a continuous mirror, not a replay on join

The first attempt replayed the host's state once when a peer joined. Wrong shape. A one-shot burst
can be dropped, mistimed, or raced against the client's own save load, and when it fails the
symptom is a player standing in front of a world that will not open with **no way to recover short
of reconnecting** — which is exactly what happened in testing.

The mirror is ~70 bytes of packed bits resent once a second on the reliable channel. Whatever went
wrong, the next one fixes it. That property is worth far more than the bandwidth, and it let the
one-shot dump be deleted rather than kept alongside — one mechanism instead of two.

### What it carries

The four permanent stores: file progress (0x124 bits), jiggies, empty honeycombs, Mumbo tokens.
544 bits, packed into 17 of the 20 available words, with an `#error` guard so a future store that
outgrows the packet fails the build rather than silently truncating — a truncated mirror would
*clear* real flags on every client.

Carried as 4-byte words because rdram's word-swapped layout makes sub-word access across the
boundary unsafe (§11). The mod packs and unpacks; the transport moves opaque words.

Applied as a **diff through the game's own setters**, never a byte copy: `fileProgressFlag_set`
recomputes two CRCs, so a raw write produces a save the game rejects. Diffing also keeps it silent
in the steady state where both saves already agree.

### It mirrors in both directions

A bit the host does not have is **cleared** on the client. That is what "the host's save is
authoritative" means and it is the plan's stated model — but it does mean **joining somebody's game
overwrites your own progression with theirs**. A client's own pickups still reach the host as
events first, so the mirror confirms them rather than undoing them.

Per-player inventory — health, lives, eggs, feathers — is deliberately not mirrored.

## 20. What we take from SM64CoopDX

CoopDX solved this problem well and is the reference. Its code is not transplantable — it is built
on a *decompilation* of a different game, where Mario is an `Object` with a struct and behaviours
are editable C, while we attach to a static recompilation of Banjo-Kazooie in which the player is
special-cased globals. Its *architecture* transplants completely, and the plan's legality note is
explicit that architecture is what we borrow.

Adopted so far:

| CoopDX idea | Where it landed |
|---|---|
| Three sync levels: player state, sync objects, mod data | player state (Phase 2), object frames (Phase 5), mod API deferred to Phase 9 |
| Everyone simulates; the owner corrects | `enemy.c` — and it is the *only* model that works here, since suppressing AI blocks kills |
| Deactivation sent reliably | `BC_EV_ENEMY_DEAD` on the reliable channel |
| Discard packets older than the last event id | object-frame sequence numbers, `accept_objects` |
| **Lowest player id in the area is the fallback owner** | `owns_enemies_here()` — replaced "the host owns everything" |
| Host-authoritative progression, shared star/jiggy counts | the Phase 6 mirror |

Why lowest-id beats proximity for ownership: every peer computes the same answer from state it
already has, so there is no negotiation, no transfer packet, and no window in which two peers both
believe they own the same enemy. Ownership moves on its own as players come and go.

Still to take: distance-based send-rate throttling (bounded min/max per object), spawn packets for
dynamically created actors, player list, chat, pickup toasts, revive/carry, and a mod-facing sync
API — the last being what turned CoopDX into a platform rather than a mod.

## 21. Actors latch their state once — the lair entrances especially

This is the same bug three times over, and it cost more debugging than anything else in the
project: **an actor decides something once, at spawn, and never looks again.** Notes that
respawned already collected. A jiggy left standing after somebody else took it. And finally the
world entrances.

`func_80388524` (src/lair/code_0.c) has two one-shot blocks. `!initialized` sets the world-open
flag if the puzzle is complete; `!volatile_initialized` performs the physical opening — the MM
door swings by setting `yaw = 270`, the CC bars and GV gate despawn themselves, the TTC chest lid
pitches. Ten entrance actors, each with its own idea of "open".

A mirrored client can walk into a lobby *before* the host's mirror arrives. The door latches shut,
then the flag turns on behind it. The player is left in front of a closed world holding every flag
that opens it, which reads exactly like broken sync — and walking out and back in fixes it, which
is the tell.

The fix is to clear `volatile_initialized` on those actors whenever the mirror changes anything,
so the game re-runs **its own** opening logic. We deliberately do not reimplement what "open"
means for ten doors; the game already knows.

**Generalise this:** anything gated on progression that a player can already be standing next to
needs a way to re-evaluate. Assume every actor caches its state at spawn until proven otherwise.

## 22. Diagnosing anything requires unbuffered logs

stdout through a pipe is block-buffered at 4 KB. Diagnostic lines therefore sat in the buffer
indefinitely while the game ran, and were lost outright on a crash. Several rounds of this project
were spent guessing at causes because the evidence existed and was invisible.

`bcnet_init` now calls `setvbuf(stdout, NULL, _IOLBF, 0)`. There is also an unconditional
once-a-second heartbeat (`hb frame= status= connected= remotes= map=`) whose *absence* is itself
the diagnosis — everything else only prints on a change or while connected, which makes silence
ambiguous.

`make run` / `make run-p2` tee to `run/logs/`; `make debug` / `make debug-p2` to
`build/crash-p*.log`. `make diag` reads them back.

**And: `make test-modlogic` compiles the real `src/mod/progress.c` against fake game accessors.**
The mod's code only loads once a game session starts, so its logic cannot be observed by launching
the runtime — without this harness the only way to check a change was to ask a human to play
Banjo-Kazooie, which is a terrible way to find an off-by-one.

## 23. The host's saved notes are readable only from the world

Notes are save data, but there is no way to query them on 1.0.1: BanjoRecomp keeps collected notes
in a save extension whose accessors (`bkrecomp_is_note_collected` and friends) exist only on
`main` (§16). So the progression mirror cannot carry them the way it carries jiggies and flags.

The world is the record instead. `note_saving` clears the alive bit of every already-collected
note as the map loads, so on arrival the host can walk the cube list and read which notes its save
says are gone — the same walk that assigns note identity. Each one is marked and announced as an
ordinary collection event, after which it replicates like any note collected live.

Host only, obviously: a client's own dead notes say nothing about the session.

This is why notes the host had collected in an *earlier* session kept appearing for clients while
notes collected *during* a session synced fine. The live path was never broken; there was simply
no event for something that happened before anyone was watching.

Worth noting the queue sizing: Mumbo's Mountain has 85 static notes, and this announces a whole
map's worth in one frame, so `BCNET_EVENT_QUEUE` is 128. At 64 a mostly-completed save would have
silently dropped notes past the limit.

## 24. The save file is the session's save

Nothing in memory can answer "which notes has the host collected in a map nobody has visited this
session". BanjoRecomp keeps per-note state in a save extension with no accessor on 1.0.1 (§16),
and the world only knows about the map currently loaded. The mirror could therefore only ever
carry the host's *current* map — so a client walking into a world first saw every note standing,
and no amount of in-memory replication was going to fix it.

The save file knows all of it. On join the host reads its own save (path via
`recomp_get_save_file_path`, read natively — mod code cannot touch files) and sends it. The client
writes it under a `banjocoop/` subfolder beside its own and calls `recomp_change_save_file`, which
`ultramodern::change_save_file` implements as a genuine `read_save_file()` — the cartridge save is
reloaded from disk.

The client's own save is never overwritten; disconnecting and reloading it restores their game.

**Caveat:** the save arrives at join, but the game only reads a file into RAM at file select. A
client already in-game when it connects must return to file select and load once.

## 25. Jinjos: an ADD, not a bit set

`__chJinjo_802CDBA8` credits a Jinjo with `item_adjustByDiffWithHud(ITEM_12_JINJOS, 1 << (id + 6))`
where `id` is the marker (0x5A..0x5E) — landing on bits 0..4 only because MIPS masks shift counts
to five bits.

Because it is an *add*, crediting one twice carries into the next colour's bit and corrupts the
mask. So Jinjos are replicated as an event guarded on the bit not already being held, never
through the progression mirror, and the receiver despawns its own copy — otherwise walking into it
credits a second time. This is why they were left out until the rest was stable.

## 26. Enemy coverage

`k_enemy_markers` in `src/mod/enemy.c` now covers the common enemies of Spiral Mountain through
Rusty Bucket Bay. Two categories are deliberately excluded:

- **Bosses** (Conga, Nipper, Mr Vile, Gobi, Grunty) each run a bespoke arena state machine that
  position-and-animation replication does not describe. Phase 8. Syncing them badly is worse than
  not syncing them.
- **Projectiles and anything spawned during play** have no shared identity — spawn order diverges
  between machines (§3). They need owner-assigned ids carried in a spawn event.

## 27. Pooled damage — how ganging up actually works

Every peer simulates enemies independently, which means a hit landed on one machine is invisible
to the others. An enemy needing three hits could be worked on by two players indefinitely and
never die, because each machine only ever saw its own player's hits. That is the opposite of
ganging up.

Enemy health has no generic accessor — it lives in per-actor state with a different shape per
enemy — so it cannot be read, replicated, or written. But **damage** has exactly one dispatch
point: `marker_callCollisionFunc(this, other, type)` (core2/code_A5BC0.c:1235) routes an enemy's
"ow" and "die" handlers for every enemy in the game.

So hits are observed there and replayed there. A receiver finds the enemy by net id and calls the
same dispatcher with its own `playerMarker` — whatever "being hit" means for that enemy, it means
the same thing as a local hit. No enemy-specific knowledge, no health replication.

`s_replaying_hit` guards the hook, or a single hit would bounce between peers forever.

Only types 0 (ow) and 2 (die) are reported; type 1 fires constantly during ordinary contact.

### Consequence: enemy drops stopped double-counting

Dropped notes used to be relayed for score, which counted one dropped note *once per player* —
the collector granted everyone else a note on top of the one they were about to collect. Two
players clearing the same enemies drifted steadily upward.

With damage pooled, an enemy now dies on every machine at the same moment and drops on every
machine, so each player collecting their own copy reaches the same total with nothing sent at all.
Dynamic notes are no longer reported. The failure mode flips from guaranteed over-counting to a
possible under-count if somebody leaves theirs lying, which the desync detector surfaces.

### Still not replicated: actors spawned mid-play

Projectiles and other runtime spawns still have no shared identity (§3). With every peer
simulating, they are produced independently on each machine from the same synced enemy state, so
they broadly agree without replication. Making them authoritative would require suppressing the
local spawn on non-owners, and a spawn cannot be cancelled from a `RECOMP_HOOK` — hooks observe,
they do not alter control flow. That needs either a `RECOMP_PATCH` of the spawn path (exclusive,
and the base recomp already patches 146 functions) or owner-assigned ids in a spawn event.

## 28. Ownership without the host — and the relay it needs

Ownership is the lowest player id **present in a map**, not the host (§20). So whenever the host
is somewhere else, a client owns that map's enemies and projectiles — which is the common case
under free-roam, not an edge case.

That exposed a hole: `service_host` handled HELLO, STATE and EVENT but **not** `BCNET_MSG_OBJECTS`.
A client owner's frames arrived at the host and were silently dropped, so enemies and projectiles
only ever synced in whichever map the host happened to be standing in. Everything looked fine in
two-player testing with both players together, which is exactly how it survived.

The host now relays object frames verbatim — sequence number included, so receivers can still
discard one that overtakes a newer frame — to peers whose state says they are in the map it
describes. The host does not adopt them; it is not in that map.

Two traps this turned up:

- **Sequence numbers are per sender.** Walking into a map owned by somebody else means a counter
  starting from its own zero, which the old check read as "stale" and dropped — freezing that
  map's enemies until the new owner's counter happened to overtake the old one. `accept_objects`
  resets the counter when the frame's map changes.
- `service_host` and `service_client` share the shape `} else if (type == BCNET_MSG_...)`, so a
  blind textual insert lands in **both**. It did, and the copy in `service_client` shadowed the
  real handler — clients silently stopped applying object frames at all. The regression test for
  three players caught it immediately; two-player tests never would have.

## 29. Message relay audit

Every message type, who sends it, and whether the host has to pass it on. Built from the source
rather than memory, after a relay gap survived weeks of two-player testing.

| Message | Sent by | Host | Client | Relay needed |
|---|---|---|---|---|
| `HELLO` | client | handles | — | no — handshake |
| `WELCOME` / `REJECT` | host | — | handles | no — targeted |
| `STATE` | both | handles + relays | handles | **yes** |
| `EVENT` | both | adjudicates + relays | handles | **yes** |
| `OBJECTS` | both | relays (map-scoped) | handles | **yes** |
| `PROGRESS` | host only | — | handles | no — host is the only authority |
| `SAVEFILE` | host only | — | handles | no — targeted at the joiner |
| `PEERS` | host only | — | handles | **yes** |

`PEERS` was declared from the very first version and **never sent or handled by anything**. What
that cost: the host cleared its own record when somebody disconnected but told nobody, and a
departure is indistinguishable from a player going quiet in the state stream. So the remaining
clients kept the leaver in their roster forever, the mod kept being told about them, and their
puppet stood frozen in the world for the rest of the session.

Only three players show it. With two, the survivor is the host, which does clear its own record.

The host now broadcasts the roster on every join and departure, and clients treat it as
authoritative: any slot missing from it is cleared, `has_state` and all, which is what makes the
puppet go away. Names ride along, which the Phase 7 player list will want.

**The rule this suggests:** a message is suspect whenever a client can send it, or whenever it
carries a fact one peer knows and the others cannot derive. Both gaps found so far were the second
kind — a client owning a map's objects, and a player leaving.

## 30. recompui and the player list (Phase 7)

The whole `recompui_*` surface is present in 1.0.1 — contexts, elements, labels, text inputs,
callbacks, flexbox layout and styling. Enough for the player list, chat and toasts the plan asks
for. Verified against the binary, per §16.

Shape of it:

- One context, built **once**. `recompui_create_*` allocates, so rebuilding the list every frame
  would leak steadily for a whole session. A fixed row per player slot plus `recompui_set_text` is
  free by comparison.
- Mutations must be wrapped in `recompui_open_context` / `recompui_close_context`.
- `recompui_set_context_captures_input(ctx, 0)` and the mouse equivalent — the game is being
  played underneath the overlay and must keep every button.
- Refreshed twice a second, not per frame. Ping and position move constantly; nothing here is
  worth re-laying-out the interface at 60 Hz for.
- Layout is in DP against a fixed `RECOMPUI_TOTAL_HEIGHT` of 1080, so it is resolution independent.

### No libc

The mod builds `-nostdinc` with no libc at all, so there is no `sprintf`. `ui.c` carries small
`str_put` / `str_put_u32` helpers and formats rows by hand.

### Names cannot ride in the staging structs

Everything else the mod receives is 4-byte fields in `bc_incoming`, copied wholesale. Strings
cannot be: rdram's word-swapped layout means byte N lives at N^3 (§11), so a struct copy produces
scrambled text. `bcnet_player_name` fetches one at a time and writes it byte-wise through `MEM_B`.

`func_8030AD00(map)->name` gives the map's display name, so the list says "Mumbo's Mountain"
rather than a number.

## 31. Transformations, bosses, cutscenes (Phase 8)

### A puppet's model is fixed at spawn

`ActorInfo` names one `modelId`, and a spawned actor takes it from there — there is no way to swap
a model in place. So a remote player changing form means despawning the puppet and respawning it
as a *different actor type*.

That is decided by the free actor-id range. `0x3CC..0x3D4` is nine ids (§13), and there are seven
transformations, so there is one ActorInfo per form and no room to spare. `k_form_models` is
exactly as long as `enum transformation_e`, indexed directly by it.

Respawning happens **only on an actual change**, or a transformed player would be replaced every
frame.

| Form | Model |
|---|---|
| Banjo | `ASSET_34E_MODEL_BANJOKAZOOIE_HIGH_POLY` |
| Termite | `ASSET_34F_MODEL_BANJO_TERMITE` |
| Pumpkin | `ASSET_36F_MODEL_BANJO_PUMPKIN` |
| Walrus | `ASSET_359_MODEL_BANJO_WALRUS` |
| Croc | `ASSET_374_MODEL_BANJO_CROC` |
| Bee | `ASSET_362_MODEL_BANJO_BEE` |
| Wishy-washy | `ASSET_356_MODEL_BANJO_WISHYWASHY` |

**Falls back to plain Banjo when the form's model is not in the asset cache** — which happens
whenever the other player is transformed in a world whose assets we have not loaded. The plan
wanted every form pinned at startup so this never arises, but that needs an asset cache API the
1.0.1 runtime does not have (§16). The fallback is not cosmetic caution: `actor_draw` dereferences
`assetcache_get`'s result with **no null check**, so spawning a puppet with an unloadable model is
a segfault on its first frame.

### Bosses turned out not to be bespoke

The plan expected each boss to need its own arena state machine, on the assumption that one peer
would simulate them and the rest be shown the result. That is not the architecture that emerged:
every peer runs the fight, damage is pooled through `marker_callCollisionFunc`, and death is
announced. A boss is therefore mostly a tougher enemy.

Conga, both Nippers, Mr Vile, all three Gobis, every Boss Boom Box size, Motzand and Wozza are now
synced like any other enemy.

**Not covered:** each boss's cutscenes, camera takeovers and phase transitions run independently
per machine. They are driven by the same flags and the same damage so they stay broadly in step;
where they do not, the symptom is a cutscene at slightly different moments, not an unwinnable
fight.

**Grunty is included** — see §32. The final fight turned out to be synchronisable through its own
phase setter rather than needing bespoke work.

### Cutscenes: local by design

The plan's §1.7 recommended remote players keep control and a cutscene plays only for whoever
triggered it. That is what happens, and it needs no code: cutscenes are local, and the
*consequences* — the flags they set — already replicate through the progression mirror and the
flag events. A player watching a cutscene simply stops moving from everyone else's point of view.

Not done: a shared skip vote for progression-gating cutscenes.

## 32. Bosses are scripted, not statistical

Ordinary enemies need no state replication: their behaviour is a consequence of position and
damage, and both already agree on every machine. A boss is different. A boss is a *script*, and a
peer a phase behind is fighting a different fight in the same room — one player watching a death
animation while another is still being chased.

Two mechanisms cover every boss in the game, and neither is per-boss work.

### `subaddie_set_state` is the generic phase machine

Seventy-odd actor types drive their state through it, Conga included. Hooking that one call and
replicating it for an allowlist of bosses keeps their phases in step without knowing anything
about any particular boss.

The allowlist is deliberately far smaller than the enemy list. `subaddie_set_state` fires
constantly across the whole game, and replicating all of it would flood the reliable channel for
no benefit — ordinary enemies do not need their state told to them.

### Gruntilda has her own

Her six phases (`FINALBOSS_PHASE_0_INTRO` … `5_JINJONATOR`) run through
`chfinalboss_setPhase`, not the generic setter, and the fight has discrete moments no state change
describes. All of them are single, explicit, hookable functions:

| Moment | Function |
|---|---|
| Phase transition | `chfinalboss_setPhase` |
| A Jinjo statue appears | `chfinalboss_spawnStatue` |
| Statue activated | `chfinalboss_setJinjoStatueActivated` |
| Grunty defeated | `chfinalboss_setBossDefeated` |
| Jinjonator strike | `chjinjonator_attack` |
| Final blow | `chjinjonator_finalAttack` |

Her spells (`GRUNTY_SPELL_FIREBALL`, `GREEN_BLAST`, `GRUNTY_SPELL_GREEN_ATTACK`) are replicated as
projectiles. A fireball that exists on one screen and not another is the difference between a fair
fight and an unwinnable one.

The Jinjonator's strikes matter most: they are the damage that actually ends the fight, and they
are earned from the Jinjos *each* player rescued — so they have to land for everyone, not only for
whoever triggered them.

**This is the point of the whole project.** A co-op mod that cannot finish the game has not
finished its job, and the plan's assumption that each boss needed a bespoke arena state machine
turned out to be wrong — the game already had the seams.

## 33. Revive and the mod API (Phase 9)

### Respawning next to a teammate

Dying in vanilla returns you to wherever the map started you. In co-op that is the single most
tedious thing that can happen — the party splits and whoever died spends the next minute walking
back. Two players in one world end up regrouping more than playing.

`bsdie_init` marks the death; once `bs_getState()` leaves `BS_41_DIE` the sequence has finished
and the game has put the player wherever it wanted them, which is the moment to move them beside
the nearest teammate in the same map.

Three guards, each for a reason:

- **Same map only.** Dying in one map and coming back in another means the game sent you somewhere
  deliberately — a game over, a level exit — and second-guessing that would be wrong.
- **A distance limit.** A teammate far enough away is probably somewhere unreachable: mid-flight,
  inside a Mumbo hut, part-way through a transition. Dropping a player there is worse than the walk.
- **A small offset**, so two players do not arrive inside one another.

Deliberately unchanged: whether a life is spent, and health on respawn. Both alter the game's
difficulty rather than its logistics, and the plan puts lives in the per-player column.

### The mod API

`src/mod/banjocoop_api.h` is the public header — copy it into another mod. Six entry points:

```c
u32  banjocoop_is_connected(void);
u32  banjocoop_local_player_id(void);
u32  banjocoop_player_count(void);
u32  banjocoop_is_host(void);
void banjocoop_send(u32 tag, u32 scope, u32 a, u32 b);
/* RECOMP_CALLBACK("banjocoop", banjocoop_on_message) */
```

Everything else in this project is BanjoCoop deciding what is worth replicating. This lets another
mod decide for itself and inherit the parts that were hard: reliable delivery, ordering,
attribution that cannot be spoofed, and routing by map or level.

**Scope rides in the event's own map and level fields**, not in the payload. That is not a trick
to save a word — it means the host's existing router handles these without knowing they are
somebody else's messages, so a mod gets free-roam-correct delivery without having to know
free-roam exists.

Two payload words rather than three, because the tag has to travel and an event carries three
fields. Two values plus a tag covers "something happened to object N", which is what this is for;
anything larger wants its own state, not a message.

`banjocoop_is_host` matters more than it looks: a mod that resolves something on every peer counts
it N times. It is the same trap collectibles hit, so the API exposes the answer rather than
letting each mod rediscover it.

Kept small on purpose. A wide API is a promise to keep it working.

## 34. Carrying, and the party modes (Phase 9)

### The game already had a carry mechanic

`BS_3A_CARRY_IDLE` / `BS_3B_CARRY_WALK` are how Banjo hauls Freezeezy Peak's presents, Click Clock
Wood's acorn and Blubber's gold, and the mechanic takes an `ActorMarker` — it does not care what
the marker is.

A teammate is already a real actor on your screen: their puppet. So carrying a player needed no
new movement code, animation or physics. `bacarry_set_marker(puppet)` then
`bs_setState(BS_3A_CARRY_IDLE)`, and the game does the rest including the walk cycle and the throw.

The genuinely new problem was the other half, and it is circular if approached naively: on the
carrier's machine the game moves the puppet, but the puppet's position normally *comes from* the
carried player, who would then be following themselves. Broken by splitting ownership —

- the carrier stops applying network state to that puppet, so the game owns it outright
- the carried player takes their position from the carrier rather than from their own input

Overriding the carried player's position every frame is also what makes the carry stick: anything
they walk is simply undone, so no separate movement lock is needed.

### Modes are a rule, not a system

Race and hide-and-seek are deliberately thin. Positions and maps already arrive every frame,
events are reliable and attributed, and the overlay can show text — so a mode is a rule about data
that already exists. That is also the honest test of the layer underneath: if a mode needs new
plumbing, the plumbing was wrong.

The host judges. Not for trust reasons — somebody has to decide who won, and every peer deciding
independently is how you get two winners, which is the lesson the collectibles taught.

Race's finish line is wherever the host stood when the round began. A fixed landmark would need
per-map data for every map in the game; "where I am now" needs none, and lets a host pick any
finish just by standing there.

## 35. Send rates by distance (next.md §5)

Everything owned used to be published every frame to whoever would listen. Three separate wastes
sat behind that, and they are worth separating because only one of them is really about bandwidth.

### The host was not routing its own objects

`Transport::broadcast_objects` used `kBroadcast`. On a client that is right — its only peer is the
host, which relays. On the host it sent every object frame to every client regardless of map, and
`accept_objects` discards a frame for another map on arrival.

The host was *already* filtering correctly when relaying somebody else's frame; only its own
frames escaped. The asymmetry mattered more than it looks, because the host is the peer most
likely to be alone somewhere while everyone else is together elsewhere.

Player state is deliberately left unfiltered. The player-list overlay needs every peer's `map_id`
every frame no matter where they are, and 48 bytes per peer per frame is not the problem.

### Three bands, phased

`src/mod/tier.h` puts each owned enemy in a band by its squared distance to the **nearest remote
player in that map** — remote only, because our own screen is not what the object frame is for.
Near publishes every frame, mid every third, far every eighth, and beyond that nothing at all.

Bands rather than a smooth falloff because of what a receiver actually does with the data: it only
corrects an enemy past `ENEMY_SNAP_DISTANCE`. What matters is whether a correction arrives inside
the time it takes to drift that far, which three bands express as well as a curve would while
staying readable in a log line.

The phase is the part that is easy to leave out and shouldn't be. Gating on the tick alone puts a
map's whole mid band on the same frame, so the object frame alternates between empty and over
budget. Same total traffic, far worse peaks — and it is the peaks that overflow
`BCNET_MAX_OBJECTS`. `(tick + net_id) % period` spreads them for free.

**The distances are guesses.** Nothing in the codebase establishes BK's world scale;
`ENEMY_SNAP_DISTANCE = 220.0f` is the only anchor, and it measures something else. The `objtier`
line exists to replace them with measured values — `make diag` prints it back, with what its
numbers mean.

### Enemies could starve projectiles out of the budget

Found on the way past, and a real defect rather than an inefficiency. `enemy_sync` filled
`out->objects` up to all 24 slots before `projectile_sync` ran, and `projectile_sync` bails once
full. 24 live enemies in a map therefore meant Grunty's fireballs were never published at all.
`BC_PROJECTILE_RESERVE` holds 8 slots back. Unused slots cost nothing — the frame carries `count`
objects, not a fixed 24.

### Enemies never cleared `actor_id` (fixed here)

Also found on the way past, also pre-existing. The outgoing staging buffer is reused between
frames and only `count` is reset, so a slot that carried a projectile still held its actor id — and
a non-zero actor id is exactly how a receiver decides an object is something it must spawn. An
enemy landing in a slot a projectile used to occupy would have every receiver spawn a phantom
orange at that enemy's feet. Latent before, and the nearest-first ordering would have made it
routine, since slots no longer keep the same role frame to frame.

### Projectiles are exempt, and must stay exempt

This is the rule to remember. An enemy missing from a frame is one the receiver was not told
about; it keeps running its own copy and nothing is lost. A projectile missing from a frame is one
the receiver **despawns** — that omission is what gives a projectile a lifetime without a second
message to end it. Throttling a distant projectile would delete it mid-flight on every screen but
the owner's. They are also few (16 max), so there is nothing to save.

## 36. The spawn patch — suppressing what a non-owner should never have made

BanjoCoop's only `RECOMP_PATCH`, in `src/mod/spawn.c`. Worth reading before adding a second.

### Why a patch and not a hook

The decision needs the `Actor` the spawn produced, and only a patch can see a return value —
return hooks get a clobbered context (§14). Patches are **exclusive**: any other mod patching the
same function conflicts. That is affordable here only because
`__actor_spawnWithYaw_s32` is the single funnel — `actor_spawnWithYaw_s32` / `_f32` / `_s16`,
`spawn_child_actor` and `func_803055E0` all route through it — so one slot covers every way an
enemy can throw something.

Checked against the base recomp's 145 patches before committing to it: it patches `marker_init`,
`__codeA5BC0_initProp2Ptr` and a long list of `*_draw` functions, but nothing on the spawn path.
Per §16, that check has to be against what the *runtime* has, not what `vendor/BanjoRecomp` shows —
though for a patch of a vanilla function the risk is collision, not resolution, since game symbols
resolve by address from the syms toml rather than through the runtime's export table.

Symbols the copied body needs, all confirmed present: `sSpawnableActorList` and
`sSpawnableActorSize` (datasyms), `dummy_func_80320248` (syms), `ActorSpawn` (`prop.h`).

### Suppression is a despawn, and must never be a NULL

The obvious implementation — return NULL for a suppressed actor — crashes.
`__chSnowman_spawnSnowball` writes straight through the result with no check. `chConga` does check,
but that is one caller's habit, not a contract; §13 is explicit that nothing here is NULL-checked,
and the same applies to return values.

So the actor is spawned exactly as normal and immediately `marker_despawn`ed. The caller gets a
valid `Actor` to write velocities into and never notices; the update loop skips `despawn_flag`
actors so its AI never runs; the end-of-frame flush frees it. One wasted actor slot for a fraction
of a frame, against a guaranteed crash the first time a Sir Slush throws a snowball.

### What it replaced, and why the flicker was worse than it looked

Non-owners used to spawn their own projectile and despawn it afterwards. The ordering made that
worse than one frame: `world_apply` runs the deferred despawns near the top of the frame hook, but
`projectile_sync` — which queues them — runs after it, so a projectile noticed this frame survives
until the next.

### Two guards it cannot work without

- **`projectile_is_adopting()`**. `projectile.c` spawns the owner's projectile through
  `actor_spawnWithYaw_f32`, which funnels straight into the patched function. Without the guard a
  non-owner suppresses the one projectile it is meant to keep, and no projectile ever appears on a
  client. Same shape as `s_replaying_hit`, and the same class of bug it prevents.
- **`enemy_owns_objects_here()`**. Ownership is recomputed every frame in `enemy_sync`, but the
  patch fires from inside game code wherever an enemy decided to throw something — nowhere near an
  `inc`. It reads the frame's cached answer, the same arrangement as `bc_map_id`. Defaults to
  owning nothing, so nothing is suppressed on the strength of a guess.

Every condition in `suppressed()` defaults to *allowing* the spawn. A projectile wrongly kept
costs a frame of flicker; one wrongly suppressed is a shot that never happens, and possibly a fight
that cannot be won.

### `bc_defer_despawn` stays as a safety net

`projectile.c` still despawns unmatched local projectiles. With the patch in place it should
almost never fire. If the logs show it firing regularly, the funnel is not the only spawn path —
which is worth knowing.

## Still to confirm (open items)

- Model/animation load path for pinning Banjo + transform assets (Phase 4).
- Confirm `AnimCtrl` layout (animation ID + current frame) for puppet replication — struct not found
  in `include/*.h` by a shallow grep; likely in a `core2` subheader.
- Volatile-flag allowlist (§15) — which volatile flags are world state rather than player state.
- Late-join snapshot: a player joining mid-session receives no world-state backlog, so they see
  notes and switches in their vanilla state. Needs the host to serialise its registry (Phase 6
  territory, but it will bite before then).

## Local environment status

| Item | State |
|---|---|
| ROM (raw rev0, sha1 `1fe1632…`) | verified — `~/Downloads/Banjo-Kazooie (U) (V1.0) [!].z64` |
| Decompressed ROM | built — `vendor/BanjoRecomp/banjo.us.v10.decompressed.z64` (17,550,624 bytes) |
| BanjoRecompiled runtime | prebuilt v1.0.1 Linux-X64 in `runtime/`, launches (x11, RADV) |
| `bk_rom_compressor` | built via rustup (`~/.cargo`) |
| Mod template + submodules | `vendor/BKRecompModTemplate` with `bk-decomp` + `BanjoRecompSyms` |
| clang 14 + ld.lld 14 | installed, MIPS backend confirmed |
| `RecompModTool` | `tools/RecompModTool`, prebuilt |
| **BanjoCoop mod** | **builds → `build/banjocoop.nrm`, installed to `runtime/.config/mods/`** |

Build with `make`, install with `make install`, launch with `make run`.
Run `make check-imports` before trusting a build (§16).

### The second instance lives in `run/`, not `build/`

It used to sit under `build/instance2`, which meant **`make clean` destroyed player 2's save file
and settings**. The cost was not the rebuild — it was re-importing the ROM and then sitting
through the whole intro again to create a new file, repeatedly, without it being obvious why the
save kept vanishing. It also silently reverted the mod config to player 1's copy, turning the
second instance into a second host that quietly never connects.

`run/` holds no build artifacts, so `clean` has no business there.

`scripts/setup_instance2.py` now *forces* `netmode = Join` and `player_name = player2` on every
`install-p2`, preserving every other key. Forcing rather than seeding matters twice over: the
settings are copied from the host, and the runtime rewrites this file whenever the mod's config
options change — which reintroduces the copied values.

### Save snapshots

Testing late-join and shared progress means putting a player back to a known state over and over,
and doing that by deleting the save costs the entire intro every time.

```
make snapshot-p2    # once, with player 2 in the state you want to return to
make restore-p2     # instantly, forever after
```

Snapshot a file that already exists and has had the intro watched, and restoring never replays it.
`snapshot-p1` / `restore-p1` do the same for the host.

### The test suite picks its own port

`pick_port()` derives from the pid. A fixed port collided both with a real game hosting on the
default 34567 and with the suite's own sockets still in TIME_WAIT from a run seconds earlier,
producing a wall of failures that looked exactly like a replication regression. That cost two
false alarms before it was changed.

### Phase 3 — what is actually confirmed in-game

Two instances over loopback, both in Spiral Mountain:

| Path | State |
|---|---|
| Host + client connect, puppets visible | confirmed |
| Jiggy: remote despawn **and** total | confirmed — the total needed `ITEM_26_JIGGY_TOTAL`, see §15 |
| Empty honeycomb (hex piece) | confirmed |
| Mumbo token | implemented, same shape as honeycomb — **not yet exercised** |
| Static notes: despawn, count, double-collect revoke | **not yet exercised** — Spiral Mountain has no notes; test in Mumbo's Mountain |
| Flags (switches, doors, gates) | **not yet exercised** |
| Desync detector firing on a real divergence | **not yet exercised** |
| Latency matrix (0 / 80 / 150 ms, 3% loss) | now runnable in-game — see below |

The headless transport tests cover arbitration, scoped routing and loss tolerance, but they know
nothing about the game — every row above marked "not yet exercised" needs two real instances.

### Running the latency matrix in-game

The transport has always had a latency/loss simulator, but until now nothing in the mod called
`bcnet_set_sim`, so it was reachable only from the headless tests — the "under 150 ms" half of the
Phase 3 exit criterion was untestable in a real session.

Three debug config options now drive it: **Simulated Latency**, **Simulated Jitter**, and
**Simulated Packet Loss** (per mille, so 30 = 3%). They are polled once a second, so the matrix
can be stepped through from the mod menu mid-session without restarting either instance. Settings
apply to that peer's *outgoing* packets, so putting 150 ms on one side only already gives an
asymmetric link.

### The simultaneous-grab bug (found by tracing, not by testing)

Two players grabbing the same note is nearly impossible to stage by hand, so it was reasoned
through instead — and the original design was wrong.

The host used to send the loser of a race a `NOTE_REVOKE` telling it to deduct the point. Trace it:
both players' pickups give each of them +1 locally and mark the note; the host relays the winner's
claim, which the loser ignores via its own "already recorded" check; then the revoke arrives and
**drops the loser to zero.** They lose the note entirely.

Notes are shared progress — every player's counter shows the level total — so each player should
count each note exactly once, and the loser's own pickup already *is* that count. The receiver-side
"have I already recorded this note" check is the only thing needed to prevent double-counting. The
host now simply drops the duplicate claim and sends nothing back. Event kind 3 is retired.

## 17. Free-roam (Phase 4)

Most of what free-roam needs was built into Phase 3 deliberately: `map_id` and `level_id` ride in
every state packet, puppets only spawn for peers in your map, flags route by map/level scope, and
the registry is keyed by map. Two things were genuinely missing.

### Arrival replays a map's state

A player entering a map has missed everything that happened there while they were elsewhere; a
player who just joined has missed everything, everywhere. The host detects arrival straight from
the state stream — the client is already reporting its map every frame — so there is no request
protocol. `PeerSlot::snapshot_map` holds the last map replayed to that peer; any change triggers
`send_map_snapshot`.

The snapshot is sent as ordinary `NOTE_STATIC` events rather than a bespoke message, so there
remains exactly one piece of code that knows how a collected note is applied. Replaying a map to a
peer that is already up to date is harmless — the receiver's "have I already recorded this note"
check absorbs it — which is what allows firing unconditionally on arrival instead of tracking per
client what has been delivered.

### Level transitions wipe the note counter

`itemscore_levelReset` zeroes the level's note count and note_saving restores it from the save
file. Notes replicated from other players are deliberately *not* in that save file, so without
correction the shared total collapses to whatever the local player had personally saved, every
time anyone re-enters a level.

Hooked at entry (the level argument is only readable there), but the correction is deferred to the
frame hook — running it inside the hook would just be undone by the reset it is standing in front
of. The recount totals the registry per level via `func_8030AD00(map)->level_id`, and only ever
*raises* the count: the registry holds what this session replicated, the restored value holds what
the local player saved previously, and those are different sets. Reconciling them properly needs
host-authoritative saves (Phase 6).

### "Recorded" and "removed" are different questions

A note collected while you were in another map gets *recorded* but not *despawned* — there was
nothing to despawn, its props do not exist outside that map. Walking in later spawns them fresh.

The apply path originally guarded the whole branch on "have I already recorded this note", so on
arrival the replay hit that check and returned before despawning. Result: a correct note count
sitting next to a full set of still-collectable notes. The two questions are now separate — the
record-and-credit half is guarded, the despawn always runs.

There is also an **arrival sweep** that clears already-recorded notes out of the map in one pass,
independent of the host's replay. It exists because the replay cannot retry: it arrives once, at
whatever moment the network delivers it, which may be before the cube list is populated. The sweep
re-runs every frame until `sCubeList` is actually there.

### Late joiners need the whole world, not just notes

Everything here replicates *changes*, so a player who was not connected when a change happened
never hears about it. With a fresh save that is the entire game — worlds still locked, jiggies
still standing, because they were collected before that player existed.

The host therefore replays its full world state whenever the roster grows: jiggy, honeycomb and
Mumbo-token bits, then every set file-progress flag. The flags are the part that actually matters
for progression — they gate which worlds are open and which note doors are paid, so a late joiner
without them stays locked out no matter how many jiggies they are handed.

Emitted a slice at a time (`DUMP_EVENTS_PER_FRAME`): a full dump can run to several hundred events
and the outgoing queue holds 64 per frame. Overflowing it would silently drop world state — the
exact failure being fixed. Blind re-sending is safe because every apply is idempotent and guarded
on the value actually changing.

### Still open for Phase 4
- **No asset pinning.** The plan assumes a BanjoRecomp asset cache/replacement export exists; the
  1.0.1 runtime has no such API (only `bkrecomp_setup_custom_skinning` and the drawn-model
  transform helpers — §16). The puppet already renders base Banjo unconditionally, which is the
  plan's own stated fallback, so a transformed remote player appears untransformed rather than
  broken.
- **No player-list UI.** `recompui_*` exports are present in 1.0.1 but nothing is built on them;
  that is Phase 7.

### Still open for Phase 3

- **Exit criterion unverified.** Notes have never run in a real session; Spiral Mountain has none.
- **Ownership layer** (owner simulates, proximity assignment, transfer on disconnect) — deferred to
  Phase 5. For a static world, host authority is the degenerate case; ownership only earns its
  keep once objects are actually simulated.
- **Moving platforms** — deferred with it, for the same reason: continuous state, not events.
- **Volatile flags** — deliberately excluded, see §15.
- **CI** (plan's Verification §5, also a Phase 0 exit criterion) — does not exist. The project is
  not under version control at all, so there is no history to bisect when a regression appears.

### Toolchain gotchas

- We do **not** build BanjoRecomp from source (only relevant if we forked), so BUILDING.md's
  `libsdl2-dev`/`libgtk-3-dev`/`llvm` are unnecessary. But `clang` **and `lld`** are both required —
  the mod link step calls `ld.lld` directly.
- **RecompModTool resolves `[inputs]` paths relative to the toml file's own directory**, not the
  CWD. Hence the `../` prefixes in `config/mod.toml`.
- The prebuilt release needs **SDL ≥ 2.26** (`SDL_GetWindowSizeInPixels`); Ubuntu 22.04 ships
  2.0.20 and the binary dies at startup with a symbol lookup error. Worked around by borrowing
  gamescope's bundled 2.26.3 via `LD_LIBRARY_PATH=/opt/gamescope/lib/x86_64-linux-gnu` (baked into
  the Makefile `run` target). No install required.
- **Portable mode does not work in the prebuilt release.** The binary contains a
  `.config/portable.txt` string, but creating that marker next to the executable has no effect —
  it still uses `~/.config/BanjoRecompiled`. Mods go in `~/.config/BanjoRecompiled/mods`, and the
  game imports the ROM there itself as `bk.n64.us.1.0.z64`.
  The Makefile's `BANJO_CONFIG` variable overrides the config dir, which is how Phase 1 will run
  two instances side by side:
  `make run BANJO_CONFIG=$HOME/.config/BanjoRecompiled-p2` — needs verifying that the runtime
  honours it (it may require `XDG_CONFIG_HOME` instead).
