// ARPG HUD: Diablo-style health and energy globes drawn in the empty side
// areas next to the 640px interface bar, and Alt-to-show item labels.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "game/arpg.h"
#include "game/art.h"
#include "game/critter.h"
#include "game/gconfig.h"
#include "game/gmouse.h"
#include "game/intface.h"
#include "game/inventry.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/party.h"
#include "game/proto.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"
#include "plib/gnw/text.h"

namespace fallout {

#define HUD_HEIGHT 100
#define GLOBE_RADIUS 44

static int globe_windows[2] = { -1, -1 };
static int globe_width = 0;
static int last_values[4] = { -1, -1, -1, -1 };
static int last_screen_width = 0;
static bool hud_hidden = false;

static std::vector<Object*> labeled_items;
static unsigned int next_label_refresh = 0;
static bool labels_on = false;
static bool highlights_on = false;

static inline int rgb5(int r, int g, int b)
{
    r = std::max(0, std::min(31, r));
    g = std::max(0, std::min(31, g));
    b = std::max(0, std::min(31, b));
    return colorTable[(r << 10) | (g << 5) | b];
}

static void hud_destroy_windows()
{
    for (int index = 0; index < 2; index++) {
        if (globe_windows[index] != -1) {
            win_delete(globe_windows[index]);
            globe_windows[index] = -1;
        }
    }
    for (int index = 0; index < 4; index++) {
        last_values[index] = -1;
    }
}

static bool hud_create_windows()
{
    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight();
    int sideWidth = (screenWidth - 640) / 2;
    if (sideWidth < GLOBE_RADIUS * 2 + 8) {
        return false;
    }

    globe_width = sideWidth;
    last_screen_width = screenWidth;
    globe_windows[0] = win_add(0, screenHeight - HUD_HEIGHT, sideWidth, HUD_HEIGHT, colorTable[0], 0);
    globe_windows[1] = win_add(screenWidth - sideWidth, screenHeight - HUD_HEIGHT, sideWidth, HUD_HEIGHT, colorTable[0], 0);
    return globe_windows[0] != -1 && globe_windows[1] != -1;
}

// Draws a liquid-filled glass globe. `blue` selects the energy palette.
static void hud_draw_globe(int win, int value, int maxValue, bool blue, const char* caption)
{
    unsigned char* buf = win_get_buf(win);
    if (buf == NULL) {
        return;
    }

    int width = globe_width;
    memset(buf, colorTable[0], width * HUD_HEIGHT);

    double frac = maxValue > 0 ? (double)value / maxValue : 0.0;
    frac = std::max(0.0, std::min(1.0, frac));

    int cx = width / 2;
    int cy = HUD_HEIGHT / 2;
    int r = GLOBE_RADIUS;
    int level = cy + r - (int)(2 * r * frac);

    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= HUD_HEIGHT) {
            continue;
        }
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= width) {
                continue;
            }

            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) {
                continue;
            }

            // Light from the upper left.
            double light = 0.55 - 0.45 * (dx + dy) / (1.414 * r);
            light = std::max(0.15, std::min(1.0, light));
            // Darken towards the rim for a spherical look.
            double edge = 1.0 - (double)d2 / (r * r);
            light *= 0.55 + 0.45 * sqrt(edge);

            int color;
            if (d2 >= (r - 2) * (r - 2)) {
                color = rgb5(16, 13, 8);
            } else if (y >= level) {
                if (blue) {
                    color = rgb5((int)(1 + 4 * light), (int)(3 + 8 * light), (int)(8 + 23 * light));
                } else {
                    color = rgb5((int)(6 + 25 * light), (int)(1 + 3 * light), (int)(1 + 2 * light));
                }
            } else {
                int v = (int)(1 + 4 * light);
                color = rgb5(v, v, v + 1);
            }

            // Specular highlight.
            int hx = x - (cx - r / 3);
            int hy = y - (cy - r / 2);
            if (hx * hx * 4 + hy * hy * 9 < (r / 3) * (r / 3) * 4) {
                color = rgb5(24, 24, 24);
            }

            buf[y * width + x] = (unsigned char)color;
        }
    }

    char text[32];
    snprintf(text, sizeof(text), "%d/%d", value, maxValue);

    int oldFont = text_curr();
    text_font(101);
    int textWidth = text_width(text);
    int textHeight = text_height();
    int tx = cx - textWidth / 2;
    int ty = cy - textHeight / 2;
    if (tx >= 0 && ty >= 0 && tx + textWidth < width) {
        // Drop shadow, then text.
        text_to_buf(buf + (ty + 1) * width + tx + 1, text, width - tx - 1, width, colorTable[0]);
        text_to_buf(buf + ty * width + tx, text, width - tx, width, colorTable[32767]);
    }

    int captionWidth = text_width(caption);
    int capX = cx - captionWidth / 2;
    int capY = ty + textHeight + 2;
    if (capX >= 0 && capY + textHeight < HUD_HEIGHT) {
        text_to_buf(buf + (capY + 1) * width + capX + 1, caption, width - capX - 1, width, colorTable[0]);
        text_to_buf(buf + capY * width + capX, caption, width - capX, width, rgb5(26, 26, 20));
    }
    text_font(oldFont);

    win_draw(win);
}

// D2 belt: how many of each flask (1-5 keys) are carried, in the free
// column between the life globe and the interface bar.
static const int belt_pids[5][2] = {
    { PROTO_ID_SUPER_STIMPACK, PROTO_ID_STIMPACK },
    { PROTO_ID_RADAWAY, 109 /* Rad-X */ },
    { PROTO_ID_BUFF_OUT, -1 },
    { PROTO_ID_MENTATS, -1 },
    { PROTO_ID_PSYCHO, PROTO_ID_NUKA_COLA },
};
static int last_belt[5] = { -1, -1, -1, -1, -1 };

static void hud_draw_belt(int win)
{
    unsigned char* buf = win_get_buf(win);
    if (buf == NULL) {
        return;
    }

    // Keys 1-5 top to bottom.
    static const char* names[] = { "Heal", "Rad", "Buff", "Ment", "Rage" };
    int width = globe_width;
    int columnX = width / 2 + GLOBE_RADIUS + 4;
    if (columnX + 36 > width) {
        return;
    }

    // The hotkey bar shows the same counts with item art.
    bool hotkeyBar = true;
    configGetBool(&game_config, "arpg", "hotkey_bar", &hotkeyBar);
    if (hotkeyBar) {
        return;
    }

    int oldFont = text_curr();
    text_font(101);
    int lineHeight = text_height() + 3;
    int top = (HUD_HEIGHT - lineHeight * 5) / 2;
    for (int slot = 0; slot < 5; slot++) {
        char text[24];
        snprintf(text, sizeof(text), "%s %d", names[slot], last_belt[slot]);
        int color = last_belt[slot] > 0 ? rgb5(10, 28, 10) : rgb5(10, 10, 10);
        int y = top + slot * lineHeight;
        unsigned char* row = buf + y * width + columnX;
        // Clear the row first; the globe redraw doesn't touch this column.
        for (int line = 0; line < lineHeight && y + line < HUD_HEIGHT; line++) {
            memset(buf + (y + line) * width + columnX, colorTable[0], width - columnX);
        }
        text_to_buf(row, text, width - columnX, width, color);
    }
    text_font(oldFont);
}

