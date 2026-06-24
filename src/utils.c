/* Copyright 2026 Advanced Micro Devices, Inc.
 *
 * For code from vkcube:
 *    Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 *    Copyright (c) 2012 Rob Clark <rob@ti.com>
 *    Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>

#ifndef _WIN32
#include <fcntl.h>   /* for open */
#include <unistd.h>  /* for execl */
#include <errno.h>   /* for execl */
#endif

#include "common.h"

bool
check_filter_string(const char *filter_string, const char *name)
{
   if (!filter_string)
      return true;

   int filter_len = strlen(filter_string);

   /* If the filter ends with $, the test name must contain the filter string at the end. */
   if (filter_len && filter_string[filter_len - 1] == '$') {
      char filter_no_dollar[256];

      snprintf(filter_no_dollar, MIN2(filter_len, ARRAY_SIZE(filter_no_dollar)),
               "%s", filter_string);

      return strstr(name, filter_no_dollar) == name + strlen(name) - (filter_len - 1);
   }

   return strstr(name, filter_string) != 0;
}

double
get_time_in_seconds_from_timestamps(api_context *ctx, api_timestamp_query_pool *pool)
{
   assert(pool->num_read_queries + 1 < pool->num_written_queries);
   uint64_t start = pool->results[pool->num_read_queries++];
   uint64_t end = pool->results[pool->num_read_queries++];
   return (end - start) * ctx->timestamp_period_in_seconds;
}

void
print_throughput_from_next_timestamps(api_context *ctx, api_timestamp_query_pool *pool,
                                      uint64_t num_units, const char *rate_format,
                                      const char *bandwidth_format, unsigned bandwidth_exp2_divisor)
{
   double rate = num_units /  get_time_in_seconds_from_timestamps(ctx, pool);

   if (ctx->options.report_bandwidth) {
      printf(bandwidth_format, rate / (1ull << bandwidth_exp2_divisor));
   } else {
      /* If the frequency is set, print the results in units per clock cycle, else print it
       * in billions per second.
       */
      if (ctx->options.freq_mhz)
         rate *= 1.0 / (ctx->options.freq_mhz * 1000000.0);
      else
         rate *= 1.0 / 1000000000;

      /* If max_rate is set, print the results as % of the max_rate. */
      if (ctx->options.max_rate)
         rate *= 100.0 / ctx->options.max_rate;

      printf(rate_format, rate);
   }
}

void printflike(1, 2)
error(const char *format, ...)
{
   va_list args;

   va_start(args, format);
   vfprintf(stderr, format, args);
   fprintf(stderr, "\n");
   exit(1);
}

char *
strdup(const char *s)
{
   int len = strlen(s);

   char *dup = malloc(len + 1);
   memcpy(dup, s, len + 1);
   return dup;
}

void
print_progress(unsigned num_items, unsigned *num_processed_items, unsigned print_period)
{
   unsigned num = atomic_fetch_add(num_processed_items, 1);

   if (num_items / print_period && /* prevent mod by 0 */
       num % (num_items / print_period) == (num_items / print_period - 1)) {
      printf(" %.0f%%", 100 * (double)num / num_items);
      fflush(stdout);
   }
}

void
write_png_rgba8(const char *path, api_image *image_info, uint8_t *pixels)
{
   FILE *f = NULL;
   png_structp png_writer = NULL;
   png_infop png_info = NULL;

   uint8_t **rows = (uint8_t **)malloc(sizeof(rows[0]) * image_info->height);

   for (unsigned y = 0; y < image_info->height; y++)
      rows[y] = pixels + y * image_info->width * 4;

   f = fopen(path, "wb");
   if (!f)
      error("failed to open file for writing: %s", path);

   png_writer = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                        NULL, NULL, NULL);
   if (!png_writer)
      error("failed to create png writer");

   png_info = png_create_info_struct(png_writer);
   if (!png_info)
      error("failed to create png writer info");

   png_init_io(png_writer, f);
   png_set_IHDR(png_writer, png_info,
                image_info->width, image_info->height,
                8, PNG_COLOR_TYPE_RGBA,
                PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                PNG_FILTER_TYPE_DEFAULT);
   png_write_info(png_writer, png_info);
   png_set_rows(png_writer, png_info, rows);
   png_set_write_user_transform_fn(png_writer, NULL);
   png_write_png(png_writer, png_info, PNG_TRANSFORM_IDENTITY, NULL);

   png_destroy_write_struct(&png_writer, &png_info);

   fclose(f);
   free(rows);
   printf("Image written to: %s\n", path);
}

void
run_image_viewer(const char *image_filename)
{
   if (execl("/usr/bin/xdg-open", "xdg-open", image_filename, NULL) == -1)
      error("failed to run an image viewer for %s (errno=%i)", image_filename, errno);
}

unsigned
get_pixel_size_from_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
      return 1;

   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return 2;

   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_SINT_PACK32:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_D32_SFLOAT:
      return 4;

   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
      return 8;

   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
      return 16;

   default:
      error("unexpected format in get_pixel_size_from_format: %u", format);
      return 0;
   }
}

bool
format_is_integer(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return false;

   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_SINT_PACK32:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
      return true;

   default:
      error("unexpected format in format_is_integer: %u", format);
      return 0;
   }
}

bool
format_is_sint(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32B32A32_UINT:
      return false;

   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A2B10G10R10_SINT_PACK32:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32B32A32_SINT:
      return true;

   default:
      error("unexpected format in format_is_integer: %u", format);
      return 0;
   }
}

