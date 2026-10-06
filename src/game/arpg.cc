#include "game/arpg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <math.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/actions.h"
#include "game/anim.h"
#include "game/art.h"
#include "game/combat.h"
#include "game/combatai.h"
#include "game/critter.h"
#include "game/display.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/gsound.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/perk.h"
#include "game/proto.h"
#include "game/queue.h"
#include "game/protinst.h"
#include "game/roll.h"
#include "game/scripts.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "platform_compat.h"
#include "plib/color/color.h"
#include "plib/gnw/debug.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

namespace fallout {

#define ARPG_CONFIG_SECTION "arpg"

// An attack in flight. Owned jointly by the attacker's animation sequence
// (released by a forced callback when the sequence ends) and by the pending
// hit queue (released after the hit is resolved).
typedef struct ArpgAttack {
    Attack attack;
    // The critter that was aimed at; a miss can move attack.defender to
    // whatever the stray bullet hits.
    Object* target;
    int anim;
    int refs;
    bool fired;
    bool hitQueued;
    bool invalid;
    // The bullet flies in a sequence of its own, started by arpg_tick once
    // the shot is fired (arpg_launch_projectile).
    bool detached;
    bool launchPending;
} ArpgAttack;

typedef struct ArpgActor {
    unsigned int nextAttack;
    unsigned int nextThink;
    unsigned int nextRepath;
    unsigned int knockdownTime;
    Object* target;
    bool triedWeapon;
    bool pathFailed;
} ArpgActor;

// Monster ranks and modifiers, after Diablo II's MonUMod.txt (champion,
// unique and minion bonuses use its Normal-difficulty constants: minions
// +100% life, champions +200% life and 3x XP, uniques +300% life and 5x XP).
enum {
    RANK_NORMAL,
    RANK_MINION,
    RANK_CHAMPION,
    RANK_UNIQUE,
};

enum {
    // Unique modifiers (Fallout flavoured).
    AFFIX_STRONG = 0x0001, // Extra Strong
    AFFIX_FAST = 0x0002, // Extra Fast
    AFFIX_ARMORED = 0x0004, // Stone Skin
    AFFIX_FIRE = 0x0008, // Fire Enchanted: burns on hit, explodes on death
    AFFIX_RADIOACTIVE = 0x0010, // Cursed: hits irradiate
    AFFIX_DRAIN = 0x0020, // Mana Burn: hits drain energy
    AFFIX_VAMPIRIC = 0x0040, // life steal
    AFFIX_PHASING = 0x0080, // Teleport
    AFFIX_LEADER = 0x0100, // Might aura: nearby pack members hit harder
    AFFIX_VENOM = 0x0200, // hits poison
    // Champion kinds.
    CHAMP_CHAMPION = 0x1000,
    CHAMP_FANATIC = 0x2000,
    CHAMP_BERSERKER = 0x4000,
    CHAMP_GHOSTLY = 0x8000,
    CHAMP_POSSESSED = 0x10000,
};

static const unsigned int unique_affixes[] = {
    AFFIX_STRONG,
    AFFIX_FAST,
    AFFIX_ARMORED,
    AFFIX_FIRE,
    AFFIX_RADIOACTIVE,
    AFFIX_DRAIN,
    AFFIX_VAMPIRIC,
    AFFIX_PHASING,
    AFFIX_LEADER,
    AFFIX_VENOM,
};

static const unsigned int champion_kinds[] = {
    CHAMP_CHAMPION,
    CHAMP_CHAMPION,
    CHAMP_FANATIC,
    CHAMP_BERSERKER,
    CHAMP_GHOSTLY,
    CHAMP_POSSESSED,
};

static const struct {
    unsigned int flag;
    const char* name;
} affix_names[] = {
    { CHAMP_CHAMPION, "Champion" },
    { CHAMP_FANATIC, "Fanatic" },
    { CHAMP_BERSERKER, "Berserker" },
    { CHAMP_GHOSTLY, "Ghostly" },
    { CHAMP_POSSESSED, "Possessed" },
    { AFFIX_STRONG, "Extra Strong" },
    { AFFIX_FAST, "Extra Fast" },
    { AFFIX_ARMORED, "Armored" },
    { AFFIX_FIRE, "Fire Enchanted" },
    { AFFIX_RADIOACTIVE, "Radioactive" },
    { AFFIX_DRAIN, "Energy Drain" },
    { AFFIX_VAMPIRIC, "Vampiric" },
    { AFFIX_PHASING, "Phasing" },
    { AFFIX_LEADER, "Pack Leader" },
    { AFFIX_VENOM, "Venomous" },
};

typedef struct ArpgMonster {
    int rank;
    unsigned int affixes;
    int pack;
    Object* leader;
    // Damage taken is divided by (100 + hpPct) / 100: extra life without
    // touching the shared proto's maximum HP.
    int hpPct;
    int dmgPct;
    int speedPct;
    int xpPct;
    unsigned int nextPhase;
    char name[48];
} ArpgMonster;

static std::unordered_map<Object*, ArpgMonster> monsters;
static std::vector<int> alerted_packs;
static int next_pack_id = 1;
// (map * 4 + elevation) -> game hour packs were last spawned there.
static std::unordered_map<int, int> populated_hour;
// Levels already populated since the map was last loaded.
static std::vector<int> populated_levels;
static bool populate_pending = false;
// The map being loaded is a random encounter / wilderness map (no respawn
// timer), and the map index that applies to.
static bool random_map_pending = false;
static int random_map_index = -1;
// Humans that already called in their raider pack.
static std::vector<Object*> reinforced;
static bool raider_packs = true;
static int party_damage_taken_pct = 70;
static unsigned int populate_time = 0;
static unsigned int next_aggro_check = 0;

static bool packs_enabled = true;
static int pack_min = 2;
static int pack_max = 4;
static int pack_density_pct = 100;
static int champion_pct = 10;
static int unique_pct = 7;
static int pack_respawn_hours = 12;
static int max_spawns = 60;
static int aggro_radius = 9;
static int stat_spawned = 0;
static int stat_clone_scripts = 0;

// Shrines (D2 Shrines.txt: boosters last 96 or 144 s).
enum {
    SHRINE_REFILL,
    SHRINE_ARMOR,
    SHRINE_COMBAT,
    SHRINE_ENERGY,
    SHRINE_EXPERIENCE,
    SHRINE_PURIFY,
    SHRINE_MONSTER,
    SHRINE_COUNT,
};

static const struct {
    const char* name;
    const char* hud;
    int seconds;
} shrine_info[SHRINE_COUNT] = {
    { "Refilling Shrine", "", 0 },
    { "Armor Shrine", "Armor", 96 },
    { "Combat Shrine", "Combat", 96 },
    { "Energy Shrine", "Energy", 96 },
    { "Experience Shrine", "XP", 144 },
    { "Purifying Shrine", "Purity", 144 },
    { "Monster Shrine", "", 0 },
};

typedef struct ArpgShrine {
    Object* obj;
    int kind;
    bool used;
    bool announced;
} ArpgShrine;

static std::vector<ArpgShrine> shrines;
static unsigned int buff_until[SHRINE_COUNT];
static bool shrines_enabled = true;
static int shrine_pid = -1;

static bool arpg_buff(int kind);
static void arpg_spawn_shrines(const std::vector<Object*>& seeds);

static bool arpg_on = false;
static bool arpg_test = false;
static int enemy_delay_pct = 130;
static int player_delay_pct = 100;
static int stagger_pct = 8;
static int player_stagger_pct = 15;
static int stagger_immunity_ms = 2000;
static int autotest_xp = 0;
static std::unordered_map<Object*, unsigned int> stagger_until;
// Chilled critters (UPOW_CHILL) move and attack at half speed until then.
static std::unordered_map<Object*, unsigned int> chill_until;
static int leash_distance = 30;
static bool show_numbers = true;
static bool always_run = true;
static bool labels_always = false;
static bool hold_to_move = true;
static bool loot_drops = true;
static bool auto_caps = true;

// D2 death: a killing blow downs the player, who rises again at the map
// entrance a few seconds later, poorer.
static bool respawn_enabled = true;
static int respawn_caps_pct = 10;
static unsigned int death_until = 0;
static int stat_deaths = 0;
static int loot_pct = 100;
static int energy_regen_pct = 100;

// Animation speeds in percent (Diablo's IAS / FHR / run speed). Applied by
// compute_tpf() through arpg_anim_speed_pct().
static int attack_anim_pct = 200;
static int aim_anim_pct = 300;
static int enemy_attack_anim_pct = 150;
static int reaction_anim_pct = 160;
static int run_speed_pct = 120;
static int enemy_run_speed_pct = 115;
static int projectile_speed_pct = 150;

// Shooting on the move: the player's bullets fly on their own, and a shot
// fired while WASD is held doesn't stop the run - the player keeps sliding
// along at run speed while raising the gun and firing (arpg_slide_tick).
static bool move_shot = true;
static bool slide_active = false;
static unsigned int slide_start = 0;
static unsigned int slide_last = 0;
// Wanted and applied pixel offset of the slide from the current tile.
static double slide_sx = 0.0;
static double slide_sy = 0.0;
static int slide_px = 0;
static int slide_py = 0;
static int slide_hops = 0;
// Hex direction the slide is taking round an obstacle, or -1.
static int slide_around = -1;
static int slide_blocked_ticks = 0;

// Run-and-gun metrics (autotest).
static unsigned int player_target_time = 0;
static int stat_shots_while_wasd = 0;
static int stat_moving_shots = 0;
static int stat_slide_hexes = 0;
static int stat_slide_blocked = 0;
static int stat_run_hexes = 0;
static int stat_launch_fail = 0;
static unsigned int stat_wasd_ms = 0;
static unsigned int stat_run_ms = 0;
static unsigned int stat_slide_ms = 0;
static unsigned int stat_wasd_stalled_ms = 0;
static unsigned int stat_stalled_attack_ms = 0;
static unsigned int stat_stalled_stand_ms = 0;
static int stat_launch_noart = 0;
static int stat_late_slides = 0;
static int stat_dude_fired = 0;
static int stat_dude_resolved = 0;
static int stat_dude_dropped = 0;
static unsigned int stat_latency_total = 0;
static int stat_latency_count = 0;
static int stat_unfired = 0;
static int stat_move_fail = 0;

// Autotest profiling: milliseconds spent per section since the last census.
static double prof_tick_ms = 0;
static double prof_critters_ms = 0;
static double prof_hud_ms = 0;
static double prof_refresh_ms = 0;
// hits, autotest, autoplay, player/hud, aggro.
static double prof_section_ms[5];
// combat_display, react, apply_damage, float text.
static double prof_resolve_ms[4];
static int prof_resolve_count = 0;

static double arpg_now_ms()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct ArpgTickTimer {
    double start = arpg_now_ms();
    ~ArpgTickTimer() { prof_tick_ms += arpg_now_ms() - start; }
};


// Energy (the blue globe) fuels special attacks.
static double energy = -1.0;
static unsigned int last_energy_tick = 0;
static bool player_special = false;
// Aimed shot from the skill bar (Q/E/T): hit location, or -1.
static int player_aim_location = -1;
static bool player_in_place = false;
static unsigned int next_range_warning = 0;

// Control scheme.
static int controls = ARPG_CONTROLS_POE;
static int dodge_cost = 20;
static unsigned int dodge_until = 0;
static int wasd_dx = 0;
static int wasd_dy = 0;
static int wasd_target_tile = -1;
static unsigned int wasd_next_move = 0;
static bool wasd_moving = false;
static int autotest_wasd = -1;
static bool dump_items = false;
static bool autotest_iframes = false;
static bool autotest_god = false;
static int autotest_saveload = 0;
static int autotest_sweep = 0;
static int autotest_die = 0;
static bool autotest_shrines = false;
static bool autotest_gallery = false;
static bool autotest_loot = false;
static bool autotest_swarm = false;
static int autotest_magic = 0;
static bool autotest_encounter = false;
static bool autotest_hostile = false;
static bool autotest_sweep_encounters = false;

static unsigned int next_hold_action = 0;
static int hold_last_tile = -1;

static std::vector<int> loot_table;
static bool autoplay = false;
static unsigned int autoplay_warp_time = 0;
static bool autoplay_warped = false;
static int autotest_seconds = 0;
static int autoshot_ms = 0;
static unsigned int autotest_start = 0;
static unsigned int next_autoshot = 0;
static FILE* arpg_log_file = NULL;
static int autotest_weapon_pid = -1;
static int autotest_ammo_pid = -1;
static int autotest_armor_pid = -1;
static char autotest_map[64] = "";
static bool autotest_armed = false;
static unsigned int autoplay_idle_since = 0;
static int stat_attacks = 0;
static int stat_attack_fail = 0;
static int stat_react_blocked = 0;
static int stat_kills = 0;

static void arpg_log(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    debug_printf("%s", buffer);

    if (arpg_test) {
        if (arpg_log_file == NULL) {
            arpg_log_file = fopen("arpg.log", "wt");
        }
        if (arpg_log_file != NULL) {
            fprintf(arpg_log_file, "[%u] %s", get_time(), buffer);
            fflush(arpg_log_file);
        }
    }
}

static std::unordered_map<Object*, ArpgActor> actors;
static std::vector<ArpgAttack*> pending_hits;
static std::vector<ArpgAttack*> live_attacks;

static Object** crit_list = NULL;
static int crit_count = 0;
static unsigned int next_list_refresh = 0;
static unsigned int alert_until = 0;
static unsigned int next_join_check = 0;
static int last_map = -1;
static int last_elevation = -1;

static Object* player_target = NULL;
static bool player_chasing = false;
static unsigned int player_next_attack = 0;
static unsigned int player_next_repath = 0;
static int player_chase_failures = 0;
static unsigned int dude_knockdown_time = 0;
static unsigned int autotest_knockdown_at = 0;
static bool autotest_knockdown_logged = false;
static Object* player_unreachable = NULL;
static unsigned int player_unreachable_until = 0;

static void arpg_drop_loot(Object* victim, int rolls, bool guaranteedGear);
static void arpg_react(Attack* attack, Object* victim, int damage, int flags, int knockback);
static void arpg_wasd_tick(unsigned int now);
static void arpg_dodge();
static void arpg_player_aimed(Object* target, int hitLocation);

// display_print() modifies its argument, so never pass it a literal.
static void arpg_print(const char* text)
{
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", text);
    display_print(buffer);
}

static void arpg_release(ArpgAttack* aa)
{
    aa->refs -= 1;
    if (aa->refs <= 0) {
        live_attacks.erase(std::remove(live_attacks.begin(), live_attacks.end(), aa), live_attacks.end());
        delete aa;
    }
}

// Animation callbacks. These run inside the animation system, so they only
// record state; the real work happens in arpg_tick().
static int arpg_cb_fired(void* a1, void* a2)
{
    ArpgAttack* aa = (ArpgAttack*)a1;
    if (!aa->fired && aa->attack.attacker == obj_dude) {
        stat_dude_fired++;
    }
    aa->fired = true;
    if (aa->detached && !aa->launchPending) {
        // Keeps the attack alive until arpg_tick launches the bullet, even
        // if the shooter's sequence is cut short right after the shot.
        aa->launchPending = true;
        aa->refs += 1;
    }
    if (aa->attack.attacker == obj_dude && player_target_time != 0) {
        // Click-to-shot latency, for tuning the run-and-gun feel.
        stat_latency_total += get_time() - player_target_time;
        stat_latency_count++;
        player_target_time = 0;
    }
    return 0;
}

static int arpg_cb_hit(void* a1, void* a2)
{
    ArpgAttack* aa = (ArpgAttack*)a1;
    if (aa->fired && !aa->hitQueued) {
        aa->hitQueued = true;
        aa->refs += 1;
        pending_hits.push_back(aa);
    }
    return 0;
}

static int arpg_cb_release(void* a1, void* a2)
{
    ArpgAttack* aa = (ArpgAttack*)a1;
    if (arpg_test && !aa->fired && aa->attack.attacker == obj_dude) {
        stat_unfired++;
        if (stat_unfired <= 20) {
            arpg_log("ARPG: dude attack ended unfired (anim %d, fid anim %d, code %d)\n",
                aa->anim, FID_ANIM_TYPE(obj_dude->fid), (obj_dude->fid & 0xF000) >> 12);
        }
    }
    // A ranged shot that was already fired still lands even if the shooter's
    // sequence was interrupted.
    arpg_cb_hit(aa, NULL);
    arpg_release(aa);
    return 0;
}

// End of the shooter's sequence when the bullet flies on its own: the hit
// comes from the bullet's sequence, so this only lets go of the attack.
static int arpg_cb_shooter_done(void* a1, void* a2)
{
    ArpgAttack* aa = (ArpgAttack*)a1;
    if (arpg_test && !aa->fired && aa->attack.attacker == obj_dude) {
        stat_unfired++;
        if (stat_unfired <= 20) {
            arpg_log("ARPG: dude attack ended unfired (anim %d, fid anim %d, code %d)\n",
                aa->anim, FID_ANIM_TYPE(obj_dude->fid), (obj_dude->fid & 0xF000) >> 12);
        }
    }
    arpg_release(aa);
    return 0;
}

bool arpg_enabled()
{
    return arpg_on;
}

bool arpg_always_run()
{
    return arpg_on && always_run;
}

bool arpg_labels_always()
{
    return labels_always;
}

char* arpg_autotest_map()
{
    return autotest_map[0] != '\0' ? autotest_map : NULL;
}

bool arpg_autotest()
{
    return arpg_test;
}

bool arpg_autotest_sweeping()
{
    return arpg_test && (autotest_sweep > 0 || autotest_hostile);
}

bool arpg_is_spawned(Object* obj)
{
    return obj != NULL && (obj->flags & ARPG_OBJECT_SPAWNED) != 0;
}

static int arpg_monster_speed_pct(Object* critter);
static int arpg_dude_attack_speed_pct();

// Percent of an animation's normal frame rate (compute_tpf hook). Attacks,
// aiming, hit recovery and running are faster, Diablo style; everything else
// (talking, using, dying) keeps Fallout's pace.
int arpg_anim_speed_pct(Object* obj, int fid)
{
    if (!arpg_on || obj == NULL) {
        return 100;
    }

    int type = FID_TYPE(fid);
    if (type == OBJ_TYPE_MISC) {
        // Projectiles (and explosions) advance one step per frame.
        return projectile_speed_pct;
    }

    if (type != OBJ_TYPE_CRITTER) {
        return 100;
    }

    bool dude = obj == obj_dude;
    int pct;
    switch (FID_ANIM_TYPE(fid)) {
    case ANIM_POINT:
    case ANIM_UNPOINT:
        pct = dude ? aim_anim_pct : enemy_attack_anim_pct;
        break;
    case ANIM_THROW_PUNCH:
    case ANIM_KICK_LEG:
    case ANIM_THROW_ANIM:
    case ANIM_THRUST_ANIM:
    case ANIM_SWING_ANIM:
    case ANIM_FIRE_SINGLE:
    case ANIM_FIRE_BURST:
    case ANIM_FIRE_CONTINUOUS:
        pct = dude ? attack_anim_pct * arpg_dude_attack_speed_pct() / 100 : enemy_attack_anim_pct;
        break;
    case ANIM_WALK:
    case ANIM_RUNNING:
        if (dude || isPartyMember(obj)) {
            // Followers keep up with the player.
            pct = run_speed_pct;
        } else if (arpg_is_engaged(obj)) {
            pct = enemy_run_speed_pct;
        } else {
            return 100;
        }
        break;
    case ANIM_HIT_FROM_FRONT:
    case ANIM_HIT_FROM_BACK:
    case ANIM_PRONE_TO_STANDING:
    case ANIM_BACK_TO_STANDING:
    case ANIM_DODGE_ANIM:
        pct = reaction_anim_pct;
        break;
    default:
        return 100;
    }

    if (!dude) {
        pct = pct * arpg_monster_speed_pct(obj) / 100;
    }
    return pct;
}

void arpg_init()
{
    bool enabled = true;
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "enabled", &enabled);
    arpg_on = enabled;

    bool autotest = false;
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest", &autotest);
    arpg_test = autotest;
    if (arpg_test) {
        arpg_install_crash_handler();
    }

    config_get_value(&game_config, ARPG_CONFIG_SECTION, "enemy_attack_delay_pct", &enemy_delay_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "player_attack_delay_pct", &player_delay_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "stagger_pct", &stagger_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "player_stagger_pct", &player_stagger_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "stagger_immunity_ms", &stagger_immunity_ms);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_xp", &autotest_xp);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "leash_distance", &leash_distance);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "damage_numbers", &show_numbers);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autoplay", &autoplay);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_seconds", &autotest_seconds);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autoshot_ms", &autoshot_ms);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_weapon", &autotest_weapon_pid);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_ammo", &autotest_ammo_pid);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_armor", &autotest_armor_pid);
    char* mapName;
    if (config_get_string(&game_config, ARPG_CONFIG_SECTION, "autotest_map", &mapName) && mapName[0] != '\0' && strcmp(mapName, "0") != 0) {
        snprintf(autotest_map, sizeof(autotest_map), "%s", mapName);
    }

    configGetBool(&game_config, ARPG_CONFIG_SECTION, "always_run", &always_run);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "hold_to_move", &hold_to_move);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "labels_always", &labels_always);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "loot_drops", &loot_drops);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "auto_caps", &auto_caps);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "respawn", &respawn_enabled);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "respawn_caps_pct", &respawn_caps_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "loot_pct", &loot_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "energy_regen_pct", &energy_regen_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "dodge_cost", &dodge_cost);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "attack_anim_pct", &attack_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "aim_anim_pct", &aim_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "enemy_attack_anim_pct", &enemy_attack_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "reaction_anim_pct", &reaction_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "run_speed_pct", &run_speed_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "enemy_run_speed_pct", &enemy_run_speed_pct);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "move_shot", &move_shot);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "projectile_speed_pct", &projectile_speed_pct);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "packs", &packs_enabled);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "shrines", &shrines_enabled);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "raider_packs", &raider_packs);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "party_damage_taken_pct", &party_damage_taken_pct);
    // Shrines are drawn with a scenery proto: a Burning Metal Barrel by
    // default (the Altars are huge multi-hex Cathedral pieces).
    int shrineScenery = 1;
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "shrine_scenery", &shrineScenery);
    shrine_pid = (OBJ_TYPE_SCENERY << 24) | shrineScenery;
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "pack_min", &pack_min);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "pack_max", &pack_max);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "pack_density_pct", &pack_density_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "champion_pct", &champion_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "unique_pct", &unique_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "pack_respawn_hours", &pack_respawn_hours);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "max_spawns", &max_spawns);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "aggro_radius", &aggro_radius);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "dump_items", &dump_items);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_iframes", &autotest_iframes);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_god", &autotest_god);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_saveload", &autotest_saveload);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_sweep", &autotest_sweep);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_die", &autotest_die);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_shrines", &autotest_shrines);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_gallery", &autotest_gallery);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_loot", &autotest_loot);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_swarm", &autotest_swarm);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_magic", &autotest_magic);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_encounter", &autotest_encounter);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_hostile", &autotest_hostile);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_sweep_encounters", &autotest_sweep_encounters);
    // Autotest: hold a WASD direction (0=W 1=D 2=S 3=A) through the real
    // movement path.
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "autotest_wasd", &autotest_wasd);

    char* scheme;
    if (config_get_string(&game_config, ARPG_CONFIG_SECTION, "controls", &scheme)) {
        if (compat_stricmp(scheme, "classic") == 0) {
            controls = ARPG_CONTROLS_CLASSIC;
        } else if (compat_stricmp(scheme, "wasd") == 0) {
            controls = ARPG_CONTROLS_WASD;
        } else {
            controls = ARPG_CONTROLS_POE;
        }
    }

    // Command-line test switches are merged into game_config, which CE saves
    // back to fallout.cfg on exit. Neutralize them so they don't stick.
    static const char* testKeys[] = { "autotest", "autoplay", "autotest_seconds", "autoshot_ms", "autotest_weapon", "autotest_ammo", "labels_always", "dump_items", "autotest_armor", "autotest_map", "autotest_xp", "autotest_wasd", "autotest_iframes", "autotest_god", "autotest_saveload", "autotest_sweep", "autotest_die", "autotest_shrines", "autotest_gallery", "autotest_loot", "autotest_swarm", "autotest_magic", "autotest_encounter", "autotest_hostile", "autotest_sweep_encounters" };
    for (const char* key : testKeys) {
        char* value;
        if (config_get_string(&game_config, ARPG_CONFIG_SECTION, key, &value)) {
            config_set_value(&game_config, ARPG_CONFIG_SECTION, key, strcmp(key, "autotest_weapon") == 0 || strcmp(key, "autotest_ammo") == 0 || strcmp(key, "autotest_wasd") == 0 ? -1 : 0);
        }
    }

    arpg_log("ARPG: enabled=%d autotest=%d\n", arpg_on, arpg_test);
}

// Autotest: frame counts, frame rates and per-rotation frame-offset drift of
// a critter's attack animations, to size the speed-ups.
static void arpg_log_anim_info(Object* critter)
{
    static const int anims[] = { ANIM_STAND, ANIM_POINT, ANIM_FIRE_SINGLE, ANIM_FIRE_BURST, ANIM_UNPOINT, ANIM_THROW_PUNCH, ANIM_RUNNING, ANIM_HIT_FROM_FRONT };
    for (int anim : anims) {
        int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, anim, (critter->fid & 0xF000) >> 12, 0);
        CacheEntry* handle;
        Art* art = art_ptr_lock(fid, &handle);
        if (art == NULL) {
            continue;
        }

        int frames = art_frame_max_frame(art);
        char drift[160];
        int length = 0;
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int sumX = 0;
            int sumY = 0;
            for (int frame = 1; frame < frames; frame++) {
                int x;
                int y;
                art_frame_hot(art, frame, rotation, &x, &y);
                sumX += x;
                sumY += y;
            }
            length += snprintf(drift + length, sizeof(drift) - length, " %d,%d", sumX, sumY);
        }
        arpg_log("ARPG anim %s anim=%d frames=%d fps=%d drift:%s\n", critter_name(critter), anim, frames, art_frame_fps(art), drift);
        art_ptr_unlock(handle);
    }
}

static void arpg_free_crit_list()
{
    if (crit_list != NULL) {
        obj_delete_list(crit_list);
        crit_list = NULL;
    }
    crit_count = 0;
}

// Per-level state, reset on every elevation or map change.
static void arpg_reset_level()
{
    for (ArpgAttack* aa : pending_hits) {
        arpg_release(aa);
    }
    pending_hits.clear();

    // Attacks still referenced by animation sequences are released by their
    // forced callbacks when anim_stop() ends those sequences.
    for (ArpgAttack* aa : live_attacks) {
        aa->invalid = true;
    }

    actors.clear();
    stagger_until.clear();
    chill_until.clear();
    combat_ai_over();
    arpg_free_crit_list();
    player_target = NULL;
    player_chasing = false;
    player_special = false;
    player_aim_location = -1;
    player_in_place = false;
    player_unreachable = NULL;
    wasd_moving = false;
    wasd_target_tile = -1;
    wasd_dx = 0;
    wasd_dy = 0;
    slide_active = false;
    dodge_until = 0;
    alert_until = 0;
    next_list_refresh = 0;
    last_energy_tick = 0;
    alerted_packs.clear();
    arpg_hud_reset();
}

// Every object is about to be freed (map load, game load or reset).
void arpg_reset()
{
    if (!arpg_on) {
        return;
    }

    arpg_reset_level();
    monsters.clear();
    shrines.clear();
    reinforced.clear();
    populated_levels.clear();
    if (death_until != 0) {
        death_until = 0;
        game_ui_enable();
    }
    populate_pending = false;
    // Force the map-change path in arpg_tick, even for a reload of the same
    // map, so packs are spawned again.
    last_map = -1;
    last_elevation = -1;
}

