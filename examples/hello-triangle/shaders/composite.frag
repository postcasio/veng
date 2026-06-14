#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 v_UV;

layout(location = 0) out vec4 o_Color;

// Bindless registry set 0 (see Veng/Renderer/BindlessRegistry.h) — bound once
// by BindlessRegistry::Bind, never declared by the author otherwise.
layout(set = 0, binding = 0) uniform texture2D u_Textures[];
layout(set = 0, binding = 1) uniform sampler u_Samplers[];

layout(push_constant) uniform PushConstants
{
    uint u_SceneTexture;
    uint u_ImGuiTexture;
    uint u_Sampler;
};

void main()
{
    vec4 scene = texture(sampler2D(u_Textures[nonuniformEXT(u_SceneTexture)], u_Samplers[nonuniformEXT(u_Sampler)]), v_UV);
    vec4 ui = texture(sampler2D(u_Textures[nonuniformEXT(u_ImGuiTexture)], u_Samplers[nonuniformEXT(u_Sampler)]), v_UV);

    o_Color = vec4(mix(scene.rgb, ui.rgb, ui.a), 1.0);
}
