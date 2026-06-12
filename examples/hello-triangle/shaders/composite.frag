#version 450

layout(location = 0) in vec2 v_UV;

layout(location = 0) out vec4 o_Color;

layout(set = 0, binding = 0) uniform sampler2D u_Scene;
layout(set = 0, binding = 1) uniform sampler2D u_ImGui;

void main()
{
    vec4 scene = texture(u_Scene, v_UV);
    vec4 ui = texture(u_ImGui, v_UV);

    o_Color = vec4(mix(scene.rgb, ui.rgb, ui.a), 1.0);
}
