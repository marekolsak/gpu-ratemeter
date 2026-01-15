/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#define D3D11_PRIVATE
#include "common.h"

static api_buffer *
d3d11_create_buffer(api_context *ctx, uint64_t size, api_heap_type heap)
{
   api_buffer *buf = calloc(1, sizeof(api_buffer));
   buf->size = size;
   buf->heap = heap;


   return buf;
}

static api_image *
d3d11_create_image(api_context *ctx, VkFormat format, unsigned width, unsigned height,
                   unsigned samples, VkImageTiling tiling, api_heap_type heap,
                   VkImageLayout initial_layout)
{
   api_image *image = calloc(1, sizeof(api_image));
   image->width = width;
   image->height = height;
   image->samples = samples;


   return image;
}

static api_framebuffer *
d3d11_create_framebuffer(api_context *ctx, api_image *colorbuf, api_image *zbuf,
                         unsigned width, unsigned height, unsigned samples)
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

static api_pipeline *
d3d11_create_pipeline(api_context *ctx, const api_pipeline_desc *desc)
{
   api_pipeline *pipeline = calloc(1, sizeof(api_pipeline));
   pipeline->fb = desc->fb;


   return pipeline;
}

static void
d3d11_bind_pipeline(api_context *ctx, api_pipeline *pipeline)
{
}

static void
d3d11_begin_cmdbuf(api_context *ctx)
{
}

static void
d3d11_end_cmdbuf_and_submit(api_context *ctx)
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

static api_timestamp_query_pool *
d3d11_create_timestamp_pool(api_context *ctx, unsigned num_queries)
{
   api_timestamp_query_pool *pool = calloc(1, sizeof(api_timestamp_query_pool));

   pool->num_written_queries = 0;
   pool->num_queries = num_queries;
   pool->results = calloc(num_queries, sizeof(uint64_t));

   return pool;
}

static void
d3d11_write_next_timestamp(api_context *ctx, api_timestamp_query_pool *pool)
{
}

static void
d3d11_query_timestamps(api_context *ctx, api_timestamp_query_pool *pool)
{
}

static void
d3d11_upload_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      const void *data)
{
}

static void
d3d11_image_write_png(api_context *ctx, api_image *image, const char *filename)
{
}

api_context *
d3d11_create_context(const program_options *options)
{
   api_context *ctx = calloc(1, sizeof(api_context));
   ctx->options = *options;


   ctx->timestamp_period_in_seconds = 0.000000001;
   ctx->vram_usage = 0;

   ctx->destroy_context = NULL;

   ctx->create_buffer = d3d11_create_buffer;
   ctx->upload_buffer_data = d3d11_upload_buffer_data;
   ctx->destroy_buffer = NULL;

   ctx->create_image = d3d11_create_image;
   ctx->image_write_png = d3d11_image_write_png;
   ctx->destroy_image = NULL;

   ctx->create_framebuffer = d3d11_create_framebuffer;
   ctx->destroy_framebuffer = NULL;

   ctx->create_shader = d3d11_create_shader;
   ctx->destroy_shader = NULL;

   ctx->create_pipeline = d3d11_create_pipeline;
   ctx->bind_pipeline = d3d11_bind_pipeline;
   ctx->destroy_pipeline = NULL;

   ctx->begin_cmdbuf = d3d11_begin_cmdbuf;
   ctx->end_cmdbuf_and_submit = d3d11_end_cmdbuf_and_submit;

   ctx->begin_render_pass = d3d11_begin_render_pass;
   ctx->end_render_pass = d3d11_end_render_pass;

   ctx->bind_vertex_buffers = d3d11_bind_vertex_buffers;
   ctx->draw = d3d11_draw;

   ctx->create_timestamp_pool = d3d11_create_timestamp_pool;
   ctx->write_next_timestamp = d3d11_write_next_timestamp;
   ctx->query_timestamps = d3d11_query_timestamps;

   return ctx;
}
