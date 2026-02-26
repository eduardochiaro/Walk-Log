#include <pebble.h>
#include "session.h"
#include "settings_window.h"
#include "logs_window.h"

// ============================================================================
//  Main window elements
// ============================================================================

static Window         *s_main_window;
static ActionBarLayer *s_action_bar;
static TextLayer      *s_title_layer;
static TextLayer      *s_status_layer;
static TextLayer      *s_time_layer;
static TextLayer      *s_steps_layer;

// Expire-alert window
static Window    *s_alert_window;
static TextLayer *s_alert_text;
static AppTimer  *s_alert_dismiss_timer;

// Action-bar icons
static GBitmap *s_icon_settings;
static GBitmap *s_icon_play;
static GBitmap *s_icon_stop;
static GBitmap *s_icon_logs;

// ============================================================================
//  Tracking state
// ============================================================================

static bool    s_tracking              = false;
static int     s_elapsed_seconds       = 0;
static time_t  s_start_time            = 0;
static int32_t s_steps_at_start        = 0;
static bool    s_expired_while_closed  = false;

static AppTimer *s_tick_timer          = NULL;

// App-wide settings
static Settings s_settings;

// ---- forward declarations ----
static void stop_tracking(bool expired);
static void update_app_glance(void);

// ============================================================================
//  Health helper
// ============================================================================

static int32_t get_step_count(void) {
#if defined(PBL_HEALTH)
  return (int32_t)health_service_sum_today(HealthMetricStepCount);
#else
  return 0;
#endif
}

// ============================================================================
//  UI updates
// ============================================================================

static char s_time_buf[16];
static char s_steps_buf[24];

static void update_time_display(void) {
  int m = s_elapsed_seconds / 60;
  int s = s_elapsed_seconds % 60;
  snprintf(s_time_buf, sizeof(s_time_buf), "%02d:%02d", m, s);
  text_layer_set_text(s_time_layer, s_time_buf);
}

static void update_steps_display(void) {
  if (!s_tracking) {
    text_layer_set_text(s_steps_layer, "");
    return;
  }
  int32_t session_steps = get_step_count() - s_steps_at_start;
  if (session_steps < 0) session_steps = 0;
  snprintf(s_steps_buf, sizeof(s_steps_buf), "steps: %ld", (long)session_steps);
  text_layer_set_text(s_steps_layer, s_steps_buf);
}

// ============================================================================
//  AppGlance – show tracking status on the launcher menu
// ============================================================================

#if !defined(PBL_PLATFORM_APLITE)

static void app_glance_reload_cb(AppGlanceReloadSession *session,
                                  size_t limit, void *ctx) {
  if (limit < 1) return;

  const AppGlanceSlice slice = (AppGlanceSlice) {
    .layout = {
      .icon            = APP_GLANCE_SLICE_DEFAULT_ICON,
      .subtitle_template_string = (const char *)ctx,
    },
    .expiration_time = APP_GLANCE_SLICE_NO_EXPIRATION,
  };
  app_glance_add_slice(session, slice);
}

#endif  // !PBL_PLATFORM_APLITE

#if !defined(PBL_PLATFORM_APLITE)
static char s_glance_buf[32];
#endif

static void update_app_glance(void) {
#if !defined(PBL_PLATFORM_APLITE)
  if (s_tracking) {
    snprintf(s_glance_buf, sizeof(s_glance_buf), "Tracking...");
  } else {
    int count = session_get_count();
    snprintf(s_glance_buf, sizeof(s_glance_buf), "%d sessions logged", count);
  }
  app_glance_reload(app_glance_reload_cb, s_glance_buf);
#endif  // !PBL_PLATFORM_APLITE
}

// ============================================================================
//  Timeline – send session data to PebbleKit JS
// ============================================================================