static bool arpg_alive(Object* critter)
{
    return critter != NULL
        && FID_TYPE(critter->fid) == OBJ_TYPE_CRITTER
        && (critter->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) == 0
        && (critter->flags & OBJECT_HIDDEN) == 0;
}

static bool arpg_on_map(Object* critter)
{
    for (int index = 0; index < crit_count; index++) {
        if (crit_list[index] == critter) {
            return true;
        }
    }
    return false;
}

void arpg_on_critter_removed(Object* critter)
{
    if (!arpg_on) {
        return;
    }

    actors.erase(critter);
    stagger_until.erase(critter);
    chill_until.erase(critter);
    monsters.erase(critter);
    reinforced.erase(std::remove(reinforced.begin(), reinforced.end(), critter), reinforced.end());
    for (auto& entry : monsters) {
        if (entry.second.leader == critter) {
            entry.second.leader = NULL;
        }
    }

    for (auto& entry : actors) {
        if (entry.second.target == critter) {
            entry.second.target = NULL;
        }
    }

    if (player_target == critter) {
        player_target = NULL;
        player_chasing = false;
    }

    for (ArpgAttack* aa : live_attacks) {
        if (aa->attack.attacker == critter || aa->attack.defender == critter) {
            aa->invalid = true;
        }
        if (aa->target == critter) {
            aa->target = NULL;
        }
        for (int index = 0; index < aa->attack.extrasLength; index++) {
            if (aa->attack.extras[index] == critter) {
                aa->invalid = true;
            }
        }
    }

    for (int index = 0; index < crit_count; index++) {
        if (crit_list[index] == critter) {
            crit_list[index] = crit_list[crit_count - 1];
            crit_count -= 1;
            break;
        }
    }

    combatai_delete_critter(critter);
}

static void arpg_refresh_list()
{
    combat_ai_over();
    arpg_free_crit_list();

    crit_count = obj_create_list(-1, map_elevation, OBJ_TYPE_CRITTER, &crit_list);
    if (crit_count < 0) {
        crit_count = 0;
        crit_list = NULL;
    }

    combat_ai_begin(crit_count, crit_list);

    // Drop actors that are no longer on this elevation.
    for (auto it = actors.begin(); it != actors.end();) {
        if (!arpg_on_map(it->first)) {
            it = actors.erase(it);
        } else {
            ++it;
        }
    }
}

// Checks whether `attacker` can hit `defender` right now, ignoring action
// points (real-time mode has no AP).
static int arpg_bad_shot(Object* attacker, Object* defender, int hitMode)
{
    int oldAp = attacker->data.critter.combat.ap;
    attacker->data.critter.combat.ap = 999;
    int rc = combat_check_bad_shot(attacker, defender, hitMode, false);
    attacker->data.critter.combat.ap = oldAp;
    return rc;
}

static unsigned int arpg_attack_delay(Object* attacker, int hitMode)
{
    int cost = item_w_mp_cost(attacker, hitMode, false);
    int maxAp = stat_level(attacker, STAT_MAXIMUM_ACTION_POINTS);
    if (maxAp < 1) {
        maxAp = 1;
    }
    if (cost < 1) {
        cost = 1;
    }

    int pct = attacker == obj_dude ? player_delay_pct : enemy_delay_pct;
    return (unsigned int)(cost * 1000 / maxAp * pct / 100);
}

static void arpg_float_text(Object* obj, const char* text, int color)
{
    if (!show_numbers || obj == NULL) {
        return;
    }

    char buffer[32];
    strncpy(buffer, text, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    text_object_remove(obj);

    Rect rect;
    if (text_object_create(obj, buffer, 101, color, colorTable[0], &rect) == 0) {
        tile_refresh_rect(&rect, obj->elevation);
    }
}

// ---------------------------------------------------------------------------
// Monster packs, champions and uniques (Diablo II style)
// ---------------------------------------------------------------------------

static ArpgMonster* arpg_monster(Object* critter)
{
    if (critter == NULL) {
        return NULL;
    }
    auto it = monsters.find(critter);
    return it != monsters.end() ? &(it->second) : NULL;
}

// Wild creatures: these come in packs and attack on sight. People, dogs
// (Dogmeat), brahmin, robots and plants never do.
static bool arpg_is_monster_kind(Object* critter)
{
    switch (critter_kill_count_type(critter)) {
    case KILL_TYPE_RADSCORPION:
    case KILL_TYPE_RAT:
    case KILL_TYPE_FLOATER:
    case KILL_TYPE_CENTAUR:
    case KILL_TYPE_MANTIS:
    case KILL_TYPE_DEATH_CLAW:
        return true;
    default:
        return false;
    }
}

static int arpg_monster_speed_pct(Object* critter)
{
    ArpgMonster* monster = arpg_monster(critter);
    int pct = monster != NULL ? monster->speedPct : 100;
    // Chilled (D2 cold damage): half speed.
    auto chill = chill_until.find(critter);
    if (chill != chill_until.end() && get_time() < chill->second) {
        pct /= 2;
    }
    return pct;
}

// ---------------------------------------------------------------------------
// Magic items (D2 MagicPrefix/MagicSuffix, Fallout flavoured). One affix makes
// a magic (blue) item, a prefix and a suffix a rare (yellow) one. Affixes are
// stored in unused object flag bits, so they are saved with the item.
// ---------------------------------------------------------------------------

enum {
    WPRE_NONE,
    WPRE_VICIOUS, // +25% damage
    WPRE_BRUTAL, // +50% damage
    WPRE_BURNING, // +2..6 damage
    WPRE_SHOCKING, // +3..8 damage
    WPRE_VAMPIRIC, // heal 10% of damage dealt
    WPRE_DEADLY, // 15% chance of double damage
    WPRE_COUNT,
};

enum {
    WSUF_NONE,
    WSUF_SPEED, // attacks 25% faster
    WSUF_HASTE, // attacks 50% faster
    WSUF_LEECH, // heal 15% of damage dealt
    WSUF_SLAYING, // +40% damage to champions and uniques
    WSUF_FORTUNE, // more loot
    WSUF_POWER, // +15% damage
    WSUF_COUNT,
};

enum {
    APRE_NONE,
    APRE_STURDY, // -10% damage taken
    APRE_REINFORCED, // -20% damage taken
    APRE_REGENERATING, // +1 HP every 4 s
    APRE_LEAD_LINED, // sheds radiation
    APRE_COUNT,
};

enum {
    ASUF_NONE,
    ASUF_THORNS, // melee attackers take 30% of their damage
    ASUF_VIGOR, // +50% energy regeneration
    ASUF_EVASION, // 10% of hits miss
    ASUF_FORTUNE, // more loot
    ASUF_COUNT,
};

static const char* weapon_prefix_names[WPRE_COUNT] = { "", "Vicious", "Brutal", "Burning", "Shocking", "Vampiric", "Deadly" };
static const char* weapon_suffix_names[WSUF_COUNT] = { "", "of Speed", "of Haste", "of the Leech", "of Slaying", "of Fortune", "of Power" };
static const char* armor_prefix_names[APRE_COUNT] = { "", "Sturdy", "Reinforced", "Regenerating", "Lead-Lined" };
static const char* armor_suffix_names[ASUF_COUNT] = { "", "of Thorns", "of Vigor", "of Evasion", "of Fortune" };

static int arpg_item_prefix(Object* item)
{
    return item != NULL ? (item->flags & ARPG_ITEM_PREFIX_MASK) >> ARPG_ITEM_PREFIX_SHIFT : 0;
}

static int arpg_item_suffix(Object* item)
{
    return item != NULL ? (item->flags & ARPG_ITEM_SUFFIX_MASK) >> ARPG_ITEM_SUFFIX_SHIFT : 0;
}

// Unique items (D2 gold): a fixed base item, our own name and two or three
// affixes. The prefix field holds a marker (8..15, never rolled as a magic
// prefix) and the suffix field the low four bits of the unique's number:
// number = (15 - marker) * 16 + suffix. Marker 15 is the original encoding
// (numbers 0..15), so items saved before the widening keep their meaning.
#define UNIQUE_MARKER 15
#define UNIQUE_MARKER_MIN 8
#define UNIQUE_MAX ((UNIQUE_MARKER - UNIQUE_MARKER_MIN + 1) * 16)

// Unique-only powers (D2 unique item mods that magic affixes don't roll).
enum {
    UPOW_KNOCKBACK = 0x01, // hits knock the target back
    UPOW_CRUSHING = 0x02, // 20% chance: +25% of the target's current HP
    UPOW_CHILL = 0x04, // +3..9 cold damage, slows the target for 3 s
};

typedef struct ArpgUniqueItem {
    int pid;
    const char* name;
    int prefixes[2];
    int suffixes[2];
    int powers; // UPOW_*
    // Art base name (8.3) in art/items and art/inven. These FRMs are made
    // locally from the player's own Diablo II install by tools/d2art and are
    // never shipped; without them the base item's art is used.
    const char* art;
} ArpgUniqueItem;

static const ArpgUniqueItem unique_items[] = {
    { 0, "", { 0, 0 }, { 0, 0 } }, // 0 is "none"
    { 8, "Wastelander's Friend", { WPRE_VICIOUS, 0 }, { WSUF_SPEED, 0 } }, // 10mm Pistol
    { 94, "Junktown Justice", { WPRE_BRUTAL, 0 }, { WSUF_POWER, 0 } }, // Shotgun
    { 116, "The Ripper's Kiss", { WPRE_VAMPIRIC, 0 }, { WSUF_LEECH, WSUF_SPEED } }, // Ripper
    { 10, "Overseer's Mercy", { WPRE_DEADLY, 0 }, { WSUF_SLAYING, 0 } }, // Hunting Rifle
    { 22, "Hub Peacemaker", { WPRE_BRUTAL, 0 }, { WSUF_HASTE, 0 } }, // 14mm Pistol
    { 23, "Old Faithful", { WPRE_VICIOUS, 0 }, { WSUF_HASTE, WSUF_FORTUNE } }, // Assault Rifle
    { 16, "Sunburst", { WPRE_BURNING, WPRE_VICIOUS }, { WSUF_SPEED, 0 } }, // Laser Pistol
    { 235, "Brotherhood's Wrath", { WPRE_SHOCKING, WPRE_BRUTAL }, { WSUF_HASTE, 0 } }, // Power Fist
    { 11, "Glow Torch", { WPRE_BURNING, 0 }, { WSUF_POWER, WSUF_SLAYING } }, // Flamer
    { 242, "Deathclaw Bane", { WPRE_DEADLY, WPRE_VICIOUS }, { WSUF_SLAYING, 0 } }, // Combat Shotgun
    { 74, "Vault Jacket of Ages", { APRE_REGENERATING, 0 }, { ASUF_VIGOR, ASUF_FORTUNE } }, // Leather Jacket
    { 2, "Scorpion Shell", { APRE_REINFORCED, 0 }, { ASUF_THORNS, 0 } }, // Metal Armor
    { 1, "Lead Mantle", { APRE_LEAD_LINED, APRE_STURDY }, { ASUF_EVASION, 0 } }, // Leather Armor
    { 17, "Bulwark of the Hub", { APRE_REINFORCED, APRE_REGENERATING }, { ASUF_FORTUNE, 0 } }, // Combat Armor
    // D2 homages from here on: our own names, D2-style powers, art made from
    // the D2 item (see `art`). Comments name the D2 unique and base.
    { 143, "Dust Devil", { WPRE_BRUTAL, WPRE_DEADLY }, { WSUF_HASTE, WSUF_POWER }, UPOW_KNOCKBACK, "d2lwb" }, // Sniper Rifle (Windforce, Hydra Bow)
    // 16+: second marker (prefix field 14) onwards.
    { 115, "The Granddaddy", { WPRE_BRUTAL, WPRE_VICIOUS }, { WSUF_POWER, 0 }, 0, "d2gsdu" }, // Super Sledge (The Grandfather, Colossus Blade)
    { 116, "Neon Saber", { WPRE_SHOCKING, WPRE_VAMPIRIC }, { WSUF_HASTE, 0 }, 0, "d2crsu" }, // Ripper (Lightsabre, Phase Blade)
    { 160, "Stormwhip", { WPRE_SHOCKING, 0 }, { WSUF_SPEED, 0 }, UPOW_CRUSHING, "d2fla" }, // Cattle Prod (Stormlash, Scourge)
    { 6, "Doomsayer", { WPRE_BRUTAL, WPRE_VAMPIRIC }, { WSUF_LEECH, 0 }, 0, "d2bswu" }, // Sledgehammer (Doombringer, Champion Sword)
    { 7, "Toll Collector", { WPRE_DEADLY, 0 }, { WSUF_LEECH, 0 }, UPOW_CHILL, "d2scyu" }, // Spear (The Reaper's Toll, Thresher)
    { 115, "Hellraiser", { WPRE_BURNING, WPRE_BRUTAL }, { WSUF_POWER, 0 }, 0, "d2btxu" }, // Super Sledge (Hellslayer, Decapitator)
    { 6, "Skullbuster", { WPRE_BRUTAL, 0 }, { WSUF_SLAYING, 0 }, UPOW_CRUSHING | UPOW_KNOCKBACK, "d2gma" }, // Sledgehammer (The Cranium Basher, Thunder Maul)
    { 236, "Nuka Frost", { WPRE_VICIOUS, 0 }, { WSUF_SPEED, 0 }, UPOW_CHILL, "d2lsdu" }, // Combat Knife (Frostwind, Cryptic Sword)
    { 7, "Rad Needle", { WPRE_DEADLY, WPRE_BRUTAL }, { WSUF_HASTE, 0 }, 0, "d2spr" }, // Spear (Arioc's Needle, Hyperion Spear)
    { 10, "Eagle Eye", { WPRE_DEADLY, WPRE_VICIOUS }, { WSUF_SLAYING, 0 }, 0, "d2lbb" }, // Hunting Rifle (Eaglehorn, Crusader Bow)
    { 118, "Frostbite Kyanon", { WPRE_BRUTAL, 0 }, { WSUF_SPEED, 0 }, UPOW_CHILL, "d2hxbu" }, // Laser Rifle (Buriza-Do Kyanon, Balista)
    { 9, "Demon Repeater", { WPRE_BURNING, 0 }, { WSUF_HASTE, 0 }, 0, "d2rxbu" }, // 10mm SMG (Demon Machine, Chu-Ko-Nu)
    { 5, "Starfall", { WPRE_BURNING, WPRE_SHOCKING }, { WSUF_SPEED, 0 }, UPOW_CHILL, "d2mstu" }, // Club (Baranar's Star, Devil Star)
    { 20, "Reaver's Bite", { WPRE_BURNING, WPRE_BRUTAL }, { WSUF_FORTUNE, 0 }, 0, "d2gaxu" }, // Crowbar (Messerschmidt's Reaver, Champion Axe)
    { 4, "Brainstorm Shiv", { WPRE_VICIOUS, 0 }, { WSUF_SPEED, 0 }, UPOW_CHILL, "d2dgr" }, // Knife (Wizardspike, Bone Knife)
};

#define UNIQUE_ITEM_COUNT ((int)(sizeof(unique_items) / sizeof(*unique_items)))
static_assert(UNIQUE_ITEM_COUNT <= UNIQUE_MAX, "unique number doesn't fit the affix fields");

static bool arpg_item_has_unique_marker(Object* item)
{
    return arpg_item_prefix(item) >= UNIQUE_MARKER_MIN;
}

// The unique's number stored on `item` (0 when not marked unique).
static int arpg_unique_index(Object* item)
{
    if (!arpg_item_has_unique_marker(item)) {
        return 0;
    }
    return (UNIQUE_MARKER - arpg_item_prefix(item)) * 16 + arpg_item_suffix(item);
}

static void arpg_set_unique(Object* item, int index)
{
    item->flags &= ~ARPG_ITEM_AFFIX_MASK;
    item->flags |= ((UNIQUE_MARKER - index / 16) << ARPG_ITEM_PREFIX_SHIFT) & ARPG_ITEM_PREFIX_MASK;
    item->flags |= ((index % 16) << ARPG_ITEM_SUFFIX_SHIFT) & ARPG_ITEM_SUFFIX_MASK;
}

static const ArpgUniqueItem* arpg_unique_item(Object* item)
{
    int index = arpg_unique_index(item);
    if (index <= 0 || index >= UNIQUE_ITEM_COUNT || unique_items[index].pid != (item->pid & 0xFFFFFF)) {
        return NULL;
    }
    return &(unique_items[index]);
}

static bool arpg_item_has_power(Object* item, int power)
{
    const ArpgUniqueItem* unique = arpg_unique_item(item);
    return unique != NULL && (unique->powers & power) != 0;
}

// Unique item art: FIDs of the generated FRMs, registered once (after
// art_init) by appending their names to the item and inventory art lists.
// -1 when the files aren't there (no D2 install / converter not run).
static int unique_ground_fid[UNIQUE_ITEM_COUNT];
static int unique_inv_fid[UNIQUE_ITEM_COUNT];

static void arpg_unique_art_init()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    int found = 0;
    for (int index = 0; index < UNIQUE_ITEM_COUNT; index++) {
        unique_ground_fid[index] = -1;
        unique_inv_fid[index] = -1;
        const char* art = unique_items[index].art;
        if (art == NULL || art[0] == '\0') {
            continue;
        }

        char name[16];
        snprintf(name, sizeof(name), "%s.frm", art);
        int groundIndex = art_add_name(OBJ_TYPE_ITEM, name);
        int invIndex = art_add_name(OBJ_TYPE_INVENTORY, name);
        if (groundIndex == -1 || invIndex == -1) {
            continue;
        }

        int groundFid = art_id(OBJ_TYPE_ITEM, groundIndex, 0, 0, 0);
        int invFid = art_id(OBJ_TYPE_INVENTORY, invIndex, 0, 0, 0);
        if (art_exists(groundFid) && art_exists(invFid)) {
            unique_ground_fid[index] = groundFid;
            unique_inv_fid[index] = invFid;
            found++;
        }
    }
    arpg_log("ARPG: unique item art: %d of %d found\n", found, UNIQUE_ITEM_COUNT - 1);
}

void arpg_art_init()
{
    arpg_unique_art_init();
}

// item_inv_fid() hook: a unique's own inventory art, or -1.
int arpg_item_inv_fid(Object* item)
{
    if (!arpg_on || item == NULL || PID_TYPE(item->pid) != OBJ_TYPE_ITEM || !arpg_item_has_unique_marker(item)) {
        return -1;
    }
    if (arpg_unique_item(item) == NULL) {
        return -1;
    }
    arpg_unique_art_init();
    return unique_inv_fid[arpg_unique_index(item)];
}

// Ground art of a unique: its own when available, else the base item's (also
// repairs FIDs saved by a session whose art list was different).
static void arpg_unique_fix_fid(Object* item)
{
    if (item == NULL || PID_TYPE(item->pid) != OBJ_TYPE_ITEM || !arpg_item_has_unique_marker(item)) {
        return;
    }
    if (arpg_unique_item(item) == NULL) {
        return;
    }
    arpg_unique_art_init();
    int fid = unique_ground_fid[arpg_unique_index(item)];
    if (fid == -1) {
        Proto* proto;
        if (proto_ptr(item->pid, &proto) == -1) {
            return;
        }
        fid = proto->fid;
    }
    if (item->fid != fid) {
        Rect rect;
        obj_change_fid(item, fid, &rect);
        if (item->owner == NULL && item->tile != -1) {
            tile_refresh_rect(&rect, item->elevation);
        }
    }
}

// Runs over ground items on the current level and the player's inventory.
static void arpg_unique_fix_all()
{
    Object** items = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_ITEM, &items);
    for (int index = 0; index < count; index++) {
        arpg_unique_fix_fid(items[index]);
        if (arpg_test && arpg_unique_item(items[index]) != NULL) {
            arpg_log("ARPG: level unique #%d %s fid 0x%x\n", arpg_unique_index(items[index]), item_name(items[index]), items[index]->fid);
        }
    }
    if (count > 0) {
        obj_delete_list(items);
    }
    if (obj_dude != NULL) {
        Inventory* inventory = &(obj_dude->data.inventory);
        for (int index = 0; index < inventory->length; index++) {
            arpg_unique_fix_fid(inventory->items[index].item);
        }
    }
}

// 0 normal, 1 magic, 2 rare, 3 unique.
int arpg_item_quality(Object* item)
{
    if (item == NULL || PID_TYPE(item->pid) != OBJ_TYPE_ITEM) {
        return 0;
    }
    int type = item_get_type(item);
    if (type != ITEM_TYPE_WEAPON && type != ITEM_TYPE_ARMOR) {
        return 0;
    }
    if (arpg_item_has_unique_marker(item)) {
        return arpg_unique_item(item) != NULL ? 3 : 0;
    }
    return (arpg_item_prefix(item) != 0 ? 1 : 0) + (arpg_item_suffix(item) != 0 ? 1 : 0);
}

// Whether an item carries an affix (uniques can carry two of a kind).
static bool arpg_item_has_prefix(Object* item, int prefix)
{
    if (item == NULL || prefix == 0) {
        return false;
    }
    const ArpgUniqueItem* unique = arpg_unique_item(item);
    if (unique != NULL) {
        return unique->prefixes[0] == prefix || unique->prefixes[1] == prefix;
    }
    return arpg_item_prefix(item) == prefix && arpg_item_quality(item) > 0;
}

static bool arpg_item_has_suffix(Object* item, int suffix)
{
    if (item == NULL || suffix == 0) {
        return false;
    }
    const ArpgUniqueItem* unique = arpg_unique_item(item);
    if (unique != NULL) {
        return unique->suffixes[0] == suffix || unique->suffixes[1] == suffix;
    }
    return arpg_item_suffix(item) == suffix && arpg_item_quality(item) > 0;
}

// item_name() hook: "Vicious 10mm Pistol of Speed", a unique's own name, or
// NULL for plain items.
char* arpg_item_name(Object* item)
{
    if (!arpg_on || arpg_item_quality(item) == 0) {
        return NULL;
    }

    static char buffers[4][96];
    static int next = 0;
    char* buffer = buffers[next];
    next = (next + 1) % 4;

    const ArpgUniqueItem* unique = arpg_unique_item(item);
    if (unique != NULL) {
        snprintf(buffer, 96, "%s", unique->name);
        return buffer;
    }

    bool weapon = item_get_type(item) == ITEM_TYPE_WEAPON;
    int prefix = arpg_item_prefix(item);
    int suffix = arpg_item_suffix(item);
    const char* prefixName = "";
    const char* suffixName = "";
    if (weapon) {
        prefixName = prefix < WPRE_COUNT ? weapon_prefix_names[prefix] : "";
        suffixName = suffix < WSUF_COUNT ? weapon_suffix_names[suffix] : "";
    } else {
        prefixName = prefix < APRE_COUNT ? armor_prefix_names[prefix] : "";
        suffixName = suffix < ASUF_COUNT ? armor_suffix_names[suffix] : "";
    }

    // At most 48 characters: callers like the character sheet export
    // concatenate three names into 256-byte lines (vanilla names are short).
    snprintf(buffer, 49, "%s%s%s%s%s",
        prefixName,
        prefixName[0] != '\0' ? " " : "",
        proto_name(item->pid),
        suffixName[0] != '\0' ? " " : "",
        suffixName);
    return buffer;
}

// What a magic, rare or unique item's affixes do, for examining it (D2 item
// tooltips). Writes line `index` and returns true while there are lines.
bool arpg_item_affix_text(Object* item, int index, char* out, int size)
{
    if (!arpg_on || arpg_item_quality(item) == 0) {
        return false;
    }

    static const char* weaponPrefixText[WPRE_COUNT] = { "", "+25% damage", "+50% damage", "+2-6 fire damage",
        "+3-8 shock damage", "heals 10% of damage dealt", "15% chance of double damage" };
    static const char* weaponSuffixText[WSUF_COUNT] = { "", "+25% attack speed", "+50% attack speed",
        "heals 15% of damage dealt", "+40% damage to champions and uniques", "more and better loot", "+15% damage" };
    static const char* armorPrefixText[APRE_COUNT] = { "", "-10% damage taken", "-20% damage taken",
        "regenerates 1 HP every 4 s", "sheds radiation" };
    static const char* armorSuffixText[ASUF_COUNT] = { "", "melee attackers take 30% of their damage",
        "+50% energy regeneration", "10% of hits miss", "more and better loot" };

    bool weapon = item_get_type(item) == ITEM_TYPE_WEAPON;
    const char* const* prefixText = weapon ? weaponPrefixText : armorPrefixText;
    const char* const* suffixText = weapon ? weaponSuffixText : armorSuffixText;
    int prefixCount = weapon ? WPRE_COUNT : APRE_COUNT;
    int suffixCount = weapon ? WSUF_COUNT : ASUF_COUNT;

    // The lines, in order: a unique's banner, then prefixes, then suffixes.
    std::vector<std::string> lines;
    const ArpgUniqueItem* unique = arpg_unique_item(item);
    if (unique != NULL) {
        lines.push_back(std::string("Unique ") + proto_name(item->pid) + ".");
        for (int prefix : unique->prefixes) {
            if (prefix > 0 && prefix < prefixCount) {
                lines.push_back(std::string(prefixText[prefix]) + ".");
            }
        }
        for (int suffix : unique->suffixes) {
            if (suffix > 0 && suffix < suffixCount) {
                lines.push_back(std::string(suffixText[suffix]) + ".");
            }
        }
        if ((unique->powers & UPOW_KNOCKBACK) != 0) {
            lines.push_back("knocks enemies back.");
        }
        if ((unique->powers & UPOW_CRUSHING) != 0) {
            lines.push_back("20% chance of crushing blow.");
        }
        if ((unique->powers & UPOW_CHILL) != 0) {
            lines.push_back("+3-9 cold damage, chills.");
        }
    } else {
        int prefix = arpg_item_prefix(item);
        int suffix = arpg_item_suffix(item);
        if (prefix > 0 && prefix < prefixCount) {
            lines.push_back(std::string(weapon ? weapon_prefix_names[prefix] : armor_prefix_names[prefix]) + ": " + prefixText[prefix] + ".");
        }
        if (suffix > 0 && suffix < suffixCount) {
            lines.push_back(std::string(weapon ? weapon_suffix_names[suffix] : armor_suffix_names[suffix]) + ": " + suffixText[suffix] + ".");
        }
    }

    if (index < 0 || index >= (int)lines.size()) {
        return false;
    }
    snprintf(out, size, "%s", lines[index].c_str());
    return true;
}

// A unique whose base item fits the budget, or 0.
static int arpg_pick_unique(int budget)
{
    std::vector<int> eligible;
    for (int index = 1; index < UNIQUE_ITEM_COUNT; index++) {
        Proto* proto;
        if (proto_ptr(unique_items[index].pid, &proto) != -1 && proto->item.cost <= budget) {
            eligible.push_back(index);
        }
    }
    return eligible.empty() ? 0 : eligible[roll_random(0, (int)eligible.size() - 1)];
}

