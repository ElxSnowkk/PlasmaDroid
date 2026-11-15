// labwc.h — Piperella Wayland compositor public + internal interface.
//
// Public C API (called from jni_bridge.cpp):
//   compositor_start              (UI thread)   -> spawns dispatch thread
//   compositor_on_window_resized  (UI thread)
//   compositor_on_window_destroyed(UI thread)   -> drops EGL, keeps wl_display
//   compositor_on_window_recreated(UI thread)   -> rebuilds EGL on new ANW
//   compositor_on_vsync           (UI thread)   -> renders one frame
//   compositor_on_refresh_changed (UI thread)   -> re-emits wl_output::mode
//   compositor_stop               (UI thread)   -> joins thread, frees state
//
// Input:
//   compositor_dispatch_motion / compositor_dispatch_key — called from
//   input-queue.cpp's ITinywlInput callback (forwarded by SurfaceServiceKt).
//
// IME:
//   compositor_ime_commit_text / compositor_ime_set_active — called from
//   TextInputBridge.kt via JNI.
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

// C20-11: CompositorHandle — opaque C ABI handle (forward-declared incomplete struct).
// JNI callers store as jlong and cast via reinterpret_cast<CompositorHandle*>(jlong).
// C++ files cast to State* internally. This provides type safety at the C ABI
// boundary without exposing State internals to JNI or Kotlin consumers.
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

// Input forwarding from input-queue.cpp (touch + key + scroll).
//
// Coordinates are in SurfaceView pixels; the seat code converts to
// surface-local wl_fixed_t values.
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

// Gamepad dispatch (from jni_bridge.cpp via SurfaceProvider.nativeDispatchGamepad).
// source: 0x00000401=SOURCE_GAMEPAD (button), 0x01000010=SOURCE_JOYSTICK (axis).
void compositor_dispatch_gamepad(struct CompositorHandle* h,
                                 int32_t source,
                                 int32_t device_id,
                                 int32_t key_code,
                                 int32_t key_action,
                                 const float* axis_values,
                                 int64_t event_time_ns);

// IME bridge.
void compositor_ime_set_active(struct CompositorHandle* h, int active);
void compositor_ime_set_preedit_string(struct CompositorHandle* h,
                                       const char* preedit_text,
                                       int32_t cursor_begin,
                                       int32_t cursor_end);
void compositor_ime_commit_text(struct CompositorHandle* h, const char* utf8);

// HDR — toggle HDR output mode. enabled=true sets BT.2020 PQ color space
// on the EGL surface and ANativeWindow; false reverts to sRGB. Returns true
// if the mode change was applied.
bool compositor_set_hdr_mode(struct CompositorHandle* h, bool enabled);

// Vsync re-arm — called from dispatch thread when new damage arrives,
// signals Choreographer to post a new frame callback via JNI.
void compositor_signal_vsync_needed();

// Window list query (MW2) — returns a JSON string with an array of objects:
//   [{"app_id":"...","title":"...","pid":1234,"x":0,"y":0,"w":800,"h":600,"focused":true}]
// The caller must free the returned string with free().
char* compositor_get_window_list_json(struct CompositorHandle* h);
bool compositor_attach_client_window(struct CompositorHandle* h, uint64_t id, struct ANativeWindow* window, int width, int height);
void compositor_detach_client_window(struct CompositorHandle* h, uint64_t id);
void compositor_resize_client_window(struct CompositorHandle* h, uint64_t id, int width, int height);
void compositor_focus_toplevel(struct CompositorHandle* h, uint64_t id);
void compositor_close_toplevel(struct CompositorHandle* h, uint64_t id);

#ifdef __cplusplus
}
#endif

// ===========================================================================
// Internal C++ data model. All fields below are private to the compositor's
// translation units; only `CompositorHandle*` crosses the C ABI boundary.
// ===========================================================================
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

