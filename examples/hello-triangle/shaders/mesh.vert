#version 450

// veng's canonical mesh vertex layout (position/normal/tangent/uv) — see
// Renderer::Mesh::CanonicalLayout(). The cooked cube is uploaded in this layout.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent; // xyz = tangent, w = bitangent handedness
layout(location = 3) in vec2 a_UV;

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec2 v_UV;

layout(push_constant) uniform PushConstants
{
    mat4 MVP;
    mat4 Model;
} u_Push;

void main()
{
    gl_Position = u_Push.MVP * vec4(a_Position, 1.0);
    v_Normal = mat3(u_Push.Model) * a_Normal;
    v_UV = a_UV;
}
