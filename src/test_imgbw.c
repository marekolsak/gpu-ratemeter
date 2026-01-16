/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define RANDOM_DATA_SIZE (611953 * 8) /* prime number * 8 */

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

   ctx->upload_image_data(ctx, img, img_stride_in_bytes, sample_index - 1, data);
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

   ctx->upload_image_data(ctx, img, img_stride_in_bytes, -1, data);

   free(line);
   free(data);
}

static VkFormat formats[] = {
   VK_FORMAT_R8_UNORM,
   VK_FORMAT_R8_UINT,
   VK_FORMAT_R16_UINT,
   VK_FORMAT_R16_SFLOAT,
   VK_FORMAT_R8G8B8A8_UNORM,
   VK_FORMAT_R32_UINT,
   VK_FORMAT_R32_SFLOAT,
   VK_FORMAT_R32G32_UINT,
   VK_FORMAT_R32G32_SFLOAT,
   VK_FORMAT_R16G16B16A16_SFLOAT,
   VK_FORMAT_R32G32B32A32_UINT,
   VK_FORMAT_R32G32B32A32_SFLOAT,
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

enum {
   METHOD_DEFAULT,
   METHOD_GFX,
   METHOD_COMPUTE,
   METHOD_SPECIAL,
   NUM_METHODS,
};

static const VkClearColorValue black_color_float = {.float32 = {0, 0, 0, 0}};
static const VkClearColorValue solid_color_float = {.float32 = {0.2, 0.3, 0.4, 0.5}};
static const VkClearColorValue black_color_uint = {.uint32 = {0, 0, 0, 0}};
static const VkClearColorValue solid_color_uint = {.uint32 = {23, 45, 89, 107}};

void
test_img_bandwidth(api_context *ctx, const char *test_suite_name)
{
   uint64_t seed_xorshift128plus[2];
   uint64_t random_data[RANDOM_DATA_SIZE / 8];

   /* Set the seed for random pixel data */
   s_rand_xorshift128plus(seed_xorshift128plus, false);

   /* Pre-generate random data for initializing textures. */
   for (unsigned i = 0; i < ARRAY_SIZE(random_data); i++)
      random_data[i] = rand_xorshift128plus(seed_xorshift128plus);

   printf("Op      , Special  ,Dim, Format            ,MS,Layout, Fill       , Box         ,"
          "   small   ,   small   ,   small   ,   small   ,   LARGE   ,   LARGE   ,   LARGE   ,   LARGE\n");
   printf("--------,----------,---,-------------------,--,------,------------,-------------,"
          "  Default  ,    Gfx    ,  Compute  ,  Special  ,  Default  ,    Gfx    ,  Compute  ,  Special\n");

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

                  if (test_flavor == TEST_RESOLVE && format_is_integer(formats[format_index]))
                     continue;

                  /* Create textures. */
                  api_image *src[2] = {0}, *dst[2] = {0};
                  unsigned bpe = get_pixel_size_from_format(formats[format_index]);
                  unsigned pix_size = bpe * samples;

                  for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                     unsigned mb_size = (size_factor ? 256 : 8) * 1024 * 1024;
                     unsigned width = 1, height = 1, depth = 1;

                     /* Determine the size. The footprint must be exactly "mb_size" for 2D and 3D. */
                     if (img_type == VK_IMAGE_TYPE_1D) {
                        width = size_factor ? 16384 : 2048;
                     } else if (img_type == VK_IMAGE_TYPE_2D) {
                        width = height = get_next_power_of_two(sqrt(mb_size / pix_size));

                        for (unsigned i = 0; width * height * pix_size != mb_size; i++) {
                           if (i % 2 == 1)
                              width /= 2;
                           else
                              height /= 2;
                        }
                     } else if (img_type == VK_IMAGE_TYPE_3D) {
                        width = height = depth = get_next_power_of_two(pow(mb_size / pix_size, 0.333333));

                        for (unsigned i = 0; width * height * depth * pix_size != mb_size; i++) {
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
                        src[size_factor] = ctx->create_image(ctx, img_type, formats[format_index],
                                                             img_width, img_height, img_depth,
                                                             src_samples, src_tiling,
                                                             api_heap_device, 0);
                     }

                     unsigned dst_tiling = layout == LAYOUT_L2L || layout == LAYOUT_T2L ?
                                              VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
                     unsigned dst_samples = test_flavor == TEST_RESOLVE ? 1 : src_samples;

                     dst[size_factor] = ctx->create_image(ctx, img_type, formats[format_index],
                                                          img_width, img_height, img_depth,
                                                          dst_samples, dst_tiling, api_heap_device, 0);
                  }

                  for (unsigned fill_flavor = 0; fill_flavor < NUM_FILLS; fill_flavor++) {
                     const VkClearColorValue *clear_color =
                        format_is_integer(formats[format_index]) ?
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
                     if (test_flavor != TEST_FB_CLEAR && test_flavor != TEST_CLEAR) {
                        for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                           switch (fill_flavor) {
                           case FILL_BLACK:
                           case FILL_SOLID: {
                              union util_color packed_color;
                              util_pack_color(clear_color->float32, formats[format_index], &packed_color);

                              struct pipe_box box = {0};
                              box.width = src[size_factor]->width;
                              box.height = src[size_factor]->height;
                              box.depth = src[size_factor]->depth;

                              ctx->clear_texture(ctx, src[size_factor], 0, &box, &packed_color);
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

                        const char *special_op =
                           test_flavor == TEST_FB_CLEAR ? "cleartex" :
                           test_flavor == TEST_CLEAR && box_flavor == BOX_FULL ? "fastclear" :
                           test_flavor == TEST_BLIT && !yflip ? "copy" :
                           test_flavor == TEST_RESOLVE ? "cbresolve" : "n/a";

                        printf("%-8s, %-9s, %uD, %-18s, %u, %-5s, %-11s, %-11s",
                               test_strings[test_flavor], special_op, img_type,
                               util_format_short_name(formats[format_index]), samples,
                               layout_strings[layout], fill_strings[fill_flavor],
                               box_strings[box_flavor]);

                        for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                           /* Determine the box. */
                           struct pipe_box src_box = {0}, dst_box = {0};
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
                              if (img_type == 1) {
                                 dst_box.x = 256;
                                 dst_box.width -= 256;
                              } else if (img_type == 2) {
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
                              if (img_type >= 2) {
                                 dst_box.y = off;
                                 dst_box.height -= off;
                                 if (img_type == 3) {
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

                           for (unsigned method = 0; method < NUM_METHODS; method++) {
                              struct pipe_surface surf_templ;

                              /* Create pipe_surface for clears. */
                              if (test_flavor == TEST_FB_CLEAR || test_flavor == TEST_CLEAR) {

                                 u_surface_default_template(&surf_templ, dst[size_factor]);
                                 surf_templ.last_layer = dst[size_factor]->depth - 1;

                                 /* Bind the colorbuffer for FB clears. */
                                 if (box_flavor == BOX_FULL) {
                                    struct pipe_framebuffer_state fb = {0};
                                    fb.width = dst[size_factor]->width;
                                    fb.height = dst[size_factor]->height;
                                    fb.layers = dst[size_factor]->depth;
                                    fb.samples = dst[size_factor]->samples;
                                    fb.nr_cbufs = 1;
                                    fb.cbufs[0] = surf_templ;
                                    ctx->set_framebuffer_state(ctx, &fb);
                                    si_emit_barrier_direct(sctx);
                                 }
                              }

                              struct pipe_query *q = ctx->create_query(ctx, PIPE_QUERY_TIME_ELAPSED, 0);
                              unsigned num_warmup_repeats = 1, num_repeats = 4;
                              bool success = true;

                              /* Run tests. */
                              for (unsigned i = 0; i < num_warmup_repeats + num_repeats; i++) {
                                 /* The first few just warm up caches and the hw. */
                                 if (i == num_warmup_repeats)
                                    ctx->begin_query(ctx, q);

                                 switch (test_flavor) {
                                 case TEST_FB_CLEAR:
                                 case TEST_CLEAR:
                                    switch (method) {
                                    case METHOD_DEFAULT:
                                       if (test_flavor == TEST_FB_CLEAR) {
                                          ctx->clear(ctx, PIPE_CLEAR_COLOR, NULL, clear_color, 0, 0);
                                          sctx->barrier_flags |= SI_BARRIER_SYNC_AND_INV_CB | SI_BARRIER_INV_L2;
                                       } else {
                                          ctx->clear_render_target(ctx, &surf_templ, clear_color,
                                                                   dst_box.x, dst_box.y,
                                                                   dst_box.width, dst_box.height,
                                                                   false);
                                       }
                                       break;
                                    case METHOD_GFX:
                                       si_gfx_clear_render_target(ctx, &surf_templ, clear_color,
                                                                  dst_box.x, dst_box.y,
                                                                  dst_box.width, dst_box.height,
                                                                  false);
                                       break;
                                    case METHOD_COMPUTE:
                                       success &=
                                          si_compute_clear_image(sctx, surf_templ.texture,
                                                                 surf_templ.format, 0, &dst_box,
                                                                 clear_color, false, false);
                                       break;
                                    case METHOD_SPECIAL:
                                       if (test_flavor == TEST_CLEAR) {
                                          success &=
                                             si_compute_fast_clear_image(sctx, surf_templ.texture,
                                                                         surf_templ.format, 0,
                                                                         &dst_box, clear_color,
                                                                         false, false);
                                       } else {
                                          ctx->clear_render_target(ctx, &surf_templ, clear_color,
                                                                   dst_box.x, dst_box.y,
                                                                   dst_box.width, dst_box.height,
                                                                   false);
                                       }
                                       break;
                                    }
                                    break;

                                 case TEST_COPY:
                                    switch (method) {
                                    case METHOD_DEFAULT:
                                       si_resource_copy_region(ctx, dst[size_factor], 0, dst_box.x,
                                                               dst_box.y, dst_box.z, src[size_factor],
                                                               0, &src_box);
                                       break;
                                    case METHOD_GFX:
                                       si_gfx_copy_image(sctx, dst[size_factor], 0, dst_box.x,
                                                         dst_box.y, dst_box.z, src[size_factor],
                                                         0, &src_box);
                                       break;
                                    case METHOD_COMPUTE:
                                       success &= si_compute_copy_image(sctx, dst[size_factor], 0,
                                                                        src[size_factor], 0,
                                                                        dst_box.x, dst_box.y,
                                                                        dst_box.z, &src_box, false);
                                       break;
                                    case METHOD_SPECIAL:
                                       success = false;
                                       break;
                                    }
                                    break;

                                 case TEST_BLIT:
                                 case TEST_RESOLVE: {
                                    struct pipe_blit_info info;
                                    memset(&info, 0, sizeof(info));
                                    info.dst.resource = dst[size_factor];
                                    info.dst.level = 0;
                                    info.dst.box = dst_box;
                                    info.dst.format = formats[format_index];
                                    info.src.resource = src[size_factor];
                                    info.src.level = 0;
                                    info.src.box = src_box;
                                    info.src.format = formats[format_index];
                                    info.mask = PIPE_MASK_RGBA;

                                    switch (method) {
                                    case METHOD_DEFAULT:
                                       ctx->blit(ctx, &info);
                                       break;
                                    case METHOD_GFX:
                                       si_gfx_blit(ctx, &info);
                                       break;
                                    case METHOD_COMPUTE:
                                       success &= si_compute_blit(sctx, &info, NULL, 0, 0, false);
                                       break;
                                    case METHOD_SPECIAL:
                                       if (test_flavor == TEST_BLIT && !yflip) {
                                          si_resource_copy_region(ctx, dst[size_factor], 0, dst_box.x,
                                                                  dst_box.y, dst_box.z, src[size_factor],
                                                                  0, &src_box);
                                       } else if (test_flavor == TEST_RESOLVE) {
                                          success &= si_msaa_resolve_blit_via_CB(ctx, &info, false);
                                       } else {
                                          success = false;
                                       }
                                       break;
                                    }
                                    break;
                                 }
                                 }
                              }

                              ctx->end_query(ctx, q);

                              /* Wait for idle after all tests. */
                              sctx->barrier_flags |= SI_BARRIER_SYNC_AND_INV_CB |
                                                     SI_BARRIER_SYNC_CS |
                                                     SI_BARRIER_INV_L2 | SI_BARRIER_INV_SMEM |
                                                     SI_BARRIER_INV_VMEM;
                              si_emit_barrier_direct(sctx);

                              /* Unbind the colorbuffer. */
                              if ((test_flavor == TEST_FB_CLEAR || test_flavor == TEST_CLEAR) &&
                                  box_flavor == BOX_FULL) {
                                 struct pipe_framebuffer_state fb = {0};
                                 fb.width = 64;
                                 fb.height = 64;
                                 fb.layers = 1;
                                 fb.samples = 1;
                                 ctx->set_framebuffer_state(ctx, &fb);
                              }

                              /* Get results. */
                              if (success) {
                                 union pipe_query_result result;
                                 ctx->get_query_result(ctx, q, true, &result);
                                 ctx->destroy_query(ctx, q);

                                 double sec = (double)result.u64 / (1000 * 1000 * 1000);
                                 uint64_t pixels_per_surf = num_repeats * (uint64_t) dst_box.width *
                                                            dst_box.height * dst_box.depth;
                                 uint64_t bytes;

                                 if (test_flavor == TEST_FB_CLEAR || test_flavor == TEST_CLEAR)
                                    bytes = pixels_per_surf * pix_size;
                                 else if (test_flavor == TEST_RESOLVE)
                                    bytes = pixels_per_surf * (pix_size + bpe);
                                 else
                                    bytes = pixels_per_surf * pix_size * 2;

                                 double bytes_per_sec = bytes / sec;

                                 printf(" , %9.2f", bytes_per_sec / (1024 * 1024 * 1024));
                              } else {
                                 printf(" ,     n/a  ");
                              }
                           }
                        }

                        printf("\n");
                     }
                  }

                  // TODO: wait for idle or do it later?
                  for (unsigned size_factor = 0; size_factor <= 1; size_factor++) {
                     ctx->destroy_image(ctx, dst[size_factor]);
                     ctx->destroy_image(ctx, src[size_factor]);
                  }
               }
            }
         }
      }
   }
}
