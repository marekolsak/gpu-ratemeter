/* Copyright 2026 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include "glad/glad.h"

#define GL_PRIVATE
#include "common.h"

#define egl_check(call) \
   do { \
      if (!(call)) \
         error("EGL call failed: %s", #call); \
   } while(0)

#define gl_check_no_error() \
   do { \
      GLenum err = glGetError(); \
      if (err != GL_NO_ERROR) \
         error("GL error 0x%04X on line %i", err, __LINE__); \
   } while(0)

static api_buffer *
gl_create_buffer(api_context *ctx, uint64_t size, api_heap_type heap, unsigned sparse_block_size)
{
   api_buffer *buf = calloc(1, sizeof(api_buffer));
   buf->size = size;
   buf->heap = heap;

   assert(!sparse_block_size && "not implemented");

   unsigned flags = GL_DYNAMIC_STORAGE_BIT; /* needed by glBufferSubData */

   switch (heap) {
   case api_heap_device:
      break;
   case api_heap_host_uncached:
      flags |= GL_CLIENT_STORAGE_BIT |  /* should allocate in host memory */
               GL_MAP_WRITE_BIT;  /* should be uncached if READ_BIT is not set */
      break;
   case api_heap_host_cached:
      flags |= GL_CLIENT_STORAGE_BIT |  /* should allocate in host memory */
               GL_MAP_WRITE_BIT |
               GL_MAP_READ_BIT;   /* should be cached */
      break;
   default:
      error("invalid heap type");
   }

   glCreateBuffers(1, &buf->id);
   glNamedBufferStorage(buf->id, size, NULL, flags);
   gl_check_no_error();

   if (heap == api_heap_device)
      atomic_fetch_add(&ctx->device_mem_usage_mb, size >> 20);
   return buf;
}

static void
gl_destroy_buffer(struct api_context *ctx, api_buffer *buf)
{
   glDeleteBuffers(1, &buf->id);

   if (buf->heap == api_heap_device)
      atomic_fetch_sub(&ctx->device_mem_usage_mb, buf->size >> 20);

   free(buf);
}

static void
gl_upload_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      const void *data)
{
   glNamedBufferSubData(buf->id, offset, size, data);
   gl_check_no_error();
}

static void
gl_get_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size, void *data)
{
   glGetNamedBufferSubData(buf->id, offset, size, data);
   gl_check_no_error();
}

static void
gl_clear_buffer(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size, uint32_t value)
{
   glClearNamedBufferSubData(buf->id, GL_R32UI, offset, size, GL_RED_INTEGER, GL_UNSIGNED_INT,
                             &value);
   gl_check_no_error();
}

static void
gl_copy_buffer(api_context *ctx, api_buffer *dst, api_buffer *src, uint64_t dst_offset,
               uint64_t src_offset, uint64_t size)
{
   glCopyNamedBufferSubData(src->id, dst->id, src_offset, dst_offset, size);
   gl_check_no_error();
}

static GLenum
get_gl_internalformat(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UINT:
      return GL_R8UI;
   case VK_FORMAT_R8_SINT:
      return GL_R8I;
   case VK_FORMAT_R8_UNORM:
      return GL_R8;
   case VK_FORMAT_R8_SNORM:
      return GL_R8_SNORM;

   case VK_FORMAT_R8G8_UINT:
      return GL_RG8UI;
   case VK_FORMAT_R8G8_SINT:
      return GL_RG8I;
   case VK_FORMAT_R8G8_UNORM:
      return GL_RG8;
   case VK_FORMAT_R8G8_SNORM:
      return GL_RG8_SNORM;

   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
      return GL_RGBA4;
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
      return GL_RGB5_A1;
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return GL_RGB565;

   case VK_FORMAT_R8G8B8A8_UNORM:
      return GL_RGBA8;
   case VK_FORMAT_R8G8B8A8_SNORM:
      return GL_RGBA8_SNORM;
   case VK_FORMAT_R8G8B8A8_UINT:
      return GL_RGBA8UI;
   case VK_FORMAT_R8G8B8A8_SINT:
      return GL_RGBA8I;

   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      return GL_RGB10_A2UI;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return GL_RGB10_A2;

   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      return GL_R11F_G11F_B10F;
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
      return GL_RGB9_E5;

   case VK_FORMAT_R16_SFLOAT:
      return GL_R16F;
   case VK_FORMAT_R16_UINT:
      return GL_R16UI;
   case VK_FORMAT_R16_SINT:
      return GL_R16I;
   case VK_FORMAT_R16_UNORM:
      return GL_R16;
   case VK_FORMAT_R16_SNORM:
      return GL_R16_SNORM;

   case VK_FORMAT_R16G16_SFLOAT:
      return GL_RG16F;
   case VK_FORMAT_R16G16_UINT:
      return GL_RG16UI;
   case VK_FORMAT_R16G16_SINT:
      return GL_RG16I;
   case VK_FORMAT_R16G16_UNORM:
      return GL_RG16;
   case VK_FORMAT_R16G16_SNORM:
      return GL_RG16_SNORM;

   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return GL_RGBA16F;
   case VK_FORMAT_R16G16B16A16_UINT:
      return GL_RGBA16UI;
   case VK_FORMAT_R16G16B16A16_SINT:
      return GL_RGBA16I;
   case VK_FORMAT_R16G16B16A16_UNORM:
      return GL_RGBA16;
   case VK_FORMAT_R16G16B16A16_SNORM:
      return GL_RGBA16_SNORM;

   case VK_FORMAT_R32_SFLOAT:
      return GL_R32F;
   case VK_FORMAT_R32_UINT:
      return GL_R32UI;
   case VK_FORMAT_R32_SINT:
      return GL_R32I;

   case VK_FORMAT_R32G32_SFLOAT:
      return GL_RG32F;
   case VK_FORMAT_R32G32_UINT:
      return GL_RG32UI;
   case VK_FORMAT_R32G32_SINT:
      return GL_RG32I;

   case VK_FORMAT_R32G32B32_SFLOAT:
      return GL_RGB32F;
   case VK_FORMAT_R32G32B32_UINT:
      return GL_RGB32UI;
   case VK_FORMAT_R32G32B32_SINT:
      return GL_RGB32I;

   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return GL_RGBA32F;
   case VK_FORMAT_R32G32B32A32_UINT:
      return GL_RGBA32UI;
   case VK_FORMAT_R32G32B32A32_SINT:
      return GL_RGBA32I;

   case VK_FORMAT_D32_SFLOAT:
      return GL_DEPTH_COMPONENT32F;

   default:
      error("get_gl_internalformat: unexpected image format %u", format);
      return 0;
   }
}

