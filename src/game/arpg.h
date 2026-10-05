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

// Called once per frame from the main game loop.
void arpg_tick();

// Replacement for entering turn-based combat (`combat()`).
void arpg_request_combat(STRUCT_664980* attack);

// Player intent.
void arpg_player_attack(Object* target);
void arpg_player_clear_target();
bool arpg_is_hostile(Object* critter);
bool arpg_is_attackable(Object* critter);

bool arpg_is_engaged(Object* critter);

// Called when a critter is removed from the map.
void arpg_on_critter_removed(Object* critter);

// Hotkeys.
void arpg_quick_heal();
void arpg_quick_reload();

// Test harness: `[arpg]autotest=1` skips menus and starts a new game with the
// combat premade character.
bool arpg_autotest();
void arpg_debug_warp_to_enemy();

} // namespace fallout

#endif /* FALLOUT_GAME_ARPG_H_ */
