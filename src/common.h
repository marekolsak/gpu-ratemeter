/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdnoreturn.h>

#ifndef VK_PRIVATE
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define ALIGN_POT(x, pot_align) (((x) + (pot_align) - 1) & ~((pot_align) - 1))
#define ALIGN_NPOT(x, npot_align) (((x) + (npot_align) - 1) / (npot_align) * (npot_align))
#define IS_POT(v) (((v) & ((v) - 1)) == 0)
#define MIN2( A, B )   ( (A)<(B) ? (A) : (B) )

typedef enum {
   api_shader_vs,
   api_shader_tcs,
   api_shader_tes,
   api_shader_gs,
   api_shader_fs,
   api_shader_cs,
   api_shader_ts,
   api_shader_ms,
} api_shader_type;

typedef enum {
   api_heap_device,
   api_heap_host_uncached,
   api_heap_host_cached,
} api_heap_type;

typedef struct {
   int x, y, z;
   int width, height, depth;
} api_image_box;

typedef struct {
   uint64_t size;
   api_heap_type heap;

#ifdef GL_PRIVATE
   GLuint id;
#endif

#ifdef VK_PRIVATE
   VkBuffer buffer;
   VkDeviceMemory mem;
#endif
} api_buffer;

typedef struct {
   VkImageType type;
   unsigned width;
   unsigned height;
   unsigned depth;
   unsigned samples;
   VkFormat format;

#ifdef GL_PRIVATE
   GLuint id;
   GLenum gltarget;
   GLenum glinternalformat;
   GLenum glformat;
   GLenum gltype;
#endif

#ifdef VK_PRIVATE
   VkImage image;
   VkDeviceMemory mem;
   uint64_t mem_size;
   unsigned layer_count;
   VkImageView render_compatible_view; /* 3D images use 2D array views */
   VkImageLayout layout;
#endif
} api_image;

typedef struct {
   api_image *dst;
   api_image *src;
   api_image_box dst_box;
   api_image_box src_box;
   bool is_copy; /* whether to use a copy, i.e. no flipping/scaling/format conversions */
   bool linear_filter;
} api_blit_desc;

typedef struct {
   unsigned width;
   unsigned height;
   unsigned samples;
   api_image *colorbuf;
   api_image *zbuf;

#ifdef GL_PRIVATE
   GLuint id;
#endif

#ifdef VK_PRIVATE
   VkRenderPass render_pass[2];  /* 0=no clear, 1=clear */
   VkFramebuffer fb[2];          /* 0=no clear, 1=clear */
   unsigned num_attachments;
   unsigned colorbuf_att_index;
   unsigned zbuf_att_index;
#endif
} api_framebuffer;

typedef struct {
   unsigned gl_binding;
   unsigned vk_binding;
   unsigned array_size;
} api_descriptor_binding;

#define MAX_DESCRIPTOR_SETS                     1024
#define MAX_UNIFORM_BUFFER_BINDINGS             1
#define MAX_UNIFORM_BUFFER_ARRAY_SIZE           1
#define MAX_UNIFORM_TEXEL_BUFFER_BINDINGS       5
#define MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE     8
#define MAX_COMBINED_IMAGE_SAMPLER_ARRAY_SIZE   1
#define MAX_STORAGE_IMAGE_BINDINGS              1
#define MAX_STORAGE_IMAGE_ARRAY_SIZE            1

/* This is really just a binding layout, not a descriptor layout. */
typedef struct {
   api_descriptor_binding uniform_buffer;
   api_descriptor_binding uniform_texel_buffer[MAX_UNIFORM_TEXEL_BUFFER_BINDINGS];
   api_descriptor_binding combined_image_sampler;
   api_descriptor_binding storage_image;
} api_descriptor_set_layout_desc;

typedef struct {
   api_descriptor_set_layout_desc desc;

#ifdef VK_PRIVATE
   VkDescriptorSetLayout desc_set_layout;
   VkPipelineLayout pipeline_layout;
#endif
} api_descriptor_set_layout;

