#include "session.h"

int session_get_count(void) {
  if (persist_exists(PERSIST_KEY_NUM_SESSIONS)) {
    return (int)persist_read_int(PERSIST_KEY_NUM_SESSIONS);
  }
  return 0;
}

bool session_load(int index, Session *out) {
  int count = session_get_count();
  if (index < 0 || index >= count) return false;

  uint32_t key = PERSIST_KEY_SESSION_BASE + (uint32_t)index;
  if (persist_exists(key)) {
    persist_read_data(key, out, sizeof(Session));
    return true;
  }
  return false;
}

void session_add(const Session *session) {
  int count = session_get_count();

  // If at capacity, shift everything down (drop oldest)
  if (count >= MAX_SESSIONS) {
    for (int i = 0; i < MAX_SESSIONS - 1; i++) {
      Session temp;
      uint32_t src_key = PERSIST_KEY_SESSION_BASE + (uint32_t)(i + 1);
      uint32_t dst_key = PERSIST_KEY_SESSION_BASE + (uint32_t)i;
      if (persist_exists(src_key)) {
        persist_read_data(src_key, &temp, sizeof(Session));
        persist_write_data(dst_key, &temp, sizeof(Session));
      }
    }
    count = MAX_SESSIONS - 1;
  }

  uint32_t key = PERSIST_KEY_SESSION_BASE + (uint32_t)count;
  persist_write_data(key, session, sizeof(Session));
  persist_write_int(PERSIST_KEY_NUM_SESSIONS, count + 1);
}

void session_delete(int index) {
  int count = session_get_count();
  if (index < 0 || index >= count) return;

  // Shift sessions above this index down by one
  for (int i = index; i < count - 1; i++) {
    Session temp;
    uint32_t src = PERSIST_KEY_SESSION_BASE + (uint32_t)(i + 1);
    uint32_t dst = PERSIST_KEY_SESSION_BASE + (uint32_t)i;
    persist_read_data(src, &temp, sizeof(Session));
    persist_write_data(dst, &temp, sizeof(Session));
  }

  // Delete the last slot and update count
  persist_delete(PERSIST_KEY_SESSION_BASE + (uint32_t)(count - 1));
  persist_write_int(PERSIST_KEY_NUM_SESSIONS, count - 1);
}

void session_clear_all(void) {
  int count = session_get_count();
  for (int i = 0; i < count; i++) {
    persist_delete(PERSIST_KEY_SESSION_BASE + (uint32_t)i);
  }
  persist_write_int(PERSIST_KEY_NUM_SESSIONS, 0);
}

void settings_load(Settings *out) {
  if (persist_exists(PERSIST_KEY_SETTINGS)) {
    persist_read_data(PERSIST_KEY_SETTINGS, out, sizeof(Settings));
    // Guard against stale/zero value from older persisted data
    if (out->inactivity_timeout_minutes < 0) {
      out->inactivity_timeout_minutes = 10;
    }
  } else {
    // Defaults
    out->save_in_timeline = true;
    out->inactivity_timeout_minutes = 10;
  }
}

void settings_persist(const Settings *settings) {
  persist_write_data(PERSIST_KEY_SETTINGS, settings, sizeof(Settings));
}

void active_tracking_save(const ActiveTracking *at) {
  persist_write_data(PERSIST_KEY_ACTIVE_TRACKING, at, sizeof(ActiveTracking));
}

bool active_tracking_load(ActiveTracking *out) {
  if (persist_exists(PERSIST_KEY_ACTIVE_TRACKING)) {
    // Zero-init so any unread bytes are safe
    memset(out, 0, sizeof(ActiveTracking));
    int bytes = persist_read_data(PERSIST_KEY_ACTIVE_TRACKING, out, sizeof(ActiveTracking));
    if (bytes != (int)sizeof(ActiveTracking)) {
      // Size mismatch — struct layout changed since data was written
      APP_LOG(APP_LOG_LEVEL_WARNING,
              "ActiveTracking size mismatch: got %d, expected %d",
              bytes, (int)sizeof(ActiveTracking));
      persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
      memset(out, 0, sizeof(ActiveTracking));
      return false;
    }
    // Sanity-check fields
    if (out->start_time <= 0 || out->slow_minutes < 0) {
      persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
      memset(out, 0, sizeof(ActiveTracking));
      return false;
    }
    return out->is_tracking;
  }
  return false;
}

void active_tracking_clear(void) {
  persist_delete(PERSIST_KEY_ACTIVE_TRACKING);
}
