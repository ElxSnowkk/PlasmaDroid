
#pragma once

#include <android/native_window.h>
#include <cstdint>
#include <memory>
#include <vector>

struct RenderItem;
struct Surface;
struct GpuTracker;

class VulkanWaylandOutput {
public:
    VulkanWaylandOutput();
    ~VulkanWaylandOutput();
    VulkanWaylandOutput(const VulkanWaylandOutput&) = delete;
    VulkanWaylandOutput& operator=(const VulkanWaylandOutput&) = delete;

    bool init(ANativeWindow* window, int width, int height);
    bool render(const std::vector<RenderItem>& items, int64_t frameTimeNs);
    void resize(int width, int height);
    void forgetSurface(Surface* surface);
    void teardown();
    void setGpuTracker(GpuTracker* tracker);
    bool active() const;
    const char* deviceName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
