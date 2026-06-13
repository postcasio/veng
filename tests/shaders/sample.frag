#version 450

// Samples the compute pass's output image and writes it straight through.
// Used by the compute-dispatch test to verify the compute-written storage
// image is correctly readable by a later graphics pass.

layout(location = 0) in vec2 v_UV;

layout(location = 0) out vec4 o_Color;

layout(set = 0, binding = 0) uniform sampler2D u_Source;

void main()
{
    o_Color = texture(u_Source, v_UV);
}
