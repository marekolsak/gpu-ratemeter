/* Copyright 2026 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 *
 * The initial code was based on vkcube:
 *    Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 *    Copyright (c) 2012 Rob Clark <rob@ti.com>
 *    Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <alloca.h>
#include <limits.h>
#include <stdatomic.h>
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
vk_has_validation_layer(api_context *ctx)
{
   if (ctx->options.no_validator)
      return false;

   uint32_t count;
   vk_check(vkEnumerateInstanceLayerProperties(&count, NULL));
   VkLayerProperties *props = alloca(sizeof(*props) * count);
   vk_check(vkEnumerateInstanceLayerProperties(&count, props));

   for (unsigned i = 0; i < count; i++) {
      if (!strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation"))
         return true;
   }

   fprintf(stderr, "Note: VK_LAYER_KHRONOS_validation not found.\n");
   return false;
}

static bool
has_extension(api_context *ctx, const char *name)
{
   for (unsigned i = 0; i < ctx->num_extensions; i++) {
      if (!strcmp(ctx->extensions[i].extensionName, name))
         return true;
   }

   return false;
}

static int
vk_find_heap_with_flags(api_context *ctx, unsigned supported_heap_mask,
                        VkMemoryPropertyFlags require_flags, VkMemoryPropertyFlags disallow_flags)
{
   for (unsigned i = 0; i < ctx->memory_properties.memoryTypeCount; i++) {
      if (!(supported_heap_mask & (1u << i)))
          continue;

      VkMemoryPropertyFlags flags = ctx->memory_properties.memoryTypes[i].propertyFlags;

      if (((flags & require_flags) == require_flags) && !(flags & disallow_flags))
         return i;
   }

   return -1;
}

#define chain_next(next, ptr) do { \
      *(next) = (ptr); \
      (next) = &(ptr)->pNext; \
   } while (0) \

static int
vk_find_heap(api_context *ctx, unsigned supported_heap_mask, api_heap_type heap)
{
   VkMemoryPropertyFlags require_flags = 0, disallow_flags = 0;
   assert(supported_heap_mask);

   if (heap == api_heap_host_uncached && !ctx->has_heap[heap])
      heap = api_heap_host_cached;

   if (heap == api_heap_host_cached && !ctx->has_heap[heap]) {
      require_flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      heap = api_heap_device;
   }

   switch (heap) {
   case api_heap_device:
      require_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      disallow_flags |= VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
      break;
   case api_heap_device_coherent_amd:
      require_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                       VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
      break;
   case api_heap_host_uncached:
      require_flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      disallow_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                        VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
      break;
   case api_heap_host_uncached_coherent_amd:
      require_flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
      disallow_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      break;
   case api_heap_host_cached:
      require_flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      disallow_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
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

static void
vk_wait_for_idle(api_context *ctx)
{
   for (unsigned i = 0; i < api_num_queues; i++) {
      if (!ctx->has_queue[i])
         continue;

      vk_check(vkWaitSemaphores(ctx->device,
                                &(VkSemaphoreWaitInfo){
                                   .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &ctx->semaphore[i],
                                   .pValues = &ctx->timeline_point[i],
                                }, UINT64_MAX));
   }
}

static api_buffer *
vk_create_buffer(api_context *ctx, uint64_t size, api_heap_type heap, unsigned sparse_block_size)
{
   api_buffer *buf = calloc(1, sizeof(api_buffer));
   buf->size = size;
   buf->heap = heap;
   buf->sparse_block_size = sparse_block_size;

   vk_check(vkCreateBuffer(ctx->device,
                           &(VkBufferCreateInfo) {
                              .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .flags = sparse_block_size ? VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                                           VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT : 0,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       (ctx->has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0) |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
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

   if (sparse_block_size) {
      assert(ctx->sparse_buffer_alignment <= sparse_block_size);
      buf->num_mem_allocations = (size + sparse_block_size - 1) / sparse_block_size;
   } else {
      buf->num_mem_allocations = 1;
   }

   buf->mem = calloc(buf->num_mem_allocations, sizeof(*buf->mem));

   for (unsigned i = 0; i < buf->num_mem_allocations; i++) {
      vk_check(vkAllocateMemory(ctx->device,
                                &(VkMemoryAllocateInfo) {
                                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = sparse_block_size ? sparse_block_size : reqs.size,
                                   .memoryTypeIndex = mem_type_index,
                                },
                                NULL, &buf->mem[i]));
   }

   if (sparse_block_size) {
      buf->sparse_binds = calloc(buf->num_mem_allocations, sizeof(*buf->sparse_binds));
      buf->sparse_unbinds = calloc(buf->num_mem_allocations, sizeof(*buf->sparse_unbinds));

      for (unsigned i = 0; i < buf->num_mem_allocations; i++) {
         buf->sparse_binds[i].resourceOffset = i * sparse_block_size;
         buf->sparse_binds[i].size = sparse_block_size;
         buf->sparse_binds[i].memory = buf->mem[i];

         buf->sparse_unbinds[i].resourceOffset = i * sparse_block_size;
         buf->sparse_unbinds[i].size = sparse_block_size;
      }
   } else {
      vk_check(vkBindBufferMemory(ctx->device, buf->buffer, *buf->mem, 0));

      if (heap == api_heap_device)
         atomic_fetch_add(&ctx->device_mem_usage_mb, reqs.size >> 20);
   }

   return buf;
}

static void
vk_destroy_buffer(api_context *ctx, api_buffer *buf)
{
   vkDestroyBuffer(ctx->device, buf->buffer, NULL);

   for (unsigned i = 0; i < buf->num_mem_allocations; i++)
      vkFreeMemory(ctx->device, buf->mem[i], NULL);

   if (buf->heap == api_heap_device)
      atomic_fetch_sub(&ctx->device_mem_usage_mb, buf->size >> 20);

   free(buf->sparse_binds);
   free(buf->sparse_unbinds);
   free(buf);
}

static void
vk_pipeline_barrier_buffers(api_context *ctx, unsigned num_buffers,
                            api_buffer **buffers, uint64_t *offsets, uint64_t *sizes)
{
   VkBufferMemoryBarrier2 *barriers = alloca(sizeof(barriers[0]) * num_buffers);

   for (unsigned i = 0; i < num_buffers; i++) {
      VkAccessFlags2 access = VK_ACCESS_2_TRANSFER_READ_BIT |
                              VK_ACCESS_2_TRANSFER_WRITE_BIT;

      if (ctx->current_queue == api_queue_gfx || ctx->current_queue == api_queue_compute) {
         access |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                   VK_ACCESS_2_UNIFORM_READ_BIT |
                   VK_ACCESS_2_SHADER_READ_BIT |
                   VK_ACCESS_2_SHADER_WRITE_BIT |
                   VK_ACCESS_2_MEMORY_READ_BIT |
                   VK_ACCESS_2_MEMORY_WRITE_BIT |
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      }

      if (ctx->current_queue == api_queue_gfx) {
         access |= VK_ACCESS_2_INDEX_READ_BIT |
                   VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                   (ctx->has_xfb ? VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT : 0);
      }

      barriers[i] = (VkBufferMemoryBarrier2){
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = access,
                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dstAccessMask = access,
                    .buffer = buffers[i]->buffer,
                    .offset = offsets[i],
                    .size = sizes[i],
      };
   }

   vkCmdPipelineBarrier2(ctx->current_cmd_buffer, &(VkDependencyInfo) {
                            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .bufferMemoryBarrierCount = num_buffers,
                            .pBufferMemoryBarriers = barriers,
                         });
}

static void
vk_clear_buffer(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size, uint32_t value)
{
   vkCmdFillBuffer(ctx->current_cmd_buffer, buf->buffer, offset, size, value);
   vk_pipeline_barrier_buffers(ctx, 1, &buf, &offset, &size);
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
   vk_pipeline_barrier_buffers(ctx, 2, (api_buffer*[2]){src, dst},
                               (uint64_t[2]){src_offset, dst_offset}, (uint64_t[2]){size, size});
}

static api_fence *
get_latest_fence(api_context *ctx, api_queue_type queue)
{
   api_fence *fence = calloc(1, sizeof(api_fence));

   fence->semaphore = ctx->semaphore[queue];
   fence->timeline_point = ctx->timeline_point[queue];
   return fence;
}

static void
vk_buffer_bind_sparse(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      bool bind, api_queue_type queue, api_fence *wait_fence,
                      api_fence **signal_fence)
{
   assert(ctx->has_queue[queue]);
   assert(offset % buf->sparse_block_size == 0);
   assert(size % buf->sparse_block_size == 0);

   uint64_t first_block = offset / buf->sparse_block_size;
   uint64_t num_blocks = size / buf->sparse_block_size;

   assert(ctx->timeline_point[queue] != UINT64_MAX);
   ctx->timeline_point[queue]++;

   vk_check(vkQueueBindSparse(ctx->queue[queue], 1,
                              &(VkBindSparseInfo){
                                 .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                                 .waitSemaphoreCount = !!wait_fence,
                                 .pWaitSemaphores = wait_fence ? &wait_fence->semaphore : NULL,
                                 .bufferBindCount = 1,
                                 .pBufferBinds = &(VkSparseBufferMemoryBindInfo){
                                    .buffer = buf->buffer,
                                    .bindCount = num_blocks,
                                    .pBinds = bind ? &buf->sparse_binds[first_block]
                                                   : &buf->sparse_unbinds[first_block],
                                 },
                                 .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &ctx->semaphore[queue],
                                 .pNext = &(VkTimelineSemaphoreSubmitInfo){
                                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                    .waitSemaphoreValueCount = !!wait_fence,
                                    .pWaitSemaphoreValues = wait_fence ? &wait_fence->timeline_point : NULL,
                                    .signalSemaphoreValueCount = 1,
                                    .pSignalSemaphoreValues = &ctx->timeline_point[queue],
                                 },
                              }, NULL));

   if (signal_fence)
      *signal_fence = get_latest_fence(ctx, queue);
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
   image->heap = heap;

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
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         ((samples == 1) ? VK_IMAGE_USAGE_STORAGE_BIT : 0) ,
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
      atomic_fetch_add(&ctx->device_mem_usage_mb, image->mem_size >> 20);
   return image;
}

static void
vk_destroy_image(api_context *ctx, api_image *image)
{
   vkDestroyImageView(ctx->device, image->render_compatible_view, NULL);
   vkDestroyImage(ctx->device, image->image, NULL);
   vkFreeMemory(ctx->device, image->mem, NULL);

   if (image->heap == api_heap_device)
      atomic_fetch_sub(&ctx->device_mem_usage_mb, image->mem_size >> 20);

   free(image);
}

static void
vk_clear_image(api_context *ctx, api_image *image, const api_image_box *box,
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
vk_blit_image(api_context *ctx, api_blit_desc *desc)
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
                      unsigned width, unsigned height, unsigned samples, unsigned view_mask)
{
   api_framebuffer *fb = calloc(1, sizeof(api_framebuffer));
   fb->width = width;
   fb->height = height;
   fb->samples = samples;
   fb->view_mask = view_mask;
   fb->colorbuf = colorbuf;
   fb->zbuf = zbuf;

   assert(view_mask);
   assert(ctx->has_multiview || view_mask == 0x1);

   if (!(ctx->options.api_flags & API_VK_DYNAMIC_STATE)) {
      fb->colorbuf_att_index = 0;
      fb->zbuf_att_index = 0;
      fb->num_attachments = 0;

      VkAttachmentDescription2 att_descs[2];
      VkImageView att_views[2];

      if (colorbuf) {
         assert(!format_is_depth_or_stencil(colorbuf->format));
         assert(fb->num_attachments < ARRAY_SIZE(att_descs));

         att_descs[fb->num_attachments] = (VkAttachmentDescription2){
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = colorbuf->format,
            .samples = colorbuf->samples,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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

         att_descs[fb->num_attachments] = (VkAttachmentDescription2){
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = zbuf->format,
            .samples = zbuf->samples,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
         };

         att_views[fb->num_attachments] = zbuf->render_compatible_view;
         fb->zbuf_att_index = fb->num_attachments;
         fb->num_attachments++;
      }

      for (unsigned clear = 0; clear < 2; clear++) {
         for (unsigned i = 0; i < fb->num_attachments; i++)
            att_descs[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;

         bool multiview = view_mask != 0x1;

         VkRenderPassCreateInfo2 rp_create_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
            .attachmentCount = fb->num_attachments,
            .pAttachments = att_descs,
            .subpassCount = 1,
            .pSubpasses = (VkSubpassDescription2[]) {
               {
                  .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
                  .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                  .viewMask = multiview ? view_mask : 0,
                  .colorAttachmentCount = colorbuf ? 1 : 0,
                  .pColorAttachments = (VkAttachmentReference2[]) {
                     {
                        .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                        .attachment = colorbuf ? fb->colorbuf_att_index : VK_ATTACHMENT_UNUSED,
                        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                     },
                  },
                  .pDepthStencilAttachment = &(VkAttachmentReference2) {
                     .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                     .attachment = zbuf ? fb->zbuf_att_index : VK_ATTACHMENT_UNUSED,
                     .layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                  },
               },
            },
            .correlatedViewMaskCount = multiview ? 1 : 0,
            .pCorrelatedViewMasks = &view_mask,
         };

         vk_check(vkCreateRenderPass2(ctx->device, &rp_create_info, NULL, &fb->render_pass[clear]));
         vk_check(vkCreateFramebuffer(ctx->device,
                                      &(VkFramebufferCreateInfo) {
                                         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                         .renderPass = fb->render_pass[clear],
                                         .attachmentCount = fb->num_attachments,
                                         .pAttachments = att_views,
                                         .width = fb->width,
                                         .height = fb->height,
                                         .layers = multiview ? 1 :
                                                      colorbuf ? colorbuf->depth :
                                                      zbuf ? zbuf->depth : 1,
                                      },
                                      NULL, &fb->fb[clear]));
      }
   }

   return fb;
}

static void
vk_destroy_framebuffer(api_context *ctx, api_framebuffer *fb)
{
   if (!(ctx->options.api_flags & API_VK_DYNAMIC_STATE)) {
      for (unsigned clear = 0; clear < 2; clear++) {
         vkDestroyFramebuffer(ctx->device, fb->fb[clear], NULL);
         vkDestroyRenderPass(ctx->device, fb->render_pass[clear], NULL);
      }
   }
   free(fb);
}

static api_shader *
vk_create_shader(api_context *ctx, const char *source, api_shader_type type)
{
   shaderc_shader_kind shaderc_type;
   VkShaderStageFlagBits stage_bit;

   switch (type) {
   case api_shader_vs:
      shaderc_type = shaderc_glsl_vertex_shader;
      stage_bit = VK_SHADER_STAGE_VERTEX_BIT;
      break;
   case api_shader_tcs:
      shaderc_type = shaderc_glsl_tess_control_shader;
      stage_bit = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
      break;
   case api_shader_tes:
      shaderc_type = shaderc_glsl_tess_evaluation_shader;
      stage_bit = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
      break;
   case api_shader_gs:
      shaderc_type = shaderc_glsl_geometry_shader;
      stage_bit = VK_SHADER_STAGE_GEOMETRY_BIT;
      break;
   case api_shader_fs:
      shaderc_type = shaderc_glsl_fragment_shader;
      stage_bit = VK_SHADER_STAGE_FRAGMENT_BIT;
      break;
   case api_shader_cs:
      shaderc_type = shaderc_glsl_compute_shader;
      stage_bit = VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   case api_shader_ts:
      shaderc_type = shaderc_glsl_task_shader;
      stage_bit = VK_SHADER_STAGE_TASK_BIT_EXT;
      break;
   case api_shader_ms:
      shaderc_type = shaderc_glsl_mesh_shader;
      stage_bit = VK_SHADER_STAGE_MESH_BIT_EXT;
      break;
   default:
      error("invalid shader type");
   }

   api_shader *shader = calloc(1, sizeof(api_shader));

   shader->spirv =
       shaderc_compile_into_spv(ctx->glsl_compiler, source, strlen(source),
                                shaderc_type, "file", "main", ctx->glsl_compiler_options);
   if (!shader->spirv)
      error("shaderc_compile_into_spv returned NULL");

   if (shaderc_result_get_compilation_status(shader->spirv) != shaderc_compilation_status_success) {
      error("failed to compile shader to SPIR-V:\n%s\n\n%s", source,
            shaderc_result_get_error_message(shader->spirv));
   }

   shader->module_info = (VkShaderModuleCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shaderc_result_get_length(shader->spirv),
      .pCode = (uint32_t*)shaderc_result_get_bytes(shader->spirv),
   };

   shader->stage_info = (VkPipelineShaderStageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = &shader->module_info,
      .stage = stage_bit,
      .pName = "main",
   };

   return shader;
}

static void
vk_destroy_shader(api_context *ctx, api_shader *shader)
{
   shaderc_result_release(shader->spirv);
   free(shader);
}

static api_descriptor_set_layout *
vk_create_descriptor_set_layout(api_context *ctx,
                                const api_descriptor_set_layout_desc *desc)
{
   api_descriptor_set_layout *layout = calloc(1, sizeof(api_descriptor_set_layout));
   layout->desc = *desc;

   VkShaderStageFlags stage_flags = (ctx->max_mesh_workgroup_size ? VK_SHADER_STAGE_MESH_BIT_EXT : 0) |
                                    VK_SHADER_STAGE_FRAGMENT_BIT |
                                    VK_SHADER_STAGE_COMPUTE_BIT;
   unsigned num_bindings = 0;
   VkDescriptorSetLayoutBinding desc_set_layout_bindings[8];

   for (unsigned i = 0; i < MAX_UNIFORM_BUFFER_BINDINGS; i++) {
      if (desc->uniform_buffer[i].array_size) {
         assert(desc->uniform_buffer[i].array_size <= MAX_UNIFORM_BUFFER_ARRAY_SIZE);
         assert(num_bindings < ARRAY_SIZE(desc_set_layout_bindings));

         desc_set_layout_bindings[num_bindings++] = (VkDescriptorSetLayoutBinding){
            .binding = desc->uniform_buffer[i].vk_binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = desc->uniform_buffer[i].array_size,
            .stageFlags = stage_flags,
         };
      }
   }

   for (unsigned i = 0; i < MAX_STORAGE_BUFFER_BINDINGS; i++) {
      if (desc->storage_buffer[i].array_size) {
         assert(desc->storage_buffer[i].array_size <= MAX_STORAGE_BUFFER_ARRAY_SIZE);
         assert(num_bindings < ARRAY_SIZE(desc_set_layout_bindings));

         desc_set_layout_bindings[num_bindings++] = (VkDescriptorSetLayoutBinding){
            .binding = desc->storage_buffer[i].vk_binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = desc->storage_buffer[i].array_size,
            .stageFlags = stage_flags,
         };
      }
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

   for (unsigned i = 0; i < MAX_STORAGE_IMAGE_BINDINGS; i++) {
      if (desc->storage_image[i].array_size) {
         assert(desc->storage_image[i].array_size <= MAX_STORAGE_IMAGE_ARRAY_SIZE);
         assert(num_bindings < ARRAY_SIZE(desc_set_layout_bindings));

         desc_set_layout_bindings[num_bindings++] = (VkDescriptorSetLayoutBinding){
            .binding = desc->storage_image[i].vk_binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = desc->storage_image[i].array_size,
            .stageFlags = stage_flags,
         };
      }
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
vk_set_uniform_buffer_descriptors(api_context *ctx, api_descriptor_set *set, unsigned binding_index,
                                  api_buffer *buffer, uint64_t offset, uint64_t size)
{
   assert(1 <= set->layout->desc.uniform_buffer[binding_index].array_size);

   vkUpdateDescriptorSets(ctx->device, 1,
                          &(VkWriteDescriptorSet){
                             .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set->set,
                             .dstBinding = set->layout->desc.uniform_buffer[binding_index].vk_binding,
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
vk_set_storage_buffer_descriptors(api_context *ctx, api_descriptor_set *set, unsigned binding_index,
                                  api_buffer *buffer, uint64_t offset, uint64_t size)
{
   assert(1 <= set->layout->desc.storage_buffer[binding_index].array_size);

   vkUpdateDescriptorSets(ctx->device, 1,
                          &(VkWriteDescriptorSet){
                             .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set->set,
                             .dstBinding = set->layout->desc.storage_buffer[binding_index].vk_binding,
                             .dstArrayElement = 0,
                             .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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
vk_set_storage_image_descriptors(api_context *ctx, api_descriptor_set *set, unsigned binding_index,
                                 unsigned num_images, api_image **images)
{
   assert(num_images <= set->layout->desc.storage_image[binding_index].array_size);
   VkDescriptorImageInfo *image_infos = alloca(sizeof(*image_infos) * num_images);

   for (unsigned i = 0; i < num_images; i++) {
      if (images[i]->layout != VK_IMAGE_LAYOUT_GENERAL) {
         ctx->begin_cmdbuf(ctx, api_queue_gfx);
         vk_image_layout_transition(ctx, images[i], VK_IMAGE_LAYOUT_GENERAL);
         ctx->end_cmdbuf_and_submit(ctx, api_wait_all_queues, NULL, NULL);
      }

      image_infos[i].sampler = NULL;
      image_infos[i].imageView = images[i]->render_compatible_view;
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   }

   vkUpdateDescriptorSets(ctx->device, 1,
                          &(VkWriteDescriptorSet){
                             .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set->set,
                             .dstBinding = set->layout->desc.storage_image[binding_index].vk_binding,
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
   vkCmdBindDescriptorSets(ctx->current_cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
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
   VkPipelineShaderStageCreateInfo stages[5], prerast_stages[4], fs_stages[1];
   unsigned num_stages = 0, num_prerast_stages = 0, num_fs_stages = 0;

   if (desc->ms)
      stages[num_stages++] = prerast_stages[num_prerast_stages++] = desc->ms->stage_info;
   if (desc->vs)
      stages[num_stages++] = prerast_stages[num_prerast_stages++] = desc->vs->stage_info;

   if (desc->fs && !desc->rasterizer_discard)
      stages[num_stages++] = fs_stages[num_fs_stages++] = desc->fs->stage_info;

   bool uses_dynamic_state = ctx->options.api_flags & API_VK_DYNAMIC_STATE;
   VkDynamicState dyn_states_vi[10], dyn_states_prerast[10], dyn_states_fs[10], dyn_states_out[10];
   unsigned num_dyn_states_vi = 0, num_dyn_states_prerast = 0, num_dyn_states_fs = 0;
   unsigned num_dyn_states_out = 0;

#define check_incr(array) ((void)assert(num_##array < ARRAY_SIZE(array)), num_##array++)

   dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT;
   dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;

   if (uses_dynamic_state) {
      if (!desc->ms) {
         dyn_states_vi[check_incr(dyn_states_vi)] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
         dyn_states_vi[check_incr(dyn_states_vi)] = VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE;
         dyn_states_vi[check_incr(dyn_states_vi)] = VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
      }

      dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;
      dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_CULL_MODE;
      dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
      dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_FRONT_FACE;
      if (ctx->has_vrs)
         dyn_states_prerast[check_incr(dyn_states_prerast)] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;

      dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT;
      dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
      dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
      dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
      dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
      dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
      if (ctx->has_vrs && ctx->options.api_flags & API_VK_GPL)
         dyn_states_fs[check_incr(dyn_states_fs)] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;

      dyn_states_out[check_incr(dyn_states_out)] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
      dyn_states_out[check_incr(dyn_states_out)] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
      dyn_states_out[check_incr(dyn_states_out)] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;

   }

   VkDynamicState dyn_states[20];
   unsigned num_dyn_states = 0;

   for (unsigned i = 0; i < num_dyn_states_vi; i++)
      dyn_states[check_incr(dyn_states)] = dyn_states_vi[i];
   for (unsigned i = 0; i < num_dyn_states_prerast; i++)
      dyn_states[check_incr(dyn_states)] = dyn_states_prerast[i];
   for (unsigned i = 0; i < num_dyn_states_fs; i++)
      dyn_states[check_incr(dyn_states)] = dyn_states_fs[i];
   for (unsigned i = 0; i < num_dyn_states_out; i++)
      dyn_states[check_incr(dyn_states)] = dyn_states_out[i];

#undef check_incr

   pipeline->blend_state = (VkPipelineColorBlendAttachmentState) {
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
   };

   /* Create the graphics pipeline. */
   VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = num_stages,
      .pStages = stages,
      .pVertexInputState = uses_dynamic_state ? NULL : &vertex_input_state,
      .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = desc->topology, /* the topology class must still match dynamic state */
         .primitiveRestartEnable = uses_dynamic_state ? false : desc->primitive_restart,
      },
      .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .rasterizerDiscardEnable = uses_dynamic_state ? false : desc->rasterizer_discard,
         .polygonMode = uses_dynamic_state ? false : desc->polygon_mode,
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
         .depthTestEnable = uses_dynamic_state ? false : desc->depth_enabled,
         .depthWriteEnable = uses_dynamic_state ? false : desc->depth_write_enabled,
         .depthCompareOp = uses_dynamic_state ? 0 : desc->depth_compare_op,
      },
      .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = desc->fb->colorbuf ? 1 : 0,
         .pAttachments = uses_dynamic_state ?
            (VkPipelineColorBlendAttachmentState[1]){} : &pipeline->blend_state,
      },
      .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
         .dynamicStateCount = num_dyn_states,
         .pDynamicStates = dyn_states,
      },
      .layout = desc->desc_set_layout ? desc->desc_set_layout->pipeline_layout :
                                        ctx->empty_pipeline_layout,
      .renderPass = uses_dynamic_state ? NULL : desc->fb->render_pass[0],
   };

   bool multiview = desc->fb->view_mask != 0x1;
   VkPipelineRenderingCreateInfo dyn_rendering_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .viewMask = multiview ? desc->fb->view_mask : 0,
      .colorAttachmentCount = desc->fb->colorbuf ? 1 : 0,
      .pColorAttachmentFormats = desc->fb->colorbuf ? &desc->fb->colorbuf->format : NULL,
      .depthAttachmentFormat = desc->fb->zbuf ? desc->fb->zbuf->format : VK_FORMAT_UNDEFINED,
      .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
   };

   assert(desc->vrs_fragment_size[0] && desc->vrs_fragment_size[1]);
   assert(ctx->has_vrs || (desc->vrs_fragment_size[0] == 1 && desc->vrs_fragment_size[1] == 1));

   pipeline->vrs = (VkPipelineFragmentShadingRateStateCreateInfoKHR){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
      .fragmentSize = {desc->vrs_fragment_size[0], desc->vrs_fragment_size[1]},
      .combinerOps = {
         VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
         VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
      },
   };

   if (ctx->options.api_flags & API_VK_GPL) {
      VkGraphicsPipelineCreateInfo info_vi = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &(VkGraphicsPipelineLibraryCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
         },
         .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
         .pVertexInputState = info.pVertexInputState,
         .pInputAssemblyState = info.pInputAssemblyState,
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = num_dyn_states_vi,
            .pDynamicStates = dyn_states_vi,
         },
      };

      VkGraphicsPipelineCreateInfo info_prerast = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &(VkGraphicsPipelineLibraryCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
         },
         .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
         .stageCount = num_prerast_stages,
         .pStages = prerast_stages,
         .pRasterizationState = info.pRasterizationState,
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = num_dyn_states_prerast,
            .pDynamicStates = dyn_states_prerast,
         },
         .layout = info.layout,
         .renderPass = info.renderPass,
      };
      const void **pNext_prerast = &((VkGraphicsPipelineLibraryCreateInfoEXT*)info_prerast.pNext)->pNext;

      if (uses_dynamic_state)
         chain_next(pNext_prerast, &dyn_rendering_pipeline_info);

      if (!uses_dynamic_state && ctx->has_vrs)
         chain_next(pNext_prerast, &pipeline->vrs);

      VkGraphicsPipelineCreateInfo info_fs = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &(VkGraphicsPipelineLibraryCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
         },
         .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
         .stageCount = num_fs_stages,
         .pStages = fs_stages,
         .pMultisampleState = info.pMultisampleState,
         .pDepthStencilState = info.pDepthStencilState,
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = num_dyn_states_fs,
            .pDynamicStates = dyn_states_fs,
         },
         .layout = info.layout,
         .renderPass = info.renderPass,
      };
      const void **pNext_fs = &((VkGraphicsPipelineLibraryCreateInfoEXT*)info_fs.pNext)->pNext;

      if (uses_dynamic_state)
         chain_next(pNext_fs, &dyn_rendering_pipeline_info);

      if (!uses_dynamic_state && ctx->has_vrs)
         chain_next(pNext_fs, &pipeline->vrs);

      VkGraphicsPipelineCreateInfo info_out = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &(VkGraphicsPipelineLibraryCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
         },
         .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
         .pColorBlendState = info.pColorBlendState,
         .pMultisampleState = info.pMultisampleState,
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = num_dyn_states_out,
            .pDynamicStates = dyn_states_out,
         },
         .renderPass = info.renderPass,
      };

      const void **pNext_out = &((VkGraphicsPipelineLibraryCreateInfoEXT*)info_out.pNext)->pNext;

      if (uses_dynamic_state)
         chain_next(pNext_out, &dyn_rendering_pipeline_info);

      if (!desc->ms)
         vk_check(vkCreateGraphicsPipelines(ctx->device, NULL, 1, &info_vi, NULL, &pipeline->lib_vi));
      vk_check(vkCreateGraphicsPipelines(ctx->device, NULL, 1, &info_prerast, NULL, &pipeline->lib_prerast));
      vk_check(vkCreateGraphicsPipelines(ctx->device, NULL, 1, &info_fs, NULL, &pipeline->lib_fs));
      vk_check(vkCreateGraphicsPipelines(ctx->device, NULL, 1, &info_out, NULL, &pipeline->lib_out));

      VkGraphicsPipelineCreateInfo linked = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .pNext = &(VkPipelineLibraryCreateInfoKHR){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
            .libraryCount = desc->ms ? 3 : 4,
            .pLibraries = (VkPipeline[]){pipeline->lib_prerast, pipeline->lib_fs, pipeline->lib_out,
                                         pipeline->lib_vi},
         },
         .layout = info.layout,
      };

      vk_check(vkCreateGraphicsPipelines(ctx->device, NULL, 1, &linked, NULL, &pipeline->pipeline));
   } else {
      /* Chain structures. */
      const void **pNext = &info.pNext;

      if (uses_dynamic_state)
         chain_next(pNext, &dyn_rendering_pipeline_info);

      if (!uses_dynamic_state && ctx->has_vrs)
         chain_next(pNext, &pipeline->vrs);

      vk_check(vkCreateGraphicsPipelines(ctx->device, NULL, 1, &info, NULL,
                                         &pipeline->pipeline));
   }

   assert(pipeline);
   return pipeline;
}