// Forward-declared protocol resources — opaque to consumers.
struct PendingState {
    wl_resource* buffer = nullptr;     // attached but not committed
    bool         buffer_set = false;   // attach was called this cycle
    // wl_surface.attach() displacement (deprecated since wl_surface v5).
    int          attach_dx = 0, attach_dy = 0;
    // wl_surface.offset() displacement (wl_surface v5+).
    int          offset_x = 0, offset_y = 0;
    // Subsurface sync applied at parent commit time (E7).
    bool         sub_sync_set     = false;
    bool         sub_sync_pending = false;
    bool         offset_set = false;
    std::vector<Rect> damage_surface;
    std::vector<Rect> damage_buffer;
    std::vector<wl_resource*> frame_callbacks;
    int          buffer_scale = 1;
    int          buffer_transform = 0;
    // Viewporter (M5.3).
    bool         viewport_dirty = false;
    double       src_x = -1, src_y = -1, src_w = -1, src_h = -1; // -1 = unset
    int          dst_w = -1, dst_h = -1;
    // Double-buffered opaque/input regions (C03-11). Stored and committed; the
    // renderer/input router do not yet consume them (occlusion culling and
    // region-aware hit-testing are future work).
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
    // Plane fds owned for re-import after surfaceCreated.
    std::vector<int> fds;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> strides;
    uint32_t    flags = 0;
};

// C07-05: Deferred-import dmabuf buffer. Holds params + dup'd fds at
// wl_buffer creation time; EGL import is deferred to render time when a
// Surface* is available. Stored as wl_buffer user_data via tag+pointer.
struct DmabufBuffer {
    static const uint32_t TAG = 0x44424246u;  // 'DBBF'
    uint32_t     tag = TAG;
    int          width = 0, height = 0;
    uint32_t     fourcc = 0, flags = 0;
    uint64_t     modifier = 0;
    std::vector<int>      fds;     // dup'd fds owned by this object
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> strides;
};
// Accessor — checks tag before cast; defined in dmabuf.cpp.
DmabufBuffer* dmabuf_buffer_from_resource(wl_resource* r);

struct Surface {
    wl_resource* resource = nullptr;        // wl_surface
    wl_resource* role     = nullptr;        // xdg_surface, wl_subsurface, …
    // Destroy listener for the role object — nulls `role` when the role
    // resource (xdg_surface/wl_subsurface) is destroyed, preventing a dangling
    // read in surface_attach (C03-A4). Armed by surface_set_role().
    wl_listener  role_destroy_listener{};
    bool         role_listener_armed = false;
    PendingState pending;
    // NOTE (C03-13): `current` reuses the PendingState layout, but its `buffer`
    // member is unused — the committed buffer lives in `current_buffer` below.
    // The field is kept because PendingState is shared with `pending`.
    PendingState current;

    // Buffer + cached GL resources.
    BufferKind   buffer_kind = BufferKind::None;
    wl_resource* current_buffer = nullptr;  // wl_buffer
    int          buffer_w = 0;
    int          buffer_h = 0;
    // wl_buffer destroy listeners (C03-04): a client may destroy a wl_buffer
    // after attach but before commit (pending), or after commit but before we
    // release it (current). libwayland does not auto-clear these raw pointers,
    // so we listen and null them to avoid use-after-free.
    wl_listener  pending_buffer_destroy{};
    wl_listener  current_buffer_destroy{};
    bool         pending_buf_listener_armed = false;
    bool         current_buf_listener_armed = false;
    // Set when a new buffer is committed or fresh damage arrives; gates the
    // per-vsync SHM re-import so unchanged surfaces aren't re-uploaded (C03-12).
    bool         buffer_dirty = false;
    // PLASMA-WLOPT: true quando o buffer atual entrou em algum frame do compositor.
    bool         buffer_sampled = false;
    uint64_t     content_generation = 0; // increments on each committed buffer/damage update
    uint64_t     buffer_generation = 0;  // increments whenever wl_buffer attachment changes
    GLuint       texture = 0;               // shm path
    // Dimensions the GL texture is currently allocated at (shm path). Tracked
    // separately from buffer_w/h because the latter are now set at commit
    // (C03-10), so import_shm can't infer reallocation from buffer_w/h alone.
    int          tex_w = 0, tex_h = 0;
    int          tex_fmt = 0;              // formato wl_shm (+1) com o swizzle aplicado à textura
    GLenum       tex_target = 0x0DE1;       // GL_TEXTURE_2D / GL_TEXTURE_EXTERNAL_OES
    DmabufImport dmabuf;                    // dmabuf path

