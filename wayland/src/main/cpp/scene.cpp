// scene.cpp — z-ordered traversal of toplevels + popups, damage union,
// hand-off to the renderer. Subsurfaces ride along with their parent.

#include "labwc.h"
#include "egl_renderer.h"
#include "vulkan_wayland_renderer.h"

#include <wayland-server.h>
#include <wayland-server-protocol.h>

#include <android/log.h>
#include <android/trace.h>

#include <algorithm>

#define LOG_TAG "Piperella/scene"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Fix 9: propagate cumulative origin offset so child subsurface damage rects
// are translated into parent (and ultimately output) coordinate space.
void scene_accumulate_damage(State* st, Surface* s, std::vector<Rect>& out,
                             int origin_x, int origin_y) {
    if (!s) return;
    // C05-12: include this surface's committed wl_surface.offset so that damage
    // rects match where the buffer actually renders (offset shifts the attach point).
    int ox = origin_x + s->current.offset_x;
    int oy = origin_y + s->current.offset_y;
    // Fix 8: damage_surface is the live list (DamageBag removed).
    for (const auto& r : s->damage_surface) {
        out.push_back({r.x + ox, r.y + oy, r.w, r.h});
    }
    for (auto* k : s->children) {
        if (!k) continue;
        scene_accumulate_damage(st, k, out,
                                ox + k->sub_x,
                                oy + k->sub_y);
    }
}

// C05-01: recursively collect a surface and its subsurfaces (children) into
// RenderItems in painter's order (parent first, then children in stored z-order;
// the default per spec is children above parent).
// abs_x/abs_y are the compositor-space position of this surface's origin,
// already including any wl_surface.offset from the call site.
static void collect_surface_items(Surface* s, int abs_x, int abs_y,
                                  std::vector<RenderItem>& items) {
    if (!s) return;
    if (s->current_buffer && !s->acquire_wait_failed) {
    RenderItem it{};
    it.surface = s;
    it.x = abs_x;
    it.y = abs_y;
    int tbw = s->buffer_w, tbh = s->buffer_h;
    buffer_transform_dimensions(tbw, tbh, s->current.buffer_transform);
    // C05-02: buffer_scale — surface logical size = buffer size / scale.
    int scale = std::max(1, s->current.buffer_scale);
    int rw = (s->current.dst_w > 0) ? s->current.dst_w : tbw / scale;
    int rh = (s->current.dst_h > 0) ? s->current.dst_h : tbh / scale;
    it.w = (rw > 0) ? rw : 0;
    it.h = (rh > 0) ? rh : 0;
    if (it.w > 0 && it.h > 0) items.push_back(it);
    }
    // Recurse into children (C05-01: subsurfaces must be rendered).
    for (Surface* child : s->children) {
        if (!child) continue;
        // Child abs position = parent abs + sub_x/y + child's own offset (C05-03).
        collect_surface_items(child,
            abs_x + child->sub_x + child->current.offset_x,
            abs_y + child->sub_y + child->current.offset_y,
            items);
    }
}

static bool surface_is_descendant(Surface* root, Surface* candidate) {
    if (!root || !candidate) return false;
    if (root == candidate) return true;
    for (Surface* child : root->children)
        if (surface_is_descendant(child, candidate)) return true;
    return false;
}

