#include <pebble_worker.h>

// ============================================================================
//  Shared definitions (must match session.h in the app)
// ============================================================================

#define PERSIST_KEY_NUM_SESSIONS     0
#define PERSIST_KEY_SETTINGS         1
#define PERSIST_KEY_ACTIVE_TRACKING  2
#define PERSIST_KEY_WORKER_EXPIRED   3
#define PERSIST_KEY_EXPIRED_SESSION  4
#define PERSIST_KEY_DATA_VERSION     5
#define PERSIST_KEY_SESSION_BASE     10

#define MAX_SESSIONS                 50
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

// NOTE: Health API (health_service_sum_today) is NOT available in the
// background-worker runtime.  Calling it crashes the firmware into recovery
// mode.  The worker therefore relies purely on elapsed time: every minute
// without the foreground app resetting slow_minutes counts as a slow minute.
// When slow_minutes >= timeout, the session is expired.

static void save_session(const Session *session) {
  int count = 0;
  if (persist_exists(PERSIST_KEY_NUM_SESSIONS)) {
    count = (int)persist_read_int(PERSIST_KEY_NUM_SESSIONS);
  }
  if (count >= MAX_SESSIONS) {
    for (int i = 0; i < MAX_SESSIONS - 1; i++) {
      Session temp;
      uint32_t src = PERSIST_KEY_SESSION_BASE + (uint32_t)(i + 1);
      uint32_t dst = PERSIST_KEY_SESSION_BASE + (uint32_t)i;
      if (persist_exists(src)) {
        persist_read_data(src, &temp, sizeof(Session));
        persist_write_data(dst, &temp, sizeof(Session));
      }
    }
    count = MAX_SESSIONS - 1;
  }
  uint32_t key = PERSIST_KEY_SESSION_BASE + (uint32_t)count;
  persist_write_data(key, session, sizeof(Session));
  persist_write_int(PERSIST_KEY_NUM_SESSIONS, count + 1);
}

// ============================================================================
//  Persist tracking state (so app fallback and worker restart work)
// ============================================================================

static void persist_tracking_state(void) {
  ActiveTracking at = {
    .is_tracking        = true,
    .start_time         = s_start_time,
    .steps_at_start     = s_steps_at_start,
    .last_checked_steps = s_last_checked_steps,
    .slow_minutes       = s_slow_minutes,
  };
  persist_write_data(PERSIST_KEY_ACTIVE_TRACKING, &at, sizeof(ActiveTracking));
}

// ============================================================================
//  Expiry logic
// ============================================================================

static void expire_session(void) {
  time_t now = time(NULL);

  Session session = {
    .start_time      = s_start_time,
    .end_time        = now,
    .steps           = 0,   // worker can't read health; app fills in steps
    .elapsed_seconds = (int32_t)(now - s_start_time),
  };

  APP_LOG(APP_LOG_LEVEL_INFO, "Worker: expiring session after %ld sec",
          (long)session.elapsed_seconds);

  // Save session to log
  save_session(&session);

  // Store for the foreground app to send to timeline
  persist_write_data(PERSIST_KEY_EXPIRED_SESSION, &session, sizeof(Session));

  // Set expired flag
  persist_write_bool(PERSIST_KEY_WORKER_EXPIRED, true);

  // Clear active tracking
  persist_delete(PERSIST_KEY_ACTIVE_TRACKING);

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

  // Worker counts every minute as a "slow" minute since it cannot read
  // the Health API.  The foreground app resets slow_minutes when it detects
  // active walking.  If the app is closed, the worker eventually expires
  // the session after inactivity_timeout_minutes.
  s_slow_minutes++;

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Worker tick: slow=%ld/%ld",
          (long)s_slow_minutes, (long)s_timeout_minutes);

  if (s_slow_minutes >= s_timeout_minutes) {
    expire_session();
    return;
  }

  // Persist state every tick so app fallback stays current
  persist_tracking_state();
}

// ============================================================================
//  Initialization
// ============================================================================

static void start_monitoring(void) {
  if (!persist_exists(PERSIST_KEY_ACTIVE_TRACKING)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Worker: no active tracking state found");
    return;
  }

  // Validate data version
  if (!persist_exists(PERSIST_KEY_DATA_VERSION) ||
      persist_read_int(PERSIST_KEY_DATA_VERSION) != CURRENT_DATA_VERSION) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Worker: data version mismatch, aborting");
    persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
    return;
  }

  ActiveTracking at;
  memset(&at, 0, sizeof(at));
  int bytes = persist_read_data(PERSIST_KEY_ACTIVE_TRACKING, &at, sizeof(ActiveTracking));
  if (bytes != (int)sizeof(ActiveTracking)) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Worker: ActiveTracking size mismatch (%d vs %d)",
            bytes, (int)sizeof(ActiveTracking));
    persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
    return;
  }
  if (!at.is_tracking || at.start_time <= 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Worker: tracking flag is false or bad start_time, not monitoring");
    return;
  }

  // Read settings
  Settings settings = { .save_in_timeline = true, .inactivity_timeout_minutes = 10 };
  if (persist_exists(PERSIST_KEY_SETTINGS)) {
    persist_read_data(PERSIST_KEY_SETTINGS, &settings, sizeof(Settings));
    if (settings.inactivity_timeout_minutes <= 0) {
      settings.inactivity_timeout_minutes = 10;
    }
  }

  s_start_time         = at.start_time;
  s_steps_at_start     = at.steps_at_start;
  s_timeout_minutes    = settings.inactivity_timeout_minutes;
  s_last_checked_steps = 0;   // not used by worker anymore
  s_slow_minutes       = (at.slow_minutes >= 0 && at.slow_minutes < 1440)
                           ? at.slow_minutes : 0;
  s_monitoring         = true;

  APP_LOG(APP_LOG_LEVEL_INFO,
          "Worker: started monitoring, timeout=%ld min, steps=%ld, slow=%ld",
          (long)s_timeout_minutes, (long)s_last_checked_steps, (long)s_slow_minutes);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void init(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Worker: init");
  start_monitoring();
}

static void deinit(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Worker: deinit, monitoring=%d", s_monitoring);
  if (s_monitoring) {
    // Do NOT call persist_tracking_state() here.  When the foreground app
    // kills the worker via app_worker_kill(), the worker deinit would
    // re-write is_tracking=true AFTER the app already cleared it, causing
    // the session to appear still running on next launch.
    // The tick handler already persists every minute, so state is fresh.
    tick_timer_service_unsubscribe();
  }
}

int main(void) {
  init();
  worker_event_loop();
  deinit();
}