typedef struct {
   api_descriptor_set_layout *layout;

#ifdef GL_PRIVATE
   GLuint ubo_id;
   GLintptr ubo_offset;
   GLsizeiptr ubo_size;
   GLuint tbo_ids[MAX_UNIFORM_TEXEL_BUFFER_BINDINGS][MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE];
   GLuint tex_ids[MAX_COMBINED_IMAGE_SAMPLER_ARRAY_SIZE];
   GLuint image_ids[MAX_STORAGE_IMAGE_ARRAY_SIZE];
#endif

#ifdef VK_PRIVATE
   VkDescriptorSet set;
   VkBufferView texel_buffer_views[MAX_UNIFORM_TEXEL_BUFFER_BINDINGS][MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE];
#endif
} api_descriptor_set;

typedef struct {
#ifdef GL_PRIVATE
   GLuint id;
#endif

#ifdef VK_PRIVATE
   VkShaderModule module;
#endif
} api_shader;

#define MAX_VERTEX_BUFFERS 11

typedef struct {
   VkPrimitiveTopology topology;
   bool primitive_restart;
   VkCullModeFlags cull_mode;
   unsigned clipdist_enable_mask;
   bool rasterizer_discard;

   unsigned num_vb_desc;
   unsigned vb_strides[MAX_VERTEX_BUFFERS];
   VkFormat vb_formats[MAX_VERTEX_BUFFERS];

   api_descriptor_set_layout *desc_set_layout;
   api_shader *ms;
   api_shader *vs;
   api_shader *fs;

   uint8_t vrs_fragment_size[2];
   bool sample_shading;
   unsigned samplemask;
   bool depth_enabled;
   bool depth_write_enabled;
   VkCompareOp depth_compare_op;
   bool alpha_to_coverage;
   uint8_t colormask;
   bool blend_src_color;
   bool blend_src_alpha;
   api_framebuffer *fb;
} api_pipeline_desc;

typedef struct {
   api_pipeline_desc desc;

#ifdef GL_PRIVATE
   GLenum prim_mode;
   GLuint prog;
#endif

#ifdef VK_PRIVATE
   unsigned num_vb_desc;
   VkPipeline pipeline;
   VkVertexInputBindingDescription2EXT dyn_vi_bindings[MAX_VERTEX_BUFFERS];
   VkVertexInputAttributeDescription2EXT dyn_vi_attribs[MAX_VERTEX_BUFFERS];
   VkPipelineColorBlendAttachmentState blend_state;
   VkPipelineFragmentShadingRateStateCreateInfoKHR vrs;
#endif
} api_pipeline;

typedef struct {
   bool indexed;
   bool mesh_shader;
   unsigned count;
   unsigned instance_count;
   unsigned first_vertex;
} api_draw_desc;

typedef struct {
   unsigned num_queries;
   unsigned num_written_queries;
   unsigned num_read_queries;
   uint64_t *results;

#ifdef GL_PRIVATE
   GLuint *queries;
#endif

#ifdef VK_PRIVATE
   VkQueryPool pool;
#endif
} api_timestamp_query_pool;

typedef struct {
   api_framebuffer *fb;
   bool clear;
   VkClearColorValue color_clear_value;
   float depth_clear_value;
} api_render_pass_desc;

#define MAX_COMMAND_BUFFERS   1024

enum {
   API_VK_CORE,
   API_VK_DYNAMIC_STATE,
};

typedef struct {
   /* API and test options. */
   unsigned api_flavor;
   bool report_bandwidth;

   /* Bool options.*/
   bool lean;
   bool rdna4_timestamp_wa;

   /* Uint options. */
   unsigned freq_mhz;
   unsigned max_rate;

   /* String options. */
   const char *test_filter;
   const char *format_filter;
} program_options;

