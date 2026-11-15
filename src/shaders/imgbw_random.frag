#version 460

#if <has_stencil_output>
#extension GL_ARB_shader_stencil_export : enable
#endif

#if <has_color_output>
layout(location = 0) out <vec_type> fs_out;
#endif

/* 32-bit avalanche mixer */
uint mix32(uint v)
{
   v ^= v >> 16;
   v *= 0x7feb352du;
   v ^= v >> 15;
   v *= 0x846ca68bu;
   v ^= v >> 16;
   return v;
}

uint rand_uint(uvec4 seed)
{
   return mix32(mix32(seed.x * 0xA341316Cu) ^
                mix32(seed.y * 0xC8013EA4u) ^
                mix32(seed.z * 0xAD90777Du) ^
                mix32(seed.w * 0x7E95761Eu));
}

uvec4 rand_uvec4(uvec4 seed)
{
   uint h0 = rand_uint(seed);
   uint h1 = mix32(h0 ^ 0x68bc21ebu);
   uint h2 = mix32(h0 ^ 0x02e5be93u);
   uint h3 = mix32(h0 ^ 0x9e3779b9u);

   return uvec4(h0, h1, h2, h3);
}

vec4 rand_vec4(uvec4 seed)
{
   return rand_uvec4(seed) / 4294967296.0;
}

void main() {
   uvec4 seed = uvec4(uint(gl_FragCoord.x), uint(gl_FragCoord.y), gl_Layer, gl_SampleMaskIn[0]);

#if <has_color_output>
    fs_out = <vec_type>(<expr>);
#endif

#if <has_depth_output>
   gl_FragDepth = rand_vec4(seed).x;
#endif

#if <has_stencil_output>
   gl_FragStencilRefARB = int(rand_uvec4(seed).x >> 24);
#endif
}
