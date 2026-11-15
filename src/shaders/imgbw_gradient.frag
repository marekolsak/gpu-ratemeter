#version 460

#if <has_stencil_output>
#extension GL_ARB_shader_stencil_export : enable
#endif

#if <has_color_output>
layout(location = 0) out <vec_type> fs_out;
#endif

void main() {
#if <has_color_output>
   fs_out = <vec_type>(<gradient>);
#endif

#if <has_stencil_output>
   gl_FragStencilRefARB = int(gl_FragCoord.x) % 256;
#endif
}
