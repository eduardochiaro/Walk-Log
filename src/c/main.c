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

// Utities
bool is_color = false;
bool is_large = false;
bool is_round = false;

// ---- forward declarations ----
static void stop_tracking(bool expired);
static void update_app_glance(void);
static void deferred_expire_cb(void *data);

// ============================================================================
//  Health helper
// ============================================================================

static int32_t get_step_count(void) {
#if defined(PBL_HEALTH)
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(HealthMetricStepCount,
                                       time_start_of_today(), time(NULL));
  if (mask & HealthServiceAccessibilityMaskAvailable) {
    return (int32_t)health_service_sum_today(HealthMetricStepCount);
  }
  return 0;
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

  dict_write_int32(iter, MESSAGE_KEY_SESSION_START_TIME, (int32_t)session->start_time);
  dict_write_int32(iter, MESSAGE_KEY_SESSION_END_TIME,   (int32_t)session->end_time);
  dict_write_int32(iter, MESSAGE_KEY_SESSION_STEPS,     session->steps);
  dict_write_int32(iter, MESSAGE_KEY_SESSION_ELAPSED,   session->elapsed_seconds);

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

  // Every 60 seconds, update the persisted slow_minutes counter so the
  // background worker knows the user is still actively walking.
  if (s_elapsed_seconds % 60 == 0) {
    ActiveTracking at;
    if (active_tracking_load(&at)) {
      int32_t current_steps = get_step_count();
      int32_t delta = current_steps - at.last_checked_steps;
      if (delta < 0) delta = 0;
      if (delta >= WALK_PACE_THRESHOLD) {
        at.slow_minutes = 0;  // still walking — reset
      }
      at.last_checked_steps = current_steps;
      active_tracking_save(&at);
    }
  }

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
  if (s_alert_window) window_stack_remove(s_alert_window, false);
}

static void alert_load(Window *window) {
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  if (is_color) {
    window_set_background_color(window, GColorRed);
  }

  s_alert_text = text_layer_create(
      GRect(10, 30, bounds.size.w - 20, bounds.size.h - 40));
  {
    static char alert_msg[64];
    snprintf(alert_msg, sizeof(alert_msg),
             "Session Expired\n\nNo activity\nfor %ld min.",
             (long)s_settings.inactivity_timeout_minutes);
    text_layer_set_text(s_alert_text, alert_msg);
  }
  text_layer_set_font(s_alert_text,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_alert_text, GTextAlignmentCenter);
  text_layer_set_background_color(s_alert_text, GColorClear);
  if (is_color) {
    text_layer_set_text_color(s_alert_text, GColorWhite);
  }
  layer_add_child(root, text_layer_get_layer(s_alert_text));

  s_alert_dismiss_timer = app_timer_register(3500, alert_dismiss, NULL);
}

static void alert_unload(Window *window) {
  if (s_alert_text) text_layer_destroy(s_alert_text);
  s_alert_text = NULL;
  s_alert_window = NULL;
  window_destroy(window);
}

static void show_expire_alert(void) {
  s_alert_window = window_create();
  if (!s_alert_window) return;
  window_set_window_handlers(s_alert_window, (WindowHandlers) {
    .load   = alert_load,
    .unload = alert_unload,
  });
  window_stack_push(s_alert_window, true);
  vibes_long_pulse();
}

// ============================================================================
//  Deferred expire callback (runs AFTER window_load finishes)
// ============================================================================

static void deferred_expire_cb(void *data) {
  stop_tracking(true);
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
    .last_checked_steps    = s_steps_at_start,
    .slow_minutes          = 0,
  };
  active_tracking_save(&at);

  // Only launch background worker if inactivity timeout is enabled
  if (s_settings.inactivity_timeout_minutes > 0) {
    AppWorkerResult wr = app_worker_launch();
    APP_LOG(APP_LOG_LEVEL_INFO, "Worker launch result: %d", (int)wr);
  }

  text_layer_set_text(s_status_layer, "TRACKING");
  if (is_color) {
    text_layer_set_text_color(s_status_layer, GColorIslamicGreen);
  } 

  if (s_icon_stop) action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_stop);

  update_time_display();
  update_steps_display();
  start_tick_timer();
  update_app_glance();
}

