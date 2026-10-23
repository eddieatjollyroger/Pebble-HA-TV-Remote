#include <pebble.h>

/* ---------- Sizing: bigger fonts and spacing on Pebble Time 2 (Emery) ---------- */

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  #define FONT_S   FONT_KEY_GOTHIC_18
  #define FONT_SB  FONT_KEY_GOTHIC_18_BOLD
  #define FONT_M   FONT_KEY_GOTHIC_24
  #define FONT_MB  FONT_KEY_GOTHIC_24_BOLD
  #define LH_S 20            // line height, small font
  #define LH_M 28            // line height, medium font
  #define PAD 6
  #define HDR_H 22
  #define PROMPT_Y 26
  #define TITLE_GAP 4
  #define BAR_H 10
  #define SEG_STEP 5
  #define SEG_W 4
  #define TAG_W 92
  #define CLOCK_W 54
  #define MENU_HEADER_H 24
  #define ROW_H_SHORT 36
  #define ROW_H_TALL 52
#else
  #define FONT_S   FONT_KEY_GOTHIC_14
  #define FONT_SB  FONT_KEY_GOTHIC_14_BOLD
  #define FONT_M   FONT_KEY_GOTHIC_18
  #define FONT_MB  FONT_KEY_GOTHIC_18_BOLD
  #define LH_S 16
  #define LH_M 22
  #define PAD 4
  #define HDR_H 18
  #define PROMPT_Y 22
  #define TITLE_GAP 0
  #define BAR_H 8
  #define SEG_STEP 4
  #define SEG_W 3
  #define TAG_W 66
  #define CLOCK_W 40
  #define MENU_HEADER_H 19
  #define ROW_H_SHORT 27
  #define ROW_H_TALL 40
#endif

// On round watches the corners of a rectangle are cut off by the bezel. Draw inside the
// largest square that fits in the circle instead. Rectangular watches use the full screen.
static GRect safe_bounds(GRect full) {
#if defined(PBL_ROUND)
  int side = (full.size.w * 707) / 1000;
  return GRect((full.size.w - side) / 2, (full.size.h - side) / 2, side, side);
#else
  return full;
#endif
}

static Window *s_main_window;
static Layer *s_ui_layer;

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

static Window *s_tv_window;
static MenuLayer *s_tv_menu_layer;

#define MAX_TVS 10
#define MAX_NAME_LEN 64

// Persistent storage key for the selected TV's display name
#define PERSIST_KEY_TV_NAME 200
#define PERSIST_KEY_THEME 201

// Double-tap Back within this window (ms) to exit
#define DOUBLE_TAP_MS 250

// Terminal palette
// Dark: green on black. Light: black on white. Same layout, switched from the settings page.
static GColor s_col_bg, s_col_fg, s_col_dim, s_col_warn;
static bool s_light_theme = false;
#define C_BG     s_col_bg
#define C_BRIGHT s_col_fg
#define C_DIM    s_col_dim
#define C_WARN   s_col_warn

static void set_theme_colors(bool light) {
  s_light_theme = light;
  if (light) {
    s_col_bg = GColorWhite;
    s_col_fg = GColorBlack;
    s_col_dim = GColorDarkGray;
    s_col_warn = GColorOrange;
  } else {
    s_col_bg = GColorBlack;
    s_col_fg = GColorGreen;
    s_col_dim = GColorIslamicGreen;
    s_col_warn = GColorYellow;
  }
}

static char s_tv_names[MAX_TVS][MAX_NAME_LEN];
static char s_tv_entities[MAX_TVS][MAX_NAME_LEN];
static int s_tv_count = 0;
static int s_active_tv_index = -1;

static char s_tv_title[MAX_NAME_LEN] = "SELECT A TV";

// Now-playing info, pushed from the phone
static char s_np_state[12] = "--";
static char s_np_title[64] = "";
static char s_np_sub[48] = "";
static char s_np_app[32] = "";
static int32_t s_np_pos = 0;
static int32_t s_np_dur = 0;

static bool s_cursor_on = true;
static AppTimer *s_tick_timer = NULL;

// Power saving: tick every second only while the remote is in use
#define ACTIVE_SECS 30        // "in use" = a button was pressed in the last 30 s
static time_t s_last_activity = 0;
static time_t s_last_tick = 0;
static bool s_tick_idle = false;

static int s_remote_mode = 0;
static AppTimer *s_back_timer = NULL;

