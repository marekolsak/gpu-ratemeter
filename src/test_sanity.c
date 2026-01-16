/* Copyright 2026 Advanced Micro Devices, Inc.
 *
 * For code from vkcube:
 *    Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 *    Copyright (c) 2012 Rob Clark <rob@ti.com>
 *    Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "common.h"

static api_buffer *
init_cube_vb(api_context *ctx, uint64_t *vb_offsets)
{
   static const float vVertices[] = {
      // front
      -1.0f, -1.0f, +1.0f, // point blue
      +1.0f, -1.0f, +1.0f, // point magenta
      -1.0f, +1.0f, +1.0f, // point cyan
      +1.0f, +1.0f, +1.0f, // point white
      // back
      +1.0f, -1.0f, -1.0f, // point red
      -1.0f, -1.0f, -1.0f, // point black
      +1.0f, +1.0f, -1.0f, // point yellow
      -1.0f, +1.0f, -1.0f, // point green
      // right
      +1.0f, -1.0f, +1.0f, // point magenta
      +1.0f, -1.0f, -1.0f, // point red
      +1.0f, +1.0f, +1.0f, // point white
      +1.0f, +1.0f, -1.0f, // point yellow
      // left
      -1.0f, -1.0f, -1.0f, // point black
      -1.0f, -1.0f, +1.0f, // point blue
      -1.0f, +1.0f, -1.0f, // point green
      -1.0f, +1.0f, +1.0f, // point cyan
      // top
      -1.0f, +1.0f, +1.0f, // point cyan
      +1.0f, +1.0f, +1.0f, // point white
      -1.0f, +1.0f, -1.0f, // point green
      +1.0f, +1.0f, -1.0f, // point yellow
      // bottom
      -1.0f, -1.0f, -1.0f, // point black
      +1.0f, -1.0f, -1.0f, // point red
      -1.0f, -1.0f, +1.0f, // point blue
      +1.0f, -1.0f, +1.0f  // point magenta
   };

   static const float vColors[] = {
      // front
      0.0f,  0.0f,  1.0f, // blue
      1.0f,  0.0f,  1.0f, // magenta
      0.0f,  1.0f,  1.0f, // cyan
      1.0f,  1.0f,  1.0f, // white
      // back
      1.0f,  0.0f,  0.0f, // red
      0.0f,  0.0f,  0.0f, // black
      1.0f,  1.0f,  0.0f, // yellow
      0.0f,  1.0f,  0.0f, // green
      // right
      1.0f,  0.0f,  1.0f, // magenta
      1.0f,  0.0f,  0.0f, // red
      1.0f,  1.0f,  1.0f, // white
      1.0f,  1.0f,  0.0f, // yellow
      // left
      0.0f,  0.0f,  0.0f, // black
      0.0f,  0.0f,  1.0f, // blue
      0.0f,  1.0f,  0.0f, // green
      0.0f,  1.0f,  1.0f, // cyan
      // top
      0.0f,  1.0f,  1.0f, // cyan
      1.0f,  1.0f,  1.0f, // white
      0.0f,  1.0f,  0.0f, // green
      1.0f,  1.0f,  0.0f, // yellow
      // bottom
      0.0f,  0.0f,  0.0f, // black
      1.0f,  0.0f,  0.0f, // red
      0.0f,  0.0f,  1.0f, // blue
      1.0f,  0.0f,  1.0f  // magenta
   };

   static const float vNormals[] = {
      // front
      +0.0f, +0.0f, +1.0f, // forward
      +0.0f, +0.0f, +1.0f, // forward
      +0.0f, +0.0f, +1.0f, // forward
      +0.0f, +0.0f, +1.0f, // forward
      // back
      +0.0f, +0.0f, -1.0f, // backbard
      +0.0f, +0.0f, -1.0f, // backbard
      +0.0f, +0.0f, -1.0f, // backbard
      +0.0f, +0.0f, -1.0f, // backbard
      // right
      +1.0f, +0.0f, +0.0f, // right
      +1.0f, +0.0f, +0.0f, // right
      +1.0f, +0.0f, +0.0f, // right
      +1.0f, +0.0f, +0.0f, // right
      // left
      -1.0f, +0.0f, +0.0f, // left
      -1.0f, +0.0f, +0.0f, // left
      -1.0f, +0.0f, +0.0f, // left
      -1.0f, +0.0f, +0.0f, // left
      // top
      +0.0f, +1.0f, +0.0f, // up
      +0.0f, +1.0f, +0.0f, // up
      +0.0f, +1.0f, +0.0f, // up
      +0.0f, +1.0f, +0.0f, // up
      // bottom
      +0.0f, -1.0f, +0.0f, // down
      +0.0f, -1.0f, +0.0f, // down
      +0.0f, -1.0f, +0.0f, // down
      +0.0f, -1.0f, +0.0f  // down
   };

   unsigned vertex_offset = 0;
   unsigned colors_offset = vertex_offset + sizeof(vVertices);
   unsigned normals_offset = colors_offset + sizeof(vColors);
   unsigned mem_size = normals_offset + sizeof(vNormals);

   api_buffer *vb = ctx->create_buffer(ctx, mem_size, api_heap_device);
   ctx->upload_buffer_data(ctx, vb, vertex_offset, sizeof(vVertices), vVertices);
   ctx->upload_buffer_data(ctx, vb, colors_offset, sizeof(vColors), vColors);
   ctx->upload_buffer_data(ctx, vb, normals_offset, sizeof(vNormals), vNormals);

   vb_offsets[0] = vertex_offset;
   vb_offsets[1] = colors_offset;
   vb_offsets[2] = normals_offset;
   return vb;
}

void
test_sanity(api_context *ctx, const char *test_suite_name)
{
   uint64_t vb_offsets[3];

   api_image *colorbuf = ctx->create_image(ctx, VK_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 1,
                                           VK_IMAGE_TILING_OPTIMAL, api_heap_device, 0);
   api_framebuffer *fb = ctx->create_framebuffer(ctx, colorbuf, NULL, colorbuf->width,
                                                 colorbuf->height, colorbuf->samples);
   api_buffer *vb = init_cube_vb(ctx, vb_offsets);

   const char *vs_source =
      "#version 420 core \n"
      " \n"
      "const mat4 modelviewMatrix = mat4(0.696364, 0.369616, 0.615192, 0.000000, \n"
      "                                  0.122788, 0.783188, -0.609540, 0.000000, \n"
      "                                  -0.707107, 0.500000, 0.500000, 0.000000, \n"
      "                                  0.000000, 0.000000, -8.000000, 1.000000); \n"
      "const mat4 modelviewprojectionMatrix = mat4(1.492209, 0.792034, -1.537979, -0.615192, \n"
      "                                            0.263117, 1.678261, 1.523850, 0.609540, \n"
      "                                            -1.515229, 1.071428, -1.250000, -0.500000, \n"
      "                                            0.000000, 0.000000, 5.000000, 8.000000); \n"
      " \n"
      "layout(location = 0) in vec4 in_position; \n"
      "layout(location = 1) in vec4 in_color; \n"
      "layout(location = 2) in vec3 in_normal; \n"
      " \n"
      "vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0); \n"
      " \n"
      "layout(location = 0) out vec4 vVaryingColor; \n"
      " \n"
      "void main() \n"
      "{ \n"
      "    gl_Position = modelviewprojectionMatrix * in_position; \n"
      "    vec3 vEyeNormal = mat3(modelviewMatrix) * in_normal; \n"
      "    vec4 vPosition4 = modelviewMatrix * in_position; \n"
      "    vec3 vPosition3 = vPosition4.xyz / vPosition4.w; \n"
      "    vec3 vLightDir = normalize(lightSource.xyz - vPosition3); \n"
      "    float diff = max(0.0, dot(vEyeNormal, vLightDir)); \n"
      "    vVaryingColor = vec4(diff * in_color.rgb, 1.0); \n"
      "}";

   const char *fs_source =
      "#version 420 core \n"
      " \n"
      "layout(location = 0) in vec4 vVaryingColor; \n"
      "layout(location = 0) out vec4 f_color; \n"
      " \n"
      "void main() \n"
      "{ \n"
      "    f_color = vVaryingColor; \n"
      "}";

   api_shader *vs = ctx->create_shader(ctx, vs_source, api_shader_vs);
   api_shader *fs = ctx->create_shader(ctx, fs_source, api_shader_fs);

   api_pipeline_desc pipeline_desc = {
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
      .cull_mode = VK_CULL_MODE_BACK_BIT,

      .num_vb_desc = 3,
      .vb_strides = {
         3 * sizeof(float),
         3 * sizeof(float),
         3 * sizeof(float),
      },
      .vb_formats = {
         VK_FORMAT_R32G32B32_SFLOAT,
         VK_FORMAT_R32G32B32_SFLOAT,
         VK_FORMAT_R32G32B32_SFLOAT,
      },

      .vs = vs,
      .fs = fs,

      .vrs_fragment_size = {1, 1},
      .colormask = 0xf,
      .fb = fb,
   };
   api_pipeline *pipeline = ctx->create_pipeline(ctx, &pipeline_desc);

   ctx->begin_cmdbuf(ctx);
   ctx->begin_render_pass(ctx, &(api_render_pass_desc){
                             .fb = fb,
                             .color_clear_value.float32 = {0.2, 0.2, 0.2, 1},
                          });
   ctx->bind_pipeline(ctx, pipeline);
   ctx->bind_vertex_buffers(ctx, vb, vb_offsets);

   ctx->draw(ctx, &(api_draw_desc){.count = 4, .first_vertex = 0});
   ctx->draw(ctx, &(api_draw_desc){.count = 4, .first_vertex = 4});
   ctx->draw(ctx, &(api_draw_desc){.count = 4, .first_vertex = 8});
   ctx->draw(ctx, &(api_draw_desc){.count = 4, .first_vertex = 12});
   ctx->draw(ctx, &(api_draw_desc){.count = 4, .first_vertex = 16});
   ctx->draw(ctx, &(api_draw_desc){.count = 4, .first_vertex = 20});

   ctx->end_render_pass(ctx);
   ctx->end_cmdbuf_and_submit(ctx);

   ctx->image_write_png(ctx, fb->colorbuf, "output.png");
   run_image_viewer("output.png");
}
