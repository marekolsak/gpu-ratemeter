/* Copyright 2026 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

/* begin_render_pass clear:
 * - gl: glClearBuffer{iv,uiv,fv}
 * - vk: vkCmdBeginRenderPass / vkCmdBeginRendering
 *
 * clear_attachments:
 * - gl: scissored glClearBuffer{iv,uiv,fv}
 * - vk: vkCmdClearAttachments
 *
 * clear_image:
 * - gl: glClearTex{Sub}Image
 * - vk: vkCmdClearColorImage
 *
 * blit_image - copy:
 * - gl: glCopyImageSubData
 * - vk: vkCmdCopyImage2
 *
 * blit_image - blit:
 * - gl: glBlitNamedFramebuffer
 * - vk: vkCmdBlitImage2
 *
 * blit_image - resolve:
 * - gl: glBlitNamedFramebuffer
 * - vk: vkCmdResolveImage2
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

#define NUM_WARMUP_RUNS    1
#define NUM_RUNS           4

#define DUMP_IMAGES        0

#define MAX_DELETE_ITEMS   32

typedef struct {
   api_shader *vs_passthrough[2]; /* layered / non-layered variants */
   api_shader *fs_gradient[5]; /* output format variants */
   api_shader *fs_random[7]; /* output format variants */

   api_gfx_pipeline *delete_pipelines[MAX_DELETE_ITEMS];
   api_framebuffer *delete_fbs[MAX_DELETE_ITEMS];
   unsigned num_delete_items;
} misc_state;

static api_shader *
get_passthrough_vs(api_context *ctx, misc_state *state, bool layered)
{
   api_shader **vs = &state->vs_passthrough[layered];

   if (*vs)
      return *vs;

   char vs_source[] =
         "#version 460 \n"
         "#extension GL_ARB_shader_viewport_layer_array : enable \n"
         "#define LAYERED 0 \n"
         "#ifdef VULKAN \n"
         "   #define gl_VertexID gl_VertexIndex \n"
         "   #define gl_InstanceID gl_InstanceIndex \n"
         "#endif \n"

         "void main() { \n"
         /* Generate a quad from VertexID. */
         "   gl_Position = vec4((gl_VertexID & 1) == 0 ? -1 : 1, \n"
         "                      (gl_VertexID & 2) == 0 ? -1 : 1, 0, 1); \n"
         "#if LAYERED \n"
         "   gl_Layer = gl_InstanceID; \n"
         "#endif \n"
         "}";

   if (layered)
      strstr(vs_source, "LAYERED")[8] = '1';

   *vs = ctx->create_shader(ctx, vs_source, api_shader_vs);
   return *vs;
}

static api_shader *
get_gradient_fs(api_context *ctx, misc_state *state, VkFormat format)
{
   bool is_sint = format_is_sint(format);
   bool is_integer = format_is_integer(format);
   unsigned chan_size = get_pixel_size_from_format(format) / format_get_num_channels(format);

   const char *gradient, *vec_type;
   unsigned format_index;

   /* This fragment shader code generates the gradient pattern. */
   if (is_integer) {
      if (chan_size == 1) {
         if (is_sint) {
            gradient = "int(gl_FragCoord.x) % 128";
            vec_type = "ivec4";
            format_index = 0;
         } else {
            gradient = "uint(gl_FragCoord.x) % 256";
            vec_type = "uvec4";
            format_index = 1;
         }
      } else if (chan_size >= 2) {
         if (is_sint) {
            gradient = "int(gl_FragCoord.x)";
            vec_type = "ivec4";
            format_index = 2;
         } else {
            gradient = "uint(gl_FragCoord.x)";
            vec_type = "uvec4";
            format_index = 3;
         }
      } else {
         error("invalid chan size");
      }
   } else {
      gradient = "mod(gl_FragCoord.x, 256.0) / 256.0";
      vec_type = "vec4";
      format_index = 4;
   }

   assert(format_index < ARRAY_SIZE(state->fs_gradient));
   api_shader **fs = &state->fs_gradient[format_index];

   if (*fs)
      return *fs;

   char fs_source[1024];
   snprintf(fs_source, sizeof(fs_source),
            "#version 460 \n"
            "layout(location = 0) out %s fs_out; \n"

            "void main() { \n"
            "   fs_out = %s(%s); \n"
            "}",
            vec_type, vec_type, gradient);

   *fs = ctx->create_shader(ctx, fs_source, api_shader_fs);
   return *fs;
}

