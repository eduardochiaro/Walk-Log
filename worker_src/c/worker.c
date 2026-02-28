#include <pebble_worker.h>

// ============================================================================
//  Shared definitions (must match session.h in the app)
// ============================================================================

#define PERSIST_KEY_SETTINGS         1
#define PERSIST_KEY_ACTIVE_TRACKING  2
#define PERSIST_KEY_WORKER_EXPIRED   3
#define PERSIST_KEY_EXPIRED_SESSION  4
#define PERSIST_KEY_DATA_VERSION     5

#define WORKER_MSG_EXPIRED           0
#define WALK_PACE_THRESHOLD          30  // steps/minute
#define CURRENT_DATA_VERSION         2

typedef struct {
  time_t  start_time;
  time_t  end_time;
  int32_t steps;
  int32_t elapsed_seconds;
} Session;

typedef struct {
  bool    save_in_timeline;
  int32_t inactivity_timeout_minutes;
} Settings;

typedef struct {
  bool     is_tracking;
  time_t   start_time;
  int32_t  steps_at_start;
  int32_t  last_checked_steps;
  int32_t  slow_minutes;
} ActiveTracking;

// ============================================================================
//  State
// ============================================================================

static int32_t s_last_checked_steps = 0;
static int32_t s_slow_minutes       = 0;
static int32_t s_timeout_minutes    = 10;
static time_t  s_start_time         = 0;
static int32_t s_steps_at_start     = 0;
static bool    s_monitoring         = false;

// ============================================================================
//  Helpers
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
//  Persist tracking state (so app fallback and worker restart work)
//  Uses a static struct to keep it off the call stack.
// ============================================================================

static ActiveTracking s_persist_buf;

static void persist_tracking_state(void) {
  s_persist_buf.is_tracking        = true;
  s_persist_buf.start_time         = s_start_time;
  s_persist_buf.steps_at_start     = s_steps_at_start;
  s_persist_buf.last_checked_steps = s_last_checked_steps;
  s_persist_buf.slow_minutes       = s_slow_minutes;
  persist_write_data(PERSIST_KEY_ACTIVE_TRACKING,
                     &s_persist_buf, sizeof(ActiveTracking));
}

// ============================================================================
//  Expiry logic  (lightweight — the app saves to the session log)
// ============================================================================

static Session s_expire_session_buf;

static void expire_session(void) {
  time_t now = time(NULL);
  int32_t inactivity_secs = s_timeout_minutes * 60;

  s_expire_session_buf.start_time      = s_start_time;
  s_expire_session_buf.end_time        = now - (time_t)inactivity_secs;
  s_expire_session_buf.steps           = get_step_count() - s_steps_at_start;
  s_expire_session_buf.elapsed_seconds = (int32_t)(now - s_start_time) - inactivity_secs;
  if (s_expire_session_buf.steps < 0) s_expire_session_buf.steps = 0;
  if (s_expire_session_buf.elapsed_seconds < 0) s_expire_session_buf.elapsed_seconds = 0;

  // Store session data for the foreground app to save & send to timeline
  persist_write_data(PERSIST_KEY_EXPIRED_SESSION,
                     &s_expire_session_buf, sizeof(Session));

  // Set expired flag
  persist_write_bool(PERSIST_KEY_WORKER_EXPIRED, true);

  // Notify foreground app (if running)
  AppWorkerMessage msg = { .data0 = 0 };
  app_worker_send_message(WORKER_MSG_EXPIRED, &msg);

  s_monitoring = false;
  tick_timer_service_unsubscribe();
}

// ============================================================================
//  Tick handler (fires every minute) — pace-based inactivity
// ============================================================================

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (!s_monitoring) return;

  int32_t current = get_step_count();
  int32_t delta   = current - s_last_checked_steps;
  if (delta < 0) delta = 0;  // day rollover

  int32_t prev_slow = s_slow_minutes;

  if (delta < WALK_PACE_THRESHOLD) {
    s_slow_minutes++;
    if (s_slow_minutes >= s_timeout_minutes) {
      expire_session();
      return;
    }
  } else {
    s_slow_minutes = 0;
  }

  s_last_checked_steps = current;

  // Only persist when slow_minutes changed to reduce flash writes
  if (s_slow_minutes != prev_slow) {
    persist_tracking_state();
  }
}

// ============================================================================
//  Initialization
// ============================================================================

static void start_monitoring(void) {
  if (!persist_exists(PERSIST_KEY_ACTIVE_TRACKING)) return;

  // Validate data version
  if (!persist_exists(PERSIST_KEY_DATA_VERSION) ||
      persist_read_int(PERSIST_KEY_DATA_VERSION) != CURRENT_DATA_VERSION) {
    persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
    return;
  }

  ActiveTracking at;
  memset(&at, 0, sizeof(at));
  int bytes = persist_read_data(PERSIST_KEY_ACTIVE_TRACKING,
                                &at, sizeof(ActiveTracking));
  if (bytes != (int)sizeof(ActiveTracking)) {
    persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
    return;
  }
  if (!at.is_tracking || at.start_time <= 0) return;

  // Read settings (use static to keep off stack)
  static Settings settings;
  settings.save_in_timeline = true;
  settings.inactivity_timeout_minutes = 10;
  if (persist_exists(PERSIST_KEY_SETTINGS)) {
    persist_read_data(PERSIST_KEY_SETTINGS, &settings, sizeof(Settings));
    if (settings.inactivity_timeout_minutes <= 0) {
      settings.inactivity_timeout_minutes = 10;
    }
  }

  s_start_time         = at.start_time;
  s_steps_at_start     = at.steps_at_start;
  s_timeout_minutes    = settings.inactivity_timeout_minutes;
  s_last_checked_steps = at.last_checked_steps > 0
                           ? at.last_checked_steps : get_step_count();
  s_slow_minutes       = (at.slow_minutes >= 0 && at.slow_minutes < 1440)
                           ? at.slow_minutes : 0;
  s_monitoring         = true;

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void init(void) {
  start_monitoring();
}

static void deinit(void) {
  if (s_monitoring) {
    // Do NOT call persist_tracking_state() here.  When the foreground app
    // kills the worker via app_worker_kill(), the worker deinit would
    // re-write is_tracking=true AFTER the app already cleared it, causing
    // the session to appear still running on next launch.
    tick_timer_service_unsubscribe();
  }
}

int main(void) {
  init();
  worker_event_loop();
  deinit();
}