static bool hud_belt_changed()
{
    bool changed = false;
    for (int slot = 0; slot < 5; slot++) {
        int count = 0;
        for (int pid : belt_pids[slot]) {
            if (pid != -1) {
                count += inven_pid_quantity_carried(obj_dude, pid);
            }
        }
        if (count != last_belt[slot]) {
            last_belt[slot] = count;
            changed = true;
        }
    }
    return changed;
}

static void hud_update_globes()
{
    if (obj_dude == NULL) {
        return;
    }

    if (screenGetWidth() != last_screen_width && globe_windows[0] != -1) {
        hud_destroy_windows();
    }

    if (globe_windows[0] == -1) {
        if (!hud_create_windows()) {
            return;
        }
    }

    bool shouldHide = intface_is_hidden();
    if (shouldHide != hud_hidden) {
        hud_hidden = shouldHide;
        for (int index = 0; index < 2; index++) {
            if (shouldHide) {
                win_hide(globe_windows[index]);
            } else {
                win_show(globe_windows[index]);
            }
        }
        for (int index = 0; index < 4; index++) {
            last_values[index] = -1;
        }
    }

    if (hud_hidden) {
        return;
    }

    int hp = critter_get_hits(obj_dude);
    int maxHp = stat_level(obj_dude, STAT_MAXIMUM_HIT_POINTS);
    int en = arpg_energy();
    int maxEn = arpg_energy_max();

    bool beltChanged = hud_belt_changed();
    if (hp != last_values[0] || maxHp != last_values[1] || beltChanged) {
        last_values[0] = hp;
        last_values[1] = maxHp;
        hud_draw_globe(globe_windows[0], std::max(hp, 0), maxHp, false, "LIFE");
        hud_draw_belt(globe_windows[0]);
        win_draw(globe_windows[0]);
    }

    // Active shrine boosts, D2 state icons style, left of the energy globe.
    char buffs[ARPG_BUFF_COUNT][24];
    int buffCount = 0;
    unsigned int buffSignature = 0;
    for (int index = 0; index < ARPG_BUFF_COUNT; index++) {
        const char* name;
        int seconds = arpg_buff_seconds(index, &name);
        if (seconds > 0) {
            snprintf(buffs[buffCount++], sizeof(buffs[0]), "%s %d", name, seconds);
            buffSignature = buffSignature * 31 + index * 1000 + seconds;
        }
    }
    static unsigned int lastBuffSignature = 0;

    if (en != last_values[2] || maxEn != last_values[3] || buffSignature != lastBuffSignature) {
        last_values[2] = en;
        last_values[3] = maxEn;
        lastBuffSignature = buffSignature;
        hud_draw_globe(globe_windows[1], en, maxEn, true, "ENERGY");

        unsigned char* buf = win_get_buf(globe_windows[1]);
        int columnRight = globe_width / 2 - GLOBE_RADIUS - 4;
        if (buf != NULL && buffCount > 0 && columnRight > 30) {
            int oldFont = text_curr();
            text_font(101);
            int lineHeight = text_height() + 3;
            int top = (HUD_HEIGHT - lineHeight * buffCount) / 2;
            for (int index = 0; index < buffCount; index++) {
                int width = text_width(buffs[index]);
                int x = std::max(2, columnRight - width);
                text_to_buf(buf + (top + index * lineHeight) * globe_width + x, buffs[index], globe_width - x, globe_width, rgb5(6, 28, 31));
            }
            text_font(oldFont);
            win_draw(globe_windows[1]);
        }
    }
}

// ---------------------------------------------------------------------------
// Minimap (D2 / PoE overlay map, as a radar in the top-right corner): walls,
// loot, monsters coloured by rank, party members and the player.
// ---------------------------------------------------------------------------

#define MAP_WIDTH 200
#define MAP_HEIGHT 150
// Screen pixels per minimap pixel. A hex is 32x16 screen pixels.
#define MAP_SCALE 12

static int map_window = -1;
static unsigned int map_next_draw = 0;
static bool map_enabled = true;
static std::vector<int> map_walls;
static int map_walls_key = -1;

static void hud_destroy_map()
{
    if (map_window != -1) {
        win_delete(map_window);
        map_window = -1;
    }
}

// Walls never move, so they are collected once per level.
static void map_collect_walls()
{
    int key = map_get_index_number() * 4 + map_elevation;
    if (key == map_walls_key) {
        return;
    }
    map_walls_key = key;
    map_walls.clear();
    for (Object* obj = obj_find_first_at(map_elevation); obj != NULL; obj = obj_find_next_at()) {
        if (obj->tile == -1 || (obj->flags & OBJECT_HIDDEN) != 0) {
            continue;
        }
        int type = FID_TYPE(obj->fid);
        if (type == OBJ_TYPE_WALL || (type == OBJ_TYPE_SCENERY && (obj->flags & OBJECT_NO_BLOCK) == 0)) {
            map_walls.push_back(obj->tile);
        }
    }
}

static void map_plot(unsigned char* buf, int dudeX, int dudeY, int tile, int size, int color)
{
    int x;
    int y;
    if (tile_coord(tile, &x, &y, map_elevation) == -1) {
        return;
    }
    int px = MAP_WIDTH / 2 + (x - dudeX) / MAP_SCALE;
    int py = MAP_HEIGHT / 2 + (y - dudeY) / MAP_SCALE;
    for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
            int sx = px + dx - size / 2;
            int sy = py + dy - size / 2;
            if (sx > 0 && sy > 0 && sx < MAP_WIDTH - 1 && sy < MAP_HEIGHT - 1) {
                buf[sy * MAP_WIDTH + sx] = (unsigned char)color;
            }
        }
    }
}