    // Explicit GPU fence sync (wp_linux_drm_syncobj_v1).
    SyncPoint acquire_point;
    SyncPoint release_point;
    // True only while the current commit's explicit acquire fence could not be waited.
    // Renderers must skip this surface until a later vsync successfully waits it.
    bool acquire_wait_failed = false;
    // C07-14: pending (double-buffered) syncobj points — written by set_acquire/release_point,
    // applied at commit time, reset each commit (per-commit semantics per spec).
    SyncPoint pending_acquire_point;
    SyncPoint pending_release_point;
    // C07-11: track whether a syncobj_surface has been created for this surface
    // (to enforce the surface_exists error).
    bool         has_syncobj_surface    = false;
    wl_resource* syncobj_surface_res   = nullptr;  // C07-14: for commit-time validation

    // Aggregated damage in surface coords since last frame.
    std::vector<Rect> damage_surface;

    // wp_viewport resource (viewporter) — non-null if surface has one.
    wl_resource* viewport = nullptr;

    // wp_fractional_scale_v1 resource — non-null if surface has one.  [C10-A4]
    wl_resource* fractional_scale_res = nullptr;

    // wp_presentation feedback objects:
    //   pending_feedbacks: requested since the last commit (before commit)
    //   feedbacks: current commit's feedback, awaiting vsync presentation
    std::vector<wl_resource*> pending_feedbacks;
    std::vector<wl_resource*> feedbacks;
    uint64_t                 frame_seq = 0;

    // wl_callback queue drained at swap time.
    std::vector<wl_resource*> frame_callbacks;

    // Subsurface tree.
    Surface*     parent = nullptr;
    int          sub_x = 0, sub_y = 0;
    bool         sub_sync = true;
    std::vector<Surface*> children;

    // Double-buffered subsurface placement (C03-06/07/08). Per spec,
    // set_position / place_above / place_below are cached and applied on the
    // PARENT's commit, regardless of this subsurface's sync/desync mode.
    bool         sub_pos_pending = false;
    int          sub_pending_x = 0, sub_pending_y = 0;
    enum class ZOp { None, Above, Below };
    ZOp          sub_z_op = ZOp::None;
    Surface*     sub_z_sibling = nullptr;   // re-validated by membership at apply

    // Pre/post-commit hook vectors (compositorv2: layered validation per Smithay).
    // Each hook is a function pointer: bool (*)(Surface*, void* cookie)
    // Pre-commit: return false to reject the commit.
    // Post-commit: called after pending→current swap.
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

    // Client owning this surface (MW1) — set at surface creation.
    wl_client* client = nullptr;

    // Tearing-control hint from wp_tearing_control_v1:
    // 0 = VSYNC (default), 1 = ASYNC.
    int tearing_hint = 0;
    // Back-pointer to the wp_tearing_control_v1 object, so a surface-destroy
    // hook can null its user_data (preventing a write through a dangling
    // Surface* in tc_set_presentation_hint).
    wl_resource* tearing_resource = nullptr;
    // Same, for wp_content_type_v1 (set_content_type writes through Surface*).
    wl_resource* content_type_res = nullptr;

    // Cached opaque/input regions are no-ops for now (M6 polish).
};