static GLenum
get_gl_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32_SINT:
      return GL_RED_INTEGER;

   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R32_SFLOAT:
      return GL_RED;

   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32_SINT:
      return GL_RG_INTEGER;

   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R32G32_SFLOAT:
      return GL_RG;

   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
   case VK_FORMAT_R32G32B32_SFLOAT:
      return GL_RGB;

   case VK_FORMAT_R32G32B32_UINT:
   case VK_FORMAT_R32G32B32_SINT:
      return GL_RGB_INTEGER;

   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_UNORM:
   case VK_FORMAT_R16G16B16A16_SNORM:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return GL_RGBA;

   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R8G8B8A8_SINT:
   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32G32B32A32_SINT:
      return GL_RGBA_INTEGER;

   case VK_FORMAT_D32_SFLOAT:
      return GL_DEPTH_COMPONENT;

   default:
      error("get_gl_format: unexpected image format %u", format);
      return 0;
   }
}

static GLenum
get_gl_type(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UINT:
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8G8_UINT:
   case VK_FORMAT_R8G8_UNORM:
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_UINT:
      return GL_UNSIGNED_BYTE;

   case VK_FORMAT_R8_SINT:
   case VK_FORMAT_R8_SNORM:
   case VK_FORMAT_R8G8_SINT:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8B8A8_SINT:
      return GL_BYTE;

   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
      return GL_UNSIGNED_SHORT_4_4_4_4;

   case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
      return GL_UNSIGNED_SHORT_5_5_5_1;

   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return GL_UNSIGNED_SHORT_5_6_5;

   case VK_FORMAT_A2B10G10R10_UINT_PACK32:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return GL_UNSIGNED_INT_2_10_10_10_REV;

   case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      return GL_UNSIGNED_INT_10F_11F_11F_REV;

   case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
      return GL_UNSIGNED_INT_5_9_9_9_REV;

   case VK_FORMAT_R16_SFLOAT:
   case VK_FORMAT_R16G16_SFLOAT:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return GL_HALF_FLOAT;

   case VK_FORMAT_R16_UINT:
   case VK_FORMAT_R16_UNORM:
   case VK_FORMAT_R16G16_UINT:
   case VK_FORMAT_R16G16_UNORM:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R16G16B16A16_UNORM:
      return GL_UNSIGNED_SHORT;

   case VK_FORMAT_R16_SINT:
   case VK_FORMAT_R16_SNORM:
   case VK_FORMAT_R16G16_SINT:
   case VK_FORMAT_R16G16_SNORM:
   case VK_FORMAT_R16G16B16A16_SINT:
   case VK_FORMAT_R16G16B16A16_SNORM:
      return GL_SHORT;

   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
   case VK_FORMAT_D32_SFLOAT:
      return GL_FLOAT;

   case VK_FORMAT_R32_UINT:
   case VK_FORMAT_R32G32_UINT:
   case VK_FORMAT_R32G32B32_UINT:
   case VK_FORMAT_R32G32B32A32_UINT:
      return GL_UNSIGNED_INT;

   case VK_FORMAT_R32_SINT:
   case VK_FORMAT_R32G32_SINT:
   case VK_FORMAT_R32G32B32_SINT:
   case VK_FORMAT_R32G32B32A32_SINT:
      return GL_INT;

   default:
      error("get_gl_type: unexpected image format %u", format);
      return 0;
   }
}

/* TODO: Regenerate GLAD. */
#define GL_TEXTURE_TILING_EXT             0x9580
#define GL_LINEAR_TILING_EXT              0x9585

static void
create_texture(api_context *ctx, api_image *image, GLenum target, VkImageTiling tiling)
{
   image->gltarget = target;
   glCreateTextures(target, 1, &image->id);

   if (tiling == VK_IMAGE_TILING_LINEAR) {
      if (ctx->has_image_tiling_linear) {
         glTextureParameteri(image->id, GL_TEXTURE_TILING_EXT, GL_LINEAR_TILING_EXT);
      } else {
         error("GL doesn't support linear tiling");
      }
   }
}

static api_image *
gl_create_image(api_context *ctx, VkImageType type, VkFormat format, unsigned width, unsigned height,
                unsigned depth, unsigned samples, VkImageTiling tiling, api_heap_type heap)
{
   api_image *image = calloc(1, sizeof(api_image));
   image->type = type;
   image->width = width;
   image->height = height;
   image->depth = depth;
   image->samples = samples;
   image->format = format;
   image->heap = heap;
   image->glformat = get_gl_format(format);
   image->gltype = get_gl_type(format);
   image->mem_size = (uint64_t)width * height * get_pixel_size_from_format(format) * samples;

   if (heap != api_heap_device)
      error("GL only supports heap=device for textures");

   image->glinternalformat = get_gl_internalformat(format);

   switch (type) {
   case VK_IMAGE_TYPE_1D:
      assert(height == 1 && depth == 1 && samples == 1);
      create_texture(ctx, image, GL_TEXTURE_1D, tiling);
      glTextureStorage1D(image->id, 1, image->glinternalformat, width);
      break;

   case VK_IMAGE_TYPE_2D:
      if (samples > 1) {
         if (depth > 1) {
            create_texture(ctx, image, GL_TEXTURE_2D_MULTISAMPLE_ARRAY, tiling);
            glTextureStorage3DMultisample(image->id, samples, image->glinternalformat, width, height, depth, true);
         } else {
            create_texture(ctx, image, GL_TEXTURE_2D_MULTISAMPLE, tiling);
            glTextureStorage2DMultisample(image->id, samples, image->glinternalformat, width, height, true);
         }
      } else {
         if (depth > 1) {
            create_texture(ctx, image, GL_TEXTURE_2D_ARRAY, tiling);
            glTextureStorage3D(image->id, 1, image->glinternalformat, width, height, depth);
         } else {
            create_texture(ctx, image, GL_TEXTURE_2D, tiling);
            glTextureStorage2D(image->id, 1, image->glinternalformat, width, height);
         }
      }
      break;

   case VK_IMAGE_TYPE_3D:
      assert(samples == 1);
      create_texture(ctx, image, GL_TEXTURE_3D, tiling);
      glTextureStorage3D(image->id, 1, image->glinternalformat, width, height, depth);
      break;

   default:
      error("gl_create_image: unexpected image type");
   }

   gl_check_no_error();

   atomic_fetch_add(&ctx->device_mem_usage_mb, image->mem_size >> 20);
   return image;
}