static void hud_update_map(unsigned int now)
{
    if (!map_enabled || obj_dude == NULL || obj_dude->tile == -1 || intface_is_hidden() || !intface_is_enabled()) {
        hud_destroy_map();
        return;
    }

    if (map_window == -1) {
        map_window = win_add(screenGetWidth() - MAP_WIDTH - 6, 6, MAP_WIDTH, MAP_HEIGHT, colorTable[0], 0);
        if (map_window == -1) {
            return;
        }
        map_next_draw = 0;
    }

    if (now < map_next_draw) {
        return;
    }
    map_next_draw = now + 200;

    unsigned char* buf = win_get_buf(map_window);
    if (buf == NULL) {
        return;
    }

    map_collect_walls();

    int dudeX;
    int dudeY;
    tile_coord(obj_dude->tile, &dudeX, &dudeY, map_elevation);

    memset(buf, rgb5(1, 1, 2), MAP_WIDTH * MAP_HEIGHT);

    int wallColor = rgb5(11, 10, 8);
    for (int tile : map_walls) {
        map_plot(buf, dudeX, dudeY, tile, 2, wallColor);
    }

    // Loot and critters.
    for (Object* obj = obj_find_first_at(map_elevation); obj != NULL; obj = obj_find_next_at()) {
        if (obj->tile == -1 || (obj->flags & OBJECT_HIDDEN) != 0 || obj == obj_dude) {
            continue;
        }
        int type = FID_TYPE(obj->fid);
        if (type == OBJ_TYPE_ITEM && obj->owner == NULL) {
            map_plot(buf, dudeX, dudeY, obj->tile, 1, rgb5(28, 28, 28));
        } else if (type == OBJ_TYPE_SCENERY) {
            int shrine = arpg_shrine_state(obj);
            if (shrine != -1) {
                map_plot(buf, dudeX, dudeY, obj->tile, 4, shrine == 1 ? rgb5(6, 28, 31) : rgb5(4, 10, 12));
            }
        } else if (type == OBJ_TYPE_CRITTER && (obj->data.critter.combat.results & DAM_DEAD) == 0) {
            int rank = 0;
            char name[48];
            char mods[16];
            int color;
            if (isPartyMember(obj)) {
                color = rgb5(6, 28, 6);
            } else if (arpg_monster_info(obj, &rank, name, sizeof(name), mods, sizeof(mods)) && rank == 3) {
                color = rgb5(31, 24, 6);
            } else if (rank == 2) {
                color = rgb5(10, 14, 31);
            } else if (arpg_is_attackable(obj)) {
                color = rgb5(31, 4, 4);
            } else {
                color = rgb5(24, 22, 14);
            }
            map_plot(buf, dudeX, dudeY, obj->tile, rank >= 2 ? 4 : 3, color);
        }
    }

    // The player: a green cross.
    int cx = MAP_WIDTH / 2;
    int cy = MAP_HEIGHT / 2;
    for (int d = -3; d <= 3; d++) {
        buf[cy * MAP_WIDTH + cx + d] = (unsigned char)rgb5(8, 31, 8);
        buf[(cy + d) * MAP_WIDTH + cx] = (unsigned char)rgb5(8, 31, 8);
    }

    // Frame.
    int frame = rgb5(16, 13, 8);
    for (int x = 0; x < MAP_WIDTH; x++) {
        buf[x] = (unsigned char)frame;
        buf[(MAP_HEIGHT - 1) * MAP_WIDTH + x] = (unsigned char)frame;
    }
    for (int y = 0; y < MAP_HEIGHT; y++) {
        buf[y * MAP_WIDTH] = (unsigned char)frame;
        buf[y * MAP_WIDTH + MAP_WIDTH - 1] = (unsigned char)frame;
    }

    win_draw(map_window);
}

void arpg_hud_toggle_minimap()
{
    map_enabled = !map_enabled;
    if (!map_enabled) {
        hud_destroy_map();
    }
}

// ---------------------------------------------------------------------------
// D2 experience bar: a thin strip along the top of the interface bar.
// ---------------------------------------------------------------------------

#define XP_BAR_HEIGHT 4

static int xp_window = -1;
static int last_xp = -1;

static void hud_destroy_xp()
{
    if (xp_window != -1) {
        win_delete(xp_window);
        xp_window = -1;
    }
    last_xp = -1;
}

static void hud_update_xp()
{
    if (obj_dude == NULL || intface_is_hidden()) {
        hud_destroy_xp();
        return;
    }

    if (xp_window == -1) {
        int x = (screenGetWidth() - 640) / 2;
        xp_window = win_add(x, screenGetHeight() - HUD_HEIGHT - XP_BAR_HEIGHT, 640, XP_BAR_HEIGHT, colorTable[0], 0);
        if (xp_window == -1) {
            return;
        }
        last_xp = -1;
    }

    int xp = stat_pc_get(PC_STAT_EXPERIENCE);
    if (xp == last_xp) {
        return;
    }
    last_xp = xp;

    // Level L starts at 1000 * L(L-1)/2 XP (stat_pc_min_exp's table).
    int level = stat_pc_get(PC_STAT_LEVEL);
    int start = 1000 * (level - 1) * level / 2;
    int next = stat_pc_min_exp();
    double frac = next > start ? (double)(xp - start) / (next - start) : 1.0;
    frac = std::max(0.0, std::min(1.0, frac));

    unsigned char* buf = win_get_buf(xp_window);
    if (buf == NULL) {
        return;
    }
    int fill = (int)(638 * frac);
    for (int y = 0; y < XP_BAR_HEIGHT; y++) {
        for (int x = 0; x < 640; x++) {
            int color;
            if (y == 0 || y == XP_BAR_HEIGHT - 1 || x == 0 || x == 639) {
                color = rgb5(6, 5, 3);
            } else if (x <= fill) {
                color = y == 1 ? rgb5(31, 29, 18) : rgb5(26, 22, 8);
            } else {
                color = rgb5(3, 3, 2);
            }
            buf[y * 640 + x] = (unsigned char)color;
        }
    }
    win_draw(xp_window);
}

// ---------------------------------------------------------------------------
// Diablo II target bar: the monster under the cursor (or the one being
// attacked) at the top of the screen, its name coloured by rank (white,
// blue champion, gold unique), its modifiers and a draining life bar.
// ---------------------------------------------------------------------------

#define BAR_WIDTH 340
#define BAR_HEIGHT 36

static int bar_window = -1;
static Object* bar_critter = NULL;
static int bar_last_hp = -1;
static unsigned int bar_next_draw = 0;

static void bar_destroy()
{
    if (bar_window != -1) {
        win_delete(bar_window);
        bar_window = -1;
    }
    bar_critter = NULL;
    bar_last_hp = -1;
}

static Object* bar_focus()
{
    Object* critter = object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
    if (critter != NULL && critter != obj_dude && FID_TYPE(critter->fid) == OBJ_TYPE_CRITTER
        && (critter->data.critter.combat.results & DAM_DEAD) == 0
        && (critter->flags & OBJECT_HIDDEN) == 0) {
        return critter;
    }
    return arpg_player_target();
}

