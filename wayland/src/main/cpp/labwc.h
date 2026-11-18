
#pragma once

#include <android/native_window.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#ifdef __cplusplus
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "adpf.h"
#include "atrace.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

struct wl_client;
struct wl_display;
struct wl_event_loop;
struct wl_global;
struct wl_resource;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct CompositorHandle;

struct CompositorHandle* compositor_start(struct ANativeWindow* window,
                                          const char* socket_dir,
                                          const char* data_dir,
                                          int width,
                                          int height,
                                          int refresh_mhz);

void compositor_on_window_resized(struct CompositorHandle* h, int w, int height);
void compositor_on_window_destroyed(struct CompositorHandle* h);
void compositor_on_window_recreated(struct CompositorHandle* h,
                                    struct ANativeWindow* new_window);
bool compositor_on_vsync(struct CompositorHandle* h, int64_t frame_time_ns);
void compositor_on_refresh_changed(struct CompositorHandle* h, int refresh_mhz);
void compositor_stop(struct CompositorHandle* h);

void compositor_set_trackpad_mode(struct CompositorHandle* h, int on);
void compositor_dispatch_motion(struct CompositorHandle* h,
                                int32_t source, int32_t action,
                                int32_t pointer_id, float x, float y,
                                int64_t event_time_ns,
                                int32_t button_state);
void compositor_dispatch_key(struct CompositorHandle* h,
                             int32_t scan_code, int32_t action,
                             int64_t event_time_ns,
                             int32_t meta_state);
void compositor_dispatch_scroll(struct CompositorHandle* h,
                                float dx, float dy,
                                int64_t event_time_ns);

void compositor_dispatch_gamepad(struct CompositorHandle* h,
                                 int32_t source,
                                 int32_t device_id,
                                 int32_t key_code,
                                 int32_t key_action,
                                 const float* axis_values,
                                 int64_t event_time_ns);

void compositor_ime_set_active(struct CompositorHandle* h, int active);
void compositor_ime_set_preedit_string(struct CompositorHandle* h,
                                       const char* preedit_text,
                                       int32_t cursor_begin,
                                       int32_t cursor_end);
void compositor_ime_commit_text(struct CompositorHandle* h, const char* utf8);

bool compositor_set_hdr_mode(struct CompositorHandle* h, bool enabled);

void compositor_signal_vsync_needed();

char* compositor_get_window_list_json(struct CompositorHandle* h);
bool compositor_attach_client_window(struct CompositorHandle* h, uint64_t id, struct ANativeWindow* window, int width, int height);
void compositor_detach_client_window(struct CompositorHandle* h, uint64_t id);
void compositor_resize_client_window(struct CompositorHandle* h, uint64_t id, int width, int height);
void compositor_focus_toplevel(struct CompositorHandle* h, uint64_t id);
void compositor_close_toplevel(struct CompositorHandle* h, uint64_t id);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class EglRenderer;
class VulkanWaylandOutput;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

inline Rect rect_union(const Rect& a, const Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    int x1 = a.x < b.x ? a.x : b.x;
    int y1 = a.y < b.y ? a.y : b.y;
    int x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return Rect{ x1, y1, x2 - x1, y2 - y1 };
}

struct PendingState {
    wl_resource* buffer = nullptr;
    bool         buffer_set = false;
    int          attach_dx = 0, attach_dy = 0;
    int          offset_x = 0, offset_y = 0;
    bool         sub_sync_set     = false;
    bool         sub_sync_pending = false;
    bool         offset_set = false;
    std::vector<Rect> damage_surface;
    std::vector<Rect> damage_buffer;
    std::vector<wl_resource*> frame_callbacks;
    int          buffer_scale = 1;
    int          buffer_transform = 0;
    bool         viewport_dirty = false;
    double       src_x = -1, src_y = -1, src_w = -1, src_h = -1;
    int          dst_w = -1, dst_h = -1;
    std::vector<Rect> opaque_region;
    std::vector<Rect> input_region;
    bool         opaque_set = false;
    bool         input_set  = false;
};

enum class BufferKind { None, Shm, Dmabuf };

struct SyncPoint {
    uint32_t syncobj_handle = 0;
    uint64_t timeline_point = 0;
};

struct DmabufImport {
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    int         width = 0;
    int         height = 0;
    uint32_t    fourcc = 0;
    uint64_t    modifier = 0;
    std::vector<int> fds;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> strides;
    uint32_t    flags = 0;
};

struct DmabufBuffer {
    static const uint32_t TAG = 0x44424246u;
    uint32_t     tag = TAG;
    int          width = 0, height = 0;
    uint32_t     fourcc = 0, flags = 0;
    uint64_t     modifier = 0;
    std::vector<int>      fds;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> strides;
};
DmabufBuffer* dmabuf_buffer_from_resource(wl_resource* r);