struct Toplevel {
    uint64_t     id = 0;              // stable PlasmaDroid window identity
    Surface*     surface = nullptr;
    wl_resource* xdg_surface  = nullptr;
    wl_resource* xdg_toplevel = nullptr;
    int          x = 0, y = 0;        // compositor-assigned position
    int          width = 0, height = 0;
    int          z_order = 0;
    bool         mapped = false;
    bool         wants_fullscreen = false;
    // Minimized: kept mapped protocol-wise but excluded from rendering, input
    // hit-testing, and focus. Cleared by xdg-activation or a plugin focus
    // command (there is no taskbar on the output itself).
    bool         minimized = false;
    bool         focused = false;
    std::vector<uint32_t> pending_configure_serials;
    bool         awaiting_ack = false;

    // Window/app awareness (MW1).
    pid_t        client_pid = 0;
    std::string  app_id;
    std::string  title;

    // Decoration (zxdg_decoration_manager_v1).
    wl_resource* decoration = nullptr;
    uint32_t requested_decoration_mode = 1;

    // xdg-foreign: parent surface set via zxdg_imported_v2.set_parent_of
    // (cross-process transient/child relationship, e.g. Steam CEF content
    // parented into steam.exe's container). Compared by identity only.
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

// HDR colorimetry/luminance metadata (EDID-like).
struct HdrMetadata {
    bool  hdr_capable          = false;
    // Electro-optical transfer function (EOTF) support bits.
    bool  supports_pq         = false;   // SMPTE ST 2084 (PQ)
    bool  supports_hlg        = false;   // HLG
    // Color primaries (CIE 1931 xy chromaticity coordinates, in 0.00002 units).
    uint16_t primaries_rx = 34000, primaries_ry = 16000;  // Rec.2020 default
    uint16_t primaries_gx = 13250, primaries_gy = 34500;
    uint16_t primaries_bx = 7500,  primaries_by = 3000;
    uint16_t white_point_x = 15635, white_point_y = 16450;  // D65
    // Luminance (cd/m², 0.0001 cd/m² units as per EDID).
    uint32_t max_luminance      = 8000000;   // 800 cd/m² default
    uint32_t min_luminance      = 50;        // 0.0050 cd/m²
    uint32_t max_frame_avg_luminance = 4000000;  // 400 cd/m²
    // Current active color space (driver-configurable).
    enum class ColorSpace : uint8_t {
        SdrBt709   = 0,
        HdrBt2020  = 1,  // PQ/HLG BT.2020
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
    // VRR refresh rate range (0 = not set / default).
    int  vrr_min_refresh = 0;
    int  vrr_max_refresh = 0;
    HdrMetadata hdr;
    std::vector<wl_resource*> bound;       // every bound wl_output resource
    std::vector<wl_resource*> bound_xdg;   // zxdg_output_v1 resources
};

struct PointerState {
    std::vector<wl_resource*> resources;  // wl_pointer
    Surface*               focus    = nullptr;
    wl_resource*           focused_surface = nullptr;  // for leave serial tracking
    uint32_t               enter_serial = 0;
    bool                   constrained = false;  // pointer-constraints active
    bool                   relative   = false;   // relative-pointer client
    double                 x = 0, y = 0;
    uint32_t               button_state = 0;  // tracks previous Android button bits
    // wl_pointer.set_cursor: Wine's own cursor surface + hotspot, drawn at the
    // pointer position. null → fall back to the compositor's arrow.
    Surface*               cursor_surface   = nullptr;
    int                    cursor_hotspot_x = 0;
    int                    cursor_hotspot_y = 0;
    // Pointer-constraints / relative-pointer resources.
    std::vector<wl_resource*> constraint_resources;
    std::vector<wl_resource*> relative_resources;
};

struct KeyboardState {
    std::vector<wl_resource*> resources;  // wl_keyboard
    Surface*               focus    = nullptr;
    uint32_t               enter_serial = 0;
    int                    keymap_fd = -1;
    size_t                 keymap_size = 0;
    uint32_t               mods_depressed = 0, mods_latched = 0,
                           mods_locked = 0, group = 0;
    xkb_state*             xkb_state_obj = nullptr;  // per-seat xkb state
};

struct TouchState {
    std::vector<wl_resource*> resources;  // wl_touch
    Surface*               focus    = nullptr;
    std::unordered_map<int32_t, uint32_t> active_points;  // pointerId → down_serial
};

struct TextInput {
    wl_resource* resource = nullptr;       // zwp_text_input_v3
    Surface*     focus    = nullptr;
    bool         enabled  = false;
    bool         pending_enabled = false;  // C10-01/02: double-buffer; applied on commit
    uint32_t     serial   = 0;
};

struct Seat {
    wl_global*    global = nullptr;
    PointerState  ptr;
    KeyboardState kbd;
    TouchState    touch;
    std::vector<wl_resource*> bound_seats;   // wl_seat resources
    std::vector<TextInput>    text_inputs;
};

struct DataDevice {
    wl_global* manager_global = nullptr;
    wl_resource* selection_source = nullptr;
    // C09-01: selection_text removed — data must flow through the source client
    //   via wl_data_source.send (pipe forwarding), not a local cache.
    // C09-04: source_mime_types removed — types are tracked per-source in
    //   data_device.cpp::g_source_mime_types to survive before set_selection.
};

struct GamepadDevice {
    int32_t  android_device_id = -1;
    uint32_t gamepad_id        = 0;
    char     name[64]          = {};
    bool     connected         = false;
    uint32_t last_buttons[17]  = {};       // indexed by button index 0–16
    float    last_axes[8]      = {};       // LX, LY, RX, RY, LT, RT, HAT_X, HAT_Y
};

struct GamepadManager {
    GamepadDevice devices[4];              // max 4 concurrent gamepads
    std::vector<wl_resource*> bound_resources;  // zwp_gamepad_manager_v1 resources
};

// C20-13: Thread safety summary for State fields:
// - scene_mutex guards all mutable compositor state (surfaces, toplevels, popups,
//   seat state, output state, renderer). Must be held by dispatch thread, render
//   thread, and JNI input threads before accessing any guarded State field.
// - dispatch_queue_mutex guards dispatch_queue independently of scene_mutex.
//   Never hold both scene_mutex and dispatch_queue_mutex simultaneously.
// - input_time_base_ns is GUARDED_BY(scene_mutex).
// - running and stopping are std::atomic<bool> — no lock needed for these.
// - display and event_loop are only accessed from the dispatch thread.
//
// GPU completion tracking for deferred wl_buffer.release (guarded by scene_mutex).
// Every Vulkan output owns one timeline. `submitted` is the serial of its newest
// submit; `done` is the newest serial known to have finished on the GPU. Submits
// on one timeline complete in order, so done >= N means every submit <= N is done.
struct GpuTimeline {
    uint64_t submitted = 0;
    uint64_t done = 0;
    void*    ctx = nullptr;
    // Non-blocking: returns true if the newest submit has finished.
    bool   (*probe)(void* ctx) = nullptr;
};

struct GpuTracker {
    uint64_t serial = 0;                  // global, monotonic submit serial
    std::vector<GpuTimeline*> timelines;  // live Vulkan outputs
};

// A wl_buffer queued for deferred release, with its own destroy listener so it
// is scrubbed from State::buffers_to_release if the client frees it first.
// `gate` is the submit serial current when it was queued: the buffer can only
// have been sampled by submits numbered <= gate.
struct QueuedRelease {
    wl_resource* buffer = nullptr;
    wl_listener  destroy;
    uint64_t     gate = 0;
};

// All shared state. `scene_mutex` guards every field except `display` /
// `event_loop` / `running` (which are touched only by their owning thread).
struct State {
    wl_display*    display    = nullptr;
    wl_event_loop* event_loop = nullptr;
    wl_listener    client_created_listener{};

