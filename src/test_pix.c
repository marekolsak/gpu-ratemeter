/* Copyright 2026 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include <alloca.h>
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "common.h"

/* Draw over the whole viewport this many times. */
#define NUM_FULLSCREEN_DRAWS 10

static_assert(NUM_FULLSCREEN_DRAWS <= 50, "required by ztest_less");
static_assert(NUM_FULLSCREEN_DRAWS % 2 == 0, "required by multiview");

typedef struct {
   /* These are concatenated at runtime to form the test name.
    * C macros can't concatenate conditional string expressions, so we do it at runtime.
    */
   const char *name1;
   const char *name2;
   const char *name3;
   const char *name4;
   const char *name5;
   const char *name6;

   const char *vs_source;
   const char *fs_source;

   int reserved_for_static_assert;

   /* Only for raster tests. */
   struct {
      unsigned quads_per_row;
      unsigned rows_per_mesh;
      unsigned num_meshes_x;
   } raster;
} pipeline_info;

static const char *vs_shared_code =
   "#version 460\n"
   "#ifndef VULKAN \n"
   "#define gl_VertexIndex (gl_VertexID + gl_BaseVertex) \n"
   "#endif \n"
   "\n";

static const char *fs_shared_code =
   "#version 460\n"
   "\n"
   "#define IMAGE_STORE 0 \n" /* 0 is replaced by 1 during compilation */
   "#define FS_OUTPUT_TYPE 0 \n" /* 0=float, 1=int, 2=uint, replaced during compilation */
   "#define HAS_VRS 0 \n"
   "#define HAS_FULLY_COVERED 0 \n"
   "#define HAS_MULTIVIEW 0 \n"
   "#define GATHER_TOTAL_FS_INVOC 0 \n"
   "\n"

   "#if HAS_VRS \n"
   "#extension GL_EXT_fragment_shading_rate : require \n"
   "#endif \n"
   "\n"

   "#if HAS_FULLY_COVERED \n"
   "#extension GL_NV_conservative_raster_underestimation : require \n"
   "#endif \n"
   "\n"

   "#if HAS_MULTIVIEW \n"
   "#extension GL_EXT_multiview : require \n"
   "#endif \n"
   "\n"

   "#if GATHER_TOTAL_FS_INVOC \n"
   "#extension GL_KHR_shader_subgroup_basic : require \n"
   "#extension GL_KHR_shader_subgroup_quad : require \n"
   "#endif \n"
   "\n"

   "#if FS_OUTPUT_TYPE == 0 \n"
   "#define FS_OUTPUT_TYPE_NAME vec4 \n"
   "#elif FS_OUTPUT_TYPE == 1 \n"
   "#define FS_OUTPUT_TYPE_NAME ivec4 \n"
   "#elif FS_OUTPUT_TYPE == 2 \n"
   "#define FS_OUTPUT_TYPE_NAME uvec4 \n"
   "#endif \n"
   "\n"

   "#if IMAGE_STORE \n"
   "   #ifdef VULKAN \n"
   "      layout(set = 0, binding = 0) writeonly uniform image2D image; \n"
   "   #else \n"
   "      layout(location = 0) writeonly uniform image2D image; \n"
   "   #endif \n"
   "#else \n"
   "   layout(location = 0) out FS_OUTPUT_TYPE_NAME fs_out; \n"
   "#endif \n"
   "\n"

   "#if GATHER_TOTAL_FS_INVOC \n"
   "#ifdef VULKAN \n"
   "layout(set = 0, binding = 1, std430) buffer Counters { \n"
   "#else \n"
   "layout(binding = 0, std430) buffer Counters { \n"
   "#endif \n"
   "    uint num_invocations; \n"
   "}; \n"
   "\n"

   "bool isFirstNonHelperInvocationInQuad() \n"
   "{ \n"
   "    bool pred = !gl_HelperInvocation; \n"
   "    bool p0 = subgroupQuadBroadcast(pred, 0); \n"
   "    bool p1 = subgroupQuadBroadcast(pred, 1); \n"
   "    bool p2 = subgroupQuadBroadcast(pred, 2); \n"
   "    bool p3 = subgroupQuadBroadcast(pred, 3); \n"
   " \n"
   "    uint id = gl_SubgroupInvocationID; \n"
   "    uint id0 = subgroupQuadBroadcast(id, 0); \n"
   "    uint id1 = subgroupQuadBroadcast(id, 1); \n"
   "    uint id2 = subgroupQuadBroadcast(id, 2); \n"
   "    uint id3 = subgroupQuadBroadcast(id, 3); \n"
   " \n"
   "    uint first = p0 ? id0 : \n"
   "                 p1 ? id1 : \n"
   "                 p2 ? id2 : \n"
   "                 p3 ? id3 : 0xffffffffu; \n"
   " \n"
   "    return pred && id == first; \n"
   "} \n"
   "#endif \n"
   "\n"

   "void store_output_color0(vec4 value) { \n"
   "#if IMAGE_STORE \n"
   /* This is out of bounds on purpose because we want to measure the pixel rate,
    * not memory throughput.
    */
   "   imageStore(image, ivec2(32, 32), value); \n"
   "#elif FS_OUTPUT_TYPE == 0 \n"
   "   fs_out = value; \n"
   "#else \n"
   "   fs_out = FS_OUTPUT_TYPE_NAME(value * 127); \n"
   "#endif \n"
   "} \n"
   "\n";

/* This is one triangle that fills the whole screen. Drawing 2 triangles would make the GPU less
 * efficient along the diagonal edge, which does skew results noticably.
 */
#define VS_SET_POSITION(z) "   gl_Position = vec4(gl_VertexIndex % 3 == 0 ? vec2(-2, -1) : \n" \
                           "                      gl_VertexIndex % 3 == 1 ? vec2( 2, -1) : \n" \
                           "                                                vec2( 0,  3), "z", 1); \n"

#define VS_POS_WITH_Z(z) \
   "void main() {\n" \
   VS_SET_POSITION(z) \
   "}\n"

#define VS_POS_ONLY VS_POS_WITH_Z("0")

#define INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, sysval_name, sysval_code) \
   {num1 ? "." #num1 qual1_name : "", \
   num2 ? "." #num2 qual2_name : "", \
   sysval_name, \
   "", \
   "", \
   "", \
   \
   "#if "#num1" > 0 \n" \
   "layout(location = 0) " qual1 " out vec4 var["#num1"]; \n" \
   "#endif \n" \
   \
   "#if "#num2" > 0 \n" \
   "layout(location = "#num1") " qual2 " out vec4 linear["#num2"]; \n" \
   "#endif \n" \
   \
   "void main() {\n" \
   "   float id = float(gl_VertexIndex); \n" \
   \
   /* These multiplications prevent shader linker optimizations. */ \
   "#if "#num1" > 0 \n" \
   "   var[0].x = id; \n" \
   "   for (int i = 1; i < "#num1" * 4; i++) \n" \
   "      var[i / 4][i % 4] = var[(i - 1) / 4][(i - 1) % 4] * id; \n" \
   "\n" \
   "#endif \n" \
   \
   "#if "#num2" > 0 \n" \
   "   linear[0].x = var["#num1" - 1].w; \n"\
   "   for (int i = 1; i < "#num2" * 4; i++) \n" \
   "      linear[i / 4][i % 4] = linear[(i - 1) / 4][(i - 1) % 4] * id; \n" \
   "#endif \n" \
   \
   VS_SET_POSITION("0") \
   "}\n", \
   \
   "#if "#num1" > 0 \n" \
   "layout(location = 0) " qual1 " in vec4 var["#num1"]; \n" \
   "#endif \n" \
   \
   "#if "#num2" > 0 \n" \
   "layout(location = "#num1") " qual2 " in vec4 linear["#num2"]; \n" \
   "#endif \n" \
   \
   "void main() {\n" \
   "   vec4 v = vec4(0.001); \n" \
   \
   /* These multiplications prevent shader linker optimizations. */ \
   "#if "#num1" > 0 \n" \
   "   for (int i = 0; i < "#num1" ; i++) v *= var[i]; \n" \
   "#endif \n" \
   \
   "#if "#num2" > 0 \n" \
   "   for (int i = 0; i < "#num2" ; i++) v *= linear[i]; \n" \
   "#endif \n" \
   \
   "   v.x *= v.y * v.z * v.w; \n" \
   "   store_output_color0(v + " sysval_code "); \n" \
   "}\n"}

