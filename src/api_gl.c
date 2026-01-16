/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
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
gl_create_buffer(api_context *ctx, uint64_t size, api_heap_type heap)
{
   api_buffer *buf = calloc(1, sizeof(api_buffer));
   buf->size = size;
   buf->heap = heap;

   unsigned flags = 0;

   switch (heap) {
   case api_heap_vram:
      flags = GL_DYNAMIC_STORAGE_BIT; /* needed by glBufferSubData */
      break;
   case api_heap_sysmem_uswc:
      flags = GL_DYNAMIC_STORAGE_BIT | /* needed by glBufferSubData */
              GL_CLIENT_STORAGE_BIT |  /* should allocate in system memory */
              GL_MAP_WRITE_BIT;  /* should use USWC if READ_BIT is not set */
      break;
   case api_heap_sysmem_cached:
      flags = GL_DYNAMIC_STORAGE_BIT | /* needed by glBufferSubData */
              GL_CLIENT_STORAGE_BIT |  /* should allocate in system memory */
              GL_MAP_WRITE_BIT |
              GL_MAP_READ_BIT;   /* should be cached */
      break;
   default:
      error("invalid heap type");
   }

   glCreateBuffers(1, &buf->id);
   glNamedBufferStorage(buf->id, size, NULL, flags);
   gl_check_no_error();

   if (heap == api_heap_vram)
      ctx->vram_usage += size;
   return buf;
}

static void
gl_upload_buffer_data(api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                      const void *data)
{
   glNamedBufferSubData(buf->id, offset, size, data);
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
gl_copy_buffer(struct api_context *ctx, api_buffer *dst, api_buffer *src, uint64_t dst_offset,
               uint64_t src_offset, uint64_t size)
{
   glCopyNamedBufferSubData(src->id, dst->id, src_offset, dst_offset, size);
   gl_check_no_error();
}

static GLenum
get_gl_format(VkFormat format)
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
      error("unexpected image format %u", format);
      return 0;
   }
}

static api_image *
gl_create_image(api_context *ctx, VkFormat format, unsigned width, unsigned height,
                unsigned samples, VkImageTiling tiling, api_heap_type heap,
                VkImageLayout initial_layout)
{
   api_image *image = calloc(1, sizeof(api_image));
   image->width = width;
   image->height = height;
   image->samples = samples;
   image->format = format;

   if (heap != api_heap_vram)
      error("GL only supports heap=vram for textures");

   GLenum glformat = get_gl_format(format);

   if (samples > 1) {
      glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &image->id);
      glTextureStorage2DMultisample(image->id, samples, glformat, width, height, true);
   } else {
      glCreateTextures(GL_TEXTURE_2D, 1, &image->id);
      glTextureStorage2D(image->id, 1, glformat, width, height);
   }
   gl_check_no_error();

   ctx->vram_usage += (uint64_t)width * height * get_pixel_size_from_format(format) * samples;
   return image;
}

