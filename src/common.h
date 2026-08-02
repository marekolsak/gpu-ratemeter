/* Copyright 2026 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

/* This is a central and only header file of the project defining the API abstraction/interface
 * that all tests use. The interface is modeled around Vulkan concepts and uses Vulkan enums where
 * convenient.
 *
 * GL_PRIVATE, VK_PRIVATE, and similar #ifdef sections contain private members of the respective
 * API backends.
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

#define ARRAY_SIZE(x)               (sizeof(x) / sizeof(x[0]))
#define ALIGN_POT(x, pot_align)     (((x) + (pot_align) - 1) & ~((pot_align) - 1))
#define ALIGN_NPOT(x, npot_align)   (((x) + (npot_align) - 1) / (npot_align) * (npot_align))
#define IS_POT(v)                   (((v) & ((v) - 1)) == 0)
#define MIN2(a, b)                  ((a) < (b) ? (a) : (b))
#define MAX2(a, b)                  ((a) > (b) ? (a) : (b))

/* Return a base 2 logarithm of a 32-bit power of two as a constant expression if n is a constant
 * expression.
 */
#define LOG2_POT(n) ( \
    ((!!((n) & 0xAAAAAAAAu)) << 0) | \
    ((!!((n) & 0xCCCCCCCCu)) << 1) | \
    ((!!((n) & 0xF0F0F0F0u)) << 2) | \
    ((!!((n) & 0xFF00FF00u)) << 3) | \
    ((!!((n) & 0xFFFF0000u)) << 4))

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
   api_heap_device_coherent_amd,
   api_heap_host_uncached,
   api_heap_host_uncached_coherent_amd,
   api_heap_host_cached,
   api_num_heaps,
} api_heap_type;

typedef struct {
   int x, y, z;
   int width, height, depth;
} api_image_box;

typedef struct {
   uint64_t size;
   api_heap_type heap;
   unsigned sparse_block_size;
   uint64_t device_address;

#ifdef GL_PRIVATE
   GLuint id;
#endif

#ifdef VK_PRIVATE
   VkBuffer buffer;
   VkDeviceMemory *mem;
   unsigned num_mem_allocations;
   VkSparseMemoryBind *sparse_binds;
   VkSparseMemoryBind *sparse_unbinds;
#endif
} api_buffer;