static void bar_text(unsigned char* buf, const char* text, int y, int color)
{
    char line[96];
    snprintf(line, sizeof(line), "%s", text);
    // Shorten from the end until it fits.
    while (text_width(line) > BAR_WIDTH - 8 && strlen(line) > 4) {
        size_t length = strlen(line);
        line[length - 4] = '.';
        line[length - 3] = '.';
        line[length - 2] = '\0';
    }
    int x = (BAR_WIDTH - text_width(line)) / 2;
    text_to_buf(buf + (y + 1) * BAR_WIDTH + x + 1, line, BAR_WIDTH - x - 1, BAR_WIDTH, colorTable[0]);
    text_to_buf(buf + y * BAR_WIDTH + x, line, BAR_WIDTH - x, BAR_WIDTH, color);
}

static void bar_draw(Object* critter)
{
    unsigned char* buf = win_get_buf(bar_window);
    if (buf == NULL) {
        return;
    }

    int rank;
    char name[64];
    char mods[160];
    arpg_monster_info(critter, &rank, name, sizeof(name), mods, sizeof(mods));

    int hp = std::max(0, critter_get_hits(critter));
    int maxHp = std::max(1, stat_level(critter, STAT_MAXIMUM_HIT_POINTS));
    double frac = std::min(1.0, (double)hp / maxHp);

    memset(buf, colorTable[0], BAR_WIDTH * BAR_HEIGHT);

    // Frame and life bar.
    int barTop = 2;
    int barBottom = 17;
    int barLeft = 2;
    int barRight = BAR_WIDTH - 3;
    int fill = barLeft + (int)((barRight - barLeft) * frac);
    for (int y = barTop; y <= barBottom; y++) {
        for (int x = barLeft; x <= barRight; x++) {
            int color;
            if (y == barTop || y == barBottom || x == barLeft || x == barRight) {
                color = rgb5(16, 13, 8);
            } else if (x < fill) {
                // Darker towards the bottom, a light line on top.
                int shade = y == barTop + 1 ? 30 : 24 - (y - barTop) / 2;
                color = rgb5(shade, 2, 2);
            } else {
                color = rgb5(5, 1, 1);
            }
            buf[y * BAR_WIDTH + x] = (unsigned char)color;
        }
    }

    int nameColor;
    switch (rank) {
    case 2:
        nameColor = rgb5(12, 16, 31); // champion blue
        break;
    case 3:
        nameColor = rgb5(31, 24, 8); // unique gold
        break;
    default:
        nameColor = colorTable[32767];
        break;
    }

    int oldFont = text_curr();
    text_font(101);
    int textHeight = text_height();
    bar_text(buf, name, barTop + (barBottom - barTop + 1 - textHeight) / 2, nameColor);
    if (mods[0] != '\0') {
        bar_text(buf, mods, barBottom + 3, rank == 3 ? rgb5(31, 24, 8) : rgb5(12, 16, 31));
    }
    text_font(oldFont);

    win_draw(bar_window);
}

static void hud_update_bar(unsigned int now)
{
    Object* critter = NULL;
    if (intface_is_enabled() && !intface_is_hidden()) {
        critter = bar_focus();
    }

    if (critter == NULL) {
        if (bar_window != -1) {
            bar_destroy();
        }
        return;
    }

    if (bar_window == -1) {
        int x = (screenGetWidth() - BAR_WIDTH) / 2;
        bar_window = win_add(x, 6, BAR_WIDTH, BAR_HEIGHT, colorTable[0], 0);
        if (bar_window == -1) {
            return;
        }
        bar_critter = NULL;
    }

    int hp = critter_get_hits(critter);
    if (critter != bar_critter || hp != bar_last_hp || now >= bar_next_draw) {
        bar_critter = critter;
        bar_last_hp = hp;
        bar_next_draw = now + 500;
        bar_draw(critter);
    }
}

// ---------------------------------------------------------------------------
// Hotkey bar (D2 belt + skill bar, PoE flask/skill slots): one keycap per
// bound key above the XP bar. Flasks show their item art and count, skills
// their energy cost; a slot greys out when it can't be used, flashes when its
// key is pressed, explains itself on hover, and flask slots can be clicked.
// ---------------------------------------------------------------------------

#define SLOT_SIZE 34
#define SLOT_GAP 2
#define GROUP_GAP 10
#define HOTBAR_MAX_SLOTS 12

enum HotSlotKind {
    HOT_FLASK,
    HOT_AIM_HEAD,
    HOT_AIM_LEGS,
    HOT_AIM_EYES,
    HOT_DODGE,
    HOT_SPECIAL,
    HOT_RELOAD,
    HOT_MODE,
};

struct HotSlot {
    int kind;
    int flask; // HOT_FLASK: belt index 0-4
    int key; // lowercase key code, -1 = mouse
    const char* keyName;
    int group;
    int x;
};

static int hotbar_window = -1;
static int hotbar_width = 0;
static int hotbar_screen[2] = { 0, 0 };
static int hotbar_scheme = -1;
static HotSlot hotbar_slots[HOTBAR_MAX_SLOTS];
static int hotbar_count = 0;
static unsigned int hotbar_signature = 0;
static unsigned int hotbar_next_check = 0;
static int hotbar_flash_key = -1;
static unsigned int hotbar_flash_until = 0;
static int tip_window = -1;
static int tip_slot = -1;
static unsigned int tip_signature = 0;

// What each slot can do right now (filled by hotbar_state).
struct HotState {
    int counts[5];
    int energy;
    int aimCost;
    int specialCost;
    bool burst;
    bool canAim;
    int dodgeCost;
    int weaponFid;
    int ammo;
    int maxAmmo;
    int ammoFid;
    int cursorMode;
};

static void hotbar_destroy_tip()
{
    if (tip_window != -1) {
        win_delete(tip_window);
        tip_window = -1;
    }
    tip_slot = -1;
}

static void hotbar_destroy()
{
    hotbar_destroy_tip();
    if (hotbar_window != -1) {
        win_delete(hotbar_window);
        hotbar_window = -1;
    }
    hotbar_scheme = -1;
    hotbar_signature = 0;
    hotbar_next_check = 0;
}

static void hotbar_add(int kind, int flask, int key, const char* keyName, int group)
{
    if (hotbar_count < HOTBAR_MAX_SLOTS) {
        hotbar_slots[hotbar_count++] = { kind, flask, key, keyName, group, 0 };
    }
}

