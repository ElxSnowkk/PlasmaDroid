#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

struct NotificationManager;  // opaque handle

// Maximum number of notifications kept in the ring buffer.
static constexpr int kMaxNotifications = 64;

// A single notification entry.
struct NotificationEntry {
    int64_t     id = 0;
    int64_t     posted_time_ms = 0;
    char        package_name[128] = {};
    char        title[256] = {};
    char        text[512] = {};
    char        category[64] = {};
    bool        ongoing = false;
    bool        clearable = false;
    uint32_t    notification_id = 0;
    char        tag[64] = {};
};

// Create the notification manager.
struct NotificationManager* notification_manager_create();

// Destroy the notification manager and free all state.
void notification_manager_destroy(struct NotificationManager* mgr);

// Push a notification into the ring buffer (thread-safe).
// Returns the notification ID assigned.
int64_t notification_manager_push(struct NotificationManager* mgr,
                                   const struct NotificationEntry* entry);

// Remove a notification by internal ID (assigned by push).
void notification_manager_remove(struct NotificationManager* mgr,
                                  int64_t id);

// Remove a notification by Android notification ID and package name.
void notification_manager_remove_by_ext_id(struct NotificationManager* mgr,
                                            uint32_t notification_id,
                                            const char* package_name);

// Get the most recent notification. Returns true if one was available.
// The entry is removed from the queue.
bool notification_manager_pop(struct NotificationManager* mgr,
                               struct NotificationEntry* out_entry);

// Get the number of pending (unread) notifications.
int notification_manager_pending_count(struct NotificationManager* mgr);

// Clear all notifications.
void notification_manager_clear(struct NotificationManager* mgr);

// Check if a notification from a given package is active.
bool notification_manager_has_package(struct NotificationManager* mgr,
                                       const char* package_name);

// JNI bridge: called from Kotlin when a notification arrives.
// These are dispatched through jni_bridge.cpp.
typedef void (*NotificationCallback)(const struct NotificationEntry* entry,
                                      void* userdata);
void notification_manager_set_callback(struct NotificationManager* mgr,
                                        NotificationCallback cb,
                                        void* userdata);

#ifdef __cplusplus
}
#endif

// ===========================================================================
// Internal C++ types
// ===========================================================================
#ifdef __cplusplus

struct NotificationManager {
    std::mutex mutex;
    NotificationEntry entries[kMaxNotifications];
    int head = 0;  // next write position
    int count = 0;  // number of valid entries
    int64_t next_id = 1;

    NotificationCallback callback = nullptr;
    void* callback_userdata = nullptr;
};

#endif
