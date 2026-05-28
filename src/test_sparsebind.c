/* Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>

#include "common.h"

#define BLOCKS_PER_BUFFER     96

typedef enum {
   NO_BIND              = 1 << 0,
   BIND_ONE             = 1 << 1,
   UNBIND_ONE           = 1 << 2,
   BIND_UNBIND_SAME_ONE = 1 << 3,
   BIND_UNBIND_ALL      = 1 << 4,

   SIZE_REGULAR         = 1 << 5,
   SIZE_4X              = 1 << 6,

   BLOCK_REGULAR        = 1 << 7,
   BLOCK_8X             = 1 << 8,

   START_BOUND          = 1 << 9,
} test_flags;

static const char *flag_strings[] = {
#define STR(x) [LOG2_POT(x)] = #x
#define STR_NULL(x) [LOG2_POT(x)] = NULL
   STR(START_BOUND),

   STR(NO_BIND),
   STR(BIND_ONE),
   STR(UNBIND_ONE),
   STR(BIND_UNBIND_SAME_ONE),
   STR(BIND_UNBIND_ALL),

   STR_NULL(SIZE_REGULAR),
   STR_NULL(SIZE_4X),

   STR_NULL(BLOCK_REGULAR),
   STR_NULL(BLOCK_8X),
#undef STR
#undef STR_NULL
};

typedef enum {
   CMDBUF_NONE,
   CMDBUF_EMPTY,
   CMDBUF_SIMPLE,
   NUM_CMDBUF_OPTIONS,
} cmdbuf_option;

static unsigned
get_min_sparse_block_size(api_context *ctx)
{
   assert(ctx->sparse_buffer_alignment);
   return MAX2(ctx->sparse_buffer_alignment, 64 * 1024);
}

static unsigned
get_sparse_block_size(api_context *ctx, test_flags flags)
{
   return get_min_sparse_block_size(ctx) * (flags & BLOCK_8X ? 8 : 1);
}

static unsigned
get_buffer_size(api_context *ctx, test_flags flags)
{
   return get_min_sparse_block_size(ctx) * BLOCKS_PER_BUFFER * (flags & SIZE_4X ? 4 : 1);
}

static unsigned
get_num_iterations(api_context *ctx, test_flags flags)
{
   return get_buffer_size(ctx, flags) / get_sparse_block_size(ctx, flags);
}

static unsigned
get_num_bind_flags(test_flags flags)
{
   return bitcount(flags & (BIND_ONE | UNBIND_ONE | BIND_UNBIND_SAME_ONE |
                            BIND_UNBIND_ALL));
}

static void
run_one(api_context *ctx, api_timestamp_query_pool *timestamps, test_flags flags,
        cmdbuf_option cmdbuf_option, bool async)
{
   /* Validate flags. */
   assert(get_num_bind_flags(flags) <= 1);
   assert(!(flags & START_BOUND) || get_num_bind_flags(flags) == 1);
   assert(bitcount(flags & (SIZE_REGULAR | SIZE_4X)) == 1);

   /* Initialization. */
   unsigned sparse_block_size = get_sparse_block_size(ctx, flags);
   api_buffer *buf = ctx->create_buffer(ctx, get_buffer_size(ctx, flags), api_heap_device,
                                        sparse_block_size);

   if (flags & START_BOUND)
      ctx->buffer_bind_sparse(ctx, buf, 0, buf->size, true, NULL);

   /* Execution. */
   ctx->begin_cmdbuf(ctx);
   ctx->write_next_timestamp(ctx, timestamps);
   ctx->end_cmdbuf_and_submit(ctx, NULL);

   unsigned num_iterations = get_num_iterations(ctx, flags);

   for (unsigned i = 0; i < num_iterations; i++) {
      api_fence *fence = NULL;

      switch (flags & (BIND_ONE | UNBIND_ONE | BIND_UNBIND_SAME_ONE | BIND_UNBIND_ALL)) {
      case BIND_ONE:
         ctx->buffer_bind_sparse(ctx, buf, i * sparse_block_size, sparse_block_size, true,
                                 async ? &fence : NULL);
         break;
      case UNBIND_ONE:
         ctx->buffer_bind_sparse(ctx, buf, i * sparse_block_size, sparse_block_size, false,
                                 async ? &fence : NULL);
         break;
      case BIND_UNBIND_SAME_ONE:
         ctx->buffer_bind_sparse(ctx, buf, 0, sparse_block_size, i % 2 == 0, async ? &fence : NULL);
         break;
      case BIND_UNBIND_ALL:
         ctx->buffer_bind_sparse(ctx, buf, 0, buf->size, i % 2 == 0, async ? &fence : NULL);
         break;
      case 0:
         break;
      default:
         error("invalid BIND option");
      }

      if (cmdbuf_option != CMDBUF_NONE) {
         ctx->begin_cmdbuf(ctx);

         if (cmdbuf_option == CMDBUF_SIMPLE)
            ctx->clear_buffer(ctx, buf, 0, 64, 0);

         ctx->end_cmdbuf_and_submit(ctx, fence);
      }
   }

   ctx->begin_cmdbuf(ctx);
   ctx->write_next_timestamp(ctx, timestamps);
   ctx->end_cmdbuf_and_submit(ctx, NULL);
}

enum {
   OP_NONE,
   OP_BIND_ADD_ONE,
   OP_BIND_REMOVE_ONE,
   OP_BIND_UNBIND_SAME_ONE,
   OP_BIND_UNBIND_ALL,
   NUM_OP_OPTIONS,
};

