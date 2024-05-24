#version 450

layout(std140, set = 0, binding = 0) uniform UniformBufferObject {
  mat4 model;
  mat4 view;
  mat4 proj;
  mat3 normal;
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

layout(location = 0) in vec3 inPosition;

void main() {
  vec3 lightVec = inPosition - shadow.lightPos;
  gl_FragDepth = length(lightVec) / shadow.farPlane;
}