/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#define D3D11_PRIVATE
#include "common.h"

static api_buffer *
d3d11_create_buffer(api_context *ctx, uint64_t size, api_heap_type heap, unsigned sparse_block_size)
{
   api_buffer *buf = calloc(1, sizeof(api_buffer));
   buf->size = size;
   buf->heap = heap;


   return buf;
}

static api_image *
d3d11_create_image(api_context *ctx, VkImageType type, VkFormat format, unsigned width, unsigned height,
                   unsigned depth, unsigned samples, VkImageTiling tiling, api_heap_type heap)
{
   api_image *image = calloc(1, sizeof(api_image));
   image->type = type;
   image->width = width;
   image->height = height;
   image->depth = depth;
   image->samples = samples;


   return image;
}

static api_framebuffer *
d3d11_create_framebuffer(api_context *ctx, api_image *colorbuf, api_image *zbuf,
                         unsigned width, unsigned height, unsigned samples, unsigned view_mask)
{
   api_framebuffer *fb = calloc(1, sizeof(api_framebuffer));
   fb->width = width;
   fb->height = height;
   fb->samples = samples;
   fb->colorbuf = colorbuf;
   fb->zbuf = zbuf;


   return fb;
}

static api_shader *
d3d11_create_shader(api_context *ctx, const char *source, api_shader_type type)
{
   api_shader *shader = calloc(1, sizeof(api_shader));


   return shader;
}

static api_gfx_pipeline *
d3d11_create_gfx_pipeline(api_context *ctx, const api_gfx_pipeline_desc *desc)
{
   api_gfx_pipeline *pipeline = calloc(1, sizeof(api_gfx_pipeline));
   pipeline->desc = *desc;


   return pipeline;
}

static void
d3d11_bind_gfx_pipeline(api_context *ctx, api_gfx_pipeline *pipeline)
{
}

static void
d3d11_begin_cmdbuf(api_context *ctx, api_queue_type queue)
{
}

static void
d3d11_end_cmdbuf_and_submit(api_context *ctx, unsigned wait_queue_mask, api_fence *wait_fence,
                            api_fence **signal_fence)
{
}

static void
d3d11_begin_render_pass(api_context *ctx, const api_render_pass_desc *desc)
{
}

static void
d3d11_end_render_pass(api_context *ctx)
{
}

static void
d3d11_bind_vertex_buffers(api_context *ctx, api_buffer *vb, const uint64_t *vb_offsets)
{
}

static void
d3d11_draw(api_context *ctx, const api_draw_desc *desc)
{
}

static api_query_pool *
d3d11_create_query_pool(api_context *ctx, unsigned num_queries, api_query_type type)
{
   api_query_pool *pool = calloc(1, sizeof(api_query_pool));

   pool->type = type;
   pool->num_written_queries = 0;
   pool->num_queries = num_queries;
   pool->results = calloc(num_queries, sizeof(uint64_t));

   return pool;
}

static void
d3d11_write_next_query_value(api_context *ctx, api_query_pool *pool)
{
}

static void
d3d11_get_query_results(api_context *ctx, api_query_pool *pool)
{
}

static void
d3d11_upload_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      const void *data)
{
}

static void
d3d11_image_write_png(api_context *ctx, api_image *image, unsigned layer, const char *filename)
{
}

api_context *
d3d11_create_context(const program_options *options)
{
   api_context *ctx = calloc(1, sizeof(api_context));
   ctx->options = *options;


   ctx->timestamp_period_in_seconds = 0.000000001;

   ctx->destroy_context = NULL;

   ctx->create_buffer = d3d11_create_buffer;
   ctx->destroy_buffer = NULL;
   ctx->upload_buffer_data = d3d11_upload_buffer_data;

   ctx->create_image = d3d11_create_image;
   ctx->destroy_image = NULL;
   ctx->image_write_png = d3d11_image_write_png;

   ctx->create_framebuffer = d3d11_create_framebuffer;
   ctx->destroy_framebuffer = NULL;

   ctx->create_shader = d3d11_create_shader;
   ctx->destroy_shader = NULL;

   ctx->create_gfx_pipeline = d3d11_create_gfx_pipeline;
   ctx->destroy_gfx_pipeline = NULL;
   ctx->bind_gfx_pipeline = d3d11_bind_gfx_pipeline;

   ctx->begin_cmdbuf = d3d11_begin_cmdbuf;
   ctx->end_cmdbuf_and_submit = d3d11_end_cmdbuf_and_submit;

   ctx->begin_render_pass = d3d11_begin_render_pass;
   ctx->end_render_pass = d3d11_end_render_pass;

   ctx->bind_vertex_buffers = d3d11_bind_vertex_buffers;
   ctx->draw = d3d11_draw;

   ctx->create_query_pool = d3d11_create_query_pool;
   ctx->write_next_query_value = d3d11_write_next_query_value;
   ctx->get_query_results = d3d11_get_query_results;

   return ctx;
}