typedef struct api_context {
   program_options options;

   /* Properties. */
   bool has_host_uncached_heap;
   bool has_host_cached_heap;
   bool has_image_tiling_linear;
   double timestamp_period_in_seconds;
   unsigned max_mesh_workgroup_size; /* 0 = unsupported */
   bool has_vrs;
   bool has_xfb;
   bool has_clear_image_region;
   bool has_blit_image_3d;
   bool has_blit_image_msaa;
   bool has_resolve_image_yflip;
   VkSampleCountFlags supported_color_sample_counts;

   /* Dynamic info. */
   uint64_t device_mem_usage;

   /* Functions. */
   void (*destroy_context)(struct api_context *ctx);

   api_buffer *(*create_buffer)(struct api_context *ctx, uint64_t size, api_heap_type heap);
   void (*destroy_buffer)(struct api_context *ctx, api_buffer *buffer);
   void (*upload_buffer_data)(struct api_context *ctx, api_buffer *buf, uint64_t offset,
                              uint64_t size, const void *data);
   void (*clear_buffer)(struct api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                        uint32_t value);
   void (*copy_buffer)(struct api_context *ctx, api_buffer *dst, api_buffer *src,
                       uint64_t dst_offset, uint64_t src_offset, uint64_t size);

   api_image *(*create_image)(struct api_context *ctx, VkImageType type, VkFormat format,
                              unsigned width, unsigned height, unsigned depth, unsigned samples,
                              VkImageTiling tiling, api_heap_type heap);
   void (*destroy_image)(struct api_context *ctx, api_image *image);
   void (*clear_image)(struct api_context *ctx, api_image *image, const api_image_box *box,
                       const VkClearColorValue *value);
   void (*blit_image)(struct api_context *ctx, api_blit_desc *desc);
   void (*upload_image_data)(struct api_context *ctx, api_image *image, unsigned stride_in_bytes,
                             void *data);
   void (*image_write_png)(struct api_context *ctx, api_image *image, unsigned layer,
                           const char *filename);

   api_framebuffer *(*create_framebuffer)(struct api_context *ctx, api_image *colorbuf, api_image *zbuf,
                                          unsigned width, unsigned height, unsigned samples);
   void (*destroy_framebuffer)(struct api_context *ctx, api_framebuffer *fb);

   api_shader *(*create_shader)(struct api_context *ctx, const char *source, api_shader_type type);
   void (*destroy_shader)(struct api_context *ctx, api_shader *shader);

   api_descriptor_set_layout *(*create_descriptor_set_layout)(struct api_context *ctx,
                                                              const api_descriptor_set_layout_desc *desc);
   void (*destroy_descriptor_set_layout)(struct api_context *ctx, api_descriptor_set_layout *layout);

   api_descriptor_set *(*create_descriptor_set)(struct api_context *ctx, api_descriptor_set_layout *layout);
   void (*set_uniform_buffer_descriptor)(struct api_context *ctx, api_descriptor_set *set,
                                         api_buffer *buffer, uint64_t offset, uint64_t size);
   void (*set_uniform_texel_buffer_descriptors)(struct api_context *ctx, api_descriptor_set *set,
                                                unsigned binding_index, unsigned num_buffers,
                                                api_buffer **buffers, VkFormat *formats,
                                                uint64_t *offsets, uint64_t *sizes);
   void (*set_combined_image_sampler_descriptors)(struct api_context *ctx, api_descriptor_set *set,
                                                  unsigned num_samplers, api_image **images);
   void (*set_storage_image_descriptors)(struct api_context *ctx, api_descriptor_set *set,
                                         unsigned num_images, api_image **images);
   void (*destroy_descriptor_set)(struct api_context *ctx, api_descriptor_set *set);
   void (*bind_descriptor_set)(struct api_context *ctx, api_descriptor_set *set);

   api_pipeline *(*create_pipeline)(struct api_context *ctx, const api_pipeline_desc *desc);
   void (*destroy_pipeline)(struct api_context *ctx, api_pipeline *pipeline);
   void (*bind_pipeline)(struct api_context *ctx, api_pipeline *pipeline);

   void (*begin_cmdbuf)(struct api_context *ctx);
   void (*end_cmdbuf_and_submit)(struct api_context *ctx);
   void (*wait_idle_before_deallocation)(struct api_context *ctx);

   void (*begin_render_pass)(struct api_context *ctx, const api_render_pass_desc *desc);
   void (*end_render_pass)(struct api_context *ctx);

   void (*bind_vertex_buffers)(struct api_context *ctx, api_buffer *vb, const uint64_t *vb_offsets);
   void (*bind_index_buffer)(struct api_context *ctx, api_buffer *ib);
   void (*draw)(struct api_context *ctx, const api_draw_desc *desc);
   void (*pipeline_barrier)(struct api_context *ctx, VkPipelineStageFlagBits2 stage_bits);

   api_timestamp_query_pool *(*create_timestamp_pool)(struct api_context *ctx, unsigned num_queries);
   void (*write_next_timestamp)(struct api_context *ctx, api_timestamp_query_pool *pool);
   void (*query_timestamps)(struct api_context *ctx, api_timestamp_query_pool *pool);

   /* Private members. */
#ifdef GL_PRIVATE
   api_pipeline *current_pipeline;
   api_framebuffer *fb;
   api_framebuffer *prev_fb;
#endif

#ifdef VK_PRIVATE
   api_pipeline *current_pipeline;

   /* Device. */
   VkPhysicalDeviceMemoryProperties memory_properties;
   VkDevice device;
   VkQueue queue;

   /* Extension functions. */
   PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT;
   PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT;
   PFN_vkCmdSetRasterizationSamplesEXT vkCmdSetRasterizationSamplesEXT;
   PFN_vkCmdSetSampleMaskEXT vkCmdSetSampleMaskEXT;
   PFN_vkCmdSetAlphaToCoverageEnableEXT vkCmdSetAlphaToCoverageEnableEXT;
   PFN_vkCmdSetColorBlendEnableEXT vkCmdSetColorBlendEnableEXT;
   PFN_vkCmdSetColorBlendEquationEXT vkCmdSetColorBlendEquationEXT;
   PFN_vkCmdSetColorWriteMaskEXT vkCmdSetColorWriteMaskEXT;
   PFN_vkCmdSetFragmentShadingRateKHR vkCmdSetFragmentShadingRateKHR;

   /* Command buffers. */
   VkCommandPool cmd_buffer_pool;
   VkCommandBuffer cmd_buffers[MAX_COMMAND_BUFFERS];
   VkSemaphore timeline_semaphore;
   uint64_t current_timeline_point;
   VkCommandBuffer current_cmd_buffer;

   /* Descriptor pool. */
   VkDescriptorPool descriptor_pool;
   unsigned num_allocated_desc_sets;
   VkPipelineLayout empty_pipeline_layout;

   /* Compiler. */
   shaderc_compiler_t glsl_compiler;
   shaderc_compile_options_t glsl_compiler_options;
#endif
} api_context;

