# Copyright 2026 Advanced Micro Devices, Inc.
# Copyright 2026 Valve Corporation
# SPDX-License-Identifier: MIT

project('gpu-ratemeter', ['c', 'cpp'], version : '0.1', license : 'MIT',
        default_options : [
          'c_std=c11',
          'cpp_std=c++17',
          'buildtype=debugoptimized'
        ])

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

dep_m = cc.find_library('m', required : false)
dep_pcre2 = dependency('libpcre2-8')
dep_libpng = dependency('libpng')
dep_openmp = dependency('openmp')

dep_egl = dependency('egl')
dep_shaderc = dependency('shaderc')
dep_vulkan = dependency('vulkan')

dep_vk_util_headers = dependency(
  'VulkanUtilityLibraries',
  method : 'cmake',
  modules : ['Vulkan::UtilityHeaders'],
  required : false,
)

gpu_ratemeter_files = files(
  'src/glad/glad.c',
  'src/glad/glad.h',
  'src/glad/khrplatform.h',

  'src/extras/fast_urem_by_const.h',
  'src/extras/hash_table.c',
  'src/extras/hash_table.h',

  'src/api_d3d11.c',
  'src/api_d3d12.c',
  'src/api_gl.c',
  'src/api_vk.c',
  'src/common.h',
  'src/main.c',
  'src/test_bufbw.c',
  'src/test_imgbw.c',
  'src/test_latency.c',
  'src/test_pix.c',
  'src/test_prim.c',
  'src/test_sanity.c',
  'src/test_sparsebind.c',
  'src/utils.c',
)

all_args = [
  '-fno-omit-frame-pointer',

  '-Werror=int-conversion',
  '-Werror=format',
  '-Werror=return-type',
  '-Wall',
  '-Wformat-security',

  # For MSVC compatiblity.
  '-Werror=pointer-arith',
  '-Werror=vla',
  '-Werror=gnu-empty-initializer',
  '-Wgnu-pointer-arith',
]

c_args = [
  '-Werror=implicit-function-declaration',
  '-Werror=missing-prototypes',
  '-Werror=incompatible-pointer-types',
]

if dep_vk_util_headers.found()
  all_args += ['-DHAVE_VK_UTILITY_HEADERS']
endif

gpu_ratemeter = executable(
  'gpu-ratemeter',
  [gpu_ratemeter_files],
  c_args : cc.get_supported_arguments(all_args + c_args),
  cpp_args : cpp.get_supported_arguments(all_args),
  dependencies : [dep_m, dep_pcre2, dep_libpng, dep_openmp,
                  dep_egl, dep_shaderc, dep_vulkan],
)
