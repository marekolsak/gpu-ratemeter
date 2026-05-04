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
#include <alloca.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <shaderc/shaderc.h>

#define VK_PRIVATE
#include "common.h"

#ifdef HAVE_VK_UTILITY_HEADERS
#include <vulkan/vk_enum_string_helper.h>
#else
#define string_VkResult(r) "{can't get a string - missing Vulkan::UtilityHeaders}"
#endif

#define vk_check(call) \
   do { \
      VkResult res = call; \
      if (res != VK_SUCCESS) \
         error("Vulkan call failed on line %u: %s = %i (%s)\n", __LINE__, #call, res, string_VkResult(res)); \
   } while(0)

static bool
vk_has_validation_layer(void)
{
    uint32_t count;
    vk_check(vkEnumerateInstanceLayerProperties(&count, NULL));
    VkLayerProperties *props = alloca(sizeof(*props) * count);
    vk_check(vkEnumerateInstanceLayerProperties(&count, props));

    for (unsigned i = 0; i < count; i++) {
        if (!strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation"))
            return true;
    }

    fprintf(stderr, "Warning: VK_LAYER_KHRONOS_validation not found.\n");
    return false;
}

static int
vk_find_heap_with_flags(api_context *ctx, unsigned supported_heap_mask,
                        VkMemoryPropertyFlags require_flags, VkMemoryPropertyFlags disallow_flags)
{
   for (unsigned i = 0; (1u << i) <= supported_heap_mask && i <= ctx->memory_properties.memoryTypeCount; i++) {
      if (!(supported_heap_mask & (1u << i)))
          continue;

      VkMemoryPropertyFlags flags = ctx->memory_properties.memoryTypes[i].propertyFlags;

      if (((flags & require_flags) == require_flags) && !(flags & disallow_flags))
         return i;
   }

   return -1;
}

static int
vk_find_heap(api_context *ctx, unsigned supported_heap_mask, api_heap_type heap)
{
   VkMemoryPropertyFlags require_flags = 0, disallow_flags = 0;
   assert(supported_heap_mask);

   if (heap == api_heap_host_uncached && !ctx->has_host_uncached_heap)
      heap = api_heap_host_cached;

   if (heap == api_heap_host_cached && !ctx->has_host_cached_heap)
      heap = api_heap_device;

   switch (heap) {
   case api_heap_device:
      require_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      break;
   case api_heap_host_uncached:
      disallow_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      break;
   case api_heap_host_cached:
      require_flags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      disallow_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      break;
   default:
      error("invalid heap type");
   }

   int index = vk_find_heap_with_flags(ctx, supported_heap_mask, require_flags, disallow_flags);

   if (supported_heap_mask != ~0) {
      if (index == -1 && disallow_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
         /* Allow DEVICE_LOCAL and try again. */
         disallow_flags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
         index = vk_find_heap_with_flags(ctx, supported_heap_mask, require_flags, disallow_flags);
      }

      if (index == -1 && disallow_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
         /* Allow HOST_CACHED and try again. */
         disallow_flags &= ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
         index = vk_find_heap_with_flags(ctx, supported_heap_mask, require_flags, disallow_flags);
      }

      if (index == -1 && require_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
         /* Don't require HOST_CACHED and try again. */
         require_flags &= ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
         index = vk_find_heap_with_flags(ctx, supported_heap_mask, require_flags, disallow_flags);
      }
   }

   return index;
}

static api_buffer *
vk_create_buffer(api_context *ctx, uint64_t size, api_heap_type heap)
{
   api_buffer *buf = calloc(1, sizeof(api_buffer));
   buf->size = size;
   buf->heap = heap;

   vk_check(vkCreateBuffer(ctx->device,
                           &(VkBufferCreateInfo) {
                              .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                           },
                           NULL, &buf->buffer));

   VkMemoryRequirements reqs;
   vkGetBufferMemoryRequirements(ctx->device, buf->buffer, &reqs);

   int mem_type_index = vk_find_heap(ctx, reqs.memoryTypeBits, heap);
   if (mem_type_index == -1) {
      error("create_buffer: can't find memory type for reqs.memoryTypeBits=0x%x, heap=%u",
            reqs.memoryTypeBits, heap);
   }

   vk_check(vkAllocateMemory(ctx->device,
                             &(VkMemoryAllocateInfo) {
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = reqs.size,
                                .memoryTypeIndex = mem_type_index,
                             },
                             NULL, &buf->mem));
   vk_check(vkBindBufferMemory(ctx->device, buf->buffer, buf->mem, 0));

   if (heap == api_heap_device)
      ctx->device_mem_usage += reqs.size;
   return buf;
}

static void
vk_clear_buffer(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size, uint32_t value)
{
   vkCmdFillBuffer(ctx->current_cmd_buffer, buf->buffer, offset, size, value);

   vkCmdPipelineBarrier2(ctx->current_cmd_buffer, &(VkDependencyInfo) {
                            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .bufferMemoryBarrierCount = 1,
                            .pBufferMemoryBarriers = &(VkBufferMemoryBarrier2) {
                               .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT |
                                                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .buffer = buf->buffer,
                               .offset = offset,
                               .size = size,
                            },
                         });
}

static void
vk_copy_buffer(api_context *ctx, api_buffer *dst, api_buffer *src, uint64_t dst_offset,
               uint64_t src_offset, uint64_t size)
{
   vkCmdCopyBuffer2(ctx->current_cmd_buffer,
                    &(VkCopyBufferInfo2){
                       .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                       .srcBuffer = src->buffer,
                       .dstBuffer = dst->buffer,
                       .regionCount = 1,
                       .pRegions = &(VkBufferCopy2){
                          .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                          .srcOffset = src_offset,
                          .dstOffset = dst_offset,
                          .size = size,
                       },
                    });

   vkCmdPipelineBarrier2(ctx->current_cmd_buffer, &(VkDependencyInfo) {
                            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .bufferMemoryBarrierCount = 2,
                            .pBufferMemoryBarriers = (VkBufferMemoryBarrier2[2]) {
                               {
                                  .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT |
                                                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .buffer = src->buffer,
                                  .offset = src_offset,
                                  .size = size,
                               },
                               {
                                  .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT |
                                                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .buffer = dst->buffer,
                                  .offset = dst_offset,
                                  .size = size,
                               },
                            },
                         });
}

static void
vk_image_layout_transition(api_context *ctx, api_image *image, VkImageLayout new_layout)
{
   if (image->layout == new_layout)
      return;

   vkCmdPipelineBarrier2(ctx->current_cmd_buffer,
                         &(VkDependencyInfo) {
                            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = (VkImageMemoryBarrier2[2]) {
                               {
                                  .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                  .oldLayout = image->layout,
                                  .newLayout = new_layout,
                                  .image = image->image,
                                  .subresourceRange = {
                                     .aspectMask = format_is_depth_or_stencil(image->format) ?
                                                      VK_IMAGE_ASPECT_DEPTH_BIT :
                                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                     .levelCount = 1,
                                     .layerCount = image->type == VK_IMAGE_TYPE_3D ?
                                                      VK_REMAINING_ARRAY_LAYERS : image->layer_count,
                                  },
                               },
                            },
                         });
   image->layout = new_layout;
}

static api_image *
vk_create_image(api_context *ctx, VkImageType type, VkFormat format, unsigned width, unsigned height,
                unsigned depth, unsigned samples, VkImageTiling tiling, api_heap_type heap)
{
   api_image *image = calloc(1, sizeof(api_image));
   image->type = type;
   image->width = width;
   image->height = height;
   image->depth = depth;
   image->samples = samples;
   image->format = format;

   assert(type != VK_IMAGE_TYPE_1D || depth == 1);

   image->layer_count = type == VK_IMAGE_TYPE_2D ? depth : 1;
   bool is_zs = format_is_depth_or_stencil(format);

   vk_check(vkCreateImage(ctx->device,
                          &(VkImageCreateInfo) {
                             .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .flags = type == VK_IMAGE_TYPE_3D ?
                                          VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0,
                             .imageType = type,
                             .format = format,
                             .extent = {
                                .width = width,
                                .height = height,
                                .depth = type == VK_IMAGE_TYPE_3D ? depth : 1,
                             },
                             .mipLevels = 1,
                             .arrayLayers = image->layer_count,
                             .samples = samples,
                             .tiling = tiling,
                             .usage = is_zs ?
                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          },
                          NULL, &image->image));

   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(ctx->device, image->image, &reqs);
   image->mem_size = reqs.size;

   int mem_type_index = vk_find_heap(ctx, reqs.memoryTypeBits, heap);
   if (mem_type_index == -1) {
      error("create_image: can't find memory type for reqs.memoryTypeBits=0x%x, heap=%u",
            reqs.memoryTypeBits, heap);
   }

   vk_check(vkAllocateMemory(ctx->device,
                             &(VkMemoryAllocateInfo) {
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = reqs.size,
                                .memoryTypeIndex = mem_type_index,
                             },
                             NULL, &image->mem));
   vk_check(vkBindImageMemory(ctx->device, image->image, image->mem, 0));
   vk_check(vkCreateImageView(ctx->device,
                              &(VkImageViewCreateInfo) {
                                 .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .image = image->image,
                                 /* Vulkan only supports layered rendering into 2D array views. */
                                 .viewType = (type == VK_IMAGE_TYPE_2D || type == VK_IMAGE_TYPE_3D) &&
                                             depth > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                                             type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D,
                                 .format = image->format,
                                 .components = {
                                    .r = VK_COMPONENT_SWIZZLE_R,
                                    .g = VK_COMPONENT_SWIZZLE_G,
                                    .b = VK_COMPONENT_SWIZZLE_B,
                                    .a = VK_COMPONENT_SWIZZLE_A,
                                 },
                                 .subresourceRange = {
                                    .aspectMask = format_is_depth_or_stencil(format) ?
                                                      VK_IMAGE_ASPECT_DEPTH_BIT :
                                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                    .levelCount = 1,
                                    .layerCount = image->depth,
                                 },
                              },
                              NULL, &image->render_compatible_view));

   if (heap == api_heap_device)
      ctx->device_mem_usage += image->mem_size;
   return image;
}

static void
vk_destroy_image(struct api_context *ctx, api_image *image)
{
   vkDestroyImageView(ctx->device, image->render_compatible_view, NULL);
   vkDestroyImage(ctx->device, image->image, NULL);
   vkFreeMemory(ctx->device, image->mem, NULL);
   free(image);
}

static void
vk_clear_image(struct api_context *ctx, api_image *image, const api_image_box *box,
               const VkClearColorValue *value)
{
   assert(!format_is_depth_or_stencil(image->format));
   assert(!box);

   vk_image_layout_transition(ctx, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

   vkCmdClearColorImage(ctx->current_cmd_buffer, image->image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, value, 1,
                        &(VkImageSubresourceRange){
                           .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = 1,
                           .layerCount = image->layer_count,
                        });
}

static void
vk_blit_image(struct api_context *ctx, api_blit_desc *desc)
{
   bool is_resolve = desc->src->samples > 1 && desc->dst->samples == 1;
   assert(!format_is_depth_or_stencil(desc->src->format));
   assert(!format_is_depth_or_stencil(desc->dst->format));

   vk_image_layout_transition(ctx, desc->src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   vk_image_layout_transition(ctx, desc->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

   if (desc->is_copy || is_resolve) {
      assert(desc->dst_box.width == desc->src_box.width);
      assert(desc->dst_box.height == desc->src_box.height);
      assert(desc->dst_box.depth == desc->src_box.depth);
      assert(desc->dst_box.width > 0 &&desc->src_box.width > 0);
      assert(desc->dst_box.height > 0 &&desc->src_box.height > 0);
      assert(desc->dst_box.depth > 0 &&desc->src_box.depth > 0);
      assert(!desc->linear_filter);

      if (is_resolve) {
         vkCmdResolveImage2(ctx->current_cmd_buffer, &(VkResolveImageInfo2){
                            .sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,
                            .srcImage = desc->src->image,
                            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            .dstImage = desc->dst->image,
                            .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            .regionCount = 1,
                            .pRegions = &(VkImageResolve2){
                               .sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,
                               .srcSubresource = (VkImageSubresourceLayers) {
                                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .layerCount = desc->src->layer_count,
                               },
                               .srcOffset = {desc->src_box.x, desc->src_box.y, desc->src_box.z},
                               .dstSubresource = (VkImageSubresourceLayers) {
                                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .layerCount = desc->dst->layer_count,
                               },
                               .dstOffset = {desc->dst_box.x, desc->dst_box.y, desc->dst_box.z},
                               .extent = {desc->dst_box.width, desc->dst_box.height, desc->dst_box.depth},
                            },
                         });
      } else {
         assert(desc->dst->samples == desc->src->samples);
         vkCmdCopyImage2(ctx->current_cmd_buffer, &(VkCopyImageInfo2){
                            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
                            .srcImage = desc->src->image,
                            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            .dstImage = desc->dst->image,
                            .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            .regionCount = 1,
                            .pRegions = &(VkImageCopy2){
                               .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
                               .srcSubresource = (VkImageSubresourceLayers) {
                                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .layerCount = desc->src->layer_count,
                               },
                               .srcOffset = {desc->src_box.x, desc->src_box.y, desc->src_box.z},
                               .dstSubresource = (VkImageSubresourceLayers) {
                                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .layerCount = desc->dst->layer_count,
                               },
                               .dstOffset = {desc->dst_box.x, desc->dst_box.y, desc->dst_box.z},
                               .extent = {desc->dst_box.width, desc->dst_box.height, desc->dst_box.depth},
                            },
                         });
      }
   } else {
      vkCmdBlitImage2(ctx->current_cmd_buffer, &(VkBlitImageInfo2){
                         .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
                         .srcImage = desc->src->image,
                         .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         .dstImage = desc->dst->image,
                         .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         .regionCount = 1,
                         .pRegions = &(VkImageBlit2){
                            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                            .srcSubresource = (VkImageSubresourceLayers) {
                               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .layerCount = desc->src->layer_count,
                            },
                            .srcOffsets = {{desc->src_box.x, desc->src_box.y, desc->src_box.z},
                                           {desc->src_box.x + desc->src_box.width,
                                            desc->src_box.y + desc->src_box.height,
                                            desc->src_box.z + desc->src_box.depth}},
                            .dstSubresource = (VkImageSubresourceLayers) {
                               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .layerCount = desc->dst->layer_count,
                            },
                            .dstOffsets = {{desc->dst_box.x, desc->dst_box.y, desc->dst_box.z},
                                           {desc->dst_box.x + desc->dst_box.width,
                                            desc->dst_box.y + desc->dst_box.height,
                                            desc->dst_box.z + desc->dst_box.depth}},
                         },
                         .filter = desc->linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                     });
   }
}

static api_framebuffer *
vk_create_framebuffer(api_context *ctx, api_image *colorbuf, api_image *zbuf,
                      unsigned width, unsigned height, unsigned samples)
{
   api_framebuffer *fb = calloc(1, sizeof(api_framebuffer));
   fb->width = width;
   fb->height = height;
   fb->samples = samples;
   fb->colorbuf = colorbuf;
   fb->zbuf = zbuf;

   fb->colorbuf_att_index = 0;
   fb->zbuf_att_index = 0;
   fb->num_attachments = 0;

   VkAttachmentDescription att_descs[2];
   VkImageView att_views[2];

   if (colorbuf) {
      assert(!format_is_depth_or_stencil(colorbuf->format));
      assert(fb->num_attachments < ARRAY_SIZE(att_descs));

      att_descs[fb->num_attachments] = (VkAttachmentDescription){
         .format = colorbuf->format,
         .samples = colorbuf->samples,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      };

      att_views[fb->num_attachments] = colorbuf->render_compatible_view;
      fb->colorbuf_att_index = fb->num_attachments;
      fb->num_attachments++;
   }

   if (zbuf) {
      assert(zbuf->type != VK_IMAGE_TYPE_3D);
      assert(!colorbuf || zbuf->depth == colorbuf->depth);
      assert(fb->num_attachments < ARRAY_SIZE(att_descs));

      att_descs[fb->num_attachments] = (VkAttachmentDescription){
         .format = zbuf->format,
         .samples = zbuf->samples,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      };

      att_views[fb->num_attachments] = zbuf->render_compatible_view;
      fb->zbuf_att_index = fb->num_attachments;
      fb->num_attachments++;
   }

   vk_check(vkCreateRenderPass(ctx->device,
                               &(VkRenderPassCreateInfo) {
                                  .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = fb->num_attachments,
                                  .pAttachments = att_descs,
                                  .subpassCount = 1,
                                  .pSubpasses = (VkSubpassDescription[]) {
                                     {
                                        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = (VkAttachmentReference[]) {
                                           {
                                              .attachment = colorbuf ? fb->colorbuf_att_index : VK_ATTACHMENT_UNUSED,
                                              .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           },
                                        },
                                        .pDepthStencilAttachment = &(VkAttachmentReference) {
                                           .attachment = zbuf ? fb->zbuf_att_index : VK_ATTACHMENT_UNUSED,
                                           .layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                        },
                                     }
                                  },
                               },
                               NULL, &fb->render_pass));

   vk_check(vkCreateFramebuffer(ctx->device,
                                &(VkFramebufferCreateInfo) {
                                   .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = fb->render_pass,
                                   .attachmentCount = fb->num_attachments,
                                   .pAttachments = att_views,
                                   .width = fb->width,
                                   .height = fb->height,
                                   .layers = colorbuf ? colorbuf->depth : 1,
                                },
                                NULL, &fb->fb));

   return fb;
}

static void
vk_destroy_framebuffer(struct api_context *ctx, api_framebuffer *fb)
{
   vkDestroyFramebuffer(ctx->device, fb->fb, NULL);
   vkDestroyRenderPass(ctx->device, fb->render_pass, NULL);
   free(fb);
}

static api_shader *
vk_create_shader(api_context *ctx, const char *source, api_shader_type type)
{
   shaderc_shader_kind shaderc_type;

   switch (type) {
   case api_shader_vs:
      shaderc_type = shaderc_glsl_vertex_shader;
      break;
   case api_shader_tcs:
      shaderc_type = shaderc_glsl_tess_control_shader;
      break;
   case api_shader_tes:
      shaderc_type = shaderc_glsl_tess_evaluation_shader;
      break;
   case api_shader_gs:
      shaderc_type = shaderc_glsl_geometry_shader;
      break;
   case api_shader_fs:
      shaderc_type = shaderc_glsl_fragment_shader;
      break;
   case api_shader_cs:
      shaderc_type = shaderc_glsl_compute_shader;
      break;
   case api_shader_ts:
      shaderc_type = shaderc_glsl_task_shader;
      break;
   case api_shader_ms:
      shaderc_type = shaderc_glsl_mesh_shader;
      break;
   default:
      error("invalid shader type");
   }

   shaderc_compilation_result_t result =
       shaderc_compile_into_spv(ctx->glsl_compiler, source, strlen(source),
                                shaderc_type, "", "main", ctx->glsl_compiler_options);
   if (!result)
      error("shaderc_compile_into_spv returned NULL");

   if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
      error("failed to compile shader to SPIR-V:\n%s\n\n%s", source,
            shaderc_result_get_error_message(result));
   }


   api_shader *shader = calloc(1, sizeof(api_shader));

   vk_check(vkCreateShaderModule(ctx->device,
                                 &(VkShaderModuleCreateInfo) {
                                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = shaderc_result_get_length(result),
                                    .pCode = (uint32_t*)shaderc_result_get_bytes(result),
                                 },
                                 NULL, &shader->module));
   shaderc_result_release(result);
   return shader;
}

static api_descriptor_set_layout *
vk_create_descriptor_set_layout(api_context *ctx,
                                const api_descriptor_set_layout_desc *desc)
{
   api_descriptor_set_layout *layout = calloc(1, sizeof(api_descriptor_set_layout));
   layout->desc = *desc;

   VkShaderStageFlags stage_flags = (ctx->max_mesh_workgroup_size ? VK_SHADER_STAGE_MESH_BIT_EXT : 0) |
                                    VK_SHADER_STAGE_FRAGMENT_BIT;
   unsigned num_bindings = 0;
   VkDescriptorSetLayoutBinding desc_set_layout_bindings[8];

   if (desc->uniform_buffer.array_size) {
      assert(desc->uniform_buffer.array_size <= MAX_UNIFORM_BUFFER_ARRAY_SIZE);
      assert(num_bindings < ARRAY_SIZE(desc_set_layout_bindings));

      desc_set_layout_bindings[num_bindings++] = (VkDescriptorSetLayoutBinding){
         .binding = desc->uniform_buffer.vk_binding,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = desc->uniform_buffer.array_size,
         .stageFlags = stage_flags,
      };
   }

   for (unsigned i = 0; i < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS; i++) {
      if (desc->uniform_texel_buffer[i].array_size) {
         assert(desc->uniform_texel_buffer[i].array_size <= MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE);
         assert(num_bindings < ARRAY_SIZE(desc_set_layout_bindings));

         desc_set_layout_bindings[num_bindings++] = (VkDescriptorSetLayoutBinding){
            .binding = desc->uniform_texel_buffer[i].vk_binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            .descriptorCount = desc->uniform_texel_buffer[i].array_size,
            .stageFlags = stage_flags,
         };
      }
   }

   if (desc->storage_image.array_size) {
      assert(desc->storage_image.array_size <= MAX_STORAGE_IMAGE_ARRAY_SIZE);
      assert(num_bindings < ARRAY_SIZE(desc_set_layout_bindings));

      desc_set_layout_bindings[num_bindings++] = (VkDescriptorSetLayoutBinding){
         .binding = desc->storage_image.vk_binding,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = desc->storage_image.array_size,
         .stageFlags = stage_flags,
      };
   }

   vk_check(vkCreateDescriptorSetLayout(ctx->device,
                                        &(VkDescriptorSetLayoutCreateInfo) {
                                           .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = num_bindings,
                                           .pBindings = desc_set_layout_bindings,
                                        },
                                        NULL, &layout->desc_set_layout));
   vk_check(vkCreatePipelineLayout(ctx->device,
                                   &(VkPipelineLayoutCreateInfo) {
                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &layout->desc_set_layout,
                                   },
                                   NULL, &layout->pipeline_layout));
   return layout;
}

static api_descriptor_set *
vk_create_descriptor_set(api_context *ctx, api_descriptor_set_layout *layout)
{
   api_descriptor_set *set = calloc(1, sizeof(api_descriptor_set));
   set->layout = layout;

   assert(ctx->num_allocated_desc_sets < MAX_DESCRIPTOR_SETS);

   vk_check(vkAllocateDescriptorSets(ctx->device,
                                     &(VkDescriptorSetAllocateInfo){
                                        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = ctx->descriptor_pool,
                                        .descriptorSetCount = 1,
                                        .pSetLayouts = &layout->desc_set_layout,
                                     },
                                     &set->set));
   ctx->num_allocated_desc_sets++;
   return set;
}

static void
vk_set_uniform_buffer_descriptors(api_context *ctx, api_descriptor_set *set,
                                  api_buffer *buffer, uint64_t offset, uint64_t size)
{
   assert(set->layout->desc.uniform_buffer.array_size);

   vkUpdateDescriptorSets(ctx->device, 1,
                          &(VkWriteDescriptorSet){
                             .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set->set,
                             .dstBinding = set->layout->desc.uniform_buffer.vk_binding,
                             .dstArrayElement = 0,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             .pBufferInfo = &(VkDescriptorBufferInfo){
                                .buffer = buffer->buffer,
                                .offset = offset,
                                .range = size,
                             },
                          },
                          0, NULL);
}

static void
vk_set_uniform_texel_buffer_descriptors(api_context *ctx, api_descriptor_set *set,
                                        unsigned binding_index, unsigned num_buffers,
                                        api_buffer **buffers, VkFormat *formats,
                                        uint64_t *offsets, uint64_t *sizes)
{
   assert(num_buffers <= set->layout->desc.uniform_texel_buffer[binding_index].array_size);

   for (unsigned i = 0; i < num_buffers; i++) {
      if (set->texel_buffer_views[binding_index][i])
         vkDestroyBufferView(ctx->device, set->texel_buffer_views[binding_index][i], NULL);

      vk_check(vkCreateBufferView(ctx->device, &(VkBufferViewCreateInfo){
                                     .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                     .buffer = buffers[i]->buffer,
                                     .format = formats[i],
                                     .offset = offsets[i],
                                     .range = sizes[i],
                                  },
                                  NULL, &set->texel_buffer_views[binding_index][i]));
   }

   vkUpdateDescriptorSets(ctx->device, 1,
                          &(VkWriteDescriptorSet){
                             .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set->set,
                             .dstBinding = set->layout->desc.uniform_texel_buffer[binding_index].vk_binding,
                             .dstArrayElement = 0,
                             .descriptorCount = num_buffers,
                             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                             .pTexelBufferView = set->texel_buffer_views[binding_index],
                          },
                          0, NULL);
}

static void
vk_set_storage_image_descriptors(api_context *ctx, api_descriptor_set *set, unsigned num_images,
                                 api_image **images)
{
   assert(num_images <= set->layout->desc.storage_image.array_size);
   VkDescriptorImageInfo *image_infos = alloca(sizeof(*image_infos) * num_images);

   for (unsigned i = 0; i < num_images; i++) {
      if (images[i]->layout != VK_IMAGE_LAYOUT_GENERAL) {
         ctx->begin_cmdbuf(ctx);
         vk_image_layout_transition(ctx, images[i], VK_IMAGE_LAYOUT_GENERAL);
         ctx->end_cmdbuf_and_submit(ctx);
      }

      image_infos[i].sampler = NULL;
      image_infos[i].imageView = images[i]->render_compatible_view;
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   }

   vkUpdateDescriptorSets(ctx->device, 1,
                          &(VkWriteDescriptorSet){
                             .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set->set,
                             .dstBinding = set->layout->desc.storage_image.vk_binding,
                             .dstArrayElement = 0,
                             .descriptorCount = num_images,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                             .pImageInfo = image_infos,
                          },
                          0, NULL);
}

static void
vk_bind_descriptor_set(api_context *ctx, api_descriptor_set *set)
{
   vkCmdBindDescriptorSets(ctx->current_cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           set->layout->pipeline_layout, 0, 1, &set->set, 0, NULL);
}

static api_pipeline *
vk_create_pipeline(api_context *ctx, const api_pipeline_desc *desc)
{
   api_pipeline *pipeline = calloc(1, sizeof(api_pipeline));
   pipeline->desc = *desc;
   pipeline->num_vb_desc = desc->num_vb_desc;

   /* Vertex buffers and attributes. */
   VkVertexInputBindingDescription vi_bindings[MAX_VERTEX_BUFFERS];
   VkVertexInputAttributeDescription attr_desc[MAX_VERTEX_BUFFERS];

   assert(desc->num_vb_desc <= MAX_VERTEX_BUFFERS);

   for (unsigned i = 0; i < desc->num_vb_desc; i++) {
      pipeline->dyn_vi_bindings[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
      pipeline->dyn_vi_bindings[i].binding = vi_bindings[i].binding = i;
      pipeline->dyn_vi_bindings[i].stride = vi_bindings[i].stride = desc->vb_strides[i];
      pipeline->dyn_vi_bindings[i].inputRate = vi_bindings[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
      pipeline->dyn_vi_bindings[i].divisor = 1;

      pipeline->dyn_vi_attribs[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
      pipeline->dyn_vi_attribs[i].location = attr_desc[i].location = i;
      pipeline->dyn_vi_attribs[i].binding = attr_desc[i].binding = i;
      pipeline->dyn_vi_attribs[i].format = attr_desc[i].format = desc->vb_formats[i];
      pipeline->dyn_vi_attribs[i].offset = attr_desc[i].offset = 0;
   }

   VkPipelineVertexInputStateCreateInfo vertex_input_state = (VkPipelineVertexInputStateCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = desc->num_vb_desc,
      .pVertexBindingDescriptions = vi_bindings,
      .vertexAttributeDescriptionCount = desc->num_vb_desc,
      .pVertexAttributeDescriptions = attr_desc,
   };

   /* Shaders. */
   VkPipelineShaderStageCreateInfo shaders[5];
   unsigned num_shaders = 0;

   if (desc->ms) {
      shaders[num_shaders++] = (VkPipelineShaderStageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
         .module = desc->ms->module,
         .pName = "main",
      };
   }

   if (desc->vs) {
      shaders[num_shaders++] = (VkPipelineShaderStageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = desc->vs->module,
         .pName = "main",
      };
   }

   if (desc->fs && !desc->rasterizer_discard) {
      shaders[num_shaders++] = (VkPipelineShaderStageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = desc->fs->module,
         .pName = "main",
      };
   }

   bool uses_dynamic_state = ctx->options.api_flavor >= API_VK_DYNAMIC_STATE;
   VkDynamicState dynamic_states[20] = {
      VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
      VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
   };
   unsigned num_always_dynamic_states = 2;
   unsigned num_dynamic_states = 2;

   if (uses_dynamic_state) {
      if (!desc->ms) {
         assert(num_dynamic_states + 3 <= ARRAY_SIZE(dynamic_states));
         dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
         dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE;
         dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
      }

      assert(num_dynamic_states + 6 <= ARRAY_SIZE(dynamic_states));
      dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;
      dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_CULL_MODE;
      dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_FRONT_FACE;
      dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT;
      dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
      dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
   }

   /* Create the graphics pipeline. */
   VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = num_shaders,
      .pStages = shaders,
      .pVertexInputState = uses_dynamic_state ? NULL : &vertex_input_state,
      .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = desc->topology, /* the topology class must still match dynamic state */
         .primitiveRestartEnable = uses_dynamic_state ? false : desc->primitive_restart,
      },
      .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .rasterizerDiscardEnable = uses_dynamic_state ? false : desc->rasterizer_discard,
         .cullMode = uses_dynamic_state ? 0 : desc->cull_mode,
         .frontFace = uses_dynamic_state ? 0 : VK_FRONT_FACE_CLOCKWISE,
         .lineWidth = 1.0f,
      },
      .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = uses_dynamic_state ? 1 : desc->fb->samples,
         .sampleShadingEnable = desc->sample_shading,
         .minSampleShading = 1,
         .pSampleMask = uses_dynamic_state ? NULL : (VkSampleMask[]){desc->samplemask},
         .alphaToCoverageEnable = uses_dynamic_state ? false : desc->alpha_to_coverage,
      },
      .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = desc->depth_enabled,
         .depthWriteEnable = desc->depth_write_enabled,
         .depthCompareOp = desc->depth_compare_op,
      },
      .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkPipelineColorBlendAttachmentState[]) {
            {
               .blendEnable = desc->blend_src_color || desc->blend_src_alpha,
               .srcColorBlendFactor = desc->blend_src_alpha ? VK_BLEND_FACTOR_SRC_ALPHA :
                                                              VK_BLEND_FACTOR_SRC_COLOR,
               .dstColorBlendFactor = desc->blend_src_alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA :
                                                              VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
               .colorBlendOp = VK_BLEND_OP_ADD,
               .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
               .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
               .alphaBlendOp = VK_BLEND_OP_ADD,
               .colorWriteMask = desc->colormask,
            },
         },
      },
      .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
         .dynamicStateCount = uses_dynamic_state ? num_dynamic_states : num_always_dynamic_states,
         .pDynamicStates = dynamic_states,
      },
      .layout = desc->desc_set_layout ? desc->desc_set_layout->pipeline_layout :
                                        ctx->empty_pipeline_layout,
      .renderPass = desc->fb->render_pass,
   };

   assert(desc->vrs_fragment_size[0] && desc->vrs_fragment_size[1]);
   assert(ctx->has_vrs || (desc->vrs_fragment_size[0] == 1 && desc->vrs_fragment_size[1] == 1));

   VkPipelineFragmentShadingRateStateCreateInfoKHR vrs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
      .fragmentSize = {desc->vrs_fragment_size[0], desc->vrs_fragment_size[1]},
      .combinerOps = {
         VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
         VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
      },
   };

   if (ctx->has_vrs)
      pipeline_info.pNext = &vrs;

   vk_check(vkCreateGraphicsPipelines(ctx->device, (VkPipelineCache){ VK_NULL_HANDLE },
                                      1, &pipeline_info, NULL, &pipeline->pipeline));
   return pipeline;
}