static void
generate_tests(unsigned *tests, unsigned max_tests, unsigned *num_tests)
{
   *num_tests = 0;

   for (unsigned op = 0; op < NUM_OP_OPTIONS; op++) {
      for (unsigned double_size = 0; double_size < 2; double_size++) {
         for (unsigned double_block = 0; double_block < 2; double_block++) {
            for (unsigned start_bound = 0; start_bound < 2; start_bound++) {
               if (op == OP_NONE && (start_bound || double_size || double_block))
                  continue;

               if ((op == OP_BIND_ADD_ONE && start_bound) ||
                   (op == OP_BIND_REMOVE_ONE && !start_bound))
                  continue;

               assert(*num_tests < max_tests);

               tests[(*num_tests)++] =
                  (start_bound ? START_BOUND : 0) |
                  (op == OP_NONE ? NO_BIND : 0) |
                  (op == OP_BIND_ADD_ONE ? BIND_ONE : 0) |
                  (op == OP_BIND_REMOVE_ONE ? UNBIND_ONE : 0) |
                  (op == OP_BIND_UNBIND_SAME_ONE ? BIND_UNBIND_SAME_ONE : 0) |
                  (op == OP_BIND_UNBIND_ALL ? BIND_UNBIND_ALL : 0) |
                  (double_size ? SIZE_4X : SIZE_REGULAR) |
                  (double_block ? BLOCK_8X : BLOCK_REGULAR);
            }
         }
      }
   }
}

static bool
print_na(api_context *ctx, test_flags flags, cmdbuf_option cmdbuf_option, bool async)
{
   if (async && (!ctx->has_async_sparse_queue || flags & NO_BIND))
      return true;

   return !(flags & ~(NO_BIND | SIZE_REGULAR | BLOCK_REGULAR)) && cmdbuf_option == CMDBUF_NONE;
}

static bool
print_nothing(cmdbuf_option cmdbuf_option, bool async)
{
   return cmdbuf_option == CMDBUF_NONE && async;
}

void
test_sparsebind(api_context *ctx, const char *test_name)
{
   if (!ctx->has_sparse_buffer)
      error("Sparse buffer support is required or gpu-ratemeter doesn't support sparse buffers for this API.");

   unsigned tests[100], num_tests;
   generate_tests(tests, ARRAY_SIZE(tests), &num_tests);

   /* Create timestamp queries. */
   api_timestamp_query_pool *timestamps =
      ctx->create_timestamp_pool(ctx, num_tests * 2 * 5);

   printf("Executing tests ...");
   unsigned num_visited_tests = 0;

   for (unsigned i = 0; i < num_tests; i++) {
      for (unsigned async = 0; async < 2; async++) {
         for (unsigned cmdbuf = 0; cmdbuf < NUM_CMDBUF_OPTIONS; cmdbuf++) {
            print_progress(num_tests * 6, &num_visited_tests, 20);

            if (print_na(ctx, tests[i], cmdbuf, async) || print_nothing(cmdbuf, async))
               continue;

            run_one(ctx, timestamps, tests[i], cmdbuf, async);
         }
      }
   }
   puts("");

   puts("Reading back results...");
   ctx->query_timestamps(ctx, timestamps);

   puts("");
   puts("Units are bind calls/second and/or command buffers/second.");
   puts("");

   const unsigned name_indent = 64;

   printf("%-*s,%9s,%9s,%9s,%9s,%9s\n", name_indent, "Async sparse bind queue", "no", "no", "no", "yes", "yes");
   printf("%-*s,%9s,%9s,%9s,%9s,%9s\n", name_indent, "Command buffer", "none", "empty", "simple", "empty", "simple");

   for (unsigned i = 0; i < num_tests; i++) {
      unsigned len = 0;
      unsigned test_flags = tests[i];

      if (!get_num_bind_flags(test_flags))
         test_flags &= ~(SIZE_REGULAR | BLOCK_REGULAR);

      len += printf("%s", test_name);

      for (unsigned flag = 0; flag < ARRAY_SIZE(flag_strings); flag++) {
         if ((1 << flag) & test_flags) {
            len += printf(".");

            if (flag_strings[flag]) {
               for (unsigned c = 0; flag_strings[flag][c]; c++)
                  len += printf("%c", tolower(flag_strings[flag][c]));
            } else {
               if ((1 << flag) & (SIZE_REGULAR | SIZE_4X))
                  len += printf("size%um", get_buffer_size(ctx, test_flags) >> 20);
               else if ((1 << flag) & (BLOCK_REGULAR | BLOCK_8X))
                  len += printf("block%uk", get_sparse_block_size(ctx, test_flags) >> 10);
               else
                  error("missing flag string");
            }
         }
      }

      assert(len <= name_indent);
      printf("%*s", name_indent - len, "");

      for (unsigned async = 0; async < 2; async++) {
         for (unsigned cmdbuf = 0; cmdbuf < NUM_CMDBUF_OPTIONS; cmdbuf++) {
            if (print_nothing(cmdbuf, async))
               continue;

            if (print_na(ctx, tests[i], cmdbuf, async)) {
               printf(",      n/a");
               continue;
            }

            print_throughput_from_next_timestamps(ctx, timestamps, get_num_iterations(ctx, tests[i]),
                                                  NULL, ",%9.0f", 0);
         }
      }
      puts("");
   }
}
