/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"

/* this should be 1024 to get the expected behavior from small triangle tests */
#define FB_SIZE                  1024

#define MAX_MESH_WORKGROUPS      20000

#define MAX_VARYINGS             8
#define MAX_VARYING_SHADERS      (MAX_VARYINGS + 1)

#define NUM_ITERATIONS           100
#define NUM_PRIMITIVES_PER_DRAW  512000 /* almost the maximum that fits on the framebuffer */

/* Types of generated geometry. */
enum geometry_style {
   GEOM_TRI_LIST_REUSE2_INDEXED,   /* every triangle reuses 2 vertices from 2 previous triangles */
   GEOM_TRI_LIST_REUSE1_INDEXED,   /* every triangle reuses 1 vertex from 2 previous triangles */
   GEOM_TRI_LIST_REUSE0,           /* non-indexed primitives implicitly don't reuse any vertices */
   GEOM_TRI_STRIP,                 /* triangle strips always reuse the last 2 vertices */
   GEOM_TRI_STRIP_INDEXED,         /* same but there is an identity index buffer that doesn't serve any purpose */
   GEOM_TRI_STRIP_INDEXED_PRIM_RESTART, /* same but primitive restart is enabled and the restart index is never used */
   GEOM_MESH32_REUSE2,             /* workgroup size 32, reuse 2 vertices from 2 previous triangles */
   GEOM_MESH64_REUSE2,             /* workgroup size 64, reuse 2 vertices from 2 previous triangles */
   GEOM_MESH128_REUSE2,            /* workgroup size 128, reuse 2 vertices from 2 previous triangles */
   GEOM_MESH192_REUSE2,            /* workgroup size 192, reuse 2 vertices from 2 previous triangles */
   GEOM_MESH256_REUSE2,            /* workgroup size 256, reuse 2 vertices from 2 previous triangles */
   NUM_GEOMETRY_STYLES,
};

/* Test variations determining both generated geometry and pipeline states. */
enum cull_method {
   CULL_NONE,
   CULL_BACK,
   CULL_VIEW_XY,
   CLIPDIST1357,
   CULLDIST1357,
   CLIPDIST4,
   CULLDIST4,
   CLIPDIST8,
   CULLDIST8,
   RASTERIZER_DISCARD,
   DEGENERATE_TRIS,
   SMALL_TRIS,
   NUM_CULL_METHODS,
};

/* These flags determine special variations in generated shaders. */
enum special_attribute1 {
   SPECIAL1_NONE,
   SPECIAL1_CLIPDIST1357,
   SPECIAL1_CULLDIST1357,
   SPECIAL1_CLIPDIST4,
   SPECIAL1_CULLDIST4,
   SPECIAL1_CLIPDIST8,
   SPECIAL1_CULLDIST8,
   NUM_SPECIAL1_ATTRIBUTES,
};

enum special_attribute2 {
   SPECIAL2_NONE,
   SPECIAL2_OUTPUT_POINT_SIZE,
   SPECIAL2_OUTPUT_LAYER,
   SPECIAL2_OUTPUT_VRS1x1,
   NUM_SPECIAL2_ATTRIBUTES,
};

enum test_stage {
   INIT,
   RUN,
   REPORT,
};

static const double triangle_sizes_in_pixels[] = {
   2,       /* this means that each quad (2 triangles) occupies 2x2 pixels */
   0.1428,  /* small triangle tests use these small numbers */
   0.25,
   0.5
};

typedef struct {
   unsigned cull_percentage;
   enum geometry_style geom_style;
   enum cull_method cull_method;
   unsigned small_triangle_size_index;
   enum special_attribute2 special2;
} test_info;

