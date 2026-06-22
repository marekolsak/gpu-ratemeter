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

Examples:
- If one driver achieves 2x pixels/clock at 32 bpp than another, the other driver might not program the HW optimally for 32 bpp.
- If one driver has 4x higher early depth-test rejection rate than another, HiZ may be disabled for the other driver.
- If a driver sustains only 300 GB/s for image blits while the maximum memory bandwidth is 500 GB/s, the driver's blit path may be suboptimal.
- If pipeline objects using dynamic state are 2x slower than equivalent objects using static state, the driver’s dynamic-state handling may be suboptimal.


# How to Run

`gpu-ratemeter [optional parameters] [api].[test]`

The following APIs are supported:
- ⏳*(not implemented yet)* `d11`: Direct3D 11
- ⏳*(not implemented yet)* `d12`: Direct3D 12
- `gl`: OpenGL (linked shaders only)
- `vk`: Vulkan (graphics pipeline objects + static state)
- `vkd`: Vulkan (graphics pipeline objects + dynamic state)
- `vkl`: Vulkan (graphics pipeline libraries + static state)
- `vkld`: Vulkan (graphics pipeline libraries + dynamic state)
- ⏳*(not implemented yet)* `vkeso`: Vulkan (using VK_EXT_shader_object)

> [!tip]
> - Use GPU-specific tools like sysfs to set a constant GPU frequency to get consistent results and use the `-freq=N` parameter.
> - The output is a table in CSV. Paste it into a spreadsheet to make easy comparisons of different runs.

Optional parameters common to all tests:
- `-device=N`: the device index of the device to use (default: 0), Vulkan only
- `-no-validator`: disable the Vulkan validation layer (enabled by default)
- `-freq=N`: the GPU frequency in MHz, which causes results to be reported in units/clock instead of billion units/second (ignored when reporting bandwidth or latency)
- `-maxrate=N`: the maximum rate in units/clock, which causes results to be reported as % of the maximum rate instead of units/clock (ignored when reporting bandwidth or latency)

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
- Each subtest contains a warm-up phase where N initial iterations are discarded, but it's not enough if a GPU takes a longer time to ramp up its frequency. If that happens, use a power profile that maintains a constant frequency at all times.
- % progress is printed while building pipelines and executing subtests. Results are only printed at the end (unless a specific test has multiple stages).
- The execution time of one test should not exceed 2 minutes on a decent desktop GPU.
- The app is windowless and doesn't even register with the window system where that's possible.
- If needed for debugging or developing new tests/subtests, it can save any rendered image to a PNG and open it in an image viewer.


# Tests

## Graphics Pipeline Tests

### `pix`: Pixel Throughput (samples/clock)

Each column is a different color buffer format except for the first column, which tests a fragment shader with only an out-of-range image store (no color buffer is present in this case).

Decoding subtest names:
- `noaa`, `msaa4`, `msaa8`: the framebuffer has 1, 4, or 8 samples
- `fs_empty`: empty fragment shader
- `fs_discard`: the fragment shader only contains `discard;`
- `helper_invoc`: the subtest uses `gl_HelperInvocation` to suppress the automatic use of VRS coarse shading by some drivers
- `zbuf`: the framebuffer contains a Z buffer
- `z_tess_*.fail`, `z_tess_*.pass`: the subtest uses the given Z compare op to pass or fail the Z test
- `colormask=0`, `colormask=x`: the color mask is set to 0 or only the X component
- `blend_src_color0`, `blend_src_color1`, `blend_src_alpha0`, `blend_src_alpha1`: the subtest uses blending with color or alpha blend factors and color values such that 0 means that blending fully discards the pixels, while 1 means that blending fully overwrites the pixels
- `blend_src_color_other`, `blend_src_alpha_other`: the subtest uses blending with color or alpha blend factors and color values such that no pixels are completely discarded or completely overwritten and actual blending must take place
- `output.color`, `output.z`, `output.samplemask`: the fragment shader contains color, Z, or samplemask outputs (all other subtests also write a color output unless 1) the name contains `output` without `color`, or 2) it's the `imgStore` column)
- `z_disabled`: the Z test is disabled
- `a2c`: alpha-to-coverage is enabled
- `vrs1x2`, `vrs2x1`, `vrs2x2`: the given amount of VRS coarse shading
- `const_fill`: the color output is a constant color
- `cull_back`: back-face culling is enabled (with no effect - the full-screen triangle is front-facing)
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
- `-filter=STRING`: only run subtests containing this exact string; if `STRING` ends with $, the subtest name must end with it
- `-format=STRING`: only test image formats containing this exact string; if `STRING` ends with $, the format name must end with it
- `-rdna4ts`: a mostly functional workaround for broken timestamps on RDNA 4 (it slightly reduces perf)

