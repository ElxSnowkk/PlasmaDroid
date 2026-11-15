#include "notification_bridge.h"

#include <android/log.h>
#include <string.h>

#include <algorithm>

#define LOG_TAG "Piperella/notif"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" NotificationManager* notification_manager_create() {
    auto* mgr = new NotificationManager();
    memset(mgr->entries, 0, sizeof(mgr->entries));
    LOGI("Notification manager created");
    return mgr;
}

extern "C" void notification_manager_destroy(NotificationManager* mgr) {
    if (!mgr) return;
    LOGI("Notification manager destroyed");
    delete mgr;
}

extern "C" int64_t notification_manager_push(NotificationManager* mgr,
                                              const NotificationEntry* entry) {
    if (!mgr || !entry) return -1;
    std::lock_guard<std::mutex> g(mgr->mutex);

    int64_t id = mgr->next_id++;
    mgr->entries[mgr->head] = *entry;
    mgr->entries[mgr->head].id = id;

    mgr->head = (mgr->head + 1) % kMaxNotifications;
    if (mgr->count < kMaxNotifications) {
        mgr->count++;
    } else {
        // Ring buffer full — overwrite oldest.
        // head already advanced past it.
    }

    LOGI("Notification pushed: %s — %s (id=%lld)",
         entry->package_name, entry->title, (long long)id);

    // Fire callback if registered.
    if (mgr->callback) {
        mgr->callback(&mgr->entries[(mgr->head - 1 + kMaxNotifications) % kMaxNotifications],
                       mgr->callback_userdata);
    }

    return id;
}

extern "C" void notification_manager_remove(NotificationManager* mgr,
                                             int64_t id) {
    if (!mgr || id <= 0) return;
    std::lock_guard<std::mutex> g(mgr->mutex);

    for (int i = 0; i < mgr->count; ++i) {
        int idx = (mgr->head - mgr->count + i + kMaxNotifications) % kMaxNotifications;
        if (mgr->entries[idx].id == id) {
            // Shift remaining entries forward.
            for (int j = i; j < mgr->count - 1; ++j) {
                int src = (mgr->head - mgr->count + j + 1 + kMaxNotifications) % kMaxNotifications;
                int dst = (mgr->head - mgr->count + j + kMaxNotifications) % kMaxNotifications;
                mgr->entries[dst] = mgr->entries[src];
            }
            mgr->count--;
            memset(&mgr->entries[(mgr->head - 1 + kMaxNotifications) % kMaxNotifications],
                   0, sizeof(NotificationEntry));
            LOGI("Notification removed (id=%lld)", (long long)id);
            return;
        }
    }
}

extern "C" bool notification_manager_pop(NotificationManager* mgr,
                                          NotificationEntry* out_entry) {
    if (!mgr || !out_entry) return false;
    std::lock_guard<std::mutex> g(mgr->mutex);

    if (mgr->count <= 0) return false;

    int idx = (mgr->head - mgr->count + kMaxNotifications) % kMaxNotifications;
    *out_entry = mgr->entries[idx];
    memset(&mgr->entries[idx], 0, sizeof(NotificationEntry));
    mgr->count--;

    return true;
}

extern "C" int notification_manager_pending_count(NotificationManager* mgr) {
    if (!mgr) return 0;
    std::lock_guard<std::mutex> g(mgr->mutex);
    return mgr->count;
}

extern "C" void notification_manager_clear(NotificationManager* mgr) {
    if (!mgr) return;
    std::lock_guard<std::mutex> g(mgr->mutex);
    memset(mgr->entries, 0, sizeof(mgr->entries));
    mgr->head = 0;
    mgr->count = 0;
    LOGI("All notifications cleared");
}

extern "C" void notification_manager_remove_by_ext_id(NotificationManager* mgr,
                                                       uint32_t notification_id,
                                                       const char* package_name) {
    if (!mgr || !package_name) return;
    std::lock_guard<std::mutex> g(mgr->mutex);

    for (int i = 0; i < mgr->count; ++i) {
        int idx = (mgr->head - mgr->count + i + kMaxNotifications) % kMaxNotifications;
        if (mgr->entries[idx].notification_id == notification_id &&
            strcmp(mgr->entries[idx].package_name, package_name) == 0) {
            // Shift remaining entries forward.
            for (int j = i; j < mgr->count - 1; ++j) {
                int src = (mgr->head - mgr->count + j + 1 + kMaxNotifications) % kMaxNotifications;
                int dst = (mgr->head - mgr->count + j + kMaxNotifications) % kMaxNotifications;
                mgr->entries[dst] = mgr->entries[src];
            }
            mgr->count--;
            memset(&mgr->entries[(mgr->head - 1 + kMaxNotifications) % kMaxNotifications],
                   0, sizeof(NotificationEntry));
            LOGI("Notification removed by ext (id=%u pkg=%s)", notification_id, package_name);
            return;
        }
    }
}

extern "C" bool notification_manager_has_package(NotificationManager* mgr,
                                                  const char* package_name) {
    if (!mgr || !package_name) return false;
    std::lock_guard<std::mutex> g(mgr->mutex);

    for (int i = 0; i < mgr->count; ++i) {
        int idx = (mgr->head - mgr->count + i + kMaxNotifications) % kMaxNotifications;
        if (strcmp(mgr->entries[idx].package_name, package_name) == 0) {
            return true;
        }
    }
    return false;
}

extern "C" void notification_manager_set_callback(NotificationManager* mgr,
                                                    NotificationCallback cb,
                                                    void* userdata) {
    if (!mgr) return;
    mgr->callback = cb;
    mgr->callback_userdata = userdata;
}