enum {
  KEY_COMMAND = 0,
  KEY_FETCH_TVS = 1,
  KEY_TV_COUNT = 2,
  KEY_TV_NAME = 3,
  KEY_TV_ENTITY = 4,
  KEY_SET_ACTIVE_TV = 5,
  KEY_NP_STATE = 6,
  KEY_NP_TITLE = 7,
  KEY_NP_SUB = 8,
  KEY_NP_APP = 9,
  KEY_NP_POS = 10,
  KEY_NP_DUR = 11,
  KEY_THEME = 12
};

// Main menu rows: 0 = select tv, 1 = power on, 2 = power off, 3 = home, 4 = home (hold), 5 = reload tv
#define MENU_ROWS 6

static const char *s_mode_labels[] = {
  "> left/right", "> up/down", "> volume"
};

static void ui_redraw(void) {
  if (s_ui_layer) layer_mark_dirty(s_ui_layer);
}

static void set_tv_title(const char *name) {
  snprintf(s_tv_title, MAX_NAME_LEN, "%s", name);
  ui_redraw();
}

static void send_command(const char *command_str) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox busy");
    return;
  }
  dict_write_cstring(iter, KEY_COMMAND, command_str);
  app_message_outbox_send();
}

// Ask the phone for the TV list (it answers from its cache, so this is quick)
static void request_tv_list(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) return;
  dict_write_int8(iter, KEY_FETCH_TVS, 1);
  app_message_outbox_send();
}

static void set_active_tv_on_phone(const char *entity_id) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox busy");
    return;
  }
  dict_write_cstring(iter, KEY_SET_ACTIVE_TV, entity_id);
  app_message_outbox_send();
}

/* ---------- Terminal-style main screen ---------- */

static void fmt_time(char *buf, size_t n, int32_t secs) {
  if (secs < 0) secs = 0;
  int32_t h = secs / 3600;
  int32_t m = (secs % 3600) / 60;
  int32_t s = secs % 60;
  if (h > 0) snprintf(buf, n, "%d:%02d:%02d", (int)h, (int)m, (int)s);
  else snprintf(buf, n, "%d:%02d", (int)m, (int)s);
}

static void fmt_clock(char *buf, size_t n) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  if (clock_is_24h_style()) {
    strftime(buf, n, "%H:%M", t);
  } else {
    strftime(buf, n, "%I:%M", t);
    if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));   // 07:05 -> 7:05
  }
}

static void draw_text(GContext *ctx, const char *text, GFont font, GRect rect,
                      GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis, align, NULL);
}

/* ---------- Marquee: long text slides like a car stereo ---------- */

#define MARQUEE_MS 80          // frame time while something is scrolling
#define MARQUEE_STEP 2         // px per frame (~25 px/s)
#define MARQUEE_PAUSE 12       // frames to hold at each end (~1 s)

static AppTimer *s_scroll_timer = NULL;
static int s_scroll_tick = 0;           // the selected menu row
static bool s_scroll_active = false;    // set by drawing when it's actually scrolling
static char s_marquee_key[160] = "";
static int s_header_tick = 0;           // the main screen's TV name
static bool s_header_active = false;
static char s_header_key[MAX_NAME_LEN] = "";

// The timer only runs while something might scroll: a menu is open, or the remote was used recently
static bool scroll_wanted(void) {
  if (s_menu_window || s_tv_window) return true;
  return (time(NULL) - s_last_activity) < ACTIVE_SECS;
}

static void scroll_timer_cb(void *data) {
  s_scroll_timer = NULL;
  if (!scroll_wanted()) return;                 // stops; restarted by the next button press
  bool any = s_scroll_active || s_header_active;
  if (s_scroll_active) {
    s_scroll_tick++;
    s_scroll_active = false;                    // drawing sets it again if still scrolling
    if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
    if (s_tv_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_tv_menu_layer));
  }
  if (s_header_active) {
    s_header_tick++;
    s_header_active = false;
    ui_redraw();
  }
  s_scroll_timer = app_timer_register(any ? MARQUEE_MS : 300, scroll_timer_cb, NULL);
}

static void scroll_timer_ensure(void) {
  if (!s_scroll_timer) s_scroll_timer = app_timer_register(MARQUEE_MS, scroll_timer_cb, NULL);
}