    // Globals.
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

    // Scene graph.
    std::unordered_map<uint32_t, Surface*> surfaces_by_id; // wl_resource_get_id
    std::vector<std::unique_ptr<Surface>>  all_surfaces;
    std::vector<std::unique_ptr<Toplevel>> toplevels;
    std::vector<std::unique_ptr<Popup>>    popups;

    // Focus tracking (MW1).
    Toplevel*  focused_toplevel = nullptr;
    int        next_z_order = 0;        // monotonically increasing z-order assigner
    uint64_t   next_toplevel_id = 1;

    // ANativeWindow + EGL renderer (renderer may be null during suspend).
    ANativeWindow*               window = nullptr;
    std::unique_ptr<EglRenderer> renderer;
    // Optional Vulkan WSI output per xdg_toplevel. GLES remains the fallback.
    std::unordered_map<uint64_t, std::unique_ptr<VulkanWaylandOutput>> vulkan_outputs;
    // A retained ANativeWindow used only if a working Vulkan output must fall
    // back later. EGL and Vulkan must never connect to the same Surface at once.
    std::unordered_map<uint64_t, ANativeWindow*> vulkan_fallback_windows;
    std::unordered_set<uint64_t> egl_fallback_outputs;
    bool ahwb_pool_started = false; // one process-wide AHWB socket per compositor lifetime

