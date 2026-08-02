[[_TOC_]]

The official repository is hosted at https://gitlab.freedesktop.org/mesa/gpu-ratemeter.

# Introduction and Project Vision

This is an engineering-focused non-consumer command-line GPU microbenchmark that measures the performance of various GPU features and specific GPU workloads through APIs and how different GPUs, APIs, API translation and forwarding layers, and operating systems compare in GPU utilization and performance. The main goal is to help engineers easily identify inefficiencies in GPUs, driver stacks, and operating systems, and make improvements.

It produces CSV output and reports GPU performance in pixels per clock, samples per clock, primitives per clock, clocks per draw (TBD), rays per clock (TBD), memory throughput, latencies, etc. with different combinations of pipeline states, shaders, and different types of draw/compute/blit/RT/etc. operations to determine how observed GPU performance is affected by the choice of drivers (closed source, open source / Mesa), APIs (DX11, DX12, GL, VK), API translation and forwarding layers (DXVK, VKD3D, Zink, WSL2, VirtIO), and operating systems (Android, Linux, Windows).

Broad support of APIs and API pipeline construction codepaths enables the following comparisons:

- Open source vs closed source driver
- Windows vs Linux vs Android
- Vulkan vs OpenGL vs Zink
- Vulkan vs DX12 vs VKD3D
- Vulkan vs DX11 vs DXVK
- OpenGL in WSL2 vs Vulkan in WSL2 vs DX12
- Vulkan graphics pipeline objects vs graphics pipeline libraries vs shader objects
- Vulkan static state vs dynamic state
- VirtIO Native Context vs VirtIO VirGL/Venus vs bare metal

In an ideal world, all APIs and API translation and forwarding layers would have equivalent performance on the same HW. Since this is rarely true, a tool like this is essential.

Examples:
- If one driver achieves 2x pixels/clock at 32 bpp than another, the other driver might not program the HW optimally for 32 bpp.
- If one driver has 4x higher early depth-test rejection rate than another, HiZ may be disabled for the other driver.
- If a driver sustains only 300 GB/s for image blits while the maximum memory bandwidth is 500 GB/s, the blit implementation in that driver may be suboptimal.
- If Vulkan pipeline objects using dynamic state are 2x slower than equivalent objects using static state, the dynamic state handling in that driver may be suboptimal.

While this project is developed to aid Mesa developers, it can be equally useful to GPU vendors for HW and driver validation (both pre-silicon and post-silicon), operating system vendors, and for CI performance regression testing.


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
- `-maxvalidresult=N`: (for buggy HW timestamps) if the result is greater than N, print "error" instead of the result

Optional parameters common to "performance per clock" tests:
- `-freq=N`: the GPU frequency in MHz; required for reporting perf/clock; without it, billion units/s are reported
- `-maxrate=N`: if set, this number converts perf/clock to perf/clock % of N, e.g. results are reported as N -> 100, 2*N -> 200, N/4 -> 25; this makes perf/clock results more readable since 100% is easier to read than a HW-specific number corresponding to 100%

Examples:

```
gpu-ratemeter gl.bufbw
gpu-ratemeter vk.prim
gpu-ratemeter -lean gl.pix
gpu-ratemeter -lean vk.pix
```

> [!note]
> The desirable execution time per test is less than 1 minute. Exceeding that for some devices would warrant adding new optional parameters that would help reduce it. Pipeline object creation can also be distributed across all CPU cores (the `pix` test on Vulkan already does that).


# Tests

### API Support

|              | OpenGL | Vulkan |
|--------------|-------:|-------:|
| **Graphics Pipeline Tests**    |
| `pix`        | **✓**  | **✓**  |
| `pixbw`      | **✓**  | **✓**  |
| `prim`       | **✓**  | **✓**  |
| **Shader Tests**               |
| `latency`    |        | **✓**  |
| **Resource Operation Tests**   |
| `bufbw`      | **✓**  | **✓**  |
| `imgbw`      | **✓**  | **✓**  |
| **Miscellaneous Tests**        |
| `sparsebind` |        | **✓**  |