### `pixbw`: Color Buffer Write Bandwidth (GB/s)

Same as `pix`, but print the memory bandwidth in GB/s instead of samples/clock. Subtests from `pix` that use a Z buffer or don't write the color buffer are skipped.

### `prim`: Primitive Throughput (primitives/clock)

Each column is a different number of vec4 inputs received by the fragment shader.

Decoding subtest names:
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
- `-filter=STRING`: only run subtests containing this exact string; if `STRING` ends with $, the subtest name must end with it


## Shader Tests

### `latency`: Memory Load Latency in Clock Cycles

The test measures memory load latency as observed by shaders. Besides measuring latencies of all cache levels and memory, it can also be used to infer cache sizes and cache line sizes of all cache levels.

It reports the average number of elapsed ticks of the shader subgroup clock between issued loads and reading their result. Both uniform (scalar) and non-uniform (vector) latencies are measured.

Buffers of different sizes are completely traversed via load indirections in a manner that prevents cache hits if the whole buffer doesn't fit in the cache.
The results are printed for each tested buffer size. Each printed latency should exactly correspond to the latency of the last cache level that can hold
the buffer of that size.

Required parameters:
- `-maxsize=N`: The maximum buffer size to test. The value should be a power of two. Buffer sizes between 1K and this size are tested, with ~1.3-1.5x size increments. If needed to measure memory (cache miss) latency, it should also be > last level cache size.
Use `K`, `M`, `G` suffixes for kilo, mega, giga, respectively.
- `-spacing=N`: All load addresses are multiples of this number. This should be a power of two and <= cache line size. To make the test faster, it's recommended to set this exactly to the cache line size.
`maxsize / spacing` is the number of executed load indirections of each subtest, so it affects test length, and it also implies that the shader visits every load
address that's a multiple of `spacing` only once in the largest buffer.
Thus, larger spacing reduces the number of loads needed to traverse the largest buffer, while very small spacing with very large buffers can lead to a GPU timeout.
If spacing > cache line size, the latency of tested buffer sizes will no longer correspond to cache sizes because a subset of cache-line-sized buffer segments
is never loaded if the spacing is large enough to skip them, reducing cache utilization, and thus creating an illusion that the cache can hold more data than it should. (this behavior can be exploited to find the exact cache line size if it's unknown)

Optional parameters:
- `clockbits=N`: It the shader subgroup clock has less than 64 bits, this is the number of bits that it returns. This parameter enables low-bit clock handling.
(it must be set to 20 for RDNA 2 and 3)
- `sparse-bound`: The traversed buffer is sparse and the smallest possible buffers are bound across its whole range. This can be used to measure the impact of small pages.
- `sparse-unbound`: The whole buffer is sparse and its whole range is unbound.

Good starting parameters: `-maxsize=16M -spacing=64` (optimal if the last level cache size is 8 MB and the cache line size is 64)

Decoding subtest names:
- `default`: no GLSL qualifier is added
- `coherent`: the GLSL "coherent" qualifier is added
- `nonuniform`: the load addresses are non-uniform
- `uniform`: the load addresses are uniform
- `devmem`: the traversed buffer is in device memory
- `hostmem`: the traversed buffer is in host memory uncached by the CPU
- `devmem_coherent`: the traversed buffer is in host-coherent device memory (VK_AMD_device_coherent_memory or equivalent is required)
- `hostmem_coherent`: the traversed buffer is in host-coherent host memory uncached by the CPU (VK_AMD_device_coherent_memory or equivalent is required)
- `shared`: the traversed buffer is in shared memory

Only Vulkan is supported.


## Resource Operation Tests

### `bufbw`: Buffer Fill and Copy Bandwidth (GB/s)

Each column is the size passed to the fill or copy buffer call.

Decoding subtest names:
- `fill`, `copy`: the operation is "fill buffer" or "copy buffer"
- `devmem`, `hostmem`: indicating that the buffer is allocated either in device local memory or host memory
- `miss`, `hit`: whether cache miss or cache hit bandwidth is being tested (cache misses are guaranteed only if the last level cache is <= 256 MB)
- `maxalign`: buffer offsets passed to the fill or copy call are maximally aligned (currently 64K)
- `dst=N`, `src=N`: the destination or source buffer offset passed to the fill or copy call is aligned to N (N=1 means unaligned)
- `both=N`: both offsets are aligned to N (N=1 means unaligned)

### `imgbw`: Framebuffer Clear, Image Clear/Copy/Blit, and MSAA Image Clear/Copy/Blit/Resolve Bandwidth (GB/s)

WIP (functionally mostly finished, try it)

Optional parameters:
- `-rdna4ts`: a mostly functional workaround for broken timestamps on RDNA 4 (it slightly reduces perf)


## Miscellaneous Tests

### `sparsebind`: Sparse Bind Throughput (API calls/s)

This measures the throughput of sparse bind/unbind operations in API calls/s.

Each sparse bind/unbind operation is followed by either no command buffer, empty command buffer, or simple command buffer. The command buffer, if present, always waits for its sparse bind/unbind operation.

The last 2 columns show test results for an asynchronous sparse bind queue that runs with no waits.

The buffer used for testing is divided into an equal number of sparse blocks. Each sparse block has its own memory allocation that can be bound/unbound.

Decoding subtest names:
- `bind_one`: each call binds 1 new sparse block, increasing the number of bound sparse blocks monotonically (the subtest always starts with all sparse blocks unbound)
- `unbind_one`: each call unbinds 1 sparse block, decreasing the number of bound sparse blocks monotonically (the subtest always starts with all sparse blocks bound)
- `bind_unbind_same_one`: even calls bind the same sparse block, odd calls unbind it
- `bind_unbind_all`: even calls bind all sparse blocks at once, odd calls unbind all at once
- `sizeNm`: the buffer has N MB
- `blockNk`: each sparse block has N KB
- `start_bound`: the subtest starts with all sparse blocks bound (`bind_one` never has this, `unbind_one` always has this)

Only Vulkan is supported.


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


# How to Get Stable Results

## AMD GPUs on Linux

We recommend that the GPU power profile is set to maintain a constant GPU graphics core frequency. This is recommended for stability of memory bandwidth results, and required to get accurate measurements of performance per clock. When we measure performance per clock, the frequency that we choose (low or high) is irrelevant. Reduced constant frequencies are also useful when we want to isolate the effect of performance per clock optimizations from memory bandwidth optimizations.

Run `sudo umr --gui &`, select your GPU in the tab at the top, and then select the Power tab. In there, select one of the DPM (dynamic power management) profiles that begin with `profile`. All `profile_*` profiles maintain constant pre-determined frequencies. The frequency changes are shown in real time on the Power tab.

Since constant frequencies defeat power optimizations and can cause more heat to be generated, the firmware may change the profile back to `auto` if temperatures exceed safe limits. This will be immediately visible in the real time frequency chart on the Power tab.

Get `umr` here: https://gitlab.freedesktop.org/tomstdenis/umr
