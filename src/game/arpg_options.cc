// ARPG options page: every switch the ARPG layer added, opened from the
// Options (ESC) menu. Each row writes its [arpg] key to game_config (saved to
// fallout.cfg on close) and, where the setting lives in a variable, applies it
// at once.

#include <stdio.h>
#include <string.h>

#include "game/arpg.h"
#include "game/art.h"
#include "game/game.h"
#include "game/gconfig.h"
#include "game/gsound.h"
#include "game/tile.h"
#include "plib/color/color.h"
#include "plib/gnw/button.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/input.h"
#include "plib/gnw/kb.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/svga.h"

#include <SDL.h>
#include "plib/gnw/text.h"

namespace fallout {

#define ARPG_CONFIG_SECTION "arpg"

enum {
    OPT_HEADER,
    // Live variable in arpg.cc (arpg_option_flag).
    OPT_FLAG,
    // Read from game_config by the HUD every tick.
    OPT_CONFIG,
    // Read once at startup.
    OPT_RESTART,
    OPT_CONTROLS,
    OPT_LOOT_PCT,
    OPT_MINIMAP,
    OPT_ZOOM_SMOOTH,
};

struct ArpgOption {
    int kind;
    const char* key;
    const char* label;
    const char* hint;
    bool defaultOn;
};

#define COLUMN_COUNT 2

static const ArpgOption column_left[] = {
    { OPT_HEADER, NULL, "COMBAT & CONTROLS", NULL, false },
    { OPT_RESTART, "enabled", "Real-time ARPG mode", "Off = vanilla turn-based Fallout. Takes effect after restarting the game.", true },
    { OPT_CONTROLS, "controls", "Control scheme", "Path of Exile click-to-move, WASD movement, or the classic first version. Ctrl+W cycles in game.", false },
    { OPT_FLAG, "always_run", "Always run", "The player runs instead of walking.", true },
    { OPT_FLAG, "hold_to_move", "Hold button to move/attack", "Holding the left mouse button keeps moving or attacking.", true },
    { OPT_FLAG, "rmb_toggles_mode", "Right click switches cursor", "Vanilla right click: toggles the cursor mode instead of firing the special (PoE/WASD; F always toggles).", false },
    { OPT_FLAG, "move_shot", "Shoot on the move", "WASD: shots don't stop the run, the player glides while firing.", true },
    { OPT_FLAG, "respawn", "Respawn on death", "Diablo-style death: knocked down, then back at the map entrance with full HP, minus 10% caps.", true },
    { OPT_HEADER, NULL, "INTERFACE", NULL, false },
    { OPT_CONFIG, "globes", "Life/energy globes, XP bar", "The Diablo life and energy globes and the experience strip.", true },
    { OPT_CONFIG, "hotkey_bar", "Hotkey bar", "Keycap slots for flasks, aimed shots, dodge, special and reload.", true },
    { OPT_MINIMAP, "minimap", "Minimap (V)", "Top-right map of walls, items, monsters and shrines.", true },
    { OPT_FLAG, "damage_numbers", "Damage numbers", "Floating damage, miss and heal numbers.", true },
    { OPT_FLAG, "enemy_outline", "Enemy outlines (X)", "Red outline around every living enemy.", true },
    { OPT_ZOOM_SMOOTH, "zoom_smooth", "Smooth zoom", "Filtered scaling when the map is zoomed with the mouse wheel. Off = sharp pixels.", true },
};

static const ArpgOption column_right[] = {
    { OPT_HEADER, NULL, "LOOT", NULL, false },
    { OPT_FLAG, "loot_drops", "Monster loot drops", "Monsters drop weapons, ammo, drugs, caps, magic and unique items.", true },
    { OPT_FLAG, "inventory_drops", "Corpses drop inventory", "Dead NPCs and monsters spill everything they carried (weapon, armor, caps, ammo) on the ground.", true },
    { OPT_LOOT_PCT, "loot_pct", "Loot amount", "How much monsters drop (100% = normal).", false },
    { OPT_FLAG, "labels_always", "Always show loot labels (Z)", "Item names stay on screen without holding Alt.", false },
    { OPT_CONFIG, "loot_highlight", "Loot outlines (Alt/Z)", "Ground items are outlined in their quality colour while labels show.", true },
    { OPT_FLAG, "auto_caps", "Auto-pick caps", "Walking past caps picks them up.", true },
    { OPT_FLAG, "auto_consumables", "Auto-pick stimpaks & drugs", "Walking past flask drugs (stimpaks, RadAway, Rad-X, Buffout, Mentats, Psycho, Nuka-Cola) picks them up.", true },
    { OPT_FLAG, "auto_ammo", "Auto-pick ammo", "Walking past any ammo picks it up.", true },
    { OPT_HEADER, NULL, "WORLD", NULL, false },
    { OPT_FLAG, "packs", "Monster packs & champions", "Wild monsters come in packs with champions and named uniques. Applies to areas entered from now on.", true },
    { OPT_FLAG, "shrines", "Shrines", "Shrines with timed boosts on monster maps. Applies to areas entered from now on.", true },
    { OPT_FLAG, "raider_packs", "Raider reinforcements", "Hostile people in random encounters call in more raiders.", true },
};

static const ArpgOption* const columns[COLUMN_COUNT] = { column_left, column_right };
static const int column_rows[COLUMN_COUNT] = {
    (int)(sizeof(column_left) / sizeof(column_left[0])),
    (int)(sizeof(column_right) / sizeof(column_right[0])),
};

static const int loot_steps[] = { 50, 100, 150, 200, 300, 500 };

#define WINDOW_WIDTH 600
#define COLUMN_WIDTH 280
#define COLUMN_X 14
#define ROWS_Y 40
#define ROW_HEIGHT 15
#define HINT_HEIGHT 28

#define EVENT_TOGGLE 600
#define EVENT_ENTER 700
#define EVENT_EXIT 800
#define EVENT_DONE 900

static int opt_rgb(int r, int g, int b)
{
    return colorTable[(r << 10) | (g << 5) | b];
}

static bool opt_config_bool(const ArpgOption* opt)
{
    bool value = opt->defaultOn;
    configGetBool(&game_config, ARPG_CONFIG_SECTION, opt->key, &value);
    return value;
}

// Fills `text` with the row's value; returns 1 on, 0 off, -1 not a switch.
static int opt_value(const ArpgOption* opt, char* text, int size)
{
    int on = -1;
    switch (opt->kind) {
    case OPT_FLAG: {
        bool* var = arpg_option_flag(opt->key);
        on = var != NULL ? *var : opt_config_bool(opt);
        break;
    }
    case OPT_CONFIG:
    case OPT_RESTART:
        on = opt_config_bool(opt);
        break;
    case OPT_MINIMAP:
        on = arpg_hud_minimap_on();
        break;
    case OPT_ZOOM_SMOOTH:
        on = arpg_zoom_smooth();
        break;
    case OPT_CONTROLS: {
        static const char* names[] = { "Classic", "Path of Exile", "WASD" };
        snprintf(text, size, "%s", names[arpg_controls()]);
        return -1;
    }
    case OPT_LOOT_PCT: {
        int* pct = arpg_option_int(opt->key);
        snprintf(text, size, "%d%%", pct != NULL ? *pct : 100);
        return -1;
    }
    default:
        text[0] = '\0';
        return -1;
    }
    snprintf(text, size, "%s", on ? "ON" : "OFF");
    return on;
}

static void opt_toggle(const ArpgOption* opt)
{
    char text[32];
    bool on = opt_value(opt, text, sizeof(text)) != 1;

    switch (opt->kind) {
    case OPT_FLAG: {
        bool* var = arpg_option_flag(opt->key);
        if (var != NULL) {
            *var = on;
        }
        configSetBool(&game_config, ARPG_CONFIG_SECTION, opt->key, on);
        break;
    }
    case OPT_CONFIG:
    case OPT_RESTART:
        configSetBool(&game_config, ARPG_CONFIG_SECTION, opt->key, on);
        break;
    case OPT_MINIMAP:
        arpg_hud_set_minimap(on);
        configSetBool(&game_config, ARPG_CONFIG_SECTION, opt->key, on);
        break;
    case OPT_ZOOM_SMOOTH:
        arpg_zoom_set_smooth(on);
        configSetBool(&game_config, ARPG_CONFIG_SECTION, opt->key, on);
        break;
    case OPT_CONTROLS:
        arpg_cycle_controls();
        return;
    case OPT_LOOT_PCT: {
        int* pct = arpg_option_int(opt->key);
        if (pct == NULL) {
            return;
        }
        // Next step above the current value, wrapping to the smallest.
        int next = loot_steps[0];
        for (int step : loot_steps) {
            if (step > *pct) {
                next = step;
                break;
            }
        }
        *pct = next;
        config_set_value(&game_config, ARPG_CONFIG_SECTION, opt->key, next);
        break;
    }
    default:
        return;
    }
    gsound_play_sfx_file("ib1p1xx1");
}

static void opt_draw(unsigned char* buf, int height, int hoverColumn, int hoverRow, Size doneSize)
{
    int oldFont = text_curr();

    buf_fill(buf, WINDOW_WIDTH, height, WINDOW_WIDTH, opt_rgb(2, 2, 1));
    draw_box(buf, WINDOW_WIDTH, 0, 0, WINDOW_WIDTH - 1, height - 1, opt_rgb(14, 12, 3));
    draw_box(buf, WINDOW_WIDTH, 2, 2, WINDOW_WIDTH - 3, height - 3, opt_rgb(7, 6, 2));

    text_font(103);
    const char* title = "ARPG OPTIONS";
    text_to_buf(buf + 12 * WINDOW_WIDTH + (WINDOW_WIDTH - text_width(title)) / 2, title, WINDOW_WIDTH, WINDOW_WIDTH, opt_rgb(18, 17, 3));

    text_font(101);
    int textOffset = (ROW_HEIGHT - text_height()) / 2;
    char value[32];
    for (int column = 0; column < COLUMN_COUNT; column++) {
        int x = COLUMN_X + column * (COLUMN_WIDTH + 14);
        for (int row = 0; row < column_rows[column]; row++) {
            const ArpgOption* opt = &(columns[column][row]);
            int y = ROWS_Y + row * ROW_HEIGHT;
            unsigned char* rowBuf = buf + y * WINDOW_WIDTH + x;
            if (opt->kind == OPT_HEADER) {
                text_to_buf(rowBuf + (textOffset + 1) * WINDOW_WIDTH, opt->label, COLUMN_WIDTH, WINDOW_WIDTH, opt_rgb(14, 12, 3));
                buf_fill(rowBuf + (ROW_HEIGHT - 2) * WINDOW_WIDTH, COLUMN_WIDTH, 1, WINDOW_WIDTH, opt_rgb(7, 6, 2));
                continue;
            }

            if (column == hoverColumn && row == hoverRow) {
                buf_fill(rowBuf, COLUMN_WIDTH, ROW_HEIGHT - 1, WINDOW_WIDTH, opt_rgb(6, 6, 3));
            }

            int on = opt_value(opt, value, sizeof(value));
            int labelColor = on == 0 ? opt_rgb(12, 11, 3) : opt_rgb(18, 17, 3);
            text_to_buf(rowBuf + textOffset * WINDOW_WIDTH + 6, opt->label, COLUMN_WIDTH - 6, WINDOW_WIDTH, labelColor);

            int valueColor = on == 1 ? opt_rgb(0, 31, 0) : (on == 0 ? opt_rgb(22, 5, 4) : opt_rgb(31, 31, 31));
            int valueWidth = text_width(value);
            text_to_buf(rowBuf + textOffset * WINDOW_WIDTH + COLUMN_WIDTH - 6 - valueWidth, value, valueWidth + 1, WINDOW_WIDTH, valueColor);
        }
    }

    int rows = column_rows[0] > column_rows[1] ? column_rows[0] : column_rows[1];
    int hintY = ROWS_Y + rows * ROW_HEIGHT + 6;
    buf_fill(buf + (hintY - 3) * WINDOW_WIDTH + 12, WINDOW_WIDTH - 24, 1, WINDOW_WIDTH, opt_rgb(7, 6, 2));

    const char* hint = "Click a setting to change it. Changes apply at once and are saved to fallout.cfg.";
    if (hoverColumn != -1 && columns[hoverColumn][hoverRow].hint != NULL) {
        hint = columns[hoverColumn][hoverRow].hint;
    }
    // Word-wrap the hint into two lines.
    char line[160];
    const char* rest = hint;
    for (int lineIndex = 0; lineIndex < 2 && *rest != '\0'; lineIndex++) {
        int length = (int)strlen(rest);
        int fit = length;
        while (fit > 0) {
            snprintf(line, sizeof(line), "%.*s", fit, rest);
            if (text_width(line) <= WINDOW_WIDTH - 32) {
                break;
            }
            int cut = fit - 1;
            while (cut > 0 && rest[cut] != ' ') {
                cut--;
            }
            fit = cut;
        }
        if (fit <= 0) {
            break;
        }
        snprintf(line, sizeof(line), "%.*s", fit, rest);
        text_to_buf(buf + (hintY + lineIndex * (text_height() + 2)) * WINDOW_WIDTH + 16, line, WINDOW_WIDTH - 32, WINDOW_WIDTH, opt_rgb(0, 22, 0));
        rest += fit;
        while (*rest == ' ') {
            rest++;
        }
    }

    text_font(103);
    int doneX = WINDOW_WIDTH - 110 + doneSize.width + 6;
    int doneY = height - 14 - doneSize.height;
    text_to_buf(buf + (doneY + (doneSize.height - text_height()) / 2 + 1) * WINDOW_WIDTH + doneX, "DONE", 60, WINDOW_WIDTH, opt_rgb(18, 17, 3));

    text_font(oldFont);
}

void arpg_options_dialog()
{
    CacheEntry* upKey;
    CacheEntry* downKey;
    Size doneSize;
    Size downSize;
    // lilredup.frm / lilreddn.frm, the little red button of the pause window.
    unsigned char* up = art_lock(art_id(OBJ_TYPE_INTERFACE, 8, 0, 0, 0), &upKey, &(doneSize.width), &(doneSize.height));
    unsigned char* down = art_lock(art_id(OBJ_TYPE_INTERFACE, 9, 0, 0, 0), &downKey, &(downSize.width), &(downSize.height));
    if (up == NULL || down == NULL) {
        if (up != NULL) {
            art_ptr_unlock(upKey);
        }
        if (down != NULL) {
            art_ptr_unlock(downKey);
        }
        return;
    }

    int rows = column_rows[0] > column_rows[1] ? column_rows[0] : column_rows[1];
    int height = ROWS_Y + rows * ROW_HEIGHT + 6 + HINT_HEIGHT + doneSize.height + 18;
    int x = (screenGetWidth() - WINDOW_WIDTH) / 2;
    int y = (screenGetHeight() - height) / 2;
    if (y < 0) {
        y = 0;
    }
    int win = win_add(x, y, WINDOW_WIDTH, height, 256, WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
    if (win == -1) {
        art_ptr_unlock(upKey);
        art_ptr_unlock(downKey);
        return;
    }
    unsigned char* buf = win_get_buf(win);

    int rowButtons[COLUMN_COUNT][50];
    for (int column = 0; column < COLUMN_COUNT; column++) {
        int columnX = COLUMN_X + column * (COLUMN_WIDTH + 14);
        for (int row = 0; row < column_rows[column]; row++) {
            if (columns[column][row].kind == OPT_HEADER) {
                continue;
            }
            int code = column * 50 + row;
            rowButtons[column][row] = win_register_button(win, columnX, ROWS_Y + row * ROW_HEIGHT, COLUMN_WIDTH, ROW_HEIGHT - 1,
                EVENT_ENTER + code, EVENT_EXIT + code, -1, EVENT_TOGGLE + code, NULL, NULL, NULL, 0);
        }
    }

    int doneBtn = win_register_button(win, WINDOW_WIDTH - 110, height - 14 - doneSize.height, doneSize.width, doneSize.height,
        -1, -1, -1, EVENT_DONE, up, down, NULL, BUTTON_FLAG_TRANSPARENT);
    if (doneBtn != -1) {
        win_register_button_sound_func(doneBtn, gsound_red_butt_press, gsound_red_butt_release);
    }

    int hoverColumn = -1;
    int hoverRow = -1;
    opt_draw(buf, height, hoverColumn, hoverRow, doneSize);
    win_draw(win);

    // Autotest: hover + press the buttons of rows: enemy outlines,
    // minimap, loot amount, auto ammo, control scheme x3 (back to the start),
    // then a hover and screenshots.
    static const int scriptClicks[][2] = { { 0, 13 }, { 0, 11 }, { 1, 2 }, { 1, 7 }, { 0, 2 }, { 0, 2 }, { 0, 2 } };
    const int scriptClickCount = (int)(sizeof(scriptClicks) / sizeof(scriptClicks[0]));
    bool script = arpg_autotest_options_script();
    int scriptStep = 0;
    unsigned int scriptAt = get_time() + 600;

    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        if (script && get_time() >= scriptAt) {
            scriptAt = get_time() + 250;
            int click = scriptStep / 3;
            int phase = scriptStep % 3;
            char value[32];
            if (scriptStep == 0) {
                dump_screen();
                arpg_log("ARPG autotest: options page open\n");
            }
            if (click < scriptClickCount) {
                int column = scriptClicks[click][0];
                int row = scriptClicks[click][1];
                const ArpgOption* opt = &(columns[column][row]);
                if (phase == 0) {
                    mouse_set_position(x + COLUMN_X + column * (COLUMN_WIDTH + 14) + COLUMN_WIDTH / 2, y + ROWS_Y + row * ROW_HEIGHT + ROW_HEIGHT / 2);
                    opt_value(opt, value, sizeof(value));
                    arpg_log("ARPG autotest: options click %s (was %s)\n", opt->key, value);
                } else if (phase == 1) {
                    // SDL's own button state can't be faked, so press the
                    // row's button through GNW.
                    win_button_press_and_release(rowButtons[column][row]);
                } else {
                    opt_value(opt, value, sizeof(value));
                    arpg_log("ARPG autotest: options %s now %s\n", opt->key, value);
                }
            } else if (click == scriptClickCount && phase == 0) {
                // Hover a row with a two-line hint.
                mouse_set_position(x + COLUMN_X + COLUMN_WIDTH / 2, y + ROWS_Y + 5 * ROW_HEIGHT + ROW_HEIGHT / 2);
            } else if (click == scriptClickCount && phase == 2) {
                dump_screen();
                for (int column = 0; column < COLUMN_COUNT; column++) {
                    for (int row = 0; row < column_rows[column]; row++) {
                        const ArpgOption* opt = &(columns[column][row]);
                        if (opt->kind != OPT_HEADER) {
                            opt_value(opt, value, sizeof(value));
                            arpg_log("ARPG autotest: options %s = %s\n", opt->key, value);
                        }
                    }
                }
                arpg_log("ARPG autotest: options hover row %d,%d\n", hoverColumn, hoverRow);
            } else if (click > scriptClickCount) {
                // Close this page with a real Return, then the options menu
                // with Escape.
                static const SDL_Scancode keys[] = { SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE };
                for (SDL_Scancode scancode : keys) {
                    SDL_Event event = {};
                    event.type = SDL_KEYDOWN;
                    event.key.state = SDL_PRESSED;
                    event.key.keysym.scancode = scancode;
                    event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
                    SDL_PushEvent(&event);
                    event.type = SDL_KEYUP;
                    event.key.state = SDL_RELEASED;
                    SDL_PushEvent(&event);
                }
                script = false;
            }
            scriptStep++;
        }

        int key = get_input();
        bool redraw = false;
        if (key == KEY_ESCAPE || key == KEY_RETURN || key == KEY_LOWERCASE_D || key == KEY_UPPERCASE_D || key == EVENT_DONE || game_user_wants_to_quit != 0) {
            done = true;
        } else if (key >= EVENT_TOGGLE && key < EVENT_TOGGLE + COLUMN_COUNT * 50) {
            int code = key - EVENT_TOGGLE;
            opt_toggle(&(columns[code / 50][code % 50]));
            redraw = true;
        } else if (key >= EVENT_ENTER && key < EVENT_ENTER + COLUMN_COUNT * 50) {
            int code = key - EVENT_ENTER;
            hoverColumn = code / 50;
            hoverRow = code % 50;
            redraw = true;
        } else if (key >= EVENT_EXIT && key < EVENT_EXIT + COLUMN_COUNT * 50) {
            int code = key - EVENT_EXIT;
            if (hoverColumn == code / 50 && hoverRow == code % 50) {
                hoverColumn = -1;
                hoverRow = -1;
                redraw = true;
            }
        } else if (key == KEY_F12) {
            dump_screen();
        }

        if (redraw) {
            opt_draw(buf, height, hoverColumn, hoverRow, doneSize);
            win_draw(win);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    win_delete(win);
    art_ptr_unlock(upKey);
    art_ptr_unlock(downKey);

    gconfig_save();
    tile_refresh_display();
}

} // namespace fallout
