#include "logs_window.h"

// ---- Helpers ----

static void format_elapsed(int32_t seconds, char *buf, size_t size) {
  int h = (int)(seconds / 3600);
  int m = (int)((seconds % 3600) / 60);
  int s = (int)(seconds % 60);
  if (h > 0) {
    snprintf(buf, size, "%dh %dm %ds", h, m, s);
  } else if (m > 0) {
    snprintf(buf, size, "%dm %ds", m, s);
  } else {
    snprintf(buf, size, "%ds", s);
  }
}

// Display index 0 = newest session
static bool load_by_display_index(int display_idx, Session *out) {
  int count = session_get_count();
  int storage_idx = count - 1 - display_idx;
  return session_load(storage_idx, out);
}

// Convert display index (newest=0) to storage index
static int display_to_storage(int display_idx) {
  return session_get_count() - 1 - display_idx;
}

// Forward declarations
static void refresh_list(void);

// Helper: send delete-pin message to JS
static void send_delete_pin(time_t start_time) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res != APP_MSG_OK) return;
  dict_write_int32(iter, MESSAGE_KEY_DELETE_PIN_START_TIME, (int32_t)start_time);
  app_message_outbox_send();
}

// ===========================================================================
//  Confirm dialog  (ActionBar style, like the screenshot)
// ===========================================================================

typedef void (*ConfirmCallback)(void *context);

static Window         *s_confirm_window;
static TextLayer      *s_confirm_icon_layer;
static TextLayer      *s_confirm_text_layer;
static ActionBarLayer *s_confirm_action_bar;
static GBitmap        *s_confirm_check_icon;

static ConfirmCallback s_confirm_cb;
static void           *s_confirm_ctx;
static char            s_confirm_msg[32];

static void confirm_select(ClickRecognizerRef ref, void *ctx) {
  window_stack_remove(s_confirm_window, false);
  if (s_confirm_cb) s_confirm_cb(s_confirm_ctx);
}

static void confirm_back(ClickRecognizerRef ref, void *ctx) {
  window_stack_remove(s_confirm_window, false);
}

static void confirm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select);
  window_single_click_subscribe(BUTTON_ID_BACK,   confirm_back);
}

static void confirm_load(Window *window) {
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorLightGray);

  // Action bar with checkmark
  s_confirm_check_icon = gbitmap_create_with_resource(
      RESOURCE_ID_ICON_CHECK);
  s_confirm_action_bar = action_bar_layer_create();
  action_bar_layer_set_click_config_provider(s_confirm_action_bar,
                                              confirm_click_config);
  if (s_confirm_check_icon) {
    action_bar_layer_set_icon(s_confirm_action_bar, BUTTON_ID_SELECT,
                              s_confirm_check_icon);
  }
  action_bar_layer_add_to_window(s_confirm_action_bar, window);

  int cw = bounds.size.w - ACTION_BAR_WIDTH;

  // Question-mark icon (large text "?")
  s_confirm_icon_layer = text_layer_create(
      GRect(0, bounds.size.h / 2 - 50, cw, 50));
  text_layer_set_text(s_confirm_icon_layer, "?");
  text_layer_set_font(s_confirm_icon_layer,
      fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_confirm_icon_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_confirm_icon_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_confirm_icon_layer));

  // Confirm text
  s_confirm_text_layer = text_layer_create(
      GRect(4, bounds.size.h / 2 + 4, cw - 4, 40));
  text_layer_set_text(s_confirm_text_layer, s_confirm_msg);
  text_layer_set_font(s_confirm_text_layer,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_confirm_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_confirm_text_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_confirm_text_layer));
}

static void confirm_unload(Window *window) {
  text_layer_destroy(s_confirm_icon_layer);
  text_layer_destroy(s_confirm_text_layer);
  action_bar_layer_destroy(s_confirm_action_bar);
  gbitmap_destroy(s_confirm_check_icon);
  s_confirm_window = NULL;
  window_destroy(window);
}

static void show_confirm(const char *message, ConfirmCallback cb, void *ctx) {
  snprintf(s_confirm_msg, sizeof(s_confirm_msg), "%s", message);
  s_confirm_cb  = cb;
  s_confirm_ctx = ctx;
  s_confirm_window = window_create();
  window_set_window_handlers(s_confirm_window, (WindowHandlers) {
    .load   = confirm_load,
    .unload = confirm_unload,
  });
  window_stack_push(s_confirm_window, true);
}

// ===========================================================================
//  Detail window  (shows one session's full info)
// ===========================================================================

static Window    *s_detail_window;
static StatusBarLayer *s_detail_status_bar;
static TextLayer *s_detail_date;
static TextLayer *s_detail_start;
static TextLayer *s_detail_end;
static TextLayer *s_detail_elapsed;
static TextLayer *s_detail_steps;

static int s_detail_index;

static char s_det_date_buf[32];
static char s_det_start_buf[32];
static char s_det_end_buf[32];
static char s_det_elapsed_buf[32];
static char s_det_steps_buf[32];

