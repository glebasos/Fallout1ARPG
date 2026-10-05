# Fallout ARPG (Diablo-style Fallout 1)

A fork of [Fallout Community Edition](https://github.com/alexbatalov/fallout1-ce) (branch `arpg`)
that replaces turn-based combat with real-time action-RPG combat. Bring your own Fallout 1;
no game files are included.

## Install
1. Build (see below) or take `fallout-ce.exe`, and copy it into your Fallout folder as `fallout-arpg.exe`.
2. Run it. ARPG mode is on by default. Your saves and `falloutw.exe` are untouched.
   Set `[arpg] enabled=0` in `fallout.cfg` to get vanilla turn-based CE behaviour.

## How it plays
| Input | Action |
|---|---|
| Left-click a monster / hostile | Attack (auto-runs into range, keeps attacking until it dies) |
| Left-click ground / hold left button | Run / keep running toward the cursor |
| Left-click an item on the ground | Pick it up |
| Right-click an enemy | Special attack: burst fire if the weapon has it, else aimed head shot (costs Energy) |
| Alt (hold) | Show item labels: white = normal, blue = value ≥1000, gold = value ≥3000 |
| Q | Drink a (super) stimpak |
| R | Reload |
| Crosshair mode (right-click empty ground) | Attack anyone, including neutrals |

- Real-time AI: enemies chase and attack using Fallout's own targeting (`ai_danger_source`), teammates join, scripts still trigger fights.
- Attack speed = weapon AP cost ÷ max AP (Agility matters). Fallout's to-hit, crits, armour and damage are unchanged.
- Hit recovery like Diablo: only big hits (≥8% max HP for monsters, ≥15% for you), crits and knockdowns stagger, with a 2 s immunity.
- LIFE and ENERGY globes sit in the hi-res side areas (needs width ≥ 840, e.g. 1024x768 via `f1_res.ini`).
- Kills drop caps, stimpaks, ammo for your weapon and gear scaled by the monster's XP. XP is awarded on kill.

## Config (`[arpg]` in fallout.cfg)
`enabled`, `always_run`, `hold_to_move`, `damage_numbers`, `globes`, `loot_drops`, `loot_pct`,
`energy_regen_pct`, `enemy_attack_delay_pct` (130), `player_attack_delay_pct` (100), `stagger_pct`,
`player_stagger_pct`, `stagger_immunity_ms`, `leash_distance`, `labels_always`.

## Test harness (developers)
```
fallout-arpg.exe [arpg]autotest=1 [arpg]autoplay=1 [arpg]autotest_seconds=90 [arpg]autoshot_ms=2000
                 [arpg]autotest_map=CAVES.MAP [arpg]autotest_weapon=23 [arpg]autotest_ammo=35
                 [arpg]autotest_armor=17 [arpg]autotest_xp=30000
```
Starts a new game with the combat premade, plays with a bot, writes `arpg.log`, screenshots, and a
symbolized stack to `arpg_crash.log` on a crash. F11 warps next to the nearest critter in autotest.

## Build
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## Code map
- `src/game/arpg.cc` – real-time combat, AI tick, energy/specials, loot, hold-to-move, autotest.
- `src/game/arpg_hud.cc` – globes and Alt labels. `src/game/arpg_crash.cc` – autotest crash reporter.
- Hooks: `combat()` / `combat_attack_this()` redirect, `main_game_loop` tick, `gmouse` click handling,
  `script_chk_critters` skips critter_p_proc for fighting critters, `obj_remove_all` resets state,
  `register_clear_force` in anim.cc.

Built with AI assistance (Claude). Fallout CE is Sustainable Use License; see LICENSE.md.