typedef struct {
   VkImageType type;
   unsigned width;
   unsigned height;
   unsigned depth;
   unsigned samples;
   VkFormat format;
   api_heap_type heap;

#ifdef GL_PRIVATE
   GLuint id;
   GLenum gltarget;
   GLenum glinternalformat;
   GLenum glformat;
   GLenum gltype;
   uint64_t mem_size;
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
   unsigned view_mask;
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
#define MAX_STORAGE_BUFFER_BINDINGS             2
#define MAX_STORAGE_BUFFER_ARRAY_SIZE           1
#define MAX_UNIFORM_TEXEL_BUFFER_BINDINGS       5
#define MAX_UNIFORM_TEXEL_BUFFER_ARRAY_SIZE     8
#define MAX_COMBINED_IMAGE_SAMPLER_BINDINGS     1
#define MAX_COMBINED_IMAGE_SAMPLER_ARRAY_SIZE   1
#define MAX_STORAGE_IMAGE_BINDINGS              1
#define MAX_STORAGE_IMAGE_ARRAY_SIZE            1

/* This is really just a binding layout, not a descriptor layout. */
typedef struct {
   api_descriptor_binding uniform_buffer[MAX_UNIFORM_BUFFER_BINDINGS];
   api_descriptor_binding storage_buffer[MAX_STORAGE_BUFFER_BINDINGS];
   api_descriptor_binding uniform_texel_buffer[MAX_UNIFORM_TEXEL_BUFFER_BINDINGS];
   api_descriptor_binding combined_image_sampler[MAX_COMBINED_IMAGE_SAMPLER_BINDINGS];
   api_descriptor_binding storage_image[MAX_STORAGE_IMAGE_BINDINGS];
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

   GLuint ssbo_id;
   GLintptr ssbo_offset;
   GLsizeiptr ssbo_size;

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
   shaderc_compilation_result_t spirv;
   VkShaderModuleCreateInfo module_info;
   VkPipelineShaderStageCreateInfo stage_info;
#endif
} api_shader;

#define MAX_VERTEX_BUFFERS 11

typedef struct {
   VkPrimitiveTopology topology;
   bool primitive_restart;

   VkCullModeFlags cull_mode;
   unsigned clipdist_enable_mask; /* GL only */
   bool rasterizer_discard;
   VkPolygonMode polygon_mode;

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
   VkPipeline lib_vi, lib_prerast, lib_fs, lib_out;
   VkPipeline pipeline;
   VkVertexInputBindingDescription2EXT dyn_vi_bindings[MAX_VERTEX_BUFFERS];
   VkVertexInputAttributeDescription2EXT dyn_vi_attribs[MAX_VERTEX_BUFFERS];
   VkPipelineColorBlendAttachmentState blend_state;
   VkPipelineFragmentShadingRateStateCreateInfoKHR vrs;
#endif
} api_pipeline;

typedef struct {
#ifdef GL_PRIVATE
   GLuint prog;
#endif

#ifdef VK_PRIVATE
   VkPipeline pipeline;
#endif
} api_compute_pipeline;

typedef struct {
   bool indexed;
   bool mesh_shader;
   unsigned count;
   unsigned instance_count;
   unsigned first_vertex;
} api_draw_desc;

typedef enum {
   api_query_timestamp,
   api_query_pipeline_statistics,
} api_query_type;

typedef struct {
   uint64_t ia_vertices;
   uint64_t ia_primitives;
   uint64_t vs_invocations;
   uint64_t gs_invocations;
   uint64_t gs_primitives;
   uint64_t clip_invocations;
   uint64_t clip_primitives;
   uint64_t fs_invocations;
   uint64_t tcs_invocations;
   uint64_t tes_invocations;
   uint64_t cs_invocations;
} api_pipeline_stat_results;

typedef struct {
   api_query_type type;
   unsigned num_queries;
   unsigned num_written_queries;
   unsigned num_read_queries;

   union {
      uint64_t *results;
      api_pipeline_stat_results *pipe_stats;
   };

#ifdef GL_PRIVATE
   unsigned num_values;
   GLenum *gltargets;
   GLuint *queries;
#endif

#ifdef VK_PRIVATE
   VkQueryPool pool;
#endif
} api_query_pool;

typedef struct {
   api_framebuffer *fb;
   bool clear;
   VkClearColorValue color_clear_value;
   float depth_clear_value;
} api_render_pass_desc;

typedef struct {
#ifdef VK_PRIVATE
   VkSemaphore semaphore;
   uint64_t timeline_point;
#endif
} api_fence;

#define MAX_COMMAND_BUFFERS   1024

typedef enum {
   API_VK_DYNAMIC_STATE = 1 << 0,
   API_VK_GPL = 1 << 1,
} api_flags;

typedef struct {
   /* API and test options. */
   api_flags api_flags;
   bool report_bandwidth;

   /* Bool options.*/
   bool bda;
   bool compute;
   bool gl_tiling_linear;
   bool int8;
   bool lean;
   bool no_validator;
   bool rdna4_timestamp_wa;
   bool samplerate;
   bool sparse_bound;
   bool sparse_unbound;
   bool transfer;

   /* Uint options. */
   unsigned clock_bits;
   unsigned device; /* device index */
   unsigned freq_mhz;
   unsigned max_rate;
   unsigned max_valid_result;

   /* String options. */
   const char *filter;
   const char *format;
   const char *subset;

   /* Memory size options. */
   unsigned spacing;
   uint64_t max_size;
} program_options;

typedef enum {
   WA_RDNA4_TIMESTAMP_BUG,
} driver_wa;

typedef enum {
   api_queue_gfx,
   api_queue_compute,
   api_queue_transfer,
   api_queue_sparse,
   api_num_queues,
} api_queue_type;

#define api_wait_gfx          (1 << api_queue_gfx)
#define api_wait_compute      (1 << api_queue_compute)
#define api_wait_transfer     (1 << api_queue_transfer)
#define api_wait_sparse       (1 << api_queue_sparse)
#define api_wait_all_queues   ((1 << api_num_queues) - 1)

#define FORMAT_COUNT (VK_FORMAT_D32_SFLOAT_S8_UINT + 1)

typedef struct api_context {
   program_options options;

   /* Core properties. */
   bool has_heap[api_num_heaps];
   bool has_queue[api_num_queues];
   bool allow_parallel_create_shader;
   bool allow_parallel_create_pipeline;
   double timestamp_period_in_seconds;

   /* Feature properties. */
   bool has_blit_image_3d;
   bool has_blit_image_msaa;
   bool has_buffer_device_address;
   bool has_clear_image_region;
   bool has_fully_covered;
   bool has_image_tiling_linear;
   bool has_multiview;
   bool has_resolve_image_yflip;
   bool has_sparse_buffer;
   bool has_shader_int8;
   bool has_shader_int64;
   bool has_shader_subgroup_clock;
   bool has_shader_subgroup_ops;
   bool has_vrs;
   bool has_vs_tes_layer_output;
   bool has_xfb;
   unsigned max_mesh_workgroup_size; /* 0 = unsupported */
   unsigned max_storage_buffer_range;
   unsigned max_uniform_buffer_range;
   unsigned sparse_buffer_alignment;
   VkSampleCountFlags supported_color_sample_counts;
   VkSampleCountFlags fb_format_sample_count_support[FORMAT_COUNT];

   /* Dynamic info. */
   uint32_t device_mem_usage_mb;

   /* Functions. */
   void (*destroy_context)(struct api_context *ctx);

   api_buffer *(*create_buffer)(struct api_context *ctx, uint64_t size, api_heap_type heap,
                                unsigned sparse_block_size);
   void (*destroy_buffer)(struct api_context *ctx, api_buffer *buffer);
   void (*upload_buffer_data)(struct api_context *ctx, api_buffer *buf, uint64_t offset,
                              uint64_t size, const void *data);
   void (*get_buffer_data)(struct api_context *ctx, api_buffer *buf, uint64_t offset,
                           uint64_t size, void *data);
   void (*clear_buffer)(struct api_context *ctx, api_buffer *buf, uint64_t offset, uint64_t size,
                        uint32_t value);
   void (*copy_buffer)(struct api_context *ctx, api_buffer *dst, api_buffer *src,
                       uint64_t dst_offset, uint64_t src_offset, uint64_t size);
   void (*buffer_bind_sparse)(struct api_context *ctx, api_buffer *buf, uint64_t offset,
                              uint64_t size, bool bind, api_queue_type queue,
                              api_fence *wait_fence, api_fence **signal_fence);

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
                                          unsigned width, unsigned height, unsigned samples,
                                          unsigned view_mask);
   void (*destroy_framebuffer)(struct api_context *ctx, api_framebuffer *fb);

   api_shader *(*create_shader)(struct api_context *ctx, const char *source, api_shader_type type);
   void (*destroy_shader)(struct api_context *ctx, api_shader *shader);

   api_descriptor_set_layout *(*create_descriptor_set_layout)(struct api_context *ctx,
                                                              const api_descriptor_set_layout_desc *desc);
   void (*destroy_descriptor_set_layout)(struct api_context *ctx, api_descriptor_set_layout *layout);

   api_descriptor_set *(*create_descriptor_set)(struct api_context *ctx, api_descriptor_set_layout *layout);
   void (*set_uniform_buffer_descriptor)(struct api_context *ctx, api_descriptor_set *set,
                                         unsigned binding_index, api_buffer *buffer, uint64_t offset,
                                         uint64_t size);
   void (*set_storage_buffer_descriptor)(struct api_context *ctx, api_descriptor_set *set,
                                         unsigned binding_index, api_buffer *buffer, uint64_t offset,
                                         uint64_t size);
   void (*set_uniform_texel_buffer_descriptors)(struct api_context *ctx, api_descriptor_set *set,
                                                unsigned binding_index, unsigned num_buffers,
                                                api_buffer **buffers, VkFormat *formats,
                                                uint64_t *offsets, uint64_t *sizes);
   void (*set_combined_image_sampler_descriptors)(struct api_context *ctx, api_descriptor_set *set,
                                                  unsigned binding_index, unsigned num_samplers,
                                                  api_image **images);
   void (*set_storage_image_descriptors)(struct api_context *ctx, api_descriptor_set *set,
                                         unsigned binding_index, unsigned num_images,
                                         api_image **images);
   void (*destroy_descriptor_set)(struct api_context *ctx, api_descriptor_set *set);
   void (*bind_descriptor_set)(struct api_context *ctx, api_descriptor_set *set);

   api_pipeline *(*create_pipeline)(struct api_context *ctx, const api_pipeline_desc *desc);
   void (*destroy_pipeline)(struct api_context *ctx, api_pipeline *pipeline);
   void (*bind_pipeline)(struct api_context *ctx, api_pipeline *pipeline);

   api_compute_pipeline *(*create_compute_pipeline)(struct api_context *ctx, api_shader *shader,
                                                    api_descriptor_set_layout *layout);
   void (*destroy_compute_pipeline)(struct api_context *ctx, api_compute_pipeline *pipeline);
   void (*bind_compute_pipeline)(struct api_context *ctx, api_compute_pipeline *pipeline);
   void (*dispatch)(struct api_context *ctx, unsigned num_x, unsigned num_y, unsigned num_z);
   void (*barrier_buffers)(struct api_context *ctx, unsigned num_buffers, api_buffer **buffers,
                           uint64_t *offset_size_pairs, bool after_shader_writes);

   void (*begin_cmdbuf)(struct api_context *ctx, api_queue_type queue);
   void (*end_cmdbuf_and_submit)(struct api_context *ctx, unsigned wait_queue_mask,
                                 api_fence *wait_fence, api_fence **signal_fence);
   void (*wait_for_idle)(struct api_context *ctx);

   void (*begin_render_pass)(struct api_context *ctx, const api_render_pass_desc *desc);
   void (*end_render_pass)(struct api_context *ctx);

   void (*bind_vertex_buffers)(struct api_context *ctx, api_buffer *vb, const uint64_t *vb_offsets);
   void (*bind_index_buffer)(struct api_context *ctx, api_buffer *ib);
   void (*draw)(struct api_context *ctx, const api_draw_desc *desc);

   void (*driver_workaround)(struct api_context *ctx, driver_wa wa);

   api_query_pool *(*create_query_pool)(struct api_context *ctx, unsigned num_queries,
                                        api_query_type type);
   void (*begin_next_query)(struct api_context *ctx, api_query_pool *pool);
   void (*end_next_query)(struct api_context *ctx, api_query_pool *pool);
   void (*write_next_query_value)(struct api_context *ctx, api_query_pool *pool);
   void (*get_query_results)(struct api_context *ctx, api_query_pool *pool);

   /* Private members. */
#ifdef GL_PRIVATE
   api_pipeline *current_pipeline;
   api_framebuffer *fb;
   api_framebuffer *prev_fb;
#endif

#ifdef VK_PRIVATE
   api_pipeline *current_pipeline;

   /* Device. */
   unsigned num_extensions;
   VkExtensionProperties *extensions;
   VkPhysicalDeviceMemoryProperties memory_properties;
   VkDevice device;
   VkQueue queue[api_num_queues];
   VkCommandPool cmd_pool[api_num_queues];

   /* Extension functions. */
   PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT;
   PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT;
   PFN_vkCmdSetPolygonModeEXT vkCmdSetPolygonModeEXT;
   PFN_vkCmdSetRasterizationSamplesEXT vkCmdSetRasterizationSamplesEXT;
   PFN_vkCmdSetSampleMaskEXT vkCmdSetSampleMaskEXT;
   PFN_vkCmdSetAlphaToCoverageEnableEXT vkCmdSetAlphaToCoverageEnableEXT;
   PFN_vkCmdSetColorBlendEnableEXT vkCmdSetColorBlendEnableEXT;
   PFN_vkCmdSetColorBlendEquationEXT vkCmdSetColorBlendEquationEXT;
   PFN_vkCmdSetColorWriteMaskEXT vkCmdSetColorWriteMaskEXT;
   PFN_vkCmdSetFragmentShadingRateKHR vkCmdSetFragmentShadingRateKHR;

   /* Command buffers. */
   VkCommandBuffer cmd_buffers[api_num_queues][MAX_COMMAND_BUFFERS];
   VkSemaphore semaphore[api_num_queues];
   uint64_t timeline_point[api_num_queues];
   api_queue_type current_queue;
   VkCommandBuffer current_cmd_buffer;

   /* Descriptor pool. */
   VkDescriptorPool descriptor_pool;
   unsigned num_allocated_desc_sets;
   VkPipelineLayout empty_pipeline_layout;

   /* The GLSL compiler. */
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
void test_bufbw(api_context *ctx, const char *test_name);
void test_imgbw(api_context *ctx, const char *test_name);
void test_latency(api_context *ctx, const char *test_name);
void test_pix(api_context *ctx, const char *test_name);
void test_prim(api_context *ctx, const char *test_name);
void test_sanity(api_context *ctx, const char *test_name);
void test_sparsebind(api_context *ctx, const char *test_name);

/* utils.c */
bool check_filter_string(const char *filter_string, const char *name);
double get_time_in_seconds_from_timestamps(api_context *ctx, api_query_pool *pool);
void print_throughput_from_next_timestamps(api_context *ctx, api_query_pool *pool,
                                           uint64_t num_units, const char *rate_format,
                                           const char *bandwidth_format, const char *string_format,
                                           unsigned bandwidth_exp2_divisor);
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
bool format_is_valid(VkFormat format);
unsigned get_next_power_of_two(unsigned x);
uint16_t float_to_half(float val);
unsigned bitcount(unsigned n);
unsigned logbase2(unsigned n);
const char *heap_to_string(api_heap_type heap);
const char *queue_to_string(api_queue_type queue);

#ifdef __cplusplus
}
#endif

#endif