struct Surface {
    wl_resource* resource = nullptr;
    wl_resource* role     = nullptr;
    wl_listener  role_destroy_listener{};
    bool         role_listener_armed = false;
    PendingState pending;
    PendingState current;

    BufferKind   buffer_kind = BufferKind::None;
    wl_resource* current_buffer = nullptr;
    int          buffer_w = 0;
    int          buffer_h = 0;
    wl_listener  pending_buffer_destroy{};
    wl_listener  current_buffer_destroy{};
    bool         pending_buf_listener_armed = false;
    bool         current_buf_listener_armed = false;
    bool         buffer_dirty = false;
    bool         buffer_sampled = false;
    uint64_t     content_generation = 0;
    uint64_t     buffer_generation = 0;
    GLuint       texture = 0;
    int          tex_w = 0, tex_h = 0;
    int          tex_fmt = 0;
    GLenum       tex_target = 0x0DE1;
    DmabufImport dmabuf;

    SyncPoint acquire_point;
    SyncPoint release_point;
    bool acquire_wait_failed = false;
    SyncPoint pending_acquire_point;
    SyncPoint pending_release_point;
    bool         has_syncobj_surface    = false;
    wl_resource* syncobj_surface_res   = nullptr;

    std::vector<Rect> damage_surface;

    wl_resource* viewport = nullptr;

    wl_resource* fractional_scale_res = nullptr;

    std::vector<wl_resource*> pending_feedbacks;
    std::vector<wl_resource*> feedbacks;
    uint64_t                 frame_seq = 0;

    std::vector<wl_resource*> frame_callbacks;

    Surface*     parent = nullptr;
    int          sub_x = 0, sub_y = 0;
    bool         sub_sync = true;
    std::vector<Surface*> children;

    bool         sub_pos_pending = false;
    int          sub_pending_x = 0, sub_pending_y = 0;
    enum class ZOp { None, Above, Below };
    ZOp          sub_z_op = ZOp::None;
    Surface*     sub_z_sibling = nullptr;

    struct CommitHook {
        bool (*fn)(Surface*, void*);
        void* cookie;
    };
    std::vector<CommitHook> pre_commit_hooks;
    std::vector<CommitHook> post_commit_hooks;
    std::vector<std::function<void(Surface*)>> destruction_hooks;

    bool run_pre_commit_hooks() {
        for (auto& h : pre_commit_hooks) if (!h.fn(this, h.cookie)) return false;
        return true;
    }
    void run_post_commit_hooks() {
        for (auto& h : post_commit_hooks) h.fn(this, h.cookie);
    }

    wl_client* client = nullptr;

    int tearing_hint = 0;
    wl_resource* tearing_resource = nullptr;
    wl_resource* content_type_res = nullptr;

};

struct Toplevel {
    uint64_t     id = 0;
    Surface*     surface = nullptr;
    wl_resource* xdg_surface  = nullptr;
    wl_resource* xdg_toplevel = nullptr;
    int          x = 0, y = 0;
    int          width = 0, height = 0;
    int          z_order = 0;
    bool         mapped = false;
    bool         wants_fullscreen = false;
    bool         minimized = false;
    bool         focused = false;
    std::vector<uint32_t> pending_configure_serials;
    bool         awaiting_ack = false;

    pid_t        client_pid = 0;
    std::string  app_id;
    std::string  title;

    wl_resource* decoration = nullptr;
    uint32_t requested_decoration_mode = 1;

    Surface* foreign_parent = nullptr;
};

struct Popup {
    Surface*     surface = nullptr;
    wl_resource* xdg_surface = nullptr;
    wl_resource* xdg_popup   = nullptr;
    Surface*     parent      = nullptr;
    int          x = 0, y = 0;
    int          width = 0, height = 0;
    bool         mapped = false;
    std::vector<uint32_t> pending_configure_serials;
};

struct HdrMetadata {
    bool  hdr_capable          = false;
    bool  supports_pq         = false;
    bool  supports_hlg        = false;
    uint16_t primaries_rx = 34000, primaries_ry = 16000;
    uint16_t primaries_gx = 13250, primaries_gy = 34500;
    uint16_t primaries_bx = 7500,  primaries_by = 3000;
    uint16_t white_point_x = 15635, white_point_y = 16450;
    uint32_t max_luminance      = 8000000;
    uint32_t min_luminance      = 50;
    uint32_t max_frame_avg_luminance = 4000000;
    enum class ColorSpace : uint8_t {
        SdrBt709   = 0,
        HdrBt2020  = 1,
        DisplayP3  = 2,
    } color_space = ColorSpace::SdrBt709;
};