static void
gl_destroy_image(api_context *ctx, api_image *image)
{
   atomic_fetch_sub(&ctx->device_mem_usage_mb, image->mem_size >> 20);
   glDeleteTextures(1, &image->id);
   free(image);
}

static void
gl_clear_image(api_context *ctx, api_image *image, const api_image_box *box,
               const VkClearColorValue *value)
{
   assert(!format_is_depth_or_stencil(image->format));
   GLenum format, type;

   if (format_is_integer(image->format)) {
      format = GL_RGBA_INTEGER;
      type = format_is_sint(image->format) ? GL_INT : GL_UNSIGNED_INT;
   } else {
      format = GL_RGBA;
      type = GL_FLOAT;
   }

   if (box) {
      glClearTexSubImage(image->id, 0, box->x, box->y, box->z, box->width, box->height, box->depth,
                         format, type, value->uint32);
   } else {
      glClearTexImage(image->id, 0, format, type, value->uint32);
   }
   gl_check_no_error();
}

static void
gl_blit_image(api_context *ctx, api_blit_desc *desc)
{
   assert(!format_is_depth_or_stencil(desc->src->format));
   assert(!format_is_depth_or_stencil(desc->dst->format));

   if (desc->is_copy) {
      assert(desc->dst_box.width == desc->src_box.width);
      assert(desc->dst_box.height == desc->src_box.height);
      assert(desc->dst_box.depth == desc->src_box.depth);
      assert(desc->dst_box.width > 0 &&desc->src_box.width > 0);
      assert(desc->dst_box.height > 0 &&desc->src_box.height > 0);
      assert(desc->dst_box.depth > 0 &&desc->src_box.depth > 0);
      assert(desc->dst->samples == desc->src->samples);
      assert(!desc->linear_filter);

      glCopyImageSubData(desc->src->id, desc->src->gltarget, 0,
                         desc->src_box.x, desc->src_box.y, desc->src_box.z,
                         desc->dst->id, desc->dst->gltarget, 0,
                         desc->dst_box.x, desc->dst_box.y, desc->dst_box.z,
                         desc->src_box.width, desc->src_box.height, desc->src_box.depth);
   } else {
      /* GL doesn't have 3D blits. */
      assert(desc->src_box.depth == 1 && desc->dst_box.depth == 1);
      GLuint dst_fbo, src_fbo;

      glCreateFramebuffers(1, &dst_fbo);
      if (desc->dst->depth > 1) {
         glNamedFramebufferTextureLayer(dst_fbo, GL_COLOR_ATTACHMENT0, desc->dst->id, 0,
                                        desc->dst_box.z);
      } else {
         glNamedFramebufferTexture(dst_fbo, GL_COLOR_ATTACHMENT0, desc->dst->id, 0);
      }

      glCreateFramebuffers(1, &src_fbo);
      if (desc->src->depth > 1) {
         glNamedFramebufferTextureLayer(src_fbo, GL_COLOR_ATTACHMENT0, desc->src->id, 0,
                                        desc->src_box.z);
      } else {
         glNamedFramebufferTexture(src_fbo, GL_COLOR_ATTACHMENT0, desc->src->id, 0);
      }

      glBlitNamedFramebuffer(src_fbo, dst_fbo,
                             desc->src_box.x, desc->src_box.y,
                             desc->src_box.x + desc->src_box.width, desc->src_box.y + desc->src_box.height,
                             desc->dst_box.x, desc->dst_box.y,
                             desc->dst_box.x + desc->dst_box.width, desc->dst_box.y + desc->dst_box.height,
                             GL_COLOR_BUFFER_BIT, desc->linear_filter ? GL_LINEAR : GL_NEAREST);

      glDeleteFramebuffers(1, &dst_fbo);
      glDeleteFramebuffers(2, &src_fbo);
   }

   gl_check_no_error();
}

static void
gl_upload_image_data(api_context *ctx, api_image *image, unsigned stride_in_bytes,
                     void *data)
{
   assert(image->samples == 1);

   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glPixelStorei(GL_UNPACK_ROW_LENGTH, stride_in_bytes / get_pixel_size_from_format(image->format));

   switch (image->type) {
   case VK_IMAGE_TYPE_1D:
      glTextureSubImage1D(image->id, 0, 0, image->width, image->glformat, image->gltype, data);
      break;
   case VK_IMAGE_TYPE_2D:
      if (image->depth > 1) {
         glTextureSubImage3D(image->id, 0, 0, 0, 0, image->width, image->height, image->depth,
                             image->glformat, image->gltype, data);
      } else {
         glTextureSubImage2D(image->id, 0, 0, 0, image->width, image->height,
                             image->glformat, image->gltype, data);
      }
      break;
   case VK_IMAGE_TYPE_3D:
      glTextureSubImage3D(image->id, 0, 0, 0, 0, image->width, image->height, image->depth,
                          image->glformat, image->gltype, data);
      break;
   default:
      error("gl_upload_image_data: invalid image type");
   }

   /* Restore defaults. */
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

   gl_check_no_error();
}

