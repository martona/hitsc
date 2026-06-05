#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D tex;

// Cursor overlay quad. Unlike viewer_quad.frag (which forces alpha = 1 for the
// opaque base frame), the cursor sprite carries straight (non-premultiplied)
// alpha -- transparent outside the cursor shape, partial for type-1 alpha
// cursors -- so we pass it through for the pipeline's SrcAlpha/OneMinusSrcAlpha
// blend.
void main()
{
    fragColor = texture(tex, v_texcoord);
}