// Rolls magic or rare affixes onto a freshly dropped weapon or armor.
// `findPct` scales the chances (D2 magic find): 100 normal.
static void arpg_roll_magic(Object* item, int findPct, bool atLeastMagic)
{
    if (item == NULL) {
        return;
    }
    int type = item_get_type(item);
    if (type != ITEM_TYPE_WEAPON && type != ITEM_TYPE_ARMOR) {
        return;
    }
    // Grenades, molotovs, throwing knives and flares are used up and stack;
    // they stay plain.
    if (type == ITEM_TYPE_WEAPON && item_w_subtype(item, HIT_MODE_LEFT_WEAPON_PRIMARY) == ATTACK_TYPE_THROW) {
        return;
    }

    int roll = roll_random(1, 1000);
    bool rare = roll <= 60 * findPct / 100;
    bool magic = rare || atLeastMagic || roll <= 300 * findPct / 100;
    if (!magic) {
        return;
    }

    int prefixCount = type == ITEM_TYPE_WEAPON ? WPRE_COUNT : APRE_COUNT;
    int suffixCount = type == ITEM_TYPE_WEAPON ? WSUF_COUNT : ASUF_COUNT;
    int prefix = 0;
    int suffix = 0;
    if (rare) {
        prefix = roll_random(1, prefixCount - 1);
        suffix = roll_random(1, suffixCount - 1);
    } else if (roll_random(0, 1) == 0) {
        prefix = roll_random(1, prefixCount - 1);
    } else {
        suffix = roll_random(1, suffixCount - 1);
    }

    item->flags &= ~ARPG_ITEM_AFFIX_MASK;
    item->flags |= (prefix << ARPG_ITEM_PREFIX_SHIFT) & ARPG_ITEM_PREFIX_MASK;
    item->flags |= (suffix << ARPG_ITEM_SUFFIX_SHIFT) & ARPG_ITEM_SUFFIX_MASK;
}

// The player's weapon for `hitMode`, and the armor being worn.
static Object* arpg_dude_weapon()
{
    int hitMode;
    bool aiming;
    if (obj_dude == NULL || intface_get_attack(&hitMode, &aiming) == -1) {
        return NULL;
    }
    Object* weapon = item_hit_with(obj_dude, hitMode);
    return weapon != NULL && item_get_type(weapon) == ITEM_TYPE_WEAPON ? weapon : NULL;
}

static Object* arpg_dude_armor()
{
    return obj_dude != NULL ? inven_worn(obj_dude) : NULL;
}

static bool arpg_dude_has(bool weapon, int prefix, int suffix)
{
    Object* item = weapon ? arpg_dude_weapon() : arpg_dude_armor();
    if (item == NULL) {
        return false;
    }
    return arpg_item_has_prefix(item, prefix) || arpg_item_has_suffix(item, suffix);
}

// Weapon speed affixes (D2 increased attack speed), in percent.
static int arpg_dude_attack_speed_pct()
{
    Object* weapon = arpg_dude_weapon();
    if (arpg_item_has_suffix(weapon, WSUF_HASTE)) {
        return 150;
    }
    if (arpg_item_has_suffix(weapon, WSUF_SPEED)) {
        return 125;
    }
    return 100;
}

// Magic find from Fortune affixes.
static int arpg_dude_find_pct()
{
    int pct = 100;
    if (arpg_dude_has(true, 0, WSUF_FORTUNE)) {
        pct += 50;
    }
    if (arpg_dude_has(false, 0, ASUF_FORTUNE)) {
        pct += 50;
    }
    return pct;
}

// Log name with a rank tag: "Radscorpion", "Radscorpion[C]", "Grimspine[U]".
static const char* arpg_label(Object* critter)
{
    static char buffers[2][64];
    static int next = 0;
    char* buffer = buffers[next];
    next = (next + 1) % 2;

    ArpgMonster* monster = arpg_monster(critter);
    if (monster == NULL || monster->rank == RANK_NORMAL) {
        snprintf(buffer, 64, "%s", critter_name(critter));
    } else {
        static const char tags[] = { 'N', 'M', 'C', 'U' };
        snprintf(buffer, 64, "%s[%c]", monster->name, tags[monster->rank]);
    }
    return buffer;
}

// Our own wasteland take on D2's random unique names.
static void arpg_unique_name(char* dest, size_t size)
{
    static const char* prefixes[] = { "Rot", "Rust", "Ash", "Glow", "Bone", "Grim", "Scab", "Blight", "Dust", "Gore", "Rad", "Sludge", "Char", "Gut", "Murk", "Grub" };
    static const char* suffixes[] = { "fang", "maw", "claw", "hide", "tooth", "spine", "eye", "skull", "gut", "tail", "bite", "shell", "sting", "back", "jaw" };
    static const char* titles[] = { "the Mutant", "the Glowing", "the Hungry", "the Ravager", "the Undying", "the Feral", "the Irradiated", "the Ancient", "the Gnawer", "the Defiler", "the Vile", "the Rabid", "the Wasteland Terror", "the Pale", "the Twice-Born" };
    snprintf(dest, size, "%s%s %s",
        prefixes[roll_random(0, (int)(sizeof(prefixes) / sizeof(*prefixes)) - 1)],
        suffixes[roll_random(0, (int)(sizeof(suffixes) / sizeof(*suffixes)) - 1)],
        titles[roll_random(0, (int)(sizeof(titles) / sizeof(*titles)) - 1)]);
}

static void arpg_human_name(char* dest, size_t size);

static void arpg_make_monster(Object* critter, int rank, unsigned int affixes, int pack, Object* leader)
{
    ArpgMonster& monster = monsters[critter];
    memset(&monster, 0, sizeof(monster));
    monster.rank = rank;
    monster.affixes = affixes;
    monster.pack = pack;
    monster.leader = leader;
    monster.speedPct = 100;
    monster.xpPct = 100;

    // D2 Normal gives minions +100%, champions +200% and uniques +300% life;
    // Fallout's guns deal single-digit damage, so the bonuses here are a
    // notch lower to keep a unique around ten shots.
    switch (rank) {
    case RANK_MINION:
        monster.hpPct = 50;
        break;
    case RANK_CHAMPION:
        monster.hpPct = 150;
        monster.dmgPct = 50;
        monster.speedPct = 115;
        monster.xpPct = 300;
        break;
    case RANK_UNIQUE:
        monster.hpPct = 200;
        monster.dmgPct = 50;
        monster.speedPct = 110;
        monster.xpPct = 500;
        break;
    }

    if ((affixes & CHAMP_FANATIC) != 0) {
        monster.speedPct = 175;
    }
    if ((affixes & CHAMP_BERSERKER) != 0) {
        monster.dmgPct += 75;
    }
    if ((affixes & CHAMP_GHOSTLY) != 0) {
        monster.speedPct = 85;
    }
    if ((affixes & CHAMP_POSSESSED) != 0) {
        monster.hpPct += 100;
    }
    if ((affixes & AFFIX_STRONG) != 0) {
        monster.dmgPct += 75;
    }
    if ((affixes & AFFIX_FAST) != 0) {
        monster.speedPct = std::max(monster.speedPct, 150);
    }
    // People carry guns that already hit hard (a raider boss with a shotgun
    // and +50% did 30 a shot); their rank bonus is halved.
    if (!arpg_is_monster_kind(critter)) {
        monster.dmgPct /= 2;
    }

    // Look the part: uniques glow, champions shimmer, ghosts are see-through
    // and berserkers run red.
    if (rank == RANK_UNIQUE) {
        obj_set_light(critter, 4, 65536, NULL);
    } else if (rank == RANK_CHAMPION) {
        obj_set_light(critter, 2, 45000, NULL);
    }
    if ((affixes & CHAMP_GHOSTLY) != 0) {
        critter->flags |= OBJECT_TRANS_GLASS;
    } else if ((affixes & CHAMP_BERSERKER) != 0) {
        critter->flags |= OBJECT_TRANS_RED;
    }

    if (rank == RANK_UNIQUE) {
        if (arpg_is_monster_kind(critter)) {
            arpg_unique_name(monster.name, sizeof(monster.name));
        } else {
            arpg_human_name(monster.name, sizeof(monster.name));
        }
    } else {
        snprintf(monster.name, sizeof(monster.name), "%s", critter_name(critter));
    }
}

bool arpg_monster_info(Object* critter, int* rank, char* name, int nameSize, char* mods, int modsSize)
{
    ArpgMonster* monster = arpg_monster(critter);
    if (monster == NULL) {
        *rank = RANK_NORMAL;
        snprintf(name, nameSize, "%s", critter_name(critter));
        mods[0] = '\0';
        return false;
    }

    *rank = monster->rank;
    snprintf(name, nameSize, "%s", monster->name);

    int length = 0;
    mods[0] = '\0';
    if (monster->rank == RANK_MINION) {
        length += snprintf(mods + length, modsSize - length, "Minion");
    }
    for (const auto& entry : affix_names) {
        if ((monster->affixes & entry.flag) != 0 && length < modsSize - 1) {
            length += snprintf(mods + length, modsSize - length, "%s%s", length > 0 ? ", " : "", entry.name);
        }
    }
    return true;
}

// A free tile a few steps from `seed`, reachable on foot from it so no
// clone ends up inside a wall or behind one.
static int arpg_pack_tile(Object* seed)
{
    for (int attempt = 0; attempt < 24; attempt++) {
        int tile = tile_num_in_direction(seed->tile, roll_random(0, 5), roll_random(1, 3));
        if (attempt >= 12) {
            tile = tile_num_in_direction(tile, roll_random(0, 5), 1);
        }
        if (tile < 0 || tile >= HEX_GRID_SIZE || tile == seed->tile) {
            continue;
        }
        if (obj_blocking_at(NULL, tile, seed->elevation) != NULL) {
            continue;
        }
        if (seed->elevation == obj_dude->elevation && tile_dist(tile, obj_dude->tile) < 5) {
            continue;
        }
        int length = make_path(seed, seed->tile, tile, NULL, 1);
        if (length <= 0 || length > 6) {
            continue;
        }
        return tile;
    }
    return -1;
}

// Monster kin for mixed packs (D2 groups mix related monsters): critter
// protos of the same kill type whose XP is within half to triple the seed's.
typedef struct ArpgKin {
    int pid;
    int killType;
    int xp;
} ArpgKin;

static std::vector<ArpgKin> kin_table;

static void arpg_build_kin_table()
{
    kin_table.clear();
    for (int index = 1; index < 400; index++) {
        int pid = (OBJ_TYPE_CRITTER << 24) | index;
        Proto* proto;
        if (proto_ptr(pid, &proto) == -1 || proto->fid == -1) {
            continue;
        }
        switch (proto->critter.data.killType) {
        case KILL_TYPE_RADSCORPION:
        case KILL_TYPE_RAT:
        case KILL_TYPE_FLOATER:
        case KILL_TYPE_CENTAUR:
        case KILL_TYPE_MANTIS:
        case KILL_TYPE_DEATH_CLAW:
            break;
        default:
            continue;
        }
        // The Boneyard's Mother Deathclaw is a one-off.
        if (proto->critter.data.experience >= 1500) {
            continue;
        }
        kin_table.push_back({ pid, proto->critter.data.killType, proto->critter.data.experience });
    }
    arpg_log("ARPG: kin table has %d monster kinds\n", (int)kin_table.size());
}

static int arpg_pick_kin(Object* seed)
{
    if (kin_table.empty()) {
        arpg_build_kin_table();
    }

    int killType = critter_kill_count_type(seed);
    int xp = critter_kill_exps(seed);
    std::vector<int> choices;
    for (const ArpgKin& kin : kin_table) {
        if (kin.killType == killType && kin.pid != seed->pid && kin.xp * 2 >= xp && kin.xp <= xp * 3) {
            choices.push_back(kin.pid);
        }
    }
    return choices.empty() ? seed->pid : choices[roll_random(0, (int)choices.size() - 1)];
}

static Object* arpg_spawn_clone(Object* seed, bool allowKin = true)
{
    int tile = arpg_pack_tile(seed);
    if (tile == -1) {
        return NULL;
    }

    // A third of a pack is related kin rather than copies of the seed.
    int pid = allowKin && roll_random(1, 100) <= 30 ? arpg_pick_kin(seed) : seed->pid;

    int fid;
    if (pid == seed->pid) {
        fid = art_id(OBJ_TYPE_CRITTER, seed->fid & 0xFFF, ANIM_STAND, (seed->fid & 0xF000) >> 12, seed->rotation + 1);
    } else {
        Proto* proto;
        if (proto_ptr(pid, &proto) == -1) {
            return NULL;
        }
        fid = proto->fid;
    }

    Object* clone;
    if (obj_new(&clone, fid, pid) == -1) {
        return NULL;
    }

    // obj_new attaches the proto's script, if any. Pack monsters run on our
    // AI alone, and a script whose owner is never saved would be orphaned in
    // the save.
    if (clone->sid != -1) {
        scr_remove(clone->sid);
        clone->sid = -1;
        stat_clone_scripts++;
    }

    // Never saved: packs are rolled again when the map is entered again.
    clone->flags |= OBJECT_NO_SAVE | ARPG_OBJECT_SPAWNED;
    // Same side as the seed so the pack fights together; kin keep their own
    // AI packet.
    clone->data.critter.combat.team = seed->data.critter.combat.team;
    if (pid == seed->pid) {
        clone->data.critter.combat.aiPacket = seed->data.critter.combat.aiPacket;
    }

    Rect rect;
    obj_set_rotation(clone, roll_random(0, ROTATION_COUNT - 1), NULL);
    if (obj_move_to_tile(clone, tile, seed->elevation, &rect) == 0) {
        tile_refresh_rect(&rect, seed->elevation);
    }

    stat_spawned++;
    return clone;
}

// Our own wasteland boss names for human uniques.
static void arpg_human_name(char* dest, size_t size)
{
    static const char* firsts[] = { "Mad", "Bloody", "Iron", "Rusty", "One-Eyed", "Lucky", "Big", "Crazy", "Dirty", "Slim", "Red", "Gravel" };
    static const char* names[] = { "Pete", "Sal", "Mae", "Duke", "Carl", "Rosa", "Hank", "Vera", "Jonas", "Lou", "Mags", "Tibbs" };
    static const char* titles[] = { "the Butcher", "the Skinner", "Two-Guns", "the Raider King", "the Cruel", "the Jackal", "Deadeye", "the Collector", "of the Wastes", "the Scavenger" };
    snprintf(dest, size, "%s %s %s",
        firsts[roll_random(0, (int)(sizeof(firsts) / sizeof(*firsts)) - 1)],
        names[roll_random(0, (int)(sizeof(names) / sizeof(*names)) - 1)],
        titles[roll_random(0, (int)(sizeof(titles) / sizeof(*titles)) - 1)]);
}

static unsigned int arpg_roll_affixes(int count)
{
    unsigned int affixes = 0;
    int total = (int)(sizeof(unique_affixes) / sizeof(*unique_affixes));
    for (int picked = 0; picked < count;) {
        unsigned int affix = unique_affixes[roll_random(0, total - 1)];
        if ((affixes & affix) == 0) {
            affixes |= affix;
            picked++;
        }
    }
    return affixes;
}

// Diablo-style population: every wild monster on the level becomes the
// heart of a pack. Some packs are champions, a few are led by a named unique
// with minions.
static void arpg_populate_level()
{
    if (!packs_enabled) {
        return;
    }

    int key = map_get_index_number() * 4 + map_elevation;
    if (std::find(populated_levels.begin(), populated_levels.end(), key) != populated_levels.end()) {
        return;
    }
    populated_levels.push_back(key);

    if (arpg_test) {
        // Magic item affixes live in object flag bits vanilla never uses;
        // prove no shipped map sets them.
        int flagged = 0;
        int checked = 0;
        // Top-level objects and everything carried (one level of containers
        // nested in inventories too).
        std::vector<Object*> pending;
        for (Object* obj = obj_find_first_at(map_elevation); obj != NULL; obj = obj_find_next_at()) {
            pending.push_back(obj);
        }
        for (size_t index = 0; index < pending.size(); index++) {
            Object* obj = pending[index];
            checked++;
            if ((obj->flags & ARPG_ITEM_AFFIX_MASK) != 0) {
                flagged++;
                if (flagged <= 3) {
                    arpg_log("ARPG: affix bits set on pid %d flags 0x%x\n", obj->pid, obj->flags);
                }
            }
            int type = PID_TYPE(obj->pid);
            if (type == OBJ_TYPE_CRITTER || type == OBJ_TYPE_ITEM) {
                Inventory* inventory = &(obj->data.inventory);
                for (int item = 0; item < inventory->length && pending.size() < 20000; item++) {
                    pending.push_back(inventory->items[item].item);
                }
            }
        }
        arpg_log("ARPG: level %d affix-bit objects: %d of %d\n", key, flagged, checked);
    }

    // Packs don't respawn until some game time has passed since they were
    // last spawned here (loading an older save always respawns).
    int hour = (int)(game_time() / 36000);
    auto last = populated_hour.find(key);
    bool fresh = map_get_index_number() == random_map_index;
    if (!fresh && last != populated_hour.end() && hour >= last->second && hour - last->second < pack_respawn_hours) {
        arpg_log("ARPG: packs on level %d spawned %d h ago, not respawning\n", key, hour - last->second);
        return;
    }
    populated_hour[key] = hour;

    std::vector<Object*> seeds;
    for (int index = 0; index < crit_count; index++) {
        Object* critter = crit_list[index];
        if (critter == obj_dude || !arpg_alive(critter) || isPartyMember(critter) || arpg_is_spawned(critter)) {
            continue;
        }
        if (critter->elevation != map_elevation || critter->tile == -1 || !arpg_is_monster_kind(critter)) {
            continue;
        }
        if (critter->data.critter.combat.team == obj_dude->data.critter.combat.team) {
            continue;
        }
        // Boss monsters (the Boneyard's Mother Deathclaw) are one of a kind;
        // clones wouldn't even count for their quests.
        if (critter_kill_exps(critter) >= 1500) {
            continue;
        }
        seeds.push_back(critter);
    }

    // A fresh character meets thinner packs and fewer champions (D2 area
    // levels): 50% at level 1, 75% at 2, full from level 3.
    int level = stat_pc_get(PC_STAT_LEVEL);
    int levelPct = std::max(50, std::min(100, (level + 1) * 25));
    int uniqueChance = unique_pct * levelPct / 100;
    int championChance = champion_pct * levelPct / 100;
    // Never more than about three extra monsters per original.
    int budget = std::min(max_spawns, (int)seeds.size() * 3);

    int spawned = 0;
    int champions = 0;
    int uniques = 0;
    for (Object* seed : seeds) {
        if (spawned >= budget) {
            break;
        }

        int pack = next_pack_id++;
        arpg_make_monster(seed, RANK_NORMAL, 0, pack, NULL);

        // Big monsters come in smaller groups (D2 MonStats MinGrp/MaxGrp),
        // whatever the pack's rank.
        int xp = critter_kill_exps(seed);
        int extra;
        int championCount;
        int minMinions;
        if (xp >= 500) {
            extra = roll_random(0, 1);
            championCount = roll_random(1, 2);
            minMinions = 0;
        } else if (xp >= 200) {
            extra = roll_random(1, 2);
            championCount = 2;
            minMinions = 1;
        } else {
            extra = roll_random(pack_min, std::max(pack_min, pack_max));
            championCount = roll_random(2, 3);
            minMinions = 2;
        }
        // Scale, rounding randomly (3 x 50% is 1 or 2, not always 1).
        int scaled = extra * pack_density_pct * levelPct;
        extra = scaled / 10000;
        if (roll_random(0, 9999) < scaled % 10000) {
            extra++;
        }

        int roll = roll_random(1, 100);
        if (roll <= uniqueChance) {
            Object* boss = arpg_spawn_clone(seed, false);
            if (boss != NULL) {
                spawned++;
                uniques++;
                arpg_make_monster(boss, RANK_UNIQUE, arpg_roll_affixes(xp >= 300 ? 3 : 2), pack, NULL);
                monsters[seed].leader = boss;
                int minions = std::max(minMinions, extra);
                for (int index = 0; index < minions && spawned < budget; index++) {
                    Object* minion = arpg_spawn_clone(seed);
                    if (minion != NULL) {
                        spawned++;
                        arpg_make_monster(minion, RANK_MINION, 0, pack, boss);
                    }
                }
                ArpgMonster* info = arpg_monster(boss);
                char mods[160];
                int rank;
                char name[48];
                arpg_monster_info(boss, &rank, name, sizeof(name), mods, sizeof(mods));
                arpg_log("ARPG: unique %s (%s) [%s] with %d minions\n", name, critter_name(boss), mods, minions);
                (void)info;
            }
        } else if (roll <= uniqueChance + championChance) {
            unsigned int kind = champion_kinds[roll_random(0, (int)(sizeof(champion_kinds) / sizeof(*champion_kinds)) - 1)];
            int count = championCount;
            for (int index = 0; index < count && spawned < budget; index++) {
                Object* champion = arpg_spawn_clone(seed, false);
                if (champion != NULL) {
                    spawned++;
                    champions++;
                    arpg_make_monster(champion, RANK_CHAMPION, kind, pack, NULL);
                }
            }
        } else {
            for (int index = 0; index < extra && spawned < budget; index++) {
                Object* clone = arpg_spawn_clone(seed);
                if (clone != NULL) {
                    spawned++;
                    arpg_make_monster(clone, RANK_NORMAL, 0, pack, NULL);
                }
            }
        }
    }

    arpg_log("ARPG: populated level %d: %d seeds, %d spawned (%d champions, %d uniques, %d proto scripts removed)\n",
        key, (int)seeds.size(), spawned, champions, uniques, stat_clone_scripts);

    arpg_spawn_shrines(seeds);

    if (spawned > 0) {
        arpg_refresh_list();
    }
}

// Raider packs: on random encounter maps, a human who turns on the player
// calls in one or two armed copies of itself, sometimes a champion pack or a
// named boss. Only hostile groups grow; caravans and traders never do.
static void arpg_call_reinforcements(Object* seed)
{
    if (!raider_packs || map_get_index_number() != random_map_index) {
        return;
    }
    if (arpg_is_monster_kind(seed) || arpg_is_spawned(seed) || isPartyMember(seed) || critter_kill_exps(seed) >= 1500) {
        return;
    }
    if (std::find(reinforced.begin(), reinforced.end(), seed) != reinforced.end()) {
        return;
    }
    reinforced.push_back(seed);

    int level = stat_pc_get(PC_STAT_LEVEL);
    int levelPct = std::max(50, std::min(100, (level + 1) * 25));
    int count = roll_random(1, 2);
    if (roll_random(1, 100) > levelPct) {
        count--;
    }
    if (count <= 0) {
        return;
    }

    // The seed's weapon, for the copies.
    Object* weapon = inven_right_hand(seed);
    if (weapon == NULL || item_get_type(weapon) != ITEM_TYPE_WEAPON) {
        weapon = inven_left_hand(seed);
    }
    if (weapon != NULL && item_get_type(weapon) != ITEM_TYPE_WEAPON) {
        weapon = NULL;
    }

    ArpgMonster* existing = arpg_monster(seed);
    int pack = existing != NULL ? existing->pack : next_pack_id++;
    if (existing == NULL) {
        arpg_make_monster(seed, RANK_NORMAL, 0, pack, NULL);
    }

    int roll = roll_random(1, 100);
    int rank = RANK_NORMAL;
    unsigned int kind = 0;
    if (roll <= unique_pct * levelPct / 100) {
        rank = RANK_UNIQUE;
    } else if (roll <= (unique_pct + champion_pct) * levelPct / 100) {
        rank = RANK_CHAMPION;
        kind = champion_kinds[roll_random(0, (int)(sizeof(champion_kinds) / sizeof(*champion_kinds)) - 1)];
    }

    Object* boss = NULL;
    for (int index = 0; index < count; index++) {
        Object* clone = arpg_spawn_clone(seed, false);
        if (clone == NULL) {
            continue;
        }

        if (weapon != NULL) {
            Object* copy;
            if (obj_pid_new(&copy, weapon->pid) == 0) {
                if (item_add_force(clone, copy, 1) == 0) {
                    obj_disconnect(copy, NULL);
                    if (item_w_max_ammo(copy) > 0) {
                        item_w_set_curr_ammo(copy, item_w_max_ammo(copy));
                        int ammoPid = item_w_ammo_pid(copy);
                        Object* ammo;
                        if (ammoPid > 0 && obj_pid_new(&ammo, ammoPid) == 0) {
                            if (item_add_force(clone, ammo, 2) == 0) {
                                obj_disconnect(ammo, NULL);
                            }
                        }
                    }
                    inven_wield(clone, copy, 1);
                }
            }
        }

        if (rank == RANK_UNIQUE && boss == NULL) {
            boss = clone;
            arpg_make_monster(clone, RANK_UNIQUE, arpg_roll_affixes(2), pack, NULL);
        } else if (rank == RANK_UNIQUE) {
            arpg_make_monster(clone, RANK_MINION, 0, pack, boss);
        } else {
            arpg_make_monster(clone, rank, kind, pack, NULL);
        }

        critter_set_who_hit_me(clone, obj_dude);
        clone->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
        arpg_log("ARPG: %s calls in %s\n", critter_name(seed), arpg_label(clone));
    }
}

void arpg_note_random_map()
{
    random_map_pending = true;
}

void arpg_on_game_load()
{
    // A loaded game is another timeline: whatever was spawned before is gone
    // from it, so every level gets fresh packs.
    populated_hour.clear();
}

// D2 pack behaviour: when one member of a pack joins a fight, the rest of
// the pack within earshot joins too.
static void arpg_alert_pack(Object* critter, Object* target)
{
    ArpgMonster* monster = arpg_monster(critter);
    if (monster == NULL || monster->pack == 0) {
        return;
    }
    if (std::find(alerted_packs.begin(), alerted_packs.end(), monster->pack) != alerted_packs.end()) {
        return;
    }
    alerted_packs.push_back(monster->pack);

    int pack = monster->pack;
    int alerted = 0;
    for (int index = 0; index < crit_count; index++) {
        Object* member = crit_list[index];
        ArpgMonster* other = arpg_monster(member);
        if (other == NULL || other->pack != pack || member == critter || !arpg_alive(member)) {
            continue;
        }
        if (obj_dist(member, critter) > 15) {
            continue;
        }
        if (member->data.critter.combat.whoHitMe == NULL) {
            critter_set_who_hit_me(member, target);
        }
        member->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
        alerted++;
    }
    arpg_log("ARPG: pack %d alerted by %s: %d joined\n", pack, arpg_label(critter), alerted);
}

// D2 aggro: wild monsters attack whoever they can perceive nearby, script
// or no script (pack clones have none).
static void arpg_aggro_tick(unsigned int now)
{
    if (now < next_aggro_check) {
        return;
    }
    next_aggro_check = now + 400;

    if ((obj_dude->data.critter.combat.results & DAM_DEAD) != 0) {
        return;
    }

    for (int index = 0; index < crit_count; index++) {
        Object* critter = crit_list[index];
        if (critter == obj_dude || !arpg_alive(critter) || isPartyMember(critter)) {
            continue;
        }
        if (critter->data.critter.combat.whoHitMe != NULL || !arpg_is_monster_kind(critter)) {
            continue;
        }
        if (critter->data.critter.combat.team == obj_dude->data.critter.combat.team) {
            continue;
        }
        if (obj_dist(critter, obj_dude) > aggro_radius || !is_within_perception(critter, obj_dude)) {
            continue;
        }

        critter_set_who_hit_me(critter, obj_dude);
        critter->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
        alert_until = std::max(alert_until, now + 3000);
        arpg_alert_pack(critter, obj_dude);
    }
}

// ---------------------------------------------------------------------------
// Shrines (D2): glowing landmarks on monster levels; walk up to one for a
// refill, a timed boost, or a nasty surprise.
// ---------------------------------------------------------------------------

static bool arpg_buff(int kind)
{
    return buff_until[kind] != 0 && get_time() < buff_until[kind];
}

int arpg_buff_seconds(int index, const char** name)
{
    if (index < 0 || index >= SHRINE_COUNT) {
        return -1;
    }
    *name = shrine_info[index].hud;
    unsigned int now = get_time();
    if (buff_until[index] == 0 || now >= buff_until[index]) {
        return 0;
    }
    return (int)((buff_until[index] - now + 999) / 1000);
}

int arpg_shrine_state(Object* obj)
{
    for (const ArpgShrine& shrine : shrines) {
        if (shrine.obj == obj) {
            return shrine.used ? 0 : 1;
        }
    }
    return -1;
}

