#version 450

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec3 a_Color;
layout(location = 2) in vec2 a_UV;

layout(location = 0) out vec3 v_Color;
layout(location = 1) out vec2 v_UV;

layout(push_constant) uniform PushConstants
{
    mat4 Transform;
} u_Push;

void main()
{
    gl_Position = u_Push.Transform * vec4(a_Position, 0.0, 1.0);
    v_Color = a_Color;
    v_UV = a_UV;
}