    // Dmabuf feedback (built once at EGL init).
    int      dmabuf_feedback_fd  = -1;
    size_t   dmabuf_feedback_size = 0;
    uint64_t drm_device_id        = 0;     // st_rdev of EGL DRM device fd
    std::vector<std::pair<uint32_t, uint64_t>> dmabuf_formats;

    // Buffers deferred for release until after vsync swap (guarded by scene_mutex).
    // Each entry carries a wl_buffer destroy listener so a client freeing the
    // buffer before the swap can't leave a dangling pointer here (UAF in
    // scene_render → wl_buffer_send_release). See queue_buffer_release().
    std::vector<QueuedRelease*> buffers_to_release;
    GpuTracker gpu;  // Vulkan submit serials + timelines gating the releases above

    // Trackpad mode: when true the compositor draws a cursor at the pointer
    // position (seat.ptr.x/y). Set by compositor_set_trackpad_mode().
    bool show_cursor = false;

    // Dispatch thread.
    std::thread       dispatch_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex        scene_mutex;

    std::atomic<bool> damage_pending{false};
    uint64_t frame_seq{0};  // C08-07: global output vertical retrace sequence counter

    // Queued HDR mode switch: -1 = none, 0 = disable, 1 = enable. Set from the
    // client's JNI thread (compositor_set_hdr_mode), applied on the render
    // thread at the top of compositor_on_vsync where the EGL context is
    // current (recreating the EGL surface from another thread kills it).
    std::atomic<int> pending_hdr{-1};

    // UI-thread → dispatch-thread marshalling via eventfd.
    int                             dispatch_fd = -1;
    std::mutex                      dispatch_queue_mutex;
    std::deque<std::function<void()>> dispatch_queue;

    // GL texture deletion queue — textures must be deleted on the render
    // thread (where EGL context is current). Accumulate IDs in scene_mutex,
    // flush on vsync before rendering.
    std::mutex                      pending_gl_delete_mutex;
    std::vector<GLuint>             pending_gl_deletes;

    std::string socket_dir;
    std::string socket_path;
    std::string data_dir;

    // IME.
    bool ime_active = false;

    // Audio engine (AAudio output + virtual ALSA socket sink for FEX/Wine).
    struct AudioEngine* audio_engine = nullptr;

    // Notification manager (captures Android notifications via JNI bridge).
    struct NotificationManager* notification_mgr = nullptr;

    struct AdpfManager* adpf = nullptr;
    bool adpf_attempted = false;  // create ADPF once; retrying every vsync cost a session call per frame

    // VRR frame rate tracking (cached from ANativeWindow_setFrameRate).
    int32_t frame_rate_hz     = 0;
    int32_t frame_rate_compat = 0;