// Slot layout of a control scheme; returns the bar width.
static int hotbar_layout(int scheme)
{
    hotbar_count = 0;
    if (scheme == ARPG_CONTROLS_CLASSIC) {
        hotbar_add(HOT_FLASK, 0, KEY_LOWERCASE_Q, "Q", 0);
        hotbar_add(HOT_RELOAD, 0, KEY_LOWERCASE_R, "R", 1);
    } else {
        static const char* digits[] = { "1", "2", "3", "4", "5" };
        for (int index = 0; index < 5; index++) {
            hotbar_add(HOT_FLASK, index, KEY_1 + index, digits[index], 0);
        }
        hotbar_add(HOT_AIM_HEAD, 0, KEY_LOWERCASE_Q, "Q", 1);
        hotbar_add(HOT_AIM_LEGS, 0, KEY_LOWERCASE_E, "E", 1);
        hotbar_add(HOT_AIM_EYES, 0, KEY_LOWERCASE_T, "T", 1);
        hotbar_add(HOT_DODGE, 0, KEY_SPACE, "Spc", 1);
        hotbar_add(HOT_SPECIAL, 0, -1, "RMB", 2);
        hotbar_add(HOT_RELOAD, 0, KEY_LOWERCASE_R, "R", 2);
        if (arpg_rmb_is_skill()) {
            hotbar_add(HOT_MODE, 0, KEY_LOWERCASE_F, "F", 2);
        }
    }

    int x = 0;
    for (int index = 0; index < hotbar_count; index++) {
        if (index > 0) {
            x += hotbar_slots[index].group != hotbar_slots[index - 1].group ? GROUP_GAP : SLOT_GAP;
        }
        hotbar_slots[index].x = x;
        x += SLOT_SIZE;
    }
    return x;
}

static void hotbar_state(HotState* state)
{
    for (int slot = 0; slot < 5; slot++) {
        int count = 0;
        for (int pid : belt_pids[slot]) {
            if (pid != -1) {
                count += inven_pid_quantity_carried(obj_dude, pid);
            }
        }
        state->counts[slot] = count;
    }

    state->energy = arpg_energy();
    state->dodgeCost = arpg_dodge_cost();
    arpg_skill_costs(&state->aimCost, &state->specialCost, &state->burst, &state->canAim);

    state->weaponFid = -1;
    state->ammo = -1;
    state->maxAmmo = 0;
    state->ammoFid = -1;
    state->cursorMode = gmouse_3d_get_mode();
    int hitMode;
    bool aiming;
    if (intface_get_attack(&hitMode, &aiming) != -1) {
        Object* weapon = item_hit_with(obj_dude, hitMode);
        if (weapon != NULL) {
            state->weaponFid = item_inv_fid(weapon);
            state->maxAmmo = item_w_max_ammo(weapon);
            if (state->maxAmmo > 0) {
                state->ammo = item_w_curr_ammo(weapon);
                Proto* proto;
                int ammoPid = item_w_ammo_pid(weapon);
                if (ammoPid != -1 && proto_ptr(ammoPid, &proto) != -1) {
                    state->ammoFid = proto->item.inventoryFid;
                }
            }
        }
    }
}

// False when the slot can't be used right now (drawn dimmed).
static bool hotbar_usable(const HotSlot* slot, const HotState* state)
{
    switch (slot->kind) {
    case HOT_FLASK:
        return state->counts[slot->flask] > 0;
    case HOT_AIM_HEAD:
    case HOT_AIM_LEGS:
    case HOT_AIM_EYES:
        return state->canAim && state->energy >= state->aimCost;
    case HOT_DODGE:
        return state->energy >= state->dodgeCost;
    case HOT_SPECIAL:
        return state->weaponFid != -1 && state->energy >= state->specialCost;
    case HOT_RELOAD:
        return state->maxAmmo > 0 && state->ammo < state->maxAmmo;
    }
    return true;
}

static int hotbar_cost(const HotSlot* slot, const HotState* state)
{
    switch (slot->kind) {
    case HOT_AIM_HEAD:
    case HOT_AIM_LEGS:
    case HOT_AIM_EYES:
        return state->canAim ? state->aimCost : 0;
    case HOT_DODGE:
        return state->dodgeCost;
    case HOT_SPECIAL:
        return state->weaponFid != -1 ? state->specialCost : 0;
    }
    return 0;
}

static void hotbar_text(unsigned char* buf, int pitch, int x, int y, const char* text, int color)
{
    text_to_buf(buf + (y + 1) * pitch + x + 1, text, pitch - x - 1, pitch, colorTable[0]);
    text_to_buf(buf + y * pitch + x, text, pitch - x, pitch, color);
}

// Aimed-shot icon: a solid silhouette with the targeted body part in red
// (Fallout's called-shot screen in miniature). H = head, E = eye band,
// B = body/arms, L = legs.
static const char* figure_rows[] = {
    "    HHH    ",
    "   HHHHH   ",
    "   EEEEE   ",
    "   HHHHH   ",
    "    HHH    ",
    "     B     ",
    "  BBBBBBB  ",
    " BBBBBBBBB ",
    " BB BBB BB ",
    " BB BBB BB ",
    " BB BBB BB ",
    " B  BBB  B ",
    "    BBB    ",
    "   LLLLL   ",
    "   LL LL   ",
    "   LL LL   ",
    "   LL LL   ",
    "   LL LL   ",
    "   LL LL   ",
    "  LLL LLL  ",
};

static void hotbar_figure(unsigned char* buf, int pitch, int cx, int top, int kind, bool usable)
{
    int body = usable ? rgb5(17, 16, 13) : rgb5(9, 9, 8);
    int hot = usable ? rgb5(31, 5, 3) : rgb5(15, 9, 8);
    int rows = (int)(sizeof(figure_rows) / sizeof(*figure_rows));
    for (int y = 0; y < rows; y++) {
        const char* row = figure_rows[y];
        int width = (int)strlen(row);
        for (int x = 0; x < width; x++) {
            char part = row[x];
            if (part == ' ') {
                continue;
            }
            bool target = (kind == HOT_AIM_HEAD && (part == 'H' || part == 'E'))
                || (kind == HOT_AIM_EYES && part == 'E')
                || (kind == HOT_AIM_LEGS && part == 'L');
            int color = target ? hot : body;
            // The eye band reads as a dark visor unless it is the target.
            if (part == 'E' && !target) {
                color = rgb5(6, 6, 5);
            }
            buf[(top + y) * pitch + cx - width / 2 + x] = (unsigned char)color;
        }
    }
}

