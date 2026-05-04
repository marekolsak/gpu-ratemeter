/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>

#include "common.h"

#define MIN_SIZE           512
#define MAX_SIZE           (256 * 1024 * 1024)
#define MAX_HOSTMEM_SIZE    (16 * 1024 * 1024)
#define SIZE_STEP_SHIFT    1

#define NUM_WARMUP_RUNS    8
#define NUM_RUNS           32

#define MAX_ALIGNMENT      (64 * 1024)

enum {
   TEST_FILL_DEVMEM,
   TEST_FILL_HOSTMEM,
   TEST_COPY_DEVMEM_TO_DEVMEM,
   TEST_COPY_DEVMEM_TO_HOSTMEM,
   TEST_COPY_HOSTMEM_TO_DEVMEM,
   NUM_TESTS,
};

static const char *test_strings[] = {
   [TEST_FILL_DEVMEM] = "fill.devmem",
   [TEST_FILL_HOSTMEM] = "fill.hostmem",
   [TEST_COPY_DEVMEM_TO_DEVMEM] = "copy.devmem_to_devmem",
   [TEST_COPY_DEVMEM_TO_HOSTMEM] = "copy.devmem_to_hostmem",
   [TEST_COPY_HOSTMEM_TO_DEVMEM] = "copy.hostmem_to_devmem",
};

enum {
   ALIGN_MAX,
   ALIGN_256,
   ALIGN_128,
   ALIGN_64,
   ALIGN_4,
   ALIGN_2,
   ALIGN_1,
   ALIGN_SRC256,
   ALIGN_SRC128,
   ALIGN_SRC64,
   ALIGN_SRC4,
   ALIGN_SRC2,
   ALIGN_SRC1,
   ALIGN_DST256,
   ALIGN_DST128,
   ALIGN_DST64,
   ALIGN_DST4,
   ALIGN_DST2,
   ALIGN_DST1,
   ALIGN_SRC4_DST2,
   ALIGN_SRC4_DST1,
   ALIGN_SRC2_DST4,
   ALIGN_SRC2_DST1,
   ALIGN_SRC1_DST4,
   ALIGN_SRC1_DST2,
   NUM_ALIGNMENTS,
};

struct align_info_t {
   const char *string;
   unsigned src_offset;
   unsigned dst_offset;
};

#define ALIGN_BOTH(value)        [ALIGN_##value] = {"both="#value, value, value}
#define ALIGN_SRC(src)           [ALIGN_SRC##src] = {"src="#src, src, 0}
#define ALIGN_DST(dst)           [ALIGN_DST##dst] = {"dst="#dst, 0, dst}
#define ALIGN_SRC_DST(src, dst)  [ALIGN_SRC##src##_DST##dst] = {"src="#src".dst="#dst, src, dst}

static const struct align_info_t align_info[] = {
   [ALIGN_MAX] = {"maxalign", 0, 0},
   ALIGN_BOTH(256),
   ALIGN_BOTH(128),
   ALIGN_BOTH(64),
   ALIGN_BOTH(4),
   ALIGN_BOTH(2),
   ALIGN_BOTH(1),
   ALIGN_SRC(256),
   ALIGN_SRC(128),
   ALIGN_SRC(64),
   ALIGN_SRC(4),
   ALIGN_SRC(2),
   ALIGN_SRC(1),
   ALIGN_DST(256),
   ALIGN_DST(128),
   ALIGN_DST(64),
   ALIGN_DST(4),
   ALIGN_DST(2),
   ALIGN_DST(1),
   ALIGN_SRC_DST(4, 2),
   ALIGN_SRC_DST(4, 1),
   ALIGN_SRC_DST(2, 4),
   ALIGN_SRC_DST(2, 1),
   ALIGN_SRC_DST(1, 4),
   ALIGN_SRC_DST(1, 2),
};

enum test_stage {
   COUNT_TESTS,
   RUN,
   REPORT,
};

