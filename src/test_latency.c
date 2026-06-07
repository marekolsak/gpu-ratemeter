/* Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "common.h"

#define UNROLL_LOOP        0
#define MIN_MEM_SIZE       1024

static api_shader *
create_memory_offset_chasing_cs(api_context *ctx, bool uniform, unsigned num_indirections,
                                unsigned spacing, unsigned max_size)
{
   int max_len = 1100 + 44 * num_indirections;
   int len = 0;
   char *source = (char*)malloc(max_len);

   len += snprintf(source + len, MAX2(max_len - len, 0),
                   "#version 450 \n"
                   "#extension GL_ARB_shader_clock : require \n"
                   "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require \n"
                   "\n"

                   "#define UNIFORM %s \n"
                   "layout(local_size_x = UNIFORM ? 1 : 2, local_size_y = 1, local_size_z = 1) in; \n"
                   "\n"

                   "layout(set = 0, binding = 0, std430) readonly restrict buffer B0 { \n"
                   "   uint offsets[%u]; \n"
                   "} jumpbuf; \n"
                   "\n"

                   "layout(set = 0, binding = 1, std430) buffer B1 { \n"
                   "   uint64_t clock_cycles; \n"
                   "   uint64_t last_offset; \n"
                   "} result; \n"
                   "\n"

                   "void main() { \n"
                   "   uint start_offset = UNIFORM ? 0 : gl_LocalInvocationID.x * %u; \n"
                   "   uint offset = jumpbuf.offsets[start_offset / 4].x; \n"
                   "\n"

                   /* Warm up the caches by executing all indirections or until the buffer wraps
                    * around, whichever is less. This caches as much data as the caches can hold.
                    */
                   "   offset = jumpbuf.offsets[start_offset / 4].x; \n"
                   "   for (int i = 0; i < %u; i++) { \n"
                   "      uint prev_offset = offset; \n"
                   "      offset = jumpbuf.offsets[offset / 4].x; \n"
                   "      if (offset < prev_offset) \n"
                   "         break; \n"
                   "   } \n"
                   "\n"

                   "   offset = jumpbuf.offsets[start_offset / 4].x; \n"
                   "   uint64_t start_cycles = clockARB(); \n"
                   "\n",
                   uniform ? "true" : "false", max_size / 4, spacing, num_indirections);

#if UNROLL_LOOP
   for (unsigned i = 0; i < num_indirections; i++) {
      len += snprintf(source + len, MAX2(max_len - len, 0),
                      "   offset = jumpbuf.offsets[offset / 4].x; \n");
   }
#else
   len += snprintf(source + len, MAX2(max_len - len, 0),
                   "   for (int i = 0; i < %u; i++) \n"
                   "      offset = jumpbuf.offsets[offset / 4].x; \n", num_indirections);
#endif

   len += snprintf(source + len, MAX2(max_len - len, 0),
                   "   result.clock_cycles = clockARB() - start_cycles; \n"

                   /* Also store the offset to make the indirections not dead. */
                   "   result.last_offset = offset; \n"
                   "} \n");
   assert(len < max_len);

   api_shader *cs = ctx->create_shader(ctx, source, api_shader_cs);
   free(source);
   return cs;
}

static void
set_jump_buffer_data(api_context *ctx, api_buffer *buf, unsigned spacing, unsigned size, uint32_t *tmp)
{
   assert(spacing % 4 == 0);
   assert(size % spacing == 0);

   memset(tmp, 0, size);
   unsigned num_steps = size / spacing;

   for (unsigned i = 0; i < num_steps - 1; i++)
      tmp[i * spacing / 4] = (i + 1) * spacing;

   /* Close the circle. */
   tmp[(num_steps - 1) * spacing / 4] = 0;

   ctx->upload_buffer_data(ctx, buf, 0, size, tmp);
}

typedef enum {
   INIT,
   RUN,
   REPORT,
} test_stage;

typedef struct {
   unsigned num_indirections;
   unsigned num_tests;

   api_descriptor_set_layout *layout;
   api_compute_pipeline *pipelines[2];
   api_descriptor_set **sets;

   uint32_t *jump_buf_data;
   api_buffer *jump_buf[2];

   api_buffer *result_buf;
   uint64_t *results;
} test_state;

static unsigned
get_next_size(unsigned size)
{
   unsigned pot = 1 << logbase2(size);
   return size + pot / 2;
}