// A menu just opened: start its scroll from the beginning
static void scroll_timer_start(void) {
  s_marquee_key[0] = '\0';
  s_scroll_tick = 0;
  scroll_timer_ensure();
}

// Draws text in `area`. If it's too wide and `scroll` is true, it slides left to show
// the rest, holds, then starts over. Otherwise it's drawn normally with an ellipsis.
static void draw_marquee(GContext *ctx, const char *text, GFont font, GRect area, int cell_w,
                         GColor color, GColor bg, bool scroll, int tick, bool *active) {
  GSize sz = graphics_text_layout_get_content_size(text, font, GRect(0, 0, 1000, area.size.h),
                                                   GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int overflow = sz.w - area.size.w;
  if (!scroll || overflow <= 0) {
    draw_text(ctx, text, font, area, GTextAlignmentLeft, color);
    return;
  }

  *active = true;
  int travel = (overflow + MARQUEE_STEP - 1) / MARQUEE_STEP;
  int cycle = MARQUEE_PAUSE + travel + MARQUEE_PAUSE;
  int t = tick % cycle;
  int off = 0;
  if (t >= MARQUEE_PAUSE + travel) off = overflow;
  else if (t >= MARQUEE_PAUSE) off = (t - MARQUEE_PAUSE) * MARQUEE_STEP;
  if (off > overflow) off = overflow;

  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, GRect(area.origin.x - off, area.origin.y, sz.w + 8, area.size.h),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

  // Hide the parts that slid into the side margins
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, GRect(0, area.origin.y, area.origin.x, area.size.h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(area.origin.x + area.size.w, area.origin.y,
                                cell_w - area.origin.x - area.size.w, area.size.h), 0, GCornerNone);
}

static int text_width(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 1000, 40),
                                               GTextOverflowModeWordWrap, GTextAlignmentLeft).w;
}