    // Monotonic timestamp base captured on first input event (Fix 8: prevents
    // 49-day uint32 overflow — timestamps sent to clients are relative to this).
    // C20-02: GUARDED_BY(scene_mutex) — read/written by JNI input threads and dispatch thread.
    int64_t input_time_base_ns = 0;
};

// ---------------------------------------------------------------------------
// Module entry points (defined across surface.cpp / xdg_shell.cpp / seat.cpp /
// scene.cpp / dmabuf.cpp / presentation.cpp). Each registers its globals.
// Returns false on hard failure; caller logs + aborts global registration.
// ---------------------------------------------------------------------------
// Deferred wl_buffer release queue (guarded by scene_mutex). queue_buffer_release
// adds a destroy listener so freeing the buffer first can't dangle the queue;
// drain_buffer_releases sends wl_buffer.release for each and frees the trackers.
void queue_buffer_release(State* st, wl_resource* buffer);
void drain_buffer_releases(State* st);

bool surface_globals_register(State* st);
bool xdg_shell_globals_register(State* st);
void send_toplevel_activation(State* st, Toplevel* t, bool activated);  // focus configure
void xdg_shell_ping_clients(State* st);  // send ping to all bound wm_base clients
// Re-configure output-sized/fullscreen toplevels after the output dimensions
// changed (rotation). Dispatch thread only, scene_mutex held.
void xdg_shell_output_resized(State* st, int old_w, int old_h);
void popup_dismiss_grab();              // dismiss the currently grabbed popup (if any)
bool seat_globals_register(State* st);
void seat_cleanup(State* st);  // releases xkb_state and keymap_fd

// Bridge to set_keyboard_focus (static in seat.cpp) — used by activation.cpp
// to avoid duplicating focus logic.
void compositor_set_keyboard_focus(State* st, Toplevel* tl);
void text_input_focus_changed(State* st, Surface* new_focus);
void xdg_shell_request_close(State* st, uint64_t id);
void xdg_shell_request_size(State* st, uint64_t id, int width, int height);
// Clear keyboard focus entirely (sends leave). Dispatch thread, lock held.
void compositor_clear_keyboard_focus(State* st);
bool dmabuf_globals_register(State* st);
bool presentation_globals_register(State* st);
bool data_device_globals_register(State* st);
bool viewporter_globals_register(State* st);
bool decoration_globals_register(State* st);
bool text_input_globals_register(State* st);
bool xdg_output_globals_register(State* st);
// Re-send xdg_output state to all bound resources (called on resize).
void xdg_output_update_all(State* st);
bool fractional_scale_globals_register(State* st);
// Re-send preferred_scale to all surfaces when output DPI changes.  [C10-14]
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

// UI-thread → dispatch-thread marshalling (labwc.cpp). Only takes
// dispatch_queue_mutex — safe to call while holding scene_mutex.
void post_to_dispatch(State* st, std::function<void()> fn);

// Per-frame helpers from scene.cpp / presentation.cpp.
void scene_render(State* st, int64_t frame_time_ns);
// C05-07: rendered_surfaces is the list of surfaces that were actually drawn
// this frame; non-rendered surfaces receive 'discarded' feedback per spec.
void presentation_dispatch(State* st, int64_t frame_time_ns,
                           const std::vector<Surface*>& rendered_surfaces);

// Damage helpers used by surface.cpp commit path.
// origin_x/y are cumulative compositor-space offsets (0,0 for top-level surfaces;
// sub_x/sub_y are added recursively for subsurfaces).
void scene_accumulate_damage(State* st, Surface* s, std::vector<Rect>& out,
                             int origin_x = 0, int origin_y = 0);

// Internal helper — destroy surface + remove from all_surfaces.
void surface_destroy_state(State* st, Surface* s);

// Assign a permanent role object to a surface (C03-02/A2/A4). Returns false if
// the surface already has a role (role-exclusivity); the caller posts the
// appropriate protocol error. On success, arms a destroy listener that nulls
// the role pointer when the role object is destroyed.
[[nodiscard]] bool surface_set_role(Surface* s, wl_resource* role_res);

// wl_output buffer-transform helper (C03 / M4). transform is a wl_output
// transform value (0..7); swaps w/h for the 90°/270° variants (1,3,5,7).
void buffer_transform_dimensions(int& w, int& h, int transform);

#endif  // __cplusplus