static void stop_tracking(bool expired) {
  if (!s_tracking) return;
  s_tracking = false;
  stop_tick_timer();

  // Kill background worker (only if running)
  if (app_worker_is_running()) {
    app_worker_kill();
  }

  if (expired) {
    // Read expired session data and save to log + timeline.
    // The worker only flags the expiry; the app handles session storage.
    // The worker already subtracted inactivity time from end_time/elapsed_seconds.
    if (persist_exists(PERSIST_KEY_EXPIRED_SESSION)) {
      Session es;
      persist_read_data(PERSIST_KEY_EXPIRED_SESSION, &es, sizeof(Session));
      if (es.steps == 0 && s_steps_at_start > 0) {
        es.steps = get_step_count() - s_steps_at_start;
        if (es.steps < 0) es.steps = 0;
      }
      session_add(&es);
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
  if (is_color) {
    text_layer_set_text_color(s_status_layer, GColorDarkGray);
  }
  if (s_icon_play) action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_play);
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
  APP_LOG(APP_LOG_LEVEL_INFO, "main_window_load: start");
  Layer *root  = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // set utilities variables
  is_color = (bool) (PBL_IF_COLOR_ELSE(true, false));
  is_large = (bool) (bounds.size.w >= 200);
  is_round = (bool) (PBL_IF_ROUND_ELSE(true, false));

  // set background color
  window_set_background_color(window, GColorYellow);

  // ---- Icons ----

  s_icon_settings = gbitmap_create_with_resource(RESOURCE_ID_ICON_SETTINGS);
  s_icon_play     = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLAY);
  s_icon_stop     = gbitmap_create_with_resource(RESOURCE_ID_ICON_STOP);
  s_icon_logs     = gbitmap_create_with_resource(RESOURCE_ID_ICON_LOGS);

  if (!s_icon_settings || !s_icon_play || !s_icon_stop || !s_icon_logs) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to load one or more icons!");
  }

  // ---- Action bar (sidebar) ----
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_click_config_provider(s_action_bar, main_click_config);
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  if (s_icon_settings) action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP,     s_icon_settings);
  if (s_icon_play)     action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_play);
  if (s_icon_logs)     action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN,   s_icon_logs);
  action_bar_layer_add_to_window(s_action_bar, window);

  // Content area width (screen minus action bar)
  int cw_center = bounds.size.w - ACTION_BAR_WIDTH;
  int cw = is_round ? bounds.size.w : cw_center;

  GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  if (is_large) {
    title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  }

  // ---- App title ----
  s_title_layer = text_layer_create(GRect(0, is_round ? 20 : 0, cw, 28));
  text_layer_set_text(s_title_layer, "WALK LOG");
  text_layer_set_font(s_title_layer, title_font);
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_title_layer, GColorClear);
  text_layer_set_text_color(s_title_layer, GColorBlack);
  layer_add_child(root, text_layer_get_layer(s_title_layer));

  // ---- Status label (Gothic) ----
  s_status_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 50, cw, 28));
  text_layer_set_text(s_status_layer, "READY");
  text_layer_set_font(s_status_layer,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_layer, GColorClear);
  if (is_color) {
    text_layer_set_text_color(s_status_layer, GColorDarkGray);
  } else {
    text_layer_set_text_color(s_status_layer, GColorBlack);
  }
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  // ---- Timer display (LECO) ----
  int timer_h = is_large ? 56 : 50;
  s_time_layer = text_layer_create(GRect(0, (bounds.size.h / 2) - (timer_h /2), cw_center, timer_h));
  text_layer_set_text(s_time_layer, "00:00");
  text_layer_set_font(s_time_layer,
      fonts_get_system_font(is_large
          ? FONT_KEY_LECO_42_NUMBERS
          : FONT_KEY_LECO_36_BOLD_NUMBERS));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_time_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  GFont steps_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  if (is_large) {
    steps_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  }

  // ---- Steps label (Gothic) ----
  s_steps_layer = text_layer_create(GRect(0, bounds.size.h - 40, cw, 28));
  text_layer_set_text(s_steps_layer, "");
  text_layer_set_font(s_steps_layer,
      steps_font);
  text_layer_set_text_alignment(s_steps_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_steps_layer, GColorClear);
  if (is_color) {
    text_layer_set_text_color(s_steps_layer, GColorCobaltBlue);
  } else {
    text_layer_set_text_color(s_steps_layer, GColorBlack);
  }
  layer_add_child(root, text_layer_get_layer(s_steps_layer));

  // ---- Resume tracking state if we were tracking before app closed ----
  if (s_expired_while_closed) {
    // Session expired while app was closed – defer finalization to after
    // window_load returns, because stop_tracking pushes another window.
    s_expired_while_closed = false;
    app_timer_register(200, deferred_expire_cb, NULL);
  } else if (s_tracking) {
    text_layer_set_text(s_status_layer, "TRACKING");
    if (is_color) {
      text_layer_set_text_color(s_status_layer, GColorIslamicGreen);
    }
    if (s_icon_stop) action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_stop);
    update_time_display();
    update_steps_display();
    start_tick_timer();
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "main_window_load: done");
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
  APP_LOG(APP_LOG_LEVEL_INFO, "init: start");
  settings_load(&s_settings);
  APP_LOG(APP_LOG_LEVEL_INFO, "init: settings loaded, timeout=%ld",
          (long)s_settings.inactivity_timeout_minutes);

  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(256, 64);

  // Listen for messages from the background worker
  app_worker_message_subscribe(worker_message_handler);

  // ---- Data-version migration: clear stale tracking on struct change ----
  {
    int32_t stored_version = 0;
    if (persist_exists(PERSIST_KEY_DATA_VERSION)) {
      stored_version = persist_read_int(PERSIST_KEY_DATA_VERSION);
    }
    if (stored_version != CURRENT_DATA_VERSION) {
      APP_LOG(APP_LOG_LEVEL_WARNING,
              "Data version mismatch (%ld vs %d), clearing stale tracking",
              (long)stored_version, CURRENT_DATA_VERSION);
      persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
      persist_delete(PERSIST_KEY_WORKER_EXPIRED);
      persist_delete(PERSIST_KEY_EXPIRED_SESSION);
      persist_write_int(PERSIST_KEY_DATA_VERSION, CURRENT_DATA_VERSION);
    }
  }

  // Load active tracking state first (needed for steps even if worker expired)
  ActiveTracking at;
  bool had_active_tracking = active_tracking_load(&at);
  if (had_active_tracking) {
    s_start_time     = at.start_time;
    s_steps_at_start = at.steps_at_start;
  }

  // Check if the background worker expired the session while app was closed
  if (persist_exists(PERSIST_KEY_WORKER_EXPIRED)) {
    s_tracking = true;            // so stop_tracking(true) works in window_load
    s_expired_while_closed = true;
  } else if (had_active_tracking) {
    // We were tracking (worker should still be running)
    s_elapsed_seconds = (int)(time(NULL) - at.start_time);
    if (s_elapsed_seconds < 0) s_elapsed_seconds = 0;

    // Fallback: if worker was killed, check for expiry
    if (!app_worker_is_running() && s_settings.inactivity_timeout_minutes > 0) {
      time_t  now           = time(NULL);
      int32_t current_steps = get_step_count();
      int32_t minutes_total = (int32_t)((now - at.start_time) / 60);

      // Use persisted slow_minutes plus a rough estimate for time since
      // the worker last updated.
      int32_t total_slow = at.slow_minutes;
      if (minutes_total > 0 && at.last_checked_steps > 0) {
        int32_t step_delta  = current_steps - at.last_checked_steps;
        if (step_delta < 0) step_delta = 0;
        int32_t mins_since  = minutes_total;  // rough upper bound
        int32_t avg_pace    = (mins_since > 0) ? step_delta / mins_since : 0;
        if (avg_pace < WALK_PACE_THRESHOLD) {
          total_slow += mins_since;
        }
      }

      if (total_slow >= s_settings.inactivity_timeout_minutes) {
        int32_t inactivity_secs = s_settings.inactivity_timeout_minutes * 60;
        Session session = {
          .start_time      = at.start_time,
          .end_time        = now - (time_t)inactivity_secs,
          .steps           = current_steps - at.steps_at_start,
          .elapsed_seconds = (int32_t)(now - at.start_time) - inactivity_secs,
        };
        if (session.steps < 0) session.steps = 0;
        if (session.elapsed_seconds < 0) session.elapsed_seconds = 0;
        session_add(&session);
        persist_write_data(PERSIST_KEY_EXPIRED_SESSION, &session, sizeof(Session));
        active_tracking_clear();

        s_tracking = true;
        s_expired_while_closed = true;
      } else {
        s_tracking = true;
        // Re-launch worker (only if timeout enabled)
        if (s_settings.inactivity_timeout_minutes > 0) {
          ActiveTracking updated = at;
          updated.last_checked_steps = current_steps;
          active_tracking_save(&updated);
          AppWorkerResult wr = app_worker_launch();
          APP_LOG(APP_LOG_LEVEL_INFO, "Fallback worker launch: %d", (int)wr);
        }
      }
    } else {
      // Worker is still running (or timeout disabled), just resume UI
      s_tracking = true;
    }
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "init: creating main window, tracking=%d expired=%d",
          s_tracking, s_expired_while_closed);
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load   = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
  APP_LOG(APP_LOG_LEVEL_INFO, "init: done");
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
