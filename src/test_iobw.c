/* Copyright 2024 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

/**
 * Measure shader IO throughput in GB/s. The throughput is measured for:
 * - VS input loads (all VS inputs are read, but all primitives are culled)
 *   * We can't use rasterizer discard because drivers could remove most VS inputs knowing that
 *     FS can't execute.
 * - VS output stores via transform feedback (VS inputs have stride=0, rasterizer discard)
 * - VS output stores into FS (VS inputs have stride=0, execute FS to test FS input loads)
 * - GS output stores into FS (VS inputs have stride=0, execute FS to test FS input loads)
 * - TCS output stores into TES (VS inputs have stride=0, all TES inputs are read, but all primitives are culled)
 *
 * If shader IO is passed via memory, the goal for driver developers is to achieve either
 * the memory throughput or the last level cache throughput.
 */

#include <assert.h>
#include <stdio.h>

#include "common.h"

/* this must be a power of two to prevent precision issues */
#define FB_SIZE 1024

#define MAX_COMPS (16 * 4)
#define MAX_VEC4S (ALIGN_POT(MAX_COMPS, 4) / 4)

#define BO_ALLOC_SIZE		(64 * 1024 * 1024)
#define NUM_VERTICES_PER_ITER	(1000 * 3)
#define MAX_VERTICES_PER_DRAW	(INT32_MAX - 1) /* divisible by 3 */

typedef enum {
   TEST_VS_IN,
   TEST_VS_OUT_XFB,
   TEST_VS_OUT_FS,
   TEST_GS_OUT,
   TEST_TCS_OUT,
   TEST_TCS_PATCH_OUT,
   NUM_TEST_SETS,
} test_type;

/* Declare num_comps components of IO in vec4s except the last one, whose type
 * is the number of components left.
 */
static unsigned
glsl_declare_io(char *code, unsigned offset, unsigned max_size, const char *qual,
                const char *name, unsigned num_comps, bool is_array, bool is_vs_input)
{
   for (unsigned i = 0; i < num_comps; i += 4) {
      const char *type = i + 4 <= num_comps ? "vec4" :
                                              i + 3 <= num_comps ? "vec3" :
                                                                   i + 2 <= num_comps ? "vec2" : "float";

      if (is_vs_input) {
         offset += snprintf(code + offset, max_size - offset,
                            "layout(location=%u) ", i / 4);
      }
      offset += snprintf(code + offset, max_size - offset,
                         "%s %s %s%u%s;\n", qual, type, name, i / 4,
                         is_array ? "[]" : "");
   }
   return offset;
}

static unsigned
generate_gl_position(char *code, unsigned offset, unsigned max_size, const char *prim_id,
                     const char *vertex_id)
{
   /* Spread primitives evenly across the whole screen to utilize the hw.
         *
         * If we drew primitives to only a tiny portion of the screen, they might all go
         * into a single render output unit instead of all render output units.
         * The primitives must also be small enough to test GS throughput instead of FS.
         */
   offset += snprintf(code + offset, max_size - offset,
                      "   int div = 512;\n" /* must be at least 2 and less than FB_SIZE */
                      "   int prim_id = (%s) %% (div * div);\n"
                      "   int vertex_id = (%s);\n"
                      "   float x = float(prim_id %% div) / (div / 2) + float(vertex_id /  2) / (div / 2) - 1;\n"
                      "   float y = float(prim_id /  div) / (div / 2) + float(vertex_id %% 2) / (div / 2) - 1;\n"
                      "   gl_Position = vec4(x, y, 0, 1);\n", prim_id, vertex_id);
   return offset;
}