unsigned
format_get_num_channels(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
      return 1;

   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
      return 2;

   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
      return 3;

   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_SINT_PACK32:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 4;

   default:
      error("unexpected format in format_get_num_channels: %u", format);
      return 0;
   }
}

bool
format_is_depth_or_stencil(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_SINT_PACK32:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return false;

   case VK_FORMAT_D32_SFLOAT:
      return true;

   default:
      error("unexpected format in format_is_depth_or_stencil: %u", format);
      return 0;
   }
}

unsigned
get_next_power_of_two(unsigned x)
{
   unsigned val = x;

   if (x <= 1)
      return 1;

   if (IS_POT(x))
      return x;

   val--;
   val = (val >> 1) | val;
   val = (val >> 2) | val;
   val = (val >> 4) | val;
   val = (val >> 8) | val;
   val = (val >> 16) | val;
   val++;
   return val;
}

/**
 * Convert a 4-byte float to a 2-byte half float. Copied from Mesa.
 *
 * Not all float32 values can be represented exactly as a float16 value. We
 * round such intermediate float32 values to the nearest float16. When the
 * float32 lies exactly between to float16 values, we round to the one with
 * an even mantissa.
 *
 * This rounding behavior has several benefits:
 *   - It has no sign bias.
 *
 *   - It reproduces the behavior of real hardware: opcode F32TO16 in Intel's
 *     GPU ISA.
 *
 *   - By reproducing the behavior of the GPU (at least on Intel hardware),
 *     compile-time evaluation of constant packHalf2x16 GLSL expressions will
 *     result in the same value as if the expression were executed on the GPU.
 */
uint16_t
float_to_half(float val)
{
   typedef union { float f; int32_t i; uint32_t u; } fi_type;

   const fi_type fi = {val};
   const int flt_m = fi.i & 0x7fffff;
   const int flt_e = (fi.i >> 23) & 0xff;
   const int flt_s = (fi.i >> 31) & 0x1;
   int s, e, m = 0;
   uint16_t result;

   /* sign bit */
   s = flt_s;

   /* handle special cases */
   if ((flt_e == 0) && (flt_m == 0)) {
      /* zero */
      /* m = 0; - already set */
      e = 0;
   }
   else if ((flt_e == 0) && (flt_m != 0)) {
      /* denorm -- denorm float maps to 0 half */
      /* m = 0; - already set */
      e = 0;
   }
   else if ((flt_e == 0xff) && (flt_m == 0)) {
      /* infinity */
      /* m = 0; - already set */
      e = 31;
   }
   else if ((flt_e == 0xff) && (flt_m != 0)) {
      /* Retain the top bits of a NaN to make sure that the quiet/signaling
       * status stays the same.
       */
      m = flt_m >> 13;
      if (!m)
         m = 1;
      e = 31;
   }
   else {
      /* regular number */
      const int new_exp = flt_e - 127;
      if (new_exp < -14) {
         /* The float32 lies in the range (0.0, min_normal16) and is rounded
          * to a nearby float16 value. The result will be either zero, subnormal,
          * or normal.
          */
         e = 0;
         m = lrintf((1 << 24) * fabsf(fi.f));
      }
      else if (new_exp > 15) {
         /* map this value to infinity */
         /* m = 0; - already set */
         e = 31;
      }
      else {
         /* The float32 lies in the range
          *   [min_normal16, max_normal16 + max_step16)
          * and is rounded to a nearby float16 value. The result will be
          * either normal or infinite.
          */
         e = new_exp + 15;
         m = lrintf(flt_m / (float) (1 << 13));
      }
   }

   assert(0 <= m && m <= 1024);
   if (m == 1024) {
      /* The float32 was rounded upwards into the range of the next exponent,
       * so bump the exponent. This correctly handles the case where f32
       * should be rounded up to float16 infinity.
       */
      ++e;
      m = 0;
   }

   result = (s << 15) | (e << 10) | m;
   return result;
}

unsigned
bitcount(unsigned n)
{
   /* K&R classic bitcount.
    *
    * For each iteration, clear the LSB from the bitfield.
    * Requires only one iteration per set bit, instead of
    * one iteration per bit less than highest set bit.
    */
   unsigned bits;
   for (bits = 0; n; bits++) {
      n &= n - 1;
   }
   return bits;
}

unsigned
logbase2(unsigned n)
{
   unsigned pos = 0;
   if (n >= 1 << 16) { n >>= 16; pos += 16; }
   if (n >= 1 <<  8) { n >>=  8; pos +=  8; }
   if (n >= 1 <<  4) { n >>=  4; pos +=  4; }
   if (n >= 1 <<  2) { n >>=  2; pos +=  2; }
   if (n >= 1 <<  1) {           pos +=  1; }
   return pos;
}

const char *
heap_to_string(api_heap_type heap)
{
   static const char *table[] = {
      [api_heap_device] = "devmem",
      [api_heap_device_coherent_amd] = "devmem_coherent",
      [api_heap_host_uncached] = "hostmem",
      [api_heap_host_uncached_coherent_amd] = "hostmem_coherent",
      [api_heap_host_cached] = "hostmem_cached",
   };

   return table[heap];
}

const char *
queue_to_string(api_queue_type queue)
{
   static const char *table[] = {
      [api_queue_gfx] = "Graphics",
      [api_queue_compute] = "Compute",
      [api_queue_transfer] = "Transfer",
      [api_queue_sparse] = "Sparse",
   };

   return table[queue];
}
