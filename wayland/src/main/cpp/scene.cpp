
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

void scene_accumulate_damage(State* st, Surface* s, std::vector<Rect>& out,
                             int origin_x, int origin_y) {
    if (!s) return;
    int ox = origin_x + s->current.offset_x;
    int oy = origin_y + s->current.offset_y;
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
    int scale = std::max(1, s->current.buffer_scale);
    int rw = (s->current.dst_w > 0) ? s->current.dst_w : tbw / scale;
    int rh = (s->current.dst_h > 0) ? s->current.dst_h : tbh / scale;
    it.w = (rw > 0) ? rw : 0;
    it.h = (rh > 0) ? rh : 0;
    if (it.w > 0 && it.h > 0) items.push_back(it);
    }
    for (Surface* child : s->children) {
        if (!child) continue;
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

    std::vector<RenderItem> items;
    items.reserve(st->toplevels.size() + st->popups.size());

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
            int tbw = s->buffer_w, tbh = s->buffer_h;
            buffer_transform_dimensions(tbw, tbh, s->current.buffer_transform);
            int scale = std::max(1, s->current.buffer_scale);
            int rw = (s->current.dst_w > 0) ? s->current.dst_w : tbw / scale;
            int rh = (s->current.dst_h > 0) ? s->current.dst_h : tbh / scale;
            it.w = (rw > 0) ? rw : t->width;
            it.h = (rh > 0) ? rh : t->height;
            items.push_back(it);
        }
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
        int pp_x = p->x + s->current.offset_x;
        int pp_y = p->y + s->current.offset_y;
        if (s->current_buffer && !s->acquire_wait_failed) {
            RenderItem it{};
            it.surface = s;
            it.x = pp_x;
            it.y = pp_y;
            int tbw = s->buffer_w, tbh = s->buffer_h;
            buffer_transform_dimensions(tbw, tbh, s->current.buffer_transform);
            int scale = std::max(1, s->current.buffer_scale);
            int rw = (s->current.dst_w > 0) ? s->current.dst_w : tbw / scale;
            int rh = (s->current.dst_h > 0) ? s->current.dst_h : tbh / scale;
            it.w = (rw > 0) ? rw : p->width;
            it.h = (rh > 0) ? rh : p->height;
            items.push_back(it);
        }
        for (Surface* child : s->children) {
            if (!child) continue;
            collect_surface_items(child,
                pp_x + child->sub_x + child->current.offset_x,
                pp_y + child->sub_y + child->current.offset_y,
                items);
        }
    }

    Rect damage{};
    bool any_damage = false;
    for (const auto& t : st->toplevels) {
        if (!t->surface) continue;
        std::vector<Rect> per;
        scene_accumulate_damage(st, t->surface, per);
        for (const auto& r : per) {
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

    for (auto& plasma_it : items) if (plasma_it.surface) plasma_it.surface->buffer_sampled = true;
    const bool embedded_outputs = st->renderer->has_window_outputs() || !st->vulkan_outputs.empty();
    if (embedded_outputs) {
        st->renderer->set_cursor(0, 0, false);
        st->renderer->skip_primary_frame();
    } else {
        st->renderer->render_scene(items, any_damage ? &damage : nullptr,
                                   frame_time_ns);
    }

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
        for (auto& plasma_ci : client_items) if (plasma_ci.surface) plasma_ci.surface->buffer_sampled = true;
        auto vkOutput = st->vulkan_outputs.find(t->id);
        if (vkOutput != st->vulkan_outputs.end() && vkOutput->second) {
            if (vkOutput->second->render(client_items, frame_time_ns))
                continue;

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

    drain_buffer_releases(st);

    uint32_t time_ms = static_cast<uint32_t>(frame_time_ns / 1'000'000);
    for (auto& it : items) {
        Surface* s = it.surface;
        if (!s) continue;
        for (auto* cb : s->frame_callbacks) {
            wl_callback_send_done(cb, time_ms);
            wl_resource_destroy(cb);
        }
        s->frame_callbacks.clear();
        s->damage_surface.clear();
    }

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

    {
        std::vector<Surface*> rendered;
        rendered.reserve(items.size());
        for (const auto& it : items)
            if (it.surface) rendered.push_back(it.surface);
        presentation_dispatch(st, frame_time_ns, rendered);
    }

    if (ATrace_isEnabled()) ATrace_endSection();
}