static void ui_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w;
  int h = b.size.h;

  GFont fs = fonts_get_system_font(FONT_S);
  GFont fsb = fonts_get_system_font(FONT_SB);
  GFont fm = fonts_get_system_font(FONT_M);
  GFont fmb = fonts_get_system_font(FONT_MB);

  bool playing = strcmp(s_np_state, "PLAY") == 0;
  bool paused = strcmp(s_np_state, "PAUSE") == 0;
  bool off = strcmp(s_np_state, "OFF") == 0 || strcmp(s_np_state, "OFFLINE") == 0;

  // Layout, anchored to the bottom so it adapts to the screen height
  int footer_y = h - LH_S - 2;
  int times_y = footer_y - LH_S - 2;
  int bar_y = times_y - BAR_H - 4;
  int app_y = bar_y - LH_S - 2;
  int sub_y = app_y - LH_S;
  int title_y = PROMPT_Y + LH_S + TITLE_GAP;
  int title_h = sub_y - title_y - 1;
  bool compact = title_h < LH_M;   // very small screens: give the title the room instead
  if (compact) {
    sub_y = app_y = bar_y;
    title_h = bar_y - title_y - 1;
  }

  char buf[80];

  // Header: "# <tv name>"            [STATE]
  char tag[16];
  snprintf(tag, sizeof(tag), "[%s]", s_np_state);
  int tag_w = text_width(tag, fsb) + 2;
  snprintf(buf, sizeof(buf), "# %s", s_tv_title);
  // Slide a long name only while the remote is in use; otherwise show it cut off with "..."
  bool hscroll = (time(NULL) - s_last_activity) < ACTIVE_SECS;
  if (!hscroll || strcmp(s_header_key, s_tv_title) != 0) {
    snprintf(s_header_key, sizeof(s_header_key), "%s", s_tv_title);
    s_header_tick = 0;                          // always start from the beginning
  }
  draw_marquee(ctx, buf, fsb, GRect(PAD, 0, w - 3 * PAD - tag_w, HDR_H), w, C_BRIGHT, C_BG,
               hscroll, s_header_tick, &s_header_active);
  draw_text(ctx, tag, fsb, GRect(w - PAD - tag_w, 0, tag_w, HDR_H), GTextAlignmentRight,
            playing ? C_BRIGHT : (paused ? C_WARN : C_DIM));

  // Dashed divider
  graphics_context_set_stroke_color(ctx, C_DIM);
  for (int x = PAD; x < w - PAD; x += 4) {
    graphics_draw_line(ctx, GPoint(x, HDR_H + 2), GPoint(x + 1, HDR_H + 2));
  }

  // Prompt with blinking cursor
  snprintf(buf, sizeof(buf), "$ now_playing%s", s_cursor_on ? "_" : " ");
  draw_text(ctx, buf, fs, GRect(PAD, PROMPT_Y, w - 2 * PAD, LH_S), GTextAlignmentLeft, C_DIM);

  // Title (up to two lines)
  if (s_np_title[0]) {
    draw_text(ctx, s_np_title, fmb, GRect(PAD, title_y, w - 2 * PAD, title_h), GTextAlignmentLeft, C_BRIGHT);
  } else {
    draw_text(ctx, off ? "[ standby ]" : "-- nothing playing --", fm,
              GRect(PAD, title_y, w - 2 * PAD, title_h), GTextAlignmentLeft, C_DIM);
  }

  // Subtitle (artist / series) and app
  if (!compact && s_np_sub[0]) {
    draw_text(ctx, s_np_sub, fs, GRect(PAD, sub_y, w - 2 * PAD, LH_S), GTextAlignmentLeft, C_BRIGHT);
  }
  if (!compact && s_np_app[0]) {
    snprintf(buf, sizeof(buf), "via %s", s_np_app);
    draw_text(ctx, buf, fs, GRect(PAD, app_y, w - 2 * PAD, LH_S), GTextAlignmentLeft, C_DIM);
  }

  // Progress bar and times, only for players that report a duration (e.g. Apple TV)
  if (s_np_dur > 0) {
    int bx = PAD;
    int bw = w - 2 * PAD;
    graphics_context_set_stroke_color(ctx, C_DIM);
    graphics_draw_rect(ctx, GRect(bx, bar_y, bw, BAR_H));

    int32_t pos = s_np_pos > s_np_dur ? s_np_dur : s_np_pos;
    int inner = bw - 4;
    int filled = (int)(((int64_t)inner * pos) / s_np_dur);
    graphics_context_set_fill_color(ctx, C_BRIGHT);
    for (int x = 0; x < filled; x += SEG_STEP) {
      int seg = (filled - x) < SEG_W ? (filled - x) : SEG_W;
      graphics_fill_rect(ctx, GRect(bx + 2 + x, bar_y + 2, seg, BAR_H - 4), 0, GCornerNone);
    }

    char t1[16], t2[16];
    fmt_time(t1, sizeof(t1), s_np_pos);
    fmt_time(t2, sizeof(t2), s_np_dur);
    draw_text(ctx, t1, fs, GRect(PAD, times_y, w / 2 - PAD, LH_S), GTextAlignmentLeft, C_DIM);
    draw_text(ctx, t2, fs, GRect(w / 2, times_y, w / 2 - PAD, LH_S), GTextAlignmentRight, C_DIM);
  }

  // Footer: remote mode on the left, clock on the right
  draw_text(ctx, s_mode_labels[s_remote_mode], fs,
            GRect(PAD, footer_y, w - 2 * PAD - CLOCK_W, LH_S), GTextAlignmentLeft, C_BRIGHT);
  char clock[12];
  fmt_clock(clock, sizeof(clock));
  draw_text(ctx, clock, fs, GRect(w - PAD - CLOCK_W, footer_y, CLOCK_W, LH_S), GTextAlignmentRight, C_DIM);
}

// Ticks every second while in use; when idle, once a minute on the minute so the
// clock stays right. Progress advances by real elapsed time either way.
static void tick_cb(void *data) {
  time_t now = time(NULL);
  int32_t elapsed = (int32_t)(now - s_last_tick);
  if (elapsed < 0) elapsed = 0;
  s_last_tick = now;

  if (strcmp(s_np_state, "PLAY") == 0 && s_np_dur > 0) {
    s_np_pos += elapsed;
    if (s_np_pos > s_np_dur) s_np_pos = s_np_dur;
  }

  bool active = (now - s_last_activity) < ACTIVE_SECS;
  s_cursor_on = active ? !s_cursor_on : true;   // cursor only blinks while in use
  s_tick_idle = !active;
  ui_redraw();

  uint32_t delay = 1000;
  if (!active) {
    struct tm *t = localtime(&now);
    delay = (uint32_t)(60 - t->tm_sec) * 1000 + 50;   // wake right after the minute changes
  }
  s_tick_timer = app_timer_register(delay, tick_cb, NULL);
}

