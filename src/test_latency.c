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

typedef enum {
   INIT,
   RUN,
   REPORT,
} test_stage;

typedef struct {
   unsigned min_size;
   unsigned num_indirections;
   unsigned num_tests;

   api_descriptor_set_layout *layout;
   api_compute_pipeline *pipelines[2];
   api_descriptor_set **sets;

   uint32_t *sequence_tmp;
   uint32_t *jump_buf_data;
   api_buffer *jump_buf[2];

   api_buffer *result_buf;
   uint64_t *results;
} test_state;

static api_shader *
create_memory_offset_chasing_cs(api_context *ctx, bool uniform, unsigned num_indirections,
                                unsigned spacing, unsigned max_size)
{
   char source[1200];

   int len = snprintf(source, ARRAY_SIZE(source),
                      "#version 450 \n"
                      "#extension GL_ARB_shader_clock : require \n"
                      "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require \n"
                      "\n"

                      "#define UNIFORM %s \n"
                      "#define NUM_INDIRECTIONS %u \n"
                      "#define MAX_UINTS %uu \n"
                      "#define SPACING %u \n"
                      "\n"

                      "layout(local_size_x = UNIFORM ? 1 : 2, local_size_y = 1, local_size_z = 1) in; \n"
                      "\n"

                      "layout(set = 0, binding = 0, std430) readonly restrict buffer B0 { \n"
                      "   uint offsets[MAX_UINTS]; \n"
                      "} jumpbuf; \n"
                      "\n"

                      "layout(set = 0, binding = 1, std430) buffer B1 { \n"
                      "   uint64_t clock_cycles; \n"
                      "   uint64_t last_offset; \n"
                      "} result; \n"
                      "\n"

                      "void main() { \n"
                      "   uint start_offset = UNIFORM ? 0 : gl_LocalInvocationID.x * SPACING; \n"
                      "   uint offset = jumpbuf.offsets[start_offset / 4].x; \n"
                      "\n"

                      /* Warm up the caches by executing all indirections or until the buffer wraps
                       * around, whichever is less. This caches as much data as the caches can hold.
                       */
                      "   offset = jumpbuf.offsets[start_offset / 4].x; \n"
                      "   for (int i = 0; i < NUM_INDIRECTIONS; i++) { \n"
                      "      uint prev_offset = offset; \n"
                      "      offset = jumpbuf.offsets[offset / 4].x; \n"
                      "      if (offset < prev_offset) \n"
                      "         break; \n"
                      "   } \n"
                      "\n"

                      "   offset = jumpbuf.offsets[start_offset / 4].x; \n"
                      "   uint64_t start_cycles = clockARB(); \n"
                      "\n"

                      "   for (int i = 0; i < NUM_INDIRECTIONS; i++) \n"
                      "      offset = jumpbuf.offsets[offset / 4].x; \n"
                      "\n"

                      "   result.clock_cycles = clockARB() - start_cycles; \n"

                      /* Also store the offset to make the indirections not dead. */
                      "   result.last_offset = offset; \n"
                      "} \n",
                      uniform ? "true" : "false", num_indirections, max_size / 4, spacing);
   assert(len < ARRAY_SIZE(source));

   return ctx->create_shader(ctx, source, api_shader_cs);
}

static unsigned
get_next_size(unsigned size)
{
   unsigned pot = 1 << logbase2(size);
   return size + pot / 2;
}

#if 0
#define DBG_PRINT(...) printf(__VA_ARGS__)
#else
#define DBG_PRINT(...)
#endif