static void
gl_image_write_png(api_context *ctx, api_image *image, unsigned layer, const char *filename)
{
   uint64_t size = (uint64_t)image->width * image->height * 4;
   GLenum format = format_is_integer(image->format) ? GL_RGBA_INTEGER : GL_RGBA;
   void *data = malloc(size);

   if (image->depth > 1) {
      glGetTextureSubImage(image->id, 0, 0, 0, layer, image->width, image->height, 1, format,
                           GL_UNSIGNED_BYTE, size, data);
   } else {
      assert(layer == 0);
      glGetTextureImage(image->id, 0, format, GL_UNSIGNED_BYTE, size, data);
   }

   gl_check_no_error();
   write_png_rgba8(filename, image, data);
   free(data);
}

static api_framebuffer *
gl_create_framebuffer(api_context *ctx, api_image *colorbuf, api_image *zbuf,
                      unsigned width, unsigned height, unsigned samples, unsigned view_mask)
{
   api_framebuffer *fb = calloc(1, sizeof(api_framebuffer));
   fb->width = width;
   fb->height = height;
   fb->samples = samples;
   fb->view_mask = view_mask;
   fb->colorbuf = colorbuf;
   fb->zbuf = zbuf;

   assert(view_mask == 0x1);

   glCreateFramebuffers(1, &fb->id);
   if (colorbuf)
      glNamedFramebufferTexture(fb->id, GL_COLOR_ATTACHMENT0, colorbuf->id, 0);

   if (zbuf) {
      assert(zbuf->type != VK_IMAGE_TYPE_3D);
      glNamedFramebufferTexture(fb->id, GL_DEPTH_ATTACHMENT, zbuf->id, 0);
   }

   if (!colorbuf && !zbuf) {
      glNamedFramebufferParameteri(fb->id, GL_FRAMEBUFFER_DEFAULT_WIDTH, width);
      glNamedFramebufferParameteri(fb->id, GL_FRAMEBUFFER_DEFAULT_HEIGHT, height);
      glNamedFramebufferParameteri(fb->id, GL_FRAMEBUFFER_DEFAULT_LAYERS, 1);
      glNamedFramebufferParameteri(fb->id, GL_FRAMEBUFFER_DEFAULT_SAMPLES, samples);
      glNamedFramebufferParameteri(fb->id, GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS, true);
   }
   gl_check_no_error();

   if (glCheckNamedFramebufferStatus(fb->id, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      error("FBO is incomplete");

   return fb;
}

static void
gl_destroy_framebuffer(api_context *ctx, api_framebuffer *fb)
{
   assert(fb != ctx->fb);

   if (fb == ctx->prev_fb) {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      ctx->prev_fb = NULL;
   }

   glDeleteFramebuffers(1, &fb->id);
   free(fb);
}

static api_shader *
gl_create_shader(api_context *ctx, const char *source, api_shader_type type)
{
   api_shader *shader = calloc(1, sizeof(api_shader));
   GLenum shader_type;

   switch (type) {
   case api_shader_vs:
      shader_type = GL_VERTEX_SHADER;
      break;
   case api_shader_tcs:
      shader_type = GL_TESS_CONTROL_SHADER;
      break;
   case api_shader_tes:
      shader_type = GL_TESS_EVALUATION_SHADER;
      break;
   case api_shader_gs:
      shader_type = GL_GEOMETRY_SHADER;
      break;
   case api_shader_fs:
      shader_type = GL_FRAGMENT_SHADER;
      break;
   case api_shader_cs:
      shader_type = GL_COMPUTE_SHADER;
      break;
   case api_shader_ts:
      shader_type = GL_TASK_SHADER_EXT;
      break;
   case api_shader_ms:
      shader_type = GL_MESH_SHADER_EXT;
      break;
   default:
      error("invalid shader type");
   }

   shader->id = glCreateShader(shader_type);
   glShaderSource(shader->id, 1, &source, (GLint[]){strlen(source)});
   glCompileShader(shader->id);

   GLint status;
   glGetShaderiv(shader->id, GL_COMPILE_STATUS, &status);

   if (!status) {
      char log[4 * 1024];

      glGetShaderInfoLog(shader->id, ARRAY_SIZE(log), NULL, log);
      error("Failed to compile GLSL shader:\n%s\n\n%s", source, log);
   }

   gl_check_no_error();
   return shader;
}

static void
gl_destroy_shader(api_context *ctx, api_shader *shader)
{
   glDeleteShader(shader->id);
   free(shader);
}

static api_descriptor_set_layout *
gl_create_descriptor_set_layout(api_context *ctx,
                                const api_descriptor_set_layout_desc *desc)
{
   api_descriptor_set_layout *layout = calloc(1, sizeof(api_descriptor_set_layout));
   layout->desc = *desc;

   for (unsigned i = 0; i < MAX_UNIFORM_BUFFER_BINDINGS; i++)
      assert(desc->uniform_buffer[i].array_size <= MAX_UNIFORM_BUFFER_ARRAY_SIZE);
   for (unsigned i = 0; i < MAX_STORAGE_BUFFER_BINDINGS; i++)
      assert(desc->storage_buffer[i].array_size <= MAX_STORAGE_BUFFER_ARRAY_SIZE);
   for (unsigned i = 0; i < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS; i++)
      assert(desc->uniform_texel_buffer[i].array_size <= MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE);
   for (unsigned i = 0; i < MAX_COMBINED_IMAGE_SAMPLER_BINDINGS; i++)
      assert(desc->combined_image_sampler[i].array_size <= MAX_COMBINED_IMAGE_SAMPLER_ARRAY_SIZE);
   for (unsigned i = 0; i < MAX_STORAGE_IMAGE_BINDINGS; i++)
      assert(desc->storage_image[i].array_size <= MAX_STORAGE_IMAGE_ARRAY_SIZE);

   return layout;
}

static void
gl_destroy_descriptor_set_layout(api_context *ctx, api_descriptor_set_layout *layout)
{
   free(layout);
}

static api_descriptor_set *
gl_create_descriptor_set(api_context *ctx, api_descriptor_set_layout *layout)
{
   api_descriptor_set *set = calloc(1, sizeof(api_descriptor_set));
   set->layout = layout;

   return set;
}

static void
gl_destroy_descriptor_set(api_context *ctx, api_descriptor_set *set)
{
   free(set);
}

static void
gl_set_uniform_buffer_descriptors(api_context *ctx, api_descriptor_set *set, unsigned binding_index,
                                  api_buffer *buffer, uint64_t offset, uint64_t size)
{
   assert(1 <= set->layout->desc.uniform_buffer[binding_index].array_size);
   set->ubo_id = buffer->id;
   set->ubo_offset = offset;
   set->ubo_size = size;
}

static void
gl_set_storage_buffer_descriptors(api_context *ctx, api_descriptor_set *set, unsigned binding_index,
                                  api_buffer *buffer, uint64_t offset, uint64_t size)
{
   assert(1 <= set->layout->desc.storage_buffer[binding_index].array_size);
   set->ssbo_id = buffer->id;
   set->ssbo_offset = offset;
   set->ssbo_size = size;
}

static void
gl_set_uniform_texel_buffer_descriptors(api_context *ctx, api_descriptor_set *set,
                                        unsigned binding_index, unsigned num_buffers,
                                        api_buffer **buffers, VkFormat *formats,
                                        uint64_t *offsets, uint64_t *sizes)
{
   assert(binding_index < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS);
   assert(num_buffers <= set->layout->desc.uniform_texel_buffer[binding_index].array_size);

   glDeleteTextures(num_buffers, set->tbo_ids[binding_index]);
   glCreateTextures(GL_TEXTURE_BUFFER, num_buffers, set->tbo_ids[binding_index]);

   for (unsigned i = 0; i < num_buffers; i++) {
      glTextureBufferRange(set->tbo_ids[binding_index][i], get_gl_internalformat(formats[i]), buffers[i]->id,
                           offsets[i], sizes[i]);
   }
}

static void
gl_set_combined_image_sampler_descriptors(api_context *ctx, api_descriptor_set *set,
                                          unsigned binding_index, unsigned num_samplers,
                                          api_image **images)
{
   assert(num_samplers <= set->layout->desc.combined_image_sampler[binding_index].array_size);

   glDeleteTextures(num_samplers, set->tex_ids);

   for (unsigned i = 0; i < num_samplers; i++) {
      glGenTextures(1, &set->tex_ids[i]);
      glTextureView(set->tex_ids[i], images[i]->gltarget, images[i]->id, images[i]->glinternalformat,
                    0, 1, 0, images[i]->depth);
      glTextureParameteri(set->tex_ids[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTextureParameteri(set->tex_ids[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   }

   gl_check_no_error();
}

static void
gl_set_storage_image_descriptors(api_context *ctx, api_descriptor_set *set, unsigned binding_index,
                                 unsigned num_images, api_image **images)
{
   assert(num_images <= set->layout->desc.storage_image[binding_index].array_size);

   for (unsigned i = 0; i < num_images; i++)
      set->image_ids[i] = images[i]->id;
}

static void
gl_bind_descriptor_set(api_context *ctx, api_descriptor_set *set)
{
   for (unsigned i = 0; i < MAX_UNIFORM_BUFFER_BINDINGS; i++) {
      if (set->layout->desc.uniform_buffer[i].array_size) {
         glBindBufferRange(GL_UNIFORM_BUFFER, set->layout->desc.uniform_buffer[i].gl_binding,
                           set->ubo_id, set->ubo_offset, set->ubo_size);
      }
   }

   for (unsigned i = 0; i < MAX_STORAGE_BUFFER_BINDINGS; i++) {
      if (set->layout->desc.storage_buffer[i].array_size) {
         glBindBufferRange(GL_SHADER_STORAGE_BUFFER, set->layout->desc.storage_buffer[i].gl_binding,
                           set->ssbo_id, set->ssbo_offset, set->ssbo_size);
      }
   }

   for (unsigned i = 0; i < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS; i++) {
      if (set->layout->desc.uniform_texel_buffer[i].array_size) {
         glBindTextures(set->layout->desc.uniform_texel_buffer[i].gl_binding,
                        set->layout->desc.uniform_texel_buffer[i].array_size, set->tbo_ids[i]);
      }
   }

   for (unsigned i = 0; i < MAX_COMBINED_IMAGE_SAMPLER_BINDINGS; i++) {
      if (set->layout->desc.combined_image_sampler[i].array_size) {
         glBindTextures(set->layout->desc.combined_image_sampler[i].gl_binding,
                        set->layout->desc.combined_image_sampler[i].array_size, set->tex_ids);
      }
   }

   for (unsigned i = 0; i < MAX_STORAGE_IMAGE_BINDINGS; i++) {
      if (set->layout->desc.storage_image[i].array_size) {
         glBindImageTextures(set->layout->desc.storage_image[i].gl_binding,
                             set->layout->desc.storage_image[i].array_size, set->image_ids);
      }
   }
}

static api_pipeline *
gl_create_pipeline(api_context *ctx, const api_pipeline_desc *desc)
{
   api_pipeline *pipeline = calloc(1, sizeof(api_pipeline));
   pipeline->desc = *desc;

   pipeline->prog = glCreateProgram();
   if (desc->ms)
      glAttachShader(pipeline->prog, desc->ms->id);
   if (desc->vs)
      glAttachShader(pipeline->prog, desc->vs->id);
   if (desc->fs)
      glAttachShader(pipeline->prog, desc->fs->id);
   glLinkProgram(pipeline->prog);

   GLint status;
   glGetProgramiv(pipeline->prog, GL_LINK_STATUS, &status);

   if (!status) {
      char log[4 * 1024];

      glGetProgramInfoLog(pipeline->prog, ARRAY_SIZE(log), NULL, log);
      error("Failed to link GLSL program:\n%s", log);
   }

   switch (desc->topology) {
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      pipeline->prim_mode = GL_TRIANGLES;
      break;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      pipeline->prim_mode = GL_TRIANGLE_STRIP;
      break;
   default:
      error("unexpected primitive topology %u", desc->topology);
   }

   assert(ctx->has_vrs || (desc->vrs_fragment_size[0] == 1 && desc->vrs_fragment_size[1] == 1));

   gl_check_no_error();
   return pipeline;
}

static void
gl_destroy_pipeline(api_context *ctx, api_pipeline *pipeline)
{
   assert(ctx->current_pipeline != pipeline);
   glDeleteProgram(pipeline->prog);
   free(pipeline);
}

static GLenum
get_compare_func(VkCompareOp op)
{
   switch (op) {
   case VK_COMPARE_OP_NEVER:
      return GL_NEVER;
   case VK_COMPARE_OP_LESS:
      return GL_LESS;
   case VK_COMPARE_OP_EQUAL:
      return GL_EQUAL;
   case VK_COMPARE_OP_NOT_EQUAL:
      return GL_NOTEQUAL;
   case VK_COMPARE_OP_ALWAYS:
      return GL_ALWAYS;
   default:
      error("unexpected compare op in get_compare_func");
      return 0;
   }
}

static void
gl_set_current_pipeline_color_depth_masks(api_context *ctx)
{
   if (ctx->current_pipeline) {
      glColorMask(!!(ctx->current_pipeline->desc.colormask & 0x1),
                  !!(ctx->current_pipeline->desc.colormask & 0x2),
                  !!(ctx->current_pipeline->desc.colormask & 0x4),
                  !!(ctx->current_pipeline->desc.colormask & 0x8));

      if (ctx->current_pipeline->desc.depth_enabled) {
         glDepthMask(ctx->current_pipeline->desc.depth_write_enabled);
      } else {
         glDepthMask(GL_TRUE);
      }
   } else {
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glDepthMask(GL_TRUE);
   }
}

static void
gl_bind_unbind_pipeline(api_context *ctx, api_pipeline *pipeline)
{
   if (ctx->current_pipeline == pipeline)
      return;

   /* Unbind the previous pipeline. */
   if (ctx->current_pipeline) {
      for (unsigned i = 0; i < ctx->current_pipeline->desc.num_vb_desc; i++)
         glDisableVertexAttribArray(i);
   }

   /* Bind the pipeline. */
   ctx->current_pipeline = pipeline;

   if (!pipeline) {
      gl_set_current_pipeline_color_depth_masks(ctx);
      return;
   }

   if (pipeline->desc.primitive_restart)
      glEnable(GL_PRIMITIVE_RESTART);
   else
      glDisable(GL_PRIMITIVE_RESTART);

   for (unsigned i = 0; i < pipeline->desc.num_vb_desc; i++) {
      glEnableVertexAttribArray(i);
      glVertexAttribBinding(i, i);

      switch (pipeline->desc.vb_formats[i]) {
      case VK_FORMAT_R32G32B32_SFLOAT:
         glVertexAttribFormat(i, 3, GL_FLOAT, false, 0);
         break;
      case VK_FORMAT_R32G32B32A32_SFLOAT:
         glVertexAttribFormat(i, 4, GL_FLOAT, false, 0);
         break;
      default:
         error("unexpected vertex format %u", pipeline->desc.vb_formats[i]);
      }
   }

   if (pipeline->desc.polygon_mode == VK_POLYGON_MODE_FILL)
      glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
   else if (pipeline->desc.polygon_mode == VK_POLYGON_MODE_LINE)
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
   else if (pipeline->desc.polygon_mode == VK_POLYGON_MODE_POINT)
      glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
   else
      error("gl: invalid polygon mode");

   if (pipeline->desc.cull_mode) {
      glEnable(GL_CULL_FACE);
      if (pipeline->desc.cull_mode == VK_CULL_MODE_FRONT_AND_BACK)
         glCullFace(GL_FRONT_AND_BACK);
      else if (pipeline->desc.cull_mode == VK_CULL_MODE_FRONT_BIT)
         glCullFace(GL_FRONT);
      else if (pipeline->desc.cull_mode == VK_CULL_MODE_BACK_BIT)
         glCullFace(GL_BACK);
   } else {
      glDisable(GL_CULL_FACE);
   }

   for (unsigned i = 0; i < 8; i++) {
      if (pipeline->desc.clipdist_enable_mask & (1 << i))
         glEnable(GL_CLIP_DISTANCE0 + i);
      else
         glDisable(GL_CLIP_DISTANCE0 + i);
   }

   if (pipeline->desc.rasterizer_discard)
      glEnable(GL_RASTERIZER_DISCARD);
   else
      glDisable(GL_RASTERIZER_DISCARD);

   /* Sample shading is implied by the shader. */
   glUseProgram(pipeline->prog);

   glSampleMaski(0, pipeline->desc.samplemask);

   if (pipeline->desc.depth_enabled) {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(get_compare_func(pipeline->desc.depth_compare_op));
   } else {
      glDisable(GL_DEPTH_TEST);
   }

   if (pipeline->desc.alpha_to_coverage)
      glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
   else
      glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);

   if (pipeline->desc.blend_src_color || pipeline->desc.blend_src_alpha) {
      glEnable(GL_BLEND);
      if (pipeline->desc.blend_src_alpha)
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      else
         glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
   } else {
      glDisable(GL_BLEND);
   }

   if (pipeline->desc.fb->samples > 1) {
      glEnable(GL_MULTISAMPLE);
      glEnable(GL_SAMPLE_MASK);
   } else {
      glDisable(GL_MULTISAMPLE);
      glDisable(GL_SAMPLE_MASK);
   }

   gl_set_current_pipeline_color_depth_masks(ctx);
}


static void
gl_bind_pipeline(api_context *ctx, api_pipeline *pipeline)
{
   assert(pipeline);
   gl_bind_unbind_pipeline(ctx, pipeline);
}

static void
gl_begin_cmdbuf(api_context *ctx, api_queue_type queue)
{
   assert(queue == api_queue_gfx);
}

static void
gl_end_cmdbuf_and_submit(api_context *ctx, unsigned wait_queue_mask, api_fence *wait_fence,
                         api_fence **signal_fence)
{
   assert(!wait_fence);
   assert(!signal_fence);

   gl_bind_unbind_pipeline(ctx, NULL);
   glFlush();
   gl_check_no_error();
}

static void
gl_wait_idle_before_deallocation(api_context *ctx)
{
   /* Waiting for idle before memory deallocation isn't needed with GL. */
}

static void
gl_begin_render_pass(api_context *ctx, const api_render_pass_desc *desc)
{
   ctx->fb = desc->fb;

   if (desc->fb != ctx->prev_fb) {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, desc->fb->id);
      glViewport(0, 0, desc->fb->width, desc->fb->height);
      ctx->prev_fb = NULL;
   }

   if (desc->clear) {
      if (desc->fb->colorbuf) {
         /* Clears are affected by glColorMask and glDepthMask, while we want them to be unaffected. */
         glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

         if (format_is_sint((desc->fb->colorbuf->format)))
            glClearBufferiv(GL_COLOR, 0, desc->color_clear_value.int32);
         else if (format_is_integer(desc->fb->colorbuf->format))
            glClearBufferuiv(GL_COLOR, 0, desc->color_clear_value.uint32);
         else
            glClearBufferfv(GL_COLOR, 0, desc->color_clear_value.float32);
      }

      if (desc->fb->zbuf) {
         glDepthMask(GL_TRUE);
         glClearBufferfv(GL_DEPTH, 0, &desc->depth_clear_value);
      }

      gl_set_current_pipeline_color_depth_masks(ctx);
   }
}

static void
gl_end_render_pass(api_context *ctx)
{
   ctx->prev_fb = ctx->fb;
   ctx->fb = NULL;
}

static void
gl_bind_vertex_buffers(api_context *ctx, api_buffer *vb, const uint64_t *vb_offsets)
{
   for (unsigned i = 0; i < ctx->current_pipeline->desc.num_vb_desc; i++)
      glBindVertexBuffer(i, vb->id, vb_offsets[i], ctx->current_pipeline->desc.vb_strides[i]);
}

static void
gl_bind_index_buffer(api_context *ctx, api_buffer *ib)
{
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->id);
}

static void
gl_draw(api_context *ctx, const api_draw_desc *desc)
{
   assert(desc->count && (desc->mesh_shader || desc->instance_count));

   if (desc->mesh_shader) {
      glDrawMeshTasksEXT(desc->count, 1, 1);
   } else if (desc->indexed) {
      glDrawElementsInstanced(ctx->current_pipeline->prim_mode, desc->count, GL_UNSIGNED_INT, NULL,
                              desc->instance_count);
   } else {
      glDrawArraysInstanced(ctx->current_pipeline->prim_mode, desc->first_vertex, desc->count,
                            desc->instance_count);
   }
}

static void
gl_driver_workaround(api_context *ctx, driver_wa wa)
{
   switch (wa) {
   case WA_RDNA4_TIMESTAMP_BUG:
      if (ctx->options.rdna4_timestamp_wa)
         glMemoryBarrier(GL_ELEMENT_ARRAY_BARRIER_BIT); /* PS/CS_PARTIAL_FLUSH in mesa/radeonsi */
      break;
   default:
      error("invalid workaround enum");
   }
}

static api_query_pool *
gl_create_query_pool(api_context *ctx, unsigned num_queries, api_query_type type)
{
   api_query_pool *pool = calloc(1, sizeof(api_query_pool));

   pool->type = type;
   pool->num_written_queries = 0;
   pool->num_queries = num_queries;
   pool->results = calloc(num_queries, sizeof(uint64_t));

   switch (type) {
   case api_query_timestamp:
      pool->gltarget = GL_TIMESTAMP;
      break;
   case api_query_fs_invocations:
      pool->gltarget = GL_FRAGMENT_SHADER_INVOCATIONS;
      break;
   default:
      error("invalid query type");
   }

   pool->queries = calloc(num_queries, sizeof(GLuint));
   glCreateQueries(pool->gltarget, num_queries, pool->queries);
   gl_check_no_error();

   return pool;
}

static void
gl_begin_next_query(api_context *ctx, api_query_pool *pool)
{
   assert(pool->num_written_queries < pool->num_queries);

   switch (pool->type) {
   case api_query_fs_invocations:
      glBeginQuery(pool->gltarget, pool->queries[pool->num_written_queries]);
      break;
   default:
      error("invalid begin/end query type");
   }
}

static void
gl_end_next_query(api_context *ctx, api_query_pool *pool)
{
   assert(pool->num_written_queries < pool->num_queries);

   switch (pool->type) {
   case api_query_fs_invocations:
      glEndQuery(pool->gltarget);
      break;
   default:
      error("invalid begin/end query type");
   }

   pool->num_written_queries++;
}

static void
gl_write_next_query_value(api_context *ctx, api_query_pool *pool)
{
   assert(pool->num_written_queries < pool->num_queries);
   glQueryCounter(pool->queries[pool->num_written_queries], pool->gltarget);
   pool->num_written_queries++;
}

static void
gl_get_query_results(api_context *ctx, api_query_pool *pool)
{
   for (unsigned i = 0; i < pool->num_written_queries; i++)
      glGetQueryObjectui64v(pool->queries[i], GL_QUERY_RESULT, &pool->results[i]);

   gl_check_no_error();
   pool->num_read_queries = 0;
}

api_context *
gl_create_context(const program_options *options)
{
   api_context *ctx = calloc(1, sizeof(api_context));
   ctx->options = *options;

   /* Create an EGL context. */
   EGLint count, major, minor;
   EGLDisplay egl_dpy;
   EGLConfig cfg;
   EGLContext egl_ctx;

   egl_check(egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY));
   egl_check(eglInitialize(egl_dpy, &major, &minor));
   egl_check(eglChooseConfig(egl_dpy, (EGLint[]){EGL_NONE}, &cfg, 1, &count));

   if (!count)
      error("No EGL configs");

   const char *const egl_extension_list = eglQueryString(egl_dpy, EGL_EXTENSIONS);

   if (!strstr(egl_extension_list, "EGL_KHR_surfaceless_context"))
      error("EGL_KHR_surfaceless_context unsupported");

   /* Create an OpenGL context. */
   egl_check(eglBindAPI(EGL_OPENGL_API));
   egl_check(egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, NULL));
   egl_check(eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx));
   egl_check(gladLoadGLLoader((GLADloadproc)eglGetProcAddress));
   gl_check_no_error();

   if (GLVersion.major < 4 || (GLVersion.major == 4 && GLVersion.minor < 6))
      error("OpenGL 4.6 is required.");

   printf("Renderer: %s\n", glGetString(GL_RENDERER));

   /* Adjust the initial state. */
   GLuint vao;
   glCreateVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
   glPrimitiveRestartIndex(UINT32_MAX);

   /* Set properties. */
   ctx->has_heap[api_heap_device] = true;
   ctx->has_heap[api_heap_host_uncached] = true;
   ctx->has_heap[api_heap_host_cached] = true;
   ctx->has_queue[api_queue_gfx] = true;
   ctx->timestamp_period_in_seconds = 0.000000001;

   ctx->has_blit_image_3d = false;
   ctx->has_blit_image_msaa = true;
   ctx->has_buffer_device_address = false;
   ctx->has_clear_image_region = true;
   ctx->has_image_tiling_linear = GLAD_GL_MESA_texture_tiling_linear;
   ctx->has_resolve_image_yflip = true;
   ctx->has_shader_int8 = false;
   ctx->has_shader_int64 = GLAD_GL_ARB_gpu_shader_int64;
   ctx->has_shader_subgroup_clock = GLAD_GL_ARB_shader_clock;
   ctx->has_sparse_buffer = GLAD_GL_ARB_sparse_buffer;
   ctx->has_vrs = false;
   ctx->has_vs_tes_layer_output = GLAD_GL_ARB_shader_viewport_layer_array;
   ctx->has_xfb = true;

   if (GLAD_GL_EXT_mesh_shader)
      glGetIntegerv(GL_MAX_MESH_WORK_GROUP_INVOCATIONS_EXT, (int*)&ctx->max_mesh_workgroup_size);

   glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, (GLint*)&ctx->max_storage_buffer_range);
   glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, (GLint*)&ctx->max_uniform_buffer_range);

   if (GLAD_GL_ARB_sparse_buffer)
      glGetIntegerv(GL_SPARSE_BUFFER_PAGE_SIZE_ARB, (int*)&ctx->sparse_buffer_alignment);

   /* Get color sample counts. */
   int num_sample_counts;
   int sample_counts[16];

   glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &num_sample_counts);
   if (num_sample_counts > 16)
      num_sample_counts = 16;

   glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_SAMPLES, num_sample_counts, sample_counts);

   ctx->supported_color_sample_counts |= VK_SAMPLE_COUNT_1_BIT;

   for (int i = 0; i < num_sample_counts; i++) {
      if (IS_POT(sample_counts[i]))
         ctx->supported_color_sample_counts |= sample_counts[i];
   }

   /* Set callbacks. */
   ctx->destroy_context = NULL;

   ctx->create_buffer = gl_create_buffer;
   ctx->destroy_buffer = gl_destroy_buffer;
   ctx->upload_buffer_data = gl_upload_buffer_data;
   ctx->get_buffer_data = gl_get_buffer_data;
   ctx->clear_buffer = gl_clear_buffer;
   ctx->copy_buffer = gl_copy_buffer;

   ctx->create_image = gl_create_image;
   ctx->destroy_image = gl_destroy_image;
   ctx->clear_image = gl_clear_image;
   ctx->blit_image = gl_blit_image;
   ctx->upload_image_data = gl_upload_image_data;
   ctx->image_write_png = gl_image_write_png;

   ctx->create_framebuffer = gl_create_framebuffer;
   ctx->destroy_framebuffer = gl_destroy_framebuffer;

   ctx->create_shader = gl_create_shader;
   ctx->destroy_shader = gl_destroy_shader;

   ctx->create_descriptor_set_layout = gl_create_descriptor_set_layout;
   ctx->destroy_descriptor_set_layout = gl_destroy_descriptor_set_layout;

   ctx->create_descriptor_set = gl_create_descriptor_set;
   ctx->destroy_descriptor_set = gl_destroy_descriptor_set;
   ctx->set_uniform_buffer_descriptor = gl_set_uniform_buffer_descriptors;
   ctx->set_storage_buffer_descriptor = gl_set_storage_buffer_descriptors;
   ctx->set_uniform_texel_buffer_descriptors = gl_set_uniform_texel_buffer_descriptors;
   ctx->set_combined_image_sampler_descriptors = gl_set_combined_image_sampler_descriptors;
   ctx->set_storage_image_descriptors = gl_set_storage_image_descriptors;
   ctx->bind_descriptor_set = gl_bind_descriptor_set;

   ctx->create_pipeline = gl_create_pipeline;
   ctx->destroy_pipeline = gl_destroy_pipeline;
   ctx->bind_pipeline = gl_bind_pipeline;

   ctx->begin_cmdbuf = gl_begin_cmdbuf;
   ctx->end_cmdbuf_and_submit = gl_end_cmdbuf_and_submit;
   ctx->wait_idle_before_deallocation = gl_wait_idle_before_deallocation;

   ctx->begin_render_pass = gl_begin_render_pass;
   ctx->end_render_pass = gl_end_render_pass;

   ctx->bind_vertex_buffers = gl_bind_vertex_buffers;
   ctx->bind_index_buffer = gl_bind_index_buffer;
   ctx->draw = gl_draw;
   ctx->driver_workaround = gl_driver_workaround;

   ctx->create_query_pool = gl_create_query_pool;
   ctx->begin_next_query = gl_begin_next_query;
   ctx->end_next_query = gl_end_next_query;
   ctx->write_next_query_value = gl_write_next_query_value;
   ctx->get_query_results = gl_get_query_results;

   ctx->current_pipeline = NULL;

   return ctx;
}