static void arpg_spawn_shrines(const std::vector<Object*>& seeds)
{
    if (!shrines_enabled || seeds.empty() || shrine_pid == -1) {
        return;
    }

    int count = seeds.size() >= 8 ? 2 : 1;
    for (int n = 0; n < count; n++) {
        Object* seed = seeds[roll_random(0, (int)seeds.size() - 1)];
        int tile = arpg_pack_tile(seed);
        if (tile == -1) {
            continue;
        }

        Object* obj;
        if (obj_pid_new(&obj, shrine_pid) != 0) {
            continue;
        }
        if (obj->sid != -1) {
            scr_remove(obj->sid);
            obj->sid = -1;
        }
        obj->flags |= OBJECT_NO_SAVE | ARPG_OBJECT_SPAWNED;
        Rect rect;
        obj_move_to_tile(obj, tile, seed->elevation, &rect);
        tile_refresh_rect(&rect, seed->elevation);
        obj_set_light(obj, 4, 65536, &rect);
        tile_refresh_rect(&rect, seed->elevation);

        int kind = roll_random(0, SHRINE_COUNT - 1);
        shrines.push_back({ obj, kind, false, false });
        arpg_log("ARPG: %s at %d\n", shrine_info[kind].name, tile);
    }
}

static void arpg_use_shrine(ArpgShrine& shrine, unsigned int now)
{
    shrine.used = true;
    Rect rect;
    obj_set_light(shrine.obj, 1, 16384, &rect);
    tile_refresh_rect(&rect, shrine.obj->elevation);

    char text[128];
    switch (shrine.kind) {
    case SHRINE_REFILL:
        critter_adjust_hits(obj_dude, stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS));
        energy = arpg_energy_max();
        intface_update_hit_points(false);
        snprintf(text, sizeof(text), "%s: life and energy restored.", shrine_info[shrine.kind].name);
        break;
    case SHRINE_MONSTER: {
        // The nearest wild monster becomes a unique.
        Object* best = NULL;
        for (int index = 0; index < crit_count; index++) {
            Object* critter = crit_list[index];
            ArpgMonster* monster = arpg_monster(critter);
            if (!arpg_alive(critter) || !arpg_is_monster_kind(critter) || (monster != NULL && monster->rank == RANK_UNIQUE)) {
                continue;
            }
            if (best == NULL || obj_dist(critter, obj_dude) < obj_dist(best, obj_dude)) {
                best = critter;
            }
        }
        if (best != NULL && obj_dist(best, obj_dude) <= 25) {
            ArpgMonster* old = arpg_monster(best);
            int pack = old != NULL ? old->pack : next_pack_id++;
            arpg_make_monster(best, RANK_UNIQUE, arpg_roll_affixes(2), pack, NULL);
            ArpgMonster* monster = arpg_monster(best);
            arpg_float_text(best, monster->name, colorTable[32256]);
            snprintf(text, sizeof(text), "%s: %s awakens.", shrine_info[shrine.kind].name, monster->name);
        } else {
            snprintf(text, sizeof(text), "%s: nothing stirs.", shrine_info[shrine.kind].name);
        }
        break;
    }
    default:
        buff_until[shrine.kind] = now + shrine_info[shrine.kind].seconds * 1000;
        if (shrine.kind == SHRINE_PURIFY) {
            critter_adjust_rads(obj_dude, -critter_get_rads(obj_dude));
            critter_adjust_poison(obj_dude, -critter_get_poison(obj_dude));
        }
        static const char* effects[SHRINE_COUNT] = { "", "half damage taken", "double damage", "fast energy",
            "+50% experience", "no radiation or poison", "" };
        snprintf(text, sizeof(text), "%s: %s for %d seconds.", shrine_info[shrine.kind].name, effects[shrine.kind], shrine_info[shrine.kind].seconds);
        break;
    }

    arpg_float_text(obj_dude, shrine_info[shrine.kind].name, colorTable[992]);
    arpg_print(text);
    gsound_play_sfx_file("ib1p1xx1");
    arpg_log("ARPG: used %s\n", shrine_info[shrine.kind].name);
}

static void arpg_shrine_tick(unsigned int now)
{
    static unsigned int next = 0;
    if (now < next) {
        return;
    }
    next = now + 200;

    for (ArpgShrine& shrine : shrines) {
        if (shrine.used || shrine.obj->tile == -1 || shrine.obj->elevation != obj_dude->elevation) {
            continue;
        }
        int dist = tile_dist(shrine.obj->tile, obj_dude->tile);
        if (!shrine.announced && dist <= 8) {
            // D2 shows a shrine's name when you notice it.
            shrine.announced = true;
            arpg_float_text(shrine.obj, shrine_info[shrine.kind].name, colorTable[992]);
        }
        if (dist <= 1) {
            arpg_use_shrine(shrine, now);
        }
    }

    if (arpg_buff(SHRINE_PURIFY)) {
        int rads = critter_get_rads(obj_dude);
        if (rads > 0) {
            critter_adjust_rads(obj_dude, -rads);
        }
        int poison = critter_get_poison(obj_dude);
        if (poison > 0) {
            critter_adjust_poison(obj_dude, -poison);
        }
    }
}

// Pack leader aura (D2 Might): pack members near a living Pack Leader hit
// harder.
static int arpg_aura_pct(Object* critter)
{
    ArpgMonster* monster = arpg_monster(critter);
    if (monster == NULL || monster->leader == NULL || monster->leader == critter) {
        return 0;
    }
    ArpgMonster* leader = arpg_monster(monster->leader);
    if (leader == NULL || (leader->affixes & AFFIX_LEADER) == 0 || !arpg_alive(monster->leader)) {
        return 0;
    }
    return obj_dist(critter, monster->leader) <= 8 ? 50 : 0;
}

// Champion/unique damage and life bonuses, applied before deaths are
// decided. Extra life is a damage divisor so the shared proto is untouched.
static void arpg_scale_damage(Attack* attack)
{
    ArpgMonster* attacker = arpg_monster(attack->attacker);
    int bonus = attacker != NULL ? attacker->dmgPct + arpg_aura_pct(attack->attacker) : 0;
    bool fire = attacker != NULL && (attacker->affixes & AFFIX_FIRE) != 0;

    auto scale = [&](Object* victim, int* damage) {
        if (*damage <= 0 || victim == NULL || FID_TYPE(victim->fid) != OBJ_TYPE_CRITTER) {
            return;
        }
        double value = *damage * (100 + bonus) / 100.0;
        if (fire) {
            value += roll_random(2, 6);
        }

        // Magic weapon and armor affixes.
        Object* weapon = attack->attacker == obj_dude ? attack->weapon : NULL;
        if (weapon != NULL && arpg_item_quality(weapon) > 0) {
            if (arpg_item_has_prefix(weapon, WPRE_VICIOUS)) {
                value *= 1.25;
            }
            if (arpg_item_has_prefix(weapon, WPRE_BRUTAL)) {
                value *= 1.5;
            }
            if (arpg_item_has_prefix(weapon, WPRE_BURNING)) {
                value += roll_random(2, 6);
            }
            if (arpg_item_has_prefix(weapon, WPRE_SHOCKING)) {
                value += roll_random(3, 8);
            }
            if (arpg_item_has_prefix(weapon, WPRE_DEADLY) && roll_random(1, 100) <= 15) {
                value *= 2.0;
            }
            if (arpg_item_has_suffix(weapon, WSUF_SLAYING)) {
                ArpgMonster* target = arpg_monster(victim);
                if (target != NULL && target->rank >= RANK_CHAMPION) {
                    value *= 1.4;
                }
            }
            if (arpg_item_has_suffix(weapon, WSUF_POWER)) {
                value *= 1.15;
            }
            // D2 crushing blow: a quarter of the target's current life (an
            // eighth for champions and uniques).
            if (arpg_item_has_power(weapon, UPOW_CRUSHING) && roll_random(1, 100) <= 20) {
                ArpgMonster* target = arpg_monster(victim);
                int divisor = target != NULL && target->rank >= RANK_CHAMPION ? 8 : 4;
                value += critter_get_hits(victim) / divisor;
                arpg_float_text(victim, "crush", colorTable[32392]);
                if (arpg_test) {
                    arpg_log("ARPG: crushing blow on %s (hp %d)\n", critter_name(victim), critter_get_hits(victim));
                }
            }
            if (arpg_item_has_power(weapon, UPOW_CHILL)) {
                value += roll_random(3, 9);
                unsigned int& until = chill_until[victim];
                if (get_time() >= until) {
                    arpg_float_text(victim, "chill", colorTable[12959]);
                    if (arpg_test) {
                        arpg_log("ARPG: chilled %s\n", critter_name(victim));
                    }
                }
                until = get_time() + 3000;
            }
        }
        if (victim == obj_dude) {
            Object* armor = arpg_dude_armor();
            if (arpg_item_has_prefix(armor, APRE_STURDY)) {
                value *= 0.9;
            }
            if (arpg_item_has_prefix(armor, APRE_REINFORCED)) {
                value *= 0.8;
            }
        }
        // Shrine boosts.
        if (attack->attacker == obj_dude && arpg_buff(SHRINE_COMBAT)) {
            value *= 2.0;
        }
        if (victim == obj_dude && arpg_buff(SHRINE_ARMOR)) {
            value *= 0.5;
        }
        // Followers (D2 mercenaries) would melt in pack fights.
        if (victim != obj_dude && isPartyMember(victim)) {
            value = value * party_damage_taken_pct / 100.0;
        }
        ArpgMonster* defender = arpg_monster(victim);
        if (defender != NULL) {
            value = value * 100.0 / (100 + defender->hpPct);
            if ((defender->affixes & AFFIX_ARMORED) != 0) {
                value = value * 0.75;
            }
            if ((defender->affixes & CHAMP_BERSERKER) != 0) {
                value = value * 1.25;
            }
        }
        // Round randomly so small Fallout damage numbers don't all collapse
        // to 1 against tough monsters (2.6 is 3 six times in ten).
        int whole = (int)value;
        if (roll_random(1, 1000) <= (int)((value - whole) * 1000)) {
            whole++;
        }
        *damage = std::max(1, whole);
    };

    if ((attack->attackerFlags & DAM_HIT) != 0) {
        scale(attack->defender, &(attack->defenderDamage));
    }
    for (int index = 0; index < attack->extrasLength; index++) {
        scale(attack->extras[index], &(attack->extrasDamage[index]));
    }
}

// Art of the weapon's projectile, or -1 (bullets aren't drawn).
static int arpg_projectile_fid(Object* weapon)
{
    Proto* projectileProto;
    if (proto_ptr(item_w_proj_pid(weapon), &projectileProto) == -1) {
        return -1;
    }
    return projectileProto->fid;
}

// Creates the (hidden) bullet of a shot next to the shooter, facing the
// defender. NULL if the weapon's projectile has no art.
static Object* arpg_new_projectile(Attack* attack)
{
    Object* attacker = attack->attacker;
    Object* defender = attack->defender;

    int fid = arpg_projectile_fid(attack->weapon);
    if (fid == -1) {
        return NULL;
    }

    Object* projectile;
    if (obj_new(&projectile, fid, -1) == -1) {
        return NULL;
    }

    obj_turn_off(projectile, NULL);
    obj_set_light(projectile, 9, projectile->lightIntensity, NULL);

    int origin = combat_bullet_start(attacker, defender);
    obj_move_to_tile(projectile, origin, attacker->elevation, NULL);
    obj_set_rotation(projectile, tile_dir(attacker->tile, defender->tile), NULL);
    return projectile;
}

// Sends a fired shot's bullet on its way in a sequence of its own, from
// where the shooter stands now. The hit is queued when it arrives.
static bool arpg_launch_projectile(ArpgAttack* aa)
{
    Attack* attack = &(aa->attack);
    Object* defender = attack->defender;

    Object* projectile = arpg_new_projectile(attack);
    if (projectile == NULL) {
        return false;
    }

    if (register_begin(ANIMATION_REQUEST_RESERVED) == -1) {
        obj_erase_object(projectile, NULL);
        return false;
    }

    register_object_funset(projectile, OBJECT_HIDDEN, 0);

    const char* flyingSfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_AMMO_FLYING, attack->weapon, attack->hitMode, defender);
    register_object_play_sfx(projectile, flyingSfx, 0);

    int destination = (attack->attackerFlags & DAM_HIT) != 0 ? defender->tile : attack->tile;
    register_object_move_straight_to_tile(projectile, destination, defender->elevation, ANIM_WALK, 0);
    register_object_call(aa, NULL, arpg_cb_hit, -1);
    register_object_must_erase(projectile);
    register_object_must_call(aa, NULL, arpg_cb_release, -1);

    // The sequence's forced release can run inside register_end.
    aa->refs += 1;
    if (register_end() == -1) {
        aa->refs -= 1;
        obj_erase_object(projectile, NULL);
        return false;
    }

    return true;
}

// Starts sliding the player along the WASD direction for the shot that was
// just registered. A run cut short keeps its progress toward the next tile
// (`carryX`, `carryY`), so the player doesn't jump back.
static void arpg_slide_begin(int carryX, int carryY)
{
    slide_active = true;
    slide_start = get_time();
    slide_last = slide_start;
    slide_sx = carryX;
    slide_sy = carryY;
    slide_px = 0;
    slide_py = 0;
    slide_hops = 0;
    slide_blocked_ticks = 0;
    slide_around = -1;
    // The run is re-issued toward a fresh tile once the shot is out.
    wasd_target_tile = -1;
}

// Builds and starts the attacker's animation sequence. Returns false if the
// sequence could not be registered.
static bool arpg_sequence_attack(ArpgAttack* aa)
{
    Attack* attack = &(aa->attack);
    Object* attacker = attack->attacker;
    Object* defender = attack->defender;
    Object* weapon = attack->weapon;
    int anim = aa->anim;

    bool ranged = anim >= ANIM_FIRE_SINGLE || anim == ANIM_THROW_ANIM;

    // The player's bullets fly on their own, so the shooter doesn't stand
    // there until they land. With WASD held the player also keeps moving and
    // skips lowering the gun; the run picks up again right after the shot.
    bool detached = move_shot && attacker == obj_dude && ranged && weapon != NULL && anim != ANIM_THROW_ANIM;
    bool moving = move_shot && attacker == obj_dude && ranged && wasd_moving;
    int carryX = 0;
    int carryY = 0;
    if (moving && (FID_ANIM_TYPE(attacker->fid) == ANIM_RUNNING || FID_ANIM_TYPE(attacker->fid) == ANIM_WALK)) {
        // A run's offset is its progress since the last tile.
        carryX = attacker->x;
        carryY = attacker->y;
    }

    int fid = art_id(OBJ_TYPE_CRITTER, attacker->fid & 0xFFF, anim, (attacker->fid & 0xF000) >> 12, attacker->rotation + 1);
    CacheEntry* artHandle;
    Art* art = art_ptr_lock(fid, &artHandle);
    int actionFrame = art != NULL ? art_frame_action_frame(art) : 0;
    if (art != NULL) {
        art_ptr_unlock(artHandle);
    }

    char attackSfx[16];
    const char* sfx;
    if (ranged && ((attacker->fid & 0xF000) >> 12) == 0) {
        sfx = gsnd_build_character_sfx_name(attacker, anim, CHARACTER_SOUND_EFFECT_UNUSED);
    } else if (!ranged && (anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG)) {
        sfx = gsnd_build_character_sfx_name(attacker, anim, CHARACTER_SOUND_EFFECT_UNUSED);
    } else {
        sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_ATTACK, weapon, attack->hitMode, defender);
    }
    if (sfx == NULL) {
        sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_ATTACK, weapon, attack->hitMode, defender);
    }
    strncpy(attackSfx, sfx != NULL ? sfx : "", sizeof(attackSfx) - 1);
    attackSfx[sizeof(attackSfx) - 1] = '\0';

    Object* projectile = NULL;

    register_clear(attacker);

    if (register_begin(ANIMATION_REQUEST_RESERVED) == -1) {
        return false;
    }

    register_object_turn_towards(attacker, defender->tile);

    if (ranged && anim != ANIM_THROW_ANIM) {
        register_object_animate(attacker, ANIM_POINT, -1);
    }

    register_object_play_sfx(attacker, attackSfx, -1);
    register_object_animate(attacker, anim, 0);
    register_object_call(aa, NULL, arpg_cb_fired, actionFrame);

    if (detached) {
        if (!moving) {
            register_object_animate(attacker, ANIM_UNPOINT, -1);
        }
        register_object_must_call(aa, NULL, arpg_cb_shooter_done, -1);

        aa->detached = true;
        if (register_end() == -1) {
            aa->detached = false;
            return false;
        }

        if (moving) {
            arpg_slide_begin(carryX, carryY);
            stat_moving_shots++;
        }
        return true;
    }

    if (ranged && weapon != NULL && anim != ANIM_THROW_ANIM) {
        projectile = arpg_new_projectile(attack);
        if (projectile != NULL) {
            register_object_funset(projectile, OBJECT_HIDDEN, 0);

            const char* flyingSfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_AMMO_FLYING, weapon, attack->hitMode, defender);
            register_object_play_sfx(projectile, flyingSfx, 0);

            int destination = (attack->attackerFlags & DAM_HIT) != 0 ? defender->tile : attack->tile;
            register_object_move_straight_to_tile(projectile, destination, defender->elevation, ANIM_WALK, 0);
            register_object_call(aa, NULL, arpg_cb_hit, -1);
            register_object_must_erase(projectile);
        } else {
            register_object_call(aa, NULL, arpg_cb_hit, 0);
        }

        register_object_animate(attacker, ANIM_UNPOINT, -1);
    } else {
        register_object_call(aa, NULL, arpg_cb_hit, 0);
    }

    register_object_must_call(aa, NULL, arpg_cb_release, -1);

    if (register_end() == -1) {
        if (projectile != NULL) {
            obj_erase_object(projectile, NULL);
        }
        return false;
    }

    if (moving) {
        // Thrown weapons: no bullet, but the thrower keeps moving too.
        arpg_slide_begin(carryX, carryY);
        stat_moving_shots++;
    }

    return true;
}

// Drawing a weapon is an animation sequence (ANIM_TAKE_OUT) that sets the
// weapon art when it ends. Running cancels it, leaving a critter that holds a
// gun but has no art for shooting it. Put the right weapon art back.
static void arpg_sync_weapon_art(Object* critter, int hitMode)
{
    Object* weapon = item_hit_with(critter, hitMode);
    int code = weapon != NULL && item_get_type(weapon) == ITEM_TYPE_WEAPON ? item_w_anim_code(weapon) : 0;
    if (((critter->fid & 0xF000) >> 12) == code) {
        return;
    }

    int fid = art_id(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_STAND, code, critter->rotation + 1);
    if (!art_exists(fid)) {
        return;
    }

    Rect rect;
    obj_change_fid(critter, fid, &rect);
    tile_refresh_rect(&rect, critter->elevation);
    arpg_log("ARPG: restored weapon art on %s (code %d)\n", critter_name(critter), code);
}

static bool arpg_start_attack(Object* attacker, Object* defender, int hitMode, int hitLocation = HIT_LOCATION_TORSO)
{
    if (!anim_busy(attacker) || FID_ANIM_TYPE(attacker->fid) == ANIM_WALK || FID_ANIM_TYPE(attacker->fid) == ANIM_RUNNING) {
        arpg_sync_weapon_art(attacker, hitMode);
    }

    if (hitMode == HIT_MODE_PUNCH && roll_random(1, 4) == 1) {
        int fid = art_id(OBJ_TYPE_CRITTER, attacker->fid & 0xFFF, ANIM_KICK_LEG, (attacker->fid & 0xF000) >> 12, (attacker->fid & 0x70000000) >> 28);
        if (art_exists(fid)) {
            hitMode = HIT_MODE_KICK;
        }
    }

    ArpgAttack* aa = new ArpgAttack();
    memset(&(aa->attack), 0, sizeof(aa->attack));
    combat_ctd_init(&(aa->attack), attacker, defender, hitMode, hitLocation);

    if (combat_compute_attack(&(aa->attack)) == -1) {
        delete aa;
        return false;
    }

    aa->anim = item_w_anim(attacker, aa->attack.hitMode);
    aa->target = defender;
    aa->refs = 1;
    aa->fired = false;
    aa->hitQueued = false;
    aa->invalid = false;
    aa->detached = false;
    aa->launchPending = false;
    live_attacks.push_back(aa);

    stat_attacks++;

    if (arpg_test) {
        // Once per attacker kind: frame counts and offsets of its attacks.
        static std::vector<int> logged;
        int key = attacker->fid & 0xF0FFF;
        if (std::find(logged.begin(), logged.end(), key) == logged.end()) {
            logged.push_back(key);
            arpg_log_anim_info(attacker);
        }
    }

    if (arpg_test && attacker == obj_dude) {
        int fid = art_id(OBJ_TYPE_CRITTER, attacker->fid & 0xFFF, aa->anim, (attacker->fid & 0xF000) >> 12, attacker->rotation + 1);
        if (!art_exists(fid)) {
            arpg_log("ARPG: dude attack anim %d has no art (code %d, fid anim %d)\n", aa->anim, (attacker->fid & 0xF000) >> 12, FID_ANIM_TYPE(attacker->fid));
        }
    }

    if (!arpg_sequence_attack(aa)) {
        stat_attack_fail++;
        arpg_log("ARPG: attack sequence failed %s -> %s\n", critter_name(attacker), critter_name(defender));
        live_attacks.erase(std::remove(live_attacks.begin(), live_attacks.end(), aa), live_attacks.end());
        delete aa;
        return false;
    }

    // Spend ammo when the shot is taken.
    Object* weapon = aa->attack.weapon;
    if (weapon != NULL && item_w_max_ammo(weapon) > 0) {
        int ammo = item_w_curr_ammo(weapon) - aa->attack.ammoQuantity;
        item_w_set_curr_ammo(weapon, ammo < 0 ? 0 : ammo);
        if (attacker == obj_dude) {
            intface_update_ammo_lights();
        }
    }

    if (attacker == obj_dude) {
        critter_set_who_hit_me(obj_dude, defender);
    }

    // Shots fired make noise.
    alert_until = get_time() + 10000;

    return true;
}

// Plays hit reaction / death animation for one victim, D2 style: small hits
// don't interrupt, big hits (>= stagger_pct of max HP), crits, knockdowns and
// deaths do.
static void arpg_react(Attack* attack, Object* victim, int damage, int flags, int knockback)
{
    if (victim == NULL || FID_TYPE(victim->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    if ((victim->data.critter.combat.results & DAM_DEAD) != 0) {
        return;
    }

    int maxHp = stat_level(victim, STAT_MAXIMUM_HIT_POINTS);
    bool dies = (flags & DAM_DEAD) != 0;
    bool forced = dies
        || (flags & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) != 0
        || knockback > 0;
    int pct = victim == obj_dude ? player_stagger_pct : stagger_pct;
    bool big = forced
        || (flags & (DAM_CRITICAL | DAM_ON_FIRE)) != 0
        || damage * 100 >= maxHp * pct;

    if (!big || damage <= 0) {
        return;
    }

    // Hit recovery: after a stagger, a victim can't be staggered again for a
    // moment, so a stream of hits can't lock it out of acting.
    if (!forced) {
        unsigned int now = get_time();
        unsigned int& until = stagger_until[victim];
        if (now < until) {
            return;
        }
        until = now + stagger_immunity_ms;
    }

    int rc = dies ? register_clear_force(victim) : register_clear(victim);
    if (rc == -2) {
        stat_react_blocked++;
        arpg_log("ARPG: reaction blocked on %s\n", critter_name(victim));
        return;
    }

    bool front = is_hit_from_front(attack->attacker, victim);
    register_begin(ANIMATION_REQUEST_RESERVED);
    show_damage_to_object(victim,
        damage,
        flags,
        attack->weapon,
        front,
        knockback,
        tile_dir(attack->attacker->tile, victim->tile),
        item_w_anim(attack->attacker, attack->hitMode),
        attack->attacker,
        0);
    register_end();
}

// D2 death. Called with the player's share of an attack after deaths were
// decided: a killing blow becomes a knockdown leaving 1 HP and returns true
// (the caller then calls arpg_dude_downed). While downed, nothing hurts.
static bool arpg_spare_dude(int* damage, int* flags, int* knockback)
{
    if (!respawn_enabled) {
        return false;
    }

    if (death_until != 0) {
        *damage = 0;
        *flags = 0;
        *knockback = 0;
        return false;
    }

    if ((*flags & (DAM_DEAD | DAM_KNOCKED_OUT)) == 0) {
        return false;
    }

    *damage = std::max(0, critter_get_hits(obj_dude) - 1);
    *flags = (*flags & ~(DAM_DEAD | DAM_KNOCKED_OUT)) | DAM_KNOCKED_DOWN;
    return true;
}

// The player went down: monsters lose interest, input stops, and
// arpg_respawn() runs when death_until passes.
static void arpg_dude_downed(Object* killer)
{
    unsigned int now = get_time();
    death_until = now + 3000;
    stat_deaths++;

    player_target = NULL;
    player_chasing = false;
    wasd_moving = false;

    // At 1 HP the next poison tick would kill for real (Fallout's own death,
    // game over), so the poison goes now.
    int poison = critter_get_poison(obj_dude);
    if (poison > 0) {
        critter_adjust_poison(obj_dude, -poison);
    }

    for (int index = 0; index < crit_count; index++) {
        Object* critter = crit_list[index];
        if (critter != obj_dude && critter->data.critter.combat.whoHitMe == obj_dude) {
            critter_set_who_hit_me(critter, NULL);
            critter->data.critter.combat.maneuver &= ~CRITTER_MANEUVER_ENGAGING;
        }
    }
    for (auto& entry : actors) {
        if (entry.second.target == obj_dude) {
            entry.second.target = NULL;
        }
    }

    arpg_float_text(obj_dude, "You have died", colorTable[31744]);
    arpg_print("You have died. You will rise again at the entrance.");
    game_ui_disable(1);
    arpg_log("ARPG: player downed by %s\n", killer != NULL && FID_TYPE(killer->fid) == OBJ_TYPE_CRITTER ? arpg_label(killer) : "?");
}

static void arpg_respawn()
{
    death_until = 0;

    register_clear_force(obj_dude);
    obj_dude->data.critter.combat.results &= ~(DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_DEAD);
    dude_stand(obj_dude, obj_dude->rotation, -1);

    int tile = map_data.enteringTile;
    int elevation = map_data.enteringElevation;
    if (tile > 0 && tile < HEX_GRID_SIZE && elevation >= 0 && elevation < ELEVATION_COUNT) {
        Rect rect;
        obj_move_to_tile(obj_dude, tile, elevation, &rect);
        if (elevation != map_elevation) {
            map_set_elevation(elevation);
        }
        obj_set_rotation(obj_dude, map_data.enteringRotation, NULL);
    }
    tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW);

    critter_adjust_hits(obj_dude, stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS));
    int poison = critter_get_poison(obj_dude);
    if (poison > 0) {
        critter_adjust_poison(obj_dude, -poison);
    }
    energy = arpg_energy_max();

    int caps = item_caps_total(obj_dude);
    int lost = caps * respawn_caps_pct / 100;
    if (lost > 0) {
        item_caps_adjust(obj_dude, -lost);
    }

    game_ui_enable();
    intface_update_hit_points(false);
    intface_update_items(false);

    // A moment of grace: the entrance can be inside the fight (encounter
    // maps). Dodge i-frames, and monsters notice the player a bit later.
    unsigned int now = get_time();
    dodge_until = now + 2500;
    next_aggro_check = now + 2500;

    char text[128];
    snprintf(text, sizeof(text), "You rise again. You lost %d caps.", lost);
    arpg_print(text);
    arpg_log("ARPG: player respawned at %d (lost %d caps)\n", obj_dude->tile, lost);
}