static test_info tests[] = {
   /* Face and view culling tests. */
   {100, GEOM_TRI_LIST_REUSE0, CULL_BACK},
   {100, GEOM_TRI_LIST_REUSE1_INDEXED, CULL_BACK},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK},
   {100, GEOM_TRI_STRIP, CULL_BACK},
   {100, GEOM_MESH32_REUSE2, CULL_BACK},
   {100, GEOM_MESH64_REUSE2, CULL_BACK},
   {100, GEOM_MESH128_REUSE2, CULL_BACK},
   {100, GEOM_MESH192_REUSE2, CULL_BACK},
   {100, GEOM_MESH256_REUSE2, CULL_BACK},

   {75, GEOM_TRI_LIST_REUSE0, CULL_BACK},
   {75, GEOM_TRI_LIST_REUSE1_INDEXED, CULL_BACK},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK},
   {75, GEOM_TRI_STRIP, CULL_BACK},
   {75, GEOM_MESH32_REUSE2, CULL_BACK},
   {75, GEOM_MESH64_REUSE2, CULL_BACK},
   {75, GEOM_MESH128_REUSE2, CULL_BACK},
   {75, GEOM_MESH192_REUSE2, CULL_BACK},
   {75, GEOM_MESH256_REUSE2, CULL_BACK},

   {50, GEOM_TRI_LIST_REUSE0, CULL_BACK},
   {50, GEOM_TRI_LIST_REUSE1_INDEXED, CULL_BACK},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK},
   {50, GEOM_TRI_STRIP, CULL_BACK},
   {50, GEOM_MESH32_REUSE2, CULL_BACK},
   {50, GEOM_MESH64_REUSE2, CULL_BACK},
   {50, GEOM_MESH128_REUSE2, CULL_BACK},
   {50, GEOM_MESH192_REUSE2, CULL_BACK},
   {50, GEOM_MESH256_REUSE2, CULL_BACK},

   {0, GEOM_TRI_LIST_REUSE0, CULL_NONE},
   {0, GEOM_TRI_LIST_REUSE1_INDEXED, CULL_NONE},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_NONE},
   {0, GEOM_TRI_STRIP, CULL_NONE},
   {0, GEOM_MESH32_REUSE2, CULL_NONE},
   {0, GEOM_MESH64_REUSE2, CULL_NONE},
   {0, GEOM_MESH128_REUSE2, CULL_NONE},
   {0, GEOM_MESH192_REUSE2, CULL_NONE},
   {0, GEOM_MESH256_REUSE2, CULL_NONE},

   /* Additional culling options. */
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, DEGENERATE_TRIS},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, RASTERIZER_DISCARD},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_VIEW_XY},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_VIEW_XY},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_VIEW_XY},

   /* Small (subpixel) triangle tests. */
   {0, GEOM_TRI_LIST_REUSE0, SMALL_TRIS, 1},
   {0, GEOM_TRI_LIST_REUSE1_INDEXED, SMALL_TRIS, 1},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, SMALL_TRIS, 1},
   {0, GEOM_TRI_STRIP, SMALL_TRIS, 1},
   {0, GEOM_MESH128_REUSE2, SMALL_TRIS, 1},

   {0, GEOM_TRI_LIST_REUSE0, SMALL_TRIS, 2},
   {0, GEOM_TRI_LIST_REUSE1_INDEXED, SMALL_TRIS, 2},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, SMALL_TRIS, 2},
   {0, GEOM_TRI_STRIP, SMALL_TRIS, 2},
   {0, GEOM_MESH128_REUSE2, SMALL_TRIS, 2},

   {0, GEOM_TRI_LIST_REUSE0, SMALL_TRIS, 3},
   {0, GEOM_TRI_LIST_REUSE1_INDEXED, SMALL_TRIS, 3},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, SMALL_TRIS, 3},
   {0, GEOM_TRI_STRIP, SMALL_TRIS, 3},
   {0, GEOM_MESH128_REUSE2, SMALL_TRIS, 3},

   /* ClipDistance and CullDistance culling tests. */
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST1357},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST1357},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8},
   {100, GEOM_MESH128_REUSE2, CLIPDIST4},
   {100, GEOM_MESH128_REUSE2, CULLDIST4},
   {100, GEOM_MESH128_REUSE2, CLIPDIST1357},
   {100, GEOM_MESH128_REUSE2, CULLDIST1357},
   {100, GEOM_MESH128_REUSE2, CLIPDIST8},
   {100, GEOM_MESH128_REUSE2, CULLDIST8},

   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST1357},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST1357},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8},
   {75, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8},
   {75, GEOM_MESH128_REUSE2, CLIPDIST4},
   {75, GEOM_MESH128_REUSE2, CULLDIST4},
   {75, GEOM_MESH128_REUSE2, CLIPDIST1357},
   {75, GEOM_MESH128_REUSE2, CULLDIST1357},
   {75, GEOM_MESH128_REUSE2, CLIPDIST8},
   {75, GEOM_MESH128_REUSE2, CULLDIST8},

   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST1357},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST1357},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8},
   {50, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8},
   {50, GEOM_MESH128_REUSE2, CLIPDIST4},
   {50, GEOM_MESH128_REUSE2, CULLDIST4},
   {50, GEOM_MESH128_REUSE2, CLIPDIST1357},
   {50, GEOM_MESH128_REUSE2, CULLDIST1357},
   {50, GEOM_MESH128_REUSE2, CLIPDIST8},
   {50, GEOM_MESH128_REUSE2, CULLDIST8},

   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST1357},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST1357},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8},
   {0, GEOM_MESH128_REUSE2, CLIPDIST4},
   {0, GEOM_MESH128_REUSE2, CULLDIST4},
   {0, GEOM_MESH128_REUSE2, CLIPDIST1357},
   {0, GEOM_MESH128_REUSE2, CULLDIST1357},
   {0, GEOM_MESH128_REUSE2, CLIPDIST8},
   {0, GEOM_MESH128_REUSE2, CULLDIST8},

   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK, 0, SPECIAL2_OUTPUT_VRS1x1},
   {100, GEOM_MESH128_REUSE2, CULL_BACK, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_MESH128_REUSE2, CULL_BACK, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_MESH128_REUSE2, CULL_BACK, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_MESH128_REUSE2, CULL_BACK, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_MESH128_REUSE2, CULL_BACK, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_MESH128_REUSE2, CULL_BACK, 0, SPECIAL2_OUTPUT_VRS1x1},

   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},
   {100, GEOM_MESH128_REUSE2, CLIPDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_MESH128_REUSE2, CLIPDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_MESH128_REUSE2, CLIPDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_MESH128_REUSE2, CLIPDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_MESH128_REUSE2, CLIPDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_MESH128_REUSE2, CLIPDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},

   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},
   {100, GEOM_MESH128_REUSE2, CULLDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_MESH128_REUSE2, CULLDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_MESH128_REUSE2, CULLDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_MESH128_REUSE2, CULLDIST4, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_MESH128_REUSE2, CULLDIST4, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_MESH128_REUSE2, CULLDIST4, 0, SPECIAL2_OUTPUT_VRS1x1},

   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
   {100, GEOM_MESH128_REUSE2, CLIPDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_MESH128_REUSE2, CLIPDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_MESH128_REUSE2, CLIPDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_MESH128_REUSE2, CLIPDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_MESH128_REUSE2, CLIPDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_MESH128_REUSE2, CLIPDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},

   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
   {100, GEOM_MESH128_REUSE2, CULLDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {100, GEOM_MESH128_REUSE2, CULLDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {100, GEOM_MESH128_REUSE2, CULLDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_TRI_LIST_REUSE2_INDEXED, CULLDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
   {0, GEOM_MESH128_REUSE2, CULLDIST8, 0, SPECIAL2_OUTPUT_POINT_SIZE},
   {0, GEOM_MESH128_REUSE2, CULLDIST8, 0, SPECIAL2_OUTPUT_LAYER},
   {0, GEOM_MESH128_REUSE2, CULLDIST8, 0, SPECIAL2_OUTPUT_VRS1x1},
};

/* These flags determine variations in generated vertex buffers caused by culling options. */
enum geom_cull_type {
   GEOM_CULL_TYPE_NONE,
   GEOM_CULL_TYPE_BACK,
   GEOM_CULL_TYPE_VIEW_XY,
   GEOM_CULL_TYPE_DEGENERATE,
   GEOM_CULL_TYPE_CLIP_CULL_DIST4,
   GEOM_CULL_TYPE_CLIP_CULL_DIST8,
};

/* This is used to deduplicate vertex and index buffer sets. */
typedef union {
   struct {
      uint16_t geom_style_reduced:4;
      uint16_t cull_type:3;
      uint16_t cull_percentage_div25:3;
      uint16_t small_triangle_size_index:2;
      uint16_t pad:4;
   };
   uint16_t index;
} buffer_set_index;

typedef struct unique_buffer_set {
   api_buffer *vb;
   unsigned vb_pos_offset;
   unsigned vb_special0_offset;
   unsigned vb_special1_offset;
   api_buffer *ib;
   api_buffer *mesh_group_buf;
   unsigned num_vertices;
   unsigned num_indices;
   unsigned num_mesh_groups;
   api_descriptor_set *mesh_desc_set[MAX_VARYING_SHADERS];
} unique_buffer_set;

typedef struct {
   const char *test_suite_name;

   api_shader *ms[NUM_GEOMETRY_STYLES][NUM_SPECIAL1_ATTRIBUTES][NUM_SPECIAL2_ATTRIBUTES][MAX_VARYING_SHADERS];
   api_shader *vs[NUM_SPECIAL1_ATTRIBUTES][NUM_SPECIAL2_ATTRIBUTES][MAX_VARYING_SHADERS];
   api_shader *fs[MAX_VARYING_SHADERS];

   unique_buffer_set buffers[1 << 12]; /* indexed by buffer_set_index */

   api_framebuffer *fb;
   api_descriptor_set_layout *ms_desc_set_layout[MAX_VARYING_SHADERS];
   api_pipeline *pipelines[NUM_GEOMETRY_STYLES][NUM_CULL_METHODS][MAX_VARYING_SHADERS];
   api_timestamp_query_pool *timestamps;
} test_state;

static api_shader *
compile_fs(api_context *ctx, unsigned num_varyings)
{
   char fs_source[1024];
   snprintf(fs_source, sizeof(fs_source),
            "#version 460 \n"
            "#define N %u \n"

            "layout(location = 0) out vec4 fs_out; \n"
            "#if N > 0 \n"
            "layout(location = 0) in vec4 v[N]; \n"
            "#endif \n"

            "void main() { \n"
            "#if N > 0 \n"
            "  vec4 mul = vec4(1); \n"
            "  for (int i = 0; i < N; i++) mul *= v[i]; \n"
            "  fs_out = vec4(vec3(mul.x * mul.y * mul.z * mul.w == 1 ? 1 : 0), 1); \n"
            "#else \n"
            "    fs_out = vec4(1); \n"
            /* For coloring individual triangles to be able to tell them apart. */
            /*"  fs_out = vec4(vec3(ivec3(gl_PrimitiveID & 0x3, (gl_PrimitiveID >> 2) & 0x3, (gl_PrimitiveID >> 4) & 0x3)) / 3, 1); \n"*/
            "#endif \n"
            "}", num_varyings);

   return ctx->create_shader(ctx, fs_source, api_shader_fs);
}

static api_shader *
compile_vs(api_context *ctx, unsigned num_varyings, enum special_attribute1 special1,
           enum special_attribute2 special2)
{
   const char *special1_code_vs_vert = "", *special2_code_vs_vert = "";

   switch (special1) {
   case SPECIAL1_NONE:
      break;
   case SPECIAL1_CLIPDIST1357:
      special1_code_vs_vert = "gl_ClipDistance[0] = 1; \n"
                              "gl_ClipDistance[1] = special0.x; \n"
                              "gl_ClipDistance[2] = 1; \n"
                              "gl_ClipDistance[3] = special0.y; \n"
                              "gl_ClipDistance[4] = 1; \n"
                              "gl_ClipDistance[5] = special0.z; \n"
                              "gl_ClipDistance[6] = 1; \n"
                              "gl_ClipDistance[7] = special0.w; \n";
      break;
   case SPECIAL1_CULLDIST1357:
      special1_code_vs_vert = "gl_CullDistance[0] = 1; \n"
                              "gl_CullDistance[1] = special0.x; \n"
                              "gl_CullDistance[2] = 1; \n"
                              "gl_CullDistance[3] = special0.y; \n"
                              "gl_CullDistance[4] = 1; \n"
                              "gl_CullDistance[5] = special0.z; \n"
                              "gl_CullDistance[6] = 1; \n"
                              "gl_CullDistance[7] = special0.w; \n";
      break;
   case SPECIAL1_CLIPDIST4:
      special1_code_vs_vert = "gl_ClipDistance[0] = special0.x; \n"
                              "gl_ClipDistance[1] = special0.y; \n"
                              "gl_ClipDistance[2] = special0.z; \n"
                              "gl_ClipDistance[3] = special0.w; \n";
      break;
   case SPECIAL1_CULLDIST4:
      special1_code_vs_vert = "gl_CullDistance[0] = special0.x; \n"
                              "gl_CullDistance[1] = special0.y; \n"
                              "gl_CullDistance[2] = special0.z; \n"
                              "gl_CullDistance[3] = special0.w; \n";
      break;
   case SPECIAL1_CLIPDIST8:
      special1_code_vs_vert = "gl_ClipDistance[0] = special0.x; \n"
                              "gl_ClipDistance[1] = special0.y; \n"
                              "gl_ClipDistance[2] = special0.z; \n"
                              "gl_ClipDistance[3] = special0.w; \n"
                              "gl_ClipDistance[4] = special1.x; \n"
                              "gl_ClipDistance[5] = special1.y; \n"
                              "gl_ClipDistance[6] = special1.z; \n"
                              "gl_ClipDistance[7] = special1.w; \n";
      break;
   case SPECIAL1_CULLDIST8:
      special1_code_vs_vert = "gl_CullDistance[0] = special0.x; \n"
                              "gl_CullDistance[1] = special0.y; \n"
                              "gl_CullDistance[2] = special0.z; \n"
                              "gl_CullDistance[3] = special0.w; \n"
                              "gl_CullDistance[4] = special1.x; \n"
                              "gl_CullDistance[5] = special1.y; \n"
                              "gl_CullDistance[6] = special1.z; \n"
                              "gl_CullDistance[7] = special1.w; \n";
      break;
   default:
      error("vs: invalid special1 attribute enum");
   }

   switch (special2) {
   case SPECIAL2_NONE:
      break;
   case SPECIAL2_OUTPUT_POINT_SIZE:
      /* It's always 1, but we don't want the driver think that we are setting 1. */
      special2_code_vs_vert = "gl_PointSize = gl_InstanceID > 0 ? 0 : 1; \n";
      break;
   case SPECIAL2_OUTPUT_LAYER:
      /* It's always 0, but we don't want the driver think that we are setting 0. */
      special2_code_vs_vert = "gl_Layer = gl_InstanceID; \n";
      break;
   case SPECIAL2_OUTPUT_VRS1x1:
      if (!ctx->has_vrs)
         return NULL;
      /* It's always 1x1, but we don't want the driver think that we are setting 1x1. */
      special2_code_vs_vert = "gl_PrimitiveShadingRateEXT = gl_InstanceID > 0 ? gl_ShadingRateFlag2HorizontalPixelsEXT : 0; \n";
      break;
   default:
      error("vs: invalid special2 attribute enum");
   }

   char vs_source[1024];
   snprintf(vs_source, sizeof(vs_source),
            "#version 460 \n"
            "#extension GL_EXT_fragment_shading_rate : enable \n"

            "#ifdef VULKAN \n"
            "#define gl_InstanceID gl_InstanceIndex \n"
            "#endif \n"

            "#extension GL_ARB_shader_viewport_layer_array : enable \n"

            "#define N %u \n"

            "layout(location = 0) in vec4 pos; \n"
            "layout(location = 1) in vec4 special0; \n"
            "layout(location = 2) in vec4 special1; \n"
            "#if N > 0 \n"
            "   layout(location = 3) in vec4 a[N]; \n"
            "   layout(location = 0) out vec4 v[N]; \n"
            "#endif \n"

            "void main() { \n"
            "#if N > 0 \n"
            "  for (int i = 0; i < N; i++) v[i] = a[i]; \n"
            "#endif \n"
            "  gl_Position = pos; \n"
            "%s"
            "%s"
            "}", num_varyings, special1_code_vs_vert, special2_code_vs_vert);

   return ctx->create_shader(ctx, vs_source, api_shader_vs);
}

static unsigned
get_mesh_wg_size(enum geometry_style geom_style)
{
   return geom_style == GEOM_MESH32_REUSE2 ? 32 :
          geom_style == GEOM_MESH64_REUSE2 ? 64 :
          geom_style == GEOM_MESH128_REUSE2 ? 128 :
          geom_style == GEOM_MESH192_REUSE2 ? 192 :
          geom_style == GEOM_MESH256_REUSE2 ? 256 : 0;
}

static api_shader *
compile_ms(api_context *ctx, unsigned num_varyings, enum geometry_style geom_style,
           enum special_attribute1 special1, enum special_attribute2 special2)
{
   const char *special1_code_ms_vert = "", *special1_code_ms_prim = "";
   const char *special2_code_ms_vert = "", *special2_code_ms_prim = "";

   unsigned wg_size = get_mesh_wg_size(geom_style);
   if (!wg_size)
      return NULL;

   switch (special1) {
   case SPECIAL1_NONE:
      break;
   case SPECIAL1_CLIPDIST1357:
      special1_code_ms_vert = "vec4 loaded_special0 = texelFetch(special0, int(vertex_id)); \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[0] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[1] = loaded_special0.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[2] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[3] = loaded_special0.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[4] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[5] = loaded_special0.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[6] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[7] = loaded_special0.w; \n";
      break;
   case SPECIAL1_CULLDIST1357:
      special1_code_ms_vert = "vec4 loaded_special0 = texelFetch(special0, int(vertex_id)); \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[0] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[1] = loaded_special0.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[2] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[3] = loaded_special0.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[4] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[5] = loaded_special0.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[6] = 1; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[7] = loaded_special0.w; \n";
      break;
   case SPECIAL1_CLIPDIST4:
      special1_code_ms_vert = "vec4 loaded_special0 = texelFetch(special0, int(vertex_id)); \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[0] = loaded_special0.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[1] = loaded_special0.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[2] = loaded_special0.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[3] = loaded_special0.w; \n";
      break;
   case SPECIAL1_CULLDIST4:
      special1_code_ms_vert = "vec4 loaded_special0 = texelFetch(special0, int(vertex_id)); \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[0] = loaded_special0.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[1] = loaded_special0.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[2] = loaded_special0.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[3] = loaded_special0.w; \n";
      break;
   case SPECIAL1_CLIPDIST8:
      special1_code_ms_vert = "vec4 loaded_special0 = texelFetch(special0, int(vertex_id)); \n"
                              "vec4 loaded_special1 = texelFetch(special1, int(vertex_id)); \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[0] = loaded_special0.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[1] = loaded_special0.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[2] = loaded_special0.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[3] = loaded_special0.w; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[4] = loaded_special1.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[5] = loaded_special1.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[6] = loaded_special1.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_ClipDistance[7] = loaded_special1.w; \n";
      break;
   case SPECIAL1_CULLDIST8:
      special1_code_ms_vert = "vec4 loaded_special0 = texelFetch(special0, int(vertex_id)); \n"
                              "vec4 loaded_special1 = texelFetch(special1, int(vertex_id)); \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[0] = loaded_special0.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[1] = loaded_special0.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[2] = loaded_special0.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[3] = loaded_special0.w; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[4] = loaded_special1.x; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[5] = loaded_special1.y; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[6] = loaded_special1.z; \n"
                              "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_CullDistance[7] = loaded_special1.w; \n";
      break;
   default:
      error("ms: invalid special1 attribute enum");
   }

   switch (special2) {
   case SPECIAL2_NONE:
      break;
   case SPECIAL2_OUTPUT_POINT_SIZE:
      /* It's always 1, but we don't want the driver think that we are setting 1. */
      special2_code_ms_vert = "gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_PointSize = zeros.x > 0 ? 0 : 1; \n";
      break;
   case SPECIAL2_OUTPUT_LAYER:
      /* It's always 0, but we don't want the driver think that we are setting 0. */
      special2_code_ms_prim = "gl_MeshPrimitivesEXT[gl_LocalInvocationID.x].gl_Layer = zeros.x; \n";
      break;
   case SPECIAL2_OUTPUT_VRS1x1:
      if (!ctx->has_vrs)
         return NULL;
      /* It's always 1x1, but we don't want the driver think that we are setting 1x1. */
      special2_code_ms_prim = "gl_MeshPrimitivesEXT[gl_LocalInvocationID.x].gl_PrimitiveShadingRateEXT = "
                              "zeros.x > 0 ? gl_ShadingRateFlag2HorizontalPixelsEXT : 0; \n";
      break;
   default:
      error("ms: invalid special2 attribute enum");
   }

   char ms_source[4096];
   snprintf(ms_source, sizeof(ms_source),
            "#version 460 \n"
            "#extension GL_EXT_mesh_shader : enable \n"
            "#extension GL_EXT_fragment_shading_rate : enable \n"
            "#define N %u \n"

            "layout(local_size_x = %u, local_size_y = 1, local_size_z = 1) in; \n"
            "layout(max_vertices = %u, max_primitives = %u, triangles) out; \n"

            "layout(binding = 0) uniform GroupInfo { \n"
            "  ivec4 zeros; \n"
            "  uvec4 group_info[%u]; \n"
            "}; \n"

            "#ifdef VULKAN \n"
            "   layout(binding = 1) uniform usamplerBuffer indices; \n"
            "   layout(binding = 2) uniform samplerBuffer pos; \n"
            "   layout(binding = 3) uniform samplerBuffer special0; \n"
            "   layout(binding = 4) uniform samplerBuffer special1; \n"
            "   #if N > 0 \n"
            "      layout(binding = 5) uniform samplerBuffer a[N]; \n"
            "   #endif \n"
            "#else \n"
            "   layout(binding = 0) uniform usamplerBuffer indices; \n"
            "   layout(binding = 1) uniform samplerBuffer pos; \n"
            "   layout(binding = 2) uniform samplerBuffer special0; \n"
            "   layout(binding = 3) uniform samplerBuffer special1; \n"
            "   #if N > 0 \n"
            "      layout(binding = 4) uniform samplerBuffer a[N]; \n"
            "   #endif \n"
            "#endif \n"

            "#if N > 0 \n"
            "   layout(location = 0) out vec4 v[][N]; \n"
            "#endif \n"

            "void main() { \n"
            "  uvec4 info = group_info[gl_WorkGroupID.x]; \n"
            "  uint base_vertex = info.x; \n"
            "  uint base_prim = info.y; \n"
            "  uint num_vertices = info.z; \n"
            "  uint num_primitives = info.w; \n"

            "  if (gl_LocalInvocationID.x == 0) \n"
            "    SetMeshOutputsEXT(num_vertices, num_primitives); \n"

            "  uint vertex_id = base_vertex + gl_LocalInvocationID.x; \n"
            "  uint prim_id = base_prim + gl_LocalInvocationID.x; \n"

            "  if (gl_LocalInvocationID.x < num_vertices) { \n"
            "    gl_MeshVerticesEXT[gl_LocalInvocationID.x].gl_Position = texelFetch(pos, int(vertex_id)); \n"
            "#if N > 0 \n"
            "    for (int i = 0; i < N; i++) v[gl_LocalInvocationID.x][i] = texelFetch(a[i], int(vertex_id) * zeros.x); \n"
            "#endif \n"
            "    %s"
            "    %s"
            "  } \n"

            "  if (gl_LocalInvocationID.x < num_primitives) { \n"
            "    gl_PrimitiveTriangleIndicesEXT[gl_LocalInvocationID.x] = texelFetch(indices, int(prim_id)).xyz;\n"
            "    %s"
            "    %s"
            "  } \n"

            "}", num_varyings, wg_size, wg_size, wg_size - 2, MAX_MESH_WORKGROUPS,
            special1_code_ms_vert, special2_code_ms_vert,
            special1_code_ms_prim, special2_code_ms_prim);

   return ctx->create_shader(ctx, ms_source, api_shader_ms);
}

static void
compile_shaders(api_context *ctx, test_state *state)
{
   for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
      if (v == 5 || v == 7)
         continue;

      state->fs[v] = compile_fs(ctx, v);

      for (enum special_attribute1 s1 = 0; s1 < NUM_SPECIAL1_ATTRIBUTES; s1++) {
         for (enum special_attribute2 s2 = 0; s2 < NUM_SPECIAL2_ATTRIBUTES; s2++) {
            state->vs[s1][s2][v] = compile_vs(ctx, v, s1, s2);

            if (ctx->has_mesh_shader) {
               for (enum geometry_style g = 0; g < NUM_GEOMETRY_STYLES; g++)
                  state->ms[g][s1][s2][v] = compile_ms(ctx, v, g, s1, s2);
            }
         }
      }
   }
}

static void
set_special_attribs(enum geom_cull_type cull_type, bool cull, unsigned base_index,
                    unsigned num_vertices, float *special0, float *special1)
{
   switch (cull_type) {
   case GEOM_CULL_TYPE_CLIP_CULL_DIST4:
      for (unsigned i = 0; i < num_vertices; i++) {
         for (unsigned c = 0; c < 4; c++)
            special0[((base_index + i) * 4) + c] = cull ? -1 : 1;
      }
      break;
   case GEOM_CULL_TYPE_CLIP_CULL_DIST8:
      for (unsigned i = 0; i < num_vertices; i++) {
         for (unsigned c = 0; c < 4; c++) {
            special0[((base_index + i) * 4) + c] = cull ? -1 : 1;
            special1[((base_index + i) * 4) + c] = cull ? -1 : 1;
         }
      }
      break;
   default:;
   }
}

static void
gen_triangle_tile(unsigned num_quads_per_dim, double prim_size_in_pixels, unsigned cull_percentage,
                  unsigned num_vertices_per_prim, enum geom_cull_type cull_type,
                  unsigned mesh_wg_size, unsigned max_vertices, unsigned *num_vertices,
                  float *vertices, float *special0, float *special1,
                  unsigned max_indices, unsigned *num_indices, uint32_t *indices,
                  unsigned max_groups, unsigned *num_groups, uint32_t *groups)
{
   /* clip space coordinates in both X and Y directions: */
   const double first = -1;
   const double max_length = 2;
   const double d = prim_size_in_pixels * 2.0 / FB_SIZE;

   unsigned num_vertices_per_group = 0;
   unsigned num_prims_per_group = 0;

   assert(d * num_quads_per_dim <= max_length);
   assert(*num_vertices == 0);

   /* the vertex ordering is counter-clockwise */
   for (unsigned ty = 0; ty < num_quads_per_dim; ty++) {
      bool cull;

      if (cull_percentage == 0)
         cull = false;
      else if (cull_percentage == 25)
         cull = ty % 4 == 0;
      else if (cull_percentage == 50)
         cull = ty % 2 == 1;
      else if (cull_percentage == 75)
         cull = ty % 4 != 0;
      else if (cull_percentage == 100)
         cull = true;
      else
         assert(!"wrong cull_percentage");

      for (unsigned tx = 0; tx < num_quads_per_dim; tx++) {
         unsigned x = tx;
         unsigned y = ty;

         /* view culling in different directions */
         double xoffset = 0, yoffset = 0, zoffset = 0;

         if (cull && cull_type == GEOM_CULL_TYPE_VIEW_XY) {
            unsigned side = (ty / 2) % 4;

            if (side == 0)		xoffset = -2;
            else if (side == 1)	xoffset =  2;
            else if (side == 2)	yoffset = -2;
            else if (side == 3)	yoffset =  2;
         }

         if (indices) {
            unsigned elem = *num_vertices * 3;

            /* generate horizontal stripes with maximum reuse */
            if (x == 0) {
               num_vertices_per_group += 2;
               *num_vertices += 2;
               assert(*num_vertices <= max_vertices);

               set_special_attribs(cull_type, cull, elem / 3, 2, special0, special1);

               vertices[elem++] = xoffset + first + d * x;
               vertices[elem++] = yoffset + first + d * y;
               vertices[elem++] = zoffset;

               vertices[elem++] = xoffset + first + d * x;
               vertices[elem++] = yoffset + first + d * (y + 1);
               vertices[elem++] = zoffset;
            }

            int base_index = *num_vertices;

            *num_vertices += num_vertices_per_prim == 2 ? 4 : 2;
            assert(*num_vertices <= max_vertices);

            set_special_attribs(cull_type, cull, elem / 3, num_vertices_per_prim == 2 ? 4 : 2,
                                special0, special1);

            if (num_vertices_per_prim == 2) {
               vertices[elem++] = xoffset + first + d * x;
               vertices[elem++] = yoffset + first + d * (y + 1);
               vertices[elem++] = zoffset;
            }

            vertices[elem++] = xoffset + first + d * (x + 1);
            vertices[elem++] = yoffset + first + d * y;
            vertices[elem++] = zoffset;

            if (num_vertices_per_prim == 2) {
               vertices[elem++] = xoffset + first + d * (x + 1);
               vertices[elem++] = yoffset + first + d * y;
               vertices[elem++] = zoffset;
            }

            vertices[elem++] = xoffset + first + d * (x + 1);
            vertices[elem++] = yoffset + first + d * (y + 1);
            vertices[elem++] = zoffset;

            /* generate indices */
            unsigned idx = *num_indices;
            *num_indices += 6;
            assert(*num_indices <= max_indices);

            if (num_vertices_per_prim == 2) {
               indices[idx++] = base_index - 2;
               indices[idx++] = base_index + 1;
               indices[idx++] = base_index;

               indices[idx++] = base_index - 1;
               indices[idx++] = base_index + 2;
               indices[idx++] = base_index + 3;
            } else {
               indices[idx++] = base_index - 2;
               indices[idx++] = base_index;
               indices[idx++] = base_index - 1;

               indices[idx++] = base_index - 1;
               indices[idx++] = base_index;
               indices[idx++] = base_index + 1;
            }

            if (cull && cull_type == GEOM_CULL_TYPE_BACK) {
               /* switch the winding order */
               unsigned tmp = indices[idx - 6];
               indices[idx - 6] = indices[idx - 5];
               indices[idx - 5] = tmp;

               tmp = indices[idx - 3];
               indices[idx - 3] = indices[idx - 2];
               indices[idx - 2] = tmp;
            }

            if (cull && cull_type == GEOM_CULL_TYPE_DEGENERATE) {
               indices[idx - 5] = indices[idx - 4];
               indices[idx - 2] = indices[idx - 1];
            }

            if (mesh_wg_size) {
               num_vertices_per_group += 2;
               num_prims_per_group += 2;

               unsigned base_vertex = *num_vertices - num_vertices_per_group;

               for (unsigned i = idx - 6; i < idx; i++)
                  indices[i] -= base_vertex;

               /* Break the group if the group is full or we are at the end of the strip. */
               if (num_prims_per_group == mesh_wg_size - 2 || tx == num_quads_per_dim - 1) {
                  unsigned group_id = (*num_groups);
                  assert(group_id < max_groups);

                  unsigned base_prim = *num_indices / 3 - num_prims_per_group;

                  groups[group_id * 4 + 0] = base_vertex;
                  groups[group_id * 4 + 1] = base_prim;
                  groups[group_id * 4 + 2] = num_vertices_per_group;
                  groups[group_id * 4 + 3] = num_prims_per_group;

                  (*num_groups)++;
                  num_vertices_per_group = 0;
                  num_prims_per_group = 0;

                  /* If we are not at the end of the strip, reuse the last 2 vertices
                   * from the previous group as the start of the new group to continue
                   * the strip.
                   */
                  if (tx < num_quads_per_dim - 1)
                     num_vertices_per_group += 2;
               }
            }
         } else {
            assert(!mesh_wg_size);
            unsigned elem = *num_vertices * 3;
            *num_vertices += 6;
            assert(*num_vertices <= max_vertices);

            set_special_attribs(cull_type, cull, elem / 3, 6, special0, special1);

            vertices[elem++] = xoffset + first + d * x;
            vertices[elem++] = yoffset + first + d * y;
            vertices[elem++] = zoffset;

            vertices[elem++] = xoffset + first + d * (x + 1);
            vertices[elem++] = yoffset + first + d * y;
            vertices[elem++] = zoffset;

            vertices[elem++] = xoffset + first + d * x;
            vertices[elem++] = yoffset + first + d * (y + 1);
            vertices[elem++] = zoffset;

            vertices[elem++] = xoffset + first + d * x;
            vertices[elem++] = yoffset + first + d * (y + 1);
            vertices[elem++] = zoffset;

            vertices[elem++] = xoffset + first + d * (x + 1);
            vertices[elem++] = yoffset + first + d * y;
            vertices[elem++] = zoffset;

            vertices[elem++] = xoffset + first + d * (x + 1);
            vertices[elem++] = yoffset + first + d * (y + 1);
            vertices[elem++] = zoffset;

            if (cull && cull_type == GEOM_CULL_TYPE_BACK) {
               /* switch the winding order */
               float old[6*3];
               memcpy(old, vertices + elem - 6*3, 6*3*4);

               for (unsigned i = 0; i < 6; i++) {
                  vertices[elem - 6*3 + i*3 + 0] = old[(5 - i)*3 + 0];
                  vertices[elem - 6*3 + i*3 + 1] = old[(5 - i)*3 + 1];
                  vertices[elem - 6*3 + i*3 + 2] = old[(5 - i)*3 + 2];
               }
            }

            if (cull && cull_type == GEOM_CULL_TYPE_DEGENERATE) {
               /* use any previously generated vertices */
               unsigned v0 = rand() % *num_vertices;
               unsigned v1 = rand() % *num_vertices;

               memcpy(&vertices[elem - 5*3], &vertices[v0*3], 12);
               memcpy(&vertices[elem - 4*3], &vertices[v0*3], 12);

               memcpy(&vertices[elem - 2*3], &vertices[v1*3], 12);
               memcpy(&vertices[elem - 1*3], &vertices[v1*3], 12);
            }
         }
      }
   }
}

static void
gen_triangle_strip_tile(unsigned num_quads_per_dim, double prim_size_in_pixels,
                        unsigned cull_percentage, enum geom_cull_type cull_type,
                        unsigned max_vertices, unsigned *num_vertices, float *vertices,
                        unsigned max_indices, unsigned *num_indices, uint32_t *indices)
{
   /* clip space coordinates in both X and Y directions: */
   const double first = -1;
   const double max_length = 2;
   const double d = prim_size_in_pixels * 2.0 / FB_SIZE;

   assert(d * num_quads_per_dim <= max_length);
   assert(*num_vertices == 0);

   /* the vertex ordering is counter-clockwise */
   for (unsigned y = 0; y < num_quads_per_dim; y++) {
      bool cull;

      if (cull_percentage == 0)
         cull = false;
      else if (cull_percentage == 25)
         cull = y % 4 == 0;
      else if (cull_percentage == 50)
         cull = y % 2 == 0;
      else if (cull_percentage == 75)
         cull = y % 4 != 0;
      else if (cull_percentage == 100)
         cull = true;
      else
         assert(!"wrong cull_percentage");

      /* view culling in different directions */
      double xoffset = 0, yoffset = 0, zoffset = 0;

      if (cull && cull_type == GEOM_CULL_TYPE_VIEW_XY) {
         unsigned side = (y / 2) % 4;

         if (side == 0)		xoffset = -2;
         else if (side == 1)	xoffset =  2;
         else if (side == 2)	yoffset = -2;
         else if (side == 3)	yoffset =  2;
      }

      if (cull && cull_type == GEOM_CULL_TYPE_DEGENERATE) {
         unsigned elem = *num_vertices * 3;
         *num_vertices += 2 + num_quads_per_dim * 2;
         assert(*num_vertices <= max_vertices);

         for (unsigned x = 0; x < 2 + num_quads_per_dim * 2; x++) {
            vertices[elem++] = 0;
            vertices[elem++] = 0;
            vertices[elem++] = 0;
         }
         continue;
      }

      unsigned elem = *num_vertices * 3;
      bool add_degenerates = y > 0;
      *num_vertices += (add_degenerates ? 4 : 0) + 2 + num_quads_per_dim * 2;
      assert(*num_vertices <= max_vertices);

      unsigned x = 0;
      unsigned y0 = y;
      unsigned y1 = y + 1;

      if (cull && cull_type == GEOM_CULL_TYPE_BACK) {
         y0 = y + 1;
         y1 = y;
      }

      /* Add degenerated triangles to connect with the previous triangle strip. */
      if (add_degenerates) {
         unsigned base = elem;

         vertices[elem++] = vertices[base - 3];
         vertices[elem++] = vertices[base - 2];
         vertices[elem++] = vertices[base - 1];
      }

      for (unsigned i = 0; i < (add_degenerates ? 4 : 1); i++) {
         vertices[elem++] = xoffset + first + d * x;
         vertices[elem++] = yoffset + first + d * y1;
         vertices[elem++] = zoffset;
      }

      vertices[elem++] = xoffset + first + d * x;
      vertices[elem++] = yoffset + first + d * y0;
      vertices[elem++] = zoffset;

      for (; x < num_quads_per_dim; x++) {
         vertices[elem++] = xoffset + first + d * (x + 1);
         vertices[elem++] = yoffset + first + d * y1;
         vertices[elem++] = zoffset;

         vertices[elem++] = xoffset + first + d * (x + 1);
         vertices[elem++] = yoffset + first + d * y0;
         vertices[elem++] = zoffset;
      }
   }

   if (indices) {
      for (unsigned i = 0; i < *num_vertices; i++)
         indices[i] = i;

      *num_indices = *num_vertices;
   }
}

static buffer_set_index
get_buffer_set_index(enum geometry_style geom_style, enum cull_method cull_method,
                     unsigned small_triangle_size_index, unsigned cull_percentage)
{
   assert(cull_percentage == 0 || cull_percentage == 25 || cull_percentage == 50 ||
          cull_percentage == 75 || cull_percentage == 100);
   assert(small_triangle_size_index < ARRAY_SIZE(triangle_sizes_in_pixels));

   buffer_set_index set;

   if (geom_style == GEOM_TRI_STRIP_INDEXED_PRIM_RESTART)
      set.geom_style_reduced = GEOM_TRI_STRIP_INDEXED;
   else
      set.geom_style_reduced = geom_style;

   set.cull_type = cull_method == CULL_BACK ? GEOM_CULL_TYPE_BACK :
                   cull_method == CULL_VIEW_XY ? GEOM_CULL_TYPE_VIEW_XY :
                   cull_method == DEGENERATE_TRIS ? GEOM_CULL_TYPE_DEGENERATE :
                   cull_method == CLIPDIST1357 || cull_method == CULLDIST1357 ||
                   cull_method == CLIPDIST4 || cull_method == CULLDIST4 ? GEOM_CULL_TYPE_CLIP_CULL_DIST4 :
                   cull_method == CLIPDIST8 || cull_method == CULLDIST8 ? GEOM_CULL_TYPE_CLIP_CULL_DIST8 :
                                                                          GEOM_CULL_TYPE_NONE;
   set.cull_percentage_div25 = set.cull_type == GEOM_CULL_TYPE_NONE ? 0 : cull_percentage / 25;
   set.small_triangle_size_index = small_triangle_size_index;
   set.pad = 0;
   return set;
}

static void
init_buffers(api_context *ctx, test_state *state, const test_info *test)
{
   const unsigned max_indices = 8100000 * 3;
   const unsigned max_vertices = max_indices;
   const unsigned max_groups = MAX_MESH_WORKGROUPS;

   buffer_set_index set = get_buffer_set_index(test->geom_style, test->cull_method, test->small_triangle_size_index,
                                               test->cull_percentage);

   assert(set.index < ARRAY_SIZE(state->buffers));
   unique_buffer_set *buffer_set = &state->buffers[set.index];

   /* If we have already initialized this buffer set, nothing to do. */
   if (buffer_set->vb)
      return;

   const unsigned num_quads_per_draw = 0.5 * NUM_PRIMITIVES_PER_DRAW;
   const unsigned num_quads_per_dim = sqrt(num_quads_per_draw);

   assert(set.small_triangle_size_index < ARRAY_SIZE(triangle_sizes_in_pixels));
   double quad_size_in_pixels = triangle_sizes_in_pixels[set.small_triangle_size_index];

   while (num_quads_per_dim * quad_size_in_pixels >= FB_SIZE)
      quad_size_in_pixels *= 0.5;

   unsigned num_special_attribs = set.cull_type == GEOM_CULL_TYPE_CLIP_CULL_DIST8 ? 2 :
                                  set.cull_type == GEOM_CULL_TYPE_CLIP_CULL_DIST4 ? 1 : 0;

   /* Generate vertices. */
   float *vertices = (float*)malloc(max_vertices * 12);
   float *special0 = num_special_attribs >= 1 ? (float*)malloc(max_vertices * 16) : NULL;
   float *special1 = num_special_attribs == 2 ? (float*)malloc(max_vertices * 16) : NULL;
   uint32_t *indices = NULL;
   uint32_t *mesh_groups = NULL;

   if (set.geom_style_reduced == GEOM_TRI_LIST_REUSE2_INDEXED ||
       set.geom_style_reduced == GEOM_TRI_LIST_REUSE1_INDEXED ||
       set.geom_style_reduced == GEOM_TRI_STRIP_INDEXED ||
       get_mesh_wg_size(set.geom_style_reduced))
      indices = (uint32_t*)malloc(max_indices * 4);

   if (get_mesh_wg_size(set.geom_style_reduced))
      mesh_groups = (uint32_t*)malloc(max_groups * 16);

   if (set.geom_style_reduced == GEOM_TRI_STRIP ||
       set.geom_style_reduced == GEOM_TRI_STRIP_INDEXED) {
      gen_triangle_strip_tile(num_quads_per_dim, quad_size_in_pixels, set.cull_percentage_div25 * 25,
                              set.cull_type, max_vertices, &buffer_set->num_vertices, vertices,
                              max_indices, &buffer_set->num_indices, indices);
   } else {
      gen_triangle_tile(num_quads_per_dim, quad_size_in_pixels, set.cull_percentage_div25 * 25,
                        set.geom_style_reduced == GEOM_TRI_LIST_REUSE1_INDEXED ? 2 : 1,
                        set.cull_type, get_mesh_wg_size(set.geom_style_reduced),
                        max_vertices, &buffer_set->num_vertices, vertices, special0, special1,
                        max_indices, &buffer_set->num_indices, indices,
                        max_groups, &buffer_set->num_mesh_groups, mesh_groups);
   }

#if 0 /* Print vertices, indices, and groups for mesh shaders for debugging */
   for (unsigned g = 0; g < buffer_set->num_groups; g++) {
      unsigned base_vertex = groups[g * 4 + 0];
      unsigned base_prim = groups[g * 4 + 1];
      unsigned num_vertices = groups[g * 4 + 2];
      unsigned num_prims = groups[g * 4 + 3];

      printf("group[%u] = %u, %u, %u, %u\n", g, base_vertex, base_prim, num_vertices, num_prims);

      for (unsigned p = 0; p < num_prims; p++) {
         printf("prim[%u] = ", base_prim + p);

         for (unsigned i = 0; i < 3; i++) {
            printf("%u, ", indices[(base_prim + p) * 3 + i]);
         }
         puts("");
      }

      for (unsigned v = 0; v < num_vertices; v++) {
         printf("vertex[%u] = ", base_vertex + v);

         for (unsigned i = 0; i < 3; i++) {
            printf("%f, ", vertices[(base_vertex + v) * 3 + i]);
         }
         puts("");
      }
   }
#endif

   unsigned pos_size = buffer_set->num_vertices * 12;
   unsigned special0_size = special0 ? buffer_set->num_vertices * 16 : 0;
   unsigned special1_size = special1 ? buffer_set->num_vertices * 16 : 0;
   unsigned ib_size = buffer_set->num_indices * 4;
   unsigned mesh_group_data_size = buffer_set->num_mesh_groups * 16;
   const float zero_stride_attrib[4] = {1, 1, 1, 1}; /* all varyings load this */

   buffer_set->vb_pos_offset = 16;
   buffer_set->vb_special0_offset = buffer_set->vb_pos_offset + pos_size;
   buffer_set->vb_special1_offset = buffer_set->vb_special0_offset + special0_size;
   unsigned vb_size = buffer_set->vb_special1_offset + special1_size;

   /* Create buffers. */
   buffer_set->vb = ctx->create_buffer(ctx, vb_size, api_heap_vram);
   ctx->upload_buffer_data(ctx, buffer_set->vb, 0, 16, zero_stride_attrib);
   ctx->upload_buffer_data(ctx, buffer_set->vb, buffer_set->vb_pos_offset, pos_size, vertices);
   if (special0_size)
      ctx->upload_buffer_data(ctx, buffer_set->vb, buffer_set->vb_special0_offset, special0_size, special0);
   if (special1_size)
      ctx->upload_buffer_data(ctx, buffer_set->vb, buffer_set->vb_special1_offset, special1_size, special1);
   free(vertices);
   free(special0);
   free(special1);

   if (indices) {
      buffer_set->ib = ctx->create_buffer(ctx, ib_size, api_heap_vram);
      ctx->upload_buffer_data(ctx, buffer_set->ib, 0, ib_size, indices);
      free(indices);
   }

   if (mesh_groups) {
      buffer_set->mesh_group_buf = ctx->create_buffer(ctx, 16 + mesh_group_data_size, api_heap_vram);
      ctx->upload_buffer_data(ctx, buffer_set->mesh_group_buf, 16, mesh_group_data_size, mesh_groups);
      free(mesh_groups);
   }

   /* Non-existent special attribs are remapped to the zero-stride attrib. */
   if (!special0_size) {
      buffer_set->vb_special0_offset = 0;
      special0_size = 16;
   }
   if (!special1_size) {
      buffer_set->vb_special1_offset = 0;
      special1_size = 16;
   }

   if (get_mesh_wg_size(set.geom_style_reduced)) {
      assert(indices);

      for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
         buffer_set->mesh_desc_set[v] = ctx->create_descriptor_set(ctx, state->ms_desc_set_layout[v]);

         ctx->set_uniform_buffer_descriptor(ctx, buffer_set->mesh_desc_set[v],
                                            buffer_set->mesh_group_buf, 0,
                                            buffer_set->mesh_group_buf->size);

         /* indices */
         ctx->set_uniform_texel_buffer_descriptors(ctx, buffer_set->mesh_desc_set[v], 0, 1,
                                                   (api_buffer*[1]){buffer_set->ib},
                                                   (VkFormat[1]){VK_FORMAT_R32G32B32_UINT},
                                                   (uint64_t[1]){0},
                                                   (uint64_t[1]){buffer_set->ib->size});

         /* pos */
         ctx->set_uniform_texel_buffer_descriptors(ctx, buffer_set->mesh_desc_set[v], 1, 1,
                                                   (api_buffer*[1]){buffer_set->vb},
                                                   (VkFormat[1]){VK_FORMAT_R32G32B32_SFLOAT},
                                                   (uint64_t[1]){buffer_set->vb_pos_offset},
                                                   (uint64_t[1]){pos_size});

         /* special0 */
         ctx->set_uniform_texel_buffer_descriptors(ctx, buffer_set->mesh_desc_set[v], 2, 1,
                                                   (api_buffer*[1]){buffer_set->vb},
                                                   (VkFormat[1]){VK_FORMAT_R32G32B32A32_SFLOAT},
                                                   (uint64_t[1]){buffer_set->vb_special0_offset },
                                                   (uint64_t[1]){special0_size});

         /* special1 */
         ctx->set_uniform_texel_buffer_descriptors(ctx, buffer_set->mesh_desc_set[v], 3, 1,
                                                   (api_buffer*[1]){buffer_set->vb},
                                                   (VkFormat[1]){VK_FORMAT_R32G32B32A32_SFLOAT},
                                                   (uint64_t[1]){buffer_set->vb_special1_offset},
                                                   (uint64_t[1]){special1_size});

         if (v) {
            api_buffer *buffers[MAX_VARYINGS];
            VkFormat formats[MAX_VARYINGS];
            uint64_t offsets[MAX_VARYINGS];
            uint64_t sizes[MAX_VARYINGS];

            for (unsigned i = 0; i < v; i++) {
               buffers[i] = buffer_set->vb;
               formats[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
               offsets[i] = 0;
               sizes[i] = 16;
            }

            ctx->set_uniform_texel_buffer_descriptors(ctx, buffer_set->mesh_desc_set[v], 4, v,
                                                      buffers, formats, offsets, sizes);
         }
      }
   }
}

static void
run_draws(api_context *ctx, unsigned num_iterations, enum geometry_style geom_style, unsigned count)
{
   if (geom_style == GEOM_TRI_LIST_REUSE2_INDEXED ||
       geom_style == GEOM_TRI_LIST_REUSE1_INDEXED ||
       geom_style == GEOM_TRI_STRIP_INDEXED ||
       geom_style == GEOM_TRI_STRIP_INDEXED_PRIM_RESTART) {
      for (unsigned i = 0; i < num_iterations; i++)
         ctx->draw(ctx, &(api_draw_desc){.indexed = true, .count = count});
   } else if (geom_style == GEOM_TRI_LIST_REUSE0 ||
              geom_style == GEOM_TRI_STRIP) {
      for (unsigned i = 0; i < num_iterations; i++)
         ctx->draw(ctx, &(api_draw_desc){.count = count});
   } else if (get_mesh_wg_size(geom_style)) {
      for (unsigned i = 0; i < num_iterations; i++)
         ctx->draw(ctx, &(api_draw_desc){.mesh_shader = true, .count = count});
   } else {
      error("unhandled geom_style");
   }
}

static void
run_pipeline(api_context *ctx, test_state *state, unsigned num_iterations, const test_info *test,
             unsigned num_varyings)
{
   buffer_set_index set = get_buffer_set_index(test->geom_style, test->cull_method, test->small_triangle_size_index,
                                               test->cull_percentage);
   unique_buffer_set *buffer_set = &state->buffers[set.index];

   assert(num_iterations);
   assert(buffer_set->vb);

   ctx->begin_cmdbuf(ctx);
   ctx->begin_render_pass(ctx, &(api_render_pass_desc){
                             .fb = state->fb,
                             .color_clear_value.float32 = {0.2, 0.2, 0.2, 1},
                          });
   assert(state->pipelines[test->geom_style][test->cull_method][num_varyings]);
   ctx->bind_pipeline(ctx, state->pipelines[test->geom_style][test->cull_method][num_varyings]);

   unsigned count = 0;

   if (get_mesh_wg_size(test->geom_style)) {
      ctx->bind_descriptor_set(ctx, buffer_set->mesh_desc_set[num_varyings]);
      count = buffer_set->num_mesh_groups;
   } else {
      const uint64_t vb_offsets[3 + MAX_VARYINGS] = {
         buffer_set->vb_pos_offset,
         buffer_set->vb_special0_offset,
         buffer_set->vb_special1_offset,
         /* other offsets are 0 */
      };

      ctx->bind_vertex_buffers(ctx, buffer_set->vb, vb_offsets);
      if (buffer_set->ib)
         ctx->bind_index_buffer(ctx, buffer_set->ib);

      count = buffer_set->ib ? buffer_set->num_indices : buffer_set->num_vertices;
   }

   /* Warm up the GPU. */
   run_draws(ctx, num_iterations / 4, test->geom_style, count);

   ctx->write_next_timestamp(ctx, state->timestamps);
   run_draws(ctx, num_iterations, test->geom_style, count);
   ctx->write_next_timestamp(ctx, state->timestamps);

   ctx->end_render_pass(ctx);
   ctx->end_cmdbuf_and_submit(ctx);
}

static void
create_pipeline(api_context *ctx, test_state *state, const test_info *test, unsigned num_varyings)
{
   if (!state->fs[num_varyings])
      return;

   bool is_mesh_shader = get_mesh_wg_size(test->geom_style) != 0;

   if (!ctx->has_mesh_shader && is_mesh_shader)
      return;

   if (test->special2 == SPECIAL2_OUTPUT_VRS1x1 && !ctx->has_vrs)
      return;

   enum special_attribute1 s1 = SPECIAL1_NONE;

   unsigned special0_stride = 0;
   unsigned special1_stride = 0;

   switch (test->cull_method) {
   case CLIPDIST1357:
      s1 = SPECIAL1_CLIPDIST1357;
      special0_stride = 16;
      break;
   case CULLDIST1357:
      s1 = SPECIAL1_CULLDIST1357;
      special0_stride = 16;
      break;
   case CLIPDIST4:
      s1 = SPECIAL1_CLIPDIST4;
      special0_stride = 16;
      break;
   case CULLDIST4:
      s1 = SPECIAL1_CULLDIST4;
      special0_stride = 16;
      break;
   case CLIPDIST8:
      s1 = SPECIAL1_CLIPDIST8;
      special0_stride = 16;
      special1_stride = 16;
      break;
   case CULLDIST8:
      s1 = SPECIAL1_CULLDIST8;
      special0_stride = 16;
      special1_stride = 16;
      break;
   default:;
   }

   api_pipeline_desc desc = {
      .topology = test->geom_style == GEOM_TRI_LIST_REUSE2_INDEXED ||
                  test->geom_style == GEOM_TRI_LIST_REUSE1_INDEXED ||
                  test->geom_style == GEOM_TRI_LIST_REUSE0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
      .primitive_restart = test->geom_style == GEOM_TRI_STRIP_INDEXED_PRIM_RESTART,

      .num_vb_desc = 3 + num_varyings,
      .vb_strides = {12, special0_stride, special1_stride}, /* other strides are 0 */
      .vb_formats = {
         VK_FORMAT_R32G32B32_SFLOAT, /* pos */
         VK_FORMAT_R32G32B32A32_SFLOAT, /* special0 */
         VK_FORMAT_R32G32B32A32_SFLOAT, /* special1 */
         VK_FORMAT_R32G32B32A32_SFLOAT, /* varyings */
         VK_FORMAT_R32G32B32A32_SFLOAT,
         VK_FORMAT_R32G32B32A32_SFLOAT,
         VK_FORMAT_R32G32B32A32_SFLOAT,
         VK_FORMAT_R32G32B32A32_SFLOAT,
         VK_FORMAT_R32G32B32A32_SFLOAT,
         VK_FORMAT_R32G32B32A32_SFLOAT,
         VK_FORMAT_R32G32B32A32_SFLOAT,
      },

      .desc_set_layout = is_mesh_shader ? state->ms_desc_set_layout[num_varyings] : NULL,
      .ms = is_mesh_shader ? state->ms[test->geom_style][s1][test->special2][num_varyings] : NULL,
      .vs = !is_mesh_shader ? state->vs[s1][test->special2][num_varyings] : NULL,
      .fs = state->fs[num_varyings],

      .vrs_fragment_size = {1, 1},
      .colormask = 0xf,
      .fb = state->fb,
   };

   switch (test->cull_method) {
   case CULL_NONE:
   case CULL_VIEW_XY:
   case CULLDIST1357:
   case CULLDIST4:
   case CULLDIST8:
   case DEGENERATE_TRIS:
   case SMALL_TRIS:
      break;
   case CLIPDIST1357:
      desc.clipdist_enable_mask = (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7);
      break;
   case CLIPDIST4:
      desc.clipdist_enable_mask = 0xf;
      break;
   case CLIPDIST8:
      desc.clipdist_enable_mask = 0xff;
      break;
   case CULL_BACK:
      desc.cull_mode = VK_CULL_MODE_BACK_BIT;
      break;
   case RASTERIZER_DISCARD:
      desc.rasterizer_discard = true;
      break;
   default:
      error("invalid cull method in create_pipeline");
   }

   state->pipelines[test->geom_style][test->cull_method][num_varyings] = ctx->create_pipeline(ctx, &desc);
}

static void
run_test(api_context *ctx, test_state *state, enum test_stage test_stage, const test_info *test)
{
   if (test_stage != REPORT && !ctx->has_mesh_shader && get_mesh_wg_size(test->geom_style))
      return;

   switch (test_stage) {
   case INIT:

      for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
         create_pipeline(ctx, state, test, v);

         if (!state->pipelines[test->geom_style][test->cull_method][v])
            continue;

         init_buffers(ctx, state, test);
      }
      break;

   case RUN:
      for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
         if (!state->pipelines[test->geom_style][test->cull_method][v])
            continue;

         run_pipeline(ctx, state, NUM_ITERATIONS, test, v);
      }
      break;

   case REPORT: {
      char name[1024], cull_info1[32] = {0}, cull_method[32], special2[32];
      const char *geom = NULL;

      switch (test->geom_style) {
      case GEOM_TRI_LIST_REUSE2_INDEXED:
         geom = ".trilist.reuse2";
         break;
      case GEOM_TRI_LIST_REUSE1_INDEXED:
         geom = ".trilist.reuse1";
         break;
      case GEOM_TRI_LIST_REUSE0:
         geom = ".trilist.reuse0";
         break;
      case GEOM_TRI_STRIP:
         geom = ".tristrip";
         break;
      case GEOM_TRI_STRIP_INDEXED:
         geom = ".tristrip.indexed";
         break;
      case GEOM_TRI_STRIP_INDEXED_PRIM_RESTART:
         geom = ".tristrip.indexed.primrestart";
         break;
      case GEOM_MESH32_REUSE2:
         geom = ".mesh32.reuse2";
         break;
      case GEOM_MESH64_REUSE2:
         geom = ".mesh64.reuse2";
         break;
      case GEOM_MESH128_REUSE2:
         geom = ".mesh128.reuse2";
         break;
      case GEOM_MESH192_REUSE2:
         geom = ".mesh192.reuse2";
         break;
      case GEOM_MESH256_REUSE2:
         geom = ".mesh256.reuse2";
         break;
      default:
         error("invalid geometry style");
      }

      if (test->cull_method == SMALL_TRIS) {
         snprintf(cull_info1, sizeof(cull_info1), ".%u_small_tris_pp",
                  (unsigned)(2.0 / (triangle_sizes_in_pixels[test->small_triangle_size_index] *
                                    triangle_sizes_in_pixels[test->small_triangle_size_index])));
      } else {
         snprintf(cull_info1, sizeof(cull_info1), ".cull_%u%%", test->cull_percentage);
      }

      snprintf(cull_method, sizeof(cull_method), "%s",
               test->cull_method == CULL_NONE || test->cull_method == SMALL_TRIS ? "" :
               test->cull_method == CULL_BACK ? ".cull_back" :
               test->cull_method == CULL_VIEW_XY ? ".cull_view_xy" :
               test->cull_method == CLIPDIST1357 ? ".clipdist1357" :
               test->cull_method == CULLDIST1357 ? ".culldist1357" :
               test->cull_method == CLIPDIST4 ? ".clipdist4" :
               test->cull_method == CULLDIST4 ? ".culldist4" :
               test->cull_method == CLIPDIST8 ? ".clipdist8" :
               test->cull_method == CULLDIST8 ? ".culldist8" :
               test->cull_method == RASTERIZER_DISCARD ? ".rasterizer_discard" :
               test->cull_method == DEGENERATE_TRIS ? ".degenerate" : "INVALID");

      snprintf(special2, sizeof(special2), "%s",
               test->special2 == SPECIAL2_NONE ? "" :
               test->special2 == SPECIAL2_OUTPUT_POINT_SIZE ? ".output_pointsize" :
               test->special2 == SPECIAL2_OUTPUT_LAYER ? ".output_layer" :
               test->special2 == SPECIAL2_OUTPUT_VRS1x1 ? ".output_vrs1x1" : "INVALID");

      snprintf(name, sizeof(name), "%s%s%s%s%s", state->test_suite_name, cull_info1, geom,
               cull_method, special2);
      printf("%-62s", name);

      for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
         if (!state->fs[v])
            continue;

         if (state->pipelines[test->geom_style][test->cull_method][v]) {
            print_throughput_from_next_timestamps(ctx, state->timestamps,
                                                  NUM_ITERATIONS * NUM_PRIMITIVES_PER_DRAW,
                                                  ctx->options.max_rate ? ",%6.1f" : ",%6.2f", NULL);
         } else {
            printf(",   n/a");
         }
      }

      printf("\n");
      break;
   }
   }
}

