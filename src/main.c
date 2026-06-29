/* Copyright 2026 Advanced Micro Devices, Inc.
 * Copyright 2026 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

/* TODO:
 *
 * Tests:
 * - iobw: shader input and output throughput in GB/s, including transform feedback (port shader-io-rate)
 * - bufbwtiny - tiny buffer clears and copies (4-32 B), clocks/dword, clocks/command
 * - rt: ray tracing performance in rays/clock
 * - mma: matrix multiply accumulate
 * - draw: direct/indirect draw/multidraw clocks per draw
 * - compute: launched compute shader invocations per clock, clocks per dispatch
 * - sampler: image load and filter rate
 *
 * APIs:
 * - DX12
 * - DX11
 *
 * bufbw:
 * - VK_KHR_copy_memory_indirect
 *
 * imgbw:
 * - write README
 * - clears:
 *    - vkCmdBeginRenderPass (MRTs + Z/S), vkCmdClearAttachments, vkCmdClearDepthStencilImage
 *    - glClear (MRTs + Z/S) with and without scissor, glClearBuffer
 * - resolve:
 *    - pResolveAttachments via a render pass
 * - 2D array images
 * - Z/S?
 * - VK_KHR_copy_memory_indirect
 * - transfer queue
 *
 * latency:
 * - instruction fetch (jump chasing)
 * - to remove LSB masking, add -bda as alternative to SSBOs
 *
 * pix:
 * - (maybe) VK_NV_fill_rectangle as a raster subtest
 * - (maybe) VK_KHR_fragment_shader_barycentric / GL_EXT_fragment_shader_barycentric
 * - (maybe) stencil/HiS
 * - (maybe) using glSubgroupInvocationID instead of gl_HelperInvocation might be better on some HW.
 *
 * prim:
 * - VS + transform feedback
 * - GS passthrough + transform feedback
 * - GS passthrough
 * - GS that only emits max_vertices / 2, max_vertices / 4, etc.
 * - GS that only broadcasts one triangle to multiple layers
 * - TES?
 * - GL clip planes
 * - multiview
 * - task shader (test a low number of mesh workgroups per TS invocation)
 * - fill the screen in the Morton order instead of linearly
 * - 1-primitive instances/clock
 * - 1-primitive draws in a multidraw, prims/clock == draws/clock
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

static const char *
get_substring_before_dot(const char *input, char *output, unsigned output_max_size)
{
   if (!input) {
      output[0] = 0;
      return NULL;
   }

   const char *dot = strchr(input, '.');

   if (dot == input) {
      output[0] = 0;
      return dot + 1;
   }

   if (dot) {
      unsigned len = dot - input + 1;
      snprintf(output, output_max_size < len ? output_max_size : len, "%s", input);
      return dot + 1;
   } else {
      snprintf(output, output_max_size, "%s", input);
      return NULL;
   }
}

static const struct {
   const char *name;
   api_context *(*create_context)(const program_options *options);
   api_flags api_flags;
} apis[] = {
   {"d3d11", d3d11_create_context},
   {"d3d12", d3d12_create_context},
   {"gl", gl_create_context},
   {"vk", vk_create_context, 0},
   {"vkd", vk_create_context, API_VK_DYNAMIC_STATE},
   {"vkl", vk_create_context, API_VK_GPL},
   {"vkld", vk_create_context, API_VK_GPL | API_VK_DYNAMIC_STATE},
};

static const struct {
   const char *name;
   void (*execute)(api_context *ctx, const char *test_name);
   bool report_bandwidth;
} tests[] = {
   {"bufbw", test_bufbw, true},
   {"imgbw", test_imgbw, true},
   {"latency", test_latency},
   {"pix", test_pix},
   {"pixbw", test_pix, true},
   {"prim", test_prim},
   {"sanity", test_sanity},
   {"sparsebind", test_sparsebind, true},
};

typedef enum {
   OPTION_BOOL,
   OPTION_UINT,
   OPTION_STRING,
   OPTION_MEMSIZE,
} option_type;

typedef struct {
   option_type type;
   const char *name;
   size_t offset;
} option_desc;

static const option_desc option_list[] = {
   {OPTION_BOOL, "-bda", offsetof(program_options, bda)},
   {OPTION_BOOL, "-compute", offsetof(program_options, compute)},
   {OPTION_BOOL, "-int8", offsetof(program_options, int8)},
   {OPTION_BOOL, "-lean", offsetof(program_options, lean)},
   {OPTION_BOOL, "-no-validator", offsetof(program_options, no_validator)},
   {OPTION_BOOL, "-rdna4ts", offsetof(program_options, rdna4_timestamp_wa)},
   {OPTION_BOOL, "-sparse-bound", offsetof(program_options, sparse_bound)},
   {OPTION_BOOL, "-sparse-unbound", offsetof(program_options, sparse_unbound)},
   {OPTION_BOOL, "-transfer", offsetof(program_options, transfer)},

   {OPTION_UINT, "-clockbits=", offsetof(program_options, clock_bits)},
   {OPTION_UINT, "-device=", offsetof(program_options, device)},
   {OPTION_UINT, "-freq=", offsetof(program_options, freq_mhz)},
   {OPTION_UINT, "-maxrate=", offsetof(program_options, max_rate)},
   {OPTION_UINT, "-maxvalidresult=", offsetof(program_options, max_valid_result)},

   {OPTION_STRING, "-filter=", offsetof(program_options, filter)},
   {OPTION_STRING, "-format=", offsetof(program_options, format)},
   {OPTION_STRING, "-subset=", offsetof(program_options, subset)},

   {OPTION_MEMSIZE, "-spacing=", offsetof(program_options, spacing)},
   {OPTION_MEMSIZE, "-maxsize=", offsetof(program_options, max_size)},
};

int
main(int argc, char **argv)
{
   program_options options = {0};

   for (unsigned i = 1; i < argc - 1; i++) {
      bool valid = false;

      for (unsigned o = 0; o < ARRAY_SIZE(option_list); o++) {
         unsigned len = option_list[o].type != OPTION_BOOL ? strlen(option_list[o].name) : 0;

         switch (option_list[o].type) {
         case OPTION_BOOL:
            if (!strcmp(argv[i], option_list[o].name)) {
               *((uint8_t*)&options + option_list[o].offset) = true;
               valid = true;
            }
            break;
         case OPTION_UINT:
            if (!strncmp(argv[i], option_list[o].name, len)) {
               sscanf(argv[i] + len, "%u",
                      (unsigned*)((uint8_t*)&options + option_list[o].offset));
               valid = true;
            }
            break;
         case OPTION_STRING:
            if (!strncmp(argv[i], option_list[o].name, len)) {
               *(const char**)((uint8_t*)&options + option_list[o].offset) = argv[i] + len;
               valid = true;
            }
            break;
         case OPTION_MEMSIZE:
            if (!strncmp(argv[i], option_list[o].name, len)) {
               uint64_t *value = (uint64_t*)((uint8_t*)&options + option_list[o].offset);
               const char *arg = argv[i] + len;
               char si_prefix = arg[strlen(arg) - 1];
               unsigned order = si_prefix == 'K' ? 1 :
                                si_prefix == 'M' ? 2 :
                                si_prefix == 'G' ? 3 :
                                si_prefix == 'T' ? 4 : 0;
               sscanf(arg, "%"PRIu64, value);
               *value *= 1ull << (10 * order);
               valid = true;
            }
            break;
         }
         if (valid)
            break;
      }

      if (!valid)
         error("Unknown option: %s", argv[i]);
   }

   api_context *ctx = NULL;
   const char *name = NULL;

   if (argc >= 2)
      name = argv[argc - 1];
   else
      error("Test name missing in parameters");

   printf("Test name: %s\n", name);

   char api[16];
   name = get_substring_before_dot(name, api, sizeof(api));

   puts("Initializing API...");

   for (unsigned i = 0; i < ARRAY_SIZE(apis); i++) {
      if (!strcmp(api, apis[i].name)) {
         options.api_flags = apis[i].api_flags;
         ctx = apis[i].create_context(&options);
         break;
      }
   }

   if (!ctx)
      error("Invalid API or API selection missing in parameters: %s", api);

   char test[16], test_name[32];
   get_substring_before_dot(name, test, sizeof(test));

   snprintf(test_name, sizeof(test_name), "%s.%s", api, test);

   printf("Starting %s...\n", test_name);

   bool executed = false;
   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (!strcmp(test, tests[i].name)) {
         ctx->options.report_bandwidth = tests[i].report_bandwidth;
         tests[i].execute(ctx, test_name);
         executed = true;
         break;
      }
   }

   if (!executed)
      error("Invalid test name or test name missing in parameters: %s", test);

   return 0;
}
