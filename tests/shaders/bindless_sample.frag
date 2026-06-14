#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Samples a single bindless-registered texture/sampler pair, selected by
// push-constant indices. Used by the veng_gpu bindless registry test
// (planset-5/05) to verify Register/Bind round-trips through an actual draw.

layout(location = 0) in vec2 v_UV;

layout(location = 0) out vec4 o_Color;

// Bindless registry set 0 (Veng/Renderer/BindlessRegistry.h) — bound once via
// BindlessRegistry::Bind, never declared by the author otherwise.
layout(set = 0, binding = 0) uniform texture2D u_Textures[];
layout(set = 0, binding = 1) uniform sampler u_Samplers[];

layout(push_constant) uniform PushConstants
{
    uint u_TextureIndex;
    uint u_SamplerIndex;
};

void main()
{
    o_Color = texture(sampler2D(u_Textures[nonuniformEXT(u_TextureIndex)], u_Samplers[nonuniformEXT(u_SamplerIndex)]), v_UV);
}