## Graphics Pipeline Tests

### `pix`: Pixel Throughput (pixels/clock)

Each column is a different color buffer format except for the first column, which tests a fragment shader with only an out-of-range image store (no color buffer is present in this case).

The test is executed multiple times, each time with a different framebuffer (and rasterization) behavior. The following framebuffer configurations are tested:
- `noaa`: the framebuffer is 2D with 1 sample and 1 layer
- `msaa2`, `msaa4`, `msaa8`: the framebuffer is 2D with 2, 4, or 8 samples and 1 layer
- `multiview`: the framebuffer is 2D with 1 sample and 2 layers, using the multiview feature to render to both layers simultaneously (per-view varyings are not tested currently)
- `image3d`: the framebuffer is a 3D image with 8 slices, but only slice 0 is drawn to (only a small subset of representative tests is executed for this case)
- `linear`: the framebuffer is a 2D image using the linear layout (only a small subset of representative tests is executed for this case)

Decoding subtest names:
- `fs_empty`: empty fragment shader
- `fs_discard`: the fragment shader contains `discard;` before writing the color output
- `fs_discard_no_output`: the fragment shader contains `discard;` with no outputs (it only discards Z writes)
- `helper_invoc`: the subtest uses `gl_HelperInvocation` to suppress the automatic use of VRS by some drivers
- `zbuf`: the framebuffer contains a Z buffer
- `zwrite`: Z writes are enabled
- `z_tess_*.fail`, `z_tess_*.pass`: the subtest uses the given Z compare op to pass or fail the Z test
- `colormask=0`, `colormask=x`: the color mask is set to 0 or only the X component
- `blend_src_color0`, `blend_src_color1`, `blend_src_alpha0`, `blend_src_alpha1`: the subtest uses blending with color or alpha blend factors and color values such that 0 means that blending fully discards the pixels, while 1 means that blending fully overwrites the pixels
- `blend_src_color_other`, `blend_src_alpha_other`: the subtest uses blending with color or alpha blend factors and color values such that no pixels are completely discarded or completely overwritten and actual blending must take place
- `output.color`, `output.z`, `output.samplemask`: the fragment shader contains color, Z, or samplemask outputs (all other subtests also write a color output unless 1) the name contains `output` without `color`, or 2) it's the `imgStore` column)
- `z_disabled`: the Z test is disabled
- `a2c`: alpha-to-coverage is enabled
- `vrs1x2`, `vrs2x1`, `vrs2x2`: the given amount of VRS
- `rasterN`: fill the screen with triangles of a specific size; N is the percentage of non-helper FS invocations relating to the size of triangles, see the description in the subsection below
- `const_fill`: the color output is a constant color
- `cull_back`: back-face culling is enabled (with no effect - the full-screen triangle is front-facing)
- Used system values are indicated as follows:
  - `face`: `gl_FrontFacing`
  - `fragpos_*`: `gl_FragCoord.*` using only the listed components
  - `fully_covered`: `gl_FullyCoveredEXT` (also known as `gl_FragFullyCoveredNV`)
  - `layer`: `gl_Layer`
  - `primitive_id`: `gl_PrimitiveID`
  - `sampleid`: `gl_SampleID` (this forces sample shading if framebuffer samples > 1)
  - `samplemask`: `gl_SampleMaskIn`
  - `samplepos`: `gl_SamplePosition` (this forces sample shading if framebuffer samples > 1)
  - `shading_rate`: `gl_ShadingRateEXT`
  - `view_index`: `gl_ViewIndex`
  - `viewport_index`: `gl_ViewportIndex`
- Used inputs are indicated as follows:
  - `Nflat`: N flat inputs
  - `Npersp`: N inputs with perspective interpolation at center
  - `Npersp_sample`: N inputs with perspective interpolation at sample (this forces sample shading if framebuffer samples > 1)
  - `Ncentroid`: N inputs with perspective interpolation at centroid
  - `Nlinear`: N inputs with linear (`noperspective`) interpolation at center

