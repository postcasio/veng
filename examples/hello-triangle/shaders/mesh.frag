#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec2 v_UV;

layout(location = 0) out vec4 o_Color;

// Bindless registry set 0 (Veng/Renderer/BindlessRegistry.h) — bound once via
// BindlessRegistry::Bind, never declared by the author otherwise.
layout(set = 0, binding = 0) uniform texture2D u_Textures[];
layout(set = 0, binding = 1) uniform sampler u_Samplers[];

// Fragment-stage push-constant range (offset 128 — past the vertex stage's two
// mat4s: MVP at 0, Model at 64).
layout(push_constant) uniform PushConstants
{
    layout(offset = 128) uint u_TextureIndex;
    uint u_SamplerIndex;
};

void main()
{
    vec4 brick = texture(sampler2D(u_Textures[nonuniformEXT(u_TextureIndex)], u_Samplers[nonuniformEXT(u_SamplerIndex)]), v_UV);

    // Simple directional lambert so the cube's faces read as distinct.
    vec3 normal = normalize(v_Normal);
    vec3 light = normalize(vec3(0.4, 0.8, 0.6));
    float diffuse = max(dot(normal, light), 0.0);
    float ambient = 0.25;

    o_Color = vec4(brick.rgb * (ambient + diffuse), brick.a);
}