void scene_render(State* st, int64_t frame_time_ns) {
    if (ATrace_isEnabled()) ATrace_beginSection("scene_render");
    if (!st->renderer) { if (ATrace_isEnabled()) ATrace_endSection(); return; }
    // C20-09: No per-frame title logging here — title is only logged on set_title
    // and on toplevel destroy (event-driven, not per-vsync). Keep it that way.

    // Collect renderable surfaces in z-order (MW2): sort toplevels by z_order,
    // popups always render on top of every toplevel.
    std::vector<RenderItem> items;
    items.reserve(st->toplevels.size() + st->popups.size());

    // Sort toplevels by z_order for correct stacking.
    std::vector<Toplevel*> sorted_tls;
    sorted_tls.reserve(st->toplevels.size());
    for (const auto& t : st->toplevels) sorted_tls.push_back(t.get());
    std::stable_sort(sorted_tls.begin(), sorted_tls.end(),
        [](const Toplevel* a, const Toplevel* b) { return a->z_order < b->z_order; });

    for (const auto* t : sorted_tls) {
        if (!t->mapped || t->minimized || !t->surface) continue;
        Surface* s = t->surface;
        int tl_x = t->x + s->current.offset_x;
        int tl_y = t->y + s->current.offset_y;
        if (s->current_buffer && !s->acquire_wait_failed) {
            RenderItem it{};
            it.surface = s;
            it.x = tl_x;
            it.y = tl_y;
            // C03-A10: 90/270 buffer transforms swap the surface's logical w/h.
            int tbw = s->buffer_w, tbh = s->buffer_h;
            buffer_transform_dimensions(tbw, tbh, s->current.buffer_transform);
            // C05-02: buffer_scale — surface logical size = buffer size / scale.
            int scale = std::max(1, s->current.buffer_scale);
            int rw = (s->current.dst_w > 0) ? s->current.dst_w : tbw / scale;
            int rh = (s->current.dst_h > 0) ? s->current.dst_h : tbh / scale;
            it.w = (rw > 0) ? rw : t->width;
            it.h = (rh > 0) ? rh : t->height;
            items.push_back(it);
        }
        // C05-01: collect subsurfaces (children in stored z-order; default: above parent).
        for (Surface* child : s->children) {
            if (!child) continue;
            collect_surface_items(child,
                tl_x + child->sub_x + child->current.offset_x,
                tl_y + child->sub_y + child->current.offset_y,
                items);
        }
    }
    for (const auto& p : st->popups) {
        if (!p->mapped || !p->surface) continue;
        Surface* s = p->surface;
        // C05-03: apply wl_surface.offset to compositor render position.
        int pp_x = p->x + s->current.offset_x;
        int pp_y = p->y + s->current.offset_y;
        if (s->current_buffer && !s->acquire_wait_failed) {
            RenderItem it{};
            it.surface = s;
            it.x = pp_x;
            it.y = pp_y;
            // C03-A10: 90/270 buffer transforms swap the surface's logical w/h.
            int tbw = s->buffer_w, tbh = s->buffer_h;
            buffer_transform_dimensions(tbw, tbh, s->current.buffer_transform);
            // C05-02: buffer_scale — surface logical size = buffer size / scale.
            int scale = std::max(1, s->current.buffer_scale);
            int rw = (s->current.dst_w > 0) ? s->current.dst_w : tbw / scale;
            int rh = (s->current.dst_h > 0) ? s->current.dst_h : tbh / scale;
            it.w = (rw > 0) ? rw : p->width;
            it.h = (rh > 0) ? rh : p->height;
            items.push_back(it);
        }
        // C05-01: collect subsurfaces for popups too.
        for (Surface* child : s->children) {
            if (!child) continue;
            collect_surface_items(child,
                pp_x + child->sub_x + child->current.offset_x,
                pp_y + child->sub_y + child->current.offset_y,
                items);
        }
    }

    // Damage union (output coords).
    Rect damage{};
    bool any_damage = false;
    for (const auto& t : st->toplevels) {
        if (!t->surface) continue;
        std::vector<Rect> per;
        scene_accumulate_damage(st, t->surface, per);
        for (const auto& r : per) {
            // shift by toplevel position
            damage = rect_union(damage, Rect{ r.x + t->x, r.y + t->y, r.w, r.h });
            any_damage = true;
        }
    }
    for (const auto& p : st->popups) {
        if (!p->surface) continue;
        std::vector<Rect> per;
        scene_accumulate_damage(st, p->surface, per);
        for (const auto& r : per) {
            damage = rect_union(damage, Rect{ r.x + p->x, r.y + p->y, r.w, r.h });
            any_damage = true;
        }
    }

    // Cursor: prefer Wine's own cursor surface (wl_pointer.set_cursor) drawn at
    // the pointer; fall back to the compositor arrow until Wine sets one.
    Surface* cs = st->seat.ptr.cursor_surface;
    bool wine_cursor = st->show_cursor && cs && cs->texture &&
                       cs->buffer_kind != BufferKind::None &&
                       cs->buffer_w > 0 && cs->buffer_h > 0;
    if (wine_cursor) {
        RenderItem ci{};
        ci.surface = cs;
        ci.x = static_cast<int>(st->seat.ptr.x) - st->seat.ptr.cursor_hotspot_x;
        ci.y = static_cast<int>(st->seat.ptr.y) - st->seat.ptr.cursor_hotspot_y;
        ci.w = cs->buffer_w;
        ci.h = cs->buffer_h;
        items.push_back(ci);
    }
    st->renderer->set_cursor(static_cast<int>(st->seat.ptr.x),
                             static_cast<int>(st->seat.ptr.y),
                             st->show_cursor && !wine_cursor);

    // When Android has attached one SurfaceView per xdg_toplevel, the primary
    // output is only a tiny lifetime/Wayland-output anchor. Clear it instead of
    // drawing a duplicate desktop containing every Linux client. Each client
    // is rendered below into its own Android Surface/EGLSurface. If no client
    // output is attached yet, preserve the normal single-output fallback.
    for (auto& plasma_it : items) if (plasma_it.surface) plasma_it.surface->buffer_sampled = true;  // PLASMA-WLOPT-MARK
    const bool embedded_outputs = st->renderer->has_window_outputs() || !st->vulkan_outputs.empty();
    if (embedded_outputs) {
        st->renderer->set_cursor(0, 0, false);
        st->renderer->skip_primary_frame();  // no clear/swap for a surface nobody sees
    } else {
        st->renderer->render_scene(items, any_damage ? &damage : nullptr,
                                   frame_time_ns);
    }

    // PlasmaDroid window embedding: each mapped xdg_toplevel with an attached
    // Android Surface receives its own Vulkan output. Buffers are imported into
    // that output's Vulkan cache; EGL/GLES is used only after an explicit Vulkan
    // teardown when that output needs a compatibility fallback. Popups and
    // subsurfaces remain with their owning toplevel.
    for (const Toplevel* t : sorted_tls) {
        if (!t || !t->mapped || t->minimized || !t->surface) continue;
        std::vector<RenderItem> client_items;
        client_items.reserve(8);
        Surface* root = t->surface;
        const int ox = root->current.offset_x;
        const int oy = root->current.offset_y;
        if (root->current_buffer && !root->acquire_wait_failed) {
            RenderItem it{};
            it.surface = root;
            it.x = ox;
            it.y = oy;
            int bw = root->buffer_w, bh = root->buffer_h;
            buffer_transform_dimensions(bw, bh, root->current.buffer_transform);
            int scale = std::max(1, root->current.buffer_scale);
            it.w = root->current.dst_w > 0 ? root->current.dst_w : bw / scale;
            it.h = root->current.dst_h > 0 ? root->current.dst_h : bh / scale;
            if (it.w <= 0) it.w = t->width;
            if (it.h <= 0) it.h = t->height;
            client_items.push_back(it);
        }
        for (Surface* child : root->children) {
            if (!child) continue;
            collect_surface_items(child,
                ox + child->sub_x + child->current.offset_x,
                oy + child->sub_y + child->current.offset_y,
                client_items);
        }
        for (const auto& popup : st->popups) {
            if (!popup->mapped || !popup->surface ||
                !surface_is_descendant(root, popup->parent)) continue;
            const int px = popup->x - t->x + popup->surface->current.offset_x;
            const int py = popup->y - t->y + popup->surface->current.offset_y;
            if (popup->surface->current_buffer && !popup->surface->acquire_wait_failed) {
                RenderItem pi{};
                pi.surface = popup->surface;
                pi.x = px;
                pi.y = py;
                pi.w = popup->width > 0 ? popup->width : popup->surface->buffer_w;
                pi.h = popup->height > 0 ? popup->height : popup->surface->buffer_h;
                client_items.push_back(pi);
            }
            for (Surface* child : popup->surface->children) {
                if (child) collect_surface_items(child,
                    px + child->sub_x + child->current.offset_x,
                    py + child->sub_y + child->current.offset_y,
                    client_items);
            }
        }
        for (auto& plasma_ci : client_items) if (plasma_ci.surface) plasma_ci.surface->buffer_sampled = true;  // PLASMA-WLOPT-MARK2
        auto vkOutput = st->vulkan_outputs.find(t->id);
        if (vkOutput != st->vulkan_outputs.end() && vkOutput->second) {
            if (vkOutput->second->render(client_items, frame_time_ns))
                continue;

            // Vulkan render/import failed for this output. Destroy the Vulkan
            // swapchain first, then connect EGL to the retained SurfaceView.
            // This avoids competing Vulkan/EGL native-window API connections.
            vkOutput->second->teardown();
            st->vulkan_outputs.erase(vkOutput);
            auto fallback = st->vulkan_fallback_windows.find(t->id);
            if (fallback != st->vulkan_fallback_windows.end() && fallback->second) {
                const int fw = ANativeWindow_getWidth(fallback->second);
                const int fh = ANativeWindow_getHeight(fallback->second);
                if (st->renderer->attach_window_output(t->id, fallback->second, fw, fh))
                    st->egl_fallback_outputs.insert(t->id);
                ANativeWindow_release(fallback->second);
                st->vulkan_fallback_windows.erase(fallback);
            }
        }
        if (st->egl_fallback_outputs.count(t->id) != 0)
            st->renderer->render_scene_to_window_output(t->id, client_items, frame_time_ns);
    }

    // Fix 1: wl_buffer.release after swap. Per the Wayland spec, the compositor
    // must send wl_buffer.release only when it is done with the buffer and the
    // GPU is no longer sampling it. Deferred from surface_commit() → released
    // here after eglSwapBuffers returns. See wayland-book §sub-surface-buffers
    // and https://wayland.app/protocols/wayland#wl_buffer
    drain_buffer_releases(st);

    // Drain frame callbacks on every drawn surface (Wayland requires the
    // server to fire `done` on the wl_callback once the frame has been
    // submitted to the display, so the client knows it can request the
    // next one).
    uint32_t time_ms = static_cast<uint32_t>(frame_time_ns / 1'000'000);
    for (auto& it : items) {
        Surface* s = it.surface;
        if (!s) continue;
        for (auto* cb : s->frame_callbacks) {
            wl_callback_send_done(cb, time_ms);
            wl_resource_destroy(cb);
        }
        s->frame_callbacks.clear();
        // Damage consumed for this frame.
        s->damage_surface.clear();
    }

    // Fix 10: fire frame callbacks for surfaces that have pending callbacks but
    // were NOT in the rendered items list (offscreen, unmapped, subsurface-only).
    // Stalling these clients indefinitely would deadlock their render loops.
    for (auto& up : st->all_surfaces) {
        Surface* s = up.get();
        if (!s || s->frame_callbacks.empty()) continue;
        bool was_rendered = false;
        for (const auto& it : items) {
            if (it.surface == s) { was_rendered = true; break; }
        }
        if (!was_rendered) {
            for (auto* cb : s->frame_callbacks) {
                wl_callback_send_done(cb, time_ms);
                wl_resource_destroy(cb);
            }
            s->frame_callbacks.clear();
        }
    }

    // C05-07: dispatch presentation feedback from inside scene_render where
    // the rendered items list is in scope. Surfaces in 'items' get 'presented';
    // all others with pending feedback get 'discarded' (spec requires binary outcome).
    {
        std::vector<Surface*> rendered;
        rendered.reserve(items.size());
        for (const auto& it : items)
            if (it.surface) rendered.push_back(it.surface);
        presentation_dispatch(st, frame_time_ns, rendered);
    }

    if (ATrace_isEnabled()) ATrace_endSection();
}