#define INPUTS(num1, qual1_name, qual1, num2, qual2_name, qual2) \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, num1 || num2 ? "" : ".const_fill", "vec4(0.5, 0.4, 0, 1)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".face", "vec4(gl_FrontFacing)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".face.cull_back", "vec4(gl_FrontFacing)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".samplemask", "vec4(gl_SampleMaskIn[0])"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_x", "gl_FragCoord.xxxx"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_y", "gl_FragCoord.yyyy"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_z", "gl_FragCoord.zzzz"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_w", "gl_FragCoord.wwww"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xy", "vec4(dot(gl_FragCoord.xy, vec2(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_zw", "vec4(dot(gl_FragCoord.zw, vec2(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xyz", "vec4(dot(gl_FragCoord.xyz, vec3(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xyw", "vec4(dot(gl_FragCoord.xyw, vec3(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xyzw", "vec4(dot(gl_FragCoord, vec4(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xy.face", "vec4(dot(gl_FragCoord.xy, vec2(1)) + float(gl_FrontFacing))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xy.face.cull_back", "vec4(dot(gl_FragCoord.xy, vec2(1)) + float(gl_FrontFacing))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xy.samplemask", "vec4(dot(gl_FragCoord.xy, vec2(1)) + float(gl_SampleMaskIn[0]))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xy.face.samplemask", "vec4(dot(gl_FragCoord.xy, vec2(1)) + float(gl_FrontFacing) + float(gl_SampleMaskIn[0]))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xyzw.face.samplemask", "vec4(dot(gl_FragCoord, vec4(1)) + float(gl_FrontFacing) + float(gl_SampleMaskIn[0]))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fragpos_xyzw.face.samplemask.cull_back", "vec4(dot(gl_FragCoord, vec4(1)) + float(gl_FrontFacing) + float(gl_SampleMaskIn[0]))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".layer", "vec4(gl_Layer)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".primitive_id", "vec4(gl_PrimitiveID)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".shading_rate", "vec4(gl_ShadingRateEXT)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".fully_covered", "vec4(gl_FragFullyCoveredNV)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid", "vec4(gl_SampleID)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.samplepos", "vec4(float(gl_SampleID) + dot(gl_SamplePosition.xy, vec2(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.samplemask", "vec4(float(gl_SampleID) + float(gl_SampleMaskIn[0]))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.samplepos.samplemask", "vec4(float(gl_SampleID) + dot(gl_SamplePosition.xy, vec2(1)) + float(gl_SampleMaskIn[0]))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.fragpos_xy", "vec4(float(gl_SampleID) + dot(gl_FragCoord.xy, vec2(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.fragpos_z", "vec4(float(gl_SampleID) + gl_FragCoord.z)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.fragpos_xyzw", "vec4(float(gl_SampleID) + dot(gl_FragCoord, vec4(1)))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".sampleid.samplepos.samplemask.fragpos_xyzw.face", "vec4(float(gl_SampleID) + dot(gl_SamplePosition.xy, vec2(1)) + float(gl_SampleMaskIn[0]) + dot(gl_FragCoord, vec4(1)) + float(gl_FrontFacing))"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".view_index", "vec4(gl_ViewIndex)"), \
   INPUTS_IMPL(num1, qual1_name, qual1, num2, qual2_name, qual2, ".viewport_index", "vec4(gl_ViewportIndex)")

#define INPUTS1(qual1_name, qual1) \
   INPUTS(1, qual1_name, qual1, 0, "", ""), \
   INPUTS(2, qual1_name, qual1, 0, "", ""), \
   INPUTS(3, qual1_name, qual1, 0, "", ""), \
   INPUTS(4, qual1_name, qual1, 0, "", ""), \
   INPUTS(5, qual1_name, qual1, 0, "", ""), \
   INPUTS(6, qual1_name, qual1, 0, "", ""), \
   INPUTS(7, qual1_name, qual1, 0, "", ""), \
   INPUTS(8, qual1_name, qual1, 0, "", "")

#define INPUTS2(qual1_name, qual1, qual2_name, qual2) \
   INPUTS(1, qual1_name, qual1, 1, qual2_name, qual2), \
   INPUTS(2, qual1_name, qual1, 1, qual2_name, qual2), \
   INPUTS(3, qual1_name, qual1, 1, qual2_name, qual2), \
   INPUTS(4, qual1_name, qual1, 1, qual2_name, qual2), \
   INPUTS(5, qual1_name, qual1, 1, qual2_name, qual2), \
   INPUTS(6, qual1_name, qual1, 1, qual2_name, qual2), \
   INPUTS(7, qual1_name, qual1, 1, qual2_name, qual2)

#define WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(zbuf, write_color, write_z, write_samplemask, alpha_to_coverage, color_disabled, z_disabled, helper_invoc) \
   { zbuf ? ".zbuf.zwrite.output" : ".output", \
   write_color && write_z && write_samplemask ? ".color+z+samplemask" : \
   write_color && write_z ? ".color+z" : \
   write_color && write_samplemask ? ".color+samplemask" : \
   !write_color && write_z && write_samplemask ? ".z+samplemask" : \
   !write_color && write_z ? ".z" : \
   !write_color && write_samplemask ? ".samplemask" : \
   write_color && alpha_to_coverage ? ".color" : ".INVALID", \
   alpha_to_coverage ? ".a2c" : "", \
   color_disabled ? ".colormask=0" : "", \
   z_disabled ? ".z_disabled" : "", \
   helper_invoc ? ".helper_invoc" : "", \
   \
   VS_POS_ONLY,\
   \
   "void main() {\n" \
   "#if "#helper_invoc" \n" \
   "   float not_helperf = float(!gl_HelperInvocation); \n" \
   "   int not_helperi = int(!gl_HelperInvocation); \n" \
   "#else \n" \
   "   float not_helperf = 1; \n" \
   "   int not_helperi = 1; \n" \
   "#endif \n" \
   \
   "#if "#write_color" \n" \
   "   store_output_color0(vec4(not_helperf * 0.5, 0.6, 0.7, 0.8)); \n" \
   "#endif \n" \
   \
   "#if "#write_z" \n" \
   "   gl_FragDepth = not_helperf * 0.5; \n" \
   "#endif \n" \
   \
   "#if "#write_samplemask" \n" \
   "   gl_SampleMask[0] = not_helperi; \n" \
   "#endif \n" \
   "}", \
   \
   STATIC_ASSERT_EXPR(write_color + write_z + write_samplemask + alpha_to_coverage >= (write_color ? 2 : 1)) + \
   STATIC_ASSERT_EXPR(write_color || (!color_disabled && !alpha_to_coverage)) + \
   STATIC_ASSERT_EXPR(write_z || !z_disabled) + \
   STATIC_ASSERT_EXPR(!(write_z && !write_color && !write_samplemask) || !z_disabled) + \
   STATIC_ASSERT_EXPR(!write_samplemask || !alpha_to_coverage) + \
   STATIC_ASSERT_EXPR(zbuf || !z_disabled)}

#define WRITE_COLOR_Z_SAMPLEMASK_A2C(helper_invoc) \
   /* output.color+z+samplemask */ \
   /* zbuf=0, write_color=1, write_z=1, write_samplemask=1, A2C=0, color_disabled=#, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 1, 1, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 1, 1, 0, 1, 0, helper_invoc), \
   /* output.color+z */ \
   /* zbuf=0, write_color=1, write_z=1, write_samplemask=0, A2C=#, color_disabled=#, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 1, 0, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 1, 0, 0, 1, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 1, 0, 1, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 1, 0, 1, 1, 0, helper_invoc), \
   /* output.color+samplemask */ \
   /* zbuf=0, write_color=1, write_z=0, write_samplemask=1, A2C=0, color_disabled=#, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 0, 1, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 0, 1, 0, 1, 0, helper_invoc), \
   /* output.z+samplemask */ \
   /* zbuf=0, write_color=0, write_z=1, write_samplemask=1, A2C=0, color_disabled=0, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 0, 1, 1, 0, 0, 0, helper_invoc), \
   /* output.color.alpha_to_coverage */ \
   /* zbuf=0, write_color=1, write_z=0, write_samplemask=0, A2C=1, color_disabled=#, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 0, 0, 1, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 1, 0, 0, 1, 1, 0, helper_invoc), \
   /* output.z */ \
   /* zbuf=0, write_color=0, write_z=1, write_samplemask=0, A2C=0, color_disabled=0, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 0, 1, 0, 0, 0, 0, helper_invoc), \
   /* output.samplemask */ \
   /* zbuf=0, write_color=0, write_z=0, write_samplemask=1, A2C=0, color_disabled=0, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(0, 0, 0, 1, 0, 0, 0, helper_invoc), \
   \
   /* zbuf=1: */ \
   /* zbuf.output.color+z+samplemask */ \
   /* zbuf=1, write_color=1, write_z=1, write_samplemask=1, A2C=0, color_disabled=#, z_disabled=# */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 1, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 1, 0, 0, 1, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 1, 0, 1, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 1, 0, 1, 1, helper_invoc), \
   /* zbuf.output.color+z */ \
   /* zbuf=1, write_color=1, write_z=1, write_samplemask=0, A2C=#, color_disabled=#, z_disabled=# */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 0, 0, 1, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 0, 1, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 0, 1, 1, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 1, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 1, 0, 1, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 1, 1, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 1, 0, 1, 1, 1, helper_invoc), \
   /* zbuf.output.color+samplemask */ \
   /* zbuf=1, write_color=1, write_z=0, write_samplemask=1, A2C=0, color_disabled=#, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 0, 1, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 0, 1, 0, 1, 0, helper_invoc), \
   /* zbuf.output.z+samplemask */ \
   /* zbuf=1, write_color=0, write_z=1, write_samplemask=1, A2C=0, color_disabled=0, z_disabled=# */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 0, 1, 1, 0, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 0, 1, 1, 0, 0, 1, helper_invoc), \
   /* zbuf.output.color.alpha_to_coverage */ \
   /* zbuf=1, write_color=1, write_z=0, write_samplemask=0, A2C=1, color_disabled=#, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 0, 0, 1, 0, 0, helper_invoc), \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 1, 0, 0, 1, 1, 0, helper_invoc), \
   /* zbuf.output.z */ \
   /* zbuf=1, write_color=0, write_z=1, write_samplemask=0, A2C=0, color_disabled=0, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 0, 1, 0, 0, 0, 0, helper_invoc), \
   /* zbuf.output.samplemask */ \
   /* zbuf=1, write_color=0, write_z=0, write_samplemask=1, A2C=0, color_disabled=0, z_disabled=0 */ \
   WRITE_COLOR_Z_SAMPLEMASK_A2C_IMPL(1, 0, 0, 1, 0, 0, 0, helper_invoc)