struct OutputState {
    int  width_px        = 1920;
    int  height_px       = 1080;
    int  refresh_mhz     = 60000;
    int  width_mm        = 290;
    int  height_mm       = 130;
    int  scale           = 1;
    int  vrr_min_refresh = 0;
    int  vrr_max_refresh = 0;
    HdrMetadata hdr;
    std::vector<wl_resource*> bound;
    std::vector<wl_resource*> bound_xdg;
};

struct PointerState {
    std::vector<wl_resource*> resources;
    Surface*               focus    = nullptr;
    wl_resource*           focused_surface = nullptr;
    uint32_t               enter_serial = 0;
    bool                   constrained = false;
    bool                   relative   = false;
    double                 x = 0, y = 0;
    uint32_t               button_state = 0;
    Surface*               cursor_surface   = nullptr;
    int                    cursor_hotspot_x = 0;
    int                    cursor_hotspot_y = 0;
    std::vector<wl_resource*> constraint_resources;
    std::vector<wl_resource*> relative_resources;
};

struct KeyboardState {
    std::vector<wl_resource*> resources;
    Surface*               focus    = nullptr;
    uint32_t               enter_serial = 0;
    int                    keymap_fd = -1;
    size_t                 keymap_size = 0;
    uint32_t               mods_depressed = 0, mods_latched = 0,
                           mods_locked = 0, group = 0;
    xkb_state*             xkb_state_obj = nullptr;
};

struct TouchState {
    std::vector<wl_resource*> resources;
    Surface*               focus    = nullptr;
    std::unordered_map<int32_t, uint32_t> active_points;
};

struct TextInput {
    wl_resource* resource = nullptr;
    Surface*     focus    = nullptr;
    bool         enabled  = false;
    bool         pending_enabled = false;
    uint32_t     serial   = 0;
};

struct Seat {
    wl_global*    global = nullptr;
    PointerState  ptr;
    KeyboardState kbd;
    TouchState    touch;
    std::vector<wl_resource*> bound_seats;
    std::vector<TextInput>    text_inputs;
};

struct DataDevice {
    wl_global* manager_global = nullptr;
    wl_resource* selection_source = nullptr;
};

struct GamepadDevice {
    int32_t  android_device_id = -1;
    uint32_t gamepad_id        = 0;
    char     name[64]          = {};
    bool     connected         = false;
    uint32_t last_buttons[17]  = {};
    float    last_axes[8]      = {};
};

struct GamepadManager {
    GamepadDevice devices[4];
    std::vector<wl_resource*> bound_resources;
};

struct GpuTimeline {
    uint64_t submitted = 0;
    uint64_t done = 0;
    void*    ctx = nullptr;
    bool   (*probe)(void* ctx) = nullptr;
};

struct GpuTracker {
    uint64_t serial = 0;
    std::vector<GpuTimeline*> timelines;
};

struct QueuedRelease {
    wl_resource* buffer = nullptr;
    wl_listener  destroy;
    uint64_t     gate = 0;
};

struct State {
    wl_display*    display    = nullptr;
    wl_event_loop* event_loop = nullptr;
    wl_listener    client_created_listener{};

    wl_global* g_compositor    = nullptr;
    wl_global* g_subcompositor = nullptr;
    wl_global* g_shm           = nullptr;
    wl_global* g_xdg_wm_base   = nullptr;
    wl_global* g_output        = nullptr;
    wl_global* g_xdg_output_mgr= nullptr;
    wl_global* g_seat          = nullptr;
    wl_global* g_dmabuf        = nullptr;
    wl_global* g_presentation  = nullptr;
    wl_global* g_pointer_constraints = nullptr;
    wl_global* g_relative_pointer    = nullptr;
    wl_global* g_data_device         = nullptr;
    wl_global* g_viewporter          = nullptr;
    wl_global* g_decoration          = nullptr;
    wl_global* g_text_input          = nullptr;
    wl_global* g_gamepad_mgr         = nullptr;
    wl_global* g_tearing_control     = nullptr;
    wl_global* g_idle_inhibit         = nullptr;
    wl_global* g_keyboard_shortcuts_inhibit = nullptr;
    wl_global* g_cursor_shape         = nullptr;
    wl_global* g_single_pixel_buffer  = nullptr;
    wl_global* g_ext_idle_notify      = nullptr;
    wl_global* g_content_type         = nullptr;
    wl_global* g_pointer_gestures     = nullptr;
    wl_global* g_alpha_modifier       = nullptr;
    wl_global* g_xdg_system_bell      = nullptr;
    wl_global* g_xdg_dialog           = nullptr;
    wl_global* g_fractional_scale    = nullptr;
    wl_global* g_activation          = nullptr;
    wl_global* g_syncobj             = nullptr;
    wl_global* g_xdg_exporter        = nullptr;
    wl_global* g_xdg_importer        = nullptr;

    OutputState  output;
    Seat         seat;
    DataDevice   data;
    GamepadManager gamepad;