static void
vk_destroy_pipeline(api_context *ctx, api_pipeline *pipeline)
{
   if (ctx->options.api_flags & API_VK_GPL) {
      if (pipeline->lib_vi)
         vkDestroyPipeline(ctx->device, pipeline->lib_vi, NULL);
      vkDestroyPipeline(ctx->device, pipeline->lib_prerast, NULL);
      vkDestroyPipeline(ctx->device, pipeline->lib_fs, NULL);
      vkDestroyPipeline(ctx->device, pipeline->lib_out, NULL);
   }

   vkDestroyPipeline(ctx->device, pipeline->pipeline, NULL);
   free(pipeline);
}

static void
vk_bind_pipeline(api_context *ctx, api_pipeline *pipeline)
{
   assert(pipeline);
   ctx->current_pipeline = pipeline;
   vkCmdBindPipeline(ctx->current_cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

   if (ctx->options.api_flags & API_VK_DYNAMIC_STATE) {
      if (!pipeline->desc.ms) {
         ctx->vkCmdSetVertexInputEXT(ctx->current_cmd_buffer,
                                     pipeline->num_vb_desc, pipeline->dyn_vi_bindings,
                                     pipeline->num_vb_desc, pipeline->dyn_vi_attribs);
         vkCmdSetPrimitiveTopology(ctx->current_cmd_buffer, pipeline->desc.topology);
         vkCmdSetPrimitiveRestartEnable(ctx->current_cmd_buffer, pipeline->desc.primitive_restart);
      }

      vkCmdSetRasterizerDiscardEnable(ctx->current_cmd_buffer, pipeline->desc.rasterizer_discard);
      ctx->vkCmdSetPolygonModeEXT(ctx->current_cmd_buffer, pipeline->desc.polygon_mode);
      vkCmdSetCullMode(ctx->current_cmd_buffer, pipeline->desc.cull_mode);
      vkCmdSetFrontFace(ctx->current_cmd_buffer, VK_FRONT_FACE_CLOCKWISE);

      ctx->vkCmdSetRasterizationSamplesEXT(ctx->current_cmd_buffer, pipeline->desc.fb->samples);
      ctx->vkCmdSetSampleMaskEXT(ctx->current_cmd_buffer, pipeline->desc.fb->samples, &pipeline->desc.samplemask);
      ctx->vkCmdSetAlphaToCoverageEnableEXT(ctx->current_cmd_buffer, pipeline->desc.alpha_to_coverage);

      vkCmdSetDepthTestEnable(ctx->current_cmd_buffer, pipeline->desc.depth_enabled);

      if (pipeline->desc.fb->zbuf) {
         vkCmdSetDepthWriteEnable(ctx->current_cmd_buffer, pipeline->desc.depth_write_enabled);
         vkCmdSetDepthCompareOp(ctx->current_cmd_buffer, pipeline->desc.depth_compare_op);
      }

      if (pipeline->desc.fb->colorbuf) {
         ctx->vkCmdSetColorBlendEnableEXT(ctx->current_cmd_buffer, 0, 1, &pipeline->blend_state.blendEnable);
         ctx->vkCmdSetColorBlendEquationEXT(ctx->current_cmd_buffer, 0, 1, &(VkColorBlendEquationEXT){
                                               .srcColorBlendFactor = pipeline->blend_state.srcColorBlendFactor,
                                               .dstColorBlendFactor = pipeline->blend_state.dstColorBlendFactor,
                                               .colorBlendOp = pipeline->blend_state.colorBlendOp,
                                               .srcAlphaBlendFactor = pipeline->blend_state.srcAlphaBlendFactor,
                                               .dstAlphaBlendFactor = pipeline->blend_state.dstAlphaBlendFactor,
                                               .alphaBlendOp = pipeline->blend_state.alphaBlendOp,
                                            });
         ctx->vkCmdSetColorWriteMaskEXT(ctx->current_cmd_buffer, 0, 1, &pipeline->blend_state.colorWriteMask);
      }

      if (ctx->has_vrs) {
         ctx->vkCmdSetFragmentShadingRateKHR(ctx->current_cmd_buffer, &pipeline->vrs.fragmentSize,
                                             pipeline->vrs.combinerOps);
      }
   }
}

static api_compute_pipeline *
vk_create_compute_pipeline(api_context *ctx, api_shader *shader,
                           api_descriptor_set_layout *layout)
{
   api_compute_pipeline *pipeline = calloc(1, sizeof(api_compute_pipeline));

   vk_check(vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1,
                                     &(VkComputePipelineCreateInfo){
                                        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage  = shader->stage_info,
                                        .layout = layout->pipeline_layout,
                                     },
                                     NULL, &pipeline->pipeline));
   return pipeline;
}