// Call on any button press: wakes the 1-second tick if it was idling
static void note_activity(void) {
  s_last_activity = time(NULL);
  scroll_timer_ensure();   // a long TV name in the header can slide again
  if (s_tick_idle && s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_idle = false;
    tick_cb(NULL);   // catch up now and resume 1 s ticks
  }
}

/* ---------- Terminal-style menus ---------- */

static void draw_menu_header(GContext *ctx, const Layer *cell_layer, const char *left, const char *right) {
  GRect b = layer_get_bounds(cell_layer);
  int w = b.size.w;
  GFont fsb = fonts_get_system_font(FONT_SB);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  int rw = right ? text_width(right, fsb) + 2 : 0;
  draw_text(ctx, left, fsb, GRect(PAD, 0, w - 2 * PAD - (right ? rw + PAD : 0), b.size.h - 2),
            GTextAlignmentLeft, C_DIM);
  if (right) draw_text(ctx, right, fsb, GRect(w - PAD - rw, 0, rw, b.size.h - 2), GTextAlignmentRight, C_DIM);
  graphics_context_set_stroke_color(ctx, C_DIM);
  for (int x = PAD; x < w - PAD; x += 4) {
    graphics_draw_line(ctx, GPoint(x, b.size.h - 2), GPoint(x + 1, b.size.h - 2));
  }
}

// Selected row is drawn inverted (black on green), like a terminal selection bar.
static void draw_menu_row(GContext *ctx, const Layer *cell_layer, const char *title,
                          const char *subtitle) {
  GRect b = layer_get_bounds(cell_layer);
  bool hl = menu_cell_layer_is_highlighted(cell_layer);
  GColor fg = hl ? C_BG : C_BRIGHT;
  GColor dim = hl ? C_BG : C_DIM;
  GFont fs = fonts_get_system_font(FONT_S);
  GFont ft = fonts_get_system_font(FONT_MB);

  graphics_context_set_fill_color(ctx, hl ? C_BRIGHT : C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  char buf[96];
  snprintf(buf, sizeof(buf), "> %s", title);

  // New selected row: restart its scroll from the beginning
  if (hl) {
    char key[160];
    snprintf(key, sizeof(key), "%s|%s", title, subtitle ? subtitle : "");
    if (strcmp(key, s_marquee_key) != 0) {
      snprintf(s_marquee_key, sizeof(s_marquee_key), "%s", key);
      s_scroll_tick = 0;
    }
  }

  GColor bg = hl ? C_BRIGHT : C_BG;
  int w = b.size.w;
  if (subtitle && subtitle[0]) {
    draw_marquee(ctx, buf, ft, GRect(PAD, 1, w - 2 * PAD, LH_M), w, fg, bg, hl, s_scroll_tick, &s_scroll_active);
    draw_marquee(ctx, subtitle, fs, GRect(PAD, 1 + LH_M, w - 2 * PAD, LH_S), w, dim, bg, hl, s_scroll_tick, &s_scroll_active);
  } else {
    draw_marquee(ctx, buf, ft, GRect(PAD, (b.size.h - LH_M) / 2 - 2, w - 2 * PAD, LH_M), w, fg, bg, hl, s_scroll_tick, &s_scroll_active);
  }
}

static void style_menu_layer(MenuLayer *layer) {
  menu_layer_set_normal_colors(layer, C_BG, C_BRIGHT);
  menu_layer_set_highlight_colors(layer, C_BRIGHT, C_BG);
}

// Up on the first row jumps to the last; Down on the last row jumps to the first
static void menu_wrap_move(MenuLayer *layer, bool up, uint16_t rows) {
  MenuIndex idx = menu_layer_get_selected_index(layer);
  if (up && idx.row == 0) {
    menu_layer_set_selected_index(layer, MenuIndex(0, rows - 1), MenuRowAlignCenter, true);
  } else if (!up && idx.row >= rows - 1) {
    menu_layer_set_selected_index(layer, MenuIndex(0, 0), MenuRowAlignCenter, true);
  } else {
    menu_layer_set_selected_next(layer, up, MenuRowAlignCenter, true);
  }
}

// Switch palette live: recolor the windows and redraw everything
static void apply_theme(bool light, bool save) {
  set_theme_colors(light);
  if (save) persist_write_bool(PERSIST_KEY_THEME, light);
  if (s_main_window) window_set_background_color(s_main_window, C_BG);
  if (s_menu_window) window_set_background_color(s_menu_window, C_BG);
  if (s_tv_window) window_set_background_color(s_tv_window, C_BG);
  if (s_menu_layer) { style_menu_layer(s_menu_layer); menu_layer_reload_data(s_menu_layer); }
  if (s_tv_menu_layer) { style_menu_layer(s_tv_menu_layer); menu_layer_reload_data(s_tv_menu_layer); }
  ui_redraw();
}

/* ---------- TV list (nested under "select tv") ---------- */

static uint16_t tv_row_count(void) {
  return s_tv_count == 0 ? 1 : s_tv_count;
}

static uint16_t tv_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return tv_row_count();
}