static unsigned
generate_passthrough_vs(char *vs_code, unsigned max_size, unsigned num_comps, bool before_rasterizer)
{
   unsigned offset;

   offset = snprintf(vs_code, max_size, "#version 400\n");
   offset = glsl_declare_io(vs_code, offset, max_size, "in", "attr", num_comps, false, true);
   offset = glsl_declare_io(vs_code, offset, max_size, "out", "vs_out", num_comps, false, false);
   offset += snprintf(vs_code + offset, max_size - offset,
                      "void main() {\n");

   for (unsigned i = 0; i < num_comps; i += 4) {
      offset += snprintf(vs_code + offset, max_size - offset,
                         "   vs_out%u = attr%u;\n", i / 4, i / 4);
   }

   if (before_rasterizer) {
      offset = generate_gl_position(vs_code, offset, max_size, "gl_VertexID / 3", "gl_VertexID % 3");
   } else {
      offset += snprintf(vs_code + offset, max_size - offset,
                         "   gl_Position = vec4(0, 0, 0, 1);\n");
   }
   offset += snprintf(vs_code + offset, max_size - offset,
                      "}\n");
   return offset;
}

/* Compute a sum of all inputs and put the final result in sum.x.
 *
 * For optimal vectorization and instruction-level parallelism, inputs are first summed up
 * as vec4s, then the result is reduced to a scalar.
 */
static unsigned
generate_add_reduce_code(char *code, unsigned offset, unsigned max_size, unsigned num_comps,
                         const char *input_name, const char *input_indexing)
{
   offset += snprintf(code + offset, max_size - offset,
                      "   vec4 sum = vec4(0)");

   unsigned num_vec4s = ALIGN_POT(num_comps, 4) / 4;
   for (unsigned i = 0; i < num_vec4s; i++) {
      if (i == num_vec4s - 1 && num_comps % 4) {
         offset += snprintf(code + offset, max_size - offset,
                            " + vec4(%s%u%s, %s)", input_name, i, input_indexing,
                            num_comps % 4 == 1 ? "0, 0, 0" :
                                                 num_comps % 4 == 2 ? "0, 0" : "0");
      } else {
         offset += snprintf(code + offset, max_size - offset,
                            " + %s%u%s", input_name, i, input_indexing);
      }
   }
   offset += snprintf(code + offset, max_size - offset,
                      ";\n"
                      "   sum.x += sum.y + sum.z + sum.w;\n");
   return offset;
}

/* Generate a shader that reads N input components and combines them into 1 output component. */
static unsigned
generate_input_reduce_fs(char *code, unsigned max_size, unsigned num_comps,
                         const char *input_name)
{
   unsigned offset;

   offset = snprintf(code, max_size, "#version 400\n");
   offset = glsl_declare_io(code, offset, max_size, "in", input_name, num_comps, false, false);
   offset += snprintf(code + offset, max_size - offset,
                      "out vec4 color;\n"
                      "void main() {\n"
                      "   color = vec4(1);\n");

   unsigned num_vec4s = ALIGN_POT(num_comps, 4) / 4;
   for (unsigned i = 0; i < num_vec4s; i++) {
      if (i == num_vec4s - 1 && num_comps % 4) {
         offset += snprintf(code + offset, max_size - offset,
                            "   color *= vec4(%s%u, %s);\n", input_name, i,
                            num_comps % 4 == 1 ? "1, 1, 1" :
                                                 num_comps % 4 == 2 ? "1, 1" : "1");
      } else {
         offset += snprintf(code + offset, max_size - offset,
                            "   color *= %s%u;\n", input_name, i);
      }
   }
   offset += snprintf(code + offset, max_size - offset,
                      "   color += vec4(1);\n"
                      "}\n");
   return offset;
}

#define NUM_ITERATIONS           100

typedef struct {
   api_framebuffer *fb;
   api_query_pool *timestamps;
} test_state;

static void
run_test(api_context *ctx, test_state *state, test_type test, unsigned max_vertices_per_draw)
{
   const uint64_t num_vertices = NUM_ITERATIONS * (uint64_t)NUM_VERTICES_PER_ITER;
   const unsigned warmup_iterations = NUM_ITERATIONS / 4;

   ctx->begin_render_pass(ctx, &(api_render_pass_desc){.fb = state->fb});

   for (unsigned i = 0; i < warmup_iterations + NUM_ITERATIONS; i++) {
      if (i == warmup_iterations) {
         ctx->end_render_pass(ctx);

         ctx->write_next_query_value(ctx, state->timestamps);
         ctx->begin_render_pass(ctx, &(api_render_pass_desc){.fb = state->fb});
      }

      assert(max_vertices_per_draw % 3 == 0);

      for (uint64_t start = 0; start < num_vertices; start += max_vertices_per_draw) {
         if (test == TEST_VS_OUT_XFB)
            ctx->begin_transform_feedback(ctx);

         ctx->draw(ctx, &(api_draw_desc){
                      .count = MIN2(num_vertices - start, max_vertices_per_draw),
                      .instance_count = 1,
                   });

         if (test == TEST_VS_OUT_XFB)
            ctx->end_transform_feedback(ctx);
      }
   }

   ctx->end_render_pass(ctx);
   ctx->write_next_query_value(ctx, state->timestamps);
}