static void detail_delete_confirmed(void *context) {
  int display_idx = s_detail_index;
  int storage_idx = display_to_storage(display_idx);

  // Delete timeline pin before removing session
  Session session;
  if (session_load(storage_idx, &session)) {
    send_delete_pin(session.start_time);
  }

  session_delete(storage_idx);

  // Pop detail window back to list
  if (s_detail_window) window_stack_remove(s_detail_window, false);
}

static void detail_select_long(ClickRecognizerRef ref, void *ctx) {
  show_confirm("Delete log?", detail_delete_confirmed, NULL);
}

static void detail_click_config(void *ctx) {
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, detail_select_long, NULL);
}

static void detail_load(Window *window) {
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorYellow);
  bool is_round = (bool) PBL_IF_ROUND_ELSE(true, false);
  bool is_color = (bool) PBL_IF_COLOR_ELSE(true, false);
  bool has_health = (bool) PBL_IF_HEALTH_ELSE(true, false);

  Session session;
  load_by_display_index(s_detail_index, &session);


  // Format strings
  
  struct tm *st = localtime(&session.start_time);
  strftime(s_det_start_buf, sizeof(s_det_start_buf), "Start: %H:%M:%S", st);
  strftime(s_det_date_buf, sizeof(s_det_date_buf), "%b %d, %Y", st);

  struct tm *et = localtime(&session.end_time);
  strftime(s_det_end_buf, sizeof(s_det_end_buf), "End:    %H:%M:%S", et);

  char human[20];
  format_elapsed(session.elapsed_seconds, human, sizeof(human));
  snprintf(s_det_elapsed_buf, sizeof(s_det_elapsed_buf), "Time:  %s", human);

  if (has_health) {
    snprintf(s_det_steps_buf, sizeof(s_det_steps_buf), "Steps: %ld", (long)session.steps);
  } 

  // Status bar
  s_detail_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_detail_status_bar, GColorYellow, GColorBlack);
  status_bar_layer_set_separator_mode(s_detail_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_detail_status_bar));

  int y = STATUS_BAR_LAYER_HEIGHT + 8;
  int separator = 1;
  int lh = 20;

  // FULL Date`
  s_detail_date = text_layer_create(GRect(8, y, bounds.size.w - 16, lh + 4));
  text_layer_set_text(s_detail_date, s_det_date_buf);
  text_layer_set_font(s_detail_date, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  if (is_round) {
    text_layer_set_text_alignment(s_detail_date, GTextAlignmentCenter);
  }
  text_layer_set_background_color(s_detail_date, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_detail_date));
  y += lh + separator + 12;

  // Start
  s_detail_start = text_layer_create(GRect(8, y, bounds.size.w - 16, lh));
  text_layer_set_text(s_detail_start, s_det_start_buf);
  text_layer_set_font(s_detail_start, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  if (is_round) {
    text_layer_set_text_alignment(s_detail_start, GTextAlignmentCenter);
  }
  text_layer_set_background_color(s_detail_start, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_detail_start));
  y += lh + separator;

  // End
  s_detail_end = text_layer_create(GRect(8, y, bounds.size.w - 16, lh));
  text_layer_set_text(s_detail_end, s_det_end_buf);
  text_layer_set_font(s_detail_end, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  if (is_round) {
    text_layer_set_text_alignment(s_detail_end, GTextAlignmentCenter);
  }
  text_layer_set_background_color(s_detail_end, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_detail_end));
  y += lh + separator;

  // Elapsed
  s_detail_elapsed = text_layer_create(GRect(8, y, bounds.size.w - 16, lh));
  text_layer_set_text(s_detail_elapsed, s_det_elapsed_buf);
  text_layer_set_font(s_detail_elapsed, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  if (is_round) {
    text_layer_set_text_alignment(s_detail_elapsed, GTextAlignmentCenter);
  }
  text_layer_set_background_color(s_detail_elapsed, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_detail_elapsed));
  y += lh + separator;

  // Steps
  s_detail_steps = text_layer_create(GRect(8, y, bounds.size.w - 16, lh));
  text_layer_set_text(s_detail_steps, s_det_steps_buf);
  text_layer_set_font(s_detail_steps, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  if (is_round) {
    text_layer_set_text_alignment(s_detail_steps, GTextAlignmentCenter);
  }
  text_layer_set_background_color(s_detail_steps, GColorClear);
  if (is_color) {
    text_layer_set_text_color(s_detail_steps, GColorBlue);
  }
  layer_add_child(root, text_layer_get_layer(s_detail_steps));

  // Hook long-press for delete
  window_set_click_config_provider(window, detail_click_config);
}

static void detail_unload(Window *window) {
  status_bar_layer_destroy(s_detail_status_bar);
  text_layer_destroy(s_detail_date);
  text_layer_destroy(s_detail_start);
  text_layer_destroy(s_detail_end);
  text_layer_destroy(s_detail_elapsed);
  text_layer_destroy(s_detail_steps);
  s_detail_window = NULL;
  window_destroy(window);
}