static void
vk_destroy_compute_pipeline(api_context *ctx, api_compute_pipeline *pipeline)
{
   vkDestroyPipeline(ctx->device, pipeline->pipeline, NULL);
   free(pipeline);
}

static void
vk_bind_compute_pipeline(api_context *ctx, api_compute_pipeline *pipeline)
{
   vkCmdBindPipeline(ctx->current_cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
}

static void
vk_dispatch(api_context *ctx, unsigned num_x, unsigned num_y, unsigned num_z)
{
   vkCmdDispatch(ctx->current_cmd_buffer, num_x, num_y, num_z);
}

static void
vk_pipeline_barrier_buffer(struct api_context *ctx, api_buffer *buf)
{
   vk_pipeline_barrier_buffers(ctx, 1, &buf, (uint64_t[1]){0}, (uint64_t[1]){buf->size});
}

static void
vk_begin_cmdbuf(api_context *ctx, api_queue_type queue)
{
   assert(ctx->current_cmd_buffer == NULL);

   if (ctx->timeline_point[queue] >= MAX_COMMAND_BUFFERS - 1) {
      uint64_t wait_point = ctx->timeline_point[queue] - 511;

      vk_check(vkWaitSemaphores(ctx->device,
                                &(VkSemaphoreWaitInfo){
                                   .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &ctx->semaphore[queue],
                                   .pValues = &wait_point,
                                }, UINT64_MAX));
   }

   ctx->current_queue = queue;
   ctx->current_cmd_buffer = ctx->cmd_buffers[queue][ctx->timeline_point[queue] % MAX_COMMAND_BUFFERS];
   vk_check(vkBeginCommandBuffer(ctx->current_cmd_buffer,
                                 &(VkCommandBufferBeginInfo) {
                                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 }));
}

static void
vk_end_cmdbuf_and_submit(api_context *ctx, unsigned wait_queue_mask, api_fence *wait_fence,
                         api_fence **signal_fence)
{
   api_queue_type queue = ctx->current_queue;

   vk_check(vkEndCommandBuffer(ctx->current_cmd_buffer));

   /* Gather wait semaphores. */
   unsigned num_wait_semaphores = 0;
   VkSemaphoreSubmitInfo wait_semaphores[api_num_queues + 1];

   for (unsigned i = 0; i < api_num_queues; i++) {
      if (ctx->has_queue[i] && i != queue && (wait_queue_mask & (1 << i))) {
         wait_semaphores[num_wait_semaphores++] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = ctx->semaphore[i],
            .value = ctx->timeline_point[i],
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         };
      }
   }

   if (wait_fence) {
      wait_semaphores[num_wait_semaphores++] = (VkSemaphoreSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = wait_fence ? wait_fence->semaphore : NULL,
         .value = wait_fence ? wait_fence->timeline_point : 0,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      };
   }

   /* Submit. */
   assert(ctx->timeline_point[queue] != UINT64_MAX);
   ctx->timeline_point[queue]++;

   vk_check(vkQueueSubmit2(ctx->queue[queue], 1,
      &(VkSubmitInfo2) {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
         .waitSemaphoreInfoCount = num_wait_semaphores,
         .pWaitSemaphoreInfos = wait_semaphores,
         .commandBufferInfoCount = 1,
         .pCommandBufferInfos = &(VkCommandBufferSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = ctx->current_cmd_buffer,
         },
         .signalSemaphoreInfoCount = 1,
         .pSignalSemaphoreInfos = &(VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = ctx->semaphore[queue],
            .value = ctx->timeline_point[queue],
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         },
      }, NULL));

   ctx->current_cmd_buffer = NULL;
   ctx->current_pipeline = NULL;

   if (signal_fence)
      *signal_fence = get_latest_fence(ctx, queue);
}

