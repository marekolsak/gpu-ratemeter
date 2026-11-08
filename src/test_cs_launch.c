/* Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "common.h"

#define MAX_INVOCATIONS                   (1 << 21)
#define MAX_DISPATCHES                    (1 << 13)

#define MIN_WG_SIZE                       32
#define MAX_WG_SIZE                       1024
#define NUM_INVOC_PER_DISPATCH_STEP_MUL   4

#define NUM_WG_SIZES       (1 + LOG2_POT(MAX_WG_SIZE) - LOG2_POT(MIN_WG_SIZE))


typedef struct {
   const char *name;
   unsigned num_invoc_ids;
   bool invoc_index;
   bool subgroup_ids;
} cl_sysval_info;

static cl_sysval_info sysvals[] = {
   {"local_invoc_id.x__", .num_invoc_ids = 1,   },
   {"local_invoc_id.xy_", .num_invoc_ids = 2,   },
   {"local_invoc_id.xyz", .num_invoc_ids = 3,   },
   {"local_invoc_index",  .invoc_index = true,  },
   {"subgroup_ids",       .subgroup_ids = true, },
};

typedef struct {
   unsigned num_tests;
   unsigned num_indirect_tests;
   api_query_pool *timestamps;

   api_descriptor_set_layout *layout;
   api_compute_pipeline *pipelines[NUM_WG_SIZES][3][ARRAY_SIZE(sysvals)][2];

   api_descriptor_set *set[2];
   api_image *store_image[2];

   api_buffer *indirect;
} cl_test_state;

/* Given a number (of invocations, workgroups, etc.), generate a 3D shape for that size. */
static void
size_to_dim(unsigned num_dim, unsigned size, unsigned *w, unsigned *h, unsigned *d)
{
   assert(IS_POT(size));
   assert(num_dim >= 1 && num_dim <= 3);

   unsigned log_size = LOG2_POT(size);

   if (num_dim == 1) {
      *w = 1 << log_size;
      *h = 1;
      *d = 1;
   } else if (num_dim == 2) {
      *w = 1 << (log_size / 2);
      *h = 1 << (log_size - log_size / 2);
      *d = 1;
   } else if (num_dim == 3) {
      *w = 1 << (log_size / 3);
      *h = 1 << (log_size / 3);
      *d = 1 << (log_size - 2 * (log_size / 3));
   }
}

static api_shader *
create_cs(api_context *ctx, unsigned wg_size, unsigned num_wg_ids, const cl_sysval_info *info,
          bool second_variant)
{
   char source[1200];
   unsigned wg_size_x, wg_size_y, wg_size_z;

   size_to_dim(MAX2(info->num_invoc_ids, 1), wg_size, &wg_size_x, &wg_size_y, &wg_size_z);

   assert(num_wg_ids >= 1 && num_wg_ids <= 3);

   int len = snprintf(source, ARRAY_SIZE(source),
                      "#version 450 \n"
                      "#extension GL_KHR_shader_subgroup_basic : require \n"
                      "\n"

                      "#define NUM_INVOC_IDS %u \n"
                      "#define NUM_WG_IDS %u \n"
                      "#define SG_IDS %u \n"
                      "#define INVOC_INDEX %u \n"
                      "#define WG_SIZE_X %u \n"
                      "#define WG_SIZE_Y %u \n"
                      "#define WG_SIZE_Z %u \n"
                      "#define SECOND_VARIANT %u \n"
                      "\n"

                      "layout(local_size_x = WG_SIZE_X, local_size_y = WG_SIZE_Y, local_size_z = WG_SIZE_Z) in; \n"
                      "\n"

                      "#ifdef VULKAN \n"
                      "   layout(set = 0, binding = 0) writeonly uniform uimage1D image; \n"
                      "#else \n"
                      "   layout(location = 0) writeonly uniform uimage1D image; \n"
                      "#endif \n"
                      "\n"

                      "void main() { \n"
                      "   uint value = 0; \n"

                      "   value += NUM_INVOC_IDS >= 1 ? gl_LocalInvocationID.x : 0u; \n"
                      "   value += NUM_INVOC_IDS >= 2 ? gl_LocalInvocationID.y : 0u; \n"
                      "   value += NUM_INVOC_IDS == 3 ? gl_LocalInvocationID.z : 0u; \n"

                      "   value += NUM_WG_IDS >= 1 ? gl_WorkGroupID.x : 0u; \n"
                      "   value += NUM_WG_IDS >= 2 ? gl_WorkGroupID.y : 0u; \n"
                      "   value += NUM_WG_IDS == 3 ? gl_WorkGroupID.z : 0u; \n"

                      "   value += SG_IDS != 0 ? gl_SubgroupID + gl_SubgroupInvocationID : 0u; \n"
                      "   value += INVOC_INDEX != 0 ? gl_LocalInvocationIndex : 0u; \n"

                      "   uvec4 v; \n"
                      "   v.x = value; \n" /* keep all other components undefined */

                      /* The purpose of this dummy out-of-bounds image store is to keep sysvals live. */
                      "   imageStore(image, 20 + (SECOND_VARIANT != 0 ? 1 : 0), v); \n"
                      "} \n",
                      info->num_invoc_ids, num_wg_ids, info->subgroup_ids, info->invoc_index,
                      wg_size_x, wg_size_y, wg_size_z, second_variant);

   assert(len < ARRAY_SIZE(source));

   return ctx->create_shader(ctx, source, api_shader_cs);
}

