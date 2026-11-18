
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

struct NotificationManager;

static constexpr int kMaxNotifications = 64;

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

struct NotificationManager* notification_manager_create();

void notification_manager_destroy(struct NotificationManager* mgr);

int64_t notification_manager_push(struct NotificationManager* mgr,
                                   const struct NotificationEntry* entry);

void notification_manager_remove(struct NotificationManager* mgr,
                                  int64_t id);

void notification_manager_remove_by_ext_id(struct NotificationManager* mgr,
                                            uint32_t notification_id,
                                            const char* package_name);

bool notification_manager_pop(struct NotificationManager* mgr,
                               struct NotificationEntry* out_entry);

int notification_manager_pending_count(struct NotificationManager* mgr);

void notification_manager_clear(struct NotificationManager* mgr);

bool notification_manager_has_package(struct NotificationManager* mgr,
                                       const char* package_name);

typedef void (*NotificationCallback)(const struct NotificationEntry* entry,
                                      void* userdata);
void notification_manager_set_callback(struct NotificationManager* mgr,
                                        NotificationCallback cb,
                                        void* userdata);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

struct NotificationManager {
    std::mutex mutex;
    NotificationEntry entries[kMaxNotifications];
    int head = 0;
    int count = 0;
    int64_t next_id = 1;

    NotificationCallback callback = nullptr;
    void* callback_userdata = nullptr;
};

#endif
