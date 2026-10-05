// ARPG HUD: Diablo-style health and energy globes drawn in the empty side
// areas next to the 640px interface bar, and Alt-to-show item labels.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "game/arpg.h"
#include "game/critter.h"
#include "game/gconfig.h"
#include "game/intface.h"
#include "game/item.h"
#include "game/map.h"
#include "game/object.h"
#include "game/proto.h"
#include "game/stat.h"
#include "game/textobj.h"
#include "game/tile.h"
#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/kb.h"
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

    if (hp != last_values[0] || maxHp != last_values[1]) {
        last_values[0] = hp;
        last_values[1] = maxHp;
        hud_draw_globe(globe_windows[0], std::max(hp, 0), maxHp, false, "LIFE");
    }

    if (en != last_values[2] || maxEn != last_values[3]) {
        last_values[2] = en;
        last_values[3] = maxEn;
        hud_draw_globe(globe_windows[1], en, maxEn, true, "ENERGY");
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
        // Diablo-style quality colors keyed off value.
        if (proto->item.cost >= 3000) {
            return rgb5(31, 22, 6); // unique gold
        }
        if (proto->item.cost >= 1000) {
            return rgb5(12, 16, 31); // magic blue
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
        }
        return;
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
}

void arpg_hud_reset()
{
    hud_destroy_windows();
    hud_hidden = false;
    labels_clear();
    labels_on = false;
    for (int index = 0; index < 4; index++) {
        last_values[index] = -1;
    }
}

} // namespace fallout
