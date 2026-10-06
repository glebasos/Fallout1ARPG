#ifndef FALLOUT_GAME_ARPG_H_
#define FALLOUT_GAME_ARPG_H_

#include "game/combat_defs.h"
#include "game/object_types.h"

namespace fallout {

// Real-time "action RPG" combat layer. When enabled (`[arpg] enabled=1` in
// fallout.cfg, or `[arpg]enabled=1` on the command line) turn-based combat
// never starts: hostile critters chase and attack in real time and the player
// attacks by clicking enemies.

void arpg_init();
void arpg_reset();
bool arpg_enabled();
bool arpg_always_run();
bool arpg_labels_always();

// Called once per frame from the main game loop.
void arpg_tick();

// Replacement for entering turn-based combat (`combat()`).
void arpg_request_combat(STRUCT_664980* attack);

// Control schemes (`[arpg] controls=classic|poe|wasd`, Ctrl+W cycles).
//   classic: Diablo-ish first version (Q heal, R reload, Shift walks).
//   poe:     Path of Exile click-to-move: 1-5 flasks, Q/E/T aimed shots,
//            Shift attacks in place, Z toggles loot labels, Space dodges.
//   wasd:    poe + PoE2 WASD movement; the mouse only aims, attacks never move.
enum {
    ARPG_CONTROLS_CLASSIC,
    ARPG_CONTROLS_POE,
    ARPG_CONTROLS_WASD,
};

int arpg_controls();

// True when a left click must not move the player (WASD scheme, or Shift
// held in the poe scheme).
bool arpg_attack_in_place();

// Returns true if the key was consumed by the ARPG control scheme.
bool arpg_handle_key(int key);

// Player intent.
void arpg_player_attack(Object* target, bool inPlace = false);
void arpg_player_clear_target();
Object* arpg_player_target();
void arpg_player_special(Object* target, bool inPlace = false);
Object* arpg_target_near_cursor();
int arpg_energy();
int arpg_energy_max();

// HUD (arpg_hud.cc): HP / energy globes and Alt item labels.
void arpg_hud_tick(unsigned int now);
void arpg_hud_reset();
void arpg_hud_toggle_minimap();

// Shrine boosts for the HUD: seconds left on boost `index` (0 when inactive,
// -1 past the end) and its short name.
#define ARPG_BUFF_COUNT 7
int arpg_buff_seconds(int index, const char** name);
// -1 not a shrine, 0 used shrine, 1 unused shrine.
int arpg_shrine_state(Object* obj);
bool arpg_is_hostile(Object* critter);
bool arpg_is_attackable(Object* critter);

bool arpg_is_engaged(Object* critter);

// Called when a critter is removed from the map.
void arpg_on_critter_removed(Object* critter);

// Percent of normal animation speed for `fid` played on `obj` (compute_tpf).
int arpg_anim_speed_pct(Object* obj, int fid);

// Object flag bit (unused by vanilla) marking monsters spawned by the pack
// system. Those also carry OBJECT_NO_SAVE, so the bit never reaches disk.
#define ARPG_OBJECT_SPAWNED 0x08000000

// True for monsters spawned by the pack system (never saved).
bool arpg_is_spawned(Object* obj);

// Diablo-style monster rank for the HUD: 0 normal, 1 minion, 2 champion,
// 3 unique. Fills the display name and the modifier list ("Champion",
// "Extra Fast, Fire Enchanted"). Returns false for ordinary critters.
bool arpg_monster_info(Object* critter, int* rank, char* name, int nameSize, char* mods, int modsSize);

// A saved game is being loaded (packs aren't saved, so they respawn).
void arpg_on_game_load();

// The world map is loading a random encounter or wilderness map: those are
// fresh every time, so the next map's packs ignore the respawn timer.
void arpg_note_random_map();

// Autotest save/load without the UI (loadsave.cc).
int arpg_ls_save_slot(int slot, const char* description);
int arpg_ls_load_slot(int slot);

// Hotkeys.
void arpg_quick_heal();
void arpg_quick_reload();

// Test harness: `[arpg]autotest=1` skips menus and starts a new game with the
// combat premade character.
bool arpg_autotest();
// True while the autotest map sweep runs (dialogues are skipped).
bool arpg_autotest_sweeping();
void arpg_install_crash_handler();
char* arpg_autotest_map();
void arpg_debug_warp_to_enemy();

} // namespace fallout

#endif /* FALLOUT_GAME_ARPG_H_ */
