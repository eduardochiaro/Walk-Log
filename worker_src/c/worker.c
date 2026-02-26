#include <pebble_worker.h>

// ============================================================================
//  Shared definitions (must match session.h in the app)
// ============================================================================

#define PERSIST_KEY_NUM_SESSIONS     0
#define PERSIST_KEY_SETTINGS         1
#define PERSIST_KEY_ACTIVE_TRACKING  2
#define PERSIST_KEY_WORKER_EXPIRED   3
#define PERSIST_KEY_EXPIRED_SESSION  4
#define PERSIST_KEY_SESSION_BASE     10

#define MAX_SESSIONS                 50
#define WORKER_MSG_EXPIRED           0

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
  time_t   last_step_change_time;
  int32_t  last_checked_steps;
} ActiveTracking;

// ============================================================================
//  State
// ============================================================================

static int32_t s_last_checked_steps    = 0;
static time_t  s_last_step_change_time = 0;
static int32_t s_timeout_minutes       = 10;
static time_t  s_start_time            = 0;
static int32_t s_steps_at_start        = 0;
static bool    s_monitoring            = false;

// ============================================================================
//  Helpers
// ============================================================================

static int32_t get_step_count(void) {
#if defined(PBL_HEALTH)
  return (int32_t)health_service_sum_today(HealthMetricStepCount);
#else
  return 0;
#endif
}

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
//  Expiry logic
// ============================================================================

static void expire_session(void) {
  time_t now = time(NULL);
  int32_t current_steps = get_step_count();

  Session session = {
    .start_time      = s_start_time,
    .end_time        = now,
    .steps           = current_steps - s_steps_at_start,
    .elapsed_seconds = (int32_t)(now - s_start_time),
  };
  if (session.steps < 0) session.steps = 0;

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
//  Tick handler (fires every minute)
// ============================================================================

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (!s_monitoring) return;

  int32_t current = get_step_count();
  time_t  now     = time(NULL);

  if (current > s_last_checked_steps) {
    s_last_checked_steps    = current;
    s_last_step_change_time = now;

    // Update persist so app fallback check has fresh data
    ActiveTracking at;
    if (persist_exists(PERSIST_KEY_ACTIVE_TRACKING)) {
      persist_read_data(PERSIST_KEY_ACTIVE_TRACKING, &at, sizeof(ActiveTracking));
      at.last_step_change_time = now;
      at.last_checked_steps    = current;
      persist_write_data(PERSIST_KEY_ACTIVE_TRACKING, &at, sizeof(ActiveTracking));
    }
  }

  int elapsed_inactive = (int)(now - s_last_step_change_time);
  if (elapsed_inactive >= s_timeout_minutes * 60) {
    expire_session();
  }
}

// ============================================================================
//  Initialization
// ============================================================================

static void start_monitoring(void) {
  // Read active tracking state
  if (!persist_exists(PERSIST_KEY_ACTIVE_TRACKING)) return;

  ActiveTracking at;
  persist_read_data(PERSIST_KEY_ACTIVE_TRACKING, &at, sizeof(ActiveTracking));
  if (!at.is_tracking) return;

  // Read settings
  Settings settings = { .save_in_timeline = true, .inactivity_timeout_minutes = 10 };
  if (persist_exists(PERSIST_KEY_SETTINGS)) {
    persist_read_data(PERSIST_KEY_SETTINGS, &settings, sizeof(Settings));
    if (settings.inactivity_timeout_minutes <= 0) {
      settings.inactivity_timeout_minutes = 10;
    }
  }

  s_start_time            = at.start_time;
  s_steps_at_start        = at.steps_at_start;
  s_timeout_minutes       = settings.inactivity_timeout_minutes;
  s_last_checked_steps    = at.last_checked_steps > 0
                              ? at.last_checked_steps : get_step_count();
  s_last_step_change_time = at.last_step_change_time > 0
                              ? at.last_step_change_time : time(NULL);
  s_monitoring            = true;

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void init(void) {
  start_monitoring();
}

static void deinit(void) {
  if (s_monitoring) {
    tick_timer_service_unsubscribe();
  }
}

int main(void) {
  init();
  worker_event_loop();
  deinit();
}
