#version 460
#extension GL_ARB_shader_viewport_layer_array : enable
#ifdef VULKAN
   #define gl_VertexID gl_VertexIndex
#endif

/* This shader generates same-sized equilateral triangles at random positions on the screen
 * with random vertex colors from VertexID.
 *
 * aspect_scale determines the triangle circumradius per axis for sizing and aspect correction.
 */

layout(std140, binding = 0) uniform Uniforms {
    vec2 aspect_scale;
    uint num_layers;
};

layout(location = 0) out vec4 vColor;

uint rand_u32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rand01(uint seed)
{
    return float(rand_u32(seed)) / 4294967295.0;
}

vec4 rand4(uint seed)
{
    return vec4(
        rand01(seed * 4u + 0u),
        rand01(seed * 4u + 1u),
        rand01(seed * 4u + 2u),
        rand01(seed * 4u + 3u)
    );
}

const float PI2 = 6.28318530718;

void main()
{
    uint primitiveID = gl_VertexID / 3u;
    uint vertexInTriangle = gl_VertexID % 3u;

    uint primSeed = primitiveID;
    uint vertexSeed = gl_VertexID;

    uint angleSeed = primSeed ^ 0xabcabcab;
    uint layerSeed = primSeed ^ 0x65432198;
    uint zSeed = vertexSeed ^ 0x65465465;
    uint colorSeed = vertexSeed ^ 0xfedcba98;

    vec2 center = rand4(primSeed).xy * 2 - 1;   // The center of the triangle.
    float angle = rand01(angleSeed) * PI2;      // Random rotation per triangle.

    // All 3 vertices are 120 degress apart on the circumcircle, forming an equilateral triangle.
    float cornerAngle = angle + float(vertexInTriangle) * PI2 / 3;

    // Generate the vector from the center to one of the 3 vertices.
    vec2 offset = vec2(cos(cornerAngle), sin(cornerAngle));

    // Aspect correction and size scaling, so that triangles look equilateral on the screen and are
    // the right size. This includes circumradius scaling from local space to NDC space.
    offset *= aspect_scale;

    gl_Position = vec4(center + offset, rand01(zSeed), 1.0);
#if <layered>
    gl_Layer = int(rand_u32(layerSeed) % num_layers);
#endif

    vColor = rand4(colorSeed);     // Random color per vertex.
}