static void
vk_begin_render_pass(api_context *ctx, const api_render_pass_desc *desc)
{
   if (desc->fb->colorbuf) {
      vk_image_layout_transition(ctx, desc->fb->colorbuf,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
   }
   if (desc->fb->zbuf) {
      vk_image_layout_transition(ctx, desc->fb->zbuf,
                                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
   }

   if (ctx->options.api_flags & API_VK_DYNAMIC_STATE) {
      bool multiview = desc->fb->view_mask != 0x1;

      vkCmdBeginRendering(ctx->current_cmd_buffer,
                          &(VkRenderingInfo){
                             .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                             .renderArea = {
                                .offset = {0, 0},
                                .extent = {desc->fb->width, desc->fb->height},
                             },
                             .layerCount = multiview ? 1 :
                                             desc->fb->colorbuf ? desc->fb->colorbuf->depth :
                                             desc->fb->zbuf ? desc->fb->zbuf->depth : 1,
                             .viewMask = multiview ? desc->fb->view_mask : 0,
                             .colorAttachmentCount = desc->fb->colorbuf ? 1 : 0,
                             .pColorAttachments = &(VkRenderingAttachmentInfo){
                                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                .imageView = desc->fb->colorbuf ?
                                                desc->fb->colorbuf->render_compatible_view : NULL,
                                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                .loadOp = desc->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                        VK_ATTACHMENT_LOAD_OP_LOAD,
                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                .clearValue = {
                                    .color = desc->color_clear_value,
                                },
                             },
                             .pDepthAttachment = &(VkRenderingAttachmentInfo){
                                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                .imageView = desc->fb->zbuf ?
                                                desc->fb->zbuf->render_compatible_view : NULL,
                                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                .loadOp = desc->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                        VK_ATTACHMENT_LOAD_OP_LOAD,
                                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                .clearValue = {
                                    .depthStencil = {desc->depth_clear_value, 0},
                                },
                             },
                          });
   } else {
      VkClearValue *clear_values = alloca(desc->fb->num_attachments * sizeof(VkClearValue));

      if (desc->fb->colorbuf)
         clear_values[desc->fb->colorbuf_att_index] = (VkClearValue){.color = desc->color_clear_value};
      if (desc->fb->zbuf)
         clear_values[desc->fb->zbuf_att_index] = (VkClearValue){.depthStencil.depth = desc->depth_clear_value};

      vkCmdBeginRenderPass(ctx->current_cmd_buffer,
                           &(VkRenderPassBeginInfo) {
                              .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                              .renderPass = desc->fb->render_pass[desc->clear],
                              .framebuffer = desc->fb->fb[desc->clear],
                              .renderArea = { { 0, 0 }, { desc->fb->width, desc->fb->height } },
                              .clearValueCount = desc->fb->num_attachments,
                              .pClearValues = clear_values,
                           },
                           VK_SUBPASS_CONTENTS_INLINE);
   }

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
   if (ctx->options.api_flags & API_VK_DYNAMIC_STATE)
      vkCmdEndRendering(ctx->current_cmd_buffer);
   else
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
   assert(ctx->current_pipeline);

   if (desc->mesh_shader)
      ctx->vkCmdDrawMeshTasksEXT(ctx->current_cmd_buffer, desc->count, 1, 1);
   else if (desc->indexed)
      vkCmdDrawIndexed(ctx->current_cmd_buffer, desc->count, desc->instance_count, 0, 0, 0);
   else
      vkCmdDraw(ctx->current_cmd_buffer, desc->count, desc->instance_count, desc->first_vertex, 0);
}

static void
vk_driver_workaround(api_context *ctx, driver_wa wa)
{
   VkPipelineStageFlagBits2 stage_bits = 0;

   switch (wa) {
   case WA_RDNA4_TIMESTAMP_BUG:
      if (ctx->options.rdna4_timestamp_wa) {
         stage_bits = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |   /* PS_PARTIAL_FLUSH */
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;     /* CS_PARTIAL_FLUSH */
      }
      break;
   default:
      error("invalid workaround enum");
   }

   if (!stage_bits)
      return;

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

   vk_begin_cmdbuf(ctx, api_queue_gfx);
   vkCmdResetQueryPool(ctx->current_cmd_buffer, pool->pool, 0, pool->num_queries);
   vk_end_cmdbuf_and_submit(ctx, api_wait_all_queues, NULL, NULL);

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

   vk_wait_for_idle(ctx);
   vk_check(vkGetQueryPoolResults(ctx->device, pool->pool, 0, pool->num_written_queries,
                                  sizeof(uint64_t) * pool->num_written_queries, pool->results,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
}

static void
vk_upload_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      const void *data)
{
   api_buffer *staging = vk_create_buffer(ctx, size, api_heap_host_uncached, false);
   uint8_t *map;

   vk_check(vkMapMemory(ctx->device, *staging->mem, 0, size, 0, (void**)&map));
   memcpy(map, data, size);
   vkUnmapMemory(ctx->device, *staging->mem);

   vk_begin_cmdbuf(ctx, api_queue_gfx);
   vk_pipeline_barrier_buffers(ctx, 1, &buf, &offset, &size);
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
   vk_pipeline_barrier_buffers(ctx, 1, &buf, &offset, &size);
   vk_end_cmdbuf_and_submit(ctx, api_wait_all_queues, NULL, NULL);
}

static void
vk_get_buffer_data(struct api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                   void *data)
{
   api_buffer *staging = vk_create_buffer(ctx, size, api_heap_host_cached, 0);

   vk_begin_cmdbuf(ctx, api_queue_gfx);
   vk_pipeline_barrier_buffers(ctx, 1, &buf, &offset, &size);
   vkCmdCopyBuffer2(ctx->current_cmd_buffer,
                    &(VkCopyBufferInfo2) {
                       .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
                       .srcBuffer = buf->buffer,
                       .dstBuffer = staging->buffer,
                       .regionCount = 1,
                       .pRegions = &(VkBufferCopy2) {
                          .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
                          .srcOffset = offset,
                          .dstOffset = 0,
                          .size = size,
                       },
                    });
   vk_end_cmdbuf_and_submit(ctx, api_wait_all_queues, NULL, NULL);
   vk_wait_for_idle(ctx);

   void *map;
   vk_check(vkMapMemory(ctx->device, staging->mem[0], 0, size, 0, &map));
   memcpy(data, map, size);
   vkUnmapMemory(ctx->device, staging->mem[0]);
}

static void
vk_image_write_png(api_context *ctx, api_image *image, unsigned layer, const char *filename)
{
   api_image *staging = vk_create_image(ctx, VK_IMAGE_TYPE_2D,
                                        format_is_integer(image->format) ?
                                           VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM,
                                        image->width, image->height, 1, 1, VK_IMAGE_TILING_LINEAR,
                                        api_heap_host_cached);

   vk_begin_cmdbuf(ctx, api_queue_gfx);
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
   vk_end_cmdbuf_and_submit(ctx, api_wait_all_queues, NULL, NULL);
   vk_wait_for_idle(ctx);

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
                                .enabledLayerCount = vk_has_validation_layer(ctx) ? 1 : 0,
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

   printf("Physical devices: %u\n", count);
   if (ctx->options.device >= count)
      error("Device %u doesn't exist.", ctx->options.device);

   VkPhysicalDevice *devices = alloca(sizeof(devices[0]) * count);
   vk_check(vkEnumeratePhysicalDevices(instance, &count, devices));

   VkPhysicalDevice physical_device = devices[ctx->options.device];

   /* Get the extension list. */
   vk_check(vkEnumerateDeviceExtensionProperties(physical_device, NULL, &ctx->num_extensions, NULL));
   ctx->extensions = calloc(ctx->num_extensions, sizeof(VkExtensionProperties));
   vk_check(vkEnumerateDeviceExtensionProperties(physical_device, NULL, &ctx->num_extensions,
                                                 ctx->extensions));

   /* Get properties. */
   VkPhysicalDeviceProperties2 device_props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
   };
   VkPhysicalDeviceConservativeRasterizationPropertiesEXT EXT_conservative_rasterization_props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT,
   };
   VkPhysicalDeviceMeshShaderPropertiesEXT EXT_mesh_shader_props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT,
   };

   void **pNext = &device_props.pNext;

   if (has_extension(ctx, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME))
      chain_next(pNext, &EXT_conservative_rasterization_props);
   if (has_extension(ctx, VK_EXT_MESH_SHADER_EXTENSION_NAME))
      chain_next(pNext, &EXT_mesh_shader_props);

   vkGetPhysicalDeviceProperties2(physical_device, &device_props);
   printf("Selected device: %s\n", device_props.properties.deviceName);

   /* Core features. */
   VkPhysicalDeviceFeatures2 Vulkan10 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
   };
   VkPhysicalDeviceFeatures2 enabled_Vulkan10 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
   };

   VkPhysicalDeviceVulkan11Features Vulkan11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };
   VkPhysicalDeviceVulkan11Features enabled_Vulkan11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
   };

   VkPhysicalDeviceVulkan12Features Vulkan12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
   };
   VkPhysicalDeviceVulkan12Features enabled_Vulkan12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
   };

   VkPhysicalDeviceVulkan13Features Vulkan13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   VkPhysicalDeviceVulkan13Features enabled_Vulkan13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };

   /* KHR extensions. */
   VkPhysicalDeviceFragmentShadingRateFeaturesKHR KHR_fragment_shading_rate = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
   };
   VkPhysicalDeviceFragmentShadingRateFeaturesKHR enabled_KHR_fragment_shading_rate = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
   };

   VkPhysicalDeviceMaintenance5FeaturesKHR KHR_maintenance5 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
   };
   VkPhysicalDeviceMaintenance5FeaturesKHR enabled_KHR_maintenance5 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
   };

   VkPhysicalDeviceShaderClockFeaturesKHR KHR_shader_clock = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
   };
   VkPhysicalDeviceShaderClockFeaturesKHR enabled_KHR_shader_clock = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
   };

   /* EXT extensions. */
   VkPhysicalDeviceExtendedDynamicState3FeaturesEXT EXT_extended_dynamic_state3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
   };
   VkPhysicalDeviceExtendedDynamicState3FeaturesEXT enabled_EXT_extended_dynamic_state3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
   };

   VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT EXT_graphics_pipeline_library = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
   };
   VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT enabled_EXT_graphics_pipeline_library = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
   };

   VkPhysicalDeviceMeshShaderFeaturesEXT EXT_mesh_shader = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
   };
   VkPhysicalDeviceMeshShaderFeaturesEXT enabled_EXT_mesh_shader = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
   };

   VkPhysicalDeviceTransformFeedbackFeaturesEXT EXT_transform_feedback = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
   };
   VkPhysicalDeviceTransformFeedbackFeaturesEXT enabled_EXT_transform_feedback = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
   };

   VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT EXT_vertex_input_dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
   };
   VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT enabled_EXT_vertex_input_dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
   };

   /* AMD extensions. */
   VkPhysicalDeviceCoherentMemoryFeaturesAMD AMD_device_coherent_memory = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD,
   };
   VkPhysicalDeviceCoherentMemoryFeaturesAMD enabled_AMD_device_coherent_memory = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD,
   };

   /* Chain the feature structures and build the enabled extension list. */
   unsigned num_enabled_extensions = 0;
   const char *enabled_extensions[20];

   void **supported_pNext = &Vulkan10.pNext;
   void **enabled_pNext = &enabled_Vulkan10.pNext;