static int16_t tv_get_header_height(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return MENU_HEADER_H;
}

static void tv_draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *context) {
  draw_menu_header(ctx, cell_layer, "$ select_tv_", NULL);
}

static int16_t tv_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return s_tv_count == 0 ? ROW_H_TALL : ROW_H_SHORT;
}

static void tv_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count == 0) {
    draw_menu_row(ctx, cell_layer, "loading tvs...", "check phone app link");
    return;
  }
  char buf[MAX_NAME_LEN + 8];
  snprintf(buf, sizeof(buf), "%s %s", cell_index->row == s_active_tv_index ? "[*]" : "[ ]",
           s_tv_names[cell_index->row]);
  draw_menu_row(ctx, cell_layer, buf, NULL);
}

static void tv_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  note_activity();
  if (s_tv_count == 0) return;
  s_active_tv_index = cell_index->row;
  set_active_tv_on_phone(s_tv_entities[s_active_tv_index]);
  set_tv_title(s_tv_names[s_active_tv_index]);
  persist_write_string(PERSIST_KEY_TV_NAME, s_tv_names[s_active_tv_index]);
  // New TV: clear the old now-playing info until the phone sends fresh data
  snprintf(s_np_state, sizeof(s_np_state), "--");
  s_np_title[0] = s_np_sub[0] = s_np_app[0] = '\0';
  s_np_pos = s_np_dur = 0;
  vibes_double_pulse();
  // Close both the list and the main menu, back to the remote screen
  if (s_menu_window) window_stack_remove(s_menu_window, false);
  window_stack_remove(s_tv_window, true);
}

static void tv_up_click(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  menu_wrap_move((MenuLayer *)context, true, tv_row_count());
}

static void tv_down_click(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  menu_wrap_move((MenuLayer *)context, false, tv_row_count());
}

static void tv_select_click(ClickRecognizerRef recognizer, void *context) {
  MenuLayer *layer = (MenuLayer *)context;
  MenuIndex idx = menu_layer_get_selected_index(layer);
  tv_select(layer, &idx, NULL);
}

static void tv_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, tv_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, tv_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, tv_select_click);
}

static void tv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_tv_menu_layer = menu_layer_create(safe_bounds(layer_get_bounds(window_layer)));
  style_menu_layer(s_tv_menu_layer);
  menu_layer_set_callbacks(s_tv_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = tv_get_num_rows,
    .get_header_height = tv_get_header_height,
    .draw_header = tv_draw_header,
    .get_cell_height = tv_get_cell_height,
    .draw_row = tv_draw_row,
  });
  // Own click handling (instead of the menu's default) so the list wraps around
  window_set_click_config_provider_with_context(window, tv_click_config, s_tv_menu_layer);
  layer_add_child(window_layer, menu_layer_get_layer(s_tv_menu_layer));
  if (s_active_tv_index >= 0 && s_active_tv_index < s_tv_count) {
    menu_layer_set_selected_index(s_tv_menu_layer, MenuIndex(0, s_active_tv_index), MenuRowAlignCenter, false);
  }
}

static void tv_window_unload(Window *window) {
  menu_layer_destroy(s_tv_menu_layer);
  s_tv_menu_layer = NULL;
  window_destroy(window);
  s_tv_window = NULL;
}

static void open_tv_list(void) {
  if (s_tv_window) return;
  s_tv_window = window_create();
  window_set_background_color(s_tv_window, C_BG);
  window_set_window_handlers(s_tv_window, (WindowHandlers) {
    .load = tv_window_load,
    .unload = tv_window_unload
  });
  window_stack_push(s_tv_window, true);
  scroll_timer_start();
  request_tv_list();
}

/* ---------- Main menu: select tv, power on/off, home, home (hold) ---------- */

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return MENU_ROWS;
}