static void
generate_sequence(uint32_t *array, unsigned base, unsigned n)
{
   /* With cache line size = 4 elements and sequence (0, 1, 2, 3, 4, 5, 6, 7), we would get cache
    * hits at 0->1->2->3 and 4->5->6->7, skewing our results. To prevent that, the closer
    * the indirections are in a sequence, the farther apart they should be in memory. In that
    * example, an optimal sequence would be (0, 4, 2, 6, 1, 5, 3, 7).
    *
    * Therefore, we need to generate a sequence of numbers minimizing address locality. For n=8,
    * the above sequence can be rewritten as:
    *    i=0: 0 = 0 * n/8 + 0 * n/4 + 0 * n/2
    *    i=1: 4 = 0 * n/8 + 0 * n/4 + 1 * n/2
    *    i=2: 2 = 0 * n/8 + 1 * n/4 + 0 * n/2
    *    i=3: 6 = 0 * n/8 + 1 * n/4 + 1 * n/2
    *    i=4: 1 = 1 * n/8 + 0 * n/4 + 0 * n/2
    *    i=5: 5 = 1 * n/8 + 0 * n/4 + 1 * n/2
    *    i=6: 3 = 1 * n/8 + 1 * n/4 + 0 * n/2
    *    i=7: 7 = 1 * n/8 + 1 * n/4 + 1 * n/2
    *
    * In that, we see dot products of boolean vectors made of bits of "i" and constant vector
    * (n >> log2(n), n >> log2(n)-1, ..., n >> 1).
    *
    * Similarly for k=2^x, n=3*k, for example k=4, n=12, i.e. n is half way between 2 powers of
    * two, we need:
    *    j=0:   0 = 0 * k/4 + 0 * k/2 + 0 * n/3
    *    j=1:   4 = 0 * k/4 + 0 * k/2 + 1 * n/3
    *    j=2:   8 = 0 * k/4 + 0 * k/2 + 2 * n/3
    *    j=3:   2 = 0 * k/4 + 1 * k/2 + 0 * n/3
    *    j=4:   6 = 0 * k/4 + 1 * k/2 + 1 * n/3
    *    j=5:  10 = 0 * k/4 + 1 * k/2 + 2 * n/3
    *    j=6:   1 = 1 * k/4 + 0 * k/2 + 0 * n/3
    *    j=7:   5 = 1 * k/4 + 0 * k/2 + 1 * n/3
    *    j=8:   9 = 1 * k/4 + 0 * k/2 + 2 * n/3
    *    j=9:   3 = 1 * k/4 + 1 * k/2 + 0 * n/3
    *    j=10:  7 = 1 * k/4 + 1 * k/2 + 1 * n/3
    *    j=11: 11 = 1 * k/4 + 1 * k/2 + 2 * n/3
    *
    * In that, we see dot products generating the same sequence as the first example by plugging
    * i=j/3, n=k into it and "(j % 3) * n/3" added.
    */
   assert(IS_POT(n) || IS_POT(n / 3));

   DBG_PRINT("\n");

   if (IS_POT(n)) {
      unsigned log2n = logbase2(n);

      for (unsigned i = 0; i < n; i++) {
         unsigned sum = 0;

         /* Calculate the dot product. */
         for (int bit = log2n; bit >= 1; bit--)
            sum += ((i >> (bit - 1)) & 0x1) * (n >> bit);

         array[i] = base + sum;
         DBG_PRINT("%2u, ", array[i]);
      }
   } else {
      unsigned k = n / 3;
      unsigned log2k = logbase2(k);

      for (unsigned i = 0; i < k; i++) {
         unsigned sum = 0;

         /* Calculate the dot product. */
         for (int bit = log2k; bit >= 1; bit--)
            sum += ((i >> (bit - 1)) & 0x1) * (k >> bit);

         for (unsigned c = 0; c < 3; c++) {
            array[i * 3 + c] = sum + c * n / 3;
            DBG_PRINT("%2u, ", array[i * 3 + c]);
         }
      }
   }

   DBG_PRINT("\n");
}

static void
set_jump_buffer_data(api_context *ctx, test_state *state, api_buffer *buf, unsigned size)
{
   unsigned spacing = ctx->options.spacing;
   uint32_t *sequence = state->sequence_tmp;
   uint32_t *mem = state->jump_buf_data;

   assert(spacing % 4 == 0);
   assert(size % spacing == 0);
   unsigned n = size / spacing;

   generate_sequence(sequence, 0, n);

   memset(mem, 0, size);

   for (unsigned i = 0; i < n - 1; i++)
      mem[sequence[i] * spacing / 4] = sequence[i + 1] * spacing;

   /* Close the circle. */
   mem[(n - 1) * spacing / 4] = 0;

   ctx->upload_buffer_data(ctx, buf, 0, size, mem);
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
      assert(ctx->options.spacing % 4 == 0);
      state->sequence_tmp = (uint32_t*)malloc(ctx->options.max_size / (ctx->options.spacing / 4));
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

      for (unsigned size = state->min_size; size <= ctx->options.max_size; size = get_next_size(size)) {
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

         for (unsigned size = state->min_size; size <= ctx->options.max_size;
              size = get_next_size(size)) {
            switch (stage) {
            case INIT:
               break;

            case RUN:
               set_jump_buffer_data(ctx, state, state->jump_buf[heap], size);
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

   if (ctx->options.max_size > ctx->max_storage_buffer_range) {
      error("The maximum storage buffer range is %u MB, need %"PRIu64" MB.",
            ctx->max_storage_buffer_range >> 20, ctx->options.max_size >> 20);
   }

   if (!ctx->options.spacing || !ctx->options.max_size) {
      error("Missing parameters.\n"
            "\n"
            "Required parameters:\n"
            "   -spacing=N        The spacing between addresses in bytes (e.g. 64).\n"
            "                     It should be <= cache line size to get valid results.\n"
            "   -maxsize=N[KMGT]  The maximum tested buffer size (e.g. 32M), it should be\n"
            "                     slightly greater than the last level cache size.\n");
   }

   if (ctx->options.spacing < 4 || !IS_POT(ctx->options.spacing))
      error("Spacing must be >= 4 and a power of two.");

   unsigned num_indirections = ctx->options.max_size / ctx->options.spacing;

   if (num_indirections < 1024) {
      error("Not enough indirections (only %u, need 1024). Increase maxsize or decrease spacing.",
            num_indirections);
   }

   test_state state = {0};
   state.min_size = MAX2(ctx->options.spacing * 4, 1024);
   state.num_indirections = num_indirections;

   printf("Indirections per shader: %u\n", state.num_indirections);
   puts("Building compute pipelines...");

   run(ctx, test_name, INIT, &state);

   printf("Executing tests ...");

   run(ctx, test_name, RUN, &state);

   puts("");
   puts("Reading back results...");

   run(ctx, test_name, REPORT, &state);

   free(state.sequence_tmp);
   free(state.jump_buf_data);
   free(state.results);
}
