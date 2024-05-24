#version 450
#extension GL_ARB_shader_viewport_layer_array : require

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) in vec3 inColor;
layout(location = 4) in vec2 inTexCoord;

layout(std140, set = 0, binding = 0) uniform TransformUniformBufferObject {
  mat4 model;
  mat4 view;
  mat4 proj;
  mat4 normal;
  vec3 cameraPos;
}
transform;

layout(std140, set = 1, binding = 0) uniform ShadowUniformBufferObject {
  mat4 views[6];
  mat4 projection;
  mat4 model;
  vec3 lightPos;
  float farPlane;
  int layer;
}
shadow;

layout(push_constant) uniform LayerBlock { int layer; }
push;

layout(location = 0) out vec3 outPosition;

void main() {
  gl_Position = shadow.projection * shadow.views[push.layer] *
                (shadow.model * vec4(inPosition, 1.0));

  outPosition = vec3(transform.model * vec4(inPosition, 1.0));
  gl_Layer = push.layer;
}