static api_shader *
get_random_color_fs(api_context *ctx, misc_state *state, VkFormat format)
{
   bool is_sint = format_is_sint(format);
   bool is_integer = format_is_integer(format);
   unsigned chan_size = get_pixel_size_from_format(format) / format_get_num_channels(format);

   const char *rand_color, *vec_type;
   unsigned format_index;

   /* This fragment shader code generates the gradient pattern. */
   if (is_integer) {
      if (chan_size == 1) {
         if (is_sint) {
            rand_color = "rand_uvec4(seed) >> 25";
            vec_type = "ivec4";
            format_index = 0;
         } else {
            rand_color = "rand_uvec4(seed) >> 24";
            vec_type = "uvec4";
            format_index = 1;
         }
      } else if (chan_size == 2) {
         if (is_sint) {
            rand_color = "rand_uvec4(seed) >> 17";
            vec_type = "ivec4";
            format_index = 2;
         } else {
            rand_color = "rand_uvec4(seed) >> 16";
            vec_type = "uvec4";
            format_index = 3;
         }
      } else if (chan_size == 4) {
         if (is_sint) {
            rand_color = "rand_uvec4(seed) >> 1";
            vec_type = "ivec4";
            format_index = 4;
         } else {
            rand_color = "rand_uvec4(seed)";
            vec_type = "uvec4";
            format_index = 5;
         }
      } else {
         error("invalid chan size");
      }
   } else {
      rand_color = "rand_vec4(seed)";
      vec_type = "vec4";
      format_index = 6;
   }

   assert(format_index < ARRAY_SIZE(state->fs_random));
   api_shader **fs = &state->fs_random[format_index];

   if (*fs)
      return *fs;

   char fs_source[1024];
   snprintf(fs_source, sizeof(fs_source),
            "#version 460 \n"
            "layout(location = 0) out %s fs_out; \n"

            /* 32-bit avalanche mixer */
            "uint mix32(uint v) \n"
            "{ \n"
            "   v ^= v >> 16; \n"
            "   v *= 0x7feb352du; \n"
            "   v ^= v >> 15; \n"
            "   v *= 0x846ca68bu; \n"
            "   v ^= v >> 16; \n"
            "   return v; \n"
            "} \n"

            "uint rand_uint(uvec4 seed) \n"
            "{ \n"
            "   return mix32(mix32(seed.x * 0xA341316Cu) ^ \n"
            "                mix32(seed.y * 0xC8013EA4u) ^ \n"
            "                mix32(seed.z * 0xAD90777Du) ^ \n"
            "                mix32(seed.w * 0x7E95761Eu)); \n"
            "} \n"

            "uvec4 rand_uvec4(uvec4 seed) \n"
            "{ \n"
            "   uint h0 = rand_uint(seed); \n"
            "   uint h1 = mix32(h0 ^ 0x68bc21ebu); \n"
            "   uint h2 = mix32(h0 ^ 0x02e5be93u); \n"
            "   uint h3 = mix32(h0 ^ 0x9e3779b9u); \n"
            " \n"
            "   return uvec4(h0, h1, h2, h3); \n"
            "} \n"

            "vec4 rand_vec4(uvec4 seed) \n"
            "{ \n"
            "   return rand_uvec4(seed) / 4294967296.0; \n"
            "} \n"

            "void main() { \n"
            "   uvec4 seed = uvec4(uint(gl_FragCoord.x), uint(gl_FragCoord.y), gl_Layer, gl_SampleMaskIn[0]); \n"
            "   fs_out = %s(%s); \n"
            "}",
            vec_type, vec_type, rand_color);

   *fs = ctx->create_shader(ctx, fs_source, api_shader_fs);
   return *fs;
}