static void
print_result(api_context *ctx, test_state *state, unsigned vertex_size, test_type test)
{
   double iters_per_sec = perf_measure_gpu_rate(draw, 0.01);
   double verts_per_sec = iters_per_sec * NUM_VERTICES_PER_ITER;
   if (test == TEST_TCS_PATCH_OUT)
      verts_per_sec /= 3;
   double bytes_per_sec = verts_per_sec * vertex_size;
   double GBps = bytes_per_sec / (1024 * 1024 * 1024);
   printf(", %8.2f", GBps);
}

void
test_iobw(api_context *ctx)
{
   /* Indexed by the number of input/output components - 1. */
   api_gfx_pipeline *prog_vs_in[MAX_COMPS] = {0};
   api_gfx_pipeline *prog_vs_out_xfb[MAX_COMPS] = {0};
   api_gfx_pipeline *prog_vs_out_fs[MAX_COMPS] = {0};
   api_gfx_pipeline *prog_gs_out[3][MAX_COMPS] = {0}; /* max_vertices = (first_index + 1) * 3; */
   api_gfx_pipeline *prog_tcs_out[3][MAX_COMPS] = {0}; /* gl_TessLevelOuter[0] = first_index; */
   api_gfx_pipeline *prog_tcs_patch_out[3][MAX_COMPS] = {0}; /* gl_TessLevelOuter[0] = first_index; */

   puts("Compiling shaders...");

   char *dummy_fs_code = "#version 400\n"
                         "void main() {\n"
                         "   gl_FragColor.x = 1;\n"
                         "}\n";
   api_shader *dummy_fs = ctx->create_shader(ctx, dummy_fs_code, api_shader_fs);

   for (test_type test = 0; test < NUM_TEST_SETS; test++) {
      /*if (skip_test)
         continue;*/

      switch (test) {
      case TEST_VS_IN:
         /* VS input loads (all VS inputs are read, but all primitives are culled) */
         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            char vs_code[2048];
            unsigned offset;

            /* Generate the VS. */
            offset = snprintf(vs_code, sizeof(vs_code), "#version 400\n");
            offset = glsl_declare_io(vs_code, offset, sizeof(vs_code), "in", "attr", num_comps, false, true);
            offset += snprintf(vs_code + offset, sizeof(vs_code) - offset,
                               "void main() {\n");
            offset = generate_add_reduce_code(vs_code, offset, sizeof(vs_code), num_comps,
                                              "attr", "");
            offset += snprintf(vs_code + offset, sizeof(vs_code) - offset,
                               "   gl_Position = vec4(sum.x, 0, 0, -1);\n"
                               "}\n");
            assert(offset < sizeof(vs_code));

            api_shader *vs = ctx->create_shader(ctx, vs_code, api_shader_vs);

            prog_vs_in[num_comps - 1] =
               ctx->create_gfx_pipeline(ctx,
                                        &(api_gfx_pipeline_desc){
                                           .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                           .vs = vs,
                                           .fs = dummy_fs,
                                           .depth_enabled = true,
                                           .depth_compare_op = VK_COMPARE_OP_NEVER,
                                        });
         }
         break;

      case TEST_VS_OUT_XFB:
         /* VS output stores via transform feedback (VS inputs have stride=0, rasterizer discard) */
         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            char vs_code[2048];
            unsigned offset;

            /* Generate the VS. */
            offset = generate_passthrough_vs(vs_code, sizeof(vs_code), num_comps, false);
            assert(offset < sizeof(vs_code));

            /* Generate the list of xfb varyings. */
            const char *varying_ptrs[MAX_VEC4S];
            char varyings[MAX_VEC4S][32];

            for (unsigned i = 0; i < MAX_VEC4S; i++) {
               snprintf(varyings[i], sizeof(varyings[i]), "vs_out%u", i);
               varying_ptrs[i] = varyings[i];
            }

            api_shader *vs = ctx->create_shader(ctx, vs_code, api_shader_vs);

            /* TODO: Port this into GLSL xfb_* qualifiers. */
            /*glTransformFeedbackVaryings(prog, (num_comps + 3) / 4, varying_ptrs,
                                        GL_INTERLEAVED_ATTRIBS);*/

            prog_vs_out_xfb[num_comps - 1] =
               ctx->create_gfx_pipeline(ctx,
                                        &(api_gfx_pipeline_desc){
                                           .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                           .vs = vs,
                                           .rasterizer_discard = true,
                                           .fs = dummy_fs
                                        });
         }
         break;

      case TEST_VS_OUT_FS:
         /* VS output stores into FS (VS inputs have stride=0, execute FS to test FS input loads) */
         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            char vs_code[2048], fs_code[2048];
            unsigned offset;

            /* Generate the VS. */
            offset = generate_passthrough_vs(vs_code, sizeof(vs_code), num_comps, true);
            assert(offset < sizeof(vs_code));

            /* Generate the FS. */
            offset = generate_input_reduce_fs(fs_code, sizeof(fs_code), num_comps, "vs_out");
            assert(offset < sizeof(fs_code));

            api_shader *vs = ctx->create_shader(ctx, vs_code, api_shader_vs);
            api_shader *fs = ctx->create_shader(ctx, fs_code, api_shader_fs);

            prog_vs_out_fs[num_comps - 1] =
               ctx->create_gfx_pipeline(ctx,
                                        &(api_gfx_pipeline_desc){
                                           .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                           .vs = vs,
                                           .fs = fs,
                                        });
         }
         break;

      case TEST_GS_OUT:
         /* GS output stores into FS (VS inputs have stride=0, execute FS to test FS input loads) */
         for (unsigned amp_factor = 0; amp_factor < 3; amp_factor++) {
            unsigned max_vertices = (amp_factor + 1) * 3;

            for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
               char vs_code[2048], gs_code[2048], fs_code[2048];
               unsigned offset;

               /* Generate the VS. */
               offset = generate_passthrough_vs(vs_code, sizeof(vs_code), num_comps, false);
               assert(offset < sizeof(vs_code));

               /* Generate the GS. */
               offset = snprintf(gs_code, sizeof(gs_code),
                                 "#version 400\n"
                                 "layout(triangles) in;\n"
                                 "layout(triangle_strip, max_vertices=%u) out;\n", max_vertices);
               offset = glsl_declare_io(gs_code, offset, sizeof(gs_code),
                                        "in", "vs_out", num_comps, true, false);
               offset = glsl_declare_io(gs_code, offset, sizeof(gs_code),
                                        "out", "gs_out", num_comps, false, false);
               offset += snprintf(gs_code + offset, sizeof(gs_code) - offset,
                                  "void main() {\n"
                                  "   for (int i = 0; i < %u; i++) {\n", max_vertices);

               for (unsigned i = 0; i < num_comps; i += 4) {
                  offset += snprintf(gs_code + offset, sizeof(gs_code) - offset,
                                     "      gs_out%u = vs_out%u[min(i, 2)];\n", i / 4, i / 4);
               }

               offset = generate_gl_position(gs_code, offset, sizeof(gs_code), "gl_PrimitiveIDIn", "i");
               offset += snprintf(gs_code + offset, sizeof(gs_code) - offset,
                                  "      EmitVertex();\n"
                                  "   }\n"
                                  "}\n");
               assert(offset < sizeof(gs_code));

               /* Generate the FS. */
               offset = generate_input_reduce_fs(fs_code, sizeof(fs_code), num_comps, "gs_out");
               assert(offset < sizeof(fs_code));

               api_shader *vs = ctx->create_shader(ctx, vs_code, api_shader_vs);
               api_shader *gs = ctx->create_shader(ctx, gs_code, api_shader_gs);
               api_shader *fs = ctx->create_shader(ctx, fs_code, api_shader_fs);

               prog_gs_out[amp_factor][num_comps - 1] =
                  ctx->create_gfx_pipeline(ctx,
                                           &(api_gfx_pipeline_desc){
                                              .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                              .vs = vs,
                                              .gs = gs,
                                              .fs = fs,
                                           });
            }
         }
         break;

      case TEST_TCS_OUT:
         /* TCS output stores into TES (VS inputs have stride=0, all TES inputs are read, but all primitives are culled) */
         for (unsigned tess_level_outer = 0; tess_level_outer < 3; tess_level_outer++) {
            for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
               char vs_code[2048], tcs_code[2048], tes_code[4096];
               unsigned offset;

               /* Generate the VS. */
               offset = generate_passthrough_vs(vs_code, sizeof(vs_code), num_comps, false);
               assert(offset < sizeof(vs_code));

               /* Generate the TCS. */
               offset = snprintf(tcs_code, sizeof(tcs_code),
                                 "#version 400\n"
                                 "layout(vertices=3) out;\n");
               offset = glsl_declare_io(tcs_code, offset, sizeof(tcs_code),
                                        "in", "vs_out", num_comps, true, false);
               offset = glsl_declare_io(tcs_code, offset, sizeof(tcs_code),
                                        "out", "tcs_out", num_comps, true, false);
               offset += snprintf(tcs_code + offset, sizeof(tcs_code) - offset,
                                  "void main() {\n");

               for (unsigned i = 0; i < num_comps; i += 4) {
                  offset += snprintf(tcs_code + offset, sizeof(tcs_code) - offset,
                                     "   tcs_out%u[gl_InvocationID] = vs_out%u[gl_InvocationID];\n",
                                     i / 4, i / 4);
               }

               offset += snprintf(tcs_code + offset, sizeof(tcs_code) - offset,
                                  "   gl_TessLevelOuter = float[4](%u, 1, 1, 1);\n"
                                  "   gl_TessLevelInner[0] = 1;\n"
                                  "}\n", tess_level_outer);
               assert(offset < sizeof(tcs_code));

               /* Generate the TES. */
               offset = snprintf(tes_code, sizeof(tes_code),
                                 "#version 400\n"
                                 "layout(triangles) in;\n");
               offset = glsl_declare_io(tes_code, offset, sizeof(tes_code),
                                        "in", "tcs_out", num_comps, true, false);
               offset += snprintf(tes_code + offset, sizeof(tes_code) - offset,
                                  "void main() {\n"
                                  "   int vertexID = gl_TessCoord.x == 1 ? 0 : gl_TessCoord.y == 1 ? 1 : 2;\n");
               offset = generate_add_reduce_code(tes_code, offset, sizeof(tes_code), num_comps,
                                                 "tcs_out", "[vertexID]");
               offset += snprintf(tes_code + offset, sizeof(tes_code) - offset,
                                  "   gl_Position = vec4(sum.x, 0, 0, -1);\n"
                                  "}\n");
               assert(offset < sizeof(tes_code));

               api_shader *vs = ctx->create_shader(ctx, vs_code, api_shader_vs);
               api_shader *tcs = ctx->create_shader(ctx, tcs_code, api_shader_tcs);
               api_shader *tes = ctx->create_shader(ctx, tes_code, api_shader_tes);

               prog_tcs_out[tess_level_outer][num_comps - 1] =
                  ctx->create_gfx_pipeline(ctx,
                                           &(api_gfx_pipeline_desc){
                                              .topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
                                              .patch_control_points = 3,
                                              .vs = vs,
                                              .tcs = tcs,
                                              .tes = tes,
                                              .fs = dummy_fs,
                                           });
            }
         }
         break;
      case TEST_TCS_PATCH_OUT:
         /* TCS patch output stores into TES (VS inputs have stride=0, all TES inputs are read, but all primitives are culled) */
         for (unsigned tess_level_outer = 0; tess_level_outer < 3; tess_level_outer++) {
            for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
               char vs_code[2048], tcs_code[2048], tes_code[4096];
               unsigned offset;

               /* Generate the VS. */
               offset = generate_passthrough_vs(vs_code, sizeof(vs_code), num_comps, false);
               assert(offset < sizeof(vs_code));

               /* Generate the TCS. */
               offset = snprintf(tcs_code, sizeof(tcs_code),
                                 "#version 400\n"
                                 "layout(vertices=3) out;\n");
               offset = glsl_declare_io(tcs_code, offset, sizeof(tcs_code),
                                        "in", "vs_out", num_comps, true, false);
               offset = glsl_declare_io(tcs_code, offset, sizeof(tcs_code),
                                        "patch out", "tcs_out", num_comps, false, false);
               offset += snprintf(tcs_code + offset, sizeof(tcs_code) - offset,
                                  "void main() {\n");

               for (unsigned i = 0; i < num_comps; i += 4) {
                  offset += snprintf(tcs_code + offset, sizeof(tcs_code) - offset,
                                     "   tcs_out%u = vs_out%u[0];\n",
                                     i / 4, i / 4);
               }

               offset += snprintf(tcs_code + offset, sizeof(tcs_code) - offset,
                                  "   gl_TessLevelOuter = float[4](%u, 1, 1, 1);\n"
                                  "   gl_TessLevelInner[0] = 1;\n"
                                  "}\n", tess_level_outer);
               assert(offset < sizeof(tcs_code));

               /* Generate the TES. */
               offset = snprintf(tes_code, sizeof(tes_code),
                                 "#version 400\n"
                                 "layout(triangles) in;\n");
               offset = glsl_declare_io(tes_code, offset, sizeof(tes_code),
                                        "patch in", "tcs_out", num_comps, false, false);
               offset += snprintf(tes_code + offset, sizeof(tes_code) - offset,
                                  "void main() {\n");
               offset = generate_add_reduce_code(tes_code, offset, sizeof(tes_code), num_comps,
                                                 "tcs_out", "");
               offset += snprintf(tes_code + offset, sizeof(tes_code) - offset,
                                  "   gl_Position = vec4(sum.x, 0, 0, -1);\n"
                                  "}\n");
               assert(offset < sizeof(tes_code));

               api_shader *vs = ctx->create_shader(ctx, vs_code, api_shader_vs);
               api_shader *tcs = ctx->create_shader(ctx, tcs_code, api_shader_tcs);
               api_shader *tes = ctx->create_shader(ctx, tes_code, api_shader_tes);

               prog_tcs_patch_out[tess_level_outer][num_comps - 1] =
                  ctx->create_gfx_pipeline(ctx,
                                           &(api_gfx_pipeline_desc){
                                              .topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
                                              .patch_control_points = 3,
                                              .vs = vs,
                                              .tcs = tcs,
                                              .tes = tes,
                                              .fs = dummy_fs,
                                           });
            }
         }
         break;
      default:
         error("invalid case");
      }
   }

   puts("");
   puts("All numbers are in GB/s.");
   puts("");

   /*****************
    * Run the test.
    *****************/

   GLuint vao;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   static float zeroed[4];
   GLuint buf_single_vertex;
   glGenBuffers(1, &buf_single_vertex);
   glBindBuffer(GL_ARRAY_BUFFER, buf_single_vertex);
   glBufferData(GL_ARRAY_BUFFER, 16, zeroed, GL_STATIC_DRAW);
   glBindBuffer(GL_ARRAY_BUFFER, 0);

   test_state state = {0};

   api_image *colorbuf = ctx->create_image(ctx, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                           FB_SIZE, FB_SIZE, 1, 1, VK_IMAGE_TILING_OPTIMAL,
                                           api_heap_device);
   api_image *zsbuf = ctx->create_image(ctx, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT,
                                        FB_SIZE, FB_SIZE, 1, 1, VK_IMAGE_TILING_OPTIMAL,
                                        api_heap_device);
   state.fb = ctx->create_framebuffer(ctx, 1, &colorbuf, zsbuf, FB_SIZE, FB_SIZE, 1, 0x1);

   /* Clear the framebuffer. */
   ctx->begin_cmdbuf(ctx, api_queue_gfx);
   ctx->begin_render_pass(ctx, &(api_render_pass_desc){
                             .fb = state.fb,
                             .clear = true,
                             .clear_values.zs.depth = 0.5,
                          });
   ctx->end_render_pass(ctx);
   ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);

   for (test_type test = 0; test < NUM_TEST_SETS; test++) {
      /*if (skip_test)
         continue;*/

      /* Reset vertex attributes to stride=0. */
      for (unsigned i = 0; i < MAX_VEC4S; i++) {
         glEnableVertexAttribArray(i);
         glBindVertexBuffer(i, 0, 0, 0);
         glVertexAttribBinding(i, 0);
         glVertexAttribFormat(i, 4, GL_FLOAT, false, 0);
      }
      glBindVertexBuffer(0, buf_single_vertex, 0, 0);

      unsigned max_vertices_per_draw = MAX_VERTICES_PER_DRAW;

      switch (test) {
      case TEST_VS_IN:
         /* VS input loads (all VS inputs are read, but all primitives are culled) */
         puts("VS INPUTS");
         puts("Vec4s, Separate, Interleaved");

         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            unsigned num_vec4s = ALIGN_POT(num_comps, 4) / 4;
            unsigned vertex_size = num_comps * 4;

            printf("%5.2f", num_comps / 4.0);

            ctx->bind_gfx_pipeline(ctx, prog_vs_in[num_comps - 1]);

            for (unsigned interleaved = 0; interleaved < 2; interleaved++) {
               GLuint buf_interleaved = 0, buf_separate[MAX_VEC4S] = {0};

               if (interleaved) {
                  /* Don't initialize the buffer data. It's read, but never used meaningfully. */
                  glGenBuffers(1, &buf_interleaved);
                  glBindBuffer(GL_ARRAY_BUFFER, buf_interleaved);
                  glBufferData(GL_ARRAY_BUFFER, BO_ALLOC_SIZE, NULL, GL_STATIC_DRAW);
                  glBindBuffer(GL_ARRAY_BUFFER, 0);

                  glBindVertexBuffer(0, buf_interleaved, 0, vertex_size);

                  for (unsigned i = 0; i < num_vec4s; i++) {
                     glVertexAttribBinding(i, 0);
                     glVertexAttribFormat(i, i == num_vec4s - 1 && num_comps % 4 ?
                                             num_comps % 4 : 4, GL_FLOAT, false,
                                          i * 16);
                  }

                  max_vertices_per_draw = BO_ALLOC_SIZE / vertex_size;
                  max_vertices_per_draw -= max_vertices_per_draw % 3;
               } else {
                  unsigned sep_buf_size = (BO_ALLOC_SIZE * 4) / num_comps;
                  sep_buf_size = ALIGN_POT(sep_buf_size, 16);

                  for (unsigned i = 0; i < num_vec4s; i++) {
                     unsigned size = i == num_vec4s - 1 && num_comps % 4 ?
                                        num_comps % 4 : 4;

                     /* Don't initialize the buffer data. It's read, but never used meaningfully. */
                     glGenBuffers(1, &buf_separate[i]);
                     glBindBuffer(GL_ARRAY_BUFFER, buf_separate[i]);
                     glBufferData(GL_ARRAY_BUFFER, sep_buf_size, NULL, GL_STATIC_DRAW);
                     glBindBuffer(GL_ARRAY_BUFFER, 0);

                     glBindVertexBuffer(i, buf_separate[i], 0, size * 4);
                     glVertexAttribBinding(i, i);
                     glVertexAttribFormat(i, size, GL_FLOAT, false, 0);
                  }

                  max_vertices_per_draw = sep_buf_size / 16;
                  max_vertices_per_draw -= max_vertices_per_draw % 3;
               }

               run_test(ctx, &state, test, max_vertices_per_draw);
               print_result(ctx, &state, vertex_size, test);

               if (interleaved) {
                  glDeleteBuffers(1, &buf_interleaved);
               } else {
                  for (unsigned i = 0; i < num_vec4s; i++)
                     glDeleteBuffers(1, &buf_separate[i]);
               }
            }
            puts("");
         }
         break;

      case TEST_VS_OUT_XFB:
         /* VS output stores via transform feedback (VS inputs have stride=0, rasterizer discard) */
         puts("VS OUTPUTS VIA XFB");
         puts("Vec4s, Interleaved");

         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            unsigned vertex_size = num_comps * 4;
            GLuint xfb_buf;

            glGenBuffers(1, &xfb_buf);
            glBindBuffer(GL_ARRAY_BUFFER, xfb_buf);
            glBufferData(GL_ARRAY_BUFFER, BO_ALLOC_SIZE, NULL, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            ctx->bind_transform_feedback_buffer(ctx, xfb_buf, 0, xfb_buf->size);

            max_vertices_per_draw = BO_ALLOC_SIZE / vertex_size;
            max_vertices_per_draw -= max_vertices_per_draw % 3;

            printf("%5.2f", num_comps / 4.0);
            ctx->bind_gfx_pipeline(ctx, prog_vs_out_xfb[num_comps - 1]);
            run_test(ctx, &state, test, max_vertices_per_draw);
            print_result(ctx, &state, vertex_size, test);
            puts("");

            glDeleteBuffers(1, &xfb_buf);
         }
         break;

      case TEST_VS_OUT_FS:
         /* VS output stores into FS (VS inputs have stride=0, execute FS to test FS input loads) */
         puts("VS OUTPUTS VIA FS");
         puts("Vec4s,   Result");

         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            unsigned vertex_size = num_comps * 4;

            printf("%5.2f", num_comps / 4.0);
            ctx->bind_gfx_pipeline(ctx, prog_vs_out_fs[num_comps - 1]);
            run_test(ctx, &state, test, max_vertices_per_draw);
            print_result(ctx, &state, vertex_size, test);
            puts("");
         }
         break;

      case TEST_GS_OUT:
         /* GS output stores into FS (VS inputs have stride=0, execute FS to test FS input loads) */
         puts("GS OUTPUTS");
         puts("Vec4s, MaxVtx=3, MaxVtx=6, MaxVtx=9");

         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            unsigned vertex_size = num_comps * 4;

            printf("%5.2f", num_comps / 4.0);
            for (unsigned amp_factor = 0; amp_factor < 3; amp_factor++) {
               ctx->bind_gfx_pipeline(ctx, prog_gs_out[amp_factor][num_comps - 1]);
               run_test(ctx, &state, test, max_vertices_per_draw);
               print_result(ctx, &state, vertex_size * (amp_factor + 1), test);
            }
            puts("");
         }
         break;

      case TEST_TCS_OUT:
         /* TCS output stores into TES (VS inputs have stride=0, all TES inputs are read, but all primitives are culled) */
         puts("TCS OUTPUTS (gl_TessLevelOuter[0] = Level, all other levels are 1)");
         puts("Vec4s,  Level=0,  Level=1,  Level=2");

         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            unsigned vertex_size = num_comps * 4;

            printf("%5.2f", num_comps / 4.0);
            for (unsigned tf = 0; tf < 3; tf++) {
               ctx->bind_gfx_pipeline(ctx, prog_tcs_out[tf][num_comps - 1]);
               run_test(ctx, &state, test, max_vertices_per_draw);
               print_result(ctx, &state, vertex_size, test);
            }
            puts("");
         }
         break;
      case TEST_TCS_PATCH_OUT:
         /* TCS output stores into TES (VS inputs have stride=0, all TES inputs are read, but all primitives are culled) */
         puts("TCS PATCH OUTPUTS (gl_TessLevelOuter[0] = Level, all other levels are 1)");
         puts("Vec4s,  Level=0,  Level=1,  Level=2");

         for (unsigned num_comps = 1; num_comps <= MAX_COMPS; num_comps++) {
            unsigned patch_size = num_comps * 4;

            printf("%5.2f", num_comps / 4.0);
            for (unsigned tf = 0; tf < 3; tf++) {
               ctx->bind_gfx_pipeline(ctx, prog_tcs_patch_out[tf][num_comps - 1]);
               run_test(ctx, &state, test, max_vertices_per_draw);
               print_result(ctx, &state, patch_size, test);
            }
            puts("");
         }
         break;
      }
   }
}
