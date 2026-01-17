[[_TOC_]]

# Introduction and Project Vision

This is a command-line microbenchmark that measures the performance of various features of GPUs through APIs, and how well different GPUs, APIs, and API translation and forwarding layers do well against each other.

It reports GPU performance in terms of pixels per clock (samples per clock), primitives per clock, draws per clock, rays per clock, memory throughput, etc. with different combinations of pipeline states, shaders, and different types of draw/compute/blit/RT/etc. operations to gather how observed GPU performance is affected by the choice of drivers (closed source, open source / Mesa), APIs (DX11, DX12, GL, VK), API translation and forwarding layers (DXVK, VKD3D, Zink, WSL2, VirtIO), and operating systems (Android, Linux, Windows).

This app enables developers to evaluate observed GPU performance across all those pieces of SW on the same hardware, and to precisely identify the root causes of inefficiencies. Examples of possible comparisons:

- Open source vs closed source driver
- Windows vs Linux vs Android
- Vulkan vs OpenGL vs Zink
- Vulkan vs DX12 vs VKD3D
- Vulkan vs DX11 vs DXVK
- OpenGL in WSL2 vs Vulkan in WSL2 vs DX12
- Vulkan with graphics pipeline objects (all shaders and states known at pipeline creation) vs all dynamic state (states unknown at pipeline creation) vs graphics pipeline
libraries (shaders are compiled independently with no knowledge of states and other shaders)
- VirtIO Native Context vs VirtIO VirGL/Venus vs native driver

In an ideal world, all APIs and API translation and forwarding layers would provide equivalent performance and achieve the GPU’s expected performance envelope. Because this is rarely true, thorough microbenchmarking is essential.

> [!note] Disclaimer
> The app’s results may not reflect performance across typical applications and workloads. The app aims to help developers improve all GPU API implementations, not serve as a ranking tool.

# How to Run

`gpu-ratemeter [optional parameters] [api].[test suite]`

The following APIs are supported:
- `d3d11`: Direct3D 11 **{- (not implemented yet) -}**
- `d3d12`: Direct3D 12 **{- (not implemented yet) -}**
- `gl`: OpenGL (linked shaders only)
- `vk`: Vulkan with regular graphics pipeline objects
- `vk.dyn`: Vulkan with all dynamic state **{- (not implemented yet) -}**
- `vk.gpl`: Vulkan with graphics pipeline libraries **{- (not implemented yet) -}**

The following test suites are available:
- `bufbw`: buffer fills and copies in GB/s
- `draw`: clocks/draw (less is better) **{- (not implemented yet) -}**
- `mma`: matrix multiply accumulate in TBD units **{- (not implemented yet) -}**
- `imgbw`: image clears, image copies, blits, and MSAA resolving in GB/s **{- (not finished yet, porting from radeonsi in progress) -}**
- `iobw`: shader input and output throughput in GB/s, including transform feedback **{- (not implemented yet, port from piglit) -}**
- `pix`: pixel throughput in samples/clock, all tests are run with 1x MSAA and 8x MSAA
- `pixbw`: color buffer write throughput in GB/s, same tests as `pix`
- `prim`: primitive throughput in primitives/clock
- `rt`: ray tracing performance in rays/clock **{- (not implemented yet) -}**
- `sanity`: verify that the API works by drawing an object and saving the result into a PNG file

> [!tip]
> - Use GPU-specific tools like sysfs to set a constant GPU frequency to get consistent results and use the `-freq=N` parameter.
> - The output is a table in CSV. Paste it into a spreadsheet to make easy comparisons of different runs.

Optional parameters common to all test suites:
- `-freq=N`: the GPU frequency in MHz, which causes results to be reported in units/clock instead of billion units/second (ignored when reporting memory bandwidth)
- `-maxrate=N`: the maximum rate in units/clock, which causes results to be reported as % of the maximum rate instead of units/clock (ignored when reporting memory bandwidth)

Examples:

```
gpu-ratemeter gl.bufbw
gpu-ratemeter vk.prim
gpu-ratemeter -lean gl.pix
gpu-ratemeter -lean vk.pix
```

> [!warning]
> The app currently expects that most features supported by desktop GPUs are supported.

# How It Works

- Results are calculated from GPU timestamps.
- Each test contains a warm-up phase where N initial iterations are discarded, but it's not enough if a GPU takes a longer time to ramp up its frequency. If that happens, use a power profile that maintains a constant frequency at all times.
- % progress is printed while building pipelines and executing tests. Results are only printed at the end (unless a specific test suite has multiple stages).
- The execution time of one test suite should not exceed 2 minutes on a decent desktop GPU.
- The app is windowless and doesn't even register with the window system where that's possible.
- If needed for debugging or developing new tests, it can save any rendered image to a PNG and open it in an image viewer.

# Test Suites

## `bufbw`: Fill and Copy Buffer Bandwidth (GB/s)

Each column is the size passed to the fill or copy buffer call.

Decoding test names:
- `fill`, `copy`: the operation is "fill buffer" or "copy buffer"
- `devmem`, `hostmem`: indicating that the buffer is allocated either in device local memory or host memory
- `maxalign`: buffer offsets passed to the fill or copy call are maximally aligned (currently 64K)
- `dst=N`, `src=N`: the destination or source buffer offset passed to the fill or copy call is aligned to N (N=1 means unaligned)
- `both=N`: both offsets are aligned to N (N=1 means unaligned)

## `imgbw`: Clear, Copy, Blit, and MSAA Resolve Bandwidth (GB/s)

TODO

## `pix`: Pixel Throughput (samples/clock)

Each column is a different color buffer format except for the first column, which tests a fragment shader with only an out-of-range image store (no color buffer is present in this case).

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
- `output.color`, `output.z`, `output.samplemask`: the fragment shader contains color, Z, or samplemask outputs (all other tests also write a color output unless 1) the name contains `output` without `color`, or 2) it's the `imgStore` column)
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
- `-filter=STRING`: only run tests containing this exact string; if `STRING` ends with $, the test name must end with it
- `-format=STRING`: only test image formats containing this exact string; if `STRING` ends with $, the format name must end with it

## `pixbw`: Color Buffer Write Bandwidth (GB/s)

Same as `pix`, but print the memory bandwidth in GB/s instead of samples/clock. Tests from `pix` that use a Z buffer or don't write the color buffer are skipped.

## `prim`: Primitive Throughput (primitives/clock)

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

Optional parameters:
- `-filter=STRING`: only run tests containing this exact string; if `STRING` ends with $, the test name must end with it

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