#define chain_features(features) do { \
      chain_next(supported_pNext, &(features)); \
      chain_next(enabled_pNext, &(enabled_##features)); \
   } while (0)

#define check_add_ext_no_features(name) do { \
      if (has_extension(ctx, (name))) { \
         assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions)); \
         enabled_extensions[num_enabled_extensions++] = (name); \
      } \
   } while (0)

#define check_add_ext(name, features) do { \
      if (has_extension(ctx, (name))) { \
         assert(num_enabled_extensions < ARRAY_SIZE(enabled_extensions)); \
         enabled_extensions[num_enabled_extensions++] = (name); \
         chain_features(features); \
      } \
   } while (0)

   chain_features(Vulkan11);
   chain_features(Vulkan12);
   chain_features(Vulkan13);

   check_add_ext(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, KHR_fragment_shading_rate);
   check_add_ext(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, KHR_maintenance5);
   check_add_ext_no_features(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
   check_add_ext(VK_KHR_SHADER_CLOCK_EXTENSION_NAME, KHR_shader_clock);

   check_add_ext_no_features(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
   check_add_ext(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, EXT_extended_dynamic_state3);
   check_add_ext(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, EXT_graphics_pipeline_library);
   check_add_ext(VK_EXT_MESH_SHADER_EXTENSION_NAME, EXT_mesh_shader);
   check_add_ext(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, EXT_transform_feedback);
   check_add_ext(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, EXT_vertex_input_dynamic_state);

   check_add_ext(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, AMD_device_coherent_memory);

   vkGetPhysicalDeviceFeatures2(physical_device, &Vulkan10);
   vkGetPhysicalDeviceMemoryProperties(physical_device, &ctx->memory_properties);

   /* Get queue support. */
   unsigned num_queue_families;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &num_queue_families, NULL);

   if (!num_queue_families)
      error("vkGetPhysicalDeviceQueueFamilyProperties returned num_queue_families=0.");

   VkQueueFamilyProperties *queue_props = alloca(sizeof(queue_props[0]) * num_queue_families);
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &num_queue_families, queue_props);

   const unsigned queue_require_flags[api_num_queues] = {
      [api_queue_gfx] = VK_QUEUE_GRAPHICS_BIT,
      [api_queue_compute] = VK_QUEUE_COMPUTE_BIT,
      [api_queue_transfer] = VK_QUEUE_TRANSFER_BIT,
      [api_queue_sparse] = VK_QUEUE_SPARSE_BINDING_BIT,
   };
   const unsigned queue_disallow_flags[api_num_queues] = {
      [api_queue_gfx] = 0,
      [api_queue_compute] = VK_QUEUE_GRAPHICS_BIT,
      [api_queue_transfer] = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
      [api_queue_sparse] = VK_QUEUE_GRAPHICS_BIT,
   };
   float default_queue_priorities[api_num_queues];

   for (unsigned i = 0; i < api_num_queues; i++)
      default_queue_priorities[i] = 1;

   unsigned queue_type_to_family[api_num_queues] = {0};
   unsigned queue_type_to_index[api_num_queues] = {0};
   unsigned num_queue_infos = 0;
   VkDeviceQueueCreateInfo *queue_infos = calloc(num_queue_families, sizeof(*queue_infos));

   for (unsigned family = 0; family < num_queue_families; family++) {
      for (unsigned type = 0; type < api_num_queues; type++) {
         if (!ctx->has_queue[type] && queue_props[family].queueFlags & queue_require_flags[type] &&
             !(queue_props[family].queueFlags & queue_disallow_flags[type])) {

            /* Find the family in the infos. */
            int info_index;
            for (info_index = 0; info_index < num_queue_infos; info_index++) {
               if (queue_infos[info_index].queueFamilyIndex == family)
                  break;
            }

            if (info_index == num_queue_infos) {
               /* Add a new info structure. */
               queue_infos[info_index].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
               queue_infos[info_index].queueFamilyIndex = family;
               queue_infos[info_index].pQueuePriorities = default_queue_priorities;
               num_queue_infos++;
            }

            ctx->has_queue[type] = true;
            queue_type_to_family[type] = family;
            queue_type_to_index[type] = queue_infos[info_index].queueCount++;
         }
      }
   }

   if (!ctx->has_queue[api_queue_gfx])
      error("VK_QUEUE_GRAPHICS_BIT support is required.");