static void
get_test_name(char *out, int len, api_queue_type queue, bool indirect, unsigned wg_size,
              unsigned num_wg_ids, unsigned sysval_index, bool shader_bind_in_between,
              bool desc_bind_in_between)
{
   static const char *num_wg_ids_str[] = {
      "x__",
      "xy_",
      "xyz",
   };

   snprintf(out, len, "%s.%s.wg%u.wg_id.%s.%s.%s",
            queue == api_queue_gfx ? "gfx" : queue == api_queue_compute ? "comp" : "ERROR",
            indirect ? "indirect" : "direct",
            wg_size,
            num_wg_ids_str[num_wg_ids - 1],
            sysvals[sysval_index].name,
            shader_bind_in_between && desc_bind_in_between ? "bind:shader+desc" :
               shader_bind_in_between ? "bind:shader" :
               desc_bind_in_between ? "bind:desc" : "bind:none");
}

#define DISPATCH_PARAM_BYTES  12

static void
run(api_context *ctx, test_stage stage, cl_test_state *state)
{
   const unsigned name_indent = 70;
   unsigned test_index = 0;
   unsigned indirect_test_index = 0;
   atomic_uint num_visited_tests = 0;
   uint32_t *indirect_data = NULL;

   switch (stage) {
   case COUNT_TESTS:
      break;

   case INIT:
      state->layout = ctx->create_descriptor_set_layout(ctx,
                                                        &(api_descriptor_set_layout_desc) {
                                                           .storage_image[0].array_size = 1,
                                                        });

      for (unsigned i = 0; i < 2; i++) {
         state->store_image[i] = ctx->create_image(ctx, VK_IMAGE_TYPE_1D, VK_FORMAT_R32_UINT, 16, 1,
                                                   1, 1, VK_IMAGE_TILING_OPTIMAL, api_heap_device,
                                                   VK_IMAGE_USAGE_STORAGE_BIT);
         state->set[i] = ctx->create_descriptor_set(ctx, state->layout);
         ctx->set_storage_image_descriptors(ctx, api_queue_gfx, state->set[i], 0, 1, &state->store_image[i]);
      }

      indirect_data = malloc(DISPATCH_PARAM_BYTES * state->num_indirect_tests);
      break;

   case RUN:
      break;

   case REPORT:
      fprintf(ctx->output, "%-*s", (int)strlen(ctx->options.name_prefix) + 1 + name_indent, "Invocations per dispatch");

      for (unsigned num_invoc = MIN_WG_SIZE; num_invoc <= MAX_INVOCATIONS;
           num_invoc *= NUM_INVOC_PER_DISPATCH_STEP_MUL)
         fprintf(ctx->output, ",%8u", num_invoc);

      fprintf(ctx->output, "\n");
      break;

   default:
      assert(0);
   }

   for (api_queue_type queue = api_queue_gfx; queue <= api_queue_compute; queue++) {
      if (!ctx->has_queue[queue])
         continue;
      for (unsigned indirect = 0; indirect < 2; indirect++) {
         for (unsigned wg_size = MIN_WG_SIZE; wg_size <= MAX_WG_SIZE; wg_size *= 2) {
            for (unsigned num_wg_ids = 1; num_wg_ids <= 3; num_wg_ids++) {
               for (unsigned sysval_index = 0; sysval_index < ARRAY_SIZE(sysvals); sysval_index++) {
                  for (unsigned shader_bind_in_between = 0; shader_bind_in_between < 2; shader_bind_in_between++) {
                     for (unsigned desc_bind_in_between = 0; desc_bind_in_between < 2; desc_bind_in_between++) {
                        const unsigned wg_size_index = LOG2_POT(wg_size) - LOG2_POT(MIN_WG_SIZE);
                        api_compute_pipeline **p =
                           &state->pipelines[wg_size_index][num_wg_ids - 1][sysval_index][0];
                        char subtest[128];

                        get_test_name(subtest, sizeof(subtest), queue, indirect, wg_size, num_wg_ids, sysval_index,
                                      shader_bind_in_between, desc_bind_in_between);

                        if (ctx->options.regex_subtest_filter &&
                            !regex_matches(ctx->options.regex_subtest_filter, subtest))
                           continue;

                        if (stage == INIT) {
                           if (!p[0]) {
                              api_shader *cs0 = create_cs(ctx, wg_size, num_wg_ids,
                                                                         &sysvals[sysval_index], false);
                              api_shader *cs1 = create_cs(ctx, wg_size, num_wg_ids,
                                                                         &sysvals[sysval_index], true);
                              p[0] = ctx->create_compute_pipeline(ctx, cs0, state->layout);
                              p[1] = ctx->create_compute_pipeline(ctx, cs1, state->layout);
                              assert(p[0]->subgroup_size == p[1]->subgroup_size);
                           }
                        }

                        if (stage == REPORT)
                           fprintf(ctx->output, "%s.%-*s", ctx->options.name_prefix, name_indent, subtest);

                        for (unsigned num_invoc_per_dispatch = MIN_WG_SIZE; num_invoc_per_dispatch <= MAX_INVOCATIONS;
                             num_invoc_per_dispatch *= NUM_INVOC_PER_DISPATCH_STEP_MUL) {
                           if (num_invoc_per_dispatch < wg_size) {
                              if (stage == REPORT)
                                 fprintf(ctx->output, ",%8s", "n/a");
                              continue;
                           }

                           const unsigned num_dispatches = MIN2(MAX_INVOCATIONS / num_invoc_per_dispatch, MAX_DISPATCHES);
                           const unsigned num_wgs_per_dispatch = num_invoc_per_dispatch / wg_size;
                           unsigned w, h, d;

                           size_to_dim(num_wg_ids, num_wgs_per_dispatch, &w, &h, &d);

                           switch (stage) {
                           case COUNT_TESTS:
                              break;

                           case INIT:
                              if (indirect) {
                                 indirect_data[indirect_test_index * 3 + 0] = w;
                                 indirect_data[indirect_test_index * 3 + 1] = h;
                                 indirect_data[indirect_test_index * 3 + 2] = d;
                              }
                              break;

                           case RUN: {
                              ctx->begin_cmdbuf(ctx, queue);
                              if (!shader_bind_in_between)
                                 ctx->bind_compute_pipeline(ctx, p[0]);
                              if (!desc_bind_in_between)
                                 ctx->bind_descriptor_set(ctx, state->set[0]);

                              ctx->write_next_query_value(ctx, state->timestamps);

                              for (unsigned i = 0; i < num_dispatches; i++) {
                                 if (shader_bind_in_between)
                                    ctx->bind_compute_pipeline(ctx, p[i % 2]);
                                 if (desc_bind_in_between)
                                    ctx->bind_descriptor_set(ctx, state->set[i % 2]);

                                 if (indirect) {
                                    ctx->dispatch_indirect(ctx, state->indirect,
                                                           DISPATCH_PARAM_BYTES * indirect_test_index);
                                 } else {
                                    ctx->dispatch(ctx, w, h, d);
                                 }
                              }

                              ctx->write_next_query_value(ctx, state->timestamps);
                              ctx->end_cmdbuf_and_submit(ctx, 0, NULL, NULL);

                              print_progress(state->num_tests, &num_visited_tests, 20);
                              break;
                           }

                           case REPORT: {
                              double num_items;

                              if (ctx->options.report_time_per_invocation_count) {
                                 num_items = (double)(num_dispatches * num_invoc_per_dispatch) /
                                             ctx->options.report_time_per_invocation_count;
                              } else if (ctx->options.subgroup_time) {
                                 /* If the workgroup size is less than the subgroup size, count one
                                  * workgroup as one subgroup.
                                  */
                                 const unsigned num_subgroups_per_dispatch =
                                    wg_size < p[0]->subgroup_size ? num_wgs_per_dispatch :
                                                                    num_invoc_per_dispatch / p[0]->subgroup_size;

                                 num_items = num_dispatches * num_subgroups_per_dispatch;
                              } else {
                                 num_items = num_dispatches;
                              }

                              print_result_from_next_timestamps(ctx, state->timestamps, num_items,
                                                                NULL, NULL, "%8.2f", "%8s", 0);
                              break;
                           }

                           default:
                              assert(0);
                           }

                           test_index++;
                           if (indirect)
                              indirect_test_index++;
                        }

                        if (stage == REPORT)
                           fprintf(ctx->output, "\n");
                     }
                  }
               }
            }
         }
      }
   }

   switch (stage) {
   case COUNT_TESTS:
      state->num_tests = test_index;
      state->num_indirect_tests = indirect_test_index;
      break;

   case INIT:
      state->timestamps =
            ctx->create_query_pool(ctx, api_queue_gfx, state->num_tests * 2, api_query_timestamp);

      if (state->num_indirect_tests) {
         state->indirect = ctx->create_buffer(ctx, state->num_indirect_tests * DISPATCH_PARAM_BYTES,
                                              api_heap_device, 0);
         ctx->upload_buffer_data(ctx, api_queue_gfx, state->indirect, 0, state->indirect->size,
                                 indirect_data);
         ctx->wait_for_idle(ctx);
      }
      free(indirect_data);
      break;

   case RUN:
      ctx->get_query_results(ctx, state->timestamps);
      break;

   case REPORT:
      break;

   default:
      assert(0);
   }
}

void
test_cs_launch(api_context *ctx)
{
   cl_test_state state = {0};

   if (!ctx->has_shader_subgroup_ops)
      error("Support for shader subgroup operations is required.");

   if (ctx->options.subgroup_time && !ctx->has_pipeline_subgroup_size_query)
      error("Subgroup size query support required. (via VK_KHR_pipeline_executable_properties or equivalent)");

   if (ctx->options.subgroup_time && ctx->options.report_time_per_invocation_count)
      error("Only one option can be present: -subgrouptime or -timeperinvocs=N");

   ctx->options.report_time = true;

   run(ctx, COUNT_TESTS, &state);

   if (!state.num_tests)
      return;

   puts("Building compute pipelines...");
   run(ctx, INIT, &state);

   printf("Executing tests ...");
   fflush(stdout);
   run(ctx, RUN, &state);
   puts("");

   run(ctx, REPORT, &state);
}