static void hotbar_draw_slot(unsigned char* buf, const HotSlot* slot, const HotState* state, unsigned int now)
{
    int pitch = hotbar_width;
    int left = slot->x;
    bool usable = hotbar_usable(slot, state);
    bool flash = hotbar_flash_key == slot->key && now < hotbar_flash_until;

    // Bevelled keycap: light top/left edge, dark bottom/right, gold when pressed.
    for (int y = 0; y < SLOT_SIZE; y++) {
        for (int x = 0; x < SLOT_SIZE; x++) {
            int color;
            if (x == 0 || y == 0) {
                color = flash ? rgb5(31, 27, 10) : rgb5(18, 15, 10);
            } else if (x == SLOT_SIZE - 1 || y == SLOT_SIZE - 1) {
                color = flash ? rgb5(24, 18, 4) : rgb5(6, 5, 3);
            } else if (flash) {
                color = rgb5(10, 8, 3);
            } else {
                color = y < SLOT_SIZE / 2 ? rgb5(4, 4, 3) : rgb5(3, 3, 2);
            }
            buf[y * pitch + left + x] = (unsigned char)color;
        }
    }

    unsigned char* inner = buf + 2 * pitch + left + 2;
    int innerSize = SLOT_SIZE - 4;
    int cx = left + SLOT_SIZE / 2;
    int cy = SLOT_SIZE / 2 + 1;
    int dim = rgb5(12, 12, 10);
    const char* caption = NULL;

    switch (slot->kind) {
    case HOT_FLASK: {
        // The drug the key would use (the first one carried), else the slot's main drug.
        int pid = belt_pids[slot->flask][0];
        for (int candidate : belt_pids[slot->flask]) {
            if (candidate != -1 && inven_pid_quantity_carried(obj_dude, candidate) > 0) {
                pid = candidate;
                break;
            }
        }
        Proto* proto;
        if (proto_ptr(pid, &proto) != -1) {
            scale_art(proto->item.inventoryFid, inner, innerSize, innerSize, pitch);
        }
        break;
    }
    case HOT_AIM_HEAD:
    case HOT_AIM_LEGS:
    case HOT_AIM_EYES:
        // Left of centre and below the key letter, clear of the cost.
        hotbar_figure(buf, pitch, left + 10, 10, slot->kind, usable);
        break;
    case HOT_DODGE: {
        // Two chevrons: >>
        int color = usable ? rgb5(8, 26, 31) : dim;
        for (int chevron = 0; chevron < 2; chevron++) {
            int baseX = cx - 8 + chevron * 8;
            for (int d = -6; d <= 6; d++) {
                int x = baseX + 6 - abs(d);
                buf[(cy + d) * pitch + x] = (unsigned char)color;
                buf[(cy + d) * pitch + x + 1] = (unsigned char)color;
            }
        }
        break;
    }
    case HOT_SPECIAL:
        if (state->weaponFid != -1) {
            scale_art(state->weaponFid, inner, innerSize, innerSize, pitch);
        } else {
            caption = "Fist";
        }
        break;
    case HOT_RELOAD:
        if (state->ammoFid != -1) {
            scale_art(state->ammoFid, inner, innerSize, innerSize, pitch);
        } else {
            caption = "Reload";
        }
        break;
    case HOT_MODE:
        if (state->cursorMode == GAME_MOUSE_MODE_MOVE) {
            // A hex outline: the move/attack cursor.
            int color = rgb5(20, 26, 12);
            for (int d = 0; d <= 6; d++) {
                buf[(cy - 7 + d / 2) * pitch + cx - 7 + d] = (unsigned char)color;
                buf[(cy - 7 + d / 2) * pitch + cx + 7 - d] = (unsigned char)color;
                buf[(cy + 1 - d / 2) * pitch + cx - 7 + d] = (unsigned char)color;
                buf[(cy + 1 - d / 2) * pitch + cx + 7 - d] = (unsigned char)color;
            }
            caption = "Move";
        } else {
            // An arrow pointer: the use/talk/look cursor.
            int color = rgb5(31, 27, 12);
            for (int y = 0; y < 11; y++) {
                for (int x = 0; x <= y * 2 / 3; x++) {
                    buf[(cy - 11 + y) * pitch + cx - 4 + x] = (unsigned char)color;
                }
            }
            for (int d = 0; d < 4; d++) {
                buf[(cy - 1 + d) * pitch + cx - 1 + d / 2] = (unsigned char)color;
                buf[(cy - 1 + d) * pitch + cx + d / 2] = (unsigned char)color;
            }
            caption = "Use";
        }
        break;
    }

    if (!usable) {
        // Checker the slot dark, D2's "can't use" look.
        for (int y = 1; y < SLOT_SIZE - 1; y++) {
            for (int x = 1 + (y & 1); x < SLOT_SIZE - 1; x += 2) {
                buf[y * pitch + left + x] = (unsigned char)colorTable[0];
            }
        }
    }

    int textHeight = text_height();
    hotbar_text(buf, pitch, left + 3, 2, slot->keyName, flash ? rgb5(31, 31, 20) : rgb5(31, 27, 14));

    char text[16];
    text[0] = '\0';
    int textColor = rgb5(31, 31, 31);
    int cost = hotbar_cost(slot, state);
    if (slot->kind == HOT_FLASK) {
        snprintf(text, sizeof(text), "%d", state->counts[slot->flask]);
        textColor = state->counts[slot->flask] > 0 ? rgb5(31, 31, 31) : rgb5(20, 8, 8);
    } else if (slot->kind == HOT_RELOAD && state->maxAmmo > 0) {
        snprintf(text, sizeof(text), "%d", state->ammo);
        textColor = state->ammo > 0 ? rgb5(31, 31, 31) : rgb5(31, 8, 8);
    } else if (cost > 0) {
        // Energy cost, blue like the energy globe; red when short of it.
        snprintf(text, sizeof(text), "%d", cost);
        textColor = state->energy >= cost ? rgb5(12, 20, 31) : rgb5(31, 8, 8);
    }
    if (text[0] != '\0') {
        hotbar_text(buf, pitch, left + SLOT_SIZE - 3 - text_width(text), SLOT_SIZE - textHeight - 2, text, textColor);
    }

    if (caption != NULL) {
        int width = text_width(caption);
        hotbar_text(buf, pitch, left + (SLOT_SIZE - width) / 2, SLOT_SIZE - textHeight - 2, caption, usable ? rgb5(26, 26, 22) : rgb5(14, 14, 12));
    }
}