static api_framebuffer *
gl_create_framebuffer(api_context *ctx, api_image *colorbuf, api_image *zbuf,
                      unsigned width, unsigned height, unsigned samples)
{
   api_framebuffer *fb = calloc(1, sizeof(api_framebuffer));
   fb->width = width;
   fb->height = height;
   fb->samples = samples;
   fb->colorbuf = colorbuf;
   fb->zbuf = zbuf;

   glCreateFramebuffers(1, &fb->id);
   if (colorbuf)
      glNamedFramebufferTexture(fb->id, GL_COLOR_ATTACHMENT0, colorbuf->id, 0);
   if (zbuf)
      glNamedFramebufferTexture(fb->id, GL_DEPTH_ATTACHMENT, zbuf->id, 0);

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

static api_descriptor_set_layout *
gl_create_descriptor_set_layout(struct api_context *ctx,
                                const api_descriptor_set_layout_desc *desc)
{
   api_descriptor_set_layout *layout = calloc(1, sizeof(api_descriptor_set_layout));
   layout->desc = *desc;

   assert(desc->uniform_buffer.array_size <= MAX_UNIFORM_BUFFER_ARRAY_SIZE);
   for (unsigned i = 0; i < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS; i++)
      assert(desc->uniform_texel_buffer[i].array_size <= MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE);
   assert(desc->storage_image.array_size <= MAX_STORAGE_IMAGE_ARRAY_SIZE);

   return layout;
}

static api_descriptor_set *
gl_create_descriptor_set(api_context *ctx, api_descriptor_set_layout *layout)
{
   api_descriptor_set *set = calloc(1, sizeof(api_descriptor_set));
   set->layout = layout;

   return set;
}

static void
gl_set_uniform_buffer_descriptors(struct api_context *ctx, api_descriptor_set *set,
                                  api_buffer *buffer, uint64_t offset, uint64_t size)
{
   assert(set->layout->desc.uniform_buffer.array_size);
   set->ubo_id = buffer->id;
   set->ubo_offset = offset;
   set->ubo_size = size;
}

static void
gl_set_uniform_texel_buffer_descriptors(struct api_context *ctx, api_descriptor_set *set,
                                        unsigned binding_index, unsigned num_buffers,
                                        api_buffer **buffers, VkFormat *formats,
                                        uint64_t *offsets, uint64_t *sizes)
{
   assert(binding_index < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS);
   assert(num_buffers <= set->layout->desc.uniform_texel_buffer[binding_index].array_size);

   glDeleteTextures(num_buffers, set->tbo_ids[binding_index]);
   glCreateTextures(GL_TEXTURE_BUFFER, num_buffers, set->tbo_ids[binding_index]);

   for (unsigned i = 0; i < num_buffers; i++) {
      glTextureBufferRange(set->tbo_ids[binding_index][i], get_gl_format(formats[i]), buffers[i]->id,
                           offsets[i], sizes[i]);
   }
}

static void
gl_set_storage_image_descriptors(api_context *ctx, api_descriptor_set *set, unsigned num_images,
                                 api_image **images)
{
   assert(num_images <= set->layout->desc.storage_image.array_size);

   for (unsigned i = 0; i < num_images; i++)
      set->image_ids[i] = images[i]->id;
}

static void
gl_bind_descriptor_set(api_context *ctx, api_descriptor_set *set)
{
   if (set->layout->desc.uniform_buffer.array_size) {
      glBindBufferRange(GL_UNIFORM_BUFFER, set->layout->desc.uniform_buffer.gl_binding,
                        set->ubo_id, set->ubo_offset, set->ubo_size);
   }

   for (unsigned i = 0; i < MAX_UNIFORM_TEXEL_BUFFER_BINDINGS; i++) {
      if (set->layout->desc.uniform_texel_buffer[i].array_size) {
         glBindTextures(set->layout->desc.uniform_texel_buffer[i].gl_binding,
                        set->layout->desc.uniform_texel_buffer[i].array_size, set->tbo_ids[i]);
      }
   }

   if (set->layout->desc.storage_image.array_size) {
      glBindImageTextures(set->layout->desc.storage_image.gl_binding,
                          set->layout->desc.storage_image.array_size, set->image_ids);
   }
}

static api_pipeline *
gl_create_pipeline(api_context *ctx, const api_pipeline_desc *desc)
{
   api_pipeline *pipeline = calloc(1, sizeof(api_pipeline));
   pipeline->fb = desc->fb;
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
gl_bind_pipeline(api_context *ctx, api_pipeline *pipeline)
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

   if (pipeline->desc.fb->samples > 1)
      glEnable(GL_MULTISAMPLE);
   else
      glDisable(GL_MULTISAMPLE);

   gl_set_current_pipeline_color_depth_masks(ctx);
}

static void
gl_begin_cmdbuf(api_context *ctx)
{
}

static void
gl_end_cmdbuf_and_submit(api_context *ctx)
{
   glFlush();
   gl_check_no_error();
}

static void
gl_begin_render_pass(api_context *ctx, const api_render_pass_desc *desc)
{
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, desc->fb->id);
   glViewport(0, 0, desc->fb->width, desc->fb->height);

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

static void
gl_end_render_pass(api_context *ctx)
{
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static void
gl_bind_vertex_buffers(api_context *ctx, api_buffer *vb, const uint64_t *vb_offsets)
{
   for (unsigned i = 0; i < ctx->current_pipeline->desc.num_vb_desc; i++)
      glBindVertexBuffer(i, vb->id, vb_offsets[i], ctx->current_pipeline->desc.vb_strides[i]);
}

static void
gl_bind_index_buffer(struct api_context *ctx, api_buffer *ib)
{
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->id);
}

static void
gl_draw(api_context *ctx, const api_draw_desc *desc)
{
   if (desc->mesh_shader)
      glDrawMeshTasksEXT(desc->count, 1, 1);
   else if (desc->indexed)
      glDrawElements(ctx->current_pipeline->prim_mode, desc->count, GL_UNSIGNED_INT, NULL);
   else
      glDrawArrays(ctx->current_pipeline->prim_mode, desc->first_vertex, desc->count);
}

static api_timestamp_query_pool *
gl_create_timestamp_pool(api_context *ctx, unsigned num_queries)
{
   api_timestamp_query_pool *pool = calloc(1, sizeof(api_timestamp_query_pool));

   pool->num_written_queries = 0;
   pool->num_queries = num_queries;
   pool->results = calloc(num_queries, sizeof(uint64_t));

   pool->queries = calloc(num_queries, sizeof(GLuint));
   glCreateQueries(GL_TIMESTAMP, num_queries, pool->queries);
   gl_check_no_error();

   return pool;
}

static void
gl_write_next_timestamp(api_context *ctx, api_timestamp_query_pool *pool)
{
   assert(pool->num_written_queries < pool->num_queries);
   glQueryCounter(pool->queries[pool->num_written_queries], GL_TIMESTAMP);
   pool->num_written_queries++;
}

static void
gl_query_timestamps(api_context *ctx, api_timestamp_query_pool *pool)
{
   for (unsigned i = 0; i < pool->num_written_queries; i++)
      glGetQueryObjectui64v(pool->queries[i], GL_QUERY_RESULT, &pool->results[i]);

   gl_check_no_error();
   pool->num_read_queries = 0;
}

static void
gl_image_write_png(api_context *ctx, api_image *image, const char *filename)
{
   uint64_t size = (uint64_t)image->width * image->height * 4;
   void *data = malloc(size);

   glGetTextureImage(image->id, 0, GL_RGBA, GL_UNSIGNED_BYTE, size, data);
   gl_check_no_error();

   write_png_rgba8(filename, image, data);
   free(data);
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
      error("OpenGL 4.6 required");

   printf("Renderer: %s\n", glGetString(GL_RENDERER));

   /* Adjust the initial state. */
   GLuint vao;
   glCreateVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
   glPrimitiveRestartIndex(UINT32_MAX);

   /* Set properties and callbacks. */
   ctx->has_sysmem_uswc = true;
   ctx->has_sysmem_cached = true;
   ctx->timestamp_period_in_seconds = 0.000000001;
   ctx->has_xfb = true;

   if (GLAD_GL_EXT_mesh_shader)
      glGetIntegerv(GL_MAX_MESH_WORK_GROUP_INVOCATIONS_EXT, (int*)&ctx->max_mesh_workgroup_size);

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

   ctx->vram_usage = 0;

   ctx->destroy_context = NULL;

   ctx->create_buffer = gl_create_buffer;
   ctx->upload_buffer_data = gl_upload_buffer_data;
   ctx->clear_buffer = gl_clear_buffer;
   ctx->copy_buffer = gl_copy_buffer;
   ctx->destroy_buffer = NULL;

   ctx->create_image = gl_create_image;
   ctx->image_write_png = gl_image_write_png;
   ctx->destroy_image = NULL;

   ctx->create_framebuffer = gl_create_framebuffer;
   ctx->destroy_framebuffer = NULL;

   ctx->create_shader = gl_create_shader;
   ctx->destroy_shader = NULL;

   ctx->create_descriptor_set_layout = gl_create_descriptor_set_layout;
   ctx->destroy_descriptor_set_layout = NULL;

   ctx->create_descriptor_set = gl_create_descriptor_set;
   ctx->set_uniform_buffer_descriptor = gl_set_uniform_buffer_descriptors;
   ctx->set_uniform_texel_buffer_descriptors = gl_set_uniform_texel_buffer_descriptors;
   ctx->set_storage_image_descriptors = gl_set_storage_image_descriptors;
   ctx->bind_descriptor_set = gl_bind_descriptor_set;
   ctx->destroy_descriptor_set = NULL;

   ctx->create_pipeline = gl_create_pipeline;
   ctx->bind_pipeline = gl_bind_pipeline;
   ctx->destroy_pipeline = NULL;

   ctx->begin_cmdbuf = gl_begin_cmdbuf;
   ctx->end_cmdbuf_and_submit = gl_end_cmdbuf_and_submit;

   ctx->begin_render_pass = gl_begin_render_pass;
   ctx->end_render_pass = gl_end_render_pass;

   ctx->bind_vertex_buffers = gl_bind_vertex_buffers;
   ctx->bind_index_buffer = gl_bind_index_buffer;
   ctx->draw = gl_draw;

   ctx->create_timestamp_pool = gl_create_timestamp_pool;
   ctx->write_next_timestamp = gl_write_next_timestamp;
   ctx->query_timestamps = gl_query_timestamps;

   ctx->current_pipeline = NULL;

   return ctx;
}
