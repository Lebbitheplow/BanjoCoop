#!/usr/bin/env python3
"""Prepare the second game instance.

The runtime picks its config directory from HOME, so a second player is just a second HOME. This
seeds that directory from player 1's (ROM, graphics, sound) so there is no first-run prompt, and
then *forces* the BanjoCoop settings that make it a client.

Forcing rather than seeding matters. The settings are copied from player 1, who is the host, so a
plain copy produces a second host that silently never connects — and the runtime rewrites this
file whenever the mod's config options change, which reintroduces the copied values. Every other
key the player has set is preserved.

Usage: setup_instance2.py <p1-config-dir> <p2-config-dir>
"""

import json
import shutil
import sys
from pathlib import Path

# Copied from player 1 only when absent, so the player's own choices are never clobbered.
SEED_FILES = [
    "bk.n64.us.1.0.z64",  # the imported ROM, the slow one to redo
    "general.json",
    "graphics.json",
    "sound.json",
    "controls.json",
]

# Non-negotiable for the second instance. Anything else in the file is left alone.
FORCED = {
    "netmode": "Join",
    "player_name": "player2",
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    p1, p2 = Path(sys.argv[1]), Path(sys.argv[2])
    (p2 / "mods").mkdir(parents=True, exist_ok=True)
    (p2 / "mod_config").mkdir(parents=True, exist_ok=True)
    (p2 / "saves").mkdir(parents=True, exist_ok=True)

    for name in SEED_FILES:
        src, dst = p1 / name, p2 / name
        if src.exists() and not dst.exists():
            shutil.copy2(src, dst)
            print(f"  seeded {name}")

    cfg_path = p2 / "mod_config" / "banjocoop.json"
    cfg = {}
    if cfg_path.exists():
        try:
            cfg = json.loads(cfg_path.read_text())
        except json.JSONDecodeError:
            print("  mod config was unreadable; rewriting it")
            cfg = {}
    elif (p1 / "mod_config" / "banjocoop.json").exists():
        # Take player 1's as a starting point purely to inherit address/port, then override below.
        cfg = json.loads((p1 / "mod_config" / "banjocoop.json").read_text())

    storage = cfg.setdefault("storage", {})
    changed = [k for k, v in FORCED.items() if storage.get(k) != v]
    storage.update(FORCED)
    cfg.setdefault("mod_id", "banjocoop")

    cfg_path.write_text(json.dumps(cfg, indent=4))
    if changed:
        print(f"  forced client settings: {', '.join(changed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
