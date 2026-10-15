#version 450
layout(location=0) in vec4 rect;
layout(location=1) in vec4 color;
layout(location=2) in uvec2 bits;
layout(location=0) out vec2 uv;
layout(location=1) flat out vec4 tint;
layout(location=2) flat out uvec2 mask;
layout(push_constant) uniform Push { vec2 extent; } pc;
void main()
{
    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    uv = corners[gl_VertexIndex];
    gl_Position = vec4((rect.xy + uv * rect.zw) / pc.extent * 2.0 - 1.0, 0, 1);
    tint = color; mask = bits;
}