static void push_detail(int display_index) {
  s_detail_index = display_index;
  s_detail_window = window_create();
  window_set_window_handlers(s_detail_window, (WindowHandlers) {
    .load   = detail_load,
    .unload = detail_unload,
  });
  window_stack_push(s_detail_window, true);
}

// ===========================================================================
//  List window  (MenuLayer, newest first)
// ===========================================================================

static Window         *s_list_window;
static StatusBarLayer *s_list_status_bar;
static MenuLayer      *s_list_menu;

static int s_list_count;

// Buffers for row drawing (reused per draw call)
static char s_row_title_buf[24];
static char s_row_sub_buf[40];

// ---- MenuLayer callbacks ----

static uint16_t list_get_num_sections(MenuLayer *menu, void *ctx) {
  return 1;
}

static uint16_t list_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_list_count > 0 ? (uint16_t)s_list_count : 1;
}

static int16_t list_get_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void list_draw_header(GContext *gctx, const Layer *cell_layer,
                              uint16_t section, void *ctx) {
  menu_cell_basic_header_draw(gctx, cell_layer, "Walk Logs");
}

static void list_draw_row(GContext *gctx, const Layer *cell_layer,
                           MenuIndex *cell_index, void *ctx) {
  if (s_list_count == 0) {
    menu_cell_basic_draw(gctx, cell_layer, "No sessions yet", NULL, NULL);
    return;
  }

  bool has_health = (bool) PBL_IF_HEALTH_ELSE(true, false);

  Session session;
  if (load_by_display_index(cell_index->row, &session)) {
    struct tm *t = localtime(&session.start_time);
    strftime(s_row_title_buf, sizeof(s_row_title_buf), "%b %d  %H:%M", t);

    char elapsed[16];
    format_elapsed(session.elapsed_seconds, elapsed, sizeof(elapsed));
    if (has_health) {
      snprintf(s_row_sub_buf, sizeof(s_row_sub_buf), "%s - %ld steps",
               elapsed, (long)session.steps);
    } else {
      snprintf(s_row_sub_buf, sizeof(s_row_sub_buf), "%s", elapsed);
    }

    menu_cell_basic_draw(gctx, cell_layer, s_row_title_buf, s_row_sub_buf, NULL);
  } else {
    menu_cell_basic_draw(gctx, cell_layer, "(error)", NULL, NULL);
  }
}

static void list_select_cb(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
  if (s_list_count == 0) return;
  push_detail(cell_index->row);
}

// ---- Long-press delete from list ----

static int s_pending_delete_display_idx;

static void list_delete_confirmed(void *context) {
  int storage_idx = display_to_storage(s_pending_delete_display_idx);

  // Delete timeline pin before removing session
  Session session;
  if (session_load(storage_idx, &session)) {
    send_delete_pin(session.start_time);
  }

  session_delete(storage_idx);
  refresh_list();
}

static void list_select_long_cb(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
  if (s_list_count == 0) return;
  s_pending_delete_display_idx = cell_index->row;
  show_confirm("Delete walk?", list_delete_confirmed, NULL);
}

// ---- List window lifecycle ----

static void list_load(Window *window) {
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorYellow);

  s_list_count = session_get_count();

  // Status bar
  s_list_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_list_status_bar, GColorYellow, GColorBlack);
  status_bar_layer_set_separator_mode(s_list_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_list_status_bar));

  // Menu layer
  GRect menu_bounds = GRect(0, STATUS_BAR_LAYER_HEIGHT,
                            bounds.size.w,
                            bounds.size.h - STATUS_BAR_LAYER_HEIGHT);
  s_list_menu = menu_layer_create(menu_bounds);
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections  = list_get_num_sections,
    .get_num_rows      = list_get_num_rows,
    .get_header_height = list_get_header_height,
    .draw_header       = list_draw_header,
    .draw_row          = list_draw_row,
    .select_click      = list_select_cb,
    .select_long_click = list_select_long_cb,
  });
  menu_layer_set_click_config_onto_window(s_list_menu, window);
  menu_layer_set_normal_colors(s_list_menu, GColorYellow, GColorBlack);
  menu_layer_set_highlight_colors(s_list_menu, GColorBlack, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_list_menu));
}

static void refresh_list(void) {
  s_list_count = session_get_count();
  if (s_list_menu) {
    menu_layer_reload_data(s_list_menu);
  }
}

static void list_window_appear(Window *window) {
  // Refresh when returning from detail (in case a session was deleted)
  refresh_list();
}

static void list_unload(Window *window) {
  status_bar_layer_destroy(s_list_status_bar);
  menu_layer_destroy(s_list_menu);
  s_list_menu = NULL;
  s_list_window = NULL;
  window_destroy(window);
}

void logs_window_push(void) {
  s_list_window = window_create();
  window_set_window_handlers(s_list_window, (WindowHandlers) {
    .load    = list_load,
    .unload  = list_unload,
    .appear  = list_window_appear,
  });
  window_stack_push(s_list_window, true);
}