void
test_prim_rate(api_context *ctx, const char *test_suite_name)
{
   test_state *state = calloc(1, sizeof(test_state));

   state->test_suite_name = test_suite_name;

   /* Create the framebuffer. */
   api_image *colorbuf = ctx->create_image(ctx, VK_FORMAT_R8G8B8A8_UNORM, FB_SIZE, FB_SIZE, 1,
                                           VK_IMAGE_TILING_OPTIMAL, api_heap_vram, 0);
   state->fb = ctx->create_framebuffer(ctx, colorbuf, NULL, colorbuf->width,
                                       colorbuf->height, colorbuf->samples);

   /* Create the timestamp pool. */
   state->timestamps = ctx->create_timestamp_pool(ctx, MAX_VARYING_SHADERS * ARRAY_SIZE(tests) * 2);

   /* Create the descriptor set for mesh shaders. */
   if (ctx->has_mesh_shader) {
      for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
         api_descriptor_set_layout_desc desc = {
            .uniform_buffer.array_size = 1
         };

         /* indices, pos, special0, special1 */
         for (unsigned i = 0; i < 5; i++) {
            desc.uniform_texel_buffer[i].gl_binding = i;
            desc.uniform_texel_buffer[i].vk_binding = 1 + i;
            desc.uniform_texel_buffer[i].array_size = i == 4 ? v : 1;
         }

         state->ms_desc_set_layout[v] =
            ctx->create_descriptor_set_layout(ctx, &desc);
      }
   }

   puts("Compiling shaders...");
   compile_shaders(ctx, state);

   /* for debugging */
   if (getenv("VS") || getenv("MESH") || getenv("CLIPDIST") || getenv("TRI")) {
      unsigned num_varyings = 1;
      test_info test = {0};

      if (getenv("VS"))
         test = (test_info){50, GEOM_TRI_LIST_REUSE2_INDEXED, CULL_BACK};
      if (getenv("MESH"))
         test = (test_info){50, GEOM_MESH128_REUSE2, CULL_BACK};
      if (getenv("CLIPDIST"))
         test = (test_info){50, GEOM_TRI_LIST_REUSE2_INDEXED, CLIPDIST8};
      if (getenv("TRI")) {
         test = (test_info){0, GEOM_TRI_LIST_REUSE0, CULL_BACK};
         num_varyings = 0;
      }

      create_pipeline(ctx, state, &test, num_varyings);
      init_buffers(ctx, state, &test);
      run_pipeline(ctx, state, 1, &test, num_varyings);
      ctx->image_write_png(ctx, colorbuf, "output.png");
      run_image_viewer("output.png");
      return;
   }

   puts("Creating buffers and building pipelines...");
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++)
      run_test(ctx, state, INIT, &tests[i]);

   printf("GPU memory allocated: %u MB\n", (unsigned)(ctx->vram_usage >> 20));
   printf("Executing tests ...");
   fflush(stdout);

   unsigned num_visited_tests = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      run_test(ctx, state, RUN, &tests[i]);
      print_progress(ARRAY_SIZE(tests), &num_visited_tests, 20);
   }
   puts("");

   puts("Reading back results...");
   ctx->query_timestamps(ctx, state->timestamps);

   printf("Units: %s\n",
          ctx->options.max_rate ? "% of the maximum primitive rate" :
          ctx->options.freq_mhz ? "primitives/clock" :
                                  "billion primitives/second");

   printf("%-62s", "Number of vec4 varyings");
   for (unsigned v = 0; v < MAX_VARYING_SHADERS; v++) {
      if (state->fs[v])
         printf(",     %u", v);
   }
   puts("");

   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++)
      run_test(ctx, state, REPORT, &tests[i]);

   free(state);
}
