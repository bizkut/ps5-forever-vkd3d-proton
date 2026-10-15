#version 450
layout(location=0) in vec2 uv;
layout(location=1) flat in vec4 tint;
layout(location=2) flat in uvec2 mask;
layout(location=0) out vec4 color;
void main()
{
    if (mask != uvec2(0xffffffffu))
    {
        uvec2 cell = min(uvec2(uv * vec2(5,7)), uvec2(4,6));
        uint bit = cell.y * 5 + (4 - cell.x);
        if (((mask[bit / 32] >> (bit % 32)) & 1u) == 0) discard;
    }
    color = tint;
}