/* gl_HelperInvocation tests exist to prevent the driver from enabling VRS coarse shading
 * automatically because most of these tests produce the same results for all pixels of the same
 * primitive and the driver can deduce that and turn on VRS coarse shading as an optimization.
 * Hopefully gl_HelperInvocation is enough to fool the driver that pixels of the same primitive
 * can be different, so that we get results with VRS disabled, which is what we want.
 * gl_HelperInvocation is free on AMD, so this should have no effect on performance.
 *
 * Automatic VRS coarse shading is OK to get with the const_fill and flat shading shaders, but
 * for HW/driver performance evaluation, we also want to know what the results are with VRS
 * coarse shading disabled, and that's why these tests have variants with gl_HelperInvocation.
 */
#define FS_WRITE_COLOR_CONST(helper_invoc, x, y, z, w) \
   "void main() {\n" \
   "#if "#helper_invoc" \n" \
   "   bool helper = gl_HelperInvocation; \n" \
   "#else \n" \
   "   bool helper = false; \n" \
   "#endif \n" \
   "   store_output_color0(vec4(helper ? 0.123456789 : "#x", "#y", "#z", "#w")); \n" \
   "}"

#define WRITE_COLOR_CONST_IMPL(name, helper_invoc, x, y, z, w) \
   {name, \
   helper_invoc ? ".helper_invoc" : "", \
   "", \
   "", \
   "", \
   "", \
   \
   VS_POS_ONLY, \
   FS_WRITE_COLOR_CONST(helper_invoc, x, y, z, w)}

#define WRITE_COLOR_CONST(helper_invoc) \
   WRITE_COLOR_CONST_IMPL(".colormask=0", helper_invoc, 0.5, 0.6, 0.7, 0.8), \
   WRITE_COLOR_CONST_IMPL(".colormask=x", helper_invoc, 0.5, 0.6, 0.7, 0.8), \
   WRITE_COLOR_CONST_IMPL(".blend_src_color0", helper_invoc, 0, 0, 0, 0), \
   WRITE_COLOR_CONST_IMPL(".blend_src_color1", helper_invoc, 1, 1, 1, 1), \
   WRITE_COLOR_CONST_IMPL(".blend_src_color_other", helper_invoc, 0.5, 0.6, 0.7, 0.8), \
   WRITE_COLOR_CONST_IMPL(".blend_src_alpha0", helper_invoc, 0.5, 0.6, 0.7, 0), \
   WRITE_COLOR_CONST_IMPL(".blend_src_alpha1", helper_invoc, 0.5, 0.6, 0.7, 1), \
   WRITE_COLOR_CONST_IMPL(".blend_src_alpha_other", helper_invoc, 0.5, 0.6, 0.7, 0.8)

#define VRS_IMPL(vrs_width, vrs_height, helper_invoc) \
   {".vrs" #vrs_width "x" #vrs_height, \
    helper_invoc ? ".helper_invoc" : "", \
    "", \
    "", \
    "", \
    "", \
    \
    VS_POS_ONLY, \
    FS_WRITE_COLOR_CONST(helper_invoc, 0.1, 0.2, 0.3, 0.4)}

#define ZTEST_IMPL(zwrite, ztest, helper_invoc, z, fs_name, fs) \
   {".zbuf", \
    zwrite ? ".zwrite" : "", \
    ztest, \
    fs_name, \
    helper_invoc ? ".helper_invoc" : "", \
    "", \
    \
   VS_POS_WITH_Z(z), \
    (fs)}

#define FS_EMPTY(helper_invoc) \
   (helper_invoc ? NULL : \
      "void main() {}")

#define FS_DISCARD(helper_invoc) \
   (helper_invoc ? \
      "void main() {\n" \
      "   if (!gl_HelperInvocation) \n" \
      "      discard; \n" \
      "   store_output_color0(vec4(0.1, 0.2, 0.3, 0.4));\n" \
      "}" : \
      "void main() {\n" \
      "   discard; \n" \
      "   store_output_color0(vec4(0.1, 0.2, 0.3, 0.4));\n" \
      "}")

#define FS_DISCARD_NO_OUTPUT(helper_invoc) \
   (helper_invoc ? \
      "void main() {\n" \
      "   if (!gl_HelperInvocation) \n" \
      "      discard; \n" \
      "}" : \
      "void main() {\n" \
      "   discard; \n" \
      "}")

#define ZTEST_FS(zwrite, ztest, helper_invoc, z) \
   ZTEST_IMPL(zwrite, ztest, helper_invoc, z, "", FS_WRITE_COLOR_CONST(helper_invoc, 0.2, 0.3, 0.4, 0.5)), \
   ZTEST_IMPL(zwrite, ztest, helper_invoc, z, ".fs_empty", zwrite ? FS_EMPTY(helper_invoc) : NULL), \
   ZTEST_IMPL(zwrite, ztest, helper_invoc, z, ".fs_discard", zwrite ? FS_DISCARD(helper_invoc) : NULL), \
   ZTEST_IMPL(zwrite, ztest, helper_invoc, z, ".fs_discard_no_output", zwrite ? FS_DISCARD_NO_OUTPUT(helper_invoc) : NULL)

#define ZTESTS_NO_ZWRITE(helper_invoc) \
   ZTEST_FS(0, ".ztest_never.fail", helper_invoc, "0")

/* Note: Z is cleared to 0.5. */
#define ZTESTS(zwrite, helper_invoc) \
   ZTEST_FS(zwrite, ".ztest_less.fail", helper_invoc, "0.7"), \
   ZTEST_FS(zwrite, ".ztest_notequal.fail", helper_invoc, "0.5"), \
   /* Decrease position.z slightly for every draw, so that the LESS test keeps passing with Z writes enabled. */ \
   ZTEST_FS(zwrite, ".ztest_less.pass", helper_invoc, "0.5 - (float(gl_VertexIndex / 3 + 1) * 0.01)"), \
   ZTEST_FS(zwrite, ".ztest_equal.pass", helper_invoc, "0.5")

#define NAME(name) name, "", "", "", "", ""

#define VRS(helper_invoc) \
   VRS_IMPL(1, 2, helper_invoc), \
   VRS_IMPL(2, 1, helper_invoc), \
   VRS_IMPL(2, 2, helper_invoc)

#define VS_RASTER(quads_per_row, rows_per_mesh, num_meshes_x) \
   "layout(location = 0) out float out0; \n" \
   "\n" \
   \
   "vec2 rotate(vec2 p, float degrees) \n" \
   "{ \n" \
   "    float a = radians(degrees); \n" \
   "    float s = sin(a); \n" \
   "    float c = cos(a); \n" \
   "\n" \
   "    return vec2(c * p.x - s * p.y, \n" \
   "                s * p.x + c * p.y); \n" \
   "} \n" \
   "\n" \
   "void main() \n" \
   "{ \n" \
   \
   /* Below is a procedurally-generated 2D grid of geometry divided into meshes.     */ \
   /* Each mesh has a fixed number of vertices and it's roughly a 2D triangle patch. */ \
   /*                                                                                */ \
   /* The 2D space is filled with the meshes in the Morton order, so the number      */ \
   /* of meshes to draw must be n^2. Then the whole thing is centered in clip space  */ \
   /* and rotated to make all edges not parallel with axes to get FS helper          */ \
   /* invocations at all edges.                                                      */ \
   /*                                                                                */ \
   /* The number of vertex indices must be: 6 * quads_per_row * rows_per_mesh * num_meshes_x^2 */ \
   /* Try to keep quads_per_row:rows_per_mesh ratio 7:10.                            */ \
   /* - quads_per_row must be a multiple of 7.                                       */ \
   /* - rows_per_mesh must be even if greater than 1.                                */ \
   /* - num_meshes_x must be 2^n.                                                    */ \
   \
   /* Geometry parameters. */ \
   "   const int vertices_per_strip = 16; \n" \
   "   const int quads_per_row = "#quads_per_row"; \n" \
   "   const int strips_per_row = quads_per_row / 7; \n" \
   "   const int rows_per_mesh = "#rows_per_mesh"; \n" \
   "   const int num_meshes_x = "#num_meshes_x"; \n" \
   "\n" \
   \
   /* Sizing. Only the ratio matters. It should be roughly an equilateral triangle. */ \
   "   const int top_edge_tri_length = 4; \n" \
   "   const int strip_width = top_edge_tri_length * 7; \n" \
   "   const int tri_height = 3; \n" \
   "\n" \
   \
   "   const int vertices_per_mesh = vertices_per_strip *  rows_per_mesh; \n" \
   "\n" \
   \
   "   const int top_edge_mesh_length = top_edge_tri_length * quads_per_row; \n" \
   "   const int mesh_height = tri_height * rows_per_mesh; \n" \
   "\n" \
   \
   /* Total size */ \
   "   const int top_edge_geom_length = top_edge_mesh_length * num_meshes_x; \n" \
   "   const int geom_height = mesh_height * num_meshes_x; \n" \
   \
   "   int index = gl_VertexIndex; \n" \
   \
   "   const int vtx = index % vertices_per_strip; \n" \
   "   index /= vertices_per_strip; \n" \
   \
   "   const int strip_index_in_row = index % strips_per_row; \n" \
   "   index /= strips_per_row; \n" \
   \
   "   const int strip_row = index % rows_per_mesh; \n" \
   "   index /= rows_per_mesh; \n" \
   \
   "   int mesh_index = index; \n" \
   "\n" \
   \
   /* Generate positions within the quad. It's a parallelogram consisting */ \
   /* of 2 roughly equilateral triangles.                                 */ \
   /*                                                                     */ \
   /* The top edge length is 4 and the height is 3.                       */ \
   /*                                                                     */ \
   /* Each quad uses these indices:                                       */ \
   /*                                                                     */ \
   /* 0  2  4  6    = X coordinates                                       */ \
   /*                                                                     */ \
   /* 0-----2     0 = Y coordinate                                        */ \
   /*  \   / \                                                            */ \
   /*   \ /   \                                                           */ \
   /*    1-----3  3 = Y coordinate                                        */ \
   /*                                                                     */ \
   /* Odd strips are flipped vertically.                                  */ \
   \
   "   ivec2 pos = ivec2(vtx * top_edge_tri_length / 2, " \
   "                     ((vtx % 2) ^ (strip_row % 2)) * tri_height); \n" \
   "\n" \
   \
   /* Generate a mesh of strips. */ \
   "   pos.x += strip_index_in_row * strip_width; \n" \
   "   pos.y += strip_row * tri_height; \n" \
   "\n" \
   \
   /* Generate meshes placed in the Z order curve on the screen. */ \
   "   for (int i = 1; mesh_index != 0; i *= 2) { \n" \
   "       pos.x += (mesh_index & 0x1) * i * top_edge_mesh_length; \n" \
   "       mesh_index >>= 1; \n" \
   "\n" \
   "       pos.y += (mesh_index & 0x1) * i * mesh_height; \n" \
   "       mesh_index >>= 1; \n" \
   "   } \n" \
   "\n" \
   \
   /* Center the geometry. */ \
   "   pos -= ivec2(top_edge_geom_length, geom_height) / 2 + ivec2(1, 0); \n" \
   "\n" \
   \
   /* Convert to clip space. Since it's rotated, we need to make it larger, so that it always */ \
   /* fills the whole screen. */ \
   "   vec2 clip_pos = vec2(pos) / float(min(top_edge_geom_length, geom_height) / 2) * 1.7; \n" \
   "\n" \
   \
   /* Rotate the geometry. */ \
   "   clip_pos = rotate(clip_pos, 45); \n" \
   "\n" \
   "   gl_Position = vec4(clip_pos, 0, 1); \n" \
   "\n" \
   \
   "   if (rows_per_mesh == 0) { \n" \
   "      if (quads_per_row == 0) { \n" \
   "      " VS_SET_POSITION("0") \
   "      } else if (quads_per_row == 1) { \n" \
   "         gl_Position = vec4((gl_VertexIndex % 2) * 2 - 1, \n" \
   "                            (gl_VertexIndex / 2) * 2 - 1, 0, 1); \n" \
   "      } \n" \
   "   } \n" \
   "\n" \
   \
   "   out0 = (vtx % 3) / 2.0; \n" \
   "} \n"