static int16_t menu_get_header_height(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return MENU_HEADER_H;
}

static void menu_draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *context) {
  char tag[16];
  snprintf(tag, sizeof(tag), "[%s]", s_np_state);
  draw_menu_header(ctx, cell_layer, "$ menu_", tag);
}

static int16_t menu_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return cell_index->row == 0 ? ROW_H_TALL : ROW_H_SHORT;
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  switch (cell_index->row) {
    case 0: draw_menu_row(ctx, cell_layer, "select tv", s_tv_title); break;   // subtitle = active TV
    case 1: draw_menu_row(ctx, cell_layer, "power on", NULL); break;
    case 2: draw_menu_row(ctx, cell_layer, "power off", NULL); break;
    case 3: draw_menu_row(ctx, cell_layer, "home", NULL); break;
    case 4: draw_menu_row(ctx, cell_layer, "home (hold)", NULL); break;
    default: draw_menu_row(ctx, cell_layer, "reload tv", NULL); break;
  }
}

static void menu_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  note_activity();
  if (cell_index->row == 0) {
    open_tv_list();
    return;
  }
  const char *cmd;
  if (cell_index->row == 1) cmd = "power_on";
  else if (cell_index->row == 2) cmd = "power_off";
  else if (cell_index->row == 3) cmd = "home";
  else if (cell_index->row == 4) cmd = "home_hold";
  else cmd = "reload";
  send_command(cmd);
  vibes_short_pulse();
  window_stack_remove(s_menu_window, true);
}

static void menu_up_click(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  menu_wrap_move((MenuLayer *)context, true, MENU_ROWS);
}

static void menu_down_click(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  menu_wrap_move((MenuLayer *)context, false, MENU_ROWS);
}

static void menu_select_click(ClickRecognizerRef recognizer, void *context) {
  MenuLayer *layer = (MenuLayer *)context;
  MenuIndex idx = menu_layer_get_selected_index(layer);
  menu_select(layer, &idx, NULL);
}

static void menu_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, menu_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, menu_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, menu_select_click);
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(safe_bounds(layer_get_bounds(window_layer)));
  style_menu_layer(s_menu_layer);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_get_num_rows,
    .get_header_height = menu_get_header_height,
    .draw_header = menu_draw_header,
    .get_cell_height = menu_get_cell_height,
    .draw_row = menu_draw_row,
  });
  window_set_click_config_provider_with_context(window, menu_click_config, s_menu_layer);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
  window_destroy(window);
  s_menu_window = NULL;
}

static void open_tv_menu() {
  if (s_menu_window) return;
  s_menu_window = window_create();
  window_set_background_color(s_menu_window, C_BG);
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = menu_window_load,
    .unload = menu_window_unload
  });
  window_stack_push(s_menu_window, true);
  scroll_timer_start();
}

/* ---------- Back button: tap = menu, double-tap = exit ---------- */

static void back_timer_cb(void *data) {
  s_back_timer = NULL;
  send_command("menu");   // no second tap arrived: it was a single tap
}

static void back_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  if (s_back_timer) {
    app_timer_cancel(s_back_timer);
    s_back_timer = NULL;
    vibes_short_pulse();
    window_stack_pop_all(true);
  } else {
    s_back_timer = app_timer_register(DOUBLE_TAP_MS, back_timer_cb, NULL);
  }
}

/* ---------- Other buttons ---------- */

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  if (s_remote_mode == 0) send_command("left");
  else if (s_remote_mode == 1) send_command("up");
  else send_command("volume_up");
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  if (s_remote_mode == 0) send_command("right");
  else if (s_remote_mode == 1) send_command("down");
  else send_command("volume_down");
}

static void select_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  send_command("select");   // always plain select, in every mode
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  s_remote_mode = (s_remote_mode + 1) % 3;
  ui_redraw();
  vibes_short_pulse();
}

static void trigger_menu_handler(ClickRecognizerRef recognizer, void *context) {
  note_activity();
  open_tv_menu();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_long_click_subscribe(BUTTON_ID_UP, 600, trigger_menu_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 600, trigger_menu_handler, NULL);

  window_single_click_subscribe(BUTTON_ID_SELECT, select_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click_handler, NULL);

  window_single_click_subscribe(BUTTON_ID_BACK, back_single_click_handler);
}

/* ---------- Messages from the phone ---------- */