static void hotbar_tooltip_text(const HotSlot* slot, const HotState* state, char* out, size_t size)
{
    static const char* flaskNames[] = {
        "Heal: Super Stimpak, then Stimpak",
        "Radiation: RadAway, then Rad-X",
        "Strength: Buffout",
        "Mind: Mentats",
        "Rage: Psycho, then Nuka-Cola",
    };

    switch (slot->kind) {
    case HOT_FLASK:
        snprintf(out, size, "%s (%d) - click or press %s", flaskNames[slot->flask], state->counts[slot->flask], slot->keyName);
        break;
    case HOT_AIM_HEAD:
    case HOT_AIM_LEGS:
    case HOT_AIM_EYES: {
        const char* part = slot->kind == HOT_AIM_HEAD ? "head" : (slot->kind == HOT_AIM_LEGS ? "legs" : "eyes");
        if (!state->canAim) {
            snprintf(out, size, "%s: aimed shot at the %s - this weapon can't aim", slot->keyName, part);
        } else {
            snprintf(out, size, "%s: aimed shot at the %s of the enemy nearest the cursor (%d energy)", slot->keyName, part, state->aimCost);
        }
        break;
    }
    case HOT_DODGE:
        snprintf(out, size, "Space: dodge 3 hexes, briefly untouchable (%d energy)", state->dodgeCost);
        break;
    case HOT_SPECIAL:
        if (state->weaponFid == -1) {
            snprintf(out, size, "Right click: special attack (needs a weapon)");
        } else {
            snprintf(out, size, "Right click: %s at the enemy nearest the cursor (%d energy)", state->burst ? "burst fire" : "aimed head shot", state->specialCost);
        }
        break;
    case HOT_RELOAD:
        if (state->maxAmmo > 0) {
            snprintf(out, size, "R: reload (%d/%d)", state->ammo, state->maxAmmo);
        } else {
            snprintf(out, size, "R: reload (this weapon takes no ammo)");
        }
        break;
    case HOT_MODE:
        if (state->cursorMode == GAME_MOUSE_MODE_MOVE) {
            snprintf(out, size, "F: cursor is move/attack - F switches to use/talk/look");
        } else {
            snprintf(out, size, "F: cursor is use/talk/look - F or right click switches back");
        }
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static int hotbar_hover_slot(int barX, int barY, unsigned int now)
{
    // Test hook: `autotest_hover=1` walks the tooltip across the slots.
    int testHover = 0;
    if (arpg_autotest() && config_get_value(&game_config, "arpg", "autotest_hover", &testHover) && testHover == 1) {
        return (int)((now / 1500) % hotbar_count);
    }

    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    if (mouseY < barY || mouseY >= barY + SLOT_SIZE) {
        return -1;
    }
    for (int index = 0; index < hotbar_count; index++) {
        int left = barX + hotbar_slots[index].x;
        if (mouseX >= left && mouseX < left + SLOT_SIZE) {
            return index;
        }
    }
    return -1;
}

static void hotbar_update_tip(int hover, int barX, int barY, const HotState* state, unsigned int signature)
{
    if (hover == -1) {
        hotbar_destroy_tip();
        return;
    }
    if (hover == tip_slot && signature == tip_signature && tip_window != -1) {
        return;
    }
    hotbar_destroy_tip();

    char text[128];
    hotbar_tooltip_text(&hotbar_slots[hover], state, text, sizeof(text));
    if (text[0] == '\0') {
        return;
    }

    int oldFont = text_curr();
    text_font(101);
    int width = text_width(text) + 10;
    int height = text_height() + 6;
    int x = barX + hotbar_slots[hover].x + SLOT_SIZE / 2 - width / 2;
    x = std::max(0, std::min(screenGetWidth() - width, x));
    int y = barY - height - 3;
    tip_window = win_add(x, y, width, height, colorTable[0], 0);
    if (tip_window != -1) {
        unsigned char* buf = win_get_buf(tip_window);
        if (buf != NULL) {
            int frame = rgb5(16, 13, 8);
            for (int px = 0; px < width; px++) {
                buf[px] = (unsigned char)frame;
                buf[(height - 1) * width + px] = (unsigned char)frame;
            }
            for (int py = 0; py < height; py++) {
                buf[py * width] = (unsigned char)frame;
                buf[py * width + width - 1] = (unsigned char)frame;
            }
            text_to_buf(buf + 3 * width + 5, text, width - 6, width, rgb5(28, 28, 24));
        }
        win_draw(tip_window);
        tip_slot = hover;
        tip_signature = signature;
    }
    text_font(oldFont);
}

static void hud_update_hotbar(unsigned int now)
{
    bool enabled = true;
    configGetBool(&game_config, "arpg", "hotkey_bar", &enabled);
    if (!enabled || obj_dude == NULL || intface_is_hidden() || !intface_is_enabled()) {
        if (hotbar_window != -1) {
            hotbar_destroy();
        }
        return;
    }

    int scheme = arpg_controls();
    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight();
    if (hotbar_window != -1 && (scheme != hotbar_scheme || screenWidth != hotbar_screen[0] || screenHeight != hotbar_screen[1])) {
        hotbar_destroy();
    }

    int barX = (screenWidth - hotbar_width) / 2;
    int barY = screenHeight - HUD_HEIGHT - XP_BAR_HEIGHT - SLOT_SIZE - 2;

    if (hotbar_window == -1) {
        hotbar_width = hotbar_layout(scheme);
        barX = (screenWidth - hotbar_width) / 2;
        hotbar_window = win_add(barX, barY, hotbar_width, SLOT_SIZE, colorTable[0], 0);
        if (hotbar_window == -1) {
            return;
        }
        hotbar_scheme = scheme;
        hotbar_screen[0] = screenWidth;
        hotbar_screen[1] = screenHeight;

        // Clicking a flask (or the reload slot) sends its key, like D2's belt.
        for (int index = 0; index < hotbar_count; index++) {
            const HotSlot* slot = &hotbar_slots[index];
            if (slot->kind == HOT_FLASK || slot->kind == HOT_RELOAD || slot->kind == HOT_MODE) {
                win_register_button(hotbar_window, slot->x, 0, SLOT_SIZE, SLOT_SIZE, -1, -1, -1, slot->key, NULL, NULL, NULL, 0);
            }
        }
    }

    if (now < hotbar_next_check) {
        return;
    }
    hotbar_next_check = now + 100;

    HotState state;
    hotbar_state(&state);

    int hover = hotbar_hover_slot(barX, barY, now);
    bool flashing = now < hotbar_flash_until;

    unsigned int signature = 17;
    for (int count : state.counts) {
        signature = signature * 31 + count;
    }
    for (int index = 0; index < hotbar_count; index++) {
        signature = signature * 31 + (hotbar_usable(&hotbar_slots[index], &state) ? 1 : 2);
    }
    signature = signature * 31 + state.aimCost;
    signature = signature * 31 + state.specialCost;
    signature = signature * 31 + (state.burst ? 1 : 0) + (state.canAim ? 2 : 0);
    signature = signature * 31 + state.weaponFid;
    signature = signature * 31 + state.ammo;
    signature = signature * 31 + state.ammoFid;
    signature = signature * 31 + state.cursorMode;
    unsigned int tipSignature = signature;
    signature = signature * 31 + (flashing ? hotbar_flash_key + 1 : 0);

    if (signature != hotbar_signature) {
        hotbar_signature = signature;
        unsigned char* buf = win_get_buf(hotbar_window);
        if (buf != NULL) {
            memset(buf, colorTable[0], hotbar_width * SLOT_SIZE);
            int oldFont = text_curr();
            text_font(101);
            for (int index = 0; index < hotbar_count; index++) {
                hotbar_draw_slot(buf, &hotbar_slots[index], &state, now);
            }
            text_font(oldFont);
            win_draw(hotbar_window);
        }
    }

    hotbar_update_tip(hover, barX, barY, &state, tipSignature);
}

void arpg_hud_key_pressed(int key)
{
    if (key >= 'A' && key <= 'Z') {
        key += 'a' - 'A';
    }
    for (int index = 0; index < hotbar_count; index++) {
        if (hotbar_slots[index].key == key) {
            hotbar_flash_key = key;
            hotbar_flash_until = get_time() + 180;
            // Redraw on the next tick.
            hotbar_next_check = 0;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Alt item labels
// ---------------------------------------------------------------------------

static void labels_clear()
{
    for (Object* item : labeled_items) {
        text_object_remove(item);
    }
    labeled_items.clear();
}

bool arpg_loot_highlights_visible()
{
    return highlights_on;
}

// Removes loot outlines from every item on the map. Walks the live object list
// (never a cached pointer), so picked-up/destroyed items are safe.
static void highlights_clear()
{
    Object** items = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_ITEM, &items);
    for (int index = 0; index < count; index++) {
        Object* item = items[index];
        if ((item->outline & OUTLINE_TYPE_ARPG_LOOT) != 0) {
            Rect rect;
            obj_remove_outline(item, &rect);
            tile_refresh_rect(&rect, item->elevation);
        }
    }
    if (count > 0) {
        obj_delete_list(items);
    }
}

static int label_color(Object* item)
{
    if (item->pid == PROTO_ID_MONEY) {
        return rgb5(31, 27, 8);
    }

    Proto* proto;
    if (proto_ptr(item->pid, &proto) == -1) {
        return colorTable[32767];
    }

    switch (proto->item.type) {
    case ITEM_TYPE_AMMO:
        return rgb5(20, 20, 20);
    case ITEM_TYPE_DRUG:
        return rgb5(10, 30, 10);
    case ITEM_TYPE_WEAPON:
    case ITEM_TYPE_ARMOR:
        // Diablo II colors: gold unique, yellow rare, blue magic, white.
        switch (arpg_item_quality(item)) {
        case 3:
            return rgb5(31, 22, 6);
        case 2:
            return rgb5(31, 31, 8);
        case 1:
            return rgb5(12, 16, 31);
        }
        return colorTable[32767];
    default:
        return rgb5(26, 26, 26);
    }
}

static void labels_refresh()
{
    labels_clear();

    Object** items = NULL;
    int count = obj_create_list(-1, map_elevation, OBJ_TYPE_ITEM, &items);
    if (count <= 0) {
        return;
    }

    std::vector<Object*> candidates;
    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight() - HUD_HEIGHT;
    for (int index = 0; index < count; index++) {
        Object* item = items[index];
        if (item->owner != NULL || (item->flags & OBJECT_HIDDEN) != 0 || item->tile == -1) {
            continue;
        }

        int x;
        int y;
        tile_coord(item->tile, &x, &y, item->elevation);
        if (x < 0 || y < 0 || x >= screenWidth || y >= screenHeight) {
            continue;
        }

        candidates.push_back(item);

        // D2-style loot highlight: outline every item on screen in its tier colour.
        if (highlights_on) {
            int outline = OUTLINE_TYPE_ARPG_LOOT | (label_color(item) & 0xFF);
            if (item->outline != outline) {
                Rect rect;
                if (obj_outline_object(item, outline, &rect) == 0) {
                    tile_refresh_rect(&rect, item->elevation);
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](Object* a, Object* b) {
        return obj_dist(obj_dude, a) < obj_dist(obj_dude, b);
    });

    // Leave room in the 20-slot text object pool for damage numbers.
    size_t limit = std::min<size_t>(candidates.size(), 12);
    for (size_t index = 0; index < limit; index++) {
        Object* item = candidates[index];
        char text[64];
        if (item->pid == PROTO_ID_MONEY) {
            snprintf(text, sizeof(text), "%d caps", item_caps_get_amount(item));
        } else {
            snprintf(text, sizeof(text), "%s", item_name(item));
        }

        Rect rect;
        if (text_object_create(item, text, 101, label_color(item), colorTable[0], &rect) == 0) {
            tile_refresh_rect(&rect, item->elevation);
            labeled_items.push_back(item);
        }
    }

    obj_delete_list(items);
}

static void hud_update_labels(unsigned int now)
{
    bool altDown = keys[SDL_SCANCODE_LALT] != 0 || keys[SDL_SCANCODE_RALT] != 0;
    bool always = arpg_labels_always();
    bool wanted = (altDown || always) && !intface_is_hidden();

    if (!wanted) {
        if (labels_on) {
            labels_clear();
            labels_on = false;
            if (highlights_on) {
                highlights_on = false;
                highlights_clear();
            }
        }
        return;
    }

    if (!labels_on) {
        bool highlight = true;
        configGetBool(&game_config, "arpg", "loot_highlight", &highlight);
        highlights_on = highlight;
    }

    // Drop labels of items that were picked up.
    for (auto it = labeled_items.begin(); it != labeled_items.end();) {
        if ((*it)->owner != NULL) {
            text_object_remove(*it);
            it = labeled_items.erase(it);
        } else {
            ++it;
        }
    }

    if (!labels_on || now >= next_label_refresh) {
        labels_on = true;
        next_label_refresh = now + 1500;
        labels_refresh();
    }
}

void arpg_hud_tick(unsigned int now)
{
    bool hud = true;
    configGetBool(&game_config, "arpg", "globes", &hud);
    if (hud) {
        hud_update_globes();
    } else if (globe_windows[0] != -1) {
        hud_destroy_windows();
    }

    hud_update_labels(now);
    hud_update_bar(now);
    if (hud) {
        hud_update_xp();
    } else {
        hud_destroy_xp();
    }
    hud_update_hotbar(now);

    static bool mapConfigRead = false;
    if (!mapConfigRead) {
        mapConfigRead = true;
        configGetBool(&game_config, "arpg", "minimap", &map_enabled);
    }
    hud_update_map(now);
}

void arpg_hud_reset()
{
    hud_destroy_windows();
    bar_destroy();
    hud_destroy_xp();
    hud_destroy_map();
    hotbar_destroy();
    map_walls_key = -1;
    for (int slot = 0; slot < 5; slot++) {
        last_belt[slot] = -1;
    }
    hud_hidden = false;
    labels_clear();
    labels_on = false;
    highlights_on = false;
    for (int index = 0; index < 4; index++) {
        last_values[index] = -1;
    }
}

} // namespace fallout