#define FS_RASTER \
   "layout(location = 0) in float in0; \n" \
   "\n" \
   \
   "void main() { \n" \
   "   store_output_color0(vec4(vec3(in0), 1)); \n" \
   "\n" \
   \
   "#if GATHER_TOTAL_FS_INVOC \n" \
   "   if (isFirstNonHelperInvocationInQuad()) \n" \
   "      atomicAdd(num_invocations, 4); \n" \
   "#endif \n" \
   "} \n"

#define RASTER(non_helper_percentage, quads_per_row, rows_per_mesh, num_meshes_x) \
   {NAME(".raster" #non_helper_percentage), \
    VS_RASTER(quads_per_row, rows_per_mesh, num_meshes_x), \
    FS_RASTER, \
    0, /* reserved_for_static_assert */ \
    {quads_per_row, rows_per_mesh, num_meshes_x}}

/* For gathering the number of FS invocations, including helper invocations, which pipeline
 * statistics don't count. Subgroup ops are needed for that.
 */
#define RASTER_GATHER_TOTAL_FS_INVOC (ctx->has_shader_subgroup_ops && getenv("g"))

#define RASTER_QUADS_PER_ROW_MEANS_FULLSCREEN_TRIANGLE 0
#define RASTER_QUADS_PER_ROW_MEANS_FULLSCREEN_QUAD     1

static const pipeline_info pipelines[] = {
   {NAME(".fs_empty"),
    VS_POS_ONLY,
    FS_EMPTY(0)},

   /* Discard. */
   {NAME(".fs_discard"),
    VS_POS_ONLY,
    FS_DISCARD(0)},

   {NAME(".fs_discard.helper_invoc"),
    VS_POS_ONLY,
    FS_DISCARD(1)},

   ZTESTS_NO_ZWRITE(0), /* helper_invoc=0 */
   ZTESTS(0, 0), /* zwrite=0, helper_invoc=0 */
   ZTESTS(1, 0), /* zwrite=1, helper_invoc=0 */
   ZTESTS_NO_ZWRITE(1), /* helper_invoc=1 */
   ZTESTS(0, 1), /* zwrite=0, helper_invoc=1 */
   ZTESTS(1, 1), /* zwrite=1, helper_invoc=1 */

   WRITE_COLOR_CONST(0), /* helper_invoc=0 */
   WRITE_COLOR_CONST(1), /* helper_invoc=1 */

   WRITE_COLOR_Z_SAMPLEMASK_A2C(0), /* helper_invoc=0 */
   WRITE_COLOR_Z_SAMPLEMASK_A2C(1), /* helper_invoc=1 */

   VRS(0), /* helper_invoc=0 */
   VRS(1), /* helper_invoc=1 */

   /* To gather statistics on AMD (they are incorrect by default due to using clip_invocation instead of clip_primitives):
    *    AMD_DEBUG=nggc,mono g=1 build/gpu-ratemeter -lean -subset=1 -filter=raster gl.pix
    *
    * There must be a multiple of 7 quads (14 triangles, 16 vertices) per row unless there is only
    * 1 row. If there are multiple meshes, the number of rows per mesh must be even. The number of
    * meshes in the X axis must be a power of two, and a non-zero number of meshes also indiates
    * that it's a raster subest.
    *
    * Triangle and quad tests should set rows_per_mesh=0, and then quads_per_row identfies
    * the subest.
    *
    * (non-helper FS invocation percentage, quads_per_row, rows_per_mesh, num_meshs_x)
    */
   RASTER(100.fullscreen_triangle, RASTER_QUADS_PER_ROW_MEANS_FULLSCREEN_TRIANGLE, 0, 1),
   RASTER(99.8.fullscreen_quad, RASTER_QUADS_PER_ROW_MEANS_FULLSCREEN_QUAD, 0, 1),
   RASTER(99.9, 1, 1, 1),
   RASTER(99.6, 7, 2, 1),
   RASTER(99.4, 7, 3, 1),
   RASTER(99.1, 7, 5, 1),
   RASTER(98.7, 7, 7, 1),
   RASTER(98.0, 14, 10, 1),
   RASTER(97.2, 14, 14, 1),
   RASTER(96.3, 14, 20, 1),
   RASTER(94.6, 21, 30, 1),
   RASTER(93.0, 14, 20, 2),
   RASTER(89.8, 21, 30, 2),
   RASTER(84.1, 35, 50, 2),
   RASTER(81.5, 21, 30, 4),
   RASTER(76.8, 14, 20, 8),
   RASTER(72.5, 35, 50, 4),
   RASTER(68.8, 21, 30, 8),
   RASTER(62.3, 14, 20, 16),
   RASTER(57.0, 35, 50, 8),
   RASTER(52.5, 21, 30, 16),
   RASTER(45.3, 14, 20, 32),
   RASTER(39.9, 35, 50, 16),

   /* Constant fill. */
   INPUTS(0, "", "", 0, "", ""),

   /* Inputs */
   INPUTS1("flat",         "flat"),
   INPUTS1("persp",        ""),
   INPUTS1("persp_sample", "sample"),

   INPUTS2("persp",        "",       "centroid", "centroid"),
   INPUTS2("persp",        "",       "linear",   "noperspective"),
   INPUTS2("persp_sample", "sample", "linear",   "noperspective sample"),
};

typedef struct {
   const char *name;
   VkFormat format;
   bool lean;
} format_info;

static const format_info formats[] = {
   {"imgStore", 0, true},

   {"R8", VK_FORMAT_R8_UNORM},
   {"R8S", VK_FORMAT_R8_SNORM},
   {"R8U", VK_FORMAT_R8_UINT},
   {"R8I", VK_FORMAT_R8_SINT},

   {"RG8", VK_FORMAT_R8G8_UNORM},
   {"RG8S", VK_FORMAT_R8G8_SNORM},
   {"RG8U", VK_FORMAT_R8G8_UINT},
   {"RG8I", VK_FORMAT_R8G8_SINT},

   {"R16F", VK_FORMAT_R16_SFLOAT},
   {"R16U", VK_FORMAT_R16_UINT},
   {"R16I", VK_FORMAT_R16_SINT},
   {"R16", VK_FORMAT_R16_UNORM},
   {"R16S", VK_FORMAT_R16_SNORM},

   {"RGBA4", VK_FORMAT_R4G4B4A4_UNORM_PACK16},
   {"RGB5A1", VK_FORMAT_R5G5B5A1_UNORM_PACK16},
   {"RGB565", VK_FORMAT_R5G6B5_UNORM_PACK16},

   {"RGBA8", VK_FORMAT_R8G8B8A8_UNORM, true},
   {"RGBA8S", VK_FORMAT_R8G8B8A8_SNORM, true},
   {"RGBA8U", VK_FORMAT_R8G8B8A8_UINT, true},
   {"RGBA8I", VK_FORMAT_R8G8B8A8_SINT, true},

   {"RGB10A2U", VK_FORMAT_A2B10G10R10_UINT_PACK32},
   {"RGB10A2", VK_FORMAT_A2B10G10R10_UNORM_PACK32},

   {"R11GB10F", VK_FORMAT_B10G11R11_UFLOAT_PACK32, true},
   {"RGB9E5", VK_FORMAT_E5B9G9R9_UFLOAT_PACK32},

   {"RG16F", VK_FORMAT_R16G16_SFLOAT, true},
   {"RG16U", VK_FORMAT_R16G16_UINT, true},
   {"RG16I", VK_FORMAT_R16G16_SINT, true},
   {"RG16", VK_FORMAT_R16G16_UNORM, true},
   {"RG16S", VK_FORMAT_R16G16_SNORM, true},

   {"R32F", VK_FORMAT_R32_SFLOAT, true},
   {"R32U", VK_FORMAT_R32_UINT, true},
   {"R32I", VK_FORMAT_R32_SINT, true},

   {"RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT, true},
   {"RGBA16U", VK_FORMAT_R16G16B16A16_UINT, true},
   {"RGBA16I", VK_FORMAT_R16G16B16A16_SINT, true},
   {"RGBA16", VK_FORMAT_R16G16B16A16_UNORM, true},
   {"RGBA16S", VK_FORMAT_R16G16B16A16_SNORM, true},

   {"RG32F", VK_FORMAT_R32G32_SFLOAT, true},
   {"RG32U", VK_FORMAT_R32G32_UINT, true},
   {"RG32I", VK_FORMAT_R32G32_SINT, true},

   {"RGBA32F", VK_FORMAT_R32G32B32A32_SFLOAT, true},
   {"RGBA32U", VK_FORMAT_R32G32B32A32_UINT, true},
   {"RGBA32I", VK_FORMAT_R32G32B32A32_SINT, true},
};

typedef struct {
   api_shader *vs;
   api_shader *fs_out_float;
   api_shader *fs_out_int;
   api_shader *fs_out_uint;
   api_shader *fs_image_store;
   api_shader *fs_count_total_invoc; /* the color output is float */
} compiled_shaders_state;

typedef struct {
   bool skip;
   api_image *colorbuf;
   api_image *colorbuf_raster;
   api_image *zbuf;
   api_framebuffer *fb_color_only; /* there is no color buffer if the format is "imgStore" */
   api_framebuffer *fb_color_only_raster;
   api_framebuffer *fb_color_and_zbuf;
   api_gfx_pipeline *pipelines[ARRAY_SIZE(pipelines)];
   api_gfx_pipeline *pipelines_quad_count[ARRAY_SIZE(pipelines)];
} fb_pipelines;

typedef enum {
   TEST_NORMAL,
   TEST_MULTIVIEW,
   TEST_IMAGE_3D,
   TEST_LINEAR,
} test_flavor;

static void
get_pipeline_name_prefix(char *out, unsigned max_out_length, unsigned samples, test_flavor test_flavor)
{
   assert(test_flavor == TEST_NORMAL || samples == 1);

   switch (test_flavor) {
   case TEST_NORMAL:
      if (samples > 1)
         snprintf(out, max_out_length, "msaa%u", samples);
      else
         snprintf(out, max_out_length, "noaa");
      break;
   case TEST_MULTIVIEW:
      snprintf(out, max_out_length, "multiview");
      break;
   case TEST_IMAGE_3D:
      snprintf(out, max_out_length, "image3d");
      break;
   case TEST_LINEAR:
      snprintf(out, max_out_length, "linear");
      break;
   default:
      error("pix: unexpected test_flavor");
   }
}

static void
get_pipeline_name(char *out, unsigned max_out_length, unsigned samples, test_flavor test_flavor,
                  const pipeline_info *pipeline)
{
   char prefix[16];

   get_pipeline_name_prefix(prefix, sizeof(prefix), samples, test_flavor);

   snprintf(out, max_out_length, ".%s%s%s%s%s%s%s", prefix, pipeline->name1, pipeline->name2,
            pipeline->name3, pipeline->name4, pipeline->name5, pipeline->name6);
}

static bool
test_filter(api_context *ctx, unsigned samples, test_flavor test_flavor, const pipeline_info *pipeline)
{
   char pipeline_name[256];
   get_pipeline_name(pipeline_name, sizeof(pipeline_name), samples, test_flavor, pipeline);

   return check_filter_string(ctx->options.filter, pipeline_name);
}

static void
run_test_pix(api_context *ctx, const char *test_name, unsigned samples,
             compiled_shaders_state *compiled_shaders, api_descriptor_set *desc_set,
             test_flavor test_flavor, api_buffer *total_fs_invoc, api_buffer *raster_indexbuf)
{
   fb_pipelines fbs[ARRAY_SIZE(formats)] = {0};
   api_gfx_pipeline_desc pipeline_descs[ARRAY_SIZE(pipelines)] = {0};
   bool skip_pipeline[ARRAY_SIZE(pipelines)] = {0};
   unsigned num_pipelines = 0;
   const bool multiview = test_flavor == TEST_MULTIVIEW;
   const bool raster_gather_total_fs_invoc = RASTER_GATHER_TOTAL_FS_INVOC;
   const bool raster_dump_image = getenv("d") != NULL;

   for (unsigned p = 0; p < ARRAY_SIZE(pipelines); p++) {
      char pipeline_name[256];
      get_pipeline_name(pipeline_name, sizeof(pipeline_name), samples, test_flavor, &pipelines[p]);

      /* The pipeline name determines the states. */
      bool colormask0 = strstr(pipeline_name, ".colormask=0");
      bool colormask_x = strstr(pipeline_name, ".colormask=x");
      bool sample_shading = strstr(pipeline_name, "_sample") || /* per-sample interpolation */
                            strstr(pipeline_name, "sampleid") ||
                            strstr(pipeline_name, "samplepos");
      bool cull_back = strstr(pipeline_name, ".cull_back");

      if (!pipelines[p].fs_source ||
          !test_filter(ctx, samples, test_flavor, &pipelines[p]) ||
          (!multiview && strstr(pipeline_name, ".view_index")) ||
          (multiview && strstr(pipeline_name, ".layer")) ||
          (sample_shading && samples == 1 && !strstr(pipeline_name, "1persp_sample"))) {
         skip_pipeline[p] = true;
         continue;
      }

      /* For bandwidth tests, skip those that use a Z buffer or don't write a color buffer. */
      if (ctx->options.report_bandwidth &&
          (strstr(pipeline_name, "fs_empty") ||
           strstr(pipeline_name, "fs_discard") ||
           strstr(pipeline_name, "blend_src_color0") ||
           strstr(pipeline_name, "blend_src_alpha0") ||
           strstr(pipeline_name, "zbuf") ||
           colormask0 ||
           (strstr(pipeline_name, "output") && !strstr(pipeline_name, "color")))) {
         skip_pipeline[p] = true;
         continue;
      }

      /* Skip most subtests for image 3D and linear. */
      if ((test_flavor == TEST_IMAGE_3D || test_flavor == TEST_LINEAR) &&
          (strstr(pipeline_name, "vrs") ||
           strstr(pipeline_name, "raster") ||
           strstr(pipeline_name, "zbuf") ||
           strstr(pipeline_name, "output") ||
           strstr(pipeline_name, "face") ||
           strstr(pipeline_name, "fully_covered") ||
           strstr(pipeline_name, "fragpos") ||
           strstr(pipeline_name, "layer") ||
           strstr(pipeline_name, "primitive_id") ||
           strstr(pipeline_name, "sample") ||
           strstr(pipeline_name, "shading_rate") ||
           strstr(pipeline_name, "viewport_index") ||
           strstr(pipeline_name, "centroid") ||
           strstr(pipeline_name, "1linear") ||
           (strstr(pipeline_name, "flat") && !strstr(pipeline_name, "1flat")) ||
           (strstr(pipeline_name, "persp") && !strstr(pipeline_name, "1persp")))) {
         skip_pipeline[p] = true;
         continue;
      }

      assert((intptr_t)compiled_shaders[p].vs != 2);

      pipeline_descs[p] = (api_gfx_pipeline_desc){
         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
         .cull_mode = cull_back ? VK_CULL_MODE_BACK_BIT : 0,
         .vs = compiled_shaders[p].vs,

         .vrs_fragment_size = {1, 1},
         .sample_shading = sample_shading,
         .samplemask = (1 << samples) - 1,
         .depth_enabled = strstr(pipeline_name, "zbuf") && !strstr(pipeline_name, "z_disabled"),
         .depth_write_enabled = strstr(pipeline_name, ".zwrite"),
         .depth_compare_op = strstr(pipeline_name, ".ztest_never") ? VK_COMPARE_OP_NEVER :
                             strstr(pipeline_name, ".ztest_less") ? VK_COMPARE_OP_LESS :
                             strstr(pipeline_name, ".ztest_equal") ? VK_COMPARE_OP_EQUAL :
                             strstr(pipeline_name, ".ztest_notequal") ? VK_COMPARE_OP_NOT_EQUAL :
                                                                        VK_COMPARE_OP_ALWAYS,
         .alpha_to_coverage = strstr(pipeline_name, "a2c"),
         .colormask = colormask0 ? 0 : colormask_x ? 0x1 : 0xf,
         .blend_src_color = strstr(pipeline_name, "blend_src_color"),
         .blend_src_alpha = strstr(pipeline_name, "blend_src_alpha"),
      };

      if (strstr(pipeline_name, "vrs")) {
         if (strstr(pipeline_name, "vrs1x2")) {
            pipeline_descs[p].vrs_fragment_size[1] = 2;
         } else if (strstr(pipeline_name, "vrs2x1")) {
            pipeline_descs[p].vrs_fragment_size[0] = 2;
         } else if (strstr(pipeline_name, "vrs2x2")) {
            pipeline_descs[p].vrs_fragment_size[0] = 2;
            pipeline_descs[p].vrs_fragment_size[1] = 2;
         } else {
            error("unexpected VRS test");
         }
      }

      num_pipelines++;
   }

   if (!num_pipelines)
      return;

   unsigned num_formats = 0;
   for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
      assert(!formats[f].format || format_is_valid(formats[f].format));
      assert(formats[f].format < ARRAY_SIZE(ctx->fb_format_sample_count_support));

      if ((ctx->options.lean && !formats[f].lean) ||
          (ctx->options.report_bandwidth && !formats[f].format) ||
          !(ctx->fb_format_sample_count_support[formats[f].format] & samples) ||
          !check_filter_string(ctx->options.format, formats[f].name)) {
         fbs[f].skip = true;
         continue;
      }

      num_formats++;
   }

   char prefix[16];
   get_pipeline_name_prefix(prefix, sizeof(prefix), samples, test_flavor);

   bool na = (test_flavor == TEST_MULTIVIEW && !ctx->has_multiview) ||
             (test_flavor == TEST_LINEAR && !ctx->has_image_tiling_linear);

   /* Create framebuffers. */
   for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
      if (fbs[f].skip)
         continue;

      if (na)
         continue;

      VkFormat format = formats[f].format;

      unsigned fb_size = 1024;
      unsigned fb_raster_size = fb_size; /* raster tests use the same size for all formats */
      unsigned pix_size = format ? get_pixel_size_from_format(format) : 0;

      /* Scale the framebuffer size up or down depending on bpp and samples to normalize
       * execution time.
       */
      if (samples >= 8)
         fb_size /= 4;
      else if (samples >= 4)
         fb_size /= 2;

      if (pix_size == 16)
         fb_size /= 2;

      VkImageType image_type = test_flavor == TEST_IMAGE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      VkImageTiling layout = test_flavor == TEST_LINEAR ? VK_IMAGE_TILING_LINEAR
                                                        : VK_IMAGE_TILING_OPTIMAL;
      const unsigned num_layers = test_flavor == TEST_IMAGE_3D ? 8 : (multiview ? 2 : 1);
      const unsigned view_mask = multiview ? 0x3 : 0x1;

      if (format) {
         fbs[f].colorbuf = ctx->create_image(ctx, image_type, format, fb_size,
                                             fb_size, num_layers, samples, layout,
                                             api_heap_device);

         fbs[f].colorbuf_raster = ctx->create_image(ctx, image_type, format, fb_raster_size,
                                                    fb_raster_size, num_layers, samples, layout,
                                                    api_heap_device);
      }

      fbs[f].fb_color_only = ctx->create_framebuffer(ctx, fbs[f].colorbuf, NULL,
                                                     fb_size, fb_size, samples, view_mask);
      fbs[f].fb_color_only_raster = ctx->create_framebuffer(ctx, fbs[f].colorbuf_raster, NULL,
                                                            fb_raster_size, fb_raster_size,
                                                            samples, view_mask);

      if (test_flavor != TEST_IMAGE_3D && test_flavor != TEST_LINEAR) {
         fbs[f].zbuf = ctx->create_image(ctx, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, fb_size,
                                         fb_size, num_layers, samples, VK_IMAGE_TILING_OPTIMAL,
                                         api_heap_device);

         fbs[f].fb_color_and_zbuf = ctx->create_framebuffer(ctx, fbs[f].colorbuf, fbs[f].zbuf,
                                                            fb_size, fb_size, samples, view_mask);
      }
   }

   printf("GPU memory allocated: %u MB\n", ctx->device_mem_usage_mb);
   printf("Building pipelines for %s.%s ...", test_name, prefix);

   /* Create pipelines. */
   unsigned num_visited_pipelines = 0;

#pragma omp parallel for if (ctx->allow_parallel_create_pipeline) collapse(2) schedule(static, 50)
   for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
      for (unsigned p = 0; p < ARRAY_SIZE(pipelines); p++) {
         if (skip_pipeline[p] || fbs[f].skip)
            continue;

         print_progress(num_pipelines * num_formats, &num_visited_pipelines, 20);

         if (na)
            continue;

         VkFormat format = formats[f].format;
         api_gfx_pipeline_desc pipeline_desc = pipeline_descs[p];

         char pipeline_name[256];
         get_pipeline_name(pipeline_name, sizeof(pipeline_name), samples, test_flavor,
                           &pipelines[p]);

         /* The pipeline name determines the states. */
         bool helper_invoc = strstr(pipeline_name, "helper_invoc");
         bool a2c = strstr(pipeline_name, "a2c");
         bool colormask0 = strstr(pipeline_name, ".colormask=0");
         bool colormask_x = strstr(pipeline_name, ".colormask=x");
         bool blend = strstr(pipeline_name, "blend");
         bool shading_rate = strstr(pipeline_name, "shading_rate");
         bool fully_covered = strstr(pipeline_name, "fully_covered");
         bool raster = pipelines[p].raster.num_meshes_x != 0;

         if (!format && (helper_invoc || a2c || colormask0 || colormask_x || blend))
            continue;

         if (format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && colormask_x)
            continue;

         if (colormask_x && format_get_num_channels(format) == 1)
            continue;

         if ((pipeline_desc.blend_src_color || pipeline_desc.blend_src_alpha ||
              pipeline_desc.alpha_to_coverage) &&
             format_is_integer(format))
            continue;

         if (!ctx->has_vrs &&
             (pipeline_desc.vrs_fragment_size[0] > 1 ||
              pipeline_desc.vrs_fragment_size[1] > 1 || shading_rate))
            continue;

         if (!ctx->has_fully_covered && fully_covered)
            continue;

         bool require_zbuf = strstr(pipeline_name, "zbuf");

         if (format) {
            if (format_is_sint(format))
               pipeline_desc.fs = compiled_shaders[p].fs_out_int;
            else if (format_is_integer(format))
               pipeline_desc.fs = compiled_shaders[p].fs_out_uint;
            else
               pipeline_desc.fs = compiled_shaders[p].fs_out_float;
         } else {
            pipeline_desc.fs = compiled_shaders[p].fs_image_store;
         }

         pipeline_desc.desc_set_layout = format ? NULL : desc_set->layout;
         assert(!require_zbuf || !raster);
         pipeline_desc.fb = require_zbuf ? fbs[f].fb_color_and_zbuf :
                            raster ? fbs[f].fb_color_only_raster : fbs[f].fb_color_only;

         assert(pipeline_desc.vs);
         assert(pipeline_desc.fs);
         assert(pipeline_desc.fb);

         fbs[f].pipelines[p] = ctx->create_gfx_pipeline(ctx, &pipeline_desc);

         if (raster_gather_total_fs_invoc && raster && format && !format_is_integer(format)) {
            pipeline_desc.desc_set_layout = desc_set->layout;
            pipeline_desc.fs = compiled_shaders[p].fs_count_total_invoc;
            fbs[f].pipelines_quad_count[p] = ctx->create_gfx_pipeline(ctx, &pipeline_desc);
         }
      }
   }
   puts("");
   assert(num_visited_pipelines <= num_pipelines * num_formats);

   /* Create timestamp queries. */
   api_query_pool *timestamps =
      ctx->create_query_pool(ctx, num_pipelines * num_formats * 2, api_query_timestamp);
   api_query_pool *pipe_stats = NULL;

   const char *raster_names[30] = {0};
   if (raster_gather_total_fs_invoc)
      pipe_stats = ctx->create_query_pool(ctx, 30, api_query_pipeline_statistics);

   printf("Executing tests ...");

   bool raster_dumped_images = false;

   /* Run tests. */
   num_visited_pipelines = 0;
   unsigned num_raster_gather_tests = 0;

   for (unsigned p = 0; p < ARRAY_SIZE(pipelines); p++) {
      if (skip_pipeline[p])
         continue;

      bool raster_gathered_total_fs_invoc = false;

      for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
         if (!fbs[f].pipelines[p])
            continue;

         const unsigned num_fullscreen_draws = NUM_FULLSCREEN_DRAWS / (multiview ? 2 : 1);

         const bool raster = pipelines[p].raster.num_meshes_x != 0;
         const unsigned num_raster_vertices =
               !pipelines[p].raster.rows_per_mesh ?
                  (pipelines[p].raster.quads_per_row == RASTER_QUADS_PER_ROW_MEANS_FULLSCREEN_TRIANGLE ? 3 :
                   pipelines[p].raster.quads_per_row == RASTER_QUADS_PER_ROW_MEANS_FULLSCREEN_QUAD ? 6 : 0) :
               6 * pipelines[p].raster.quads_per_row *
               pipelines[p].raster.rows_per_mesh *
               pipelines[p].raster.num_meshes_x * pipelines[p].raster.num_meshes_x;
         const unsigned num_raster_instances = num_fullscreen_draws;

         assert(pipelines[p].raster.rows_per_mesh <= 1 ||
                pipelines[p].raster.quads_per_row % 7 == 0);
         assert(pipelines[p].raster.num_meshes_x <= 1 ||
                pipelines[p].raster.rows_per_mesh % 2 == 0);
         assert(IS_POT(pipelines[p].raster.num_meshes_x));

         const unsigned num_normal_vertices = 3 * num_fullscreen_draws;
         const unsigned num_normal_instances = 1;

         const unsigned num_warmup_vertices = raster ? num_raster_vertices : num_normal_vertices / 4;
         const unsigned num_warmup_instances = raster ? num_raster_instances / 4 : num_normal_instances;

         assert(num_raster_vertices * 4 < raster_indexbuf->size);

         if (raster && raster_gather_total_fs_invoc && !raster_gathered_total_fs_invoc &&
             fbs[f].colorbuf_raster && !format_is_integer(fbs[f].colorbuf_raster->format)) {
            ctx->wait_for_idle(ctx);
            ctx->set_storage_buffer_descriptor(ctx, desc_set, 0, total_fs_invoc,
                                               num_raster_gather_tests * 4, 4);

            num_raster_gather_tests++;

            ctx->begin_cmdbuf(ctx, api_queue_gfx);
            ctx->bind_descriptor_set(ctx, desc_set);
            ctx->bind_index_buffer(ctx, raster_indexbuf);
            ctx->bind_gfx_pipeline(ctx, fbs[f].pipelines_quad_count[p]);
            ctx->begin_next_query(ctx, pipe_stats);
            ctx->begin_render_pass(ctx, &(api_render_pass_desc){
                                      .fb = fbs[f].pipelines[p]->desc.fb,
                                   });
            ctx->draw(ctx, &(api_draw_desc){.indexed = true,
                                            .count = num_raster_vertices,
                                            .instance_count = 1});
            ctx->end_render_pass(ctx);
            ctx->end_next_query(ctx, pipe_stats);
            ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);

            raster_names[pipe_stats->num_written_queries - 1] = pipelines[p].name1;
            raster_gathered_total_fs_invoc = true;
         }

         ctx->begin_cmdbuf(ctx, api_queue_gfx);

         /* Bind states for the command buffer. */
         if (!formats[f].format)
            ctx->bind_descriptor_set(ctx, desc_set);
         if (raster)
            ctx->bind_index_buffer(ctx, raster_indexbuf);
         ctx->bind_gfx_pipeline(ctx, fbs[f].pipelines[p]);

         /* Render pass for the warm-up. */
         ctx->begin_render_pass(ctx, &(api_render_pass_desc){
                                   .fb = fbs[f].pipelines[p]->desc.fb,
                                   .clear = true,
                                   .color_clear_value.float32 = {1, 0, 0, 1},
                                   .depth_clear_value = 0.5,
                                });

         /* Warm up the GPU. */
         ctx->draw(ctx, &(api_draw_desc){.indexed = raster,
                                         .count = num_warmup_vertices,
                                         .instance_count = num_warmup_instances});
         ctx->end_render_pass(ctx);
         ctx->driver_workaround(ctx, WA_RDNA4_TIMESTAMP_BUG);

         ctx->write_next_query_value(ctx, timestamps);
         /* Render pass for the measurement. */
         ctx->begin_render_pass(ctx, &(api_render_pass_desc){
                                   .fb = fbs[f].pipelines[p]->desc.fb,
                                });
         ctx->draw(ctx, &(api_draw_desc){
                      .indexed = raster,
                      .count = raster ? num_raster_vertices : num_normal_vertices,
                      .instance_count = raster ? num_raster_instances : num_normal_instances,
                      .first_vertex = raster ? 0 : num_warmup_vertices});
         ctx->end_render_pass(ctx);
         ctx->write_next_query_value(ctx, timestamps);
         ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);

         if (raster && raster_dump_image && !raster_dumped_images && fbs[f].colorbuf_raster) {
            ctx->image_write_png(ctx, fbs[f].colorbuf_raster, 0, "raster.png");
            raster_dumped_images = true;
         }

         print_progress(num_pipelines * num_formats, &num_visited_pipelines, 20);
      }
   }
   assert(num_visited_pipelines <= num_pipelines * num_formats);
   puts("");

   ctx->get_query_results(ctx, timestamps);

   if (raster_gather_total_fs_invoc) {
      uint32_t *total_fs_invoc_results = alloca(4 * num_raster_gather_tests);

      ctx->get_buffer_data(ctx, total_fs_invoc, 0, 4 * num_raster_gather_tests, total_fs_invoc_results);
      ctx->get_query_results(ctx, pipe_stats);

      printf("| Subtest                       | Total FS invoc. | %% non-helper invoc. | Visible tris | Avg. tri area | Avg. pixels / tri | Avg. FS invoc. / tri |\n");
      printf("|-------------------------------|-----------------|---------------------|--------------|---------------|-------------------|----------------------|\n");

      for (unsigned i = 0; i < num_raster_gather_tests; i++) {
         unsigned total_fs_invoc = total_fs_invoc_results[i];
         api_pipeline_stat_results *stats = &pipe_stats->pipe_stats[i];

         /* Note: This is only correct with radeonsi and AMD_DEBUG=nggc,mono because
          * clip_primitives is wrong RDNA 4. All other HW should use clip_primitives here,
          * but this stat gathering is only for test debugging, so it doesn't matter.
          */
         double non_helpers_percent = 100.0 * stats->fs_invocations / total_fs_invoc;
         uint64_t clip_prims = stats->clip_invocations;
         double pix_per_tri = (double)stats->fs_invocations / clip_prims;
         double fs_invoc_per_tri = (double)total_fs_invoc / clip_prims;

         char name[64];
         snprintf(name, sizeof(name), "%s", raster_names[i] + 7);

         if (i) {
            char percent[16];
            snprintf(percent, sizeof(percent), "%.1f", non_helpers_percent);
            memcpy(name, percent, strlen(percent));
         }

         printf("| raster%-23s | %15u | %17.1f %% | %12"PRIu64" | %12.2f² | %17.1f | %20.1f |\n",
                name, total_fs_invoc, non_helpers_percent, clip_prims, sqrt(pix_per_tri),
                pix_per_tri, fs_invoc_per_tri);
      }
   }

   printf("%-87s", "Formats");

   unsigned off = 0;

   for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
      if (fbs[f].skip)
         continue;

      int len = strlen(formats[f].name);

      /* This realigns names at the top of columns after long names make them misaligned.
       * See the placement of commas.
       *
       *    Before:   short,looooooong, shoort,regular,  short,regular,  short,
       *                  n,      n,      n,      n,      n,      n,      n,
       *
       *    After:    short,looooooong,shoort,regular,short,regular,  short,
       *                  n,      n,      n,      n,      n,      n,      n,
       */
      if (len <= 7 - off) {
         len = 7 - off;
         off = 0;
      } else if (len < 7 && off > 0) {
         off -= 7 - len;
      } else if (len > 7) {
         off += len - 7;
      } else {
         assert(len == 7 && off > 0);
      }

      printf(",%*s", len, formats[f].name);
   }
   puts("");

   /* Print results. */
   for (unsigned p = 0; p < ARRAY_SIZE(pipelines); p++) {
      if (skip_pipeline[p])
         continue;

      char pipeline_name[256];
      get_pipeline_name(pipeline_name, sizeof(pipeline_name), samples, test_flavor, &pipelines[p]);

      char name[256];
      snprintf(name, sizeof(name), "%s%s", test_name, pipeline_name);
      printf("%-87s", name);

      bool raster = strstr(pipeline_name, "raster");

      for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
         if (fbs[f].skip)
            continue;

         if (!fbs[f].pipelines[p]) {
            printf(",    n/a");
            continue;
         }

         api_framebuffer *fb = raster ? fbs[f].fb_color_only_raster : fbs[f].fb_color_only;
         uint64_t num_units = (uint64_t)fb->width * fb->height *
                              NUM_FULLSCREEN_DRAWS;
         if (ctx->options.samplerate)
            num_units *= samples;
         if (ctx->options.report_bandwidth)
            num_units *= get_pixel_size_from_format(formats[f].format);

         print_throughput_from_next_timestamps(ctx, timestamps, num_units,
                                               "%7.1f", "%7.0f", "%7s", 30);
      }
      puts("");
   }

   /* Free memory. */
   ctx->wait_for_idle(ctx);

   for (unsigned f = 0; f < ARRAY_SIZE(formats); f++) {
      if (fbs[f].fb_color_only)
         ctx->destroy_framebuffer(ctx, fbs[f].fb_color_only);
      if (fbs[f].fb_color_only_raster)
         ctx->destroy_framebuffer(ctx, fbs[f].fb_color_only_raster);
      if (fbs[f].fb_color_and_zbuf)
         ctx->destroy_framebuffer(ctx, fbs[f].fb_color_and_zbuf);
      if (fbs[f].colorbuf)
         ctx->destroy_image(ctx, fbs[f].colorbuf);
      if (fbs[f].colorbuf_raster)
         ctx->destroy_image(ctx, fbs[f].colorbuf_raster);
      if (fbs[f].zbuf)
         ctx->destroy_image(ctx, fbs[f].zbuf);

      /* Free pipelines. */
      for (unsigned p = 0; p < ARRAY_SIZE(pipelines); p++) {
         if (fbs[f].pipelines[p])
            ctx->destroy_gfx_pipeline(ctx, fbs[f].pipelines[p]);
         if (fbs[f].pipelines_quad_count[p])
            ctx->destroy_gfx_pipeline(ctx, fbs[f].pipelines_quad_count[p]);
      }
   }
}

