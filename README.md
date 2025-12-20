[[_TOC_]]

# Introduction and Project Vision

This is a command-line microbenchmark that measures the performance of various features of GPUs through APIs, and how well different GPUs, APIs, and API translation layers do well against each other.

It reports GPU performance in terms of pixels per clock, primitives per clock, vertices per clock, draws per clock, rays per clock, memory throughput, etc. with different combinations of pipeline states, shaders, and different types of draw/compute/blit operations to gather how raw GPU performance is affected by the choice of drivers (Windows / closed-source, Mesa), APIs (DX, GL, VK), and API translation layers (DXVK, VKD3D, Zink).

This helps driver and API translation layer developers compare the GPU performance when the same GPU is used with different drivers, APIs, API translation layers, and operating systems. It helps precisely identify root causes of performance differences and resolve them. The following comparisons can be made:
- Open source vs closed source driver
- Windows vs Linux vs Android
- Vulkan vs OpenGL vs Zink
- Vulkan vs DX12 vs VKD3D
- Vulkan vs DX11 vs DXVK
- Vulkan with graphics pipeline objects (all shaders and states known at pipeline creation) vs all dynamic state (states unknown at pipeline creation) vs graphics pipeline
libraries (shaders are compiled independently with no knowledge of states and other shaders)

For GPU vendors who know precise performance characteristics of their GPUs, this facilitates verification whether their drivers achieve the expected GPU design performance as per the HW design.

Ideally, performance should be identical between all APIs and API translation layers and reaching expected GPU design performance, however, **that's quite rare in practice**, and we are seeing **vastly different performance** between all of those such that it necessitates precise microbenchmarking to exactly identify inefficiencies and missing optimizations, and resolve them.

# How to Run

`gpu-ratemeter [optional parameters] [api].[test suite]`

The following APIs are supported:
- `d3d11`: Direct3D 11 **{- (not implemented yet) -}**
- `d3d12`: Direct3D 12 **{- (not implemented yet) -}**
- `gl`: OpenGL (linked shaders only)
- `vk`: Vulkan with regular pipeline objects
- `vk.dyn`: Vulkan with all dynamic state **{- (not implemented yet) -}**
- `vk.gpl`: Vulkan with graphics pipeline libraries **{- (not implemented yet) -}**

The following test suites are available:
- `bufbw`: buffer clears and copies in GB/s
- `imgbw`: image clears and copies in GB/s **{- (not implemented yet, import from radeonsi) -}**
- `iobw`: shader input and output throughput in GB/s, including transform feedback **{- (not implemented yet, import from piglit) -}**
- `pix`: pixels/clock, all tests are run with 1x MSAA and 8x MSAA
- `pixbw`: GB/s for color buffer writes, same tests as `pix`
- `prim`: primitives/clock
- `sanity`: verify that the API works by drawing an object and saving the result into a PNG file (the app is windowless)

> [!tip]
> Use GPU-specific tools like sysfs to set a constant GPU frequency to get consistent results and use the `-freq=N` parameter.

> [!tip]
> The output is a table in CSV. Paste it into a spreadsheet app to make easy comparisons between runs.

Optional parameters common to all test suites:
- `-freq=N`: the GPU frequency in MHz, which causes results to be reported in units/clock instead of billion units/second (ignored when reporting memory bandwidth)
- `-maxrate=N`: the maximum rate in units/clock, which causes results to be reported as % of the maximum rate instead of units/clock (ignored when reporting memory bandwidth)

Example: `gpu-ratemeter -freq=2390 -maxrate=128 vk.pix` measures pixel thoughput with Vulkan and reports numbers as % of the maximum rate assuming a constant GPU frequency. In this case, the frequency is set to 2390 MHz, which converts pixels/s results to pixels/clock, and the maximum rate is set to 128 pixels/clock, which converts pixels/clock results to % of the maximum rate, which is the most readable way to present the results. (e.g. 100 is full rate, 50 is 1/2 rate, 25 is 1/4 rate)

> [!warning]
> The app currently expects that most features supported by desktop GPUs are supported.

# Test suites

## `bufbw`: Fill and copy buffer bandwidth

Each column is the size passed to the fill or copy buffer call.

Decoding test names:
- `fill`, `copy`: the operation is "fill buffer" or "copy buffer"
- `vram`, `sysm`: indicating that the buffer is allocated either in device local memory (VRAM) or system memory
- `maxalign`: buffer offsets passed to the fill or copy call are maximally aligned (currently 256K)
- `dst=N`, `src=N`: the destination or source buffer offset passed to the fill or copy call is aligned to N
- `both=N`: both offsets are aligned to N

## `pix`: Pixel throughput

Each column is a different color buffer format except for the first column, which tests a fragment shader with an out-of-range image store instead of writing a color output, and no color buffer is present in this case.

