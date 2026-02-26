#pragma once
#include <pebble.h>

#define MAX_SESSIONS 50
#define PERSIST_KEY_NUM_SESSIONS 0
#define PERSIST_KEY_SETTINGS 1
#define PERSIST_KEY_ACTIVE_TRACKING 2   // ActiveTracking struct
#define PERSIST_KEY_WORKER_EXPIRED 3   // bool: worker expired a session
#define PERSIST_KEY_EXPIRED_SESSION 4  // Session: expired session for timeline
#define PERSIST_KEY_SESSION_BASE 10

// Worker ↔ App message types
#define WORKER_MSG_EXPIRED 0

typedef struct {
  time_t start_time;
  time_t end_time;
  int32_t steps;
  int32_t elapsed_seconds;
} Session;

typedef struct {
  bool save_in_timeline;
  int32_t inactivity_timeout_minutes;  // 1, 5, 10, 20, 30
} Settings;

// Session management
int session_get_count(void);
bool session_load(int index, Session *out);
void session_add(const Session *session);
void session_delete(int index);       // delete by storage index
void session_clear_all(void);

// Active tracking state (survives app close)
typedef struct {
  bool     is_tracking;
  time_t   start_time;
  int32_t  steps_at_start;
  time_t   last_step_change_time;  // last time steps were seen to increase
  int32_t  last_checked_steps;     // step count at last check
} ActiveTracking;

void active_tracking_save(const ActiveTracking *at);
bool active_tracking_load(ActiveTracking *out);
void active_tracking_clear(void);

// Settings management
void settings_load(Settings *out);
void settings_persist(const Settings *settings);
