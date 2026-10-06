#ifndef FALLOUT_GAME_ARPG_H_
#define FALLOUT_GAME_ARPG_H_

#include "game/combat_defs.h"
#include "game/object_types.h"
#include "plib/gnw/rect.h"

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
// poe/wasd (unless `rmb_toggles_mode=1`): the right button never switches the
// cursor mode, F does.
bool arpg_rmb_is_skill();
// Right-click special at the enemy nearest the cursor, else the current
// target; "No target" otherwise.
void arpg_rmb_special(bool inPlace);
int arpg_energy();
int arpg_energy_max();

// HUD (arpg_hud.cc): HP / energy globes and Alt item labels.
void arpg_hud_tick(unsigned int now);
void arpg_hud_reset();
void arpg_hud_toggle_minimap();
// True while Alt / Z loot highlights are shown (gates OUTLINE_TYPE_ARPG_LOOT).
bool arpg_loot_highlights_visible();
// Hotkey bar: flashes the slot bound to `key` (called for every game key).
void arpg_hud_key_pressed(int key);
int arpg_dodge_cost();
// Energy costs of the Q/E/T aimed shots and the right-click special with the
// active hit mode; `burst` = the special is a burst, `canAim` = Q/E/T work.
bool arpg_skill_costs(int* aimCost, int* specialCost, bool* burst, bool* canAim);

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

// Magic item affixes, stored in object flag bits vanilla never uses (so they
// are saved with the item): prefix index in 0x00F00000, suffix index in
// 0x000003C0. 0 = none.
#define ARPG_ITEM_PREFIX_SHIFT 20
#define ARPG_ITEM_PREFIX_MASK 0x00F00000
#define ARPG_ITEM_SUFFIX_SHIFT 6
#define ARPG_ITEM_SUFFIX_MASK 0x000003C0
#define ARPG_ITEM_AFFIX_MASK (ARPG_ITEM_PREFIX_MASK | ARPG_ITEM_SUFFIX_MASK)

// Magic items: 0 normal, 1 magic (blue), 2 rare (yellow), 3 unique (gold).
int arpg_item_quality(Object* item);
// item_name() hook: the affixed (or unique) name of an item, else NULL.
char* arpg_item_name(Object* item);
// item_inv_fid() hook: a unique item's own inventory art, or -1.
int arpg_item_inv_fid(Object* item);
// Registers unique item art (called right after art_init).
void arpg_art_init();
// Examine text: writes line `index` of what the affixes do; false past the
// last line.
bool arpg_item_affix_text(Object* item, int index, char* out, int size);

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

// Weapon swap (intface_toggle_items): sets the dude's weapon art to the active
// hand without the put-away/take-out sequence, keeping the current walk/run.
// Deferred to arpg_tick while another animation (an attack) is playing.
void arpg_sync_dude_weapon_fid();

// Map zoom (mouse wheel). The map is rendered into a buffer of the "render"
// size (view size / zoom) and scaled into the display window, so tile_coord /
// tile_num / object picking work in render ("world buffer") pixels while the
// mouse and the HUD work in screen pixels.
// The render buffer is at most this many times the view (zoom 50%).
constexpr int ARPG_ZOOM_MAX_RENDER_SCALE = 2;
void arpg_zoom_init(unsigned char* displayBuffer, int width, int height);
void arpg_zoom_exit();
// +1 zooms in one step, -1 out. Refreshes the map.
void arpg_zoom_step(int delta);
void arpg_zoom_set_pct(int pct);
int arpg_zoom_pct();
bool arpg_zoom_active();
// Tile renderer blit callback while zoomed: scales the render-buffer rect
// into the display window and draws it.
void arpg_zoom_blit(Rect* rect);
// Screen (display window) <-> render buffer pixels. Identity at 100%.
void arpg_zoom_screen_to_world(int* x, int* y);
void arpg_zoom_world_to_screen(int* x, int* y);
// mouse_get_position converted to render buffer pixels.
void arpg_mouse_world_position(int* x, int* y);
// Size of the render buffer (what tile_coord coordinates span).
void arpg_zoom_view_size(int* width, int* height);

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