    std::unordered_map<uint32_t, Surface*> surfaces_by_id;
    std::vector<std::unique_ptr<Surface>>  all_surfaces;
    std::vector<std::unique_ptr<Toplevel>> toplevels;
    std::vector<std::unique_ptr<Popup>>    popups;

    Toplevel*  focused_toplevel = nullptr;
    int        next_z_order = 0;
    uint64_t   next_toplevel_id = 1;

    ANativeWindow*               window = nullptr;
    std::unique_ptr<EglRenderer> renderer;
    std::unordered_map<uint64_t, std::unique_ptr<VulkanWaylandOutput>> vulkan_outputs;
    std::unordered_map<uint64_t, ANativeWindow*> vulkan_fallback_windows;
    std::unordered_set<uint64_t> egl_fallback_outputs;
    bool ahwb_pool_started = false;

    int      dmabuf_feedback_fd  = -1;
    size_t   dmabuf_feedback_size = 0;
    uint64_t drm_device_id        = 0;
    std::vector<std::pair<uint32_t, uint64_t>> dmabuf_formats;

    std::vector<QueuedRelease*> buffers_to_release;
    GpuTracker gpu;

    bool show_cursor = false;

    std::thread       dispatch_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex        scene_mutex;

    std::atomic<bool> damage_pending{false};
    uint64_t frame_seq{0};

    std::atomic<int> pending_hdr{-1};

    int                             dispatch_fd = -1;
    std::mutex                      dispatch_queue_mutex;
    std::deque<std::function<void()>> dispatch_queue;

    std::mutex                      pending_gl_delete_mutex;
    std::vector<GLuint>             pending_gl_deletes;

    std::string socket_dir;
    std::string socket_path;
    std::string data_dir;

    bool ime_active = false;

    struct AudioEngine* audio_engine = nullptr;

    struct NotificationManager* notification_mgr = nullptr;

    struct AdpfManager* adpf = nullptr;
    bool adpf_attempted = false;

    int32_t frame_rate_hz     = 0;
    int32_t frame_rate_compat = 0;

    int64_t input_time_base_ns = 0;
};

void queue_buffer_release(State* st, wl_resource* buffer);
void drain_buffer_releases(State* st);

bool surface_globals_register(State* st);
bool xdg_shell_globals_register(State* st);
void send_toplevel_activation(State* st, Toplevel* t, bool activated);
void xdg_shell_ping_clients(State* st);
void xdg_shell_output_resized(State* st, int old_w, int old_h);
void popup_dismiss_grab();
bool seat_globals_register(State* st);
void seat_cleanup(State* st);

void compositor_set_keyboard_focus(State* st, Toplevel* tl);
void text_input_focus_changed(State* st, Surface* new_focus);
void xdg_shell_request_close(State* st, uint64_t id);
void xdg_shell_request_size(State* st, uint64_t id, int width, int height, bool force = false);
void compositor_clear_keyboard_focus(State* st);
bool dmabuf_globals_register(State* st);
bool presentation_globals_register(State* st);
bool data_device_globals_register(State* st);
bool viewporter_globals_register(State* st);
bool decoration_globals_register(State* st);
bool text_input_globals_register(State* st);
bool xdg_output_globals_register(State* st);
void xdg_output_update_all(State* st);
bool fractional_scale_globals_register(State* st);
void fractional_scale_update_all(State* st);
bool activation_globals_register(State* st);
bool syncobj_globals_register(State* st);
bool gamepad_globals_register(State* st);
void gamepad_globals_unregister(State* st);
bool tearing_control_globals_register(State* st);
bool idle_inhibit_globals_register(State* st);
bool keyboard_shortcuts_inhibit_globals_register(State* st);
bool cursor_shape_globals_register(State* st);
bool single_pixel_buffer_globals_register(State* st);
bool ext_idle_notify_globals_register(State* st);
bool content_type_globals_register(State* st);
bool pointer_gestures_globals_register(State* st);
bool alpha_modifier_globals_register(State* st);
bool xdg_system_bell_globals_register(State* st);
bool xdg_dialog_globals_register(State* st);
bool xdg_foreign_globals_register(State* st);

void post_to_dispatch(State* st, std::function<void()> fn);

void scene_render(State* st, int64_t frame_time_ns);
void presentation_dispatch(State* st, int64_t frame_time_ns,
                           const std::vector<Surface*>& rendered_surfaces);

void scene_accumulate_damage(State* st, Surface* s, std::vector<Rect>& out,
                             int origin_x = 0, int origin_y = 0);

void surface_destroy_state(State* st, Surface* s);

[[nodiscard]] bool surface_set_role(Surface* s, wl_resource* role_res);

void buffer_transform_dimensions(int& w, int& h, int transform);

#endif