static void
generate_pixels(api_context *ctx, misc_state *state, api_image *image, api_shader *fs,
                unsigned samplemask)
{
   bool layered = image->depth > 1;
   api_framebuffer *fb = ctx->create_framebuffer(ctx, image, NULL, image->width, image->height,
                                                 image->samples, 0x1);
   api_gfx_pipeline *pipeline =
      ctx->create_gfx_pipeline(ctx,
                               &(api_gfx_pipeline_desc){
                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                  .vs = get_passthrough_vs(ctx, state, layered),
                                  .fs = fs,
                                  .vrs_fragment_size = {1, 1},
                                  .samplemask = samplemask,
                                  .colormask = 0xf,
                                  .fb = fb,
                               });

   ctx->begin_cmdbuf(ctx, api_queue_gfx);
   ctx->begin_render_pass(ctx, &(api_render_pass_desc){.fb = fb, .clear = true});

   ctx->bind_gfx_pipeline(ctx, pipeline);
   ctx->draw(ctx, &(api_draw_desc){.count = 4, .instance_count = layered ? image->depth : 1});

   ctx->end_render_pass(ctx);
   ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);

   assert(state->num_delete_items < ARRAY_SIZE(state->delete_pipelines));
   assert(state->num_delete_items < ARRAY_SIZE(state->delete_fbs));
   state->delete_pipelines[state->num_delete_items] = pipeline;
   state->delete_fbs[state->num_delete_items] = fb;
   state->num_delete_items++;
}

static void
set_gradient_pixels(api_context *ctx, misc_state *state, api_image *image)
{
   generate_pixels(ctx, state, image, get_gradient_fs(ctx, state, image->format),
                   (1 << image->samples) - 1);
}

static void
set_random_pixels(api_context *ctx, misc_state *state, api_image *image, unsigned samplemask)
{
   generate_pixels(ctx, state, image, get_random_color_fs(ctx, state, image->format), samplemask);
}