static void send_to_timeline(const Session *session) {
  if (!s_settings.save_in_timeline) return;

  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res != APP_MSG_OK) return;

  dict_write_int32(iter, MESSAGE_KEY_SessionStartTime, (int32_t)session->start_time);
  dict_write_int32(iter, MESSAGE_KEY_SessionEndTime,   (int32_t)session->end_time);
  dict_write_int32(iter, MESSAGE_KEY_SessionSteps,     session->steps);
  dict_write_int32(iter, MESSAGE_KEY_SessionElapsed,   session->elapsed_seconds);

  app_message_outbox_send();
}

// ============================================================================
//  Tick timer  (1 s while tracking)
// ============================================================================

static void tick_callback(void *data) {
  if (!s_tracking) return;
  s_elapsed_seconds++;
  update_time_display();
  update_steps_display();
  s_tick_timer = app_timer_register(1000, tick_callback, NULL);
}

static void start_tick_timer(void) {
  s_tick_timer = app_timer_register(1000, tick_callback, NULL);
}

static void stop_tick_timer(void) {
  if (s_tick_timer) { app_timer_cancel(s_tick_timer); s_tick_timer = NULL; }
}

// ============================================================================
//  Expire alert (full-screen notification when session auto-expires)
// ============================================================================

static void alert_dismiss(void *data) {
  if (s_alert_window) window_stack_remove(s_alert_window, true);
}

static void alert_load(Window *window) {
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

#ifdef PBL_COLOR
  window_set_background_color(window, GColorRed);
#endif

  s_alert_text = text_layer_create(
      GRect(10, bounds.size.h / 2 - 42, bounds.size.w - 20, 84));
  {
    static char alert_msg[64];
    snprintf(alert_msg, sizeof(alert_msg),
             "Session Expired\n\nNo activity for\n%ld minutes",
             (long)s_settings.inactivity_timeout_minutes);
    text_layer_set_text(s_alert_text, alert_msg);
  }
  text_layer_set_font(s_alert_text,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_alert_text, GTextAlignmentCenter);
  text_layer_set_background_color(s_alert_text, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_alert_text, GColorWhite);
#endif
  layer_add_child(root, text_layer_get_layer(s_alert_text));

  s_alert_dismiss_timer = app_timer_register(3500, alert_dismiss, NULL);
}

static void alert_unload(Window *window) {
  text_layer_destroy(s_alert_text);
  window_destroy(s_alert_window);
  s_alert_window = NULL;
}

static void show_expire_alert(void) {
  vibes_long_pulse();
  s_alert_window = window_create();
  window_set_window_handlers(s_alert_window, (WindowHandlers) {
    .load   = alert_load,
    .unload = alert_unload,
  });
  window_stack_push(s_alert_window, true);
}

// ============================================================================
//  Tracking start / stop
// ============================================================================

static void start_tracking(void) {
  s_tracking        = true;
  s_elapsed_seconds = 0;
  s_start_time      = time(NULL);
  s_steps_at_start  = get_step_count();

  // Persist state and launch background worker for inactivity monitoring
  ActiveTracking at = {
    .is_tracking           = true,
    .start_time            = s_start_time,
    .steps_at_start        = s_steps_at_start,
    .last_step_change_time = s_start_time,
    .last_checked_steps    = s_steps_at_start,
  };
  active_tracking_save(&at);
  app_worker_launch();

  text_layer_set_text(s_status_layer, "Tracking");
#ifdef PBL_COLOR
  text_layer_set_text_color(s_status_layer, GColorIslamicGreen);
#endif

  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_stop);

  update_time_display();
  update_steps_display();
  start_tick_timer();
  update_app_glance();
}

