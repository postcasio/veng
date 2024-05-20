#version 450

struct PointLight {
  vec3 position;
  vec3 color;
  float intensity;
  float constant;
  float linear;
  float quadratic;
  float radius;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) in vec3 inColor;
layout(location = 4) in vec2 inTexCoord;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec3 fragPosTangentSpace;
layout(location = 4) out vec3 viewPosTangentSpace;
layout(location = 5) out vec3 fragNormal;
layout(location = 6) out vec3 lightPosTangentSpace[4];

layout(set = 0, binding = 2) uniform sampler2D displacementSampler;

layout(set = 1, binding = 0) uniform TransformUniformBufferObject {
  mat4 model;
  mat4 view;
  mat4 proj;
  mat4 normal;
  vec3 cameraPos;
}
transform;

layout(std140, set = 2, binding = 0) uniform PointLightBufferObject {
  PointLight pointLights[4];
  uint pointLightCount;
}
lighting;

void main() {
  float disp = texture(displacementSampler, inTexCoord).r;
  vec3 displace = inPosition;

  float displaceFactor = 0.1;
  float displaceBias = 0.01;

  displace.xyz += (displaceFactor * disp - displaceBias) * inNormal;

  gl_Position =
      // vec4(displace, 1.0) * transform.model * transform.view *
      // transform.proj;
      transform.proj * transform.view * transform.model * vec4(displace, 1.0);

  fragColor = inColor;
  fragTexCoord = inTexCoord;
  fragPos = vec3(transform.model * vec4(inPosition, 1.0));

  vec3 T = normalize(vec3(transform.normal * vec4(inTangent, 0.0)));
  vec3 N = normalize(vec3(transform.normal * vec4(inNormal, 0.0)));
  vec3 B = cross(N, T);

  mat3 fragTBN = mat3(T, B, N);

  fragPosTangentSpace = fragTBN * fragPos;
  viewPosTangentSpace = fragTBN * transform.cameraPos;

  for (int i = 0; i < lighting.pointLightCount; i++) {
    lightPosTangentSpace[i] = fragTBN * lighting.pointLights[i].position;
  }

  fragNormal = inNormal;
}