static void
run(api_context *ctx, const char *test_name, test_stage stage, test_state *state)
{
   if (stage == INIT) {
      /* Build pipelines. */
      state->layout =
         ctx->create_descriptor_set_layout(ctx,
                                           &(api_descriptor_set_layout_desc){
                                              .storage_buffer[0].array_size = 1,
                                              .storage_buffer[1].array_size = 1,
                                              .storage_buffer[1].vk_binding = 1,
                                           });

      for (int uniform = 0; uniform <= 1; uniform++) {
         api_shader *cs = create_memory_offset_chasing_cs(ctx, uniform, state->num_indirections,
                                                          ctx->options.spacing,
                                                          ctx->options.max_size);
         state->pipelines[uniform] = ctx->create_compute_pipeline(ctx, cs, state->layout);
      }

      /* Initialize the rest. */
      state->num_tests = 0;
      state->jump_buf_data = (uint32_t*)malloc(ctx->options.max_size);

      state->jump_buf[api_heap_device] = ctx->create_buffer(ctx, ctx->options.max_size,
                                                            api_heap_device, 0);
      if (ctx->has_host_uncached_heap) {
         state->jump_buf[api_heap_host_uncached] = ctx->create_buffer(ctx, ctx->options.max_size,
                                                                      api_heap_host_uncached, 0);
      }
   }


   if (stage == RUN) {
      /* Now that we know the number of tests, finish the initialization. */
      state->result_buf = ctx->create_buffer(ctx, 16 * state->num_tests, api_heap_device, 0);
      state->sets = calloc(state->num_tests, sizeof(state->sets[0]));

      for (unsigned i = 0; i < state->num_tests; i++) {
         state->sets[i] = ctx->create_descriptor_set(ctx, state->layout);
         ctx->set_storage_buffer_descriptor(ctx, state->sets[i], 1, state->result_buf, i * 16, 16);
      }
   }

   const unsigned name_indent = 36;
   const unsigned max_digits = 6;

   if (stage == REPORT) {
      state->results = malloc(state->result_buf->size);
      ctx->get_buffer_data(ctx, state->result_buf, 0, state->result_buf->size, state->results);

      printf("%-*s,", name_indent, "Size");

      for (unsigned size = MIN_MEM_SIZE; size <= ctx->options.max_size; size = get_next_size(size)) {
         unsigned number = 0;
         unsigned order;

         for (order = 0; order < 5; order++) {
            if (size < 1ull << (order * 10 + 11)) {
               number = size >> (order * 10);
               break;
            }
         }

         printf("%*u", max_digits - !!order, number);
         if (order)
            printf("%c", " KMGE"[order]);
         printf(",");
      }

      puts("");
   }

   unsigned test_index = 0;

   for (int uniform = 0; uniform <= 1; uniform++) {
      for (api_heap_type heap = api_heap_device; heap <= api_heap_host_uncached; heap++) {
         if (!state->jump_buf[heap])
            continue;

         if (stage == REPORT) {
            char name[1024];

            snprintf(name, sizeof(name), "%s.%s.%s",
                     test_name, uniform ? "uniform" : "nonuniform",
                     heap == api_heap_device ? "devmem" :
                                               heap == api_heap_host_uncached ? "hostmem" :
                                                                                "hostmem_cached");
            printf("%-*s,", name_indent, name);
         }

         for (unsigned size = MIN_MEM_SIZE; size <= ctx->options.max_size;
              size = get_next_size(size)) {
            switch (stage) {
            case INIT:
               break;

            case RUN:
               set_jump_buffer_data(ctx, state->jump_buf[heap], ctx->options.spacing, size,
                                    state->jump_buf_data);
               ctx->set_storage_buffer_descriptor(ctx, state->sets[test_index], 0,
                                                  state->jump_buf[heap], 0, ctx->options.max_size);

               ctx->begin_cmdbuf(ctx);
               ctx->bind_descriptor_set(ctx, state->sets[test_index]);
               ctx->bind_compute_pipeline(ctx, state->pipelines[uniform]);
               ctx->dispatch(ctx, 1, 1, 1);
               ctx->pipeline_barrier_buffer(ctx, state->result_buf);
               ctx->end_cmdbuf_and_submit(ctx, NULL);
               break;

            case REPORT: {
               uint64_t clock_cycles = state->results[test_index * 2] / state->num_indirections;
               uint64_t last_offset = state->results[test_index * 2 + 1];

               /* We have an extra iteration before clockARB, and the non-uniform shader has
                * InvocationID=1 ahead by one spacing.
                */
               uint64_t expected_last_offset =
                  (ctx->options.spacing * (1 + !uniform + state->num_indirections)) % size;

               assert(last_offset == expected_last_offset);

               if (0) {
                  printf("%*u,", max_digits, (unsigned)last_offset);
                  //printf("%*u,", max_digits, (unsigned)expected_last_offset);
                  break;
               }

               if (clock_cycles >= pow(10, max_digits + 1))
                  printf("%*s,", max_digits, "n/a");
               else
                  printf("%*u,", max_digits, (unsigned)clock_cycles);
               break;
            }
            }

            if (stage == RUN)
               print_progress(state->num_tests, &test_index, 20);
            else
               test_index++;
         }

         if (stage == REPORT)
            puts("");
      }
   }

   if (stage == INIT)
      state->num_tests = test_index;
}

void
test_latency(api_context *ctx, const char *test_name)
{
   if (!ctx->has_shader_subgroup_clock)
      error("Shader subgroup clock support required.");

   /* Notes:
    * - ideally set spacing = cache_line
    * - if you set spacing = cache_line * 2, the cache will be thrashed at cache_size * 2,
    *   making it misleadingly appear as if the cache size is 2x
    * - if you set spacing = cache_line / 2, there will be no effect on results
    * - this can be used to find the true cache size and cache line size
    */

   if (!ctx->options.spacing || !ctx->options.max_size) {
      error("Missing parameters.\n"
            "\n"
            "Required parameters:\n"
            "   -spacing=N        The spacing between addresses in bytes (e.g. 64).\n"
            "                     It should be <= cache line size to get valid results.\n"
            "   -maxsize=N[KMGT]  The maximum tested buffer size (e.g. 32M), it should be\n"
            "                     slightly greater than the last level cache size.\n");
   }

   if (ctx->options.max_size > ctx->max_storage_buffer_range) {
      error("The maximum storage buffer range is %u MB, specified %"PRIu64" MB.\n",
            ctx->max_storage_buffer_range >> 20, ctx->options.max_size >> 20);
   }

   test_state state = {0};
   state.num_indirections = ctx->options.max_size / ctx->options.spacing;

   run(ctx, test_name, INIT, &state);

   printf("Indirections per shader: %u\n", state.num_indirections);
   printf("Executing tests ...");

   run(ctx, test_name, RUN, &state);

   puts("");
   puts("Reading back results...");

   run(ctx, test_name, REPORT, &state);

   free(state.jump_buf_data);
   free(state.results);
}