// Fire Enchanted death: Fallout's explosion art and sound, with damage to
// the player and party next to the corpse. Not action_explode(): that one
// disables the interface and kills outside our damage path.
static void arpg_death_explosion(Object* victim)
{
    Object* boom;
    int fid = art_id(OBJ_TYPE_MISC, 10, 0, 0, 0);
    if (obj_new(&boom, fid, -1) == 0) {
        boom->flags |= OBJECT_NO_SAVE;
        obj_turn_off(boom, NULL);
        obj_move_to_tile(boom, victim->tile, victim->elevation, NULL);
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_play_sfx(boom, "whn1xxx1", 0);
        register_object_funset(boom, OBJECT_HIDDEN, 0);
        register_object_animate_and_hide(boom, ANIM_STAND, 0);
        register_object_must_erase(boom);
        if (register_end() == -1) {
            obj_erase_object(boom, NULL);
        }
    }

    arpg_log("ARPG: %s explodes\n", critter_name(victim));

    for (int index = 0; index < crit_count; index++) {
        Object* critter = crit_list[index];
        if (critter != obj_dude && !isPartyMember(critter)) {
            continue;
        }
        if (!arpg_alive(critter) || critter->elevation != victim->elevation || obj_dist(critter, victim) > 2) {
            continue;
        }
        if (critter == obj_dude && get_time() < dodge_until) {
            continue;
        }

        Attack blast;
        memset(&blast, 0, sizeof(blast));
        combat_ctd_init(&blast, victim, critter, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
        blast.attackerFlags = DAM_HIT;
        blast.defenderDamage = roll_random(6, 14);
        death_checks(&blast);
        bool downed = critter == obj_dude && arpg_spare_dude(&(blast.defenderDamage), &(blast.defenderFlags), &(blast.defenderKnockback));
        arpg_react(&blast, critter, blast.defenderDamage, blast.defenderFlags, 0);
        apply_damage(&blast, false);
        if (downed) {
            arpg_dude_downed(victim);
        }

        char text[16];
        snprintf(text, sizeof(text), "%d", blast.defenderDamage);
        arpg_float_text(critter, text, colorTable[31744]);
        arpg_log("ARPG: explosion hits %s for %d\n", critter_name(critter), blast.defenderDamage);
    }
}

// Unique modifiers that trigger when a monster hits the player.
static void arpg_on_hit_effects(Object* attacker, int damage)
{
    ArpgMonster* monster = arpg_monster(attacker);
    if (monster == NULL || monster->affixes == 0) {
        return;
    }

    if ((monster->affixes & AFFIX_RADIOACTIVE) != 0) {
        critter_adjust_rads(obj_dude, roll_random(4, 12));
    }
    if ((monster->affixes & AFFIX_VENOM) != 0) {
        critter_adjust_poison(obj_dude, roll_random(2, 6));
    }
    if ((monster->affixes & (AFFIX_DRAIN | CHAMP_GHOSTLY)) != 0) {
        energy = std::max(0.0, energy - (15 + damage));
    }
    if ((monster->affixes & AFFIX_VAMPIRIC) != 0 && arpg_alive(attacker)) {
        critter_adjust_hits(attacker, std::max(1, damage / 2));
    }
}

// A monster died to anything: rewards (D2: champions 3x and uniques 5x XP,
// extra item picks), Fire Enchanted explosions, lights out.
static void arpg_on_kill(Object* victim, Object* killer)
{
    stat_kills++;

    ArpgMonster* monster = arpg_monster(victim);
    bool byPlayer = killer == obj_dude || isPartyMember(killer);

    int rolls = 1;
    bool gear = false;
    if (monster != NULL) {
        if (monster->rank == RANK_CHAMPION || monster->rank == RANK_UNIQUE) {
            Rect rect;
            obj_set_light(victim, 0, 0, &rect);
            tile_refresh_rect(&rect, victim->elevation);
        }

        if (monster->rank == RANK_CHAMPION) {
            rolls = 2;
        } else if (monster->rank == RANK_UNIQUE) {
            rolls = 4;
            gear = true;
            arpg_log("ARPG: unique %s slain\n", monster->name);
        }

        if (byPlayer && monster->xpPct > 100) {
            int bonus = critter_kill_exps(victim) * (monster->xpPct - 100) / 100;
            if (bonus > 0) {
                combat_give_exps(bonus);
            }
        }

        if ((monster->affixes & AFFIX_FIRE) != 0) {
            arpg_death_explosion(victim);
        }
    }

    if (byPlayer && arpg_buff(SHRINE_EXPERIENCE)) {
        int bonus = critter_kill_exps(victim) / 2;
        if (bonus > 0) {
            combat_give_exps(bonus);
        }
    }

    if (byPlayer && !isPartyMember(victim)) {
        arpg_drop_loot(victim, rolls, gear);
    }
}

static void arpg_resolve(ArpgAttack* aa)
{
    prof_resolve_count++;
    Attack* attack = &(aa->attack);
    Object* attacker = attack->attacker;
    Object* defender = attack->defender;

    if (attacker == NULL || defender == NULL) {
        return;
    }

    // Who was already dead before this hit (two shots in flight at one
    // target must not kill, loot or reward twice).
    std::vector<Object*> victims;
    std::vector<bool> wasDead;
    auto noteVictim = [&](Object* victim) {
        if (victim != NULL && FID_TYPE(victim->fid) == OBJ_TYPE_CRITTER) {
            victims.push_back(victim);
            wasDead.push_back((victim->data.critter.combat.results & DAM_DEAD) != 0);
        }
    };
    noteVictim(defender);
    for (int index = 0; index < attack->extrasLength; index++) {
        noteVictim(attack->extras[index]);
    }

    // Champion / unique damage and life bonuses.
    arpg_scale_damage(attack);

    // HP may have changed since the roll; recompute deaths.
    attack->defenderFlags &= ~DAM_DEAD;
    attack->attackerFlags &= ~DAM_DEAD;
    for (int index = 0; index < attack->extrasLength; index++) {
        attack->extrasFlags[index] &= ~DAM_DEAD;
    }
    death_checks(attack);

    bool hit = (attack->attackerFlags & DAM_HIT) != 0;

    // Armor "of Evasion": one hit in ten misses.
    if (hit && defender == obj_dude && arpg_dude_has(false, 0, ASUF_EVASION) && roll_random(1, 100) <= 10) {
        attack->attackerFlags &= ~(DAM_HIT | DAM_CRITICAL);
        attack->defenderFlags = 0;
        attack->defenderDamage = 0;
        attack->defenderKnockback = 0;
        hit = false;
        arpg_float_text(obj_dude, "evade", colorTable[21140]);
    }

    if (hit && defender == obj_dude && get_time() < dodge_until) {
        // Dodge roll i-frames: the blow lands on empty air.
        attack->attackerFlags &= ~(DAM_HIT | DAM_CRITICAL);
        attack->defenderFlags = 0;
        attack->defenderDamage = 0;
        attack->defenderKnockback = 0;
        hit = false;
        arpg_log("ARPG: dodged %s\n", critter_name(attacker));
    }

    // D2 death: a killing blow on the player knocks them down instead; they
    // rise again at the map entrance (arpg_respawn). A downed player takes
    // no more damage.
    bool dudeDowned = false;
    if (hit && defender == obj_dude) {
        dudeDowned |= arpg_spare_dude(&(attack->defenderDamage), &(attack->defenderFlags), &(attack->defenderKnockback));
    }
    for (int index = 0; index < attack->extrasLength; index++) {
        if (attack->extras[index] == obj_dude) {
            dudeDowned |= arpg_spare_dude(&(attack->extrasDamage[index]), &(attack->extrasFlags[index]), &(attack->extrasKnockback[index]));
        }
    }

    if (hit) {
        const char* sfx;
        if (aa->anim == ANIM_THROW_PUNCH || aa->anim == ANIM_KICK_LEG) {
            sfx = gsnd_build_character_sfx_name(attacker, aa->anim, CHARACTER_SOUND_EFFECT_CONTACT);
        } else {
            sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_HIT, attack->weapon, attack->hitMode, defender);
        }
        gsound_play_sfx_file(sfx);
    }

    bool involvesPlayer = attacker == obj_dude || defender == obj_dude || isPartyMember(attacker) || isPartyMember(defender);
    if (involvesPlayer) {
        double profStart = arpg_now_ms();
        combat_display(attack);
        prof_resolve_ms[0] += arpg_now_ms() - profStart;
    }

    // D2 knockback (UPOW_KNOCKBACK uniques): two hexes, unless the target is
    // too big to move (Fallout's own rule for multi-hex critters).
    if (hit && attacker == obj_dude && arpg_item_has_power(attack->weapon, UPOW_KNOCKBACK)
        && FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER && (defender->flags & OBJECT_MULTIHEX) == 0) {
        attack->defenderKnockback = std::max(attack->defenderKnockback, 2);
        if (arpg_test) {
            arpg_log("ARPG: knockback on %s from tile %d\n", critter_name(defender), defender->tile);
        }
    }

    double profReact = arpg_now_ms();
    if (hit) {
        arpg_react(attack, defender, attack->defenderDamage, attack->defenderFlags, attack->defenderKnockback);
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        arpg_react(attack, attack->extras[index], attack->extrasDamage[index], attack->extrasFlags[index], attack->extrasKnockback[index]);
    }

    prof_resolve_ms[1] += arpg_now_ms() - profReact;
    double profApply = arpg_now_ms();
    // Not animated: Fallout's rolling HP counter blocks the game loop for a
    // few hundred milliseconds per hit, which a swarm turns into a slideshow.
    apply_damage(attack, false);
    prof_resolve_ms[2] += arpg_now_ms() - profApply;

    arpg_log("ARPG: %s -> %s hit=%d dmg=%d hp=%d dead=%d\n",
        arpg_label(attacker),
        FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER ? arpg_label(defender) : "obj",
        hit ? 1 : 0,
        attack->defenderDamage,
        FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER ? critter_get_hits(defender) : 0,
        FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER ? ((defender->data.critter.combat.results & DAM_DEAD) != 0) : 0);

    if (FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER) {
        char text[32];
        if (hit && attack->defenderDamage > 0) {
            bool crit = (attack->defenderFlags & DAM_CRITICAL) != 0 || (attack->attackerFlags & DAM_CRITICAL) != 0;
            snprintf(text, sizeof(text), crit ? "%d!" : "%d", attack->defenderDamage);
            int color = defender == obj_dude ? colorTable[31744] : (crit ? colorTable[32256] : colorTable[32767]);
            double profFloat = arpg_now_ms();
            arpg_float_text(defender, text, color);
            prof_resolve_ms[3] += arpg_now_ms() - profFloat;
        } else if (!hit && involvesPlayer) {
            arpg_float_text(defender, "miss", colorTable[21140]);
        }
    } else if (aa->target != defender && aa->target != NULL && arpg_alive(aa->target)
        && (attacker == obj_dude || aa->target == obj_dude)) {
        // The shot went wide and hit the scenery.
        arpg_float_text(aa->target, "miss", colorTable[21140]);
    }

    if (defender != obj_dude && FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER) {
        if ((defender->data.critter.combat.results & DAM_DEAD) == 0) {
            defender->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
            // Shooting one member of a pack brings the whole pack.
            if (attacker == obj_dude || isPartyMember(attacker)) {
                arpg_alert_pack(defender, attacker);
            }
        }
    }

    // Monster modifiers that trigger on a hit on the player.
    if (hit && defender == obj_dude && attack->defenderDamage > 0) {
        arpg_on_hit_effects(attacker, attack->defenderDamage);
    }

    // Life stealing weapons (Vampiric, of the Leech).
    if (hit && attacker == obj_dude && attack->defenderDamage > 0 && attack->weapon != NULL) {
        int pct = (arpg_item_has_prefix(attack->weapon, WPRE_VAMPIRIC) ? 10 : 0)
            + (arpg_item_has_suffix(attack->weapon, WSUF_LEECH) ? 15 : 0);
        if (pct > 0) {
            int heal = attack->defenderDamage * pct / 100;
            if (heal == 0 && roll_random(1, 100) <= attack->defenderDamage * pct) {
                heal = 1;
            }
            if (heal > 0 && critter_get_hits(obj_dude) < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS)) {
                critter_adjust_hits(obj_dude, heal);
                intface_update_hit_points(false);
            }
        }
    }

    // Armor "of Thorns": melee attackers take 30% of the damage they deal.
    bool melee = aa->anim == ANIM_THROW_PUNCH || aa->anim == ANIM_KICK_LEG || aa->anim == ANIM_THRUST_ANIM || aa->anim == ANIM_SWING_ANIM;
    if (hit && melee && defender == obj_dude && attack->defenderDamage > 0 && arpg_alive(attacker)
        && arpg_dude_has(false, 0, ASUF_THORNS)) {
        Attack thorns;
        memset(&thorns, 0, sizeof(thorns));
        combat_ctd_init(&thorns, obj_dude, attacker, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
        thorns.attackerFlags = DAM_HIT;
        thorns.defenderDamage = std::max(1, attack->defenderDamage * 30 / 100);
        bool wasDead = (attacker->data.critter.combat.results & DAM_DEAD) != 0;
        death_checks(&thorns);
        arpg_react(&thorns, attacker, thorns.defenderDamage, thorns.defenderFlags, 0);
        apply_damage(&thorns, false);
        char text[16];
        snprintf(text, sizeof(text), "%d", thorns.defenderDamage);
        arpg_float_text(attacker, text, colorTable[32256]);
        if (!wasDead && (attacker->data.critter.combat.results & DAM_DEAD) != 0) {
            arpg_on_kill(attacker, obj_dude);
        }
    }

    if (dudeDowned) {
        arpg_dude_downed(attacker);
    }

    for (size_t index = 0; index < victims.size(); index++) {
        Object* victim = victims[index];
        if (!wasDead[index] && victim != obj_dude && (victim->data.critter.combat.results & DAM_DEAD) != 0) {
            arpg_on_kill(victim, attacker);
        }
    }

    if (attacker == obj_dude && FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER
        && (defender->data.critter.combat.results & DAM_DEAD) != 0) {
        player_target = NULL;
        player_chasing = false;
    }
}

void arpg_request_combat(STRUCT_664980* attack)
{
    if (attack != NULL) {
        Object* attacker = attack->attacker;
        Object* defender = attack->defender;

        if (attacker != NULL && FID_TYPE(attacker->fid) == OBJ_TYPE_CRITTER
            && defender != NULL && FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER) {
            critter_set_who_hit_me(attacker, defender);
            if (defender != obj_dude) {
                critter_set_who_hit_me(defender, attacker);
            }

            attacker->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;

            if (attacker == obj_dude) {
                arpg_player_attack(defender);
            }

            arpg_log("ARPG: combat request %s -> %s\n", critter_name(attacker), critter_name(defender));
        }
    }

    alert_until = get_time() + 10000;
    next_join_check = 0;
}

bool arpg_is_hostile(Object* critter)
{
    if (!arpg_alive(critter) || critter == obj_dude) {
        return false;
    }

    if (isPartyMember(critter)) {
        return false;
    }

    if (critter->data.critter.combat.whoHitMe == obj_dude) {
        return true;
    }

    auto it = actors.find(critter);
    if (it != actors.end() && it->second.target != NULL) {
        Object* target = it->second.target;
        if (target == obj_dude || isPartyMember(target)) {
            return true;
        }
    }

    return false;
}

bool arpg_is_attackable(Object* critter)
{
    if (arpg_is_hostile(critter)) {
        return true;
    }

    if (!arpg_alive(critter) || critter == obj_dude || isPartyMember(critter)) {
        return false;
    }

    // Monsters are fair game on a plain click. People (and livestock and
    // dogs) need to be hostile first, or targeted with the crosshair.
    switch (critter_kill_count_type(critter)) {
    case KILL_TYPE_RADSCORPION:
    case KILL_TYPE_RAT:
    case KILL_TYPE_FLOATER:
    case KILL_TYPE_CENTAUR:
    case KILL_TYPE_MANTIS:
    case KILL_TYPE_DEATH_CLAW:
    case KILL_TYPE_PLANT:
        return true;
    default:
        return false;
    }
}

int arpg_energy_max()
{
    if (obj_dude == NULL) {
        return 0;
    }
    return stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS) * 10;
}

int arpg_energy()
{
    return energy < 0 ? 0 : (int)energy;
}

static void arpg_energy_tick(unsigned int now)
{
    int maxEnergy = arpg_energy_max();
    if (energy < 0) {
        energy = maxEnergy;
    }

    if (last_energy_tick != 0 && now > last_energy_tick) {
        // Regenerate max AP points per second (x energy_regen_pct).
        double perSecond = stat_level(obj_dude, STAT_MAXIMUM_ACTION_POINTS) * energy_regen_pct / 100.0;
        if (arpg_buff(SHRINE_ENERGY)) {
            perSecond *= 5.0;
        }
        if (arpg_dude_has(false, 0, ASUF_VIGOR)) {
            perSecond *= 1.5;
        }
        energy += perSecond * (now - last_energy_tick) / 1000.0;
    }
    last_energy_tick = now;

    if (energy > maxEnergy) {
        energy = maxEnergy;
    }
}

void arpg_player_attack(Object* target, bool inPlace)
{
    if (target == NULL || target == obj_dude) {
        return;
    }

    if (player_target != target) {
        player_target_time = get_time();
    }
    player_target = target;
    player_chasing = false;
    player_next_repath = 0;
    player_special = false;
    player_aim_location = -1;
    player_in_place = inPlace;
    player_chase_failures = 0;
}

void arpg_player_special(Object* target, bool inPlace)
{
    if (target == NULL || target == obj_dude) {
        return;
    }

    player_target = target;
    player_chasing = false;
    player_next_repath = 0;
    player_special = true;
    player_aim_location = -1;
    player_in_place = inPlace;
    player_chase_failures = 0;
}

static void arpg_player_aimed(Object* target, int hitLocation)
{
    arpg_player_special(target, arpg_attack_in_place());
    player_aim_location = hitLocation;
}

Object* arpg_player_target()
{
    return player_target != NULL && arpg_alive(player_target) ? player_target : NULL;
}

void arpg_player_clear_target()
{
    player_target = NULL;
    player_chasing = false;
}

static void arpg_report_bad_shot(int rc)
{
    MessageListItem messageListItem;
    switch (rc) {
    case COMBAT_BAD_SHOT_NO_AMMO:
        messageListItem.num = 101;
        break;
    case COMBAT_BAD_SHOT_ARM_CRIPPLED:
        messageListItem.num = 106;
        break;
    case COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED:
        messageListItem.num = 105;
        break;
    default:
        return;
    }

    if (message_search(&combat_message_file, &messageListItem)) {
        display_print(messageListItem.text);
    }
}

static void arpg_move_towards(Object* mover, Object* target)
{
    register_clear(mover);
    if (register_begin(ANIMATION_REQUEST_RESERVED) == 0) {
        if (register_object_run_to_object(mover, target, -1, 0) == 0) {
            if (register_end() == -1) {
                stat_move_fail++;
            }
        }
    } else {
        stat_move_fail++;
    }
}

static void arpg_player_tick(unsigned int now)
{
    if (player_target == NULL) {
        return;
    }

    if (!arpg_on_map(player_target) || !arpg_alive(player_target) || player_target->elevation != obj_dude->elevation) {
        player_target = NULL;
        player_chasing = false;
        return;
    }

    if ((obj_dude->data.critter.combat.results & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_DEAD)) != 0) {
        return;
    }

    if (autotest_knockdown_at != 0 && !autotest_knockdown_logged) {
        autotest_knockdown_logged = true;
        arpg_log("ARPG autotest: back on feet and fighting\n");
    }

    int hitMode;
    bool aiming;
    if (intface_get_attack(&hitMode, &aiming) == -1) {
        player_target = NULL;
        return;
    }

    int rc = arpg_bad_shot(obj_dude, player_target, hitMode);

    if (rc == COMBAT_BAD_SHOT_NO_AMMO) {
        Object* weapon = item_hit_with(obj_dude, hitMode);
        if (weapon != NULL && anim_busy(obj_dude)) {
            // Reload as soon as the current hit reaction or attack is over;
            // in a crowd that's most of the time.
            return;
        }

        if (weapon != NULL && item_w_try_reload(obj_dude, weapon) == 0) {
            intface_update_ammo_lights();
            const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, weapon, hitMode, NULL);
            gsound_play_sfx_file(sfx);
            player_next_attack = now + 600;
            arpg_log("ARPG: reloaded %s (%d)\n", item_name(weapon), item_w_curr_ammo(weapon));
            return;
        }

        const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_OUT_OF_AMMO, weapon, hitMode, NULL);
        gsound_play_sfx_file(sfx);
        arpg_report_bad_shot(rc);
        player_target = NULL;
        return;
    }

    if (rc == COMBAT_BAD_SHOT_ALREADY_DEAD) {
        player_target = NULL;
        return;
    }

    if (rc == COMBAT_BAD_SHOT_ARM_CRIPPLED || rc == COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED) {
        arpg_report_bad_shot(rc);
        player_target = NULL;
        return;
    }

    if (rc == COMBAT_BAD_SHOT_OUT_OF_RANGE || rc == COMBAT_BAD_SHOT_AIM_BLOCKED) {
        if (player_in_place) {
            // Attack in place (Shift / WASD scheme): never walk to the target.
            if (now >= next_range_warning) {
                next_range_warning = now + 1500;
                arpg_float_text(obj_dude, rc == COMBAT_BAD_SHOT_AIM_BLOCKED ? "Blocked" : "Out of range", colorTable[21140]);
            }
            player_target = NULL;
            player_special = false;
            return;
        }

        if (now < player_next_repath) {
            return;
        }

        // Moving: re-path once a second. Standing: retry a failed path
        // after a short pause.
        bool busy = anim_busy(obj_dude) != 0;
        if (busy && player_chasing && now < player_next_repath + 1000) {
            return;
        }

        arpg_move_towards(obj_dude, player_target);
        player_chasing = true;

        if (anim_busy(obj_dude)) {
            player_chase_failures = 0;
            player_next_repath = now + 1000;
        } else {
            // No path (wall, closed door, other level section): give up
            // after a few tries instead of standing there forever.
            player_chase_failures++;
            player_next_repath = now + 300;
            if (player_chase_failures >= 3) {
                arpg_log("ARPG: can't reach %s (rc=%d dist=%d dude=%d target=%d path=%d busy=%d)\n", critter_name(player_target), rc, obj_dist(obj_dude, player_target), obj_dude->tile, player_target->tile, make_path(obj_dude, obj_dude->tile, player_target->tile, NULL, 0), anim_busy(obj_dude));
                player_unreachable = player_target;
                player_unreachable_until = now + 8000;
                player_target = NULL;
                player_chasing = false;
            }
        }
        return;
    }

    // In range.
    if (now < player_next_attack) {
        return;
    }

    // Run and gun: an attack cuts a walk or run short (WASD picks the run up
    // again once the shot is out), but never another attack or a hit
    // reaction.
    if (anim_busy(obj_dude)) {
        int anim = FID_ANIM_TYPE(obj_dude->fid);
        if (anim != ANIM_WALK && anim != ANIM_RUNNING) {
            return;
        }
    }

    if (wasd_moving) {
        stat_shots_while_wasd++;
    }

    player_chasing = false;

    int hitLocation = HIT_LOCATION_TORSO;

    if (player_special) {
        // Right-click special: burst fire if the weapon has it, otherwise an
        // aimed shot at the head. Costs energy (10 per AP).
        player_special = false;

        int specialMode = hitMode;
        bool aimed = true;
        int aimLocation = player_aim_location;
        player_aim_location = -1;
        if (aimLocation != -1) {
            // Skill bar aimed shot at a chosen body part.
            if (!item_w_called_shot(obj_dude, hitMode)) {
                arpg_float_text(obj_dude, "Can't aim", colorTable[21140]);
                aimLocation = HIT_LOCATION_TORSO;
                aimed = false;
            }
        } else if (hitMode == HIT_MODE_LEFT_WEAPON_PRIMARY || hitMode == HIT_MODE_RIGHT_WEAPON_PRIMARY) {
            int secondary = hitMode + 1;
            int secondaryAnim = item_w_anim(obj_dude, secondary);
            if (secondaryAnim == ANIM_FIRE_BURST || secondaryAnim == ANIM_FIRE_CONTINUOUS) {
                specialMode = secondary;
                aimed = false;
            }
        }

        int cost = item_w_mp_cost(obj_dude, specialMode, aimed) * 10;
        if (aimLocation != -1 && !aimed) {
            cost = 0;
        } else if (arpg_energy() < cost) {
            arpg_float_text(obj_dude, "Need energy", colorTable[992]);
            gsound_play_sfx_file("iisxxxx1");
        } else {
            energy -= cost;
            hitMode = specialMode;
            if (aimed) {
                hitLocation = aimLocation != -1 ? aimLocation : HIT_LOCATION_HEAD;
                switch (hitLocation) {
                case HIT_LOCATION_EYES:
                    arpg_float_text(obj_dude, "Eyes!", colorTable[992]);
                    break;
                case HIT_LOCATION_LEFT_LEG:
                case HIT_LOCATION_RIGHT_LEG:
                    arpg_float_text(obj_dude, "Legs!", colorTable[992]);
                    break;
                default:
                    arpg_float_text(obj_dude, "Aimed shot!", colorTable[992]);
                    break;
                }
            } else {
                arpg_float_text(obj_dude, "Burst!", colorTable[992]);
            }
            arpg_log("ARPG: special %s cost=%d energy=%d\n", aimed ? "aimed" : "burst", cost, arpg_energy());
        }

        // A special is a single action; don't keep auto-attacking afterwards
        // unless the player clicks again.
        Object* target = player_target;
        if (arpg_start_attack(obj_dude, target, hitMode, hitLocation)) {
            player_next_attack = now + arpg_attack_delay(obj_dude, hitMode) * 100 / arpg_dude_attack_speed_pct();
        }
        player_target = NULL;
        return;
    }

    if (arpg_start_attack(obj_dude, player_target, hitMode)) {
        player_next_attack = now + arpg_attack_delay(obj_dude, hitMode) * 100 / arpg_dude_attack_speed_pct();
    }
}

// ---------------------------------------------------------------------------
// Loot (Diablo-style drops scattered around the corpse)
// ---------------------------------------------------------------------------

static void arpg_build_loot_table()
{
    loot_table.clear();
    static const int excluded[] = { 19, 79, 205, 120, 161, 162, 163, 28 };
    for (int pid = 1; pid < 600; pid++) {
        Proto* proto;
        if (proto_ptr(pid, &proto) == -1) {
            continue;
        }
        if (proto->item.type != ITEM_TYPE_WEAPON && proto->item.type != ITEM_TYPE_ARMOR) {
            continue;
        }
        if (proto->item.cost < 30) {
            continue;
        }
        bool skip = false;
        for (int ex : excluded) {
            if (ex == pid) {
                skip = true;
            }
        }
        if (!skip) {
            loot_table.push_back(pid);
        }
    }
    arpg_log("ARPG: loot table has %d items\n", (int)loot_table.size());
}

static int arpg_free_tile_near(Object* origin)
{
    for (int attempt = 0; attempt < 12; attempt++) {
        int rotation = roll_random(0, 5);
        int distance = roll_random(1, 2);
        int tile = tile_num_in_direction(origin->tile, rotation, distance);
        if (tile >= 0 && obj_blocking_at(NULL, tile, origin->elevation) == NULL) {
            return tile;
        }
    }
    return origin->tile;
}

static Object* arpg_drop_item(Object* corpse, int pid, int quantity)
{
    Object* item;
    if (obj_pid_new(&item, pid) != 0) {
        return NULL;
    }

    if (pid == PROTO_ID_MONEY) {
        item_caps_set_amount(item, quantity);
    }

    Rect rect;
    int tile = arpg_free_tile_near(corpse);
    obj_move_to_tile(item, tile, corpse->elevation, &rect);
    tile_refresh_rect(&rect, corpse->elevation);
    return item;
}

