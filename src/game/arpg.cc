#include "game/arpg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <math.h>

#include <algorithm>
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
} ArpgAttack;

typedef struct ArpgActor {
    unsigned int nextAttack;
    unsigned int nextThink;
    unsigned int nextRepath;
    unsigned int knockdownTime;
    Object* target;
    bool triedWeapon;
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

static bool arpg_on = false;
static bool arpg_test = false;
static int enemy_delay_pct = 130;
static int player_delay_pct = 100;
static int stagger_pct = 8;
static int player_stagger_pct = 15;
static int stagger_immunity_ms = 2000;
static int autotest_xp = 0;
static std::unordered_map<Object*, unsigned int> stagger_until;
static int leash_distance = 30;
static bool show_numbers = true;
static bool always_run = true;
static bool labels_always = false;
static bool hold_to_move = true;
static bool loot_drops = true;
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

// Run-and-gun metrics (autotest).
static unsigned int player_target_time = 0;
static int stat_shots_while_wasd = 0;
static unsigned int stat_latency_total = 0;
static int stat_latency_count = 0;
static int stat_unfired = 0;

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
    aa->fired = true;
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

bool arpg_is_spawned(Object* obj)
{
    return obj != NULL && (obj->flags & ARPG_OBJECT_SPAWNED) != 0;
}

static int arpg_monster_speed_pct(Object* critter);

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
        pct = dude ? attack_anim_pct : enemy_attack_anim_pct;
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
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "loot_pct", &loot_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "energy_regen_pct", &energy_regen_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "dodge_cost", &dodge_cost);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "attack_anim_pct", &attack_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "aim_anim_pct", &aim_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "enemy_attack_anim_pct", &enemy_attack_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "reaction_anim_pct", &reaction_anim_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "run_speed_pct", &run_speed_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "enemy_run_speed_pct", &enemy_run_speed_pct);
    config_get_value(&game_config, ARPG_CONFIG_SECTION, "projectile_speed_pct", &projectile_speed_pct);
    configGetBool(&game_config, ARPG_CONFIG_SECTION, "packs", &packs_enabled);
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
    static const char* testKeys[] = { "autotest", "autoplay", "autotest_seconds", "autoshot_ms", "autotest_weapon", "autotest_ammo", "labels_always", "dump_items", "autotest_armor", "autotest_map", "autotest_xp", "autotest_wasd", "autotest_iframes", "autotest_god" };
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
    populated_levels.clear();
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
    monsters.erase(critter);
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
    return monster != NULL ? monster->speedPct : 100;
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
        arpg_unique_name(monster.name, sizeof(monster.name));
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