static void
run(api_context *ctx, const char *test_suite_name, enum test_stage stage,
    api_timestamp_query_pool *timestamps, unsigned *num_tests)
{
   if (stage == REPORT) {
      printf("%-53s", "Size per command");
      for (unsigned size = MIN_SIZE; size <= MAX_SIZE; size <<= SIZE_STEP_SHIFT) {
         if (size >= 1024 * 1024)
            printf(",%6uMB", size / (1024 * 1024));
         else if (size >= 1024)
            printf(",%6uKB", size / 1024);
         else
            printf(", %6uB", size);
      }
      printf("\n");
   }

   /* Create buffers. */
   /* We allocate enough memory and cycle through the whole range to prevent caching. */
   api_buffer *devmem0 = ctx->create_buffer(ctx, 2 * MAX_SIZE + 256, api_heap_device);
   api_buffer *devmem1 = ctx->create_buffer(ctx, 2 * MAX_SIZE + 256, api_heap_device);
   api_buffer *hostmem = ctx->create_buffer(ctx, 2 * MAX_SIZE + 256, api_heap_host_uncached);

   unsigned num_visited_tests = 0;

   /* Run tests. */
   for (unsigned test_flavor = 0; test_flavor < NUM_TESTS; test_flavor++) {
      bool is_copy = test_flavor >= TEST_COPY_DEVMEM_TO_DEVMEM;
      api_buffer *dst = test_flavor == TEST_FILL_DEVMEM ||
                        test_flavor == TEST_COPY_DEVMEM_TO_DEVMEM ||
                        test_flavor == TEST_COPY_HOSTMEM_TO_DEVMEM ? devmem0 :
                        test_flavor == TEST_FILL_HOSTMEM ||
                        test_flavor == TEST_COPY_DEVMEM_TO_HOSTMEM ? hostmem : NULL;
      api_buffer *src = test_flavor == TEST_COPY_DEVMEM_TO_DEVMEM ||
                        test_flavor == TEST_COPY_DEVMEM_TO_HOSTMEM ? devmem1 :
                        test_flavor == TEST_COPY_HOSTMEM_TO_DEVMEM ? hostmem : NULL;
      assert(dst);
      assert(!src || src != dst);

      if (!ctx->has_host_uncached_heap && (dst == hostmem || src == hostmem))
         continue;

      for (unsigned cached = 0; cached < 2; cached ++) {
         unsigned cycled_offset_base = 0;

         for (unsigned align = 0; align < NUM_ALIGNMENTS; align++) {
            unsigned test_src_offset = align_info[align].src_offset;
            unsigned test_dst_offset = align_info[align].dst_offset;

            /* Don't be unaligned by some number from 0 for <= 2 byte alignment. Shift the offset by 4. */
            if (test_src_offset && test_src_offset < 4)
               test_src_offset += 4;
            if (test_dst_offset && test_dst_offset < 4)
               test_dst_offset += 4;

            if (!is_copy && (test_src_offset != 0 || test_dst_offset % 4))
               continue;

            if (stage == REPORT) {
               char name[1024];

               snprintf(name, sizeof(name), "%s.%s.%s.%s", test_suite_name,
                        test_strings[test_flavor], cached ? "hit" : "miss",
                        align_info[align].string);
               printf("%-53s", name);
            }

            for (unsigned size = MIN_SIZE; size <= MAX_SIZE; size <<= SIZE_STEP_SHIFT) {
               /* Don't test large sizes with host memory because it can be too slow. */
               if (size > MAX_HOSTMEM_SIZE &&
                   (dst->heap != api_heap_device || (src && src->heap != api_heap_device))) {
                  if (stage == REPORT)
                     printf(",%8s", "n/a");
                  continue;
               }

               if (stage == COUNT_TESTS)
                  (*num_tests)++;

               if (stage == RUN) {
                  ctx->begin_cmdbuf(ctx);

                  for (unsigned iter = 0; iter < NUM_WARMUP_RUNS + NUM_RUNS; iter++) {
                     if (iter == NUM_WARMUP_RUNS)
                        ctx->write_next_timestamp(ctx, timestamps);

                     if ((cycled_offset_base + test_dst_offset + size > dst->size) ||
                         (is_copy && cycled_offset_base + test_src_offset + size > src->size))
                        cycled_offset_base = 0;

                     assert(cycled_offset_base + test_dst_offset + size <= dst->size);
                     assert(!is_copy || cycled_offset_base + test_src_offset + size <= src->size);
                     assert(cycled_offset_base % MAX_ALIGNMENT == 0);

                     if (is_copy) {
                        ctx->copy_buffer(ctx, dst, src,
                                         cycled_offset_base + test_dst_offset,
                                         cycled_offset_base + test_src_offset, size);
                     } else {
                        ctx->clear_buffer(ctx, dst, cycled_offset_base + test_dst_offset,
                                          size, 0x23456789);
                     }

                     if (!cached) {
                        /* Use a different portion of the buffers for each test, so that they don't just
                         * stay in the cache.
                         */
                        cycled_offset_base += size;
                        cycled_offset_base = ALIGN_POT(cycled_offset_base, MAX_ALIGNMENT);
                     }
                  }

                  ctx->write_next_timestamp(ctx, timestamps);
                  ctx->end_cmdbuf_and_submit(ctx);
               }

               if (stage == REPORT) {
                  /* When copying from DEVMEM to DEVMEM, we put 2x demand on DEVMEM bandwidth, and when
                   * copying between DEVMEM and HOSTMEM, we put only 1x demand on each, so only double
                   * the size for DEVMEM->DEVMEM copies to use the real DEVMEM bandwidth usage.
                   */
                  uint64_t num_bytes = (uint64_t)size * NUM_RUNS *
                                       (test_flavor == TEST_COPY_DEVMEM_TO_DEVMEM ? 2 : 1);
                  print_throughput_from_next_timestamps(ctx, timestamps, num_bytes, NULL, ",%8.2f");
               }

               if (stage == RUN)
                  print_progress(*num_tests, &num_visited_tests, 20);
            }

            if (stage == REPORT)
               puts("");
         }
      }
   }
}

void
test_buf_bandwidth(api_context *ctx, const char *test_suite_name)
{
   unsigned num_tests = 0;
   run(ctx, test_suite_name, COUNT_TESTS, NULL, &num_tests);

   /* Create timestamp queries. */
   api_timestamp_query_pool *timestamps =
      ctx->create_timestamp_pool(ctx, num_tests * 2);

   printf("Executing tests ...");
   run(ctx, test_suite_name, RUN, timestamps, &num_tests);

   puts("");
   puts("Reading back results...");
   ctx->query_timestamps(ctx, timestamps);

   puts("Units: GB/s");
   run(ctx, test_suite_name, REPORT, timestamps, &num_tests);
}