// Diablo-style drops. `rolls` is the number of item picks (D2 treasure class
// picks: champions get more, uniques more still and always some gear).
// Spawned pack fodder drops less often, so packs don't flood the floor.
static void arpg_drop_loot(Object* victim, int rolls, bool guaranteedGear)
{
    if (!loot_drops) {
        return;
    }

    if (loot_table.empty()) {
        arpg_build_loot_table();
    }

    int xp = critter_kill_exps(victim);
    ArpgMonster* monster = arpg_monster(victim);
    int pct = loot_pct;
    if (arpg_is_spawned(victim) && (monster == NULL || monster->rank == RANK_NORMAL || monster->rank == RANK_MINION)) {
        pct = pct / 2;
    }
    // Fortune affixes (D2 gold/magic find).
    int findPct = arpg_dude_find_pct();
    pct = pct * findPct / 100;
    // Champions and uniques drop better gear (D2 treasure class quality).
    if (monster != NULL && monster->rank == RANK_CHAMPION) {
        findPct += 50;
    } else if (monster != NULL && monster->rank == RANK_UNIQUE) {
        findPct += 150;
    }

    for (int roll = 0; roll < rolls; roll++) {
        if (roll_random(1, 100) <= 55 * pct / 100) {
            arpg_drop_item(victim, PROTO_ID_MONEY, roll_random(1, 3 + xp / 3));
        }

        if (roll_random(1, 100) <= 10 * pct / 100) {
            arpg_drop_item(victim, PROTO_ID_STIMPACK, 1);
        }

        if (roll_random(1, 100) <= 20 * pct / 100) {
            int hitMode;
            bool aiming;
            if (intface_get_attack(&hitMode, &aiming) != -1) {
                Object* weapon = item_hit_with(obj_dude, hitMode);
                if (weapon != NULL && item_w_max_ammo(weapon) > 0) {
                    int ammoPid = item_w_ammo_pid(weapon);
                    if (ammoPid > 0) {
                        arpg_drop_item(victim, ammoPid, 1);
                    }
                }
            }
        }

        int gearChance = (8 + std::min(xp / 20, 15)) * pct / 100;
        bool forced = guaranteedGear && roll == 0;
        if (!loot_table.empty() && (forced || roll_random(1, 100) <= gearChance)) {
            int budget = (300 + xp * 25) * (guaranteedGear ? 2 : 1);

            // Unique items (D2 gold): mostly from unique monsters.
            int uniquePerMille = monster != NULL && monster->rank == RANK_UNIQUE ? 150
                : monster != NULL && monster->rank == RANK_CHAMPION ? 40
                                                                    : 5;
            if (roll_random(1, 1000) <= uniquePerMille * findPct / 100) {
                int index = arpg_pick_unique(budget * 2);
                if (index != 0) {
                    Object* item = arpg_drop_item(victim, unique_items[index].pid, 1);
                    if (item != NULL) {
                        arpg_set_unique(item, index);
                        arpg_unique_fix_fid(item);
                        arpg_log("ARPG: %s dropped unique %s\n", critter_name(victim), item_name(item));
                        continue;
                    }
                }
            }
            std::vector<int> eligible;
            for (int pid : loot_table) {
                Proto* proto;
                if (proto_ptr(pid, &proto) != -1 && proto->item.cost <= budget) {
                    eligible.push_back(pid);
                }
            }
            if (!eligible.empty()) {
                int pid = eligible[roll_random(0, (int)eligible.size() - 1)];
                Object* item = arpg_drop_item(victim, pid, 1);
                if (item != NULL) {
                    arpg_roll_magic(item, findPct, forced);
                    arpg_log("ARPG: %s dropped %s (quality %d)\n", critter_name(victim), item_name(item), arpg_item_quality(item));
                }
            }
        }
    }
}

// Armor affixes that work over time: Regenerating heals, Lead-Lined sheds
// radiation.
static void arpg_gear_tick(unsigned int now)
{
    static unsigned int nextSecond = 0;
    static int seconds = 0;
    if (now < nextSecond) {
        return;
    }
    nextSecond = now + 1000;
    seconds++;

    Object* armor = arpg_dude_armor();
    if (arpg_item_quality(armor) == 0) {
        return;
    }

    if (arpg_item_has_prefix(armor, APRE_REGENERATING) && seconds % 4 == 0
        && critter_get_hits(obj_dude) < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS)) {
        critter_adjust_hits(obj_dude, 1);
        intface_update_hit_points(false);
    }
    if (arpg_item_has_prefix(armor, APRE_LEAD_LINED) && critter_get_rads(obj_dude) > 0) {
        critter_adjust_rads(obj_dude, -std::min(2, critter_get_rads(obj_dude)));
    }
}

// D2 gold: caps lying next to the player are picked up just by walking past.
static void arpg_caps_tick(unsigned int now)
{
    static unsigned int next = 0;
    if (!auto_caps || now < next) {
        return;
    }
    next = now + 200;

    if (obj_dude->tile == -1 || (obj_dude->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
        return;
    }

    std::vector<Object*> found;
    for (Object* obj = obj_find_first_at(obj_dude->elevation); obj != NULL; obj = obj_find_next_at()) {
        if (obj->pid == PROTO_ID_MONEY && obj->owner == NULL && (obj->flags & OBJECT_HIDDEN) == 0
            && obj->tile != -1 && tile_dist(obj->tile, obj_dude->tile) <= 2) {
            found.push_back(obj);
        }
    }

    for (Object* caps : found) {
        int amount = item_caps_get_amount(caps);
        if (obj_pickup(obj_dude, caps) == 0 && caps->owner == obj_dude) {
            char text[32];
            snprintf(text, sizeof(text), "+%d caps", amount);
            arpg_float_text(obj_dude, text, colorTable[32256]);
            gsound_play_sfx_file("ib1p1xx1");
            intface_update_items(false);
            arpg_log("ARPG: picked up %d caps\n", amount);
        }
    }
}

// ---------------------------------------------------------------------------
// Hold-to-move (hold the left button to keep running toward the cursor)
// ---------------------------------------------------------------------------

static void arpg_hold_tick(unsigned int now)
{
    if (!hold_to_move) {
        return;
    }

    int buttons = mouse_get_buttons();
    if ((buttons & MOUSE_EVENT_LEFT_BUTTON_REPEAT) == 0) {
        hold_last_tile = -1;
        return;
    }

    if (gmouse_3d_get_mode() != GAME_MOUSE_MODE_MOVE || !intface_is_enabled() || intface_is_hidden()) {
        return;
    }

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    if (mouseY >= screenGetHeight() - 100) {
        return;
    }

    if (now < next_hold_action) {
        return;
    }
    next_hold_action = now + 300;

    bool inPlace = arpg_attack_in_place();
    Object* critter = inPlace ? arpg_target_near_cursor() : object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
    if (critter != NULL && arpg_is_attackable(critter)) {
        if (player_target != critter || player_in_place != inPlace) {
            arpg_player_attack(critter, inPlace);
        }
        return;
    }

    if (player_target != NULL || inPlace) {
        return;
    }

    int tile = tile_num(mouseX, mouseY, map_elevation);
    if (tile == -1 || tile == hold_last_tile || tile == obj_dude->tile) {
        return;
    }

    hold_last_tile = tile;
    register_clear(obj_dude);
    dude_run(-1);
}

static bool arpg_wants_to_join(Object* critter)
{
    if ((critter->data.critter.combat.maneuver & CRITTER_MANEUVER_ENGAGING) != 0) {
        return true;
    }

    if ((critter->data.critter.combat.maneuver & (CRITTER_MANEUVER_DISENGAGING | CRITTER_MANUEVER_FLEEING)) != 0) {
        return false;
    }

    if (!is_within_perception(critter, obj_dude)) {
        return false;
    }

    if (critter->sid != -1) {
        scr_set_objs(critter->sid, NULL, NULL);
        scr_set_ext_param(critter->sid, 5);
        exec_script_proc(critter->sid, SCRIPT_PROC_COMBAT);
    }

    return (critter->data.critter.combat.maneuver & CRITTER_MANEUVER_ENGAGING) != 0;
}

static int arpg_pick_hit_mode(Object* critter, ArpgActor* actor)
{
    Object* weapon = item_hit_with(critter, HIT_MODE_RIGHT_WEAPON_PRIMARY);
    if (weapon == NULL && !actor->triedWeapon) {
        actor->triedWeapon = true;
        Object* best = ai_search_inven(critter, 0);
        if (best != NULL) {
            inven_wield(critter, best, 1);
            weapon = item_hit_with(critter, HIT_MODE_RIGHT_WEAPON_PRIMARY);
        }
    }

    if (weapon != NULL && item_get_type(weapon) == ITEM_TYPE_WEAPON) {
        return HIT_MODE_RIGHT_WEAPON_PRIMARY;
    }

    return HIT_MODE_PUNCH;
}

// Phasing (D2 Teleport): blink next to a target that is out of reach.
static bool arpg_try_phase(Object* critter, Object* target, unsigned int now)
{
    ArpgMonster* monster = arpg_monster(critter);
    if (monster == NULL || (monster->affixes & AFFIX_PHASING) == 0 || now < monster->nextPhase) {
        return false;
    }
    if (obj_dist(critter, target) < 4 || (critter->data.critter.combat.results & DAM_KNOCKED_DOWN) != 0) {
        return false;
    }

    for (int attempt = 0; attempt < 12; attempt++) {
        int tile = tile_num_in_direction(target->tile, roll_random(0, 5), roll_random(1, 2));
        if (tile < 0 || tile >= HEX_GRID_SIZE || obj_blocking_at(NULL, tile, target->elevation) != NULL) {
            continue;
        }
        if (make_path(critter, tile, target->tile, NULL, 0) <= 0) {
            continue;
        }

        if (register_clear(critter) == -2) {
            return false;
        }

        Rect oldRect;
        obj_bound(critter, &oldRect);
        Rect newRect;
        obj_move_to_tile(critter, tile, target->elevation, &newRect);
        obj_set_rotation(critter, tile_dir(tile, target->tile), NULL);
        tile_refresh_rect(&oldRect, critter->elevation);
        tile_refresh_rect(&newRect, critter->elevation);

        monster->nextPhase = now + 4000;
        arpg_float_text(critter, "*phase*", colorTable[992]);
        gsound_play_sfx_file("electri1");
        arpg_log("ARPG: %s phases to %d\n", monster->name, tile);
        return true;
    }

    monster->nextPhase = now + 1000;
    return false;
}

static void arpg_critter_tick(Object* critter, unsigned int now)
{
    ArpgActor& actor = actors[critter];

    if (now < actor.nextThink) {
        return;
    }
    actor.nextThink = now + 150;

    CritterCombatData* combat = &(critter->data.critter.combat);

    // Stand up after a knockdown.
    if ((combat->results & DAM_KNOCKED_DOWN) != 0 && (combat->results & DAM_KNOCKED_OUT) == 0) {
        if (actor.knockdownTime == 0) {
            actor.knockdownTime = now;
        } else if (now - actor.knockdownTime > 1500 && !anim_busy(critter)) {
            combat_arpg_standup(critter);
            actor.knockdownTime = 0;
        }
        return;
    }
    actor.knockdownTime = 0;

    if (obj_dist(critter, obj_dude) > leash_distance) {
        actor.target = NULL;
        return;
    }

    Object* target = ai_danger_source(critter);
    if (target != NULL && (!arpg_alive(target) || target->elevation != critter->elevation)) {
        target = NULL;
    }

    if (target == NULL && isPartyMember(critter) && player_target != NULL && arpg_alive(player_target)) {
        target = player_target;
    }

    actor.target = target;
    if (target == NULL) {
        return;
    }

    combat->maneuver |= CRITTER_MANEUVER_ENGAGING;

    if (target == obj_dude || isPartyMember(target)) {
        arpg_alert_pack(critter, target);
        arpg_call_reinforcements(critter);
    }

    int hitMode = arpg_pick_hit_mode(critter, &actor);
    int rc = arpg_bad_shot(critter, target, hitMode);

    if (rc == COMBAT_BAD_SHOT_NO_AMMO) {
        Object* weapon = item_hit_with(critter, hitMode);
        if (weapon != NULL && !anim_busy(critter)) {
            if (item_w_try_reload(critter, weapon) != 0) {
                inven_unwield(critter, 1);
            }
            actor.nextAttack = now + 800;
        }
        return;
    }

    if (rc == COMBAT_BAD_SHOT_ARM_CRIPPLED || rc == COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED) {
        if (!anim_busy(critter)) {
            inven_unwield(critter, 1);
        }
        return;
    }

    if (rc == COMBAT_BAD_SHOT_OUT_OF_RANGE || rc == COMBAT_BAD_SHOT_AIM_BLOCKED) {
        if (arpg_try_phase(critter, target, now)) {
            return;
        }
        // Re-path when the last move ended, or once a second while moving.
        // A path that failed (the way is blocked by the rest of the pack)
        // waits before trying again: a swarm otherwise runs hundreds of
        // failing searches a second.
        bool idle = !anim_busy(critter);
        if ((idle && !actor.pathFailed) || now >= actor.nextRepath) {
            arpg_move_towards(critter, target);
            actor.pathFailed = !anim_busy(critter);
            actor.nextRepath = now + (actor.pathFailed ? 700 + roll_random(0, 600) : 1200);
        }
        return;
    }

    if (rc != COMBAT_BAD_SHOT_OK) {
        return;
    }

    if (now < actor.nextAttack) {
        return;
    }

    // Moving is fine to interrupt; an attack in progress is not.
    if (anim_busy(critter) && FID_ANIM_TYPE(critter->fid) != ANIM_WALK && FID_ANIM_TYPE(critter->fid) != ANIM_RUNNING) {
        return;
    }

    if (arpg_start_attack(critter, target, hitMode)) {
        // Extra Fast / Fanatic monsters attack more often.
        actor.nextAttack = now + arpg_attack_delay(critter, hitMode) * 100 / arpg_monster_speed_pct(critter);
    }
}

void arpg_tick()
{
    ArpgTickTimer tickTimer;

    if (!arpg_on || obj_dude == NULL) {
        return;
    }

    unsigned int now = get_time();

    int mapIndex = map_get_index_number();
    if (mapIndex != last_map || map_elevation != last_elevation) {
        if (mapIndex != last_map) {
            random_map_index = random_map_pending ? mapIndex : -1;
            random_map_pending = false;
        }
        arpg_reset_level();
        last_map = mapIndex;
        last_elevation = map_elevation;
        // Spawn packs once the level has settled (scripts may still be
        // placing or removing critters on the first frames).
        populate_pending = true;
        populate_time = now + 500;
        arpg_unique_fix_all();
    }

    if (now >= next_list_refresh) {
        double refreshStart = arpg_now_ms();
        arpg_refresh_list();
        prof_refresh_ms += arpg_now_ms() - refreshStart;
        next_list_refresh = now + 1000;
    }

    if (populate_pending && now >= populate_time) {
        populate_pending = false;
        arpg_populate_level();
    }

    double profMark = arpg_now_ms();

    // Resolve hits queued by animation callbacks.
    if (!pending_hits.empty()) {
        std::vector<ArpgAttack*> hits;
        hits.swap(pending_hits);
        for (ArpgAttack* aa : hits) {
            if (aa->attack.attacker == obj_dude) {
                if (aa->invalid) {
                    stat_dude_dropped++;
                } else {
                    stat_dude_resolved++;
                }
            }
            if (!aa->invalid) {
                arpg_resolve(aa);
            }
            arpg_release(aa);
        }
    }

    // Launch the bullets of shots fired since the last tick.
    for (size_t index = 0; index < live_attacks.size();) {
        ArpgAttack* aa = live_attacks[index];
        if (!aa->launchPending) {
            index++;
            continue;
        }

        aa->launchPending = false;
        if (!aa->invalid) {
            // Bullets without art (and any with no room for a sequence)
            // land at once.
            if (arpg_projectile_fid(aa->attack.weapon) == -1) {
                stat_launch_noart++;
                arpg_cb_hit(aa, NULL);
            } else if (!arpg_launch_projectile(aa)) {
                stat_launch_fail++;
                arpg_cb_hit(aa, NULL);
            }
        }

        // The launch reference taken in arpg_cb_fired; this may free `aa`.
        size_t before = live_attacks.size();
        arpg_release(aa);
        if (live_attacks.size() == before) {
            index++;
        }
    }

    prof_section_ms[0] += arpg_now_ms() - profMark;
    profMark = arpg_now_ms();

    if ((obj_dude->data.critter.combat.results & DAM_DEAD) != 0) {
        return;
    }

    if (arpg_test) {
        if (autotest_start == 0) {
            if (dump_items) {
                for (int pid = 1; pid < 600; pid++) {
                    Proto* proto;
                    if (proto_ptr(pid, &proto) != -1) {
                        arpg_log("ITEM %d type=%d cost=%d name=%s\n", pid, proto->item.type, proto->item.cost, proto_name(pid));
                    }
                }
                for (int index = 1; index < 2000; index++) {
                    int pid = (OBJ_TYPE_SCENERY << 24) | index;
                    Proto* proto;
                    if (proto_ptr(pid, &proto) != -1 && proto->fid != -1) {
                        arpg_log("SCENERY %d type=%d art=%d light=%d name=%s\n", index, proto->scenery.type, proto->fid & 0xFFF, proto->lightDistance, proto_name(pid));
                    }
                }
                for (int index = 1; index < 400; index++) {
                    int pid = (OBJ_TYPE_CRITTER << 24) | index;
                    Proto* proto;
                    if (proto_ptr(pid, &proto) != -1) {
                        arpg_log("CRITTER %d kill=%d xp=%d hp=%d team=%d ai=%d art=%d name=%s\n",
                            index,
                            proto->critter.data.killType,
                            proto->critter.data.experience,
                            proto->critter.data.baseStats[STAT_MAXIMUM_HIT_POINTS],
                            proto->critter.team,
                            proto->critter.aiPacket,
                            proto->fid & 0xFFF,
                            proto_name(pid));
                    }
                }
            }
            autotest_start = now;
            arpg_log("ARPG autotest start, map %d elevation %d\n", map_get_index_number(), map_elevation);

            // The player's own starting kit must not carry affix bits either.
            int flagged = 0;
            Inventory* inventory = &(obj_dude->data.inventory);
            for (int index = 0; index < inventory->length; index++) {
                if ((inventory->items[index].item->flags & ARPG_ITEM_AFFIX_MASK) != 0) {
                    flagged++;
                }
            }
            arpg_log("ARPG: player inventory affix-bit items: %d of %d\n", flagged, inventory->length);
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                if (critter != obj_dude) {
                    arpg_log("  critter %s pid=%d team=%d talk=%d dist=%d xp=%d\n",
                        critter_name(critter),
                        critter->pid & 0xFFFFFF,
                        critter->data.critter.combat.team,
                        obj_action_can_talk_to(critter) ? 1 : 0,
                        obj_dist(critter, obj_dude),
                        critter_kill_exps(critter));
                }
            }
        }

        if (autoshot_ms > 0 && now >= next_autoshot) {
            next_autoshot = now + autoshot_ms;
            dump_screen();
        }

        // Map sweep: visit every map (through real map transitions) and every
        // non-empty elevation, reporting what the pack and aggro systems did.
        static int sweepMap = 0;
        static int sweepElevation = 0;
        static unsigned int sweepNext = 0;
        if (autotest_sweep > 0 && now - autotest_start > 2000 && now >= sweepNext) {
            sweepNext = now + 7000;
            // The Glow's radiation would end the sweep with a vanilla death.
            int rads = critter_get_rads(obj_dude);
            if (rads > 0) {
                critter_adjust_rads(obj_dude, -rads);
            }
            // Queued radiation-sickness events carry their own (fatal) level.
            queue_remove_this(obj_dude, EVENT_TYPE_RADIATION);
            if (sweepMap != 0) {
                int monstersHere = 0;
                int spawnedHere = 0;
                int engagedMonsters = 0;
                int engagedOthers = 0;
                for (int index = 0; index < crit_count; index++) {
                    Object* critter = crit_list[index];
                    if (critter == obj_dude || !arpg_alive(critter)) {
                        continue;
                    }
                    bool monster = arpg_is_monster_kind(critter);
                    monstersHere += monster ? 1 : 0;
                    spawnedHere += arpg_is_spawned(critter) ? 1 : 0;
                    bool engaged = arpg_is_engaged(critter) || critter->data.critter.combat.whoHitMe == obj_dude;
                    if (engaged && !isPartyMember(critter)) {
                        if (monster) {
                            engagedMonsters++;
                        } else {
                            engagedOthers++;
                            arpg_log("SWEEP   engaged non-monster: %s pid=%d team=%d\n", critter_name(critter), critter->pid & 0xFFFFFF, critter->data.critter.combat.team);
                        }
                    }
                }
                char name[16];
                map_get_name_idx(name, map_get_index_number());
                arpg_log("SWEEP map %d %s elev %d: critters=%d monsters=%d spawned=%d engaged=%d engaged_others=%d hp=%d\n",
                    map_get_index_number(), name, map_elevation, crit_count - 1, monstersHere, spawnedHere, engagedMonsters, engagedOthers, critter_get_hits(obj_dude));
            }

            // Next elevation of this map, if it has anything on it.
            bool moved = false;
            while (sweepMap != 0 && !moved && sweepElevation < ELEVATION_COUNT - 1) {
                sweepElevation++;
                if (map_is_elevation_empty(sweepElevation)) {
                    continue;
                }
                for (Object* obj = obj_find_first_at(sweepElevation); obj != NULL && !moved; obj = obj_find_next_at()) {
                    if (obj->tile == -1 || FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER) {
                        continue;
                    }
                    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                        int tile = tile_num_in_direction(obj->tile, rotation, 2);
                        if (tile >= 0 && obj_blocking_at(NULL, tile, sweepElevation) == NULL) {
                            register_clear(obj_dude);
                            obj_move_to_tile(obj_dude, tile, sweepElevation, NULL);
                            map_set_elevation(sweepElevation);
                            tile_set_center(obj_dude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
                            arpg_log("SWEEP -> elevation %d\n", sweepElevation);
                            moved = true;
                            break;
                        }
                    }
                }
            }

            if (!moved) {
                char name[16];
                // Encounter sweep: only the random-encounter family, each map
                // twice in the same game hour, noted as the world map does.
                static const char* encounterPrefixes[] = { "DESERT", "MOUNTN", "COAST", "CITY", "DESCRVN", "MNTCRVN" };
                auto isEncounter = [&](const char* mapName) {
                    for (const char* prefix : encounterPrefixes) {
                        if (compat_strnicmp(mapName, prefix, strlen(prefix)) == 0) {
                            return true;
                        }
                    }
                    return false;
                };
                static bool revisited = false;
                int next;
                if (autotest_sweep_encounters && sweepMap != 0 && !revisited) {
                    next = sweepMap;
                    revisited = true;
                    map_get_name_idx(name, next);
                } else {
                    revisited = false;
                    next = sweepMap == 0 ? autotest_sweep : sweepMap + 1;
                    while (next < 200 && (map_get_name_idx(name, next) == -1 || (autotest_sweep_encounters && !isEncounter(name)))) {
                        next++;
                    }
                }
                if (autotest_sweep_encounters && next < 200) {
                    arpg_note_random_map();
                }
                if (next >= 200) {
                    arpg_log("SWEEP done\n");
                    game_user_wants_to_quit = 2;
                } else {
                    sweepMap = next;
                    sweepElevation = 0;
                    MapTransition transition = { next, 0, -1, 0 };
                    map_leave_map(&transition);
                    arpg_log("SWEEP -> map %d %s\n", next, name);
                }
            }
        }

        // Raider pack test: treat the test map as a random encounter and turn
        // its people on the player.
        if (autotest_encounter && random_map_index != map_get_index_number()) {
            random_map_index = map_get_index_number();
        }
        static bool hostileDone = false;
        if (autotest_hostile && !hostileDone && now - autotest_start > 6000) {
            hostileDone = true;
            int count = 0;
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                if (critter != obj_dude && arpg_alive(critter) && !isPartyMember(critter) && !arpg_is_monster_kind(critter)) {
                    critter_set_who_hit_me(critter, obj_dude);
                    critter->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
                    count++;
                }
            }
            alert_until = now + 10000;
            arpg_log("ARPG autotest: %d people turn hostile\n", count);
        }

        // Stress test: every monster on the level comes for the player.
        static bool swarmDone = false;
        if (autotest_swarm && !swarmDone && now - autotest_start > 6000) {
            swarmDone = true;
            int count = 0;
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                if (critter != obj_dude && arpg_alive(critter) && arpg_is_monster_kind(critter)) {
                    critter_set_who_hit_me(critter, obj_dude);
                    critter->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
                    count++;
                }
            }
            alert_until = now + 10000;
            arpg_log("ARPG autotest: swarm of %d\n", count);
        }

        // Scenery gallery: candidate shrine looks in a row next to the player.
        static bool galleryDone = false;
        if (autotest_gallery && !galleryDone && now - autotest_start > 1500) {
            galleryDone = true;
            static const int candidates[] = { 1, 198, 484, 505, 506, 507, 508, 522, 523, 524, 194 };
            int tile = obj_dude->tile;
            for (int index = 0; index < (int)(sizeof(candidates) / sizeof(*candidates)); index++) {
                tile = tile_num_in_direction(tile_num_in_direction(tile, 2, 1), 1, 1);
                Object* obj;
                if (obj_pid_new(&obj, (OBJ_TYPE_SCENERY << 24) | candidates[index]) == 0) {
                    obj->flags |= OBJECT_NO_SAVE;
                    Rect rect;
                    obj_move_to_tile(obj, tile, obj_dude->elevation, &rect);
                    tile_refresh_rect(&rect, obj_dude->elevation);
                    arpg_log("GALLERY %d: scenery %d at %d\n", index, candidates[index], tile);
                }
            }
        }

        // Loot highlight test: one drop of each tier next to the player.
        static bool lootDone = false;
        if (autotest_loot && !lootDone && now - autotest_start > 1500) {
            lootDone = true;
            // { pid, prefix, suffix }: normal, magic, rare, unique (Dust Devil, old
            // encoding), stimpak, caps; then every unique numbered 16 and up.
            static const int drops[][3] = { { 8, 0, 0 }, { 8, 1, 0 }, { 94, 2, 3 }, { 143, UNIQUE_MARKER, 15 }, { PROTO_ID_STIMPACK, 0, 0 }, { PROTO_ID_MONEY, 0, 0 } };
            for (const auto& drop : drops) {
                Object* item = arpg_drop_item(obj_dude, drop[0], drop[0] == PROTO_ID_MONEY ? 77 : 1);
                if (item != NULL) {
                    item->flags |= (drop[1] << ARPG_ITEM_PREFIX_SHIFT) & ARPG_ITEM_PREFIX_MASK;
                    item->flags |= (drop[2] << ARPG_ITEM_SUFFIX_SHIFT) & ARPG_ITEM_SUFFIX_MASK;
                    arpg_unique_fix_fid(item);
                    arpg_log("LOOT test: %s at %d quality %d fid 0x%x\n", item_name(item), item->tile, arpg_item_quality(item), item->fid);
                }
            }
            for (int index = 16; index < UNIQUE_ITEM_COUNT; index++) {
                Object* item = arpg_drop_item(obj_dude, unique_items[index].pid, 1);
                if (item != NULL) {
                    arpg_set_unique(item, index);
                    arpg_unique_fix_fid(item);
                    arpg_log("LOOT test: %s at %d quality %d fid 0x%x inv 0x%x flags 0x%x\n", item_name(item), item->tile,
                        arpg_item_quality(item), item->fid, item_inv_fid(item), item->flags);
                }
            }
        }

        // Shrine test: every 12 s, step next to the next unused shrine.
        static unsigned int nextShrineVisit = 0;
        if (autotest_shrines && now - autotest_start > 8000 && now >= nextShrineVisit) {
            nextShrineVisit = now + 12000;
            for (ArpgShrine& shrine : shrines) {
                if (shrine.used || shrine.obj->tile == -1 || shrine.obj->elevation != obj_dude->elevation) {
                    continue;
                }
                for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                    int tile = tile_num_in_direction(shrine.obj->tile, rotation, 1);
                    if (obj_blocking_at(obj_dude, tile, shrine.obj->elevation) == NULL) {
                        register_clear(obj_dude);
                        Rect rect;
                        obj_move_to_tile(obj_dude, tile, shrine.obj->elevation, &rect);
                        tile_set_center(tile, TILE_SET_CENTER_REFRESH_WINDOW);
                        arpg_log("ARPG autotest: visiting %s\n", shrine_info[shrine.kind].name);
                        break;
                    }
                }
                break;
            }
        }

        // Respawn test: drop to 1 HP so the next monster hit is a killing blow.
        static bool dieDone = false;
        if (autotest_die > 0 && !dieDone && now - autotest_start > (unsigned int)autotest_die * 1000) {
            dieDone = true;
            critter_adjust_hits(obj_dude, -(critter_get_hits(obj_dude) - 1));
            intface_update_hit_points(false);
            arpg_log("ARPG autotest: player at 1 HP, caps %d\n", item_caps_total(obj_dude));
        }

        // Save mid-fight with packs alive, one clone knocked out so a queue
        // event is owned by an unsaved object, then load the save back.
        static int saveLoadPhase = 0;
        if (autotest_saveload > 0) {
            unsigned int at = (unsigned int)autotest_saveload * 1000;
            if (saveLoadPhase == 0 && now - autotest_start > at) {
                saveLoadPhase = 1;
                int clones = 0;
                for (int index = 0; index < crit_count; index++) {
                    Object* critter = crit_list[index];
                    if (arpg_is_spawned(critter) && arpg_alive(critter)) {
                        if (clones == 0) {
                            critter->data.critter.combat.results |= DAM_KNOCKED_OUT;
                            queue_add(600, critter, NULL, EVENT_TYPE_KNOCKOUT);
                            arpg_log("ARPG autotest: knocked out clone %s\n", arpg_label(critter));
                        }
                        clones++;
                    }
                }
                int rc = arpg_ls_save_slot(9, "ARPG autotest");
                arpg_log("ARPG autotest: saved slot 10 rc=%d with %d live clones\n", rc, clones);
            } else if (saveLoadPhase == 1 && now - autotest_start > at + 3000) {
                saveLoadPhase = 2;
                arpg_log("ARPG autotest: loading slot 10\n");
                int rc = arpg_ls_load_slot(9);
                arpg_log("ARPG autotest: loaded rc=%d, map %d, dude hp=%d\n", rc, map_get_index_number(), critter_get_hits(obj_dude));
                Object* weapon = arpg_dude_weapon();
                arpg_log("ARPG autotest: after load wielding %s (quality %d)\n", weapon != NULL ? item_name(weapon) : "-", arpg_item_quality(weapon));
                return;
            }
        }

        // Watchdog: an attack animation that has been playing for seconds
        // means a sequence is stuck. Dump it once.
        static unsigned int busySince = 0;
        static int dumps = 0;
        int dudeAnim = FID_ANIM_TYPE(obj_dude->fid);
        bool attackAnim = dudeAnim == ANIM_POINT || dudeAnim == ANIM_UNPOINT || dudeAnim == ANIM_FIRE_SINGLE || dudeAnim == ANIM_FIRE_BURST;
        if (anim_busy(obj_dude) && attackAnim) {
            if (busySince == 0) {
                busySince = now;
            } else if (now - busySince > 3000 && dumps < 3) {
                dumps++;
                busySince = now;
                arpg_log("ARPG watchdog: dude stuck in anim %d for 3 s, frame %d\n", dudeAnim, obj_dude->frame);
                anim_debug_dump(obj_dude, [](const char* line) { arpg_log("%s", line); });
            }
        } else {
            busySince = 0;
        }

        static unsigned int nextCensus = 0;
        static unsigned int censusTicks = 0;
        static unsigned int lastCensus = 0;
        censusTicks++;
        if (now >= nextCensus) {
            nextCensus = now + 10000;
            // Game loop frames per second (arpg_tick runs once per frame).
            if (lastCensus != 0 && now > lastCensus) {
                double seconds = (now - lastCensus) / 1000.0;
                arpg_log("ARPG census: %u ticks/s; arpg ms/s: tick %.1f (critters %.1f, hud %.1f, refresh %.1f)\n",
                    censusTicks * 1000 / (now - lastCensus),
                    prof_tick_ms / seconds, prof_critters_ms / seconds, prof_hud_ms / seconds, prof_refresh_ms / seconds);
            }
            prof_tick_ms = 0;
            prof_critters_ms = 0;
            prof_hud_ms = 0;
            prof_refresh_ms = 0;
            arpg_log("ARPG census: sections ms/s: hits %.1f autotest %.1f autoplay %.1f player+hud %.1f aggro %.1f\n", prof_section_ms[0] / 10.0, prof_section_ms[1] / 10.0, prof_section_ms[2] / 10.0, prof_section_ms[3] / 10.0, prof_section_ms[4] / 10.0);
            memset(prof_section_ms, 0, sizeof(prof_section_ms));
            arpg_log("ARPG census: resolve x%d ms/s: display %.1f react %.1f apply %.1f float %.1f\n", prof_resolve_count, prof_resolve_ms[0] / 10.0, prof_resolve_ms[1] / 10.0, prof_resolve_ms[2] / 10.0, prof_resolve_ms[3] / 10.0);
            memset(prof_resolve_ms, 0, sizeof(prof_resolve_ms));
            prof_resolve_count = 0;
            lastCensus = now;
            censusTicks = 0;
            int alive[4] = { 0 };
            int engaged[4] = { 0 };
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                if (critter == obj_dude || !arpg_alive(critter)) {
                    continue;
                }
                ArpgMonster* monster = arpg_monster(critter);
                int rank = monster != NULL ? monster->rank : RANK_NORMAL;
                alive[rank]++;
                if (arpg_is_engaged(critter)) {
                    engaged[rank]++;
                }
            }
            arpg_log("ARPG census: alive N%d M%d C%d U%d, engaged N%d M%d C%d U%d, dude hp=%d\n",
                alive[0], alive[1], alive[2], alive[3], engaged[0], engaged[1], engaged[2], engaged[3], critter_get_hits(obj_dude));
            int hitMode;
            bool aiming;
            intface_get_attack(&hitMode, &aiming);
            arpg_log("ARPG census: dude target=%s dist=%d rc=%d busy=%d anim=%d results=0x%x next_attack_in=%d chasing=%d in_place=%d\n",
                player_target != NULL ? arpg_label(player_target) : "-",
                player_target != NULL ? obj_dist(obj_dude, player_target) : -1,
                player_target != NULL ? arpg_bad_shot(obj_dude, player_target, hitMode) : -1,
                anim_busy(obj_dude),
                FID_ANIM_TYPE(obj_dude->fid),
                obj_dude->data.critter.combat.results,
                (int)(player_next_attack - now),
                player_chasing ? 1 : 0,
                player_in_place ? 1 : 0);
            Object* nearest = NULL;
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                if (arpg_is_attackable(critter) && (nearest == NULL || obj_dist(critter, obj_dude) < obj_dist(nearest, obj_dude))) {
                    nearest = critter;
                }
            }
            Object* weapon = item_hit_with(obj_dude, hitMode);
            arpg_log("ARPG census: hitMode=%d weapon=%s ammo=%d nearest=%s dist=%d rc=%d\n",
                hitMode,
                weapon != NULL ? item_name(weapon) : "-",
                weapon != NULL ? item_w_curr_ammo(weapon) : -1,
                nearest != NULL ? arpg_label(nearest) : "-",
                nearest != NULL ? obj_dist(nearest, obj_dude) : -1,
                nearest != NULL ? arpg_bad_shot(obj_dude, nearest, hitMode) : -1);
        }

        if (autotest_seconds > 0 && now - autotest_start > (unsigned int)autotest_seconds * 1000) {
            arpg_log("ARPG autotest done: unfired=%d spawned=%d move_fail=%d deaths=%d\n", stat_unfired, stat_spawned, stat_move_fail, stat_deaths);
            arpg_log("ARPG autotest done: dude hp=%d/%d xp=%d attacks=%d failed=%d react_blocked=%d kills=%d wasd_shots=%d latency_ms=%d\n",
                critter_get_hits(obj_dude),
                stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS),
                stat_pc_get(PC_STAT_EXPERIENCE),
                stat_attacks,
                stat_attack_fail,
                stat_react_blocked,
                stat_kills,
                stat_shots_while_wasd,
                stat_latency_count > 0 ? (int)(stat_latency_total / stat_latency_count) : -1);
            arpg_log("ARPG autotest done: move_shot=%d moving_shots=%d late_slides=%d slide_hexes=%d slide_blocked=%d launch_fail=%d launch_noart=%d\n",
                move_shot ? 1 : 0, stat_moving_shots, stat_late_slides, stat_slide_hexes, stat_slide_blocked, stat_launch_fail, stat_launch_noart);
            arpg_log("ARPG autotest done: dude shots fired=%d resolved=%d dropped=%d in_flight=%d\n",
                stat_dude_fired, stat_dude_resolved, stat_dude_dropped, (int)live_attacks.size());
            arpg_log("ARPG autotest done: wasd_ms=%u run_ms=%u slide_ms=%u stalled_ms=%u (attack %u, stand %u) run_hexes=%d (%.2f/s) slide %.2f hexes/s\n",
                stat_wasd_ms, stat_run_ms, stat_slide_ms, stat_wasd_stalled_ms, stat_stalled_attack_ms, stat_stalled_stand_ms, stat_run_hexes,
                stat_run_ms > 0 ? stat_run_hexes * 1000.0 / stat_run_ms : 0.0,
                stat_slide_ms > 0 ? stat_slide_hexes * 1000.0 / stat_slide_ms : 0.0);
            game_user_wants_to_quit = 2;
        }
    }

    prof_section_ms[1] += arpg_now_ms() - profMark;
    profMark = arpg_now_ms();

    if (arpg_test && !autotest_armed && autotest_weapon_pid != -1) {
        autotest_armed = true;
        Object* weapon;
        if (obj_pid_new(&weapon, autotest_weapon_pid) == 0) {
            if (item_add_force(obj_dude, weapon, 1) == 0) {
                obj_disconnect(weapon, NULL);
            }
            // Left hand is the active slot on a new game.
            inven_wield(obj_dude, weapon, 0);
            if (autotest_ammo_pid != -1) {
                Object* ammo;
                if (obj_pid_new(&ammo, autotest_ammo_pid) == 0) {
                    // Packs mean long fights: bring plenty.
                    if (item_add_force(obj_dude, ammo, 25) == 0) {
                        obj_disconnect(ammo, NULL);
                    }
                }
            }
            if (autotest_armor_pid != -1) {
                Object* armor;
                if (obj_pid_new(&armor, autotest_armor_pid) == 0) {
                    if (item_add_force(obj_dude, armor, 1) == 0) {
                        obj_disconnect(armor, NULL);
                    }
                    inven_wield(obj_dude, armor, 0);
                }
            }
            // Stimpaks plus one of each flask-slot drug (1-5 keys).
            static const int kit[][2] = {
                { PROTO_ID_STIMPACK, 5 },
                { PROTO_ID_SUPER_STIMPACK, 2 },
                { PROTO_ID_RADAWAY, 1 },
                { 109 /* Rad-X */, 1 },
                { PROTO_ID_BUFF_OUT, 2 },
                { PROTO_ID_MENTATS, 2 },
                { PROTO_ID_PSYCHO, 2 },
            };
            for (const auto& entry : kit) {
                Object* drug;
                if (obj_pid_new(&drug, entry[0]) == 0) {
                    if (item_add_force(obj_dude, drug, entry[1]) == 0) {
                        obj_disconnect(drug, NULL);
                    }
                }
            }
            if (autotest_xp > 0) {
                stat_pc_add_experience(autotest_xp);
                critter_adjust_hits(obj_dude, 999);
            }
            bool hurt = false;
            configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_hurt", &hurt);
            if (hurt) {
                critter_adjust_hits(obj_dude, -(critter_get_hits(obj_dude) * 7 / 10));
                config_set_value(&game_config, ARPG_CONFIG_SECTION, "autotest_hurt", 0);
            }
            intface_update_items(false);
            intface_update_ac(false);
            intface_update_hit_points(false);
            if (autotest_magic > 0) {
                // prefix * 16 + suffix, e.g. 18 = Vicious ... of Haste.
                weapon->flags |= ((autotest_magic / 16) << ARPG_ITEM_PREFIX_SHIFT) & ARPG_ITEM_PREFIX_MASK;
                weapon->flags |= ((autotest_magic % 16) << ARPG_ITEM_SUFFIX_SHIFT) & ARPG_ITEM_SUFFIX_MASK;
            }
            arpg_log("ARPG autotest: armed with %s\n", item_name(weapon));
            if (autotest_magic > 0) {
                // What the player sees when examining it.
                obj_examine_func(obj_dude, weapon, [](char* text) {
                    arpg_log("ARPG autotest: examine: %s\n", text);
                    display_print(text);
                });
            }

            // Caps on the floor nearby, for the walk-over pickup.
            for (int pile = 0; pile < 3; pile++) {
                arpg_drop_item(obj_dude, PROTO_ID_MONEY, 10 + pile);
            }
        }
    }

    bool knockdownTest = false;
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "autotest_knockdown", &knockdownTest);
    if (arpg_test && knockdownTest && stat_attacks >= 2 && autotest_knockdown_at == 0) {
        autotest_knockdown_at = now;
        config_set_value(&game_config, ARPG_CONFIG_SECTION, "autotest_knockdown", 0);
        // Simulate a knockout crit on the player through the real path.
        register_clear_force(obj_dude);
        obj_dude->data.critter.combat.results |= DAM_KNOCKED_OUT;
        queue_add(100, obj_dude, NULL, EVENT_TYPE_KNOCKOUT);
        register_begin(ANIMATION_REQUEST_RESERVED);
        register_object_animate(obj_dude, ANIM_FALL_BACK, 0);
        register_end();
        arpg_log("ARPG autotest: knocking the player down\n");
    }

    if (arpg_test && autotest_iframes) {
        // Permanent dodge i-frames to exercise the dodged-hit path.
        dodge_until = now + 60000;
    }

    if (arpg_test && autotest_god && critter_get_hits(obj_dude) < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS) / 2) {
        // Feature tests: keep the bot alive so long fights can be observed.
        critter_adjust_hits(obj_dude, stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS));
        intface_update_hit_points(false);
    }

    if (autoplay) {
        // Test bot drinks a "potion" (Q) at low life.
        static unsigned int nextHeal = 0;
        if (autotest_die == 0 && now >= nextHeal && critter_get_hits(obj_dude) * 100 < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS) * 40) {
            nextHeal = now + 2000;
            int before = critter_get_hits(obj_dude);
            arpg_quick_heal();
            arpg_log("ARPG autoplay: quick heal %d -> %d\n", before, critter_get_hits(obj_dude));
        }

        // Test bot dodges (Space) when it gets hurt.
        static unsigned int nextDodge = 0;
        if (controls != ARPG_CONTROLS_CLASSIC && now >= nextDodge && player_target != NULL
            && critter_get_hits(obj_dude) * 100 < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS) * 80) {
            nextDodge = now + 5000;
            arpg_dodge();
        }

        // Test bot: warp next to the nearest critter whenever idle, and
        // attack any monster within leash range.
        if (!autoplay_warped) {
            if (autoplay_warp_time == 0) {
                autoplay_warp_time = now + 3000;
            } else if (now >= autoplay_warp_time) {
                autoplay_warped = true;
                arpg_debug_warp_to_enemy();
            }
        } else if (player_target == NULL && !anim_busy(obj_dude)) {
            if (autoplay_idle_since == 0) {
                autoplay_idle_since = now;
            } else if (now - autoplay_idle_since > 5000) {
                autoplay_idle_since = 0;
                arpg_debug_warp_to_enemy();
            }
        } else {
            autoplay_idle_since = 0;
        }

        // Test bot: stuck chasing something behind a wall without landing an
        // attack for a while; go somewhere else.
        static int lastAttacks = 0;
        static unsigned int lastAttackTime = 0;
        if (stat_attacks != lastAttacks || lastAttackTime == 0) {
            lastAttacks = stat_attacks;
            lastAttackTime = now;
        } else if (autoplay_warped && now - lastAttackTime > 10000) {
            lastAttackTime = now;
            arpg_log("ARPG autoplay: stalled, warping\n");
            arpg_player_clear_target();
            arpg_debug_warp_to_enemy();
        }

        if (player_target == NULL) {
            Object* best = NULL;
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                // can_see() depends on facing; a WASD runner shoots at
                // anything in its line of fire, behind it included.
                int shot = controls == ARPG_CONTROLS_WASD ? arpg_bad_shot(obj_dude, critter, HIT_MODE_LEFT_WEAPON_PRIMARY) : -1;
                bool visible = controls == ARPG_CONTROLS_WASD
                    ? shot == COMBAT_BAD_SHOT_OK || shot == COMBAT_BAD_SHOT_NO_AMMO
                    : can_see(obj_dude, critter);
                if (arpg_is_attackable(critter) && obj_dist(critter, obj_dude) <= 12 && visible
                    && !(critter == player_unreachable && now < player_unreachable_until)) {
                    if (best == NULL || obj_dist(critter, obj_dude) < obj_dist(best, obj_dude)) {
                        best = critter;
                    }
                }
            }
            if (best != NULL && arpg_energy() >= arpg_energy_max() && controls != ARPG_CONTROLS_CLASSIC) {
                // Exercise the skill bar aimed shots (Q/E/T).
                static int nextAim = 0;
                static const int aims[] = { HIT_LOCATION_HEAD, HIT_LOCATION_LEFT_LEG, HIT_LOCATION_EYES };
                arpg_log("ARPG autoplay: aimed shot loc=%d at %s\n", aims[nextAim % 3], critter_name(best));
                arpg_player_aimed(best, aims[nextAim++ % 3]);
            } else if (best != NULL && arpg_energy() >= arpg_energy_max()) {
                arpg_player_special(best, arpg_attack_in_place());
            } else if (best != NULL) {
                arpg_log("ARPG autoplay: targeting %s\n", critter_name(best));
                arpg_player_attack(best, arpg_attack_in_place());
            }
        }
    }

    if (death_until != 0 && now >= death_until) {
        arpg_respawn();
    }
    bool downed = death_until != 0;

    // The player gets up after a knockdown, like critters do. Vanilla handles
    // this at the start of a combat turn, which never comes in real time.
    if (downed) {
        dude_knockdown_time = 0;
    } else if ((obj_dude->data.critter.combat.results & DAM_KNOCKED_DOWN) != 0
        && (obj_dude->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_DEAD)) == 0) {
        if (dude_knockdown_time == 0) {
            dude_knockdown_time = now;
            arpg_log("ARPG: player knocked down\n");
        } else if (now - dude_knockdown_time > 1500 && !anim_busy(obj_dude)) {
            combat_arpg_standup(obj_dude);
            dude_knockdown_time = 0;
            arpg_log("ARPG: player stood up\n");
        }
    } else {
        dude_knockdown_time = 0;
    }

    prof_section_ms[2] += arpg_now_ms() - profMark;
    profMark = arpg_now_ms();

    arpg_energy_tick(now);
    if (!downed) {
        arpg_caps_tick(now);
        arpg_shrine_tick(now);
        arpg_gear_tick(now);
        arpg_wasd_tick(now);
        arpg_hold_tick(now);
        arpg_player_tick(now);
    }
    double hudStart = arpg_now_ms();
    arpg_hud_tick(now);
    prof_hud_ms += arpg_now_ms() - hudStart;

    if ((obj_dude->data.critter.combat.results & DAM_KNOCKED_OUT) != 0
        && (obj_dude->data.critter.combat.results & DAM_DEAD) == 0) {
        obj_dude->data.critter.combat.results &= ~DAM_KNOCKED_OUT;
        obj_dude->data.critter.combat.results |= DAM_KNOCKED_DOWN;
        queue_remove_this(obj_dude, EVENT_TYPE_KNOCKOUT);
        arpg_log("ARPG: player knockout downgraded to knockdown\n");
    }

    prof_section_ms[3] += arpg_now_ms() - profMark;
    profMark = arpg_now_ms();

    if (!downed) {
        arpg_aggro_tick(now);
    }

    prof_section_ms[4] += arpg_now_ms() - profMark;

    bool alert = now < alert_until;
    bool joinCheck = alert && now >= next_join_check;
    if (joinCheck) {
        next_join_check = now + 1500;
    }

    bool anyEngaged = false;
    double crittersStart = arpg_now_ms();

    for (int index = 0; index < crit_count; index++) {
        Object* critter = crit_list[index];
        if (critter == obj_dude || !arpg_alive(critter)) {
            continue;
        }

        CritterCombatData* combat = &(critter->data.critter.combat);
        bool engaged = (combat->maneuver & CRITTER_MANEUVER_ENGAGING) != 0
            || combat->whoHitMe != NULL
            || isPartyMember(critter);

        if (!engaged && joinCheck && obj_dist(critter, obj_dude) <= leash_distance) {
            engaged = arpg_wants_to_join(critter);
        }

        if (!engaged) {
            continue;
        }

        arpg_critter_tick(critter, now);

        auto it = actors.find(critter);
        if (it != actors.end() && it->second.target != NULL && !isPartyMember(critter)) {
            anyEngaged = true;
        }
    }

    prof_critters_ms += arpg_now_ms() - crittersStart;

    if (anyEngaged) {
        alert_until = std::max(alert_until, now + 3000);
    }
}