Optional parameters:
- `-filter=STRING`: only run subtests containing this exact string; if `STRING` ends with $, the subtest name must end with it
- `-format=STRING`: only test image formats containing this exact string; if `STRING` ends with $, the format name must end with it
- `-gl-tiling-linear`: indicate that regular GL textures are allocated as linear if `GL_LINEAR_TILING_EXT` is set; this also sets `MESA_DEBUG=api-tiling-linear` to make Mesa not ignore `GL_LINEAR_TILING_EXT` for regular GL textures
- `-lean`: don't test 8bpp, 16bpp, and rgb10a2 image formats
- `-rdna4ts`: a mostly functional workaround for broken timestamps on RDNA 4 (it slightly reduces perf)
- `-samplerate`: report samples/clock instead of pixels/clock
- `-subset=STRING`: Test only one subset. If `STRING` is number 1, 2, 4, or 8, test only the subset with this number of samples. If `STRING` is `multiview`, `image3d`, or `linear`, test only the corresponding subset.

#### Rasterizer efficiency subtests

`raster` are subtests executed as part of `pix` that measure how much helper invocations and triangle
sizes negatively impact pixel throughput and when primitive throughput starts becoming the limiting
factor. While all other subtests use a single fullscreen triangle and 0 helper invocations,
the `raster` subtests draw a mesh of equally-sized roughly equilateral triangles. Each subsequent
subtest decreases the triangle size and increases the number of triangles to always fill the whole screen.
A fullscreen triangle and fullscreen quad subtests are also included for reference.

The pipeline statistics of each subtest are in the table below. Unlike standard pipeline statistics, these also include helper invocations.

| Subtest                       | Total FS invoc. | % non-helper invoc. | Visible tris | Avg. tri area | Avg. pixels / tri | Avg. FS invoc. / tri |
|-------------------------------|-----------------|---------------------|--------------|---------------|-------------------|----------------------|
| raster100.fullscreen_triangle |         1048576 |             100.0 % |            1 |      1024.00² |         1048576.0 |            1048576.0 |
| raster99.8.fullscreen_quad    |         1050624 |              99.8 % |            2 |       724.08² |          524288.0 |             525312.0 |
| raster99.9                    |         1049800 |              99.9 % |            2 |       724.08² |          524288.0 |             524900.0 |
| raster99.6                    |         1053072 |              99.6 % |            6 |       418.05² |          174762.7 |             175512.0 |
| raster99.4                    |         1054384 |              99.4 % |           11 |       308.75² |           95325.1 |              95853.1 |
| raster99.1                    |         1058496 |              99.1 % |           21 |       223.46² |           49932.2 |              50404.6 |
| raster98.7                    |         1062708 |              98.7 % |           40 |       161.91² |           26214.4 |              26567.7 |
| raster98.0                    |         1069924 |              98.0 % |           74 |       119.04² |           14169.9 |              14458.4 |
| raster97.2                    |         1078396 |              97.2 % |          134 |        88.46² |            7825.2 |               8047.7 |
| raster96.3                    |         1088344 |              96.3 % |          226 |        68.12² |            4639.7 |               4815.7 |
| raster94.6                    |         1108172 |              94.6 % |          456 |        47.95² |            2299.5 |               2430.2 |
| raster93.0                    |         1128072 |              93.0 % |          800 |        36.20² |            1310.7 |               1410.1 |
| raster89.8                    |         1167704 |              89.8 % |         1728 |        24.63² |             606.8 |                675.8 |
| raster84.1                    |         1247160 |              84.1 % |         4720 |        14.90² |             222.2 |                264.2 |
| raster81.5                    |         1286492 |              81.5 % |         6722 |        12.49² |             156.0 |                191.4 |
| raster76.8                    |         1366140 |              76.8 % |        11906 |         9.38² |              88.1 |                114.7 |
| raster72.5                    |         1445360 |              72.5 % |        18408 |         7.55² |              57.0 |                 78.5 |
| raster68.8                    |         1524564 |              68.8 % |        26508 |         6.29² |              39.6 |                 57.5 |
| raster62.3                    |         1682720 |              62.3 % |        46874 |         4.73² |              22.4 |                 35.9 |
| raster57.0                    |         1840632 |              57.0 % |        73008 |         3.79² |              14.4 |                 25.2 |
| raster52.5                    |         1998232 |              52.5 % |       105282 |         3.16² |              10.0 |                 19.0 |
| raster45.3                    |         2313032 |              45.3 % |       186502 |         2.37² |               5.6 |                 12.4 |
| raster39.9                    |         2626632 |              39.9 % |       290786 |         1.90² |               3.6 |                  9.0 |