#define require(field) do { \
      if (!field) \
         error(#field " is required."); \
      enabled_##field = field; \
   } while (0)

#define optional(field) enabled_##field = field

   //require(Vulkan10.features.robustBufferAccess);
   require(Vulkan10.features.fullDrawIndexUint32);
   //require(Vulkan10.features.imageCubeArray);
   //require(Vulkan10.features.independentBlend);
   require(Vulkan10.features.geometryShader);
   //require(Vulkan10.features.tessellationShader);
   require(Vulkan10.features.sampleRateShading);
   //require(Vulkan10.features.dualSrcBlend);
   //require(Vulkan10.features.logicOp);
   //require(Vulkan10.features.multiDrawIndirect);
   //require(Vulkan10.features.drawIndirectFirstInstance);
   //require(Vulkan10.features.depthClamp);
   //require(Vulkan10.features.depthBiasClamp);
   require(Vulkan10.features.fillModeNonSolid);
   //require(Vulkan10.features.depthBounds);
   //require(Vulkan10.features.wideLines);
   //require(Vulkan10.features.largePoints);
   //require(Vulkan10.features.alphaToOne);
   //require(Vulkan10.features.multiViewport);
   //require(Vulkan10.features.samplerAnisotropy);
   //require(Vulkan10.features.textureCompressionETC2);
   //require(Vulkan10.features.textureCompressionASTC_LDR);
   //require(Vulkan10.features.textureCompressionBC);
   //require(Vulkan10.features.occlusionQueryPrecise);
   require(Vulkan10.features.pipelineStatisticsQuery);
   require(Vulkan10.features.vertexPipelineStoresAndAtomics);
   require(Vulkan10.features.fragmentStoresAndAtomics);
   //require(Vulkan10.features.shaderTessellationAndGeometryPointSize);
   //require(Vulkan10.features.shaderImageGatherExtended);
   require(Vulkan10.features.shaderStorageImageExtendedFormats);
   //require(Vulkan10.features.shaderStorageImageMultisample);
   require(Vulkan10.features.shaderStorageImageReadWithoutFormat);
   require(Vulkan10.features.shaderStorageImageWriteWithoutFormat);
   //require(Vulkan10.features.shaderUniformBufferArrayDynamicIndexing);
   //require(Vulkan10.features.shaderSampledImageArrayDynamicIndexing);
   //require(Vulkan10.features.shaderStorageBufferArrayDynamicIndexing);
   //require(Vulkan10.features.shaderStorageImageArrayDynamicIndexing);
   require(Vulkan10.features.shaderClipDistance);
   require(Vulkan10.features.shaderCullDistance);
   //require(Vulkan10.features.shaderFloat64);
   optional(Vulkan10.features.shaderInt64);
   //require(Vulkan10.features.shaderInt16);
   //require(Vulkan10.features.shaderResourceResidency);
   //require(Vulkan10.features.shaderResourceMinLod);
   optional(Vulkan10.features.sparseBinding);
   optional(Vulkan10.features.sparseResidencyBuffer);
   //require(Vulkan10.features.sparseResidencyImage2D);
   //require(Vulkan10.features.sparseResidencyImage3D);
   //require(Vulkan10.features.sparseResidency2Samples);
   //require(Vulkan10.features.sparseResidency4Samples);
   //require(Vulkan10.features.sparseResidency8Samples);
   //require(Vulkan10.features.sparseResidency16Samples);
   //require(Vulkan10.features.sparseResidencyAliased);
   //require(Vulkan10.features.variableMultisampleRate);
   //require(Vulkan10.features.inheritedQueries);

   //require(Vulkan11.storageBuffer16BitAccess);
   //require(Vulkan11.uniformAndStorageBuffer16BitAccess);
   //require(Vulkan11.storagePushConstant16);
   //require(Vulkan11.storageInputOutput16);
   optional(Vulkan11.multiview);
   //require(Vulkan11.multiviewGeometryShader);
   //require(Vulkan11.multiviewTessellationShader);
   //require(Vulkan11.variablePointersStorageBuffer);
   //require(Vulkan11.variablePointers);
   //require(Vulkan11.protectedMemory);
   //require(Vulkan11.samplerYcbcrConversion);
   //require(Vulkan11.shaderDrawParameters);

   //require(Vulkan12.samplerMirrorClampToEdge);
   //require(Vulkan12.drawIndirectCount);
   optional(Vulkan12.storageBuffer8BitAccess);
   //require(Vulkan12.uniformAndStorageBuffer8BitAccess);
   //require(Vulkan12.storagePushConstant8);
   //require(Vulkan12.shaderBufferInt64Atomics);
   //require(Vulkan12.shaderSharedInt64Atomics);
   //require(Vulkan12.shaderFloat16);
   optional(Vulkan12.shaderInt8);
   //require(Vulkan12.descriptorIndexing);
   //require(Vulkan12.shaderInputAttachmentArrayDynamicIndexing);
   require(Vulkan12.shaderUniformTexelBufferArrayDynamicIndexing);
   //require(Vulkan12.shaderStorageTexelBufferArrayDynamicIndexing);
   //require(Vulkan12.shaderUniformBufferArrayNonUniformIndexing);
   //require(Vulkan12.shaderSampledImageArrayNonUniformIndexing);
   //require(Vulkan12.shaderStorageBufferArrayNonUniformIndexing);
   //require(Vulkan12.shaderStorageImageArrayNonUniformIndexing);
   //require(Vulkan12.shaderInputAttachmentArrayNonUniformIndexing);
   //require(Vulkan12.shaderUniformTexelBufferArrayNonUniformIndexing);
   //require(Vulkan12.shaderStorageTexelBufferArrayNonUniformIndexing);
   //require(Vulkan12.descriptorBindingUniformBufferUpdateAfterBind);
   //require(Vulkan12.descriptorBindingSampledImageUpdateAfterBind);
   //require(Vulkan12.descriptorBindingStorageImageUpdateAfterBind);
   //require(Vulkan12.descriptorBindingStorageBufferUpdateAfterBind);
   //require(Vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind);
   //require(Vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind);
   //require(Vulkan12.descriptorBindingUpdateUnusedWhilePending);
   //require(Vulkan12.descriptorBindingPartiallyBound);
   //require(Vulkan12.descriptorBindingVariableDescriptorCount);
   //require(Vulkan12.runtimeDescriptorArray);
   //require(Vulkan12.samplerFilterMinmax);
   //require(Vulkan12.scalarBlockLayout);
   //require(Vulkan12.imagelessFramebuffer);
   //require(Vulkan12.uniformBufferStandardLayout);
   //require(Vulkan12.shaderSubgroupExtendedTypes);
   require(Vulkan12.separateDepthStencilLayouts);
   //require(Vulkan12.hostQueryReset);
   require(Vulkan12.timelineSemaphore);
   optional(Vulkan12.bufferDeviceAddress);
   //require(Vulkan12.bufferDeviceAddressCaptureReplay);
   //require(Vulkan12.bufferDeviceAddressMultiDevice);
   //require(Vulkan12.vulkanMemoryModel);
   //require(Vulkan12.vulkanMemoryModelDeviceScope);
   //require(Vulkan12.vulkanMemoryModelAvailabilityVisibilityChains);
   //require(Vulkan12.shaderOutputViewportIndex);
   optional(Vulkan12.shaderOutputLayer);
   //require(Vulkan12.subgroupBroadcastDynamicId);

   //require(Vulkan13.robustImageAccess);
   //require(Vulkan13.inlineUniformBlock);
   //require(Vulkan13.descriptorBindingInlineUniformBlockUpdateAfterBind);
   //require(Vulkan13.pipelineCreationCacheControl);
   //require(Vulkan13.privateData);
   require(Vulkan13.shaderDemoteToHelperInvocation);
   //require(Vulkan13.shaderTerminateInvocation);
   //require(Vulkan13.subgroupSizeControl);
   //require(Vulkan13.computeFullSubgroups);
   require(Vulkan13.synchronization2);
   //require(Vulkan13.textureCompressionASTC_HDR);
   //require(Vulkan13.shaderZeroInitializeWorkgroupMemory);
   //require(Vulkan13.shaderIntegerDotProduct);
   require(Vulkan13.maintenance4);

   optional(KHR_fragment_shading_rate.pipelineFragmentShadingRate);
   optional(KHR_fragment_shading_rate.primitiveFragmentShadingRate);
   //optional(KHR_fragment_shading_rate.attachmentFragmentShadingRate);

   require(KHR_maintenance5.maintenance5);

   optional(KHR_shader_clock.shaderSubgroupClock);
   //optional(KHR_shader_clock.shaderDeviceClock);

   if (ctx->options.api_flags & API_VK_DYNAMIC_STATE) {
      require(Vulkan13.dynamicRendering);

      //require(EXT_extended_dynamic_state3.extendedDynamicState3TessellationDomainOrigin);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3DepthClampEnable);
      require(EXT_extended_dynamic_state3.extendedDynamicState3PolygonMode);
      require(EXT_extended_dynamic_state3.extendedDynamicState3RasterizationSamples);
      require(EXT_extended_dynamic_state3.extendedDynamicState3SampleMask);
      require(EXT_extended_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3AlphaToOneEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3LogicOpEnable);
      require(EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEnable);
      require(EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEquation);
      require(EXT_extended_dynamic_state3.extendedDynamicState3ColorWriteMask);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3RasterizationStream);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ConservativeRasterizationMode);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ExtraPrimitiveOverestimationSize);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3DepthClipEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3SampleLocationsEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendAdvanced);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ProvokingVertexMode);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3LineRasterizationMode);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3LineStippleEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3DepthClipNegativeOneToOne);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ViewportWScalingEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ViewportSwizzle);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3CoverageToColorEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3CoverageToColorLocation);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3CoverageModulationMode);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3CoverageModulationTableEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3CoverageModulationTable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3CoverageReductionMode);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3RepresentativeFragmentTestEnable);
      //require(EXT_extended_dynamic_state3.extendedDynamicState3ShadingRateImageEnable);

      require(EXT_vertex_input_dynamic_state.vertexInputDynamicState);
   }

   if (ctx->options.api_flags & API_VK_GPL)
      require(EXT_graphics_pipeline_library.graphicsPipelineLibrary);

   //optional(EXT_mesh_shader.taskShader);
   optional(EXT_mesh_shader.meshShader);
   //optional(EXT_mesh_shader.multiviewMeshShader);
   optional(EXT_mesh_shader.primitiveFragmentShadingRateMeshShader);
   //optional(EXT_mesh_shader.meshShaderQueries);

   optional(EXT_transform_feedback.transformFeedback);
   //optional(EXT_transform_feedback.geometryStreams);

   optional(AMD_device_coherent_memory.deviceCoherentMemory);

   /* Open the device. */
   vk_check(vkCreateDevice(physical_device,
                           &(VkDeviceCreateInfo) {
                              .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pNext = &enabled_Vulkan10,
                              .queueCreateInfoCount = num_queue_infos,
                              .pQueueCreateInfos = queue_infos,
                              .enabledExtensionCount = num_enabled_extensions,
                              .ppEnabledExtensionNames = enabled_extensions,
                           },
                           NULL, &ctx->device));
   free(queue_infos);

   /* Get extension functions. */