static void
vk_destroy_pipeline(struct api_context *ctx, api_pipeline *pipeline)
{
   vkDestroyPipeline(ctx->device, pipeline->pipeline, NULL);
   free(pipeline);
}

static void
vk_bind_pipeline(api_context *ctx, api_pipeline *pipeline)
{
   assert(pipeline);
   ctx->current_pipeline = pipeline;
   vkCmdBindPipeline(ctx->current_cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

   if (ctx->options.api_flavor >= API_VK_DYNAMIC_STATE) {
      if (!pipeline->desc.ms) {
         ctx->vkCmdSetVertexInputEXT(ctx->current_cmd_buffer,
                                     pipeline->num_vb_desc, pipeline->dyn_vi_bindings,
                                     pipeline->num_vb_desc, pipeline->dyn_vi_attribs);
         vkCmdSetPrimitiveTopology(ctx->current_cmd_buffer, pipeline->desc.topology);
         vkCmdSetPrimitiveRestartEnable(ctx->current_cmd_buffer, pipeline->desc.primitive_restart);
      }

      vkCmdSetRasterizerDiscardEnable(ctx->current_cmd_buffer, pipeline->desc.rasterizer_discard);
      vkCmdSetCullMode(ctx->current_cmd_buffer, pipeline->desc.cull_mode);
      vkCmdSetFrontFace(ctx->current_cmd_buffer, VK_FRONT_FACE_CLOCKWISE);

      ctx->vkCmdSetRasterizationSamplesEXT(ctx->current_cmd_buffer, pipeline->desc.fb->samples);
      ctx->vkCmdSetSampleMaskEXT(ctx->current_cmd_buffer, pipeline->desc.fb->samples, &pipeline->desc.samplemask);
      ctx->vkCmdSetAlphaToCoverageEnableEXT(ctx->current_cmd_buffer, pipeline->desc.alpha_to_coverage);
   }
}

static void
vk_begin_cmdbuf(api_context *ctx)
{
   assert(ctx->current_cmd_buffer == NULL);
   assert(ctx->next_cmdbuf_index < MAX_COMMAND_BUFFERS);
   ctx->current_cmd_buffer = ctx->cmd_buffers[ctx->next_cmdbuf_index];
   ctx->current_fence = ctx->fences[ctx->next_cmdbuf_index];

   vk_check(vkWaitForFences(ctx->device, 1, &ctx->current_fence, true, UINT64_MAX));
   vk_check(vkResetFences(ctx->device, 1, &ctx->current_fence));

   vk_check(vkBeginCommandBuffer(ctx->current_cmd_buffer,
                                 &(VkCommandBufferBeginInfo) {
                                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = 0
                                 }));
}

static void
vk_end_cmdbuf_and_submit(api_context *ctx)
{
   vk_check(vkEndCommandBuffer(ctx->current_cmd_buffer));

   vk_check(vkQueueSubmit(ctx->queue, 1,
      &(VkSubmitInfo) {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &ctx->current_cmd_buffer,
      }, ctx->current_fence));

   ctx->next_cmdbuf_index = (ctx->next_cmdbuf_index + 1) % MAX_COMMAND_BUFFERS;
   ctx->current_cmd_buffer = NULL;
   ctx->current_fence = NULL;
   ctx->current_pipeline = NULL;
}

static void
wait_for_last_fence(api_context *ctx)
{
   unsigned last_cmdbuf = (ctx->next_cmdbuf_index - 1) % MAX_COMMAND_BUFFERS;

   vk_check(vkWaitForFences(ctx->device, 1, &ctx->fences[last_cmdbuf], true, UINT64_MAX));
}

static void
vk_wait_idle_before_deallocation(api_context *ctx)
{
   wait_for_last_fence(ctx);
}

static void
vk_begin_render_pass(api_context *ctx, const api_render_pass_desc *desc)
{
   VkClearValue *clear_values = alloca(desc->fb->num_attachments * sizeof(VkClearValue));

   if (desc->fb->colorbuf)
      clear_values[desc->fb->colorbuf_att_index] = (VkClearValue){.color = desc->color_clear_value};
   if (desc->fb->zbuf)
      clear_values[desc->fb->zbuf_att_index] = (VkClearValue){.depthStencil.depth = desc->depth_clear_value};

   vkCmdBeginRenderPass(ctx->current_cmd_buffer,
                        &(VkRenderPassBeginInfo) {
                           .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                           .renderPass = desc->fb->render_pass,
                           .framebuffer = desc->fb->fb,
                           .renderArea = { { 0, 0 }, { desc->fb->width, desc->fb->height } },
                           .clearValueCount = desc->fb->num_attachments,
                           .pClearValues = clear_values,
                        },
                        VK_SUBPASS_CONTENTS_INLINE);

   if (desc->fb->colorbuf)
      desc->fb->colorbuf->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   if (desc->fb->zbuf)
      desc->fb->zbuf->layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

   const VkViewport viewport = {
      .x = 0,
      .y = 0,
      .width = desc->fb->width,
      .height = desc->fb->height,
      .minDepth = 0,
      .maxDepth = 1,
   };
   vkCmdSetViewportWithCount(ctx->current_cmd_buffer, 1, &viewport);

   const VkRect2D scissor = {
      .offset = { 0, 0 },
      .extent = { desc->fb->width, desc->fb->height },
   };
   vkCmdSetScissorWithCount(ctx->current_cmd_buffer, 1, &scissor);
}

static void
vk_end_render_pass(api_context *ctx)
{
   vkCmdEndRenderPass(ctx->current_cmd_buffer);
}

static void
vk_bind_vertex_buffers(api_context *ctx, api_buffer *vb, const uint64_t *vb_offsets)
{
   VkBuffer buffers[MAX_VERTEX_BUFFERS];

   for (unsigned i = 0; i < ctx->current_pipeline->num_vb_desc; i++)
      buffers[i] = vb->buffer;

   vkCmdBindVertexBuffers(ctx->current_cmd_buffer, 0, ctx->current_pipeline->num_vb_desc,
                          buffers, vb_offsets);
}

static void
vk_bind_index_buffer(api_context *ctx, api_buffer *ib)
{
   vkCmdBindIndexBuffer(ctx->current_cmd_buffer, ib->buffer, 0, VK_INDEX_TYPE_UINT32);
}

static void
vk_draw(api_context *ctx, const api_draw_desc *desc)
{
   assert(desc->count && (desc->mesh_shader || desc->instance_count));

   if (desc->mesh_shader)
      ctx->vkCmdDrawMeshTasksEXT(ctx->current_cmd_buffer, desc->count, 1, 1);
   else if (desc->indexed)
      vkCmdDrawIndexed(ctx->current_cmd_buffer, desc->count, desc->instance_count, 0, 0, 0);
   else
      vkCmdDraw(ctx->current_cmd_buffer, desc->count, desc->instance_count, desc->first_vertex, 0);
}

static void
vk_pipeline_barrier(struct api_context *ctx, VkPipelineStageFlagBits2 stage_bits)
{
   vkCmdPipelineBarrier2(ctx->current_cmd_buffer, &(VkDependencyInfo) {
                            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .memoryBarrierCount = 1,
                            .pMemoryBarriers = &(VkMemoryBarrier2){
                               .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = stage_bits,
                               .dstStageMask = stage_bits,
                            },
                         });
}

static api_timestamp_query_pool *
vk_create_timestamp_pool(api_context *ctx, unsigned num_queries)
{
   api_timestamp_query_pool *pool = calloc(1, sizeof(api_timestamp_query_pool));

   pool->num_written_queries = 0;
   pool->num_queries = num_queries;
   pool->results = calloc(num_queries, sizeof(uint64_t));

   vk_check(vkCreateQueryPool(ctx->device,
                              &(VkQueryPoolCreateInfo){
                                 .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                 .queryCount = num_queries,
                              }, NULL, &pool->pool));

   vk_begin_cmdbuf(ctx);
   vkCmdResetQueryPool(ctx->current_cmd_buffer, pool->pool, 0, pool->num_queries);
   vk_end_cmdbuf_and_submit(ctx);

   return pool;
}

static void
vk_write_next_timestamp(api_context *ctx, api_timestamp_query_pool *pool)
{
   assert(pool->num_written_queries < pool->num_queries);
   vkCmdWriteTimestamp(ctx->current_cmd_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool->pool,
                       pool->num_written_queries);
   pool->num_written_queries++;
}

static void
vk_query_timestamps(api_context *ctx, api_timestamp_query_pool *pool)
{
   pool->num_read_queries = 0;

   if (!pool->num_written_queries)
      return;

   wait_for_last_fence(ctx);
   vk_check(vkGetQueryPoolResults(ctx->device, pool->pool, 0, pool->num_written_queries,
                                  sizeof(uint64_t) * pool->num_written_queries, pool->results,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
}

static void
vk_upload_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      const void *data)
{
   api_buffer *staging = vk_create_buffer(ctx, size, api_heap_host_uncached);
   uint8_t *map;

   vk_check(vkMapMemory(ctx->device, staging->mem, 0, size, 0, (void**)&map));
   memcpy(map, data, size);
   vkUnmapMemory(ctx->device, staging->mem);

   vk_begin_cmdbuf(ctx);
   vkCmdCopyBuffer2(ctx->current_cmd_buffer,
                    &(VkCopyBufferInfo2) {
                       .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                       .srcBuffer = staging->buffer,
                       .dstBuffer = buf->buffer,
                       .regionCount = 1,
                       .pRegions = &(VkBufferCopy2) {
                          .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                          .srcOffset = 0,
                          .dstOffset = offset,
                          .size = size,
                       },
                    });
   vkCmdPipelineBarrier2(ctx->current_cmd_buffer, &(VkDependencyInfo) {
                            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .bufferMemoryBarrierCount = 1,
                            .pBufferMemoryBarriers = &(VkBufferMemoryBarrier2) {
                               .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                               .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
                               .buffer = buf->buffer,
                               .offset = offset,
                               .size = size,
                            },
                         });
   vk_end_cmdbuf_and_submit(ctx);
}

static void
vk_image_write_png(api_context *ctx, api_image *image, unsigned layer, const char *filename)
{
   api_image *staging = vk_create_image(ctx, VK_IMAGE_TYPE_2D,
                                        format_is_integer(image->format) ?
                                           VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM,
                                        image->width, image->height, 1, 1, VK_IMAGE_TILING_LINEAR,
                                        api_heap_host_cached);

   vk_begin_cmdbuf(ctx);
   vk_image_layout_transition(ctx, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   vk_image_layout_transition(ctx, staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

   vkCmdBlitImage2(ctx->current_cmd_buffer,
                   &(VkBlitImageInfo2) {
                      .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
                      .srcImage = image->image,
                      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      .dstImage = staging->image,
                      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      .regionCount = 1,
                      .pRegions = &(VkImageBlit2) {
                         .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                         .srcSubresource = (VkImageSubresourceLayers) {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .baseArrayLayer = image->type == VK_IMAGE_TYPE_2D ? layer : 0,
                            .layerCount = 1,
                         },
                         .srcOffsets = {{0, 0, image->type == VK_IMAGE_TYPE_3D ? layer : 0},
                                        {image->width, image->height, 1}},
                         .dstSubresource = (VkImageSubresourceLayers) {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .layerCount = 1,
                         },
                         .dstOffsets = {{0, 0, 0}, {image->width, image->height, 1}},
                      },
                  });
   vk_end_cmdbuf_and_submit(ctx);

   vk_check(vkQueueWaitIdle(ctx->queue));

   void *map;
   vk_check(vkMapMemory(ctx->device, staging->mem, 0, staging->mem_size, 0, &map));
   write_png_rgba8(filename, staging, map);
   vkUnmapMemory(ctx->device, staging->mem);
}

api_context *
vk_create_context(const program_options *options)
{
   api_context *ctx = calloc(1, sizeof(api_context));
   ctx->options = *options;

   /* Create the Vulkan instance. */
   VkInstance instance;
   vk_check(vkCreateInstance(&(VkInstanceCreateInfo) {
                                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &(VkApplicationInfo) {
                                   .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .pApplicationName = "main",
                                   .apiVersion = VK_MAKE_VERSION(1, 3, 0),
                                },
                                .enabledLayerCount = vk_has_validation_layer() ? 1 : 0,
                                .ppEnabledLayerNames = (const char *[1]) {
                                   "VK_LAYER_KHRONOS_validation",
                                },
                             },
                             NULL, &instance));

   /* Enumerate devices. */
   unsigned count;
   vk_check(vkEnumeratePhysicalDevices(instance, &count, NULL));
   if (count == 0)
      error("No Vulkan devices found.");
   VkPhysicalDevice *pd = alloca(sizeof(pd[0]) * count);
   vk_check(vkEnumeratePhysicalDevices(instance, &count, pd));
   VkPhysicalDevice physical_device = pd[0];
   printf("Physical devices: %u\n", count);

   VkPhysicalDeviceExtendedDynamicState3FeaturesEXT ext_dyn3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
   };
   VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi_dyn = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
      .pNext = &ext_dyn3,
   };
   VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
      .pNext = &vi_dyn,
   };
   VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
      .pNext = &vrs,
   };
   VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
      .pNext = &mesh,
   };
   VkPhysicalDeviceVulkan13Features vulkan13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &xfb,
   };
   VkPhysicalDeviceVulkan12Features vulkan12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &vulkan13,
   };
   VkPhysicalDeviceVulkan11Features vulkan11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &vulkan12,
   };
   VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &vulkan11,
   };
   vkGetPhysicalDeviceFeatures2(physical_device, &features2);
   vkGetPhysicalDeviceMemoryProperties(physical_device, &ctx->memory_properties);

   VkPhysicalDeviceExtendedDynamicState3PropertiesEXT ext_dyn3_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT,
   };
   VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT,
      .pNext = &ext_dyn3_properties,
   };
   VkPhysicalDeviceProperties2 device_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &mesh_properties,
   };
   vkGetPhysicalDeviceProperties2(physical_device, &device_properties);
   printf("Device: %s\n", device_properties.properties.deviceName);

   /* Check that the first queue support graphics. */
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
   if (!count)
      error("vkGetPhysicalDeviceQueueFamilyProperties returned count=0");
   VkQueueFamilyProperties *queue_props = alloca(sizeof(queue_props[0]) * count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, queue_props);
   if (!(queue_props[0].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      error("the first queue must have VK_QUEUE_GRAPHICS_BIT");

   unsigned num_enabled_extensions = 0;
   const char *enabled_extensions[5];

   if (vi_dyn.vertexInputDynamicState) {
      assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions));
      enabled_extensions[num_enabled_extensions++] = "VK_EXT_vertex_input_dynamic_state";
   }

   if (vrs.pipelineFragmentShadingRate) {
      assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions));
      enabled_extensions[num_enabled_extensions++] = "VK_KHR_fragment_shading_rate";
      vrs.attachmentFragmentShadingRate = false;
   }

   if (mesh.meshShader) {
      assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions));
      enabled_extensions[num_enabled_extensions++] = "VK_EXT_mesh_shader";
   }

   if (xfb.transformFeedback) {
      assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions));
      enabled_extensions[num_enabled_extensions++] = "VK_EXT_transform_feedback";
   }

   if (ext_dyn3.extendedDynamicState3RasterizationSamples ||
       ext_dyn3.extendedDynamicState3SampleMask ||
       ext_dyn3.extendedDynamicState3AlphaToCoverageEnable) {
      assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions));
      enabled_extensions[num_enabled_extensions++] = "VK_EXT_extended_dynamic_state3";
   }

   if (ctx->options.api_flavor >= API_VK_DYNAMIC_STATE) {
      if (!vi_dyn.vertexInputDynamicState)
         error("VK_EXT_vertex_input_dynamic_state is required.");
      if (!ext_dyn3.extendedDynamicState3RasterizationSamples)
         error("extendedDynamicState3RasterizationSamples is required.");
      if (!ext_dyn3.extendedDynamicState3SampleMask)
         error("extendedDynamicState3SampleMask is required.");
      if (!ext_dyn3.extendedDynamicState3AlphaToCoverageEnable)
         error("extendedDynamicState3AlphaToCoverageEnable is required.");
   }

   /* Open the device. */
   vk_check(vkCreateDevice(physical_device,
                           &(VkDeviceCreateInfo) {
                              .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = &features2,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
                                 .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = 0,
                                 .queueCount = 1,
                                 .pQueuePriorities = (float[]) { 1.0f },
                              },
                              .enabledExtensionCount = num_enabled_extensions,
                              .ppEnabledExtensionNames = enabled_extensions,
                           },
                           NULL, &ctx->device));

   /* Get extension functions. */