static struct {
   const char *name;
   VkFormat format;
} formats[] = {
   {"r8",      VK_FORMAT_R8_UNORM},
   {"r8g8",    VK_FORMAT_R8G8_UNORM},
   {"r16f",    VK_FORMAT_R16_SFLOAT},
   {"rgba8",   VK_FORMAT_R8G8B8A8_UNORM},
   {"r32f",    VK_FORMAT_R32_SFLOAT},
   {"rgba16f", VK_FORMAT_R16G16B16A16_SFLOAT},
   {"rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT},
};

enum {
   TEST_CLEAR_FB,
   TEST_CLEAR_IMAGE,
   TEST_COPY,
   TEST_BLIT,
   TEST_RESOLVE,
   NUM_TESTS,
};

static const char *test_strings[] = {
   [TEST_CLEAR_FB] = "clear_fb",
   [TEST_CLEAR_IMAGE] = "clear_image",
   [TEST_COPY] = "copy",
   [TEST_BLIT] = "blit",
   [TEST_RESOLVE] = "resolve",
};

enum {
   LAYOUT_DEFAULT,
   LAYOUT_SRC_LINEAR,
   LAYOUT_DST_LINEAR,
   NUM_LAYOUTS,
};

static const char *layout_strings[] = {
   [LAYOUT_DEFAULT] = "",
   [LAYOUT_SRC_LINEAR] = ".src_linear",
   [LAYOUT_DST_LINEAR] = ".dst_linear",
};

enum {
   BOX_FULL,
   BOX_FULL_YFLIP,
   BOX_PARTIAL,
   BOX_PARTIAL_UNALIGNED,
   BOX_PARTIAL_UNALIGNED_YFLIP,
   NUM_BOXES,
};

static const char *box_strings[] = {
   [BOX_FULL] = "full",
   [BOX_FULL_YFLIP] = "yflip",
   [BOX_PARTIAL] = "partial",
   [BOX_PARTIAL_UNALIGNED] = "unaligned",
   [BOX_PARTIAL_UNALIGNED_YFLIP] = "yflip_unaligned",
};

enum {
   FILL_BLACK,
   FILL_SOLID,
   FILL_GRADIENT,
   FILL_RANDOM,
   FILL_RANDOM_FRAGMENTED2,
   FILL_RANDOM_FRAGMENTED4,
   FILL_RANDOM_FRAGMENTED8,
   NUM_FILLS,
};

static const char *fill_strings[] = {
   [FILL_BLACK] = "black",
   [FILL_SOLID] = "solid",
   [FILL_GRADIENT] = "gradient",
   [FILL_RANDOM] = "random",
   [FILL_RANDOM_FRAGMENTED2] = "fragmented2",
   [FILL_RANDOM_FRAGMENTED4] = "fragmented4",
   [FILL_RANDOM_FRAGMENTED8] = "fragmented8",
};

typedef enum {
   COUNT_TESTS,
   RUN,
   REPORT,
} test_stage;

static void
verify_content(api_context *ctx, api_image *image, unsigned test_index, unsigned format_index,
               unsigned layout, unsigned fill_flavor)
{
   if (image->samples == 1) {
      for (unsigned z = 0; z < image->depth; z++) {
         char filename[1024];

         snprintf(filename, sizeof(filename), "%s_%uD_%s_%s_%ux%u_%s_%u.png",
                  test_strings[test_index], image->type + 1, formats[format_index].name,
                  layout_strings[layout], image->width, image->height, fill_strings[fill_flavor], z);

         ctx->image_write_png(ctx, image, z, filename);
      }
   }
}

static void
get_subtest_name(char *out, size_t max_len, unsigned test_index, VkImageType img_type,
                 unsigned format_index, unsigned samples, unsigned layout, unsigned fill_flavor,
                 unsigned box_flavor)
{
   snprintf(out, max_len, "%s.%ud.%s.%us.fill_%s%s.region_%s",
            test_strings[test_index], img_type + 1, formats[format_index].name, samples,
            fill_strings[fill_flavor], layout_strings[layout], box_strings[box_flavor]);
}

static void
print_table_row(bool header, unsigned test_index, VkImageType img_type, unsigned format_index,
                unsigned samples, unsigned layout, unsigned fill_flavor, unsigned box_flavor)
{
   const unsigned name_indent = 68;

   if (header) {
      printf("%-*s,%s,%s\n", name_indent, "Size", "small", "LARGE");
   } else {
      char name[128];

      get_subtest_name(name, sizeof(name), test_index, img_type, format_index, samples, layout,
                       fill_flavor, box_flavor);
      printf("%-*s", name_indent, name);
   }
}

static const VkClearColorValue black_color_float = {.float32 = {0, 0, 0, 0}};
static const VkClearColorValue solid_color_float = {.float32 = {0.2, 0.3, 0.4, 0.5}};
static const VkClearColorValue black_color_uint = {.uint32 = {0, 0, 0, 0}};
static const VkClearColorValue solid_color_uint = {.uint32 = {23, 45, 89, 107}};

static void
run(api_context *ctx, const char *test_name, test_stage stage, unsigned *num_tests,
    api_query_pool *timestamps)
{
   if (stage == REPORT)
      print_table_row(true, 0, 0, 0, 0, 0, 0, 0);

   misc_state misc_state = {0};
   unsigned num_visited_tests = 0;

   for (unsigned test_index = 0; test_index < NUM_TESTS; test_index++) {
      for (VkImageType img_type = VK_IMAGE_TYPE_1D; img_type <= VK_IMAGE_TYPE_3D; img_type++) {
         if (test_index == TEST_RESOLVE && img_type != VK_IMAGE_TYPE_2D)
            continue;

         for (unsigned format_index = 0; format_index < ARRAY_SIZE(formats); format_index++) {
            assert(format_is_valid(formats[format_index].format));

            if (test_index == TEST_RESOLVE && format_is_integer(formats[format_index].format))
               continue;

            for (unsigned samples = 1; samples <= 8; samples *= 2) {
               if (!(ctx->fb_format_sample_count_support[formats[format_index].format] & samples))
                  continue;

               if (samples >= 2 && img_type != VK_IMAGE_TYPE_2D)
                  continue;

               if (test_index == TEST_RESOLVE && samples == 1)
                  continue;

               for (unsigned layout = 0; layout < NUM_LAYOUTS; layout++) {
                  /* Reject invalid combinations. */
                  switch (test_index) {
                  case TEST_CLEAR_FB:
                  case TEST_CLEAR_IMAGE:
                  case TEST_BLIT:
                  case TEST_RESOLVE:
                     if (layout != LAYOUT_DEFAULT)
                        continue;
                     break;

                  case TEST_COPY:
                     if ((img_type == VK_IMAGE_TYPE_1D || samples >= 2) && layout != LAYOUT_DEFAULT)
                        continue;
                     break;
                  }

                  /* Report n/a for unsupported tests. */
                  bool unsupported = false;

                  if ((layout == LAYOUT_SRC_LINEAR || layout == LAYOUT_DST_LINEAR) &&
                      !ctx->has_image_tiling_linear)
                     unsupported = true;

                  if (test_index == TEST_BLIT && img_type == VK_IMAGE_TYPE_3D &&
                      !ctx->has_blit_image_3d)
                     unsupported = true;

                  if (test_index == TEST_BLIT && samples > 1 &&
                      !ctx->has_blit_image_msaa)
                     unsupported = true;

                  /* Create textures. */
                  unsigned bpe = get_pixel_size_from_format(formats[format_index].format);
                  unsigned msaa_pix_size = bpe * samples;

                  struct {
                     unsigned width;
                     unsigned height;
                     unsigned depth;
                     api_image *src;
                     api_image *dst;
                     api_framebuffer *fb;
                  } state[2] = {0};

                  for (unsigned size_index = 0; size_index <= 1; size_index++) {
                     unsigned mb_size = (size_index ? 256 : 8) * (DUMP_IMAGES ? 16 : 1024) * 1024;
                     unsigned width = 1, height = 1, depth = 1;

                     /* Determine the size. The footprint must be exactly "mb_size" for 2D and 3D. */
                     if (img_type == VK_IMAGE_TYPE_1D) {
                        width = size_index ? 16384 : 2048;
                     } else if (img_type == VK_IMAGE_TYPE_2D) {
                        width = height = get_next_power_of_two(sqrt(mb_size / msaa_pix_size));

                        for (unsigned i = 0; width * height * msaa_pix_size != mb_size; i++) {
                           if (i % 2 == 1)
                              width /= 2;
                           else
                              height /= 2;
                        }
                     } else if (img_type == VK_IMAGE_TYPE_3D) {
                        width = height = depth = get_next_power_of_two(pow(mb_size / msaa_pix_size, 0.333333));

                        for (unsigned i = 0; width * height * depth * msaa_pix_size != mb_size; i++) {
                           if (i % 3 == 2)
                              width /= 2;
                           else if (i % 3 == 1)
                              height /= 2;
                           else
                              depth /= 2;
                        }
                     }

                     state[size_index].width = MIN2(width, 16384);
                     state[size_index].height = MIN2(height, 16384);
                     state[size_index].depth = MIN2(depth, 16384);

                     if (stage == RUN && !unsupported) {
                        unsigned src_samples = samples;
                        unsigned src_tiling = layout == LAYOUT_SRC_LINEAR ?
                                                 VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
                        unsigned dst_tiling = layout == LAYOUT_DST_LINEAR ?
                                                 VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
                        unsigned dst_samples = test_index == TEST_RESOLVE ? 1 : src_samples;

                        if (test_index != TEST_CLEAR_FB && test_index != TEST_CLEAR_IMAGE) {
                           state[size_index].src =
                              ctx->create_image(ctx, img_type, formats[format_index].format,
                                                state[size_index].width, state[size_index].height,
                                                state[size_index].depth, src_samples, src_tiling,
                                                api_heap_device);
                        }

                        state[size_index].dst =
                           ctx->create_image(ctx, img_type, formats[format_index].format,
                                             state[size_index].width, state[size_index].height,
                                             state[size_index].depth, dst_samples, dst_tiling,
                                             api_heap_device);

                        state[size_index].fb =
                              ctx->create_framebuffer(ctx, state[size_index].dst, NULL,
                                                      state[size_index].width, state[size_index].height,
                                                      dst_samples, 0x1);
                     }
                  }

                  for (unsigned fill_flavor = 0; fill_flavor < NUM_FILLS; fill_flavor++) {
                     const VkClearColorValue *clear_color =
                        format_is_integer(formats[format_index].format) ?
                           (fill_flavor == FILL_BLACK ? &black_color_uint : &solid_color_uint) :
                           (fill_flavor == FILL_BLACK ? &black_color_float : &solid_color_float);

                     /* Reject invalid combinations. */
                     if ((test_index == TEST_CLEAR_FB || test_index == TEST_CLEAR_IMAGE) &&
                         fill_flavor != FILL_SOLID && fill_flavor != FILL_BLACK)
                        continue;

                     if ((samples == 1 && fill_flavor >= FILL_RANDOM_FRAGMENTED2) ||
                         (samples == 2 && fill_flavor >= FILL_RANDOM_FRAGMENTED4) ||
                         (samples == 4 && fill_flavor >= FILL_RANDOM_FRAGMENTED8))
                        continue;

                     /* Fill the source texture. */
                     if (stage == RUN && !unsupported) {
#if 0
                        char name[128];
                        get_subtest_name(name, sizeof(name), test_index, img_type, format_index,
                                         samples, layout, fill_flavor, 0);
                        printf("Executing: %s\n", name);
#endif

                        if (test_index != TEST_CLEAR_FB && test_index != TEST_CLEAR_IMAGE) {
                           for (unsigned size_index = 0; size_index <= 1; size_index++) {
                              switch (fill_flavor) {
                              case FILL_BLACK:
                              case FILL_SOLID: {
                                 ctx->begin_cmdbuf(ctx, api_queue_gfx);
                                 ctx->clear_image(ctx, state[size_index].src, NULL, clear_color);
                                 ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);
                                 break;
                              }

                              case FILL_GRADIENT:
                                 set_gradient_pixels(ctx, &misc_state, state[size_index].src);
                                 break;

                              case FILL_RANDOM:
                                 set_random_pixels(ctx, &misc_state, state[size_index].src, ~0);
                                 break;

                              case FILL_RANDOM_FRAGMENTED2:
                                 assert(samples >= 2);
                                 /* Make all samples equal. */
                                 set_random_pixels(ctx, &misc_state, state[size_index].src, ~0);
                                 /* Make sample 0 different. */
                                 set_random_pixels(ctx, &misc_state, state[size_index].src, 0x1);
                                 break;

                              case FILL_RANDOM_FRAGMENTED4:
                                 assert(samples >= 4);
                                 /* Make all samples equal. */
                                 set_random_pixels(ctx, &misc_state, state[size_index].src, ~0);
                                 /* Make samples 0..2 different. */
                                 for (unsigned i = 0; i <= 2; i++)
                                    set_random_pixels(ctx, &misc_state, state[size_index].src, 1 << i);
                                 break;

                              case FILL_RANDOM_FRAGMENTED8:
                                 assert(samples == 8);
                                 /* Make all samples equal. */
                                 set_random_pixels(ctx, &misc_state, state[size_index].src, 0);
                                 /* Make samples 0..6 different. */
                                 for (unsigned i = 0; i <= 6; i++)
                                    set_random_pixels(ctx, &misc_state, state[size_index].src, 1 << i);
                                 break;

                              default:
                                 error("invalid fill flavor");
                              }

                              if (DUMP_IMAGES) {
                                 verify_content(ctx, state[size_index].src, test_index, format_index, layout,
                                                fill_flavor);
                              }
                           }
                        }
                     }

                     for (unsigned box_flavor = 0; box_flavor < NUM_BOXES; box_flavor++) {
                        bool yflip = box_flavor == BOX_FULL_YFLIP ||
                                     box_flavor == BOX_PARTIAL_UNALIGNED_YFLIP;

                        /* Reject invalid combinations. */
                        if (test_index == TEST_CLEAR_FB && box_flavor != BOX_FULL)
                           continue;

                        if ((test_index == TEST_CLEAR_IMAGE || test_index == TEST_COPY ||
                             img_type == VK_IMAGE_TYPE_1D) && yflip)
                           continue;

                        if (stage == REPORT) {
                           print_table_row(false, test_index, img_type, format_index, samples,
                                           layout, fill_flavor, box_flavor);
                        }

                        bool report_na = unsupported;

                        if (test_index == TEST_RESOLVE && yflip && !ctx->has_resolve_image_yflip)
                           report_na = true;

                        if (test_index == TEST_CLEAR_IMAGE && box_flavor != BOX_FULL &&
                            !ctx->has_clear_image_region)
                           report_na = true;

                        if (report_na) {
                           for (unsigned size_index = 0; size_index <= 1; size_index++) {
                              if (stage == COUNT_TESTS)
                                 (*num_tests)++;

                              if (stage == RUN)
                                 print_progress(*num_tests, &num_visited_tests, 20);

                              if (stage == REPORT)
                                 printf(",%10s", "n/a");
                           }

                           if (stage == REPORT)
                              printf("\n");
                           continue;
                        }

                        for (unsigned size_index = 0; size_index <= 1; size_index++) {
                           if (stage == COUNT_TESTS)
                              (*num_tests)++;

                           api_image_box src_box = {0}, dst_box = {0};

                           /* Determine the box. */
                           dst_box.width = state[size_index].width;
                           dst_box.height = state[size_index].height;
                           dst_box.depth = state[size_index].depth;
                           src_box = dst_box;

                           switch (box_flavor) {
                           case BOX_FULL:
                              break;

                           case BOX_FULL_YFLIP:
                              src_box.y = src_box.height;
                              src_box.height = -src_box.height;
                              break;

                           case BOX_PARTIAL:
                              if (img_type == VK_IMAGE_TYPE_1D) {
                                 dst_box.x = 256;
                                 dst_box.width -= 256;
                              } else if (img_type == VK_IMAGE_TYPE_2D) {
                                 dst_box.x = 16;
                                 dst_box.y = 16;
                                 dst_box.width -= 16;
                                 dst_box.height -= 16;
                              } else {
                                 dst_box.x = 8;
                                 dst_box.y = 8;
                                 dst_box.z = 8;
                                 dst_box.width -= 8;
                                 dst_box.height -= 8;
                                 dst_box.depth -= 8;
                              }
                              src_box = dst_box;
                              break;

                           case BOX_PARTIAL_UNALIGNED:
                           case BOX_PARTIAL_UNALIGNED_YFLIP: {
                              const unsigned off = 13;
                              dst_box.x = off;
                              dst_box.width -= off;
                              if (img_type >= VK_IMAGE_TYPE_2D) {
                                 dst_box.y = off;
                                 dst_box.height -= off;
                                 if (img_type == VK_IMAGE_TYPE_3D) {
                                    dst_box.z = off;
                                    dst_box.depth -= off;
                                 }
                              }
                              src_box = dst_box;

                              if (box_flavor == BOX_PARTIAL_UNALIGNED_YFLIP) {
                                 src_box.y += src_box.height;
                                 src_box.height = -src_box.height;
                              }
                              break;
                           }

                           default:
                              error("invalid box flavor");
                           }

                           assert(dst_box.x >= 0);
                           assert(dst_box.y >= 0);
                           assert(dst_box.z >= 0);
                           assert(dst_box.width > 0);
                           assert(dst_box.height > 0);
                           assert(dst_box.depth > 0);
                           assert(dst_box.x + dst_box.width <= state[size_index].width);
                           assert(dst_box.y + dst_box.height <= state[size_index].height);
                           assert(dst_box.z + dst_box.depth <= state[size_index].depth);

                           if (state[size_index].src) {
                              assert(src_box.width);
                              assert(src_box.height);
                              assert(src_box.depth > 0);
                              if (src_box.width > 0) {
                                 assert(src_box.x >= 0);
                                 assert(src_box.x + src_box.width <= state[size_index].width);
                              } else {
                                 assert(src_box.x + src_box.width >= 0);
                                 assert(src_box.x - 1 < state[size_index].width);
                              }
                              if (src_box.height > 0) {
                                 assert(src_box.y >= 0);
                                 assert(src_box.y + src_box.height <= state[size_index].height);
                              } else {
                                 assert(src_box.y + src_box.height >= 0);
                                 assert(src_box.y - 1 < state[size_index].height);
                              }
                              assert(src_box.z >= 0);
                              assert(src_box.z + src_box.depth <= state[size_index].depth);
                           }

                           if (stage == RUN) {
                              ctx->begin_cmdbuf(ctx, api_queue_gfx);

                              /* Run tests. */
                              for (unsigned i = 0; i < NUM_WARMUP_RUNS + NUM_RUNS; i++) {
                                 /* The first few just warm up caches and the hw. */
                                 if (i == NUM_WARMUP_RUNS) {
                                    ctx->driver_workaround(ctx, WA_RDNA4_TIMESTAMP_BUG);
                                    ctx->write_next_query_value(ctx, timestamps);
                                 }

                                 switch (test_index) {
                                 case TEST_CLEAR_FB:
                                    ctx->begin_render_pass(ctx,
                                                           &(api_render_pass_desc) {
                                                              .fb = state[size_index].fb,
                                                              .clear = true,
                                                              .clear_values.color = *clear_color,
                                                           });
                                    ctx->end_render_pass(ctx);
                                    break;

                                 case TEST_CLEAR_IMAGE:
                                    assert(!yflip);
                                    ctx->clear_image(ctx, state[size_index].dst,
                                                     box_flavor == BOX_FULL ? NULL : &dst_box, clear_color);
                                    break;

                                 case TEST_COPY:
                                 case TEST_BLIT:
                                 case TEST_RESOLVE: {
                                    ctx->blit_image(ctx,
                                                    &(api_blit_desc){
                                                       .dst = state[size_index].dst,
                                                       .dst_box = dst_box,
                                                       .src = state[size_index].src,
                                                       .src_box = src_box,
                                                       .is_copy = test_index == TEST_COPY,
                                                    });
                                    break;
                                 }

                                 default:
                                    error("invalid test flavor");
                                 }
                              }

                              ctx->write_next_query_value(ctx, timestamps);
                              ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);
                           }

                           if (stage == RUN)
                              print_progress(*num_tests, &num_visited_tests, 20);

                           /* Get results. */
                           if (stage == REPORT) {
                              uint64_t num_pixels = (uint64_t)NUM_RUNS * dst_box.width *
                                                    dst_box.height * dst_box.depth;
                              uint64_t bytes;

                              if (test_index == TEST_CLEAR_FB || test_index == TEST_CLEAR_IMAGE)
                                 bytes = num_pixels * msaa_pix_size;
                              else if (test_index == TEST_RESOLVE)
                                 bytes = num_pixels * (msaa_pix_size + bpe);
                              else
                                 bytes = num_pixels * msaa_pix_size * 2;

                              print_throughput_from_next_timestamps(ctx, timestamps, bytes, NULL,
                                                                    "%10.2f", "%10s", 30);
                           }
                        }

                        if (stage == REPORT)
                           printf("\n");
                     }
                  }

                  if (stage == RUN && !unsupported) {
                     ctx->wait_for_idle(ctx);

                     for (unsigned size_index = 0; size_index <= 1; size_index++) {
                        if (state[size_index].fb)
                           ctx->destroy_framebuffer(ctx, state[size_index].fb);
                        ctx->destroy_image(ctx, state[size_index].dst);
                        if (test_index != TEST_CLEAR_FB && test_index != TEST_CLEAR_IMAGE)
                           ctx->destroy_image(ctx, state[size_index].src);
                     }

                     for (unsigned i = 0; i < misc_state.num_delete_items; i++) {
                        ctx->destroy_gfx_pipeline(ctx, misc_state.delete_pipelines[i]);
                        ctx->destroy_framebuffer(ctx, misc_state.delete_fbs[i]);
                     }
                     misc_state.num_delete_items = 0;
                  }
               }
            }
         }
      }
   }
}

void
test_imgbw(api_context *ctx, const char *test_name)
{
   unsigned num_tests = 0;

   run(ctx, test_name, COUNT_TESTS, &num_tests, NULL);

   api_query_pool *timestamps = ctx->create_query_pool(ctx, num_tests * 2, api_query_timestamp);

   printf("Executing tests ...");
   fflush(stdout);
   run(ctx, test_name, RUN, &num_tests, timestamps);
   puts("");

   ctx->get_query_results(ctx, timestamps);
   run(ctx, test_name, REPORT, NULL, timestamps);
}