#define GET_PROC_ADDR(name) ctx->name = (PFN_##name)vkGetDeviceProcAddr(ctx->device, #name)

   if (EXT_vertex_input_dynamic_state.vertexInputDynamicState)
      GET_PROC_ADDR(vkCmdSetVertexInputEXT);
   if (EXT_mesh_shader.meshShader)
      GET_PROC_ADDR(vkCmdDrawMeshTasksEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3PolygonMode)
      GET_PROC_ADDR(vkCmdSetPolygonModeEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3RasterizationSamples)
      GET_PROC_ADDR(vkCmdSetRasterizationSamplesEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3SampleMask)
      GET_PROC_ADDR(vkCmdSetSampleMaskEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable)
      GET_PROC_ADDR(vkCmdSetAlphaToCoverageEnableEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEnable)
      GET_PROC_ADDR(vkCmdSetColorBlendEnableEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEquation)
      GET_PROC_ADDR(vkCmdSetColorBlendEquationEXT);
   if (EXT_extended_dynamic_state3.extendedDynamicState3ColorWriteMask)
      GET_PROC_ADDR(vkCmdSetColorWriteMaskEXT);
   if (KHR_fragment_shading_rate.pipelineFragmentShadingRate)
      GET_PROC_ADDR(vkCmdSetFragmentShadingRateKHR);
