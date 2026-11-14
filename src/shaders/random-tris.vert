#version 450

layout(location = 0) out vec3 vColor;

const float aspect = 1;

// Triangle radius in normalized device coordinates.
// NDC spans [-1, 1], so 0.10 is about 5% of the viewport width/height.
const float TRI_RADIUS = 0.10;

// Hash utilities.
uint hash_u32(uint x)
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
    return float(hash_u32(seed)) / 4294967295.0;
}

vec2 rand2(uint seed)
{
    return vec2(
        rand01(seed * 2u + 0u),
        rand01(seed * 2u + 1u)
    );
}

vec3 rand3(uint seed)
{
    return vec3(
        rand01(seed * 3u + 0u),
        rand01(seed * 3u + 1u),
        rand01(seed * 3u + 2u)
    );
}

void main()
{
    uint vertexID = uint(gl_VertexIndex);

    uint primitiveID = vertexID / 3u;
    uint vertexInTriangle = vertexID % 3u;

    // Keep the entire triangle inside the visible NDC viewport.
    float marginX = TRI_RADIUS / max(aspect, 0.0001);
    float marginY = TRI_RADIUS;

    vec2 r = rand2(primitiveID);

    vec2 center = vec2(
        mix(-1.0 + marginX, 1.0 - marginX, r.x),
        mix(-1.0 + marginY, 1.0 - marginY, r.y)
    );

    // Random rotation per triangle.
    float angle = rand01(primitiveID + 10000u) * 6.28318530718;

    // Equilateral triangle in local space.
    float cornerAngle = angle + float(vertexInTriangle) * 2.09439510239;

    vec2 offset = vec2(cos(cornerAngle), sin(cornerAngle)) * TRI_RADIUS;

    // Aspect correction so triangles look roughly equilateral on screen.
    offset.x /= max(aspect, 0.0001);

    vec2 position = center + offset;

    gl_Position = vec4(position, 0.0, 1.0);

    // Random stable color per triangle.
    vColor = rand3(primitiveID + 20000u);
}
