#include "settings_window.h"

static Window *s_window;
static StatusBarLayer *s_status_bar;
static MenuLayer *s_menu_layer;

static Settings *s_settings_ref;

#define NUM_ROWS 2

// ---- Timeout option cycling ----

static const int32_t s_timeout_options[] = { 1, 5, 10, 20, 30 };
#define NUM_TIMEOUT_OPTIONS 5

static void cycle_timeout(void) {
  int32_t cur = s_settings_ref->inactivity_timeout_minutes;
  for (int i = 0; i < NUM_TIMEOUT_OPTIONS; i++) {
    if (s_timeout_options[i] == cur) {
      int next = (i + 1) % NUM_TIMEOUT_OPTIONS;
      s_settings_ref->inactivity_timeout_minutes = s_timeout_options[next];
      return;
    }
  }
  s_settings_ref->inactivity_timeout_minutes = 10;
}

// ---- Value text helpers ----

static char s_timeout_buf[16];

static const char *get_subtitle(int row) {
  switch (row) {
    case 0:
      return s_settings_ref->save_in_timeline ? "Save Sessions" : "Don't Save";
    case 1:
      snprintf(s_timeout_buf, sizeof(s_timeout_buf), "%ld min",
               (long)s_settings_ref->inactivity_timeout_minutes);
      return s_timeout_buf;
    default: return "";
  }
}

// ---- MenuLayer callbacks ----

static uint16_t get_num_sections_cb(MenuLayer *menu, void *ctx) {
  return 1;
}

static uint16_t get_num_rows_cb(MenuLayer *menu, uint16_t section, void *ctx) {
  return NUM_ROWS;
}

static int16_t get_header_height_cb(MenuLayer *menu, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void draw_header_cb(GContext *gctx, const Layer *cell_layer,
                            uint16_t section, void *ctx) {
  menu_cell_basic_header_draw(gctx, cell_layer, "Settings");
}

static void draw_row_cb(GContext *gctx, const Layer *cell_layer,
                         MenuIndex *cell_index, void *ctx) {
  const char *title = NULL;
  switch (cell_index->row) {
    case 0: title = "Timeline"; break;
    case 1: title = "Inactivity"; break;
  }
  menu_cell_basic_draw(gctx, cell_layer, title, get_subtitle(cell_index->row), NULL);
}

static void select_cb(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
  switch (cell_index->row) {
    case 0:
      s_settings_ref->save_in_timeline = !s_settings_ref->save_in_timeline;
      break;
    case 1:
      cycle_timeout();
      break;
  }
  settings_persist(s_settings_ref);
  menu_layer_reload_data(s_menu_layer);
}

// ---- Window lifecycle ----

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorYellow);

  // Status bar
  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorYellow, GColorBlack);
  status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));

  // Menu layer below the status bar
  GRect menu_bounds = GRect(0, STATUS_BAR_LAYER_HEIGHT,
                            bounds.size.w,
                            bounds.size.h - STATUS_BAR_LAYER_HEIGHT);
  s_menu_layer = menu_layer_create(menu_bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections  = get_num_sections_cb,
    .get_num_rows      = get_num_rows_cb,
    .get_header_height = get_header_height_cb,
    .draw_header       = draw_header_cb,
    .draw_row          = draw_row_cb,
    .select_click      = select_cb,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);

  menu_layer_set_normal_colors(s_menu_layer, GColorYellow, GColorBlack);
  menu_layer_set_highlight_colors(s_menu_layer, GColorBlack, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void window_unload(Window *window) {
  status_bar_layer_destroy(s_status_bar);
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void settings_window_push(Settings *settings) {
  s_settings_ref = settings;

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
