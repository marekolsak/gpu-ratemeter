#version 460
#extension GL_ARB_shader_viewport_layer_array : enable
#ifdef VULKAN
   #define gl_VertexID gl_VertexIndex
   #define gl_InstanceID gl_InstanceIndex
#endif

void main() {
/* Generate a quad from VertexID. */
   gl_Position = vec4((gl_VertexID & 1) == 0 ? -1 : 1,
                      (gl_VertexID & 2) == 0 ? -1 : 1,
/* This will produce a gradient for Z. */
                      (gl_VertexID & 1) == 0 ?  0.1 : 0.9, 1);
#if <layered>
   gl_Layer = gl_InstanceID;
#endif
}
