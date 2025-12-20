# Copyright 2026 Advanced Micro Devices, Inc.
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
dep_egl = dependency('egl')
dep_libpng = dependency('libpng')
dep_shaderc = dependency('shaderc')
dep_vulkan = dependency('vulkan')

gpu_ratemeter_files = files(
  'src/glad/glad.c',
  'src/glad/glad.h',
  'src/glad/khrplatform.h',

  'src/api_d3d11.c',
  'src/api_d3d12.c',
  'src/api_gl.c',
  'src/api_vk.c',
  'src/common.h',
  'src/main.c',
  'src/test_bufbw.c',
  'src/test_pix_rate.c',
  'src/test_prim_rate.c',
  'src/test_sanity.c',
  'src/utils.c',
)

all_args = [
  '-fno-omit-frame-pointer',

  '-Werror=int-conversion',
  '-Werror=format',
  '-Werror=return-type',
  '-Wall',
  '-Wno-unused-variable',
  '-Wno-unused-but-set-variable',
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

gpu_ratemeter = executable(
  'gpu-ratemeter',
  [gpu_ratemeter_files],
  c_args : cc.get_supported_arguments(all_args + c_args),
  cpp_args : cpp.get_supported_arguments(all_args),
  dependencies : [dep_m, dep_egl, dep_libpng, dep_vulkan, dep_shaderc],
)