static void in_received_handler(DictionaryIterator *iter, void *context) {
  // Theme chosen on the settings page (0 = dark, 1 = light)
  Tuple *theme_tuple = dict_find(iter, KEY_THEME);
  if (theme_tuple) {
    bool light = theme_tuple->value->int32 != 0;
    if (light != s_light_theme) apply_theme(light, true);
    return;
  }

  // Now-playing update
  Tuple *np_state = dict_find(iter, KEY_NP_STATE);
  if (np_state) {
    Tuple *t;
    snprintf(s_np_state, sizeof(s_np_state), "%s", np_state->value->cstring);
    t = dict_find(iter, KEY_NP_TITLE);
    snprintf(s_np_title, sizeof(s_np_title), "%s", t ? t->value->cstring : "");
    t = dict_find(iter, KEY_NP_SUB);
    snprintf(s_np_sub, sizeof(s_np_sub), "%s", t ? t->value->cstring : "");
    t = dict_find(iter, KEY_NP_APP);
    snprintf(s_np_app, sizeof(s_np_app), "%s", t ? t->value->cstring : "");
    t = dict_find(iter, KEY_NP_POS);
    s_np_pos = t ? t->value->int32 : 0;
    t = dict_find(iter, KEY_NP_DUR);
    s_np_dur = t ? t->value->int32 : 0;
    ui_redraw();
    if (s_menu_layer) menu_layer_reload_data(s_menu_layer);
    return;
  }

  // Active TV's display name (sent after the list)
  Tuple *name_confirm_tuple = dict_find(iter, KEY_SET_ACTIVE_TV);
  if (name_confirm_tuple) {
    set_tv_title(name_confirm_tuple->value->cstring);
    persist_write_string(PERSIST_KEY_TV_NAME, s_tv_title);
    return;
  }

  Tuple *count_tuple = dict_find(iter, KEY_TV_COUNT);
  if (count_tuple) s_tv_count = 0;

  Tuple *name_tuple = dict_find(iter, KEY_TV_NAME);
  Tuple *entity_tuple = dict_find(iter, KEY_TV_ENTITY);

  if (name_tuple && entity_tuple) {
    // If this TV is already in the list (a retried message), update it instead of adding a duplicate
    int idx = -1;
    for (int i = 0; i < s_tv_count; i++) {
      if (strcmp(s_tv_entities[i], entity_tuple->value->cstring) == 0) { idx = i; break; }
    }
    if (idx == -1) {
      if (s_tv_count >= MAX_TVS) return;
      idx = s_tv_count++;
    }

    snprintf(s_tv_names[idx], MAX_NAME_LEN, "%s", name_tuple->value->cstring);
    snprintf(s_tv_entities[idx], MAX_NAME_LEN, "%s", entity_tuple->value->cstring);

    // Highlight the row whose name matches the TV currently shown
    if (strcmp(s_tv_names[idx], s_tv_title) == 0) {
      s_active_tv_index = idx;
    }

    if (s_tv_window && s_tv_menu_layer) menu_layer_reload_data(s_tv_menu_layer);
  }
}

/* ---------- Main window ---------- */

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);

  if (persist_exists(PERSIST_KEY_TV_NAME)) {
    persist_read_string(PERSIST_KEY_TV_NAME, s_tv_title, MAX_NAME_LEN);
  }

  s_ui_layer = layer_create(safe_bounds(layer_get_bounds(window_layer)));
  layer_set_update_proc(s_ui_layer, ui_update_proc);
  layer_add_child(window_layer, s_ui_layer);

  s_last_activity = s_last_tick = time(NULL);
  s_tick_idle = false;
  scroll_timer_ensure();
  s_tick_timer = app_timer_register(1000, tick_cb, NULL);
}

static void main_window_unload(Window *window) {
  if (s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_timer = NULL;
  }
  layer_destroy(s_ui_layer);
  s_ui_layer = NULL;
}

static void init() {
  set_theme_colors(persist_exists(PERSIST_KEY_THEME) ? persist_read_bool(PERSIST_KEY_THEME) : false);
  s_main_window = window_create();
  window_set_background_color(s_main_window, C_BG);
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  app_message_register_inbox_received(in_received_handler);
  app_message_open(512, 64);
  window_stack_push(s_main_window, true);
}

static void deinit() {
  if (s_back_timer) app_timer_cancel(s_back_timer);
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
