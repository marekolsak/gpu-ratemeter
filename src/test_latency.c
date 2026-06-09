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
   api_compute_pipeline *pipelines[2][2];
   api_descriptor_set **sets;

   uint32_t *sequence_tmp;
   uint32_t *jump_buf_data;
   api_buffer *jump_buf[api_num_heaps];

   api_buffer *result_buf;
   uint64_t *results;
} test_state;

static api_shader *
create_memory_offset_chasing_cs(api_context *ctx, bool uniform, bool coherent, unsigned num_indirections)
{
   char source[1800];

   int len = snprintf(source, ARRAY_SIZE(source),
                      "#version 450 \n"
                      "#extension GL_ARB_shader_clock : require \n"
                      "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require \n"
                      "\n"

                      "#define CLOCK_BITS %u \n"
                      "#define SPACING %u \n"
                      "#define MAX_UINTS %uu \n"
                      "#define UNIFORM %s \n"
                      "#define NUM_INDIRECTIONS %u \n"
                      "#define QUALIFIER %s \n"
                      "\n"

                      /* If the clock doesn't have enough bits such that the clock wraps around and
                       * reaches the start time before the test finishes, the results will be bogus.
                       * To work around that, we need to read and accumulate the cycle count mid-test.
                       *
                       * This number determines how many indirections should execute before the clock
                       * is read. Number 12 means that we don't get bogus results if latency < 2^12.
                       */

                      "#define NUM_INDIRECTIONS_PER_CLOCK_ACCUM (1 << (CLOCK_BITS - 12)) \n"
                      "\n"

                      "layout(local_size_x = UNIFORM ? 1 : 2, local_size_y = 1, local_size_z = 1) in; \n"
                      "\n"

                      "layout(set = 0, binding = 0, std430) readonly restrict QUALIFIER buffer B0 { \n"
                      "   uint offsets[MAX_UINTS]; \n"
                      "} jumpbuf; \n"
                      "\n"

                      "layout(set = 0, binding = 1, std430) buffer B1 { \n"
                      "   uint64_t clock_cycles; \n"
                      "   uint64_t last_offset; \n"
                      "} result; \n"
                      "\n"

                      /* Subtract, but try to recover from a single clock overflow. */
                      "uint64_t subtract(uint64_t a, uint64_t b) { \n"
                      "#if CLOCK_BITS > 0 \n"
                      "   if (a < b) \n"
                      "      a += 1 << CLOCK_BITS; \n"
                      "#endif \n"
                      "   return a - b; \n"
                      "} \n"
                      "\n"

                      "void main() { \n"
                      "   uint start_offset = UNIFORM ? 0 : gl_LocalInvocationID.x * SPACING; \n"
                      "\n"

                      /* Warm up the caches by executing all indirections. */
                      "   uint offset = jumpbuf.offsets[start_offset / 4].x; \n"
                      "   for (int i = 0; i < NUM_INDIRECTIONS; i++) \n"
                      "      offset = jumpbuf.offsets[offset / 4].x; \n"
                      "\n"

                      "   offset = jumpbuf.offsets[start_offset / 4].x; \n"
                      "   uint64_t accum = 0; \n"
                      "   uint64_t start = clockARB(); \n"
                      "\n"

                      "#if CLOCK_BITS > 0 \n"
                      "   for (int i = 0; i < NUM_INDIRECTIONS; i += NUM_INDIRECTIONS_PER_CLOCK_ACCUM) { \n"
                      "      int num = min(NUM_INDIRECTIONS - i, NUM_INDIRECTIONS_PER_CLOCK_ACCUM); \n"
                      "\n"
                      "      for (int j = 0; j < num; j++) \n"
                      "         offset = jumpbuf.offsets[offset / 4].x; \n"
                      "\n"
                      "      uint64_t end = clockARB(); \n"
                      "      accum += subtract(end, start); \n"
                      "      start = end; \n"
                      "   } \n"
                      "#else \n"
                      "   for (int i = 0; i < NUM_INDIRECTIONS; i++) \n"
                      "      offset = jumpbuf.offsets[offset / 4].x; \n"
                      "#endif \n"
                      "\n"

                      "   accum += subtract(clockARB(), start); \n"
                      "   result.clock_cycles = accum; \n"

                      /* Also store the offset to make the indirections not dead. */
                      "   result.last_offset = offset; \n"
                      "} \n",
                      ctx->options.clock_bits, ctx->options.spacing,
                      (unsigned)ctx->options.max_size / 4,
                      uniform ? "true" : "false", num_indirections,
                      coherent ? "coherent" : "");
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
   /* With cache_line_size=16, uint elements, and indices (0, 1, 2, 3, 4, 5, 6, 7), we would get
    * cache hits at 0->1->2->3 and 4->5->6->7, skewing our results. To prevent that, the closer
    * the indirections are in a sequence, the farther apart they should be in memory. For the
    * given example, an optimal sequence would be (0, 4, 2, 6, 1, 5, 3, 7).
    *
    * Therefore, we need to generate a sequence of numbers minimizing address locality. First,
    * let's take apart the above optimal sequence (n=8):
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
    * If n is halfway between 2 powers of two, i.e. n=3*k, k=2^x, for example k=4, n=12,
    * an optimal sequence is (0, 4, 8, 2, 6, 10, 1, 5, 9, 3, 7, 11). We can take it apart the same
    * as above:
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
    * i=j/3, n=k into it and adding "(j % 3) * n/3" at the end.
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

      for (unsigned uniform = 0; uniform <= 1; uniform++) {
         for (unsigned coherent = 0; coherent <= 1; coherent++) {
            api_shader *cs = create_memory_offset_chasing_cs(ctx, uniform, coherent,
                                                             state->num_indirections);
            state->pipelines[uniform][coherent] =
               ctx->create_compute_pipeline(ctx, cs, state->layout);
         }
      }

      /* Initialize the rest. */
      state->num_tests = 0;
      assert(ctx->options.spacing % 4 == 0);
      state->sequence_tmp = (uint32_t*)malloc(ctx->options.max_size / (ctx->options.spacing / 4));
      state->jump_buf_data = (uint32_t*)malloc(ctx->options.max_size);

      for (unsigned i = 0; i < api_num_heaps; i++) {
         if (!ctx->has_heap[i])
            continue;

         switch (i) {
         case api_heap_device:
         case api_heap_device_coherent_amd:
         case api_heap_host_uncached:
         case api_heap_host_uncached_coherent_amd:
            break;
         default:
            continue;
         }

         state->jump_buf[i] =
            ctx->create_buffer(ctx, ctx->options.max_size, i,
                               ctx->options.sparse_bound ||
                               ctx->options.sparse_unbound ? ctx->sparse_buffer_alignment : 0);
         if (ctx->options.sparse_bound)
            ctx->buffer_bind_sparse(ctx, state->jump_buf[i], 0, ctx->options.max_size, true, NULL);
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

   const unsigned name_indent = 50;
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

   for (unsigned coherent = 0; coherent <= 1; coherent++) {
      for (int uniform = 0; uniform <= 1; uniform++) {
         for (api_heap_type heap = api_heap_device; heap < api_num_heaps; heap++) {
            if (!state->jump_buf[heap])
               continue;

            if (stage == REPORT) {
               char name[1024];

               snprintf(name, sizeof(name), "%s.%s.%s.%s", test_name,
                        coherent ? "coherent" : "default", uniform ? "uniform" : "nonuniform",
                        heap_to_string(heap));
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
                  ctx->bind_compute_pipeline(ctx, state->pipelines[uniform][coherent]);
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

   const unsigned min_indirections = 1024;
   unsigned num_indirections = ctx->options.max_size / ctx->options.spacing;

   if (num_indirections < min_indirections) {
      error("Not enough indirections for usable results (have %u, need %u). Increase maxsize or decrease spacing.",
            num_indirections, min_indirections);
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