bool arpg_is_engaged(Object* critter)
{
    if (!arpg_on) {
        return false;
    }

    auto it = actors.find(critter);
    return it != actors.end() && it->second.target != NULL;
}

// ---------------------------------------------------------------------------
// Control schemes (Path of Exile style)
// ---------------------------------------------------------------------------

int arpg_controls()
{
    return controls;
}

bool arpg_attack_in_place()
{
    if (!arpg_on) {
        return false;
    }

    if (controls == ARPG_CONTROLS_WASD) {
        return true;
    }

    return controls == ARPG_CONTROLS_POE && (keys[SDL_SCANCODE_LSHIFT] != 0 || keys[SDL_SCANCODE_RSHIFT] != 0);
}

// The critter under the cursor, or failing that the closest attackable one
// within two hexes of the cursor, so skill keys don't need pixel-exact aim.
Object* arpg_target_near_cursor()
{
    Object* critter = object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
    if (critter != NULL && arpg_is_attackable(critter)) {
        return critter;
    }

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    int tile = tile_num(mouseX, mouseY, map_elevation);
    if (tile == -1) {
        return NULL;
    }

    Object* best = NULL;
    int bestDist = 3;
    for (int index = 0; index < crit_count; index++) {
        Object* candidate = crit_list[index];
        if (candidate->elevation != map_elevation || !arpg_is_attackable(candidate)) {
            continue;
        }

        int dist = tile_dist(tile, candidate->tile);
        if (dist < bestDist) {
            best = candidate;
            bestDist = dist;
        }
    }
    return best;
}

// Uses the first carried drug from `pids` (a PoE "flask"). Returns true if
// one was taken.
static bool arpg_use_drug(const int* pids, int count, const char* floatText)
{
    for (int index = 0; index < count; index++) {
        Object* item = inven_pid_is_carried_ptr(obj_dude, pids[index]);
        if (item == NULL) {
            continue;
        }

        // Same sequence the combat AI uses for stimpaks.
        if (item_remove_mult(obj_dude, item, 1) == 0) {
            if (item_d_take_drug(obj_dude, item) == -1) {
                item_add_force(obj_dude, item, 1);
            } else {
                obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
                obj_destroy(item);
                arpg_float_text(obj_dude, floatText, colorTable[992]);
            }
        }
        intface_update_hit_points(false);
        intface_update_items(false);
        gsound_play_sfx_file("ib1p1xx1");
        return true;
    }
    return false;
}

static const int flask_heal[] = { PROTO_ID_SUPER_STIMPACK, PROTO_ID_STIMPACK };
static const int flask_rad[] = { PROTO_ID_RADAWAY, 109 /* Rad-X */ };
static const int flask_str[] = { PROTO_ID_BUFF_OUT };
static const int flask_int[] = { PROTO_ID_MENTATS };
static const int flask_rage[] = { PROTO_ID_PSYCHO, PROTO_ID_NUKA_COLA };

#define FLASK(pids, text, missing)                                         \
    if (!arpg_use_drug(pids, sizeof(pids) / sizeof(*pids), text)) {      \
        arpg_print(missing);                                               \
    }

static void arpg_use_flask(int slot)
{
    switch (slot) {
    case 1:
        arpg_quick_heal();
        break;
    case 2:
        FLASK(flask_rad, "+rad", "No RadAway or Rad-X.");
        break;
    case 3:
        FLASK(flask_str, "+Buffout", "No Buffout.");
        break;
    case 4:
        FLASK(flask_int, "+Mentats", "No Mentats.");
        break;
    case 5:
        FLASK(flask_rage, "+Psycho", "No Psycho or Nuka-Cola.");
        break;
    }
}

#undef FLASK

// A tile roughly `hexes` hexes from the player in screen direction (dx, dy).
// Fallout's hexes have no straight "up", so this aims at a screen point and
// lets the pathfinder zig-zag there.
static int arpg_tile_toward(double dx, double dy, int hexes)
{
    int x;
    int y;
    if (tile_coord(obj_dude->tile, &x, &y, obj_dude->elevation) == -1) {
        return -1;
    }

    double len = sqrt(dx * dx + dy * dy);
    if (len <= 0.0) {
        return -1;
    }

    // Isometric 2:1 units: 32 px across, 16 px down per step.
    int tx = x + 16 + (int)(dx / len * 32.0 * hexes);
    int ty = y + 8 + (int)(dy / len * 16.0 * hexes);
    return tile_num(tx, ty, obj_dude->elevation, true);
}

// Finds a reachable tile in screen direction (dx, dy), up to `maxHexes`
// away. Falls back to directions angled up to 90 degrees off so the player
// slides along walls instead of stopping dead, and rejects targets the
// pathfinder could only reach by a long detour.
static int arpg_step_target(double dx, double dy, int maxHexes, int minHexes)
{
    int x;
    int y;
    if (tile_coord(obj_dude->tile, &x, &y, obj_dude->elevation) == -1) {
        return -1;
    }

    double len = sqrt(dx * dx + dy * dy);
    if (len <= 0.0) {
        return -1;
    }

    // Try the side we slid to last time first, so a wall corner doesn't make
    // the player flip back and forth.
    static int slideSide = 1;
    double angles[] = { 0.0, 0.7854 * slideSide, -0.7854 * slideSide, 1.3963 * slideSide, -1.3963 * slideSide };
    for (int index = 0; index < 5; index++) {
        double angle = angles[index];
        double rx = dx * cos(angle) - dy * sin(angle);
        double ry = dx * sin(angle) + dy * cos(angle);
        int strideMax = index == 0 ? maxHexes : std::min(maxHexes, 3);
        for (int hexes = strideMax; hexes >= minHexes; hexes--) {
            int tile = arpg_tile_toward(rx, ry, hexes);
            if (tile == -1 || tile == obj_dude->tile) {
                continue;
            }
            if (obj_blocking_at(obj_dude, tile, obj_dude->elevation) != NULL) {
                continue;
            }

            // Must make headway in the wanted direction (hex rounding can
            // otherwise pick a tile slightly behind the player).
            int tx;
            int ty;
            tile_coord(tile, &tx, &ty, obj_dude->elevation);
            double forward = ((tx - x) / 32.0 * dx + (ty - y) / 16.0 * dy) / len;
            if (forward < 0.25) {
                continue;
            }

            int dist = tile_dist(obj_dude->tile, tile);
            int pathLength = make_path(obj_dude, obj_dude->tile, tile, NULL, 1);
            if (pathLength <= 0 || pathLength > dist + 2) {
                continue;
            }

            if (index != 0) {
                slideSide = angle > 0 ? 1 : -1;
            }
            return tile;
        }
    }
    return -1;
}

