#version 420 core

const mat4 modelviewMatrix = mat4(0.696364, 0.369616, 0.615192, 0.000000,
                                  0.122788, 0.783188, -0.609540, 0.000000,
                                  -0.707107, 0.500000, 0.500000, 0.000000,
                                  0.000000, 0.000000, -8.000000, 1.000000);
const mat4 modelviewprojectionMatrix = mat4(1.492209, 0.792034, -1.537979, -0.615192,
                                            0.263117, 1.678261, 1.523850, 0.609540,
                                            -1.515229, 1.071428, -1.250000, -0.500000,
                                            0.000000, 0.000000, 5.000000, 8.000000);

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_normal;

vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);

layout(location = 0) out vec4 vVaryingColor;

void main()
{
    gl_Position = modelviewprojectionMatrix * in_position;
    vec3 vEyeNormal = mat3(modelviewMatrix) * in_normal;
    vec4 vPosition4 = modelviewMatrix * in_position;
    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;
    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);
    float diff = max(0.0, dot(vEyeNormal, vLightDir));
    vVaryingColor = vec4(diff * in_color.rgb, 1.0);
}
