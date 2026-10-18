#include <pebble.h>

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

// Double-tap Back within this window (ms) to exit
#define DOUBLE_TAP_MS 250

// Terminal palette
#define C_BRIGHT GColorGreen
#define C_DIM    GColorIslamicGreen
#define C_WARN   GColorYellow

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
#define IDLE_TICK_MS 60000    // when idle, refresh once a minute
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
  KEY_NP_DUR = 11
};

// Main menu rows: 0 = select tv, 1 = power on, 2 = power off, 3 = home, 4 = home (hold)
#define MENU_ROWS 5

static const char *s_mode_labels[] = {
  "> mode: left/right", "> mode: up/down", "> mode: volume"
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

static void draw_text(GContext *ctx, const char *text, GFont font, GRect rect,
                      GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis, align, NULL);
}

static void ui_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w;
  int h = b.size.h;

  GFont f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont f14b = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  bool playing = strcmp(s_np_state, "PLAY") == 0;
  bool paused = strcmp(s_np_state, "PAUSE") == 0;
  bool off = strcmp(s_np_state, "OFF") == 0 || strcmp(s_np_state, "OFFLINE") == 0;

  // Layout, anchored to the bottom so it adapts to the screen height
  int footer_y = h - 18;
  int times_y = h - 36;
  int bar_y = times_y - 10;
  int app_y = bar_y - 18;
  int sub_y = app_y - 17;
  int title_y = 38;
  int title_h = sub_y - title_y - 1;

  char buf[80];

  // Header: "# <tv name>"            [STATE]
  snprintf(buf, sizeof(buf), "# %s", s_tv_title);
  draw_text(ctx, buf, f14b, GRect(4, 0, w - 4 - 66, 18), GTextAlignmentLeft, C_BRIGHT);
  snprintf(buf, sizeof(buf), "[%s]", s_np_state);
  draw_text(ctx, buf, f14b, GRect(w - 66, 0, 62, 18), GTextAlignmentRight,
            playing ? C_BRIGHT : (paused ? C_WARN : C_DIM));

  // Dashed divider
  graphics_context_set_stroke_color(ctx, C_DIM);
  for (int x = 4; x < w - 4; x += 4) {
    graphics_draw_line(ctx, GPoint(x, 20), GPoint(x + 1, 20));
  }

  // Prompt with blinking cursor
  snprintf(buf, sizeof(buf), "$ now_playing%s", s_cursor_on ? "_" : " ");
  draw_text(ctx, buf, f14, GRect(4, 22, w - 8, 16), GTextAlignmentLeft, C_DIM);

  // Title (up to two lines)
  if (s_np_title[0]) {
    draw_text(ctx, s_np_title, f18b, GRect(4, title_y, w - 8, title_h), GTextAlignmentLeft, C_BRIGHT);
  } else {
    draw_text(ctx, off ? "[ standby ]" : "-- nothing playing --", f18,
              GRect(4, title_y, w - 8, title_h), GTextAlignmentLeft, C_DIM);
  }

  // Subtitle (artist / series) and app
  if (s_np_sub[0]) {
    draw_text(ctx, s_np_sub, f14, GRect(4, sub_y, w - 8, 16), GTextAlignmentLeft, C_BRIGHT);
  }
  if (s_np_app[0]) {
    snprintf(buf, sizeof(buf), "via %s", s_np_app);
    draw_text(ctx, buf, f14, GRect(4, app_y, w - 8, 16), GTextAlignmentLeft, C_DIM);
  }

  // Progress bar: [██████    ]
  int bx = 4;
  int bw = w - 8;
  int bh = 8;
  graphics_context_set_stroke_color(ctx, C_DIM);
  graphics_draw_rect(ctx, GRect(bx, bar_y, bw, bh));
  if (s_np_dur > 0) {
    int32_t pos = s_np_pos > s_np_dur ? s_np_dur : s_np_pos;
    int inner = bw - 4;
    int filled = (int)(((int64_t)inner * pos) / s_np_dur);
    graphics_context_set_fill_color(ctx, C_BRIGHT);
    for (int x = 0; x < filled; x += 4) {
      int seg = (filled - x) < 3 ? (filled - x) : 3;
      graphics_fill_rect(ctx, GRect(bx + 2 + x, bar_y + 2, seg, bh - 4), 0, GCornerNone);
    }
  }

  // Elapsed / total
  char t1[16], t2[16];
  if (s_np_dur > 0) {
    fmt_time(t1, sizeof(t1), s_np_pos);
    fmt_time(t2, sizeof(t2), s_np_dur);
  } else {
    snprintf(t1, sizeof(t1), "--:--");
    snprintf(t2, sizeof(t2), "--:--");
  }
  draw_text(ctx, t1, f14, GRect(4, times_y, w / 2 - 4, 16), GTextAlignmentLeft, C_DIM);
  draw_text(ctx, t2, f14, GRect(w / 2, times_y, w / 2 - 4, 16), GTextAlignmentRight, C_DIM);

  // Footer: current remote mode
  draw_text(ctx, s_mode_labels[s_remote_mode], f14, GRect(4, footer_y, w - 8, 16),
            GTextAlignmentLeft, C_BRIGHT);
}

