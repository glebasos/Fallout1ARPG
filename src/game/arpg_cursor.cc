// ARPG: screen-space move/attack cursor (`[arpg] screen_cursor`).
//
// Vanilla move mode has no pointer at all: the cursor is a red hex outline
// drawn on the floor (obj_mouse_flat) that snaps tile by tile, which is hard
// to see and hard to aim with in real time. Here move mode gets a regular,
// pixel-exact mouse pointer instead (built at runtime, no art files) and the
// floor hex is skipped by the object renderer. The pointer shows what a left
// click would do, with the same rules the click uses:
//   white arrow   - move
//   gold arrow    - pick up the item under it
//   red reticle   - attack (the critter under it, or the one a WASD/Shift
//                   attack-in-place would snap to)
// Clicks never used the hex itself (check_move, object_under_mouse and
// arpg_target_near_cursor all read the mouse position), so aiming is
// unchanged apart from being visible.
//
// Ownership: gmouse only re-sets its own shape when the cursor type changes,
// so while it sits on MOUSE_CURSOR_NONE (move mode over the map) the shape is
// ours. gmouse_set_cursor calls arpg_cursor_on_set after every change.

#include "game/arpg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game/gmouse.h"
#include "game/map.h"
#include "game/object.h"
#include "plib/color/color.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/mouse.h"

namespace fallout {

enum {
    CURSOR_NONE,
    CURSOR_MOVE,
    CURSOR_LOOT,
    CURSOR_ATTACK,
    CURSOR_COUNT,
};

#define ARROW_W 16
#define ARROW_H 25
#define RETICLE_SIZE 33

struct CursorShape {
    unsigned char* data;
    int width;
    int height;
    int hotX;
    int hotY;
};

static unsigned char arrow_move[ARROW_W * ARROW_H];
static unsigned char arrow_loot[ARROW_W * ARROW_H];
static unsigned char reticle[RETICLE_SIZE * RETICLE_SIZE];
static CursorShape shapes[CURSOR_COUNT];
static bool shapes_built = false;

// What is applied right now (CURSOR_NONE = gmouse owns the shape).
static int applied = CURSOR_NONE;
static unsigned int next_hover_check = 0;
static int hover_variant = CURSOR_MOVE;
static bool reloading = false;

// Palette index for a 5-bit RGB colour. Index 0 is the shape's transparent
// colour and 229..254 cycle (water, fire, screens), so nudge away from both.
static unsigned char cursor_color(int r, int g, int b)
{
    for (int attempt = 0; attempt < 6; attempt++) {
        unsigned char index = colorTable[(r << 10) | (g << 5) | b];
        if (index != 0 && (index < 229 || index > 254)) {
            return index;
        }
        r = r < 31 ? r + 1 : r - 1;
        g = g < 31 ? g + 1 : g - 1;
        b = b < 31 ? b + 1 : b - 1;
    }
    return colorTable[(r << 10) | (g << 5) | b];
}

static bool point_in_polygon(float x, float y, const float (*points)[2], int count)
{
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        float xi = points[i][0];
        float yi = points[i][1];
        float xj = points[j][0];
        float yj = points[j][1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = true ^ inside;
        }
    }
    return inside;
}

// 1 = fill, then a 1 px dark outline around every filled pixel (8-way), so
// the pointer reads on any floor.
static void outline_mask(unsigned char* buf, int width, int height, unsigned char fill, unsigned char edge, unsigned char dark)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char* pixel = buf + y * width + x;
            if (*pixel != 1) {
                continue;
            }
            // Pixels on the right/bottom edge of the shape get the shade.
            bool rightEdge = x + 1 >= width || buf[y * width + x + 1] == 0;
            bool bottomEdge = y + 1 >= height || buf[(y + 1) * width + x] == 0;
            *pixel = (rightEdge || bottomEdge) ? 2 : 1;
        }
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char* pixel = buf + y * width + x;
            if (*pixel != 0) {
                continue;
            }
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < width && ny < height) {
                        unsigned char v = buf[ny * width + nx];
                        if (v == 1 || v == 2) {
                            near = true;
                            break;
                        }
                    }
                }
            }
            if (near) {
                *pixel = 3;
            }
        }
    }

    for (int i = 0; i < width * height; i++) {
        switch (buf[i]) {
        case 1:
            buf[i] = fill;
            break;
        case 2:
            buf[i] = edge;
            break;
        case 3:
            buf[i] = dark;
            break;
        }
    }
}

static void build_arrow(unsigned char* buf, unsigned char fill, unsigned char edge, unsigned char dark)
{
    // Classic pointer, tip at (1,1) (inside the 1 px outline margin).
    static const float points[][2] = {
        { 1.0f, 1.0f },
        { 14.5f, 14.5f },
        { 9.0f, 14.5f },
        { 12.5f, 21.5f },
        { 9.5f, 23.0f },
        { 6.0f, 16.0f },
        { 1.0f, 20.5f },
    };

    memset(buf, 0, ARROW_W * ARROW_H);
    for (int y = 0; y < ARROW_H; y++) {
        for (int x = 0; x < ARROW_W; x++) {
            if (point_in_polygon(x + 0.5f, y + 0.5f, points, sizeof(points) / sizeof(points[0]))) {
                buf[y * ARROW_W + x] = 1;
            }
        }
    }
    // The tip pixel itself, so the hot spot is never transparent.
    buf[1 * ARROW_W + 1] = 1;
    outline_mask(buf, ARROW_W, ARROW_H, fill, edge, dark);
}