static void stop_tracking(bool expired) {
  if (!s_tracking) return;
  s_tracking = false;
  stop_tick_timer();

  // Kill background worker
  app_worker_kill();

  if (expired) {
    // Worker (or fallback check) already saved the session.
    // Read it back for timeline push.
    if (persist_exists(PERSIST_KEY_EXPIRED_SESSION)) {
      Session es;
      persist_read_data(PERSIST_KEY_EXPIRED_SESSION, &es, sizeof(Session));
      send_to_timeline(&es);
      persist_delete(PERSIST_KEY_EXPIRED_SESSION);
    }
    persist_delete(PERSIST_KEY_WORKER_EXPIRED);
  } else {
    // Manual stop — build and save session ourselves
    Session session = {
      .start_time      = s_start_time,
      .end_time        = time(NULL),
      .steps           = get_step_count() - s_steps_at_start,
      .elapsed_seconds = (int32_t)s_elapsed_seconds,
    };
    if (session.steps < 0) session.steps = 0;
    session_add(&session);
    send_to_timeline(&session);
  }

  // Reset UI
  text_layer_set_text(s_status_layer, "Ready");
#ifdef PBL_COLOR
  text_layer_set_text_color(s_status_layer, GColorDarkGray);
#endif
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_play);
  s_elapsed_seconds = 0;
  update_time_display();
  text_layer_set_text(s_steps_layer, "");

  // Clear persisted tracking state
  active_tracking_clear();
  update_app_glance();

  if (expired) {
    show_expire_alert();
  } else {
    vibes_short_pulse();
  }
}

// ============================================================================
//  Main-window button handlers
// ============================================================================

static void main_select(ClickRecognizerRef ref, void *ctx) {
  if (s_tracking) stop_tracking(false); else start_tracking();
}

static void main_up(ClickRecognizerRef ref, void *ctx) {
  settings_window_push(&s_settings);
}

static void main_down(ClickRecognizerRef ref, void *ctx) {
  logs_window_push();
}

static void main_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, main_select);
  window_single_click_subscribe(BUTTON_ID_UP,     main_up);
  window_single_click_subscribe(BUTTON_ID_DOWN,   main_down);
}

// ============================================================================
//  Main-window lifecycle
// ============================================================================

static void main_window_load(Window *window) {
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  // set background color
  window_set_background_color(window, GColorYellow);

  // ---- Icons ----

s_icon_settings = gbitmap_create_with_resource(RESOURCE_ID_ICON_SETTINGS_WHITE);
s_icon_play     = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLAY_WHITE);
s_icon_stop     = gbitmap_create_with_resource(RESOURCE_ID_ICON_STOP_WHITE);
s_icon_logs     = gbitmap_create_with_resource(RESOURCE_ID_ICON_LOGS_WHITE);

  // ---- Action bar (sidebar) ----
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_click_config_provider(s_action_bar, main_click_config);
#ifdef PBL_COLOR
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
#endif
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP,     s_icon_settings);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_play);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN,   s_icon_logs);
  action_bar_layer_add_to_window(s_action_bar, window);

  // Content area width (screen minus action bar)
  int cw = bounds.size.w - ACTION_BAR_WIDTH;

  // ---- App title ----
  s_title_layer = text_layer_create(GRect(0, 0, cw, 28));
  text_layer_set_text(s_title_layer, "WALK LOG");
  text_layer_set_font(s_title_layer,
      fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_title_layer, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_title_layer, GColorBlack);
#endif
  layer_add_child(root, text_layer_get_layer(s_title_layer));

  // ---- Status label (Gothic) ----
  s_status_layer = text_layer_create(GRect(0, 28, cw, 28));
  text_layer_set_text(s_status_layer, "READY");
  text_layer_set_font(s_status_layer,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_layer, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_status_layer, GColorDarkGray);
#endif
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  // ---- Timer display (LECO) ----
  bool large_screen = (bounds.size.w >= 200);
  int timer_h = large_screen ? 56 : 50;
  s_time_layer = text_layer_create(GRect(0, 58, cw, timer_h));
  text_layer_set_text(s_time_layer, "00:00");
  text_layer_set_font(s_time_layer,
      fonts_get_system_font(large_screen
          ? FONT_KEY_LECO_42_NUMBERS
          : FONT_KEY_LECO_36_BOLD_NUMBERS));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_time_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  // ---- Steps label (Gothic) ----
  s_steps_layer = text_layer_create(GRect(0, 58 + timer_h + 4, cw, 28));
  text_layer_set_text(s_steps_layer, "");
  text_layer_set_font(s_steps_layer,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_steps_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_steps_layer, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_steps_layer, GColorMidnightGreen);
