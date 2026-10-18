#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_title_layer;
static TextLayer *s_mode_layer;

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

static char s_tv_names[MAX_TVS][MAX_NAME_LEN];
static char s_tv_entities[MAX_TVS][MAX_NAME_LEN];
static int s_tv_count = 0;
static int s_active_tv_index = -1;

static int s_remote_mode = 0;

static AppTimer *s_back_timer = NULL;

enum {
  KEY_COMMAND = 0,
  KEY_FETCH_TVS = 1,
  KEY_TV_COUNT = 2,
  KEY_TV_NAME = 3,
  KEY_TV_ENTITY = 4,
  KEY_SET_ACTIVE_TV = 5
};

// Main menu: row 0 = "Select TV" (opens the TV list), then the actions
#define NUM_ACTIONS 4

static const char *s_action_titles[NUM_ACTIONS] = {
  "Power On", "Power Off", "Home", "Home (hold)"
};
// Command strings the phone JS understands
static const char *s_action_commands[NUM_ACTIONS] = {
  "power_on", "power_off", "home", "home_hold"
};

static const char *s_mode_labels[] = {
  "[ Mode: Left / Right ]", "[ Mode: Up / Down ]", "[ Mode: Volume Control ]"
};

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

/* ---------- TV list (nested under "Select TV") ---------- */

static uint16_t tv_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_tv_count == 0 ? 1 : s_tv_count;
}

static void tv_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading TVs...", "Check phone app link", NULL);
    return;
  }
  menu_cell_basic_draw(ctx, cell_layer, s_tv_names[cell_index->row],
                       (cell_index->row == s_active_tv_index) ? "ACTIVE REMOTE" : "Select to control", NULL);
}

static void tv_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count == 0) return;
  s_active_tv_index = cell_index->row;
  set_active_tv_on_phone(s_tv_entities[s_active_tv_index]);
  text_layer_set_text(s_title_layer, s_tv_names[s_active_tv_index]);
  persist_write_string(PERSIST_KEY_TV_NAME, s_tv_names[s_active_tv_index]);
  vibes_double_pulse();
  // Close both the list and the main menu, back to the remote screen
  if (s_menu_window) window_stack_remove(s_menu_window, false);
  window_stack_remove(s_tv_window, true);
}

static void tv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_tv_menu_layer = menu_layer_create(layer_get_bounds(window_layer));
  menu_layer_set_normal_colors(s_tv_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_tv_menu_layer, GColorDarkCandyAppleRed, GColorWhite);
  menu_layer_set_callbacks(s_tv_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = tv_get_num_rows,
    .draw_row = tv_draw_row,
    .select_click = tv_select,
  });
  menu_layer_set_click_config_onto_window(s_tv_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_tv_menu_layer));
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

/* ---------- Main menu: Select TV + actions ---------- */

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return NUM_ACTIONS + 1;
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row == 0) {
    // Subtitle shows the TV that's currently active
    menu_cell_basic_draw(ctx, cell_layer, "Select TV", text_layer_get_text(s_title_layer), NULL);
  } else {
    menu_cell_basic_draw(ctx, cell_layer, s_action_titles[cell_index->row - 1], NULL, NULL);
  }
}

static void menu_select(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (cell_index->row == 0) {
    open_tv_list();
    return;
  }
  send_command(s_action_commands[cell_index->row - 1]);
  vibes_short_pulse();
  window_stack_remove(s_menu_window, true);
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(layer_get_bounds(window_layer));
  menu_layer_set_normal_colors(s_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, GColorDarkCandyAppleRed, GColorWhite);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_get_num_rows,
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
  if (s_remote_mode == 0) send_command("left");
  else if (s_remote_mode == 1) send_command("up");
  else send_command("volume_up");
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_remote_mode == 0) send_command("right");
  else if (s_remote_mode == 1) send_command("down");
  else send_command("volume_down");
}

static void select_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_command(s_remote_mode == 2 ? "menu" : "select");
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_remote_mode = (s_remote_mode + 1) % 3;
  text_layer_set_text(s_mode_layer, s_mode_labels[s_remote_mode]);
  vibes_short_pulse();
}

static void trigger_menu_handler(ClickRecognizerRef recognizer, void *context) {
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
  Tuple *name_confirm_tuple = dict_find(iter, KEY_SET_ACTIVE_TV);
  if (name_confirm_tuple) {
    static char active_display_name[MAX_NAME_LEN];
    snprintf(active_display_name, MAX_NAME_LEN, "%s", name_confirm_tuple->value->cstring);
    text_layer_set_text(s_title_layer, active_display_name);
    persist_write_string(PERSIST_KEY_TV_NAME, active_display_name);
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

    // Highlight the row whose name matches the title currently shown
    if (strcmp(s_tv_names[idx], text_layer_get_text(s_title_layer)) == 0) {
      s_active_tv_index = idx;
    }

    if (s_tv_window && s_tv_menu_layer) menu_layer_reload_data(s_tv_menu_layer);
  }
}

/* ---------- Main window ---------- */

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_title_layer = text_layer_create(GRect(0, 35, bounds.size.w, 40));

  static char s_cached_boot_name[MAX_NAME_LEN];
  if (persist_exists(PERSIST_KEY_TV_NAME)) {
    persist_read_string(PERSIST_KEY_TV_NAME, s_cached_boot_name, MAX_NAME_LEN);
    text_layer_set_text(s_title_layer, s_cached_boot_name);
  } else {
    text_layer_set_text(s_title_layer, "SELECT A TV");
  }

  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_title_layer, GColorWhite);
  text_layer_set_background_color(s_title_layer, GColorClear);
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  s_mode_layer = text_layer_create(GRect(0, 95, bounds.size.w, 40));
  text_layer_set_text(s_mode_layer, s_mode_labels[0]);
  text_layer_set_text_alignment(s_mode_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_mode_layer, GColorWhite);
  text_layer_set_background_color(s_mode_layer, GColorClear);
  text_layer_set_font(s_mode_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  layer_add_child(window_layer, text_layer_get_layer(s_mode_layer));
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_mode_layer);
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
  app_message_open(256, 64);
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