#undef GET_PROC_ADDR

   /* Get the queues. */
   for (unsigned i = 0; i < api_num_queues; i++) {
      if (!ctx->has_queue[i])
         continue;

      vkGetDeviceQueue2(ctx->device, &(VkDeviceQueueInfo2) {
                           .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                           .queueFamilyIndex = queue_type_to_family[i],
                           .queueIndex = queue_type_to_index[i],
                        }, &ctx->queue[i]);

      if (i != api_queue_sparse) {
         vk_check(vkCreateCommandPool(ctx->device,
                                      &(const VkCommandPoolCreateInfo) {
                                         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .queueFamilyIndex = queue_type_to_family[i],
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      },
                                      NULL, &ctx->cmd_pool[i]));

         vk_check(vkAllocateCommandBuffers(ctx->device,
                                           &(VkCommandBufferAllocateInfo) {
                                              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = ctx->cmd_pool[i],
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = MAX_COMMAND_BUFFERS,
                                           },
                                           ctx->cmd_buffers[i]));
      }

      vk_check(vkCreateSemaphore(ctx->device,
                                 &(VkSemaphoreCreateInfo){
                                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                    .pNext = &(VkSemaphoreTypeCreateInfo){
                                       .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                       .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                   },
                                 },
                                 NULL, &ctx->semaphore[i]));
   }

   /* Create misc stuff. */
   vk_check(vkCreateDescriptorPool(ctx->device,
                                   &(VkDescriptorPoolCreateInfo){
                                      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = MAX_DESCRIPTOR_SETS,
                                      .poolSizeCount = 4,
                                      .pPoolSizes = (VkDescriptorPoolSize[4]){
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_UNIFORM_BUFFER_BINDINGS,
                                         },
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_STORAGE_BUFFER_BINDINGS,
                                         },
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_STORAGE_IMAGE_BINDINGS,
                                         },
                                         {
                                            .type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                            .descriptorCount = MAX_DESCRIPTOR_SETS * MAX_UNIFORM_TEXEL_BUFFER_BINDINGS,
                                         },
                                      },
                                   },
                                   NULL, &ctx->descriptor_pool));

   vk_check(vkCreatePipelineLayout(ctx->device,
                                   &(VkPipelineLayoutCreateInfo) {
                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                   },
                                   NULL, &ctx->empty_pipeline_layout));

   ctx->glsl_compiler = shaderc_compiler_initialize();
   ctx->glsl_compiler_options = shaderc_compile_options_initialize();

   for (unsigned i = 0; i < api_num_heaps; i++) {
      if ((i == api_heap_device_coherent_amd || i == api_heap_host_uncached_coherent_amd) &&
          !AMD_device_coherent_memory.deviceCoherentMemory)
         continue;

      ctx->has_heap[i] = true; /* so that vk_find_heap doesn't fall back and fails when it should */
      ctx->has_heap[i] = vk_find_heap(ctx, ~0, i) != -1;
   }

   ctx->allow_parallel_create_shader = true;
   ctx->allow_parallel_create_pipeline = true;
   ctx->timestamp_period_in_seconds = device_props.properties.limits.timestampPeriod * 0.000000001;

   ctx->has_blit_image_3d = true;
   ctx->has_blit_image_msaa = false;
   ctx->has_buffer_device_address = Vulkan12.bufferDeviceAddress;
   ctx->has_clear_image_region = false;
   ctx->has_fully_covered = EXT_conservative_rasterization_props.fullyCoveredFragmentShaderInputVariable;
   ctx->has_image_tiling_linear = true;
   ctx->has_multiview = Vulkan11.multiview;
   ctx->has_resolve_image_yflip = false;
   ctx->has_shader_int8 = Vulkan12.storageBuffer8BitAccess && Vulkan12.shaderInt8;
   ctx->has_shader_int64 = Vulkan10.features.shaderInt64;
   ctx->has_shader_subgroup_clock = KHR_shader_clock.shaderSubgroupClock;
   ctx->has_sparse_buffer = ctx->has_queue[api_queue_sparse] && Vulkan10.features.sparseBinding &&
                            Vulkan10.features.sparseResidencyBuffer;
   ctx->has_vs_tes_layer_output = Vulkan12.shaderOutputLayer;
   ctx->has_vrs = KHR_fragment_shading_rate.pipelineFragmentShadingRate && KHR_fragment_shading_rate.primitiveFragmentShadingRate &&
                  (EXT_mesh_shader.meshShader ? EXT_mesh_shader.primitiveFragmentShadingRateMeshShader : true);
   ctx->has_xfb = EXT_transform_feedback.transformFeedback;

   ctx->max_mesh_workgroup_size = EXT_mesh_shader.meshShader ? EXT_mesh_shader_props.maxMeshWorkGroupInvocations : 0;
   ctx->max_storage_buffer_range = device_props.properties.limits.maxStorageBufferRange;
   ctx->max_uniform_buffer_range = device_props.properties.limits.maxUniformBufferRange;
   ctx->supported_color_sample_counts = device_props.properties.limits.framebufferColorSampleCounts;

   /* Get sparse buffer alignment. */
   if (ctx->has_sparse_buffer) {
      VkMemoryRequirements2 sparse_buf_mem_req = {
          .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      };
      vkGetDeviceBufferMemoryRequirements(ctx->device,
                                          &(VkDeviceBufferMemoryRequirements){
                                             .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
                                             .pCreateInfo = &(VkBufferCreateInfo){
                                                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                                         VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                                .size = 512 << 20,
                                                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                             },
                                          }, &sparse_buf_mem_req);
      ctx->sparse_buffer_alignment = sparse_buf_mem_req.memoryRequirements.alignment;
   }

   /* Set up the GLSL compiler. */
   shaderc_compile_options_set_target_env(ctx->glsl_compiler_options, shaderc_target_env_vulkan,
                                          shaderc_env_version_vulkan_1_3);
   shaderc_compile_options_set_target_spirv(ctx->glsl_compiler_options, shaderc_spirv_version_1_6);
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
   ctx->destroy_buffer = vk_destroy_buffer;
   ctx->upload_buffer_data = vk_upload_buffer_data;
   ctx->get_buffer_data = vk_get_buffer_data;
   ctx->clear_buffer = vk_clear_buffer;
   ctx->copy_buffer = vk_copy_buffer;
   ctx->buffer_bind_sparse = vk_buffer_bind_sparse;

   ctx->create_image = vk_create_image;
   ctx->destroy_image = vk_destroy_image;
   ctx->clear_image = vk_clear_image;
   ctx->blit_image = vk_blit_image;
   ctx->image_write_png = vk_image_write_png;

   ctx->create_framebuffer = vk_create_framebuffer;
   ctx->destroy_framebuffer = vk_destroy_framebuffer;

   ctx->create_shader = vk_create_shader;
   ctx->destroy_shader = vk_destroy_shader;

   ctx->create_descriptor_set_layout = vk_create_descriptor_set_layout;
   ctx->destroy_descriptor_set_layout = NULL;

   ctx->create_descriptor_set = vk_create_descriptor_set;
   ctx->destroy_descriptor_set = NULL;
   ctx->set_uniform_buffer_descriptor = vk_set_uniform_buffer_descriptors;
   ctx->set_storage_buffer_descriptor = vk_set_storage_buffer_descriptors;
   ctx->set_uniform_texel_buffer_descriptors = vk_set_uniform_texel_buffer_descriptors;
   ctx->set_storage_image_descriptors = vk_set_storage_image_descriptors;
   ctx->bind_descriptor_set = vk_bind_descriptor_set;

   ctx->create_pipeline = vk_create_pipeline;
   ctx->destroy_pipeline = vk_destroy_pipeline;
   ctx->bind_pipeline = vk_bind_pipeline;

   ctx->create_compute_pipeline = vk_create_compute_pipeline;
   ctx->destroy_compute_pipeline = vk_destroy_compute_pipeline;
   ctx->bind_compute_pipeline = vk_bind_compute_pipeline;
   ctx->dispatch = vk_dispatch;
   ctx->pipeline_barrier_buffer = vk_pipeline_barrier_buffer;

   ctx->begin_cmdbuf = vk_begin_cmdbuf;
   ctx->end_cmdbuf_and_submit = vk_end_cmdbuf_and_submit;
   ctx->wait_idle_before_deallocation = vk_wait_for_idle;

   ctx->begin_render_pass = vk_begin_render_pass;
   ctx->end_render_pass = vk_end_render_pass;

   ctx->bind_vertex_buffers = vk_bind_vertex_buffers;
   ctx->bind_index_buffer = vk_bind_index_buffer;
   ctx->draw = vk_draw;
   ctx->driver_workaround = vk_driver_workaround;

   ctx->create_timestamp_pool = vk_create_timestamp_pool;
   ctx->write_next_timestamp = vk_write_next_timestamp;
   ctx->query_timestamps = vk_query_timestamps;

   return ctx;
}
