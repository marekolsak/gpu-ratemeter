/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define NUM_WARMUP_RUNS    1
#define NUM_RUNS           4

#define RANDOM_DATA_SIZE (611953 * 8) /* prime number * 8 */

static void
set_image_data(api_context *ctx, api_image *image, unsigned stride_in_bytes,
               int sample_index, void *data)
{
   if (image->samples > 1) {
      /* We have to blit the data from a single-sample image to the MSAA image. */
      /* Uploading the data into a staging image. */
      api_image *staging = ctx->create_image(ctx, image->type, image->format, image->width,
                                             image->height, image->depth, 1, VK_IMAGE_TILING_LINEAR,
                                             api_heap_device, 0);
      ctx->upload_image_data(ctx, staging, stride_in_bytes, data);

      /* Create shaders. */
      const char *vs_source =
            "#version 460 \n"
            "#ifdef VULKAN \n"
            "   #define gl_VertexID gl_VertexIndex \n"
            "#endif \n"

            "layout(location = 0) out vec2 coord; \n"

            "void main() { \n"
            /* Generate a quad from VertexID. */
            "   gl_Position = vec4((gl_VertexID & 1) == 0 ? -1 : 1, \n"
            "                      (gl_VertexID & 2) == 0 ? -1 : 1, 0, 1); \n"
            "   coord = gl_Position.xy * 0.5 + 0.5; \n"
            "}";

      const char *image_type = image->depth > 1 ? (image->type == VK_IMAGE_TYPE_3D ? "3D" : "2DArray") :
                                                  (image->type == VK_IMAGE_TYPE_2D ? "2D" : "1D");
      const char *fs_out_type = format_is_sint(image->format) ? "ivec4" :
                                format_is_integer(image->format) ? "uvec4" : "vec4";

      char fs_source[1024];
      snprintf(fs_source, sizeof(fs_source),
               "#version 460 \n"
               "#ifdef VULKAN \n"
               "   layout(set = 0, binding = 0) readonly uniform sampler%s staging; \n"
               "#else \n"
               "   layout(location = 0) readonly uniform sampler%s staging; \n"
               "#endif \n"

               "layout(location = 0) in vec2 coord; \n"
               "layout(location = 0) out %s fs_out; \n"

               "void main() { \n"
               "   fs_out = texelFetch(staging, ivec2(coord)); \n"
               "}", image_type, image_type, fs_out_type);

      api_shader *vs = ctx->create_shader(ctx, vs_source, api_shader_vs);
      api_shader *fs = ctx->create_shader(ctx, fs_source, api_shader_fs);

      /* Prepare the descriptor set for binding the staging image to a fragment shader. */
      api_descriptor_set_layout *fs_desc_set_layout =
         ctx->create_descriptor_set_layout(ctx,
                                           &(api_descriptor_set_layout_desc){
                                              .sampled_image.array_size = 1,
                                           });
      api_descriptor_set *fs_desc_set = ctx->create_descriptor_set(ctx, fs_desc_set_layout);
      ctx->set_sampled_image_descriptors(ctx, fs_desc_set, 1, &staging);

      /* Do a blit via a draw. */
      api_framebuffer *fb = ctx->create_framebuffer(ctx, image, NULL, image->width, image->height,
                                                    image->samples);
      api_pipeline *draw_rect_pipeline =
         ctx->create_pipeline(ctx,
                              &(api_pipeline_desc){
                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                 .desc_set_layout = fs_desc_set_layout,
                                 .vs = vs,
                                 .fs = fs,
                                 .vrs_fragment_size = {1, 1},
                                 .samplemask = sample_index == 0 ? (1 << image->samples) - 1 :
                                                                   1 << (sample_index - 1),
                                 .colormask = 0xf,
                                 .fb = fb,
                              });

      ctx->begin_cmdbuf(ctx);
      ctx->begin_render_pass(ctx,
                             &(api_render_pass_desc){
                                .fb = fb,
                             });

      ctx->bind_pipeline(ctx, draw_rect_pipeline);
      ctx->bind_descriptor_set(ctx, fs_desc_set);
      ctx->draw(ctx, &(api_draw_desc){.count = 4});

      ctx->end_render_pass(ctx);
      ctx->end_cmdbuf_and_submit(ctx);
      ctx->wait_idle_before_deallocation(ctx);

      ctx->destroy_pipeline(ctx, draw_rect_pipeline);
      ctx->destroy_framebuffer(ctx, fb);
      ctx->destroy_descriptor_set(ctx, fs_desc_set);
      ctx->destroy_descriptor_set_layout(ctx, fs_desc_set_layout);
      ctx->destroy_shader(ctx, vs);
      ctx->destroy_shader(ctx, fs);
      ctx->destroy_image(ctx, staging);
   } else {
      ctx->upload_image_data(ctx, image, stride_in_bytes, data);
   }
}