static void build_reticle(unsigned char* buf, unsigned char fill, unsigned char edge, unsigned char dark)
{
    memset(buf, 0, RETICLE_SIZE * RETICLE_SIZE);
    int center = RETICLE_SIZE / 2;
    for (int y = 0; y < RETICLE_SIZE; y++) {
        for (int x = 0; x < RETICLE_SIZE; x++) {
            int dx = x - center;
            int dy = y - center;
            float distance = sqrtf((float)(dx * dx + dy * dy));
            bool ring = distance >= 8.5f && distance <= 10.7f;
            // Four ticks crossing the ring, with a gap in the middle.
            bool tick = (dx == 0 && abs(dy) >= 5 && abs(dy) <= 14)
                || (dy == 0 && abs(dx) >= 5 && abs(dx) <= 14);
            bool dot = dx == 0 && dy == 0;
            if (ring || tick || dot) {
                buf[y * RETICLE_SIZE + x] = 1;
            }
        }
    }
    outline_mask(buf, RETICLE_SIZE, RETICLE_SIZE, fill, edge, dark);
}

static void build_shapes()
{
    if (shapes_built) {
        return;
    }
    shapes_built = true;

    unsigned char dark = cursor_color(1, 1, 2);

    build_arrow(arrow_move, cursor_color(31, 31, 29), cursor_color(20, 20, 18), dark);
    build_arrow(arrow_loot, cursor_color(31, 27, 6), cursor_color(24, 17, 2), dark);
    build_reticle(reticle, cursor_color(31, 21, 3), cursor_color(31, 29, 14), dark);

    shapes[CURSOR_MOVE] = { arrow_move, ARROW_W, ARROW_H, 1, 1 };
    shapes[CURSOR_LOOT] = { arrow_loot, ARROW_W, ARROW_H, 1, 1 };
    shapes[CURSOR_ATTACK] = { reticle, RETICLE_SIZE, RETICLE_SIZE, RETICLE_SIZE / 2, RETICLE_SIZE / 2 };
}

// True while the floor hex should not be drawn (the pointer replaces it).
bool arpg_cursor_hides_hex(Object* obj)
{
    if (obj != obj_mouse_flat && obj != obj_mouse) {
        return false;
    }
    return arpg_enabled() && arpg_screen_cursor() && gmouse_3d_get_mode() == GAME_MOUSE_MODE_MOVE;
}

// The pointer is ours to draw: move mode, over the map, gmouse enabled and
// showing no cursor of its own. Not tied to the hex being on: gmouse hides it
// whenever the view scrolls, which with the locked camera is all the time.
static bool cursor_wanted()
{
    if (!arpg_enabled() || !arpg_screen_cursor() || !gmouse_is_enabled()) {
        return false;
    }
    if (gmouse_get_cursor() != MOUSE_CURSOR_NONE || gmouse_3d_get_mode() != GAME_MOUSE_MODE_MOVE) {
        return false;
    }
    int mouseX;
    int mouseY;
    mouse_get_position(&mouseX, &mouseY);
    return win_get_top_win(mouseX, mouseY) == display_win;
}

// Same order as the left-click handler: loot, then an attackable critter.
static int hover_variant_now()
{
    Object* item = object_under_mouse(OBJ_TYPE_ITEM, false, map_elevation);
    if (item != NULL) {
        return CURSOR_LOOT;
    }

    Object* target = arpg_attack_in_place() ? arpg_target_near_cursor() : object_under_mouse(OBJ_TYPE_CRITTER, false, map_elevation);
    if (target != NULL && arpg_is_attackable(target)) {
        return CURSOR_ATTACK;
    }

    return CURSOR_MOVE;
}

static void apply(int variant)
{
    if (variant == applied) {
        return;
    }

    if (variant == CURSOR_NONE) {
        // Hand the shape back to gmouse (e.g. F switched to the use arrow:
        // gmouse stays on MOUSE_CURSOR_NONE and would not redraw it).
        applied = CURSOR_NONE;
        reloading = true;
        gmouse_reload_cursor();
        reloading = false;
        return;
    }

    build_shapes();
    const CursorShape* shape = &shapes[variant];
    if (mouse_set_shape(shape->data, shape->width, shape->height, shape->width, shape->hotX, shape->hotY, 0) == 0) {
        applied = variant;
        arpg_log("ARPG cursor: %s\n", variant == CURSOR_ATTACK ? "attack" : variant == CURSOR_LOOT ? "loot" : "move");
    }
}

// Called by gmouse_set_cursor after it changed the shape.
void arpg_cursor_on_set(int cursor)
{
    applied = CURSOR_NONE;
    if (reloading) {
        return;
    }

    // Entering move mode over the map: put the pointer up at once instead of
    // showing nothing until the next tick.
    if (cursor == MOUSE_CURSOR_NONE && cursor_wanted()) {
        apply(hover_variant);
    }
}

void arpg_cursor_tick(unsigned int now)
{
    if (!cursor_wanted()) {
        if (applied != CURSOR_NONE) {
            apply(CURSOR_NONE);
        }
        return;
    }

    if (applied == CURSOR_NONE || now >= next_hover_check) {
        next_hover_check = now + 50;
        hover_variant = hover_variant_now();
    }
    apply(hover_variant);
}

// Autotest oracle: the pointer variant currently applied (0 none, 1 move,
// 2 loot, 3 attack).
int arpg_cursor_variant()
{
    return applied;
}

} // namespace fallout
