/* Copyright 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

/* TODO:
 *
 * Test suites:
 * - iobw (port shader-io-rate)
 * - bufbwtiny - tiny buffer clears and copies (4-32 B), clocks/dword
 * - rt
 * - mma
 * - draw: direct/indirect draw/multidraw clocks per draw
 * - compute: launched compute shader invocations per clock, clocks per dispatch
 * - sampler: image load and filter rate
 *
 * APIs:
 * - DX12
 * - Vulkan modes: 1) all states are dynamic, 2) same as 1 but also using VK_EXT_graphics_pipeline_library
 * - DX11
 * - CUDA? (for ML)
 * - OpenCL?
 *
 * bufbw:
 * - VK_KHR_copy_memory_indirect
 *
 * imgbw:
 * - clears:
 *    - vkCmdBeginRenderPass (MRTs + Z/S), vkCmdClearAttachments, vkCmdClearDepthStencilImage
 *    - glClear (MRTs + Z/S) with and without scissor, glClearBuffer
 * - resolve:
 *    - pResolveAttachments via a render pass
 * - 2D array images
 * - Z/S?
 * - VK_KHR_copy_memory_indirect
 *
 * pix:
 * - multiview
 * - render to 3D texture?
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
 * - task shader
 *
 * raster:
 * - test rasterizer efficiency with different screen-space triangle sizes (similar to pix)
 *
 * AMD TODO:
 * - pix: timestamp queries are broken on gfx12 with Z/samplemask/A2C outputs and 128bpp formats, test older gens
 */

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
   unsigned api_flavor;
} apis[] = {
   {"d3d11", d3d11_create_context},
   {"d3d12", d3d12_create_context},
   {"gl", gl_create_context},
   {"vk", vk_create_context, API_VK_CORE},
   {"vk_dyn", vk_create_context, API_VK_DYNAMIC_STATE},
};

static const struct {
   const char *name;
   void (*execute)(api_context *ctx, const char *test_suite_name);
   bool report_bandwidth;
} tests[] = {
   {"bufbw", test_buf_bandwidth, true},
   {"imgbw", test_img_bandwidth, true},
   {"pix", test_pix_rate},
   {"pixbw", test_pix_rate, true},
   {"prim", test_prim_rate},
   {"sanity", test_sanity},
};

int
main(int argc, char **argv)
{
   program_options options = {0};

   for (unsigned i = 1; i < argc - 1; i++) {
      if (!strncmp(argv[i], "-freq=", 6))
         sscanf(argv[i] + 6, "%u", &options.freq_mhz);
      else if (!strncmp(argv[i], "-maxrate=", 9))
         sscanf(argv[i] + 9, "%u", &options.max_rate);
      else if (!strcmp(argv[i], "-lean"))
         options.lean = true;
      else if (!strncmp(argv[i], "-filter=", 8))
         options.test_filter = argv[i] + 8;
      else if (!strncmp(argv[i], "-format=", 8))
         options.format_filter = argv[i] + 8;
      else
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
         options.api_flavor = apis[i].api_flavor;
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
