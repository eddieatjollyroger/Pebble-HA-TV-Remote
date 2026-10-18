#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_title_layer;
static TextLayer *s_mode_layer;

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

#define MAX_TVS 10
#define MAX_NAME_LEN 64

// STORAGE HARDWARE MEMORY ADDR KEY
#define PERSIST_KEY_TV_NAME 200

static char s_tv_names[MAX_TVS][MAX_NAME_LEN];
static char s_tv_entities[MAX_TVS][MAX_NAME_LEN];
static int s_tv_count = 0;
static int s_active_tv_index = -1; 

static int s_remote_mode = 0; 

static bool s_select_is_held = false;
static bool s_combo_was_triggered = false;

enum { 
  KEY_COMMAND = 0, 
  KEY_FETCH_TVS = 1, 
  KEY_TV_COUNT = 2, 
  KEY_TV_NAME = 3, 
  KEY_TV_ENTITY = 4, 
  KEY_SET_ACTIVE_TV = 5 
};

static void send_command(char *command_str) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (!iter) return;
  dict_write_cstring(iter, KEY_COMMAND, command_str);
  app_message_outbox_send();
}

static void request_tv_list() {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (!iter) return;
  dict_write_int8(iter, KEY_FETCH_TVS, 1);
  app_message_outbox_send();
}

static void set_active_tv_on_phone(char *entity_id) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (!iter) return;
  dict_write_cstring(iter, KEY_SET_ACTIVE_TV, entity_id);
  app_message_outbox_send();
}

static uint16_t get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_tv_count == 0 ? 1 : s_tv_count;
}

static void draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading TVs...", "Check phone app link", NULL);
    return;
  }
  menu_cell_basic_draw(ctx, cell_layer, s_tv_names[cell_index->row], (cell_index->row == s_active_tv_index) ? "ACTIVE REMOTE" : "Select to control", NULL);
}

static void select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count > 0) {
    s_active_tv_index = cell_index->row;
    set_active_tv_on_phone(s_tv_entities[s_active_tv_index]);
    text_layer_set_text(s_title_layer, s_tv_names[s_active_tv_index]);
    
    // Save selection straight to internal silicon storage memory blocks
    persist_write_string(PERSIST_KEY_TV_NAME, s_tv_names[s_active_tv_index]);
    
    vibes_double_pulse();
    window_stack_remove(s_menu_window, true);
  }
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  s_menu_layer = menu_layer_create(bounds);
  
  menu_layer_set_normal_colors(s_menu_layer, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, GColorDarkCandyAppleRed, GColorWhite);
  
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = get_num_rows_callback,
    .draw_row = draw_row_callback,
    .select_click = select_callback,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) { 
  menu_layer_destroy(s_menu_layer); 
}

static void open_tv_menu() {
  s_menu_window = window_create();
  window_set_background_color(s_menu_window, GColorBlack); 
  window_set_window_handlers(s_menu_window, (WindowHandlers) { .load = menu_window_load, .unload = menu_window_unload });
  window_stack_push(s_menu_window, true);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_select_is_held) {
    send_command("power");
    s_combo_was_triggered = true; 
    vibes_short_pulse();
  } else {
    if (s_remote_mode == 0) send_command("left");
    else if (s_remote_mode == 1) send_command("up");
    else if (s_remote_mode == 2) send_command("volume_up");
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_select_is_held) {
    send_command("home");
    s_combo_was_triggered = true; 
    vibes_short_pulse();
  } else {
    if (s_remote_mode == 0) send_command("right");
    else if (s_remote_mode == 1) send_command("down");
    else if (s_remote_mode == 2) send_command("volume_down");
  }
}

static void select_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_combo_was_triggered) {
    if (s_remote_mode == 2) send_command("menu");
    else send_command("select");
  }
  s_combo_was_triggered = false;
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_combo_was_triggered) {
    s_remote_mode = (s_remote_mode + 1) % 3;
    if (s_remote_mode == 0) text_layer_set_text(s_mode_layer, "[ Mode: Left / Right ]");
    else if (s_remote_mode == 1) text_layer_set_text(s_mode_layer, "[ Mode: Up / Down ]");
    else if (s_remote_mode == 2) text_layer_set_text(s_mode_layer, "[ Mode: Volume Control ]");
    vibes_short_pulse();
  }
}

static void select_raw_down_handler(ClickRecognizerRef recognizer, void *context) {
  s_select_is_held = true;
  s_combo_was_triggered = false;
}

static void select_raw_up_handler(ClickRecognizerRef recognizer, void *context) {
  s_select_is_held = false;
}

static void back_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_command("menu");
}

static void back_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  vibes_short_pulse();
  window_stack_pop_all(true); 
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
  window_raw_click_subscribe(BUTTON_ID_SELECT, select_raw_down_handler, select_raw_up_handler, NULL);
  
  window_single_click_subscribe(BUTTON_ID_BACK, back_single_click_handler);
  window_long_click_subscribe(BUTTON_ID_BACK, 500, back_long_click_handler, NULL);
}

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
  
  if (name_tuple && entity_tuple && s_tv_count < MAX_TVS) {
    snprintf(s_tv_names[s_tv_count], MAX_NAME_LEN, "%s", name_tuple->value->cstring);
    snprintf(s_tv_entities[s_tv_count], MAX_NAME_LEN, "%s", entity_tuple->value->cstring);
    
    // FIXED: Compare the incoming stream row name to your current main screen header title string
    if (strcmp(s_tv_names[s_tv_count], text_layer_get_text(s_title_layer)) == 0) {
      s_active_tv_index = s_tv_count; // Locks the highlight row instantly!
    }
    
    if (s_tv_count == 0 && persist_exists(PERSIST_KEY_TV_NAME) == false) {
      text_layer_set_text(s_title_layer, s_tv_names[s_tv_count]);
    }
    
    s_tv_count++;
    if (s_menu_window && window_stack_get_top_window() == s_menu_window) {
      menu_layer_reload_data(s_menu_layer);
    }
  }
}

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
  text_layer_set_text(s_mode_layer, "[ Mode: Left / Right ]");
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
  window_set_window_handlers(s_main_window, (WindowHandlers) { .load = main_window_load, .unload = main_window_unload });
  
  app_message_register_inbox_received(in_received_handler);
  app_message_open(256, 64);
  window_stack_push(s_main_window, true);
}

static void deinit() { 
  window_destroy(s_main_window); 
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
