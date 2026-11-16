#version 460

#if <n> >= 1
layout(location = 0) out <vec_type0> fs_out0;
#endif

#if <n> >= 2
layout(location = 1) out <vec_type1> fs_out1;
#endif

#if <n> >= 3
layout(location = 2) out <vec_type2> fs_out2;
#endif

#if <n> >= 4
layout(location = 3) out <vec_type3> fs_out3;
#endif

#if <n> >= 5
#error "n is too large"
#endif

layout(location = 0) in vec4 vColor;

#ifdef VULKAN
layout(binding = 1)
#else
layout(location = 0)
#endif
uniform sampler2D tex;

void main() {
    vec2 coord = gl_FragCoord.xy / vec2(textureSize(tex, 0));

    // The texture contains random noise.
    // Generate a dominating low frequency pattern with a mild high frequency pattern.
    vec4 color = mix(texture(tex, coord / 16), texture(tex, coord / 2), 0.3);

#if <n> >= 1
    fs_out0 = <vec_type0>(color * <multiplier0>);
#endif

#if <n> >= 2
    fs_out1 = <vec_type1>(color * <multiplier1>);
#endif

#if <n> >= 3
    fs_out2 = <vec_type2>(color * <multiplier2>);
#endif

#if <n> >= 4
    fs_out3 = <vec_type3>(color * <multiplier3>);
#endif
}
