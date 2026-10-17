#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_title_layer;
static TextLayer *s_mode_layer;

// TV Selection Menu Components
static Window *s_menu_window;
static MenuLayer *s_menu_layer;

#define MAX_TVS 10
#define MAX_NAME_LEN 32

static char s_tv_names[MAX_TVS][MAX_NAME_LEN];
static char s_tv_entities[MAX_TVS][MAX_NAME_LEN];
static int s_tv_count = 0;
static int s_active_tv_index = 0;

static int s_remote_mode = 1; // 0 = Up/Down, 1 = Left/Right

enum {
  KEY_COMMAND = 0,
  KEY_FETCH_TVS = 1,
  KEY_TV_COUNT = 2,
  KEY_TV_NAME = 3,
  KEY_TV_ENTITY = 4,
  KEY_SET_ACTIVE_TV = 5
};

// Communication Helpers
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

// Menu Layer Callbacks
static uint16_t get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_tv_count == 0 ? 1 : s_tv_count;
}

static void draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading TVs...", "Check phone app link", NULL);
    return;
  }
  
  char *name = s_tv_names[cell_index->row];
  char *sub = (cell_index->row == s_active_tv_index) ? "ACTIVE REMOTE" : "Select to control";
  menu_cell_basic_draw(ctx, cell_layer, name, sub, NULL);
}

static void select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  if (s_tv_count > 0) {
    s_active_tv_index = cell_index->row;
    set_active_tv_on_phone(s_tv_entities[s_active_tv_index]);
    
    // Update main window UI title
    text_layer_set_text(s_title_layer, s_tv_names[s_active_tv_index]);
    
    vibes_double_pulse();
    window_stack_remove(s_menu_window, true);
  }
}

// Menu Window Lifecycle
static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = get_num_rows_callback,
    .draw_row = draw_row_callback,
    .select_click = select_callback,
  });

  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
  
  request_tv_list(); // Ask phone for fresh list whenever menu is opened
}

static void menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
}

// Open TV Picker Menu
static void open_tv_menu() {
  s_menu_window = window_create();
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = menu_window_load,
    .unload = menu_window_unload
  });
  window_stack_push(s_menu_window, true);
}

// Button Handling
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_command(s_remote_mode == 0 ? "up" : "left");
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_command(s_remote_mode == 0 ? "down" : "right");
}

static void trigger_menu_handler(ClickRecognizerRef recognizer, void *context) {
  open_tv_menu();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_command("select");
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_remote_mode = (s_remote_mode == 1) ? 0 : 1;
  text_layer_set_text(s_mode_layer, s_remote_mode == 1 ? "[ Mode: Left / Right ]" : "[ Mode: Up / Down ]");
  vibes_short_pulse();
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  send_command("menu");
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  
  // LONG PRESS UP OR DOWN to open TV selection menu
  window_long_click_subscribe(BUTTON_ID_UP, 600, trigger_menu_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 600, trigger_menu_handler, NULL);
  
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

// App Message Inbound parsing (Receiving TV lists from Phone)
static void in_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *count_tuple = dict_find(iter, KEY_TV_COUNT);
  if (count_tuple) {
    s_tv_count = 0; // Reset list count
  }

  Tuple *name_tuple = dict_find(iter, KEY_TV_NAME);
  Tuple *entity_tuple = dict_find(iter, KEY_TV_ENTITY);
  
  if (name_tuple && entity_tuple && s_tv_count < MAX_TVS) {
    snprintf(s_tv_names[s_tv_count], MAX_NAME_LEN, "%s", name_tuple->value->cstring);
    snprintf(s_tv_entities[s_tv_count], MAX_NAME_LEN, "%s", entity_tuple->value->cstring);
    s_tv_count++;
    
    if (s_menu_window && window_stack_contains_window(s_menu_window)) {
      menu_layer_reload_data(s_menu_layer);
    }
  }
}

// UI Setup Main Window
static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_title_layer = text_layer_create(GRect(0, 30, bounds.size.w, 40));
  text_layer_set_text(s_title_layer, "SELECT A TV"); // Default text until loaded
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  s_mode_layer = text_layer_create(GRect(0, 80, bounds.size.w, 40));
  text_layer_set_text(s_mode_layer, "[ Mode: Left / Right ]");
  text_layer_set_text_alignment(s_mode_layer, GTextAlignmentCenter);
  text_layer_set_font(s_mode_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  layer_add_child(window_layer, text_layer_get_layer(s_mode_layer));
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_mode_layer);
}

static void init() {
  s_main_window = window_create();
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
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
