# Fallout ARPG (Diablo-style Fallout 1)

A fork of [Fallout Community Edition](https://github.com/alexbatalov/fallout1-ce) (branch `arpg`)
that replaces turn-based combat with real-time action-RPG combat. Bring your own Fallout 1;
no game files are included.

## Install
1. Build (see below) or take `fallout-ce.exe`, and copy it into your Fallout folder as `fallout-arpg.exe`.
2. Run it. ARPG mode is on by default. Your saves and `falloutw.exe` are untouched.
   Set `[arpg] enabled=0` in `fallout.cfg` to get vanilla turn-based CE behaviour.

## How it plays
Three control schemes. Set `[arpg] controls=poe|wasd|classic` (default `poe`), or press **Ctrl+W** in
game to cycle them.

**Mouse wheel zooms the map** in every scheme: 50% (see more) to 200% (closer), in steps. Only the map is
scaled: the HUD (globes, hotkey bar, minimap) and the cursor stay sharp, while floating numbers and
loot labels are drawn in the map and scale with it.

### `poe`: Path of Exile click-to-move (default)
| Input | Action |
|---|---|
| Left-click a monster / hostile | Attack (auto-runs into range, keeps attacking until it dies) |
| Left-click ground / hold left button | Run / keep running toward the cursor |
| **Shift** + left-click / hold | Attack in place: never walks; snaps to the enemy nearest the cursor |
| Left-click an item on the ground | Pick it up |
| Right-click an enemy | Special: burst fire if the weapon has it, else aimed head shot (costs Energy). A near miss snaps to the enemy within 2 hexes of the cursor (else your current target); right-click never switches the cursor mode in poe/wasd |
| **F** | Switch the cursor between move/attack and use/talk/look (Fallout's old right-click); right-click also switches back from use. `rmb_toggles_mode=1` restores the vanilla right-click |
| **Q / E / T** | Skill bar: aimed shot at the head / legs / eyes of the enemy near the cursor (costs Energy) |
| R | Reload |
| **1–5** | Flasks: 1 (super) stimpak, 2 RadAway / Rad-X, 3 Buffout, 4 Mentats, 5 Psycho / Nuka-Cola |
| **Space** | Dodge roll toward the cursor, with 0.6 s of invulnerability (20 Energy) |
| **Z** | Toggle loot labels + highlights on/off permanently (Alt still shows them while held). Ground items get a D2-style outline in their tier colour (white normal, blue magic, yellow rare, gold unique, green drugs); `[arpg] loot_highlight=0` turns outlines off |
| **W** | Weapon swap (switch active hand, like Diablo II's W; Fallout's B still works) |
| **V** | Show/hide the minimap |
| **X** | Toggle the red outline around living enemies (anything you can attack on a plain click), so they stand out from corpses. On by default; `[arpg] enemy_outline=0` starts with it off. Works in every control scheme |
| Mouse pointer | In move/attack mode the cursor is a normal pixel-exact pointer instead of the red floor hex: white arrow = move, gold arrow = pick up the item under it, orange reticle = a click attacks (in WASD / Shift it lights up whenever the 2-hex snap would hit an enemy). `[arpg] screen_cursor=0` (or "Pointer cursor" in the options page) brings the hex back |
| Tab | Automap |
| Crosshair mode (right-click empty ground) | Attack anyone, including neutrals |

The flasks replace Fallout's skill hotkeys 1–5 (sneak, lockpick, steal, traps, first aid). Use the
Skilldex (S or the interface button) for those; 6–8 still work. Z no longer opens the Pip-Boy clock;
P still opens the Pip-Boy.

### `wasd`: PoE2-style movement
Everything above, except:
- **W/A/S/D** run in that screen direction, with diagonals. The view follows you. The character slides
  along walls instead of stopping.
- The mouse only aims. Left-click attacks the enemy near the cursor *in place*; clicking the ground does
  nothing (items are still picked up). Space dodges in the WASD direction if one is held.
- A and S no longer start combat or open the Skilldex (use the interface buttons).
- Shooting doesn't stop you: while W/A/S/D is held, a shot plays its raise-and-fire animation facing the
  target while you keep moving at run speed, and the run picks up again the moment the shot is out (no
  lowering of the gun). Pressing a direction in the middle of a shot gets you moving straight away. Walls and
  monsters in the way stop the slide a quarter hex short, or you go round them like a run would.

### `classic`: first version
Q drinks a stimpak, Shift walks, 1–8 are skill hotkeys, Z is the Pip-Boy clock; no Q/E/T, flasks or dodge.

- Real-time AI: enemies chase and attack using Fallout's own targeting (`ai_danger_source`), teammates join, scripts still trigger fights.
- Attack speed = weapon AP cost ÷ max AP (Agility matters). Fallout's to-hit, crits, armour and damage are unchanged.

### Diablo II feel
- **Run and gun**: attacks play at Diablo speed (your attacks 2x, raising/lowering a gun 3x, enemy attacks 1.5x,
  hit recovery 1.6x, running 1.2x, projectiles 1.5x). Your bullets, lasers and rockets fly on their own, so you
  never wait for one to land before moving or shooting again. With WASD you keep moving while you shoot (see
  above); with click-to-move an attack still stops you.
- **Monster packs**: every wild monster (rats, radscorpions, mantises, floaters, centaurs, deathclaws) leads a pack
  of 2-4 more. Packs attack on sight and the whole pack comes when one of them is shot. Packs aren't saved: they
  come back when you re-enter a map after 12 game hours, or when you load a game.
- **Raider packs**: on random encounters, when people turn on you (raiders, bandits, mutant patrols) each one
  calls in one or two armed copies, sometimes a champion pack or a named boss like "Dirty Tibbs of the Wastes"
  with minions. Friendly encounters (caravans, traders) never grow. `raider_packs=0` turns this off.
- **Champions** (blue name, faint glow): Champion, Fanatic (very fast), Berserker (red, hits hard, takes more),
  Ghostly (see-through, slow, drains energy), Possessed (lots of life). 3x XP, more loot.
- **Uniques** (gold name like "Rustfang the Rabid", bright glow) with minions and two or three modifiers:
  Extra Strong, Extra Fast, Armored, Fire Enchanted (explodes when it dies — step back), Radioactive (hits give
  rads), Energy Drain, Vampiric, Phasing (teleports to you), Pack Leader (minions hit harder), Venomous.
  5x XP, four loot picks and always a piece of gear.
- **HUD**: the monster under the cursor (or your target) is shown at the top with its name, modifiers and life;
  flask counts sit next to the LIFE globe; a gold XP bar runs along the top of the interface.
- **Minimap** (top right, **V** toggles, `minimap=0` turns it off): walls, loot (white), monsters (red),
  champions (blue), uniques (gold), shrines (cyan), party (green) and you (green cross).
- **Shrines**: monster levels have one or two glowing burning barrels. Walk up to one to use it: Refilling (full
  life and energy), Armor (half damage taken, 96 s), Combat (double damage, 96 s), Energy (fast energy, 96 s),
  Experience (+50% XP, 144 s), Purifying (no radiation or poison, 144 s) or Monster (the nearest monster becomes
  a unique). Active boosts count down next to the ENERGY globe. `shrines=0` turns them off.
- **Magic items**: dropped weapons and armor can be magic (blue name, one affix) or rare (yellow, two),
  e.g. "Vicious Assault Rifle of Haste". Weapon affixes: Vicious (+25% damage), Brutal (+50%), Burning (+2-6),
  Shocking (+3-8), Vampiric (heal 10% of damage), Deadly (15% double damage), of Speed (+25% attack speed),
  of Haste (+50%), of the Leech (heal 15%), of Slaying (+40% vs champions and uniques), of Fortune (more and
  better loot), of Power (+15%). Armor: Sturdy (-10% damage taken), Reinforced (-20%), Regenerating (+1 HP every
  4 s), Lead-Lined (sheds radiation), of Thorns (melee attackers take 30% back), of Vigor (+50% energy regen),
  of Evasion (10% of hits miss), of Fortune. Champions and uniques drop better items; a unique's guaranteed
  piece of gear is always at least magic. Magic items sell for 1.5x, rare for 2.5x, and never stack with
  plain copies. The affixes are saved with the item. Grenades and other thrown weapons stay plain.
- **Unique items** (gold): named pieces with fixed affixes, mostly from unique monsters: Wastelander's Friend
  (10mm Pistol), Junktown Justice (Shotgun), The Ripper's Kiss (Ripper), Overseer's Mercy (Hunting Rifle), Hub
  Peacemaker (14mm Pistol), Old Faithful (Assault Rifle), Sunburst (Laser Pistol), Brotherhood's Wrath (Power
  Fist), Glow Torch (Flamer), Deathclaw Bane (Combat Shotgun), Vault Jacket of Ages (Leather Jacket), Scorpion
  Shell (Metal Armor), Lead Mantle (Leather Armor), Bulwark of the Hub (Combat Armor). Examine an item to see
  what its affixes do. Uniques sell for 4x.
- **Diablo II homage uniques** (our own names, D2-style powers): Dust Devil (Sniper Rifle; Windforce), The
  Granddaddy (Super Sledge; The Grandfather), Neon Saber (Ripper; Lightsabre), Stormwhip (Cattle Prod;
  Stormlash), Doomsayer (Sledgehammer; Doombringer), Toll Collector (Spear; The Reaper's Toll), Hellraiser
  (Super Sledge; Hellslayer), Skullbuster (Sledgehammer; The Cranium Basher), Nuka Frost (Combat Knife;
  Frostwind), Rad Needle (Spear; Arioc's Needle), Eagle Eye (Hunting Rifle; Eaglehorn), Frostbite Kyanon (Laser
  Rifle; Buriza-Do Kyanon), Demon Repeater (10mm SMG; Demon Machine), Starfall (Club; Baranar's Star), Reaver's
  Bite (Crowbar; Messerschmidt's Reaver), Brainstorm Shiv (Knife; Wizardspike). Unique-only powers:
  **knockback** (2 hexes; big multi-hex critters like radscorpions don't budge), **crushing blow** (20%: a
  quarter of the target's current life, an eighth for champions/uniques), **chill** (+3-9 cold damage, the
  target moves and attacks at half speed for 3 s).
- **D2 item art (optional, local only)**: with a Diablo II install next to the workspace, run
  `python tools/d2art/d2art.py` from the workspace root (the tool lives in the workspace next to
  `tools/d2data`, not in this repo). It pulls each homage unique's inventory picture out of
  your own D2 MPQs and writes Fallout FRMs to `Fallout/DATA/art/inven` and `Fallout/DATA/art/items` (ground art
  is the same picture laid on the floor). The game uses them for the inventory, the hand slot and the ground;
  without them the base Fallout item's art is used. These files are never committed or shipped.
- **Caps**, **flask consumables** (stimpaks, super stimpaks, RadAway, Rad-X, Buffout, Mentats, Psycho,
  Nuka-Cola) and **all ammo** are picked up just by walking past them (within 2 hexes). Consumables and ammo
  are skipped while you're at your carry-weight limit. `[arpg] auto_caps=0` / `auto_consumables=0` /
  `auto_ammo=0` turn each off.
- **Corpses drop their inventory**: when an NPC or monster dies, everything it carried (wielded weapon, worn
  armor, ammo, caps, drugs, other items) spills onto the tiles around the body, like D2 drops. Up to 10 units
  of each stack spill; anything beyond that stays on the corpse. Party members never drop. `[arpg]
  inventory_drops=0` (or ARPG OPTIONS -> Corpses drop inventory) keeps Fallout's loot-the-body behaviour.
- **Death** works like Diablo II: a killing blow from a monster drops you for three seconds, then you rise again at
  the map entrance with full life, losing 10% of the caps you carry. Monsters forget you while you're down.
  (Deaths outside combat, such as poison or scripted deaths, still end the game.) Set `respawn=0` for
  Fallout's permadeath.
- Hit recovery like Diablo: only big hits (≥8% max HP for monsters, ≥15% for you), crits and knockdowns stagger, with a 2 s immunity.
- LIFE and ENERGY globes sit in the hi-res side areas (needs width ≥ 840, e.g. 1024x768 via `f1_res.ini`).
- **Hotkey bar** (D2 belt + skill bar) above the XP bar: one keycap per key of the current control scheme.
  poe/wasd: flasks 1–5 (item art + count), Q/E/T aimed shots (a figure with the target body part), Space dodge,
  RMB special (your weapon), R reload (ammo icon + rounds loaded) and F cursor mode (Move / Use); classic: Q heal and R reload. Energy costs
  are blue, red when you're short; a slot is checkered dark when it can't be used (no flasks, not enough
  energy, a weapon that can't aim, magazine full). The slot flashes when you press its key, hovering shows what
  it does, and clicking a flask drinks it. `hotkey_bar=0` hides it (the old text belt next to the life globe
  comes back).
- Kills drop caps, stimpaks, ammo for your weapon and gear scaled by the monster's XP. XP is awarded on kill.

## ARPG options page (ESC menu)
ESC → **ARPG OPTIONS** (or press A in the Options menu) lists every on/off switch the mod added: ARPG mode,
control scheme, always run, hold to move, right click behaviour, shoot on the move, respawn, globes/XP bar,
hotkey bar, minimap, damage numbers, pointer cursor, enemy outlines, smooth zoom, loot drops, loot amount (50-500%), loot
labels and outlines, auto-pickup of caps / drugs / ammo, monster packs, shrines and raider reinforcements.
Click a row to change it; hovering shows what it does. Changes apply at once (packs and shrines from the next
area, ARPG mode after a restart) and are saved to the `[arpg]` keys below when the page closes.

## Config (`[arpg]` in fallout.cfg)
`enabled`, `controls` (poe/wasd/classic), `dodge_cost` (20), `always_run`, `hold_to_move`, `rmb_toggles_mode` (0), `damage_numbers`, `globes`, `loot_drops`, `inventory_drops` (1), `loot_pct`,
`energy_regen_pct`, `enemy_attack_delay_pct` (130), `player_attack_delay_pct` (100), `stagger_pct`,
`player_stagger_pct`, `stagger_immunity_ms`, `leash_distance`, `labels_always`, `loot_highlight` (1), `enemy_outline` (1), `screen_cursor` (1), `auto_caps`, `auto_consumables` (1), `auto_ammo` (1), `respawn` (1),
`respawn_caps_pct` (10).

Animation speeds (percent): `attack_anim_pct` (200), `aim_anim_pct` (300), `enemy_attack_anim_pct` (150),
`reaction_anim_pct` (160), `run_speed_pct` (120), `enemy_run_speed_pct` (115), `projectile_speed_pct` (150).
`move_shot` (1): shoot on the move with WASD and launch your projectiles on their own; 0 = the older behaviour
(a shot stops you until the bullet lands and the gun is lowered).

Packs: `packs` (1), `pack_min` (2), `pack_max` (4), `pack_density_pct` (100), `champion_pct` (10),
`unique_pct` (7), `pack_respawn_hours` (12), `max_spawns` per level (60), `aggro_radius` (9).
HUD and extras: `minimap` (1), `hotkey_bar` (1), `shrines` (1), `shrine_scenery` (scenery proto number, 1 = Burning Metal Barrel),
`raider_packs` (1), `party_damage_taken_pct` (70: followers take 30% less damage, like D2 mercenaries).
Zoom: `zoom` (100, start zoom in percent, 50-200), `zoom_smooth` (1; 0 = nearest-neighbour pixels).
Camera: `camera_center` (0; 1 = the view stays centered on the player, no screen-edge scrolling; ARPG OPTIONS ->
Camera locked on player), `camera_look` (1; with the camera locked, standing still lets the cursor pull the view up to
a quarter screen toward it; moving eases it back onto the player).

## Test harness (developers)
```
fallout-arpg.exe [arpg]autotest=1 [arpg]autoplay=1 [arpg]autotest_seconds=90 [arpg]autoshot_ms=2000
                 [arpg]autotest_map=CAVES.MAP [arpg]autotest_weapon=23 [arpg]autotest_ammo=35
                 [arpg]autotest_armor=17 [arpg]autotest_xp=30000
```
`[arpg]autotest_sweep=1` walks every map from index 1 through real map transitions (and every non-empty
elevation), logging `SWEEP` lines with monster/pack/engagement counts; combine with `autoplay=0`.
`[arpg]autotest_god=1` keeps the bot above half life, `[arpg]autotest_saveload=N` saves to slot 10 after N
seconds (with a knocked-out pack monster) and loads it back 3 s later.
`[arpg]autotest_iframes=1` keeps dodge i-frames on. `[arpg]autotest_wasd=0..3` holds W/D/S/A (3 s on, 1 s off) through the real WASD movement path;
`[arpg]autotest_rmb=1` presses F twice, then right-clicks ~1 hex off the nearest enemy every 3 s (logs the snap and mode).
`[arpg]autotest_zoom=1` steps the zoom up to 200%, down to 50% and back (3.5 s each), puts the mouse on the
player each time and logs whether it maps back to the player's tile (`OK`/`MISMATCH`).
`[arpg]autotest_options=N` opens ESC → ARPG OPTIONS after N s with real SDL keys, presses a few rows and logs
the values (`options ... now ...`), with screenshots of both pages.
`[arpg]autotest_camera_look=1` parks the cursor near the right edge (camera_look); the camera logs
`camera dude off X,Y target off X,Y moving=N` once a second while `camera_center` is on.
`[arpg]autotest_cursor=1` (with `-NoBot`) parks the mouse on floor, on the nearest enemy (pulled next to the
player), on floor in the use mode and back (F), then on a dropped 10mm pistol, logging `cursor step N mode M variant V` (1 move, 2 loot,
3 attack, 0 gmouse's own cursor) with a screenshot each.
`[arpg]autotest_hover=1` walks the hotkey-bar tooltip across the slots (1.5 s each) for screenshots.
`[arpg]dump_items=1` logs every item PID. Test keys are reset in fallout.cfg on exit, but other
overrides like `controls` or `dodge_cost` are saved.
Starts a new game with the combat premade, plays with a bot, writes `arpg.log`, screenshots, and a
symbolized stack to `arpg_crash.log` on a crash. F11 warps next to the nearest critter in autotest.

## Build
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## Diablo II reference
Design numbers (champion/unique life, XP and modifier list, pack sizes) come from Diablo II's own data tables
(`MonUMod.txt`, `MonStats.txt`, ...), read from a local D2 install with a small extractor built on
[AbyssEngine](https://github.com/AbyssEngine/AbyssEngine)'s MIT-licensed MPQ reader. AbyssEngine (a clean-room
D2 engine in C) is a reference and a source we may copy code from (keep its MIT notice). No Diablo II data or
assets are included here, and no D2 names are used in game (D2 originals are named only as credits for the
homage uniques). The optional D2 item art is generated on the player's machine from their own D2 install
(`tools/d2art` in the workspace).

## Code map
- `src/game/arpg.cc` – real-time combat, AI tick, energy/specials, loot, hold-to-move, control schemes
  (`arpg_handle_key`, WASD tick, dodge, flasks), autotest.
- `src/game/arpg_zoom.cc` – map zoom: render buffer of view size / zoom, scaled into the display window
  (`map_display_draw`); `tile_set_buffer` / `obj_set_buffer` / `text_object_set_buffer` retarget the renderer.
- `src/game/arpg_cursor.cc` – the move/attack pointer (built at runtime, set with mouse_set_shape while gmouse
  sits on MOUSE_CURSOR_NONE); object.cc skips drawing the floor hex while it is on.
- `src/game/arpg_options.cc` – the ARPG options page (table of settings); `options.cc` adds its button.
- `src/game/arpg_hud.cc` – globes, hotkey bar, target bar, XP bar, minimap and Alt labels. `src/game/arpg_crash.cc` – autotest crash reporter.
- Hooks: `game_handle_input` → `arpg_handle_key` before Fallout's key switch, `combat()` / `combat_attack_this()` redirect, `main_game_loop` tick, `gmouse` click handling,
  `script_chk_critters` skips critter_p_proc for fighting critters, `obj_remove_all` resets state,
  `register_clear_force` in anim.cc.

Built with AI assistance (Claude). Fallout CE is Sustainable Use License; see LICENSE.md.
