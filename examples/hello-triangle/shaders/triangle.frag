#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 v_Color;
layout(location = 1) in vec2 v_UV;

layout(location = 0) out vec4 o_Color;

// Bindless registry set 0 (Veng/Renderer/BindlessRegistry.h) — bound once via
// BindlessRegistry::Bind, never declared by the author otherwise.
layout(set = 0, binding = 0) uniform texture2D u_Textures[];
layout(set = 0, binding = 1) uniform sampler u_Samplers[];

// Second push-constant range (Fragment stage, offset 64 — past the vertex
// stage's mat4 Transform at offset 0).
layout(push_constant) uniform PushConstants
{
    layout(offset = 64) uint u_TextureIndex;
    uint u_SamplerIndex;
};

void main()
{
    vec4 brick = texture(sampler2D(u_Textures[nonuniformEXT(u_TextureIndex)], u_Samplers[nonuniformEXT(u_SamplerIndex)]), v_UV);
    o_Color = vec4(v_Color, 1.0) * brick;
}