#define printflike(a, b) __attribute__((format(printf, (a), (b))))

/* static_assert that can be used inside expressions. */
#define STATIC_ASSERT_EXPR(cond) (sizeof(struct { _Static_assert(cond, ""); }))

/* APIs. */
api_context *d3d11_create_context(const program_options *options);
api_context *d3d12_create_context(const program_options *options);
api_context *gl_create_context(const program_options *options);
api_context *vk_create_context(const program_options *options);

/* Tests. */
void test_buf_bandwidth(api_context *ctx, const char *test_suite_name);
void test_img_bandwidth(api_context *ctx, const char *test_suite_name);
void test_pix_rate(api_context *ctx, const char *test_suite_name);
void test_prim_rate(api_context *ctx, const char *test_suite_name);
void test_sanity(api_context *ctx, const char *test_suite_name);

/* utils.c */
bool check_filter_string(const char *filter_string, const char *name);
void print_throughput_from_next_timestamps(api_context *ctx, api_timestamp_query_pool *pool,
                                           uint64_t num_units, const char *rate_format,
                                           const char *bandwidth_format);
noreturn void error(const char *format, ...) printflike(1, 2);
char *strdup(const char *s);
void print_progress(unsigned num_items, unsigned *num_processed_items, unsigned print_period);
void write_png_rgba8(const char *path, api_image *image_info, uint8_t *pixels);
void run_image_viewer(const char *image_filename);
unsigned get_pixel_size_from_format(VkFormat format);
bool format_is_integer(VkFormat format);
bool format_is_sint(VkFormat format);
unsigned format_get_num_channels(VkFormat format);
bool format_is_depth_or_stencil(VkFormat format);
unsigned get_next_power_of_two(unsigned x);
uint16_t float_to_half(float val);

#ifdef __cplusplus
}
#endif

#endif
