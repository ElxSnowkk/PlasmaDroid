

#ifndef INCANDESCENT_ENABLE_H
#define INCANDESCENT_ENABLE_H

#include "../includes.h"
#include <GL/gl.h>
#include "glcorearb.h"

#ifdef __cplusplus
extern "C"
{
#endif

    enum inc_cap_index : int {
        MGC_BLEND = 0,
        MGC_COLOR_LOGIC_OP,
        MGC_CULL_FACE,
        MGC_DEBUG_OUTPUT,
        MGC_DEBUG_OUTPUT_SYNCHRONOUS,
        MGC_DEPTH_CLAMP,
        MGC_DEPTH_TEST,
        MGC_DITHER,
        MGC_FRAMEBUFFER_SRGB,
        MGC_LINE_SMOOTH,
        MGC_MULTISAMPLE,
        MGC_POLYGON_OFFSET_FILL,
        MGC_POLYGON_OFFSET_LINE,
        MGC_POLYGON_OFFSET_POINT,
        MGC_POLYGON_SMOOTH,
        MGC_PRIMITIVE_RESTART,
        MGC_PRIMITIVE_RESTART_FIXED_INDEX,
        MGC_PROGRAM_POINT_SIZE,
        MGC_RASTERIZER_DISCARD,
        MGC_SAMPLE_ALPHA_TO_COVERAGE,
        MGC_SAMPLE_ALPHA_TO_ONE,
        MGC_SAMPLE_COVERAGE,
        MGC_SAMPLE_MASK,
        MGC_SAMPLE_SHADING,
        MGC_SCISSOR_TEST,
        MGC_STENCIL_TEST,
        MGC_TEXTURE_CUBE_MAP_SEAMLESS,
        MGC_COUNT
    };

    enum { INC_MAX_CLIP_DISTANCES = 8, INC_MAX_DRAW_BUFFERS = 16, INC_MAX_VIEWPORTS = 16 };

    struct inc_enable_state_t {
        GLboolean scalar[MGC_COUNT];
        GLboolean blend_indexed[INC_MAX_DRAW_BUFFERS];
        GLboolean scissor_indexed[INC_MAX_VIEWPORTS];
        GLuint clip_distance_mask;
        GLuint primitive_restart_index;
        bool initialised;
        bool driver_synced;
    };

    struct inc_enable_state_t* inc_enable_state(void);

    GLboolean inc_enable_get(GLenum cap, GLuint index);

    bool inc_enable_query(GLenum pname, GLboolean* out);

    bool inc_enable_query_int(GLenum pname, GLint* out);

    void inc_enable_set_primitive_restart_index(GLuint index);

    bool inc_primitive_restart_enabled(void);
    GLuint inc_primitive_restart_index_for(GLenum type);

    void inc_enable_reset(struct inc_enable_state_t* state);

    void inc_enable_sync_driver(struct inc_enable_state_t* state);

#ifdef __cplusplus
}
#endif

#endif