#define GET_PROC_ADDR(name) ctx->name = (PFN_##name)vkGetDeviceProcAddr(ctx->device, #name)

   if (vi_dyn.vertexInputDynamicState)
      GET_PROC_ADDR(vkCmdSetVertexInputEXT);
   if (mesh.meshShader)
      GET_PROC_ADDR(vkCmdDrawMeshTasksEXT);
   if (ext_dyn3.extendedDynamicState3RasterizationSamples)
      GET_PROC_ADDR(vkCmdSetRasterizationSamplesEXT);
   if (ext_dyn3.extendedDynamicState3SampleMask)
      GET_PROC_ADDR(vkCmdSetSampleMaskEXT);
   if (ext_dyn3.extendedDynamicState3AlphaToCoverageEnable)
      GET_PROC_ADDR(vkCmdSetAlphaToCoverageEnableEXT);

   /* Get the queue. */
   vkGetDeviceQueue2(ctx->device, &(VkDeviceQueueInfo2) {
                        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                        .queueFamilyIndex = 0,
                        .queueIndex = 0,
                     }, &ctx->queue);

   vk_check(vkCreateCommandPool(ctx->device,
                                &(const VkCommandPoolCreateInfo) {
                                   .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = 0,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                },
                                NULL, &ctx->cmd_buffer_pool));

   vk_check(vkAllocateCommandBuffers(ctx->device,
      &(VkCommandBufferAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = ctx->cmd_buffer_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = MAX_COMMAND_BUFFERS,
      },
      ctx->cmd_buffers));

   for (unsigned i = 0; i < MAX_COMMAND_BUFFERS; i++) {
      vk_check(vkCreateFence(ctx->device, &(VkFenceCreateInfo){
                                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
                             },
                             NULL, &ctx->fences[i]));
   }

   vk_check(vkCreateDescriptorPool(ctx->device,
                                   &(VkDescriptorPoolCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = MAX_DESCRIPTOR_SETS,
                                      .poolSizeCount = 3,
                                      .pPoolSizes = (VkDescriptorPoolSize[3]){
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_STORAGE_IMAGE_BINDINGS,
                                         },
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_UNIFORM_TEXEL_BUFFER_BINDINGS,
                                         },
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_UNIFORM_BUFFER_BINDINGS,
                                         },
                                      },
                                   },
                                   NULL, &ctx->descriptor_pool));

   vk_check(vkCreatePipelineLayout(ctx->device,
                                   &(VkPipelineLayoutCreateInfo) {
                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                   },
                                   NULL, &ctx->empty_pipeline_layout));

   ctx->next_cmdbuf_index = 0;
   ctx->glsl_compiler = shaderc_compiler_initialize();
   ctx->glsl_compiler_options = shaderc_compile_options_initialize();

   ctx->has_host_uncached_heap = true; /* so that vk_find_heap doesn't fall back and fails when it should */
   ctx->has_host_cached_heap = true; /* so that vk_find_heap doesn't fall back and fails when it should */
   ctx->has_host_uncached_heap = vk_find_heap(ctx, ~0, api_heap_host_uncached) != -1;
   ctx->has_host_cached_heap = vk_find_heap(ctx, ~0, api_heap_host_cached) != -1;
   ctx->has_image_tiling_linear = true;
   ctx->timestamp_period_in_seconds = device_properties.properties.limits.timestampPeriod * 0.000000001;
   ctx->max_mesh_workgroup_size = mesh.meshShader ? mesh_properties.maxMeshWorkGroupInvocations : 0;
   ctx->has_vrs = vrs.pipelineFragmentShadingRate;
   ctx->has_xfb = xfb.transformFeedback;
   ctx->has_clear_image_region = false;
   ctx->has_blit_image_3d = true;
   ctx->has_blit_image_msaa = false;
   ctx->has_resolve_image_yflip = false;
   ctx->supported_color_sample_counts = device_properties.properties.limits.framebufferColorSampleCounts;

   ctx->device_mem_usage = 0;

   /* Needed by mesh shaders. */
   shaderc_compile_options_set_target_spirv(ctx->glsl_compiler_options, shaderc_spirv_version_1_4);
   shaderc_compile_options_set_limit(ctx->glsl_compiler_options,
                                     shaderc_limit_max_mesh_work_group_size_x_ext,
                                     ctx->max_mesh_workgroup_size);
   shaderc_compile_options_set_limit(ctx->glsl_compiler_options,
                                     shaderc_limit_max_mesh_work_group_size_y_ext,
                                     ctx->max_mesh_workgroup_size);
   shaderc_compile_options_set_limit(ctx->glsl_compiler_options,
                                     shaderc_limit_max_mesh_work_group_size_z_ext,
                                     ctx->max_mesh_workgroup_size);

   ctx->destroy_context = NULL;

   ctx->create_buffer = vk_create_buffer;
   ctx->destroy_buffer = NULL;
   ctx->upload_buffer_data = vk_upload_buffer_data;
   ctx->clear_buffer = vk_clear_buffer;
   ctx->copy_buffer = vk_copy_buffer;

   ctx->create_image = vk_create_image;
   ctx->destroy_image = vk_destroy_image;
   ctx->clear_image = vk_clear_image;
   ctx->blit_image = vk_blit_image;
   ctx->image_write_png = vk_image_write_png;

   ctx->create_framebuffer = vk_create_framebuffer;
   ctx->destroy_framebuffer = vk_destroy_framebuffer;

   ctx->create_shader = vk_create_shader;
   ctx->destroy_shader = NULL;

   ctx->create_descriptor_set_layout = vk_create_descriptor_set_layout;
   ctx->destroy_descriptor_set_layout = NULL;

   ctx->create_descriptor_set = vk_create_descriptor_set;
   ctx->destroy_descriptor_set = NULL;
   ctx->set_uniform_buffer_descriptor = vk_set_uniform_buffer_descriptors;
   ctx->set_uniform_texel_buffer_descriptors = vk_set_uniform_texel_buffer_descriptors;
   ctx->set_storage_image_descriptors = vk_set_storage_image_descriptors;
   ctx->bind_descriptor_set = vk_bind_descriptor_set;

   ctx->create_pipeline = vk_create_pipeline;
   ctx->destroy_pipeline = vk_destroy_pipeline;
   ctx->bind_pipeline = vk_bind_pipeline;

   ctx->begin_cmdbuf = vk_begin_cmdbuf;
   ctx->end_cmdbuf_and_submit = vk_end_cmdbuf_and_submit;
   ctx->wait_idle_before_deallocation = vk_wait_idle_before_deallocation;

   ctx->begin_render_pass = vk_begin_render_pass;
   ctx->end_render_pass = vk_end_render_pass;

   ctx->bind_vertex_buffers = vk_bind_vertex_buffers;
   ctx->bind_index_buffer = vk_bind_index_buffer;
   ctx->draw = vk_draw;
   ctx->pipeline_barrier = vk_pipeline_barrier;

   ctx->create_timestamp_pool = vk_create_timestamp_pool;
   ctx->write_next_timestamp = vk_write_next_timestamp;
   ctx->query_timestamps = vk_query_timestamps;

   return ctx;
}