void
test_pix(api_context *ctx, const char *test_name)
{
   compiled_shaders_state compiled_shaders[ARRAY_SIZE(pipelines)] = {0};
   static const unsigned sample_counts[] = {1, 2, 4, 8};
   const bool raster_gather_total_fs_invoc = RASTER_GATHER_TOTAL_FS_INVOC;

   printf("Units: %s\n",
          ctx->options.report_bandwidth ? "GB/s" :
          ctx->options.max_rate ? (ctx->options.samplerate ? "% of the specified pixel rate, multiplied by the number of MSAA samples" :
                                                             "% of the specified pixel rate") :
          ctx->options.freq_mhz ? (ctx->options.samplerate ? "pixels/clock (no MSAA) or samples/clock (MSAA)" :
                                                             "pixels/clock" ) :
                                  (ctx->options.samplerate ? "billion pixels/second (no MSAA) or billion samples/second (MSAA)" :
                                                             "billion pixels/second"));

   puts("Compiling shaders...");

   /* Compile shaders. */
#pragma omp parallel for if(ctx->allow_parallel_create_shader) schedule(static, 10)
   for (unsigned p = 0; p < ARRAY_SIZE(pipelines); p++) {
      bool match = false;

      for (unsigned s = 0; s < ARRAY_SIZE(sample_counts); s++) {
         if (test_filter(ctx, sample_counts[s], TEST_NORMAL, &pipelines[p]) ||
             (sample_counts[s] == 1 &&
              (test_filter(ctx, 1, TEST_MULTIVIEW, &pipelines[p]) ||
               test_filter(ctx, 1, TEST_IMAGE_3D, &pipelines[p]) ||
               test_filter(ctx, 1, TEST_LINEAR, &pipelines[p])))) {
            match = true;
            break;
         }
      }

      if (!match || !pipelines[p].fs_source ||
          (!ctx->has_fully_covered && strstr(pipelines[p].fs_source, "gl_FragFullyCoveredNV")) ||
          (!ctx->has_multiview && strstr(pipelines[p].fs_source, "gl_ViewIndex")) ||
          (!ctx->has_vrs && strstr(pipelines[p].fs_source, "gl_ShadingRateEXT")))
         continue;

      char vs_code[3 * 1024], fs_code[3 * 1024];
      int len;

      len = snprintf(vs_code, sizeof(vs_code), "%s%s", vs_shared_code, pipelines[p].vs_source);
      assert(len < sizeof(vs_code) - 1);
      len = snprintf(fs_code, sizeof(fs_code), "%s%s", fs_shared_code, pipelines[p].fs_source);
      assert(len < sizeof(fs_code) - 1);

      compiled_shaders[p].vs = ctx->create_shader(ctx, vs_code, api_shader_vs);

      char *fs_has_vrs_define = strstr(fs_code, "#define HAS_VRS 0");
      char *fs_has_fully_covered_define = strstr(fs_code, "#define HAS_FULLY_COVERED 0");
      char *fs_has_multiview_define = strstr(fs_code, "#define HAS_MULTIVIEW 0");

      assert(fs_has_vrs_define);
      fs_has_vrs_define[16] = ctx->has_vrs ? '1' : '0';
      fs_has_fully_covered_define[26] = ctx->has_fully_covered ? '1' : '0';
      fs_has_multiview_define[22] = ctx->has_multiview ? '1' : '0';

      compiled_shaders[p].fs_out_float = ctx->create_shader(ctx, fs_code, api_shader_fs);

      char *fs_image_store_define = strstr(fs_code, "#define IMAGE_STORE 0");
      assert(fs_image_store_define);
      fs_image_store_define[20] = '1'; /* change #define IMAGE_STORE to 1 */
      compiled_shaders[p].fs_image_store = ctx->create_shader(ctx, fs_code, api_shader_fs);
      fs_image_store_define[20] = '0'; /* restore */

      char *fs_output_type_define = strstr(fs_code, "#define FS_OUTPUT_TYPE 0");
      assert(fs_output_type_define);
      fs_output_type_define[23] = '1';
      compiled_shaders[p].fs_out_int = ctx->create_shader(ctx, fs_code, api_shader_fs);
      fs_output_type_define[23] = '0'; /* restore */

      fs_output_type_define[23] = '2';
      compiled_shaders[p].fs_out_uint = ctx->create_shader(ctx, fs_code, api_shader_fs);
      fs_output_type_define[23] = '0'; /* restore */

      if (raster_gather_total_fs_invoc) {
         char *fs_total_invoc_define = strstr(fs_code, "#define GATHER_TOTAL_FS_INVOC 0");

         if (fs_total_invoc_define) {
            fs_total_invoc_define[30] = '1';
            compiled_shaders[p].fs_count_total_invoc = ctx->create_shader(ctx, fs_code,
                                                                          api_shader_fs);
            fs_total_invoc_define[30] = '0'; /* restore */
         }
      }
   }

   /* Create an image for image stores. It's a dummy image because we are not measuring memory
    * throughput for the image store case.
    */
   api_image *store_image = ctx->create_image(ctx, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, 16, 16,
                                              1, 1, VK_IMAGE_TILING_OPTIMAL, api_heap_device);

   /* Create the descriptor layout and the set. */
   api_descriptor_set_layout *desc_set_layout =
      ctx->create_descriptor_set_layout(ctx,
                                        &(api_descriptor_set_layout_desc) {
                                           .storage_buffer[0].array_size = 1,
                                           .storage_buffer[0].vk_binding = 1,
                                           .storage_image[0].array_size = 1,
                                        });
   api_descriptor_set *desc_set = ctx->create_descriptor_set(ctx, desc_set_layout);
   ctx->set_storage_image_descriptors(ctx, desc_set, 0, 1, &store_image);

   /* Create the buffer for gathering FS invocations including helper invocations. */
   api_buffer *total_fs_invoc = NULL;

   if (raster_gather_total_fs_invoc) {
      total_fs_invoc = ctx->create_buffer(ctx, 4 * 30, api_heap_device, 0);

      ctx->begin_cmdbuf(ctx, api_queue_gfx);
      ctx->clear_buffer(ctx, total_fs_invoc, 0, 4, 0);
      ctx->barrier_buffers(ctx, 1, &total_fs_invoc, (uint64_t[]){0, 4}, false);
      ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);
   }


   /* Create the index buffer for the raster test. The vertex data is generated
    * from gl_VertexIndex, but we need an index buffer to force vertex reuse.
    *
    * Each strip has 14 triangles, 16 vertices with reuse, and 42 indices.
    */
   const unsigned raster_max_indices = 14000000;
   const unsigned raster_indexbuf_size = raster_max_indices * 4;
   const unsigned indices_per_quad = 6;
   const unsigned quads_per_strip = 7;
   const unsigned vertices_per_strip = 2 + quads_per_strip * 2;
   assert(vertices_per_strip == 16);
   const unsigned indices_per_strip = quads_per_strip * indices_per_quad;
   assert(indices_per_strip == 42);
   const unsigned max_strips = raster_max_indices / indices_per_strip;

   api_buffer *raster_indexbuf = ctx->create_buffer(ctx, raster_indexbuf_size, api_heap_device, 0);
   uint32_t *indices = malloc(raster_indexbuf_size);

   for (unsigned strip = 0; strip < max_strips; strip++) {
      /* Each quad uses these indices:
       *
       * 0-----2
       *  \   / \
       *   \ /   \
       *    1-----3
       *
       * Face culling must be disabled because the winding flips for odd triangles,
       * but the vertex order allows us to chain the quads to form a strip and it simplifies
       * the vertex position computation for the shader because even vertices are at the top,
       * odd vertices are at the bottom, and VertexIndex / 2 is the column.
       */
      for (unsigned quad = 0; quad < quads_per_strip; quad++) {
         unsigned index_base = strip * indices_per_strip + quad * indices_per_quad;
         unsigned first_vertex = strip * vertices_per_strip + quad * 2;

         assert(index_base + 5 < raster_max_indices);

         indices[index_base + 0] = first_vertex;
         indices[index_base + 1] = first_vertex + 1;
         indices[index_base + 2] = first_vertex + 2;

         indices[index_base + 3] = first_vertex + 1;
         indices[index_base + 4] = first_vertex + 2;
         indices[index_base + 5] = first_vertex + 3;
      }
   }

   ctx->upload_buffer_data(ctx, raster_indexbuf, 0, raster_indexbuf_size, indices);
   free(indices);

   /* Determine the subset to test. */
   int test_flavor = -1;
   int test_samples = -1;

   if (ctx->options.subset) {
      for (unsigned s = 0; s < ARRAY_SIZE(sample_counts); s++) {
         char samples_str[2] = {'0' + sample_counts[s]};

         if (!strcmp(ctx->options.subset, samples_str)) {
            test_flavor = TEST_NORMAL;
            test_samples = sample_counts[s];
            break;
         }
      }

      if (!strcmp(ctx->options.subset, "multiview")) {
         test_flavor = TEST_MULTIVIEW;
         test_samples = 1;
      }

      if (!strcmp(ctx->options.subset, "image3d")) {
         test_flavor = TEST_IMAGE_3D;
         test_samples = 1;
      }

      if (!strcmp(ctx->options.subset, "linear")) {
         test_flavor = TEST_LINEAR;
         test_samples = 1;
      }

      if (test_flavor == -1)
         error("-subset: invalid value");
   }

   /* Run tests. */
   for (unsigned s = 0; s < ARRAY_SIZE(sample_counts); s++) {
      unsigned samples = sample_counts[s];

      if (ctx->supported_color_sample_counts & samples &&
          (test_flavor == -1 || (test_flavor == TEST_NORMAL && test_samples == samples))) {
         run_test_pix(ctx, test_name, samples, compiled_shaders, desc_set, TEST_NORMAL, total_fs_invoc,
                      raster_indexbuf);
      }
   }

   if (test_flavor == -1 || test_flavor == TEST_MULTIVIEW) {
      run_test_pix(ctx, test_name, 1, compiled_shaders, desc_set, TEST_MULTIVIEW, total_fs_invoc,
                   raster_indexbuf);
   }

   if (test_flavor == -1 || test_flavor == TEST_IMAGE_3D) {
      run_test_pix(ctx, test_name, 1, compiled_shaders, desc_set, TEST_IMAGE_3D, total_fs_invoc,
                   raster_indexbuf);
   }

   if (test_flavor == -1 || test_flavor == TEST_LINEAR) {
      run_test_pix(ctx, test_name, 1, compiled_shaders, desc_set, TEST_LINEAR, total_fs_invoc,
                   raster_indexbuf);
   }
}