// True while the player is standing, walking or running, i.e. not in a hit
// reaction, knockdown, get-up or attack animation that movement must not cut
// short (reactions aren't prioritized, so register_clear would kill them).
static bool arpg_dude_can_move()
{
    int anim = FID_ANIM_TYPE(obj_dude->fid);
    return anim == ANIM_STAND || anim == ANIM_WALK || anim == ANIM_RUNNING;
}

static bool arpg_run_to(int tile)
{
    if (tile == -1 || tile == obj_dude->tile) {
        return false;
    }

    // Lowering the gun after a shot is only for show (the shot is out by
    // then), so a run may cut it short.
    bool lowering = move_shot && FID_ANIM_TYPE(obj_dude->fid) == ANIM_UNPOINT;
    if (!arpg_dude_can_move() && !lowering) {
        return false;
    }

    if (register_clear(obj_dude) == -2) {
        // Mid-attack; attack sequences are prioritized.
        return false;
    }

    register_begin(ANIMATION_REQUEST_RESERVED);
    if (register_object_run_to_tile(obj_dude, tile, obj_dude->elevation, -1, 0) != 0) {
        register_end();
        return false;
    }
    register_end();
    return anim_busy(obj_dude) != 0;
}

static bool arpg_input_ready()
{
    return intface_is_enabled() && !intface_is_hidden()
        && gmouse_3d_get_mode() == GAME_MOUSE_MODE_MOVE
        && (obj_dude->data.critter.combat.results & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_DEAD)) == 0;
}

static void arpg_wasd_dir(int* dx, int* dy)
{
    *dx = 0;
    *dy = 0;
    if (controls != ARPG_CONTROLS_WASD) {
        return;
    }

    if (keys[SDL_SCANCODE_W]) {
        *dy -= 1;
    }
    if (keys[SDL_SCANCODE_S]) {
        *dy += 1;
    }
    if (keys[SDL_SCANCODE_A]) {
        *dx -= 1;
    }
    if (keys[SDL_SCANCODE_D]) {
        *dx += 1;
    }

    if (arpg_test && autotest_wasd >= 0 && autotest_wasd <= 4 && autotest_start != 0) {
        static const int dirs[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
        // Hold the test direction for 3 s, then let go for 1 s. Mode 4
        // turns W, D, S, A in turn (kiting in circles).
        unsigned int elapsed = get_time() - autotest_start;
        int dir = autotest_wasd == 4 ? (int)((elapsed / 4000) % 4) : autotest_wasd;
        if (elapsed % 4000 < 3000) {
            *dx = dirs[dir][0];
            *dy = dirs[dir][1];
        }
    }
}

// Keeps the player near the middle of the view while WASD-running, since the
// cursor no longer drags the view along.
static void arpg_camera_follow()
{
    int x;
    int y;
    if (tile_coord(obj_dude->tile, &x, &y, obj_dude->elevation) == -1) {
        return;
    }

    int width = scr_size.lrx - scr_size.ulx + 1;
    int height = scr_size.lry - scr_size.uly + 1 - 100;
    int offX = x + 16 - width / 2;
    int offY = y + 8 - height / 2;

    int dx = 0;
    int dy = 0;
    if (offX > width / 6) {
        dx = 1;
    } else if (offX < -width / 6) {
        dx = -1;
    }
    if (offY > height / 6) {
        dy = 1;
    } else if (offY < -height / 6) {
        dy = -1;
    }

    if (dx != 0 || dy != 0) {
        map_scroll(dx, dy);
    }
}

// The player's measured run speed: hexes and milliseconds spent in the run
// animation (arpg_measure_run), halved now and then so it follows changes.
static double run_measured_hexes = 0.0;
static double run_measured_ms = 0.0;

// Hexes per millisecond the player covers when running, from the run
// animation: one cycle's frame offsets say how far a cycle goes, compute_tpf()
// how long it takes (run_speed_pct included). Frames only advance on game
// loop ticks, so real runs are slower than this; arpg_dude_run_rate() prefers
// the measured speed.
static double arpg_dude_art_run_rate()
{
    static int cachedFid = -1;
    static double cachedRate = 0.0;

    int fid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF, ANIM_RUNNING, (obj_dude->fid & 0xF000) >> 12, 0);
    if (fid == cachedFid) {
        return cachedRate;
    }

    double hexes = 0.0;
    int rotations = 0;
    int frames = 0;
    unsigned int frameMs = 0;
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int rotationFid = art_id(OBJ_TYPE_CRITTER, obj_dude->fid & 0xFFF, ANIM_RUNNING, (obj_dude->fid & 0xF000) >> 12, rotation + 1);
        CacheEntry* handle;
        Art* art = art_ptr_lock(rotationFid, &handle);
        if (art == NULL) {
            continue;
        }

        frames = art_frame_max_frame(art);
        int sumX = 0;
        int sumY = 0;
        for (int frame = 0; frame < frames; frame++) {
            int x;
            int y;
            art_frame_hot(art, frame, rotation, &x, &y);
            sumX += x;
            sumY += y;
        }
        art_ptr_unlock(handle);

        double step = sqrt((double)(off_tile[0][rotation] * off_tile[0][rotation] + off_tile[1][rotation] * off_tile[1][rotation]));
        hexes += sqrt((double)(sumX * sumX + sumY * sumY)) / step;
        rotations++;
        frameMs = compute_tpf(obj_dude, rotationFid);
    }

    // Without art: about four hexes a second.
    double rate = 0.004;
    if (rotations > 0 && frames > 0 && frameMs > 0 && hexes > 0.0) {
        rate = hexes / rotations / (frames * frameMs);
    }

    cachedFid = fid;
    cachedRate = rate;
    arpg_log("ARPG: run animation rate %.2f hexes/s (%d frames, %u ms/frame, %.2f hexes/cycle)\n",
        rate * 1000.0, frames, frameMs, rotations > 0 ? hexes / rotations : 0.0);
    return rate;
}

// Hexes per millisecond the player really covers when running, so a slide
// looks like the run it stands in for.
static double arpg_dude_run_rate()
{
    if (run_measured_ms >= 2000.0 && run_measured_hexes >= 8.0) {
        return run_measured_hexes / run_measured_ms;
    }
    // Before there's a measurement: about what the frame timing costs.
    return arpg_dude_art_run_rate() * 0.8;
}

// Called every tick: adds running time and the tiles it covered to the
// measured run speed.
static void arpg_measure_run(unsigned int now)
{
    static unsigned int lastTick = 0;
    static int lastTile = -1;
    static bool wasRunning = false;

    bool running = FID_ANIM_TYPE(obj_dude->fid) == ANIM_RUNNING && anim_busy(obj_dude);
    if (running && wasRunning) {
        run_measured_ms += std::min(now - lastTick, 100u);
        int hexes = tile_dist(lastTile, obj_dude->tile);
        if (hexes <= 2) {
            run_measured_hexes += hexes;
        }
        if (run_measured_ms > 30000.0) {
            run_measured_ms /= 2.0;
            run_measured_hexes /= 2.0;
        }
    }

    wasRunning = running;
    lastTick = now;
    lastTile = obj_dude->tile;
}

// Splits screen direction (ux, uy) into hex steps: `a` steps toward
// `rotation` and `b` toward the next rotation clockwise per pixel. A run that
// way zigzags between those two.
static int arpg_hex_split(double ux, double uy, double* a, double* b)
{
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int next = (rotation + 1) % ROTATION_COUNT;
        double ax = off_tile[0][rotation];
        double ay = off_tile[1][rotation];
        double bx = off_tile[0][next];
        double by = off_tile[1][next];
        double det = ax * by - ay * bx;
        *a = (ux * by - uy * bx) / det;
        *b = (ax * uy - ay * ux) / det;
        if (*a >= -1e-9 && *b >= -1e-9 && *a + *b > 0.0) {
            return rotation;
        }
    }
    *a = 1.0 / 32.0;
    *b = 0.0;
    return ROTATION_E;
}

// Screen pixels per hex step for a run in screen direction (ux, uy) (unit
// vector): straight east is 32 px a step, straight up only 12.
static double arpg_hex_step_px(double ux, double uy)
{
    double a;
    double b;
    arpg_hex_split(ux, uy, &a, &b);
    return 1.0 / (a + b);
}

// How far offset (x, y) from the current tile's centre lies toward the
// neighbour in `rotation`: 0 at the centre, 0.5 on the border, 1 at its
// centre. Measured on the hex grid itself, not on screen: the screen squashes
// it (diagonal neighbours are 12 px up or down where a regular grid's would
// be 27.7, so y is scaled by 27.7 / 12, 16/3 once squared), and screen
// distances would put the diagonal neighbours nearer than east and west.
static double arpg_toward_neighbour(double x, double y, int rotation)
{
    double ox = off_tile[0][rotation];
    double oy = off_tile[1][rotation];
    return (x * ox + y * oy * 16.0 / 3.0) / 1024.0;
}

// Moves the player along the WASD direction at run speed while a shot's
// sequence owns the player's animation (raising the gun and firing), so
// shooting doesn't stop the player. The move is done by hand: a pixel offset
// within the tile, and on to the neighbouring tile once that one's centre is
// nearer.
static void arpg_slide_tick(unsigned int now, int dx, int dy)
{
    if (!slide_active) {
        return;
    }

    int anim = FID_ANIM_TYPE(obj_dude->fid);
    bool shooting = anim == ANIM_POINT || anim == ANIM_UNPOINT || anim == ANIM_THROW_ANIM
        || (anim >= ANIM_FIRE_SINGLE && anim <= ANIM_FIRE_CONTINUOUS);
    if (anim == ANIM_STAND && now - slide_start < 100) {
        // The shot's first frame shows on the next animation tick.
        shooting = true;
    }

    if (!shooting || !anim_busy(obj_dude) || (dx == 0 && dy == 0)
        || (obj_dude->data.critter.combat.results & (DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT | DAM_DEAD)) != 0) {
        slide_active = false;
        if (arpg_test) {
            arpg_log("ARPG: slide end (%s, anim %d): %d hexes in %u ms, %d blocked ticks, rate %.2f hexes/s\n",
                dx == 0 && dy == 0 ? "released" : (!shooting || !anim_busy(obj_dude) ? "shot over" : "knocked down"),
                anim, slide_hops, now - slide_start, slide_blocked_ticks, arpg_dude_run_rate() * 1000.0);
        }
        return;
    }

    unsigned int elapsed = std::min(now - slide_last, 50u);
    slide_last = now;

    // Same screen direction as arpg_tile_toward().
    double vx = dx * 32.0;
    double vy = dy * 16.0;
    double len = sqrt(vx * vx + vy * vy);
    double ux = vx / len;
    double uy = vy / len;
    double rate = arpg_dude_run_rate();
    double distance = rate * arpg_hex_step_px(ux, uy) * elapsed;

    int elevation = obj_dude->elevation;
    bool blocked[ROTATION_COUNT];
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int tile = tile_num_in_direction(obj_dude->tile, rotation, 1);
        blocked[rotation] = tile == -1 || obj_blocking_at(obj_dude, tile, elevation) != NULL;
    }

    // A step may take the player no nearer than a quarter hex to a wall or
    // critter in the way (one of the two hex directions it heads between),
    // and never into one elsewhere.
    auto stepAllowed = [&](double x, double y) {
        double len = sqrt(x * x + y * y);
        if (len <= 0.0) {
            return false;
        }
        double a;
        double b;
        int first = arpg_hex_split(x / len, y / len, &a, &b);
        int second = (first + 1) % ROTATION_COUNT;
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            if (!blocked[rotation]) {
                continue;
            }
            double before = arpg_toward_neighbour(slide_sx, slide_sy, rotation);
            double after = arpg_toward_neighbour(slide_sx + x, slide_sy + y, rotation);
            if (after <= before) {
                continue;
            }
            bool ahead = (rotation == first && a > 1e-6) || (rotation == second && b > 1e-6);
            if (after > (ahead ? 0.25 : 0.5)) {
                return false;
            }
        }
        return true;
    };

    double stepX = ux * distance;
    double stepY = uy * distance;
    if (stepAllowed(stepX, stepY)) {
        slide_around = -1;
    } else {
        // Go round it like a run would: toward the middle of a free
        // neighbouring hex no more than 60 degrees off the WASD way, sticking
        // to the one picked before so the player doesn't wobble.
        stepX = 0.0;
        stepY = 0.0;
        std::vector<std::pair<double, int>> ways;
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            double ox = off_tile[0][rotation];
            double oy = off_tile[1][rotation];
            double facing = (ox * ux + oy * uy) / sqrt(ox * ox + oy * oy);
            if (!blocked[rotation] && facing >= 0.5) {
                ways.push_back({ rotation == slide_around ? 2.0 : facing, rotation });
            }
        }
        std::sort(ways.begin(), ways.end(), [](const std::pair<double, int>& lhs, const std::pair<double, int>& rhs) {
            return lhs.first > rhs.first;
        });

        for (const auto& way : ways) {
            int rotation = way.second;
            double cx = off_tile[0][rotation] - slide_sx;
            double cy = off_tile[1][rotation] - slide_sy;
            double clen = sqrt(cx * cx + cy * cy);
            if (clen <= 0.0) {
                continue;
            }
            double wayDistance = rate * arpg_hex_step_px(cx / clen, cy / clen) * elapsed;
            if (stepAllowed(cx / clen * wayDistance, cy / clen * wayDistance)) {
                stepX = cx / clen * wayDistance;
                stepY = cy / clen * wayDistance;
                slide_around = rotation;
                break;
            }
        }
    }

    if (distance > 0.0 && stepX == 0.0 && stepY == 0.0) {
        stat_slide_blocked++;
        slide_blocked_ticks++;
    }

    double sx = slide_sx + stepX;
    double sy = slide_sy + stepY;

    Rect dirty;
    Rect temp;
    obj_bound(obj_dude, &dirty);

    // Hop to the neighbouring tile whose centre is now nearest; moving
    // straight up or down passes a corner, so up to two hops at once.
    for (int hop = 0; hop < 3; hop++) {
        int best = -1;
        double bestValue = 0.5;
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            double value = arpg_toward_neighbour(sx, sy, rotation);
            if (value > bestValue) {
                best = rotation;
                bestValue = value;
            }
        }

        if (best == -1) {
            break;
        }

        int tile = tile_num_in_direction(obj_dude->tile, best, 1);
        if (tile == -1 || obj_blocking_at(obj_dude, tile, elevation) != NULL) {
            // Never step into anything; hold at the border.
            sx -= stepX;
            sy -= stepY;
            stat_slide_blocked++;
            slide_blocked_ticks++;
            break;
        }

        // obj_move_to_tile() zeroes the offset; keep the shooter in place on
        // screen, now measured from the new tile.
        int x = obj_dude->x;
        int y = obj_dude->y;
        obj_move_to_tile(obj_dude, tile, elevation, &temp);
        rect_min_bound(&dirty, &temp, &dirty);
        obj_offset(obj_dude, x - off_tile[0][best], y - off_tile[1][best], &temp);
        rect_min_bound(&dirty, &temp, &dirty);

        sx -= off_tile[0][best];
        sy -= off_tile[1][best];
        slide_sx -= off_tile[0][best];
        slide_sy -= off_tile[1][best];
        slide_px -= off_tile[0][best];
        slide_py -= off_tile[1][best];
        stat_slide_hexes++;
        slide_hops++;

        // Traps and trigger areas, as for a normal step.
        scr_chk_spatials_in(obj_dude, tile, elevation);
        if (obj_dude->tile != tile || obj_dude->elevation != elevation) {
            slide_active = false;
            tile_refresh_rect(&dirty, elevation);
            return;
        }
    }

    int moveX = (int)lround(sx) - slide_px;
    int moveY = (int)lround(sy) - slide_py;
    if (moveX != 0 || moveY != 0) {
        obj_offset(obj_dude, moveX, moveY, &temp);
        rect_min_bound(&dirty, &temp, &dirty);
        slide_px += moveX;
        slide_py += moveY;
    }
    slide_sx = sx;
    slide_sy = sy;

    tile_refresh_rect(&dirty, elevation);
}

static void arpg_wasd_tick(unsigned int now)
{
    if (controls != ARPG_CONTROLS_WASD) {
        wasd_moving = false;
        return;
    }

    arpg_camera_follow();
    arpg_measure_run(now);

    int dx;
    int dy;
    arpg_wasd_dir(&dx, &dy);
    bool held = (dx != 0 || dy != 0) && arpg_input_ready();

    if (arpg_test) {
        // Where the time goes while WASD is held: running, sliding through
        // a shot, or standing still.
        static unsigned int lastTick = 0;
        static int lastTile = -1;
        unsigned int elapsed = lastTick != 0 ? std::min(now - lastTick, 100u) : 0;
        lastTick = now;
        int anim = FID_ANIM_TYPE(obj_dude->fid);
        bool running = anim == ANIM_RUNNING || anim == ANIM_WALK;
        if (held) {
            stat_wasd_ms += elapsed;
            if (running) {
                stat_run_ms += elapsed;
            } else if (slide_active) {
                stat_slide_ms += elapsed;
            } else {
                stat_wasd_stalled_ms += elapsed;
                bool attacking = anim == ANIM_POINT || anim == ANIM_UNPOINT || anim == ANIM_THROW_ANIM
                    || (anim >= ANIM_FIRE_SINGLE && anim <= ANIM_FIRE_CONTINUOUS);
                if (attacking) {
                    stat_stalled_attack_ms += elapsed;
                } else if (anim == ANIM_STAND) {
                    stat_stalled_stand_ms += elapsed;
                }
            }
        }
        if (running && lastTile != -1 && obj_dude->tile != lastTile && tile_dist(lastTile, obj_dude->tile) <= 2) {
            stat_run_hexes += tile_dist(lastTile, obj_dude->tile);
        }
        lastTile = obj_dude->tile;
    }

    arpg_slide_tick(now, held ? dx : 0, held ? dy : 0);

    if (!held) {
        if (wasd_moving) {
            // Released: stop now instead of finishing the path, but never
            // cut a hit reaction, knockdown or attack short.
            wasd_moving = false;
            wasd_target_tile = -1;
            if (arpg_dude_can_move()) {
                register_clear(obj_dude);
            }
        }
        wasd_dx = 0;
        wasd_dy = 0;
        return;
    }

    if (!wasd_moving) {
        // Moving cancels a pending click attack.
        player_target = NULL;
        player_chasing = false;
    }
    wasd_moving = true;

    int anim = FID_ANIM_TYPE(obj_dude->fid);
    if (move_shot && !slide_active && anim_busy(obj_dude)
        && (anim == ANIM_POINT || (anim >= ANIM_FIRE_SINGLE && anim <= ANIM_FIRE_CONTINUOUS))) {
        // WASD pressed in the middle of a shot fired standing: get moving
        // now rather than when it's over.
        arpg_slide_begin(0, 0);
        stat_late_slides++;
    }

    bool turned = dx != wasd_dx || dy != wasd_dy;
    bool idle = !anim_busy(obj_dude) || (move_shot && anim == ANIM_UNPOINT);
    bool nearEnd = wasd_target_tile != -1 && tile_dist(obj_dude->tile, wasd_target_tile) <= 2;

    if (!turned && !idle && !nearEnd) {
        return;
    }

    if (now < wasd_next_move && !turned) {
        return;
    }
    wasd_next_move = now + 150;

    wasd_dx = dx;
    wasd_dy = dy;

    // Aim several hexes ahead so the run doesn't stop at every step.
    int tile = arpg_step_target(dx, dy, 6, 1);
    if (tile != -1 && arpg_run_to(tile)) {
        if (arpg_test) {
            arpg_log("ARPG: wasd (%d,%d) %d -> %d\n", dx, dy, obj_dude->tile, tile);
        }
        wasd_target_tile = tile;
    }
}

// PoE2 dodge roll (Space): a short dash toward the cursor (or the WASD
// direction) with a moment of invulnerability. Costs energy.
static void arpg_dodge()
{
    if (!arpg_input_ready()) {
        return;
    }

    unsigned int now = get_time();
    if (now < dodge_until + 300) {
        return;
    }

    if (arpg_energy() < dodge_cost) {
        arpg_float_text(obj_dude, "Need energy", colorTable[992]);
        gsound_play_sfx_file("iisxxxx1");
        return;
    }

    int dx;
    int dy;
    arpg_wasd_dir(&dx, &dy);

    double vx = dx;
    double vy = dy;
    if (dx == 0 && dy == 0) {
        int x;
        int y;
        int mouseX;
        int mouseY;
        mouse_get_position(&mouseX, &mouseY);
        if (tile_coord(obj_dude->tile, &x, &y, obj_dude->elevation) == -1) {
            return;
        }
        // Screen pixels to isometric 2:1 units.
        vx = (mouseX - (x + 16)) / 32.0;
        vy = (mouseY - (y + 8)) / 16.0;
    }

    int tile = arpg_step_target(vx, vy, 3, 1);
    if (tile != -1 && arpg_run_to(tile)) {
        energy -= dodge_cost;
        dodge_until = now + 600;
        player_target = NULL;
        player_chasing = false;
        wasd_target_tile = -1;
        arpg_float_text(obj_dude, "Dodge", colorTable[992]);
        arpg_log("ARPG: dodge to %d energy=%d\n", tile, arpg_energy());
    }
}

static void arpg_skill_shot(int hitLocation)
{
    if (!arpg_input_ready()) {
        return;
    }

    Object* target = arpg_target_near_cursor();
    if (target == NULL) {
        arpg_float_text(obj_dude, "No target", colorTable[21140]);
        return;
    }

    arpg_player_aimed(target, hitLocation);
}

static void arpg_cycle_controls()
{
    static const char* names[] = { "classic", "poe", "wasd" };
    static const char* descriptions[] = {
        "Controls: classic (Q heal, R reload, Shift walks).",
        "Controls: Path of Exile (1-5 flasks, Q/E/T aimed shots, Shift: attack in place, Space: dodge).",
        "Controls: WASD (W/A/S/D move, mouse aims, attacks in place).",
    };

    controls = (controls + 1) % 3;
    if (controls != ARPG_CONTROLS_WASD && wasd_moving) {
        wasd_moving = false;
        if (arpg_dude_can_move()) {
            register_clear(obj_dude);
        }
    }
    config_set_string(&game_config, ARPG_CONFIG_SECTION, "controls", names[controls]);

    // display_print() writes into its argument.
    char text[160];
    snprintf(text, sizeof(text), "%s", descriptions[controls]);
    display_print(text);
    gsound_play_sfx_file("ib1p1xx1");
}

bool arpg_handle_key(int key)
{
    if (!arpg_on || !intface_is_enabled()) {
        return false;
    }

    if (arpg_test) {
        arpg_log("ARPG: key %d (controls=%d)\n", key, controls);
    }

    if (key == KEY_CTRL_W) {
        arpg_cycle_controls();
        return true;
    }

    if (controls == ARPG_CONTROLS_CLASSIC) {
        return false;
    }

    switch (key) {
    case KEY_LOWERCASE_W:
    case KEY_UPPERCASE_W:
        if (controls == ARPG_CONTROLS_POE) {
            // D2 weapon swap: switch the active hand (Fallout's B).
            gsound_play_sfx_file("ib1p1xx1");
            intface_toggle_items(true);
            return true;
        }
        return controls == ARPG_CONTROLS_WASD;
    case KEY_LOWERCASE_A:
    case KEY_UPPERCASE_A:
    case KEY_LOWERCASE_S:
    case KEY_UPPERCASE_S:
    case KEY_LOWERCASE_D:
    case KEY_UPPERCASE_D:
        // Held keys are polled in arpg_wasd_tick; swallow the key events so
        // A doesn't start combat and S doesn't open the skilldex.
        return controls == ARPG_CONTROLS_WASD;
    case KEY_1:
    case KEY_2:
    case KEY_3:
    case KEY_4:
    case KEY_5:
        arpg_use_flask(key - KEY_1 + 1);
        return true;
    case KEY_LOWERCASE_Q:
    case KEY_UPPERCASE_Q:
        arpg_skill_shot(HIT_LOCATION_HEAD);
        return true;
    case KEY_LOWERCASE_E:
    case KEY_UPPERCASE_E:
        arpg_skill_shot(roll_random(0, 1) ? HIT_LOCATION_LEFT_LEG : HIT_LOCATION_RIGHT_LEG);
        return true;
    case KEY_LOWERCASE_T:
    case KEY_UPPERCASE_T:
        arpg_skill_shot(HIT_LOCATION_EYES);
        return true;
    case KEY_LOWERCASE_Z:
    case KEY_UPPERCASE_Z:
        labels_always = !labels_always;
        gsound_play_sfx_file("ib1p1xx1");
        return true;
    case KEY_SPACE:
        arpg_dodge();
        return true;
    case KEY_LOWERCASE_V:
    case KEY_UPPERCASE_V:
        arpg_hud_toggle_minimap();
        gsound_play_sfx_file("ib1p1xx1");
        return true;
    }

    return false;
}

void arpg_quick_heal()
{
    if (!arpg_on) {
        return;
    }

    if (!arpg_use_drug(flask_heal, sizeof(flask_heal) / sizeof(*flask_heal), "+heal")) {
        arpg_print("No stimpaks.");
    }
}

void arpg_quick_reload()
{
    if (!arpg_on) {
        return;
    }

    int hitMode;
    bool aiming;
    if (intface_get_attack(&hitMode, &aiming) == -1) {
        return;
    }

    Object* weapon = item_hit_with(obj_dude, hitMode);
    if (weapon == NULL || item_w_max_ammo(weapon) <= 0) {
        return;
    }

    if (item_w_try_reload(obj_dude, weapon) == 0) {
        intface_update_ammo_lights();
        const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, weapon, hitMode, NULL);
        gsound_play_sfx_file(sfx);
    }
}

void arpg_debug_warp_to_enemy()
{
    if (!arpg_on) {
        return;
    }

    arpg_refresh_list();

    Object* best = NULL;
    int bestDist = 0;
    for (int index = 0; index < crit_count; index++) {
        Object* critter = crit_list[index];
        if (critter == obj_dude || !arpg_alive(critter) || isPartyMember(critter)) {
            continue;
        }

        if (critter == player_unreachable && get_time() < player_unreachable_until) {
            continue;
        }

        int dist = obj_dist(obj_dude, critter);
        if (best == NULL || dist < bestDist) {
            best = critter;
            bestDist = dist;
        }
    }

    if (best == NULL) {
        arpg_print("ARPG: no critters on this level.");
        return;
    }

    // Find a free tile 5 hexes away from the critter in the direction of the
    // player.
    int rotation = tile_dir(best->tile, obj_dude->tile);
    for (int distance = 5; distance > 1; distance--) {
        int tile = tile_num_in_direction(best->tile, rotation, distance);
        if (obj_blocking_at(obj_dude, tile, best->elevation) == NULL) {
            register_clear(obj_dude);
            Rect rect;
            obj_move_to_tile(obj_dude, tile, best->elevation, &rect);
            tile_set_center(tile, TILE_SET_CENTER_REFRESH_WINDOW);
            arpg_log("ARPG: warped near %s at tile %d\n", critter_name(best), tile);
            return;
        }
    }
}

} // namespace fallout
