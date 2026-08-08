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
 * imgbw:
 * - 2D array images
 * - transfer queue
 * - buffer-to-image, image-to-buffer
 * - indirect buffer-to-image (VK_KHR_copy_memory_indirect)
 * - no barrier, uncached
 *
 * latency:
 * - instruction fetch (jump chasing)
 *
 * pix:
 * - test layer output with 2 layers (1-layer FS isn't comparable with radeonsi since radeonsi always removes it)
 * - raster tests with FS inputs to test RDNA parameter cache/attribute ring overhead
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
 * - add cyclesN tests
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

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

typedef struct {
   const char *name;
   void (*execute)(api_context *ctx);
   bool report_bandwidth;
   unsigned num_options;
   const option_desc *options;
} test_desc;

static const option_desc common_options[] = {
   /* Common to all tests. */
   {OPTION_UINT, "-baserate=", offsetof(program_options, base_rate)},
   {OPTION_UINT, "-maxvalidresult=", offsetof(program_options, max_valid_result)},

   /* OpenGL only */
   {OPTION_BOOL, "-gl-tiling-linear", offsetof(program_options, gl_tiling_linear)},

   /* Vulkan only */
   {OPTION_BOOL, "-no-validator", offsetof(program_options, no_validator)},
   {OPTION_UINT, "-device=", offsetof(program_options, device)},
};

static const option_desc bufbw_options[] = {
   {OPTION_BOOL, "-compute", offsetof(program_options, compute)},
   {OPTION_BOOL, "-transfer", offsetof(program_options, transfer)},
   {OPTION_STRING, "-filter=", offsetof(program_options, filter)},
};

static const option_desc imgbw_options[] = {
   {OPTION_BOOL, "-rdna4ts", offsetof(program_options, rdna4_timestamp_wa)},
   {OPTION_STRING, "-filter=", offsetof(program_options, filter)},
};

static const option_desc latency_options[] = {
   {OPTION_BOOL, "-bda", offsetof(program_options, bda)},
   {OPTION_BOOL, "-int8", offsetof(program_options, int8)},
   {OPTION_BOOL, "-sparse-bound", offsetof(program_options, sparse_bound)},
   {OPTION_BOOL, "-sparse-unbound", offsetof(program_options, sparse_unbound)},
   {OPTION_UINT, "-clockbits=", offsetof(program_options, clock_bits)},
   {OPTION_MEMSIZE, "-spacing=", offsetof(program_options, spacing)},
   {OPTION_MEMSIZE, "-maxsize=", offsetof(program_options, max_size)},
};

static const option_desc pix_options[] = {
   {OPTION_BOOL, "-lean", offsetof(program_options, lean)},
   {OPTION_BOOL, "-rdna4ts", offsetof(program_options, rdna4_timestamp_wa)},
   {OPTION_BOOL, "-samplerate", offsetof(program_options, samplerate)},
   {OPTION_UINT, "-freq=", offsetof(program_options, freq_mhz)},
   {OPTION_STRING, "-format=", offsetof(program_options, format)},
};

static const option_desc prim_options[] = {
   {OPTION_UINT, "-freq=", offsetof(program_options, freq_mhz)},
   {OPTION_STRING, "-filter=", offsetof(program_options, filter)},
};

static const option_desc sparsebind_options[] = {
};

static const test_desc tests[] = {
   {"bufbw", test_bufbw, true, ARRAY_SIZE(bufbw_options), bufbw_options},
   {"imgbw", test_imgbw, true, ARRAY_SIZE(imgbw_options), imgbw_options},
   {"latency", test_latency, false, ARRAY_SIZE(latency_options), latency_options},
   {"pix", test_pix, false, ARRAY_SIZE(pix_options), pix_options},
   {"pixbw", test_pix, true, ARRAY_SIZE(pix_options), pix_options},
   {"prim", test_prim, false, ARRAY_SIZE(prim_options), prim_options},
   {"sanity", test_sanity, false, 0, NULL},
   {"sparsebind", test_sparsebind, true, ARRAY_SIZE(sparsebind_options), sparsebind_options},
};

static bool
parse_option(program_options *options, const option_desc *option, const char *arg)
{
   unsigned len = option->type != OPTION_BOOL ? strlen(option->name) : 0;
   assert(option->offset < sizeof(program_options));

   switch (option->type) {
   case OPTION_BOOL:
      if (!strcmp(arg, option->name)) {
         *((uint8_t*)options + option->offset) = true;
         return true;
      }
      return false;
   case OPTION_UINT:
      if (!strncmp(arg, option->name, len)) {
         sscanf(arg + len, "%u",
                (unsigned*)((uint8_t*)options + option->offset));
         return true;
      }
      return false;
   case OPTION_STRING:
      if (!strncmp(arg, option->name, len)) {
         *(const char**)((uint8_t*)options + option->offset) = arg + len;
         return true;
      }
      return false;
   case OPTION_MEMSIZE:
      if (!strncmp(arg, option->name, len)) {
         uint64_t *value = (uint64_t*)((uint8_t*)options + option->offset);
         const char *tmp_arg = arg + len;
         char si_prefix = tmp_arg[strlen(tmp_arg) - 1];
         unsigned order = si_prefix == 'K' ? 1 :
                          si_prefix == 'M' ? 2 :
                          si_prefix == 'G' ? 3 :
                          si_prefix == 'T' ? 4 : 0;
         sscanf(tmp_arg, "%"PRIu64, value);
         *value *= 1ull << (10 * order);
         return true;
      }
      return false;
   default:
      error("fatal error: invalid option type");
   }
}

static bool
parse_option_list(program_options *options, unsigned num_options, const option_desc *option_list,
                  const char *arg)
{
   for (unsigned i = 0; i < num_options; i++) {
      if (parse_option(options, &option_list[i], arg))
         return true;
   }

   return false;
}

int
main(int argc, char **argv)
{
   /* Remove the executable name. */
   argv++;
   argc--;

   if (!argc)
      error("The test name is missing in parameters. (the test name must be last)");

   const char *name_arg = name_arg = argv[argc - 1];
   argc--;

   /* Parse the test name. */
   const void *re_split2 = regex_compile("^([^.]+)\\.([^.]+)$");
   const void *re_split3 = regex_compile("^([^.]+)\\.([^.]+)\\.(.*)$");
   char *re_groups[3];
   unsigned num_re_groups;

   num_re_groups = regex_groups(re_split2, name_arg, re_groups, ARRAY_SIZE(re_groups));
   if (num_re_groups != 2) {
      for (unsigned i = 0; i < num_re_groups; i++)
         free(re_groups[i]);

      num_re_groups = regex_groups(re_split3, name_arg, re_groups, ARRAY_SIZE(re_groups));
   }

   if (num_re_groups < 2)
      error("Cannot parse the test name: %s (the form should be api.test[.regex])\n", name_arg);

   const char *api_name = re_groups[0];
   const char *test_name = re_groups[1];
   const char *filter = num_re_groups == 3 ? re_groups[2] : NULL;
   char name_prefix[32];
   snprintf(name_prefix, sizeof(name_prefix), "%s.%s", api_name, test_name);

   const test_desc *test_desc = NULL;

   for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
      if (!strcmp(test_name, tests[i].name)) {
         test_desc = &tests[i];
         break;
      }
   }

   if (!test_desc) {
      error("Invalid test name or test name missing in parameters: %s (the test name must be last)",
            test_name);
   }

   printf("Test name: %s\n", name_prefix);

   /* Parse options. */
   program_options options = {
      .report_bandwidth = test_desc->report_bandwidth,
   };

   snprintf(options.name_prefix, sizeof(options.name_prefix), "%s.%s", api_name, test_name);
   options.regex_subtest_filter = filter ? regex_compile(filter) : NULL;

   for (unsigned i = 0; i < argc; i++) {
      if (!parse_option_list(&options, ARRAY_SIZE(common_options), common_options, argv[i]) &&
          !parse_option_list(&options, test_desc->num_options, test_desc->options, argv[i])) {
         printf("Unknown option: %s\n", argv[i]);
         puts("");
         puts("Valid options:");

         for (unsigned j = 0; j < ARRAY_SIZE(common_options); j++)
            printf("  %s\n", common_options[j].name);
         for (unsigned j = 0; j < test_desc->num_options; j++)
            printf("  %s\n", test_desc->options[j].name);

         error(" ");
      }
   }

   puts("Initializing API...");
   api_context *ctx = NULL;

   for (unsigned i = 0; i < ARRAY_SIZE(apis); i++) {
      if (!strcmp(api_name, apis[i].name)) {
         options.api_flags |= apis[i].api_flags;
         ctx = apis[i].create_context(&options);
         break;
      }
   }

   if (!ctx)
      error("Invalid API or API selection missing in parameters: %s", api_name);

   for (unsigned i = 0; i < num_re_groups; i++)
      free(re_groups[i]);

   puts("Initializing test...");
   test_desc->execute(ctx);
   return 0;
}