// Ticks every second while in use, once a minute when idle. Progress is advanced
// by the real time elapsed since the last tick, so it stays correct either way.
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
  s_tick_timer = app_timer_register(active ? 1000 : IDLE_TICK_MS, tick_cb, NULL);
}

// Call on any button press: wakes the 1-second tick if it was idling
static void note_activity(void) {
  s_last_activity = time(NULL);
  if (s_tick_idle && s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_idle = false;
    tick_cb(NULL);   // catch up progress now and resume 1 s ticks
  }
}

/* ---------- Terminal-style menus ---------- */

#define MENU_HEADER_H 19
#define ROW_H_SHORT 27
#define ROW_H_TALL 40

static void draw_menu_header(GContext *ctx, const Layer *cell_layer, const char *left, const char *right) {
  GRect b = layer_get_bounds(cell_layer);
  int w = b.size.w;
  GFont f14b = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  draw_text(ctx, left, f14b, GRect(4, 0, w - 4 - 66, 18), GTextAlignmentLeft, C_DIM);
  if (right) draw_text(ctx, right, f14b, GRect(w - 66, 0, 62, 18), GTextAlignmentRight, C_DIM);
  graphics_context_set_stroke_color(ctx, C_DIM);
  for (int x = 4; x < w - 4; x += 4) {
    graphics_draw_line(ctx, GPoint(x, b.size.h - 2), GPoint(x + 1, b.size.h - 2));
  }
}

// Selected row is drawn inverted (black on green), like a terminal selection bar
static void draw_menu_row(GContext *ctx, const Layer *cell_layer, const char *title, const char *subtitle) {
  GRect b = layer_get_bounds(cell_layer);
  bool hl = menu_cell_layer_is_highlighted(cell_layer);
  GColor fg = hl ? GColorBlack : C_BRIGHT;
  GColor dim = hl ? GColorBlack : C_DIM;
  GFont f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  graphics_context_set_fill_color(ctx, hl ? C_BRIGHT : GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  char buf[96];
  snprintf(buf, sizeof(buf), "> %s", title);
  if (subtitle && subtitle[0]) {
    draw_text(ctx, buf, f18b, GRect(4, 1, b.size.w - 8, 22), GTextAlignmentLeft, fg);
    draw_text(ctx, subtitle, f14, GRect(4, 23, b.size.w - 8, 16), GTextAlignmentLeft, dim);
  } else {
    draw_text(ctx, buf, f18b, GRect(4, (b.size.h - 22) / 2 - 2, b.size.w - 8, 22), GTextAlignmentLeft, fg);
  }
}

static void style_menu_layer(MenuLayer *layer) {
  menu_layer_set_normal_colors(layer, GColorBlack, C_BRIGHT);
  menu_layer_set_highlight_colors(layer, C_BRIGHT, GColorBlack);
}

/* ---------- TV list (nested under "select tv") ---------- */

static uint16_t tv_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_tv_count == 0 ? 1 : s_tv_count;
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

static void tv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_tv_menu_layer = menu_layer_create(layer_get_bounds(window_layer));
  style_menu_layer(s_tv_menu_layer);
  menu_layer_set_callbacks(s_tv_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = tv_get_num_rows,
    .get_header_height = tv_get_header_height,
    .draw_header = tv_draw_header,
    .get_cell_height = tv_get_cell_height,
    .draw_row = tv_draw_row,
    .select_click = tv_select,
  });
  menu_layer_set_click_config_onto_window(s_tv_menu_layer, window);
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
  window_set_background_color(s_tv_window, GColorBlack);
  window_set_window_handlers(s_tv_window, (WindowHandlers) {
    .load = tv_window_load,
    .unload = tv_window_unload
  });
  window_stack_push(s_tv_window, true);
}

/* ---------- Main menu: select tv, power, home, home (hold) ---------- */

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
    default: draw_menu_row(ctx, cell_layer, "home (hold)", NULL); break;
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
  else cmd = "home_hold";
  send_command(cmd);
  vibes_short_pulse();
  window_stack_remove(s_menu_window, true);
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(layer_get_bounds(window_layer));
  style_menu_layer(s_menu_layer);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_get_num_rows,
    .get_header_height = menu_get_header_height,
    .draw_header = menu_draw_header,
    .get_cell_height = menu_get_cell_height,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
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
  window_set_background_color(s_menu_window, GColorBlack);
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = menu_window_load,
    .unload = menu_window_unload
  });
  window_stack_push(s_menu_window, true);
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
  send_command(s_remote_mode == 2 ? "menu" : "select");
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

  s_ui_layer = layer_create(layer_get_bounds(window_layer));
  layer_set_update_proc(s_ui_layer, ui_update_proc);
  layer_add_child(window_layer, s_ui_layer);

  s_last_activity = s_last_tick = time(NULL);
  s_tick_idle = false;
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
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);
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
