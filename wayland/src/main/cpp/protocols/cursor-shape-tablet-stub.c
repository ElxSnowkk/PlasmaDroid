// cursor-shape-tablet-stub.c
//
// The cursor-shape-v1 protocol declares a get_tablet_tool_v2 request, so its
// generated glue (protocols/cursor-shape-protocol.c) references the
// tablet-tool-v2 interface in its request type table:
//
//     extern const struct wl_interface zwp_tablet_tool_v2_interface;
//
// That symbol is normally provided by the tablet-unstable-v2 protocol. This
// compositor only uses the pointer cursor path and never the tablet path, so
// instead of pulling in the entire tablet protocol we provide a minimal
// definition to satisfy the linker.
//
// The symbol is never dereferenced: get_tablet_tool_v2 is never called, so the
// empty method/event tables are harmless. If real tablet support is ever added,
// drop this stub and compile the generated tablet-unstable-v2 glue instead.
#include <wayland-util.h>

const struct wl_interface zwp_tablet_tool_v2_interface = {
    "zwp_tablet_tool_v2", 1, 0, NULL, 0, NULL,
};
