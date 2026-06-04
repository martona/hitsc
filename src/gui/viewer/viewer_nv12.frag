#version 440

// Samples an NV12 frame imported as two textures (Y plane = R8 full-res, UV plane
// = RG8 half-res) and converts to RGB in-shader -- no separate D3D11 conversion
// pass. Color matrix: BT.709 limited-range (typical for >=720p H.264). If colors
// look washed/oversaturated, this matrix (or the range) is the thing to tweak.
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D yPlane;
layout(binding = 1) uniform sampler2D uvPlane;

void main()
{
    float y = texture(yPlane, v_texcoord).r;
    vec2 uv = texture(uvPlane, v_texcoord).rg - vec2(0.5, 0.5);

    float yy = (y - 0.0625) * 1.164383;
    float r = yy + 1.792741 * uv.y;
    float g = yy - 0.213249 * uv.x - 0.532909 * uv.y;
    float b = yy + 2.112402 * uv.x;

    fragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