### `pixbw`: Color Buffer Write Bandwidth (GB/s)

Same as `pix`, but it prints the memory bandwidth in GB/s instead of pixels/clock. This can be used to determine whether pixel throughput is limited by fixed-function logic or memory bandwidth. Subtests from `pix` that use a Z buffer or don't write the color buffer are skipped.

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

If the shader compiler inserts ALU instructions between load-use and load-issue, the measured latencies will include the cost of those extra instructions. Looking at the shader disassembly is recommended to see what is actually being measured.

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
- `-bda`: Use buffer device addresses instead of storage buffers to traverse the buffer.  (this may report more accurate latencies on some drivers)
- `-int8`: Use 8-bit addresses for shared memory tests. (this may report more accurate latencies on some drivers)
- `-clockbits=N`: It the shader subgroup clock has less than 64 bits, this is the number of bits that it returns. This parameter enables low-bit clock handling.
(it must be set to 20 for RDNA 2 and 3)
- `-sparse-bound`: The traversed buffer is sparse and the smallest possible buffers are bound across its whole range. This can be used to measure the impact of small pages.
- `-sparse-unbound`: The whole buffer is sparse and its whole range is unbound.

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


## Resource Operation Tests

### `bufbw`: Buffer Fill and Copy Bandwidth (GB/s)

Each column is the size passed to the fill or copy buffer call.

Decoding subtest names:
- `fill`, `copy`: the operation is "fill buffer" or "copy buffer"
- `devmem`, `hostmem`: indicating that the buffer is allocated either in device local memory or host memory
- `hit`: whether the cache hit bandwidth is tested by repeating the same operation on the same buffer range
- `miss`: whether the cache miss bandwidth is tested by increasing the buffer offset monotonically and wrapping at 512 MB (cache misses are guaranteed only if the last level cache is smaller than that)
- `miss_no_barrier`: whether the cache miss bandwidth is tested as above, and also there is no barrier between individual operations; this is only executed for APIs that require explicit barriers at fill and copy operations
- `maxalign`: buffer offsets passed to the fill or copy call are maximally aligned (currently 64K)
- `dst=N`, `src=N`: the destination or source buffer offset passed to the fill or copy call is aligned to N (N=1 means unaligned)
- `both=N`: both offsets are aligned to N (N=1 means unaligned)

Optional parameters:
- `-compute`: Execute on the compute queue.
- `-transfer`: Execute on the transfer queue.

### `imgbw`: Framebuffer Clear, Image Clear/Copy/Blit, and MSAA Image Clear/Copy/Blit/Resolve Bandwidth (GB/s)

WIP (functionally mostly finished, try it)


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


# How It Works

- GPU timestamps are used for measurements.
- Each subtest contains a warm-up phase where N initial iterations are discarded, but it's not enough if a GPU takes a longer time to ramp up its frequency. It's recommended to use a power profile that keeps the frequency constant.
- % progress is printed while building pipelines and executing subtests. Results are only printed at the end (unless a specific test has multiple stages).
- The app is windowless and doesn't even register with the window system where that's possible.
- If needed for debugging or developing new tests/subtests, it can save any rendered image to a PNG and open it in an image viewer.