/* For MSAA, sample_index == 0 means set all samples, while sample_index > 0
 * means set the sample equal to sample_index - 1.
 *
 * random_data is random data generated in advance.
 */
static void
set_random_pixels(api_context *ctx, api_image *img, int sample_index,
                  const uint64_t *random_data)
{
   unsigned pix_size = get_pixel_size_from_format(img->format);
   unsigned img_stride_in_bytes = ALIGN_NPOT(img->width, 4) * pix_size;
   unsigned img_layer_stride_in_bytes = img_stride_in_bytes * img->height;
   uint8_t *data = (uint8_t*)malloc(img_layer_stride_in_bytes * img->depth);

   /* It's static because we want all following calls of this function to continue from
    * the previous offset.
    */
   static unsigned random_data_offset = 0;

   for (unsigned z = 0; z < img->depth; z++) {
      for (unsigned y = 0; y < img->height; y++) {
         uint64_t *ptr = (uint64_t *)(data + img_layer_stride_in_bytes * z + img_stride_in_bytes * y);
         unsigned size = img_stride_in_bytes;
         assert(size % 8 == 0);

         while (size) {
            unsigned copy_size =
               random_data_offset + size <= RANDOM_DATA_SIZE ? size :
                                                               RANDOM_DATA_SIZE - random_data_offset;

            memcpy(ptr, (uint8_t*)random_data + random_data_offset, copy_size);
            size -= copy_size;
            ptr += copy_size / 8;
            random_data_offset += copy_size;
            if (random_data_offset >= RANDOM_DATA_SIZE) {
               assert(random_data_offset == RANDOM_DATA_SIZE);
               random_data_offset = 0;
            }
         }
      }
   }

   set_image_data(ctx, img, img_stride_in_bytes, sample_index, data);
   free(data);
}