Decoding test names:
- `noaa`: the framebuffer has 1 sample
- `msaa8`: the framebuffer has 8 samples
- `fs_empty`: empty fragment shader
- `fs_discard`: the fragment shader only contains `discard;`
- `helper_invoc`: the test uses `gl_HelperInvocation` to suppress the automatic use of VRS coarse shading by some drivers
- `zbuf`: the framebuffer contains a Z buffer
- `z_tess_*.fail`, `z_tess_*.pass`: the test uses the given Z compare op to pass or fail the Z test
- `colormask=0`, `colormask=x`: the color mask is set to 0 or only the X component
- `blend_src_color0`, `blend_src_color1`, `blend_src_alpha0`, `blend_src_alpha1`: the test uses blending with color or alpha blend factors and color values such that 0 means that blending fully discards the pixels, while 1 means that blending fully overwrites the pixels
- `blend_src_color_other`, `blend_src_alpha_other`: the test uses blending with color or alpha blend factors and color values such that no pixels are completely discarded or completely overwritten and actual blending must take place
- `output.color`, `output.z`, `output.samplemask`: the fragment shader contains color, Z, or samplemask outputs (all tests write a color output unless the name contains `output` without `color`)
- `z_disabled`: the Z test is disabled
- `a2c`: alpha-to-coverage is enabled
- `vrs1x2`, `vrs2x1`, `vrs2x2`: the given amount of VRS coarse shading
- `const_fill`: the color output is a constant color
- Used system values are indicated as follows:
  - `face`: `gl_FrontFacing`
  - `samplemask`: `gl_SampleMaskIn`
  - `fragpos_*`: `gl_FragCoord.*` using only the listed components
  - `sampleid`: `gl_SampleID` (this forces sample shading if framebuffer samples > 1)
  - `samplepos`: `gl_SamplePosition` (this forces sample shading if framebuffer samples > 1)
- Used inputs are indicated as follows:
  - `Nflat`: N flat inputs
  - `Npersp`: N inputs with perspective interpolation at center
  - `Npersp_sample`: N inputs with perspective interpolation at sample (this forces sample shading if framebuffer samples > 1)
  - `Ncentroid`: N inputs with perspective interpolation at centroid
  - `Nlinear`: N inputs with linear (`noperspective`) interpolation at center

Optional parameters:
- `-lean`: don't test 8bpp, 16bpp, and rgb10a2 image formats
- `-filter=STRING`: only run tests containing this exact string
- `-format=STRING`: only test image formats containing this exact string

## `pixbw`: Render target bandwidth

Same as `pix`, but print the memory bandwidth in GB/s instead of pixels/clock. Tests that use a Z buffer or don't write the color buffer are skipped.

## `prim`: Primitive throughput

Each column is a different number of vec4 inputs received by the fragment shader.

Decoding test names:
- `cull_100%`, `cull_75%`, `cull_50%`, `cull_0%`: the test culls this % of primitives using one of the culling methods below
- `trilist`, `tristrip`: draw as triangle list or triangle strip
- `mesh32`, `mesh64`, `mesh128`, `mesh192`, `mesh256`: draw with a mesh shader, the number indicates the mesh shader workgroup size
- `reuse0`, `reuse1`, `reuse2`: how many vertices each primitive reuses from 2 previous primitives using an index buffer (triangle strips always reuse 2 vertices)
- `cull_back`: primitives are culled by back face culling
- `cull_xy`: primitives are culled by setting their (x,y) vertex positions such that the primitive is entirely outside the viewport
- `degenerate`: triangles are culled by being degenerate, i.e. 0 area (2 out of 3 vertex positions are equal)
- `rasterizer_discard`: all primitives are culled by "rasterizer discard"
- `N_small_tris_pp`: exactly N tiny triangles are drawn to fill 1 pixel, i.e. N small triangles per pixel (these are subpixel triangles that are culled due to not intersecting the pixel center, so only the single triangle intersecting the pixel center is drawn)
- `clipdist4`, `culldist4`: culled by clip/cull distance outputs, 4 clip/cull distances are written
- `clipdist8`, `culldist8`: culled by clip/cull distance outputs, 8 clip/cull distances are written
- `clipdist1357`, `culldist1357`: culled by clip/cull distance outputs where only clip/cull distance outputs 1, 3, 5, 7 do the culling; clip/cull distance outputs 0, 2, 4, 6 are set to constant 1
- `output_pointsize`: additionally write the point size output (with no effect on behavior)
- `output_layer`: additionally write the layer output (with no effect on behavior)
- `output_vrs1x1`: additionally write the primitive shading rate output (with no effect on behavior)

# How to Build

## Linux

Dependencies: Vulkan, GL, EGL (surfaceless), libpng, libshaderc

```
mkdir build
cd build
meson setup ..
ninja
```

## Windows

TBD