static Object* arpg_spawn_clone(Object* seed)
{
    int tile = arpg_pack_tile(seed);
    if (tile == -1) {
        return NULL;
    }

    int fid = art_id(OBJ_TYPE_CRITTER, seed->fid & 0xFFF, ANIM_STAND, (seed->fid & 0xF000) >> 12, seed->rotation + 1);
    Object* clone;
    if (obj_new(&clone, fid, seed->pid) == -1) {
        return NULL;
    }

    // Never saved: packs are rolled again when the map is entered again.
    clone->flags |= OBJECT_NO_SAVE | ARPG_OBJECT_SPAWNED;
    clone->data.critter.combat.team = seed->data.critter.combat.team;
    clone->data.critter.combat.aiPacket = seed->data.critter.combat.aiPacket;

    Rect rect;
    obj_set_rotation(clone, roll_random(0, ROTATION_COUNT - 1), NULL);
    if (obj_move_to_tile(clone, tile, seed->elevation, &rect) == 0) {
        tile_refresh_rect(&rect, seed->elevation);
    }

    stat_spawned++;
    return clone;
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

    // Packs don't respawn until some game time has passed since they were
    // last spawned here (loading an older save always respawns).
    int hour = (int)(game_time() / 36000);
    auto last = populated_hour.find(key);
    if (last != populated_hour.end() && hour >= last->second && hour - last->second < pack_respawn_hours) {
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
        seeds.push_back(critter);
    }

    int spawned = 0;
    int champions = 0;
    int uniques = 0;
    for (Object* seed : seeds) {
        if (spawned >= max_spawns) {
            break;
        }

        int pack = next_pack_id++;
        arpg_make_monster(seed, RANK_NORMAL, 0, pack, NULL);

        // Big monsters come in smaller groups (D2 MonStats MinGrp/MaxGrp).
        int xp = critter_kill_exps(seed);
        int extra;
        if (xp >= 500) {
            extra = roll_random(0, 1);
        } else if (xp >= 200) {
            extra = roll_random(1, 2);
        } else {
            extra = roll_random(pack_min, std::max(pack_min, pack_max));
        }
        extra = extra * pack_density_pct / 100;

        int roll = roll_random(1, 100);
        if (roll <= unique_pct) {
            Object* boss = arpg_spawn_clone(seed);
            if (boss != NULL) {
                spawned++;
                uniques++;
                arpg_make_monster(boss, RANK_UNIQUE, arpg_roll_affixes(xp >= 300 ? 3 : 2), pack, NULL);
                monsters[seed].leader = boss;
                int minions = std::max(2, extra);
                for (int index = 0; index < minions && spawned < max_spawns; index++) {
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
        } else if (roll <= unique_pct + champion_pct) {
            unsigned int kind = champion_kinds[roll_random(0, (int)(sizeof(champion_kinds) / sizeof(*champion_kinds)) - 1)];
            int count = roll_random(2, 3);
            for (int index = 0; index < count && spawned < max_spawns; index++) {
                Object* champion = arpg_spawn_clone(seed);
                if (champion != NULL) {
                    spawned++;
                    champions++;
                    arpg_make_monster(champion, RANK_CHAMPION, kind, pack, NULL);
                }
            }
        } else {
            for (int index = 0; index < extra && spawned < max_spawns; index++) {
                Object* clone = arpg_spawn_clone(seed);
                if (clone != NULL) {
                    spawned++;
                    arpg_make_monster(clone, RANK_NORMAL, 0, pack, NULL);
                }
            }
        }
    }

    arpg_log("ARPG: populated level %d: %d seeds, %d spawned (%d champions, %d uniques)\n",
        key, (int)seeds.size(), spawned, champions, uniques);

    if (spawned > 0) {
        arpg_refresh_list();
    }
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

    if (ranged && weapon != NULL && anim != ANIM_THROW_ANIM) {
        int projectilePid = item_w_proj_pid(weapon);
        Proto* projectileProto;
        if (proto_ptr(projectilePid, &projectileProto) != -1 && projectileProto->fid != -1) {
            if (obj_new(&projectile, projectileProto->fid, -1) != -1) {
                obj_turn_off(projectile, NULL);
                obj_set_light(projectile, 9, projectile->lightIntensity, NULL);

                int origin = combat_bullet_start(attacker, defender);
                obj_move_to_tile(projectile, origin, attacker->elevation, NULL);
                obj_set_rotation(projectile, tile_dir(attacker->tile, defender->tile), NULL);

                register_object_funset(projectile, OBJECT_HIDDEN, 0);

                const char* flyingSfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_AMMO_FLYING, weapon, attack->hitMode, defender);
                register_object_play_sfx(projectile, flyingSfx, 0);

                int destination = (attack->attackerFlags & DAM_HIT) != 0 ? defender->tile : attack->tile;
                register_object_move_straight_to_tile(projectile, destination, defender->elevation, ANIM_WALK, 0);
                register_object_call(aa, NULL, arpg_cb_hit, -1);
                register_object_must_erase(projectile);
            } else {
                projectile = NULL;
                register_object_call(aa, NULL, arpg_cb_hit, 0);
            }
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
        arpg_react(&blast, critter, blast.defenderDamage, blast.defenderFlags, 0);
        apply_damage(&blast, false);

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

    if (byPlayer && !isPartyMember(victim)) {
        arpg_drop_loot(victim, rolls, gear);
    }
}

static void arpg_resolve(ArpgAttack* aa)
{
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

    if (hit && defender == obj_dude && get_time() < dodge_until) {
        // Dodge roll i-frames: the blow lands on empty air.
        attack->attackerFlags &= ~(DAM_HIT | DAM_CRITICAL);
        attack->defenderFlags = 0;
        attack->defenderDamage = 0;
        attack->defenderKnockback = 0;
        hit = false;
        arpg_log("ARPG: dodged %s\n", critter_name(attacker));
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
        combat_display(attack);
    }

    if (hit) {
        arpg_react(attack, defender, attack->defenderDamage, attack->defenderFlags, attack->defenderKnockback);
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        arpg_react(attack, attack->extras[index], attack->extrasDamage[index], attack->extrasFlags[index], attack->extrasKnockback[index]);
    }

    apply_damage(attack, true);

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
            arpg_float_text(defender, text, color);
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
            register_end();
        }
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
            player_next_attack = now + arpg_attack_delay(obj_dude, hitMode);
        }
        player_target = NULL;
        return;
    }

    if (arpg_start_attack(obj_dude, player_target, hitMode)) {
        player_next_attack = now + arpg_attack_delay(obj_dude, hitMode);
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
                    arpg_log("ARPG: %s dropped %s\n", critter_name(victim), item_name(item));
                }
            }
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
        if (!anim_busy(critter) || now >= actor.nextRepath) {
            arpg_move_towards(critter, target);
            actor.nextRepath = now + 1200;
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
    if (!arpg_on || obj_dude == NULL) {
        return;
    }

    unsigned int now = get_time();

    int mapIndex = map_get_index_number();
    if (mapIndex != last_map || map_elevation != last_elevation) {
        arpg_reset_level();
        last_map = mapIndex;
        last_elevation = map_elevation;
        // Spawn packs once the level has settled (scripts may still be
        // placing or removing critters on the first frames).
        populate_pending = true;
        populate_time = now + 500;
    }

    if (now >= next_list_refresh) {
        arpg_refresh_list();
        next_list_refresh = now + 1000;
    }

    if (populate_pending && now >= populate_time) {
        populate_pending = false;
        arpg_populate_level();
    }

    // Resolve hits queued by animation callbacks.
    if (!pending_hits.empty()) {
        std::vector<ArpgAttack*> hits;
        hits.swap(pending_hits);
        for (ArpgAttack* aa : hits) {
            if (!aa->invalid) {
                arpg_resolve(aa);
            }
            arpg_release(aa);
        }
    }

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
            }
            autotest_start = now;
            arpg_log("ARPG autotest start, map %d elevation %d\n", map_get_index_number(), map_elevation);
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
        if (now >= nextCensus) {
            nextCensus = now + 10000;
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
            arpg_log("ARPG autotest done: unfired=%d spawned=%d\n", stat_unfired, stat_spawned);
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
            game_user_wants_to_quit = 2;
        }
    }

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
            arpg_log("ARPG autotest: armed with %s\n", item_name(weapon));
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
        if (now >= nextHeal && critter_get_hits(obj_dude) * 100 < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS) * 40) {
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

    // The player gets up after a knockdown, like critters do. Vanilla handles
    // this at the start of a combat turn, which never comes in real time.
    if ((obj_dude->data.critter.combat.results & DAM_KNOCKED_DOWN) != 0
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

    arpg_energy_tick(now);
    arpg_wasd_tick(now);
    arpg_hold_tick(now);
    arpg_player_tick(now);
    arpg_hud_tick(now);

    if ((obj_dude->data.critter.combat.results & DAM_KNOCKED_OUT) != 0
        && (obj_dude->data.critter.combat.results & DAM_DEAD) == 0) {
        obj_dude->data.critter.combat.results &= ~DAM_KNOCKED_OUT;
        obj_dude->data.critter.combat.results |= DAM_KNOCKED_DOWN;
        queue_remove_this(obj_dude, EVENT_TYPE_KNOCKOUT);
        arpg_log("ARPG: player knockout downgraded to knockdown\n");
    }

    arpg_aggro_tick(now);

    bool alert = now < alert_until;
    bool joinCheck = alert && now >= next_join_check;
    if (joinCheck) {
        next_join_check = now + 1500;
    }

    bool anyEngaged = false;

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
        intface_update_hit_points(true);
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

    if (!arpg_dude_can_move()) {
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

    if (arpg_test && autotest_wasd >= 0 && autotest_wasd <= 3 && autotest_start != 0) {
        static const int dirs[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
        // Hold the test direction for 3 s, then let go for 1 s.
        if ((get_time() - autotest_start) % 4000 < 3000) {
            *dx = dirs[autotest_wasd][0];
            *dy = dirs[autotest_wasd][1];
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

static void arpg_wasd_tick(unsigned int now)
{
    if (controls != ARPG_CONTROLS_WASD) {
        wasd_moving = false;
        return;
    }

    arpg_camera_follow();

    int dx;
    int dy;
    arpg_wasd_dir(&dx, &dy);

    if ((dx == 0 && dy == 0) || !arpg_input_ready()) {
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

    bool turned = dx != wasd_dx || dy != wasd_dy;
    bool idle = !anim_busy(obj_dude);
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