static void
set_gradient_pixels(api_context *ctx, api_image *img)
{
   unsigned pix_size = get_pixel_size_from_format(img->format);
   unsigned img_stride_in_bytes = ALIGN_NPOT(img->width, 4) * pix_size;
   unsigned img_layer_stride_in_bytes = img_stride_in_bytes * img->height;
   uint8_t *data = (uint8_t*)malloc(img_layer_stride_in_bytes * img->depth);

   /* Generate just 1 line of pixels. */
   unsigned line_size = img->width * pix_size;
   uint8_t *line = (uint8_t*)malloc(line_size);

   switch (img->format) {
   case VK_FORMAT_R8_UNORM:
      for (unsigned x = 0; x < img->width; x++) {
         uint8_t *p = (uint8_t*)(line + x * pix_size);
         p[0] = (float)x / (img->width - 1) * 255.5;
      }
      break;

   case VK_FORMAT_R8_UINT:
      for (unsigned x = 0; x < img->width; x++) {
         uint8_t *p = (uint8_t*)(line + x * pix_size);
         p[0] = x;
      }
      break;

   case VK_FORMAT_R16_UINT:
      for (unsigned x = 0; x < img->width; x++) {
         uint16_t *p = (uint16_t*)(line + x * pix_size);
         p[0] = x;
      }
      break;

   case VK_FORMAT_R16_SFLOAT:
      for (unsigned x = 0; x < img->width; x++) {
         uint16_t *p = (uint16_t*)(line + x * pix_size);
         p[0] = float_to_half((float)x / (img->width - 1));
      }
      break;

   case VK_FORMAT_R8G8B8A8_UNORM:
      for (unsigned x = 0; x < img->width; x++) {
         uint8_t *p = (uint8_t*)(line + x * pix_size);
         p[0] = p[1] = p[2] = p[3] = (float)x / (img->width - 1) * 255.5;
      }
      break;

   case VK_FORMAT_R32_UINT:
      for (unsigned x = 0; x < img->width; x++) {
         uint32_t *p = (uint32_t *)(line + x * pix_size);
         p[0] = x;
      }
      break;

   case VK_FORMAT_R32_SFLOAT:
      for (unsigned x = 0; x < img->width; x++) {
         float *p = (float*)(line + x * pix_size);
         p[0] = (float)x / (img->width - 1);
      }
      break;

   case VK_FORMAT_R32G32_UINT:
      for (unsigned x = 0; x < img->width; x++) {
         uint32_t *p = (uint32_t *)(line + x * pix_size);
         p[0] = p[1] = x;
      }
      break;

   case VK_FORMAT_R32G32_SFLOAT:
      for (unsigned x = 0; x < img->width; x++) {
         float *p = (float*)(line + x * pix_size);
         p[0] = p[1] = (float)x / (img->width - 1);
      }
      break;

   case VK_FORMAT_R16G16B16A16_SFLOAT:
      for (unsigned x = 0; x < img->width; x++) {
         uint16_t *p = (uint16_t*)(line + x * pix_size);
         p[0] = p[1] = p[2] = p[3] = float_to_half((float)x / (img->width - 1));
      }
      break;

   case VK_FORMAT_R32G32B32A32_UINT:
      for (unsigned x = 0; x < img->width; x++) {
         uint32_t *p = (uint32_t *)(line + x * pix_size);
         p[0] = p[1] = p[2] = p[3] = x;
      }
      break;

   case VK_FORMAT_R32G32B32A32_SFLOAT:
      for (unsigned x = 0; x < img->width; x++) {
         float *p = (float*)(line + x * pix_size);
         p[0] = p[1] = p[2] = p[3] = (float)x / (img->width - 1);
      }
      break;

   default:
      error("set_gradient_pixels: unexpected format");
   }

   /* Copy the generated line to all lines. */
   for (unsigned z = 0; z < img->depth; z++) {
      for (unsigned y = 0; y < img->height; y++)
         memcpy(data + img_layer_stride_in_bytes * z + img_stride_in_bytes * y, line, line_size);
   }

   set_image_data(ctx, img, img_stride_in_bytes, -1, data);

   free(line);
   free(data);
}

