#include "game/arpg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

// Energy (the blue globe) fuels special attacks.
static double energy = -1.0;
static unsigned int last_energy_tick = 0;
static bool player_special = false;

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

static void arpg_drop_loot(Object* victim);

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

    // Command-line test switches are merged into game_config, which CE saves
    // back to fallout.cfg on exit. Neutralize them so they don't stick.
    static const char* testKeys[] = { "autotest", "autoplay", "autotest_seconds", "autoshot_ms", "autotest_weapon", "autotest_ammo", "labels_always", "dump_items", "autotest_armor", "autotest_map", "autotest_xp" };
    for (const char* key : testKeys) {
        char* value;
        if (config_get_string(&game_config, ARPG_CONFIG_SECTION, key, &value)) {
            config_set_value(&game_config, ARPG_CONFIG_SECTION, key, strcmp(key, "autotest_weapon") == 0 || strcmp(key, "autotest_ammo") == 0 ? -1 : 0);
        }
    }

    arpg_log("ARPG: enabled=%d autotest=%d\n", arpg_on, arpg_test);
}

static void arpg_free_crit_list()
{
    if (crit_list != NULL) {
        obj_delete_list(crit_list);
        crit_list = NULL;
    }
    crit_count = 0;
}

void arpg_reset()
{
    if (!arpg_on) {
        return;
    }

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
    player_unreachable = NULL;
    alert_until = 0;
    next_list_refresh = 0;
    last_energy_tick = 0;
    arpg_hud_reset();
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
    strncpy(attackSfx, sfx, sizeof(attackSfx) - 1);
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

static bool arpg_start_attack(Object* attacker, Object* defender, int hitMode, int hitLocation = HIT_LOCATION_TORSO)
{
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
    aa->refs = 1;
    aa->fired = false;
    aa->hitQueued = false;
    aa->invalid = false;
    live_attacks.push_back(aa);

    stat_attacks++;

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

static void arpg_resolve(ArpgAttack* aa)
{
    Attack* attack = &(aa->attack);
    Object* attacker = attack->attacker;
    Object* defender = attack->defender;

    if (attacker == NULL || defender == NULL) {
        return;
    }

    // HP may have changed since the roll; recompute deaths.
    attack->defenderFlags &= ~DAM_DEAD;
    attack->attackerFlags &= ~DAM_DEAD;
    for (int index = 0; index < attack->extrasLength; index++) {
        attack->extrasFlags[index] &= ~DAM_DEAD;
    }
    death_checks(attack);

    bool hit = (attack->attackerFlags & DAM_HIT) != 0;

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
        critter_name(attacker),
        FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER ? critter_name(defender) : "obj",
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
    }

    if (defender != obj_dude && FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER) {
        if ((defender->data.critter.combat.results & DAM_DEAD) == 0) {
            defender->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
        }
    }

    if ((defender->data.critter.combat.results & DAM_DEAD) != 0 && defender != obj_dude) {
        stat_kills++;
        if ((attacker == obj_dude || isPartyMember(attacker)) && !isPartyMember(defender)) {
            arpg_drop_loot(defender);
        }
    }

    if (attacker == obj_dude && (defender->data.critter.combat.results & DAM_DEAD) != 0) {
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

void arpg_player_attack(Object* target)
{
    if (target == NULL || target == obj_dude) {
        return;
    }

    player_target = target;
    player_chasing = false;
    player_next_repath = 0;
    player_special = false;
    player_chase_failures = 0;
}

void arpg_player_special(Object* target)
{
    if (target == NULL || target == obj_dude) {
        return;
    }

    player_target = target;
    player_chasing = false;
    player_next_repath = 0;
    player_special = true;
    player_chase_failures = 0;
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
        if (weapon != NULL && !anim_busy(obj_dude) && item_w_try_reload(obj_dude, weapon) == 0) {
            intface_update_ammo_lights();
            const char* sfx = gsnd_build_weapon_sfx_name(WEAPON_SOUND_EFFECT_READY, weapon, hitMode, NULL);
            gsound_play_sfx_file(sfx);
            player_next_attack = now + 600;
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
                arpg_log("ARPG: can't reach %s (rc=%d dist=%d)\n", critter_name(player_target), rc, obj_dist(obj_dude, player_target));
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

    if (anim_busy(obj_dude) && !player_chasing) {
        return;
    }

    player_chasing = false;

    int hitLocation = HIT_LOCATION_TORSO;

    if (player_special) {
        // Right-click special: burst fire if the weapon has it, otherwise an
        // aimed shot at the head. Costs energy (10 per AP).
        player_special = false;

        int specialMode = hitMode;
        bool aimed = true;
        if (hitMode == HIT_MODE_LEFT_WEAPON_PRIMARY || hitMode == HIT_MODE_RIGHT_WEAPON_PRIMARY) {
            int secondary = hitMode + 1;
            int secondaryAnim = item_w_anim(obj_dude, secondary);
            if (secondaryAnim == ANIM_FIRE_BURST || secondaryAnim == ANIM_FIRE_CONTINUOUS) {
                specialMode = secondary;
                aimed = false;
            }
        }

        int cost = item_w_mp_cost(obj_dude, specialMode, aimed) * 10;
        if (arpg_energy() < cost) {
            arpg_float_text(obj_dude, "Need energy", colorTable[992]);
            gsound_play_sfx_file("iisxxxx1");
        } else {
            energy -= cost;
            hitMode = specialMode;
            if (aimed) {
                hitLocation = HIT_LOCATION_HEAD;
                arpg_float_text(obj_dude, "Aimed shot!", colorTable[992]);
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

static void arpg_drop_loot(Object* victim)
{
    if (!loot_drops) {
        return;
    }

    if (loot_table.empty()) {
        arpg_build_loot_table();
    }

    int xp = critter_kill_exps(victim);

    if (roll_random(1, 100) <= 55 * loot_pct / 100) {
        arpg_drop_item(victim, PROTO_ID_MONEY, roll_random(1, 3 + xp / 3));
    }

    if (roll_random(1, 100) <= 10 * loot_pct / 100) {
        arpg_drop_item(victim, PROTO_ID_STIMPACK, 1);
    }

    if (roll_random(1, 100) <= 20 * loot_pct / 100) {
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

    int gearChance = (8 + std::min(xp / 20, 15)) * loot_pct / 100;
    if (!loot_table.empty() && roll_random(1, 100) <= gearChance) {
        int budget = 300 + xp * 25;
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

    Object* critter = object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
    if (critter != NULL && arpg_is_attackable(critter)) {
        if (player_target != critter) {
            arpg_player_attack(critter);
        }
        return;
    }

    if (player_target != NULL) {
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
        actor.nextAttack = now + arpg_attack_delay(critter, hitMode);
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
        arpg_reset();
        last_map = mapIndex;
        last_elevation = map_elevation;
    }

    if (now >= next_list_refresh) {
        arpg_refresh_list();
        next_list_refresh = now + 1000;
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
            bool dumpItems = false;
            configGetBool(&game_config, ARPG_CONFIG_SECTION, "dump_items", &dumpItems);
            if (dumpItems) {
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

        if (autotest_seconds > 0 && now - autotest_start > (unsigned int)autotest_seconds * 1000) {
            arpg_log("ARPG autotest done: dude hp=%d/%d xp=%d attacks=%d failed=%d react_blocked=%d kills=%d\n",
                critter_get_hits(obj_dude),
                stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS),
                stat_pc_get(PC_STAT_EXPERIENCE),
                stat_attacks,
                stat_attack_fail,
                stat_react_blocked,
                stat_kills);
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
                    if (item_add_force(obj_dude, ammo, 5) == 0) {
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
            Object* stims;
            if (obj_pid_new(&stims, PROTO_ID_STIMPACK) == 0) {
                if (item_add_force(obj_dude, stims, 5) == 0) {
                    obj_disconnect(stims, NULL);
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

    if (autoplay) {
        // Test bot drinks a "potion" (Q) at low life.
        static unsigned int nextHeal = 0;
        if (now >= nextHeal && critter_get_hits(obj_dude) * 100 < stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS) * 40) {
            nextHeal = now + 2000;
            int before = critter_get_hits(obj_dude);
            arpg_quick_heal();
            arpg_log("ARPG autoplay: quick heal %d -> %d\n", before, critter_get_hits(obj_dude));
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

        if (player_target == NULL) {
            Object* best = NULL;
            for (int index = 0; index < crit_count; index++) {
                Object* critter = crit_list[index];
                if (arpg_is_attackable(critter) && obj_dist(critter, obj_dude) <= 12 && can_see(obj_dude, critter)
                    && !(critter == player_unreachable && now < player_unreachable_until)) {
                    if (best == NULL || obj_dist(critter, obj_dude) < obj_dist(best, obj_dude)) {
                        best = critter;
                    }
                }
            }
            if (best != NULL && arpg_energy() >= arpg_energy_max()) {
                arpg_player_special(best);
            } else if (best != NULL) {
                arpg_log("ARPG autoplay: targeting %s\n", critter_name(best));
                arpg_player_attack(best);
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

void arpg_quick_heal()
{
    if (!arpg_on) {
        return;
    }

    static const int pids[] = { PROTO_ID_SUPER_STIMPACK, PROTO_ID_STIMPACK };
    for (int pid : pids) {
        Object* item = inven_pid_is_carried_ptr(obj_dude, pid);
        if (item != NULL) {
            // Same sequence the combat AI uses for stimpaks.
            if (item_remove_mult(obj_dude, item, 1) == 0) {
                if (item_d_take_drug(obj_dude, item) == -1) {
                    item_add_force(obj_dude, item, 1);
                } else {
                    obj_connect(item, obj_dude->tile, obj_dude->elevation, NULL);
                    obj_destroy(item);
                    arpg_float_text(obj_dude, "+heal", colorTable[992]);
                }
            }
            intface_update_hit_points(true);
            intface_update_items(false);
            gsound_play_sfx_file("ib1p1xx1");
            return;
        }
    }

    arpg_print("No stimpaks.");
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