#endif
  layer_add_child(root, text_layer_get_layer(s_steps_layer));

  // ---- Resume tracking state if we were tracking before app closed ----
  if (s_expired_while_closed) {
    // Session expired while app was closed – finalize it now
    s_expired_while_closed = false;
    stop_tracking(true);
  } else if (s_tracking) {
    text_layer_set_text(s_status_layer, "TRACKING");
#ifdef PBL_COLOR
    text_layer_set_text_color(s_status_layer, GColorIslamicGreen);
#endif
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_stop);
    update_time_display();
    update_steps_display();
    start_tick_timer();
  }
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_steps_layer);
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_settings);
  gbitmap_destroy(s_icon_play);
  gbitmap_destroy(s_icon_stop);
  gbitmap_destroy(s_icon_logs);
}

// ============================================================================
//  AppMessage handlers (for timeline JS communication)
// ============================================================================

static void outbox_sent(DictionaryIterator *iter, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Timeline message sent");
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                          void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Timeline message failed: %d", (int)reason);
}

// ============================================================================
//  Background-worker message handler
// ============================================================================

static void worker_message_handler(uint16_t type, AppWorkerMessage *msg) {
  if (type == WORKER_MSG_EXPIRED) {
    stop_tracking(true);
  }
}

// ============================================================================
//  App lifecycle
// ============================================================================

static void init(void) {
  settings_load(&s_settings);

  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(256, 64);

  // Listen for messages from the background worker
  app_worker_message_subscribe(worker_message_handler);

  // Check if the background worker expired the session while app was closed
  if (persist_exists(PERSIST_KEY_WORKER_EXPIRED)) {
    s_tracking = true;            // so stop_tracking(true) works in window_load
    s_expired_while_closed = true;
  } else {
    // Check if we were tracking (worker should still be running)
    ActiveTracking at;
    if (active_tracking_load(&at)) {
      s_start_time      = at.start_time;
      s_steps_at_start  = at.steps_at_start;
      s_elapsed_seconds = (int)(time(NULL) - s_start_time);

      // Fallback: if worker was killed, check for expiry via persist
      int32_t current_steps = get_step_count();
      time_t  now           = time(NULL);
      time_t  last_change   = at.last_step_change_time;

      if (current_steps > at.last_checked_steps) last_change = now;

      int timeout_sec = s_settings.inactivity_timeout_minutes * 60;
      if (last_change > 0 && (now - last_change) >= timeout_sec) {
        // Session should have expired — save it now
        Session session = {
          .start_time      = at.start_time,
          .end_time        = last_change + timeout_sec,
          .steps           = current_steps - at.steps_at_start,
          .elapsed_seconds = (int32_t)(last_change + timeout_sec - at.start_time),
        };
        if (session.steps < 0) session.steps = 0;
        session_add(&session);
        persist_write_data(PERSIST_KEY_EXPIRED_SESSION, &session, sizeof(Session));
        active_tracking_clear();

        s_tracking = true;
        s_expired_while_closed = true;
      } else {
        s_tracking = true;
        // Re-launch worker if it was killed
        if (!app_worker_is_running()) {
          ActiveTracking updated = at;
          updated.last_checked_steps    = current_steps;
          updated.last_step_change_time = last_change > 0 ? last_change : now;
          active_tracking_save(&updated);
          app_worker_launch();
        }
      }
    }
  }

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load   = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  if (s_tracking) {
    stop_tick_timer();
    // Background worker keeps running and maintains persist state
  }
  update_app_glance();
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