static struct {
   const char *name;
   VkFormat format;
} formats[] = {
   {"r8",      VK_FORMAT_R8_UNORM},
   {"r8u",     VK_FORMAT_R8_UINT},
   {"r16u",    VK_FORMAT_R16_UINT},
   {"r16f",    VK_FORMAT_R16_SFLOAT},
   {"rgba8",   VK_FORMAT_R8G8B8A8_UNORM},
   {"r32u",    VK_FORMAT_R32_UINT},
   {"r32f",    VK_FORMAT_R32_SFLOAT},
   {"rg32u",   VK_FORMAT_R32G32_UINT},
   {"rg32f",   VK_FORMAT_R32G32_SFLOAT},
   {"rgba16f", VK_FORMAT_R16G16B16A16_SFLOAT},
   {"rgba32u", VK_FORMAT_R32G32B32A32_UINT},
   {"rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT},
};

enum {
   TEST_FB_CLEAR,
   TEST_CLEAR,
   TEST_COPY,
   TEST_BLIT,
   TEST_RESOLVE,
   NUM_TESTS,
};

static const char *test_strings[] = {
   [TEST_FB_CLEAR] = "fbclear",
   [TEST_CLEAR] = "cleartex",
   [TEST_COPY] = "copy",
   [TEST_BLIT] = "blit",
   [TEST_RESOLVE] = "resolve",
};

enum {
   LAYOUT_T2T, /* tiled to tiled or clear tiled */
   LAYOUT_L2T, /* linear to tiled */
   LAYOUT_T2L, /* tiled to linear */
   LAYOUT_L2L, /* linear to linear or clear linear */
   NUM_LAYOUTS,
};

static const char *layout_strings[] = {
   [LAYOUT_T2T] = "T2T",
   [LAYOUT_L2T] = "L2T",
   [LAYOUT_T2L] = "T2L",
   [LAYOUT_L2L] = "L2L",
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
   [BOX_PARTIAL_UNALIGNED_YFLIP] = "yflip/unali",
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

static const VkClearColorValue black_color_float = {.float32 = {0, 0, 0, 0}};
static const VkClearColorValue solid_color_float = {.float32 = {0.2, 0.3, 0.4, 0.5}};
static const VkClearColorValue black_color_uint = {.uint32 = {0, 0, 0, 0}};
static const VkClearColorValue solid_color_uint = {.uint32 = {23, 45, 89, 107}};

static void
run(api_context *ctx, const char *test_suite_name, test_stage stage, unsigned *num_tests,
    api_timestamp_query_pool *timestamps)
{
   uint64_t *random_data = NULL;

   if (stage == RUN) {
      uint64_t seed_xorshift128plus[2];

      random_data = (uint64_t*)malloc(RANDOM_DATA_SIZE);

      /* Set the seed for random pixel data */
      s_rand_xorshift128plus(seed_xorshift128plus, false);

      /* Pre-generate random data for initializing textures. */
      for (unsigned i = 0; i < RANDOM_DATA_SIZE / 8; i++)
         random_data[i] = rand_xorshift128plus(seed_xorshift128plus);
   }

   if (stage == REPORT) {
      printf("Op      , Special  ,Dim, Format            ,MS,Layout, Fill       , Box         ,"
             "   small   ,   small   ,   small   ,   small   ,   LARGE   ,   LARGE   ,   LARGE   ,   LARGE\n");
      printf("--------,----------,---,-------------------,--,------,------------,-------------,"
             "  Default  ,    Gfx    ,  Compute  ,  Special  ,  Default  ,    Gfx    ,  Compute  ,  Special\n");
   }

   for (unsigned test_flavor = 0; test_flavor < NUM_TESTS; test_flavor++) {
      for (VkImageType img_type = VK_IMAGE_TYPE_1D; img_type <= VK_IMAGE_TYPE_3D; img_type++) {
         for (unsigned format_index = 0; format_index < ARRAY_SIZE(formats); format_index++) {
            for (unsigned samples = 1; samples <= 8; samples *= 2) {
               for (unsigned layout = 0; layout < NUM_LAYOUTS; layout++) {
                  /* Reject invalid combinations. */
                  if (samples >= 2 && (img_type != VK_IMAGE_TYPE_2D || layout != LAYOUT_T2T))
                     continue;

                  if (img_type == VK_IMAGE_TYPE_1D && layout != LAYOUT_L2L)
                     continue;

                  if (test_flavor != TEST_COPY && (layout == LAYOUT_L2T || layout == LAYOUT_T2L))
                     continue;

                  if (test_flavor != TEST_COPY && img_type != VK_IMAGE_TYPE_1D && layout != LAYOUT_T2T)
                     continue;

                  if (test_flavor == TEST_RESOLVE && samples == 1)
                     continue;

                  if (test_flavor == TEST_RESOLVE && format_is_integer(formats[format_index].format))
                     continue;

                  /* Create textures. */
                  api_image *src[2] = {0}, *dst[2] = {0};
                  api_framebuffer *fb[2] = {0};
                  unsigned bpe = get_pixel_size_from_format(formats[format_index].format);
                  unsigned msaa_pix_size = bpe * samples;

                  if (stage == RUN) {
                     for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                        unsigned mb_size = (size_factor ? 256 : 8) * 1024 * 1024;
                        unsigned width = 1, height = 1, depth = 1;

                        /* Determine the size. The footprint must be exactly "mb_size" for 2D and 3D. */
                        if (img_type == VK_IMAGE_TYPE_1D) {
                           width = size_factor ? 16384 : 2048;
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

                        unsigned img_width = MIN2(width, 16384);
                        unsigned img_height = MIN2(height, 16384);
                        unsigned img_depth = MIN2(depth, 16384);
                        unsigned src_samples = samples;
                        unsigned src_tiling = layout == LAYOUT_L2L || layout == LAYOUT_L2T ?
                                                 VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;

                        if (test_flavor != TEST_FB_CLEAR && test_flavor != TEST_CLEAR) {
                           src[size_factor] = ctx->create_image(ctx, img_type, formats[format_index].format,
                                                                img_width, img_height, img_depth,
                                                                src_samples, src_tiling,
                                                                api_heap_device, 0);
                        }

                        unsigned dst_tiling = layout == LAYOUT_L2L || layout == LAYOUT_T2L ?
                                                 VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
                        unsigned dst_samples = test_flavor == TEST_RESOLVE ? 1 : src_samples;

                        dst[size_factor] = ctx->create_image(ctx, img_type, formats[format_index].format,
                                                             img_width, img_height, img_depth,
                                                             dst_samples, dst_tiling, api_heap_device, 0);

                        fb[size_factor] = ctx->create_framebuffer(ctx, dst[size_factor], NULL,
                                                                  img_width, img_height, dst_samples);
                     }
                  }

                  for (unsigned fill_flavor = 0; fill_flavor < NUM_FILLS; fill_flavor++) {
                     const VkClearColorValue *clear_color =
                        format_is_integer(formats[format_index].format) ?
                           (fill_flavor == FILL_BLACK ? &black_color_uint : &solid_color_uint) :
                           (fill_flavor == FILL_BLACK ? &black_color_float : &solid_color_float);

                     /* Reject invalid combinations. */
                     if ((test_flavor == TEST_FB_CLEAR || test_flavor == TEST_CLEAR) &&
                         fill_flavor != FILL_SOLID && fill_flavor != FILL_BLACK)
                        continue;

                     if ((samples == 1 && fill_flavor >= FILL_RANDOM_FRAGMENTED2) ||
                         (samples == 2 && fill_flavor >= FILL_RANDOM_FRAGMENTED4) ||
                         (samples == 4 && fill_flavor >= FILL_RANDOM_FRAGMENTED8))
                        continue;

                     /* Fill the source texture. */
                     if (stage == RUN) {
                        if (test_flavor != TEST_FB_CLEAR && test_flavor != TEST_CLEAR) {
                           for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                              switch (fill_flavor) {
                              case FILL_BLACK:
                              case FILL_SOLID: {
                                 ctx->clear_image(ctx, src[size_factor],
                                                  &(api_image_box){
                                                     .width = src[size_factor]->width,
                                                     .height = src[size_factor]->height,
                                                     .depth = src[size_factor]->depth,
                                                  },
                                                  clear_color);
                                 break;
                              }

                              case FILL_GRADIENT:
                                 set_gradient_pixels(ctx, src[size_factor]);
                                 break;

                              case FILL_RANDOM:
                                 set_random_pixels(ctx, src[size_factor], 0, random_data);
                                 break;

                              case FILL_RANDOM_FRAGMENTED2:
                                 assert(samples >= 2);
                                 /* Make all samples equal. */
                                 set_random_pixels(ctx, src[size_factor], 0, random_data);
                                 /* Make sample 0 different. */
                                 set_random_pixels(ctx, src[size_factor], 1, random_data);
                                 break;

                              case FILL_RANDOM_FRAGMENTED4:
                                 assert(samples >= 4);
                                 /* Make all samples equal. */
                                 set_random_pixels(ctx, src[size_factor], 0, random_data);
                                 /* Make samples 0..2 different. */
                                 for (unsigned i = 0; i <= 2; i++)
                                    set_random_pixels(ctx, src[size_factor], i + 1, random_data);
                                 break;

                              case FILL_RANDOM_FRAGMENTED8:
                                 assert(samples == 8);
                                 /* Make all samples equal. */
                                 set_random_pixels(ctx, src[size_factor], 0, random_data);
                                 /* Make samples 0..6 different. */
                                 for (unsigned i = 0; i <= 6; i++)
                                    set_random_pixels(ctx, src[size_factor], i + 1, random_data);
                                 break;

                              default:
                                 error("invalid fill flavor");
                              }
                           }
                        }
                     }

                     for (unsigned box_flavor = 0; box_flavor < NUM_BOXES; box_flavor++) {
                        bool yflip = box_flavor == BOX_FULL_YFLIP ||
                                     box_flavor == BOX_PARTIAL_UNALIGNED_YFLIP;

                        /* Reject invalid combinations. */
                        if (test_flavor == TEST_FB_CLEAR && box_flavor != BOX_FULL)
                           continue;

                        if ((test_flavor == TEST_CLEAR || test_flavor == TEST_COPY) && yflip)
                           continue;

                        if (stage == REPORT) {
                           const char *special_op =
                              test_flavor == TEST_FB_CLEAR ? "cleartex" :
                              test_flavor == TEST_CLEAR && box_flavor == BOX_FULL ? "fastclear" :
                              test_flavor == TEST_BLIT && !yflip ? "copy" :
                              test_flavor == TEST_RESOLVE ? "cbresolve" : "n/a";

                           printf("%-8s, %-9s, %uD, %-18s, %u, %-5s, %-11s, %-11s",
                                  test_strings[test_flavor], special_op, img_type + 1,
                                  formats[format_index].name, samples,
                                  layout_strings[layout], fill_strings[fill_flavor],
                                  box_strings[box_flavor]);
                        }

                        for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                           if (stage == COUNT_TESTS)
                              (*num_tests)++;

                           api_image_box src_box = {0}, dst_box = {0};

                           if (stage == RUN) {
                              /* Determine the box. */
                              dst_box.width = dst[size_factor]->width;
                              dst_box.height = dst[size_factor]->height;
                              dst_box.depth = dst[size_factor]->depth;
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
                              assert(dst_box.x + dst_box.width <= dst[size_factor]->width);
                              assert(dst_box.y + dst_box.height <= dst[size_factor]->height);
                              assert(dst_box.z + dst_box.depth <= dst[size_factor]->depth);

                              if (src[size_factor]) {
                                 assert(src_box.width);
                                 assert(src_box.height);
                                 assert(src_box.depth > 0);
                                 if (src_box.width > 0) {
                                    assert(src_box.x >= 0);
                                    assert(src_box.x + src_box.width <= src[size_factor]->width);
                                 } else {
                                    assert(src_box.x + src_box.width >= 0);
                                    assert(src_box.x - 1 < src[size_factor]->width);
                                 }
                                 if (src_box.height > 0) {
                                    assert(src_box.y >= 0);
                                    assert(src_box.y + src_box.height <= src[size_factor]->height);
                                 } else {
                                    assert(src_box.y + src_box.height >= 0);
                                    assert(src_box.y - 1 < src[size_factor]->height);
                                 }
                                 assert(src_box.z >= 0);
                                 assert(src_box.z + src_box.depth <= src[size_factor]->depth);
                              }

                              ctx->begin_cmdbuf(ctx);

                              /* Create pipe_surface for clears. */
                              if (test_flavor == TEST_FB_CLEAR || test_flavor == TEST_CLEAR) {

                                 /* Bind the colorbuffer for FB clears. */
                                 if (test_flavor == TEST_FB_CLEAR) {
                                    ctx->begin_render_pass(ctx,
                                                           &(api_render_pass_desc) {
                                                              .fb = fb[size_factor],
                                                              .color_clear_value = *clear_color,
                                                           });
                                 }
                              }

                              /* Run tests. */
                              for (unsigned i = 0; i < NUM_WARMUP_RUNS + NUM_RUNS; i++) {
                                 /* The first few just warm up caches and the hw. */
                                 if (i == NUM_WARMUP_RUNS)
                                    ctx->write_next_timestamp(ctx, timestamps);

                                 switch (test_flavor) {
                                 case TEST_FB_CLEAR:
                                    break;

                                 case TEST_CLEAR:
                                    ctx->clear_image(ctx, dst[size_factor], &dst_box, clear_color);
                                    break;

                                 case TEST_COPY:
                                 case TEST_BLIT:
                                 case TEST_RESOLVE: {
                                    ctx->blit_image(ctx,
                                                    &(api_blit_desc){
                                                       .dst = dst[size_factor],
                                                       .dst_box = dst_box,
                                                       .src = src[size_factor],
                                                       .src_box = src_box,
                                                       .is_copy = test_flavor == TEST_COPY,
                                                    });
                                    break;
                                 }

                                 default:
                                    error("invalid test flavor");
                                 }
                              }

                              ctx->write_next_timestamp(ctx, timestamps);

                              /* Unbind the colorbuffer. */
                              if (test_flavor == TEST_FB_CLEAR)
                                 ctx->end_render_pass(ctx);

                              ctx->end_cmdbuf_and_submit(ctx);
                           }

                           /* Get results. */
                           if (stage == REPORT) {
                              uint64_t num_pixels = (uint64_t)NUM_RUNS * dst_box.width *
                                                    dst_box.height * dst_box.depth;
                              uint64_t bytes;

                              if (test_flavor == TEST_FB_CLEAR || test_flavor == TEST_CLEAR)
                                 bytes = num_pixels * msaa_pix_size;
                              else if (test_flavor == TEST_RESOLVE)
                                 bytes = num_pixels * (msaa_pix_size + bpe);
                              else
                                 bytes = num_pixels * msaa_pix_size * 2;

                              print_throughput_from_next_timestamps(ctx, timestamps, bytes, NULL,
                                                                    " , %9.2f");
                           }
                        }

                        if (stage == REPORT)
                           printf("\n");
                     }
                  }

                  if (stage == RUN) {
                     ctx->wait_idle_before_deallocation(ctx);

                     for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                        ctx->destroy_framebuffer(ctx, fb[size_factor]);
                        ctx->destroy_image(ctx, dst[size_factor]);
                        if (test_flavor != TEST_FB_CLEAR && test_flavor != TEST_CLEAR)
                           ctx->destroy_image(ctx, src[size_factor]);
                     }
                  }
               }
            }
         }
      }
   }

   free(random_data);
}

void
test_img_bandwidth(api_context *ctx, const char *test_suite_name)
{
   unsigned num_tests = 0;

   run(ctx, test_suite_name, COUNT_TESTS, &num_tests, NULL);

   api_timestamp_query_pool *timestamps = ctx->create_timestamp_pool(ctx, num_tests * 2);

   run(ctx, test_suite_name, RUN, &num_tests, timestamps);

   ctx->query_timestamps(ctx, timestamps);
   run(ctx, test_suite_name, REPORT, NULL, timestamps);
}
