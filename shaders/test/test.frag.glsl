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

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragPosTangentSpace;
layout(location = 4) in vec3 viewPosTangentSpace;
layout(location = 5) in vec3 fragNormal;
layout(location = 6) in vec3 lightPosTangentSpace[4];

layout(location = 0) out vec4 outColor;

layout(std140, set = 1, binding = 0) uniform UniformBufferObject {
  mat4 model;
  mat4 view;
  mat4 proj;
  mat4 normal;
  vec3 cameraPos;
}
transform;

layout(set = 0, binding = 0) uniform sampler2D texSampler;
layout(set = 0, binding = 1) uniform sampler2D normalSampler;
layout(set = 0, binding = 3) uniform sampler2D occlusionSampler;

layout(std140, set = 2, binding = 0) uniform PointLightBufferObject {
  PointLight pointLights[4];
  uint pointLightCount;
}
lighting;

vec3 normalLookup() {
  vec3 n = texture(normalSampler, fragTexCoord).rgb;

  return normalize(n * 2.0 - 1.0);
}

float occlusionLookup() { return texture(occlusionSampler, fragTexCoord).r; }

vec3 calculatePointLight(int lightIndex, vec3 normal) {
  PointLight light = lighting.pointLights[lightIndex];

  vec3 viewDir = normalize(viewPosTangentSpace - fragPosTangentSpace);

  // ambient + diffuse
  vec3 ambient = light.color * 0.3 * occlusionLookup();

  vec3 lightDir =
      normalize(lightPosTangentSpace[lightIndex] - fragPosTangentSpace);
  float diff = max(dot(normal, lightDir), 0.0);
  vec3 diffuse = diff * light.color.rgb;

  // specular

  vec3 reflectDir = reflect(-lightDir, normal);
  float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);

  vec3 specular = 1.0 * spec * light.color.rgb;

  float dist = length(lightPosTangentSpace[lightIndex] - fragPosTangentSpace);
  float attenuation = 1.0 / (light.constant + light.linear * dist +
                             light.quadratic * (dist * dist));

  vec3 result = (ambient + diffuse + specular) * light.intensity * attenuation;

  return result;

  // return ambient * light.intensity * attenuation *
  //        texture(texSampler, fragTexCoord).rgb;
}

void main() {
  vec3 color = vec3(0.0, 0.0, 0.0);

  // outColor = vec4(
  //     (normalize(lightPosTangentSpace[0] - fragPosTangentSpace) + 1.0) / 2.0,
  //     1.0);
  // return;

  vec3 diffuse = texture(texSampler, fragTexCoord).rgb;
  for (int i = 0; i < lighting.pointLightCount; i++) {
    color += calculatePointLight(i, fragNormal) * diffuse;
  }

  outColor = vec4(color, 1.0);

  // outColor = vec4(viewPosTangentSpace, 1.0);
  // outColor = vec4(normalLookup(), 1.0);
  // outColor =
  //     vec4(calculatePointLight(0, fragNormal) *
  //     lightPosTangentSpace[0], 1.0);
  //   outColor = vec4(vec3(float(lighting.pointLightCount)), 1.0);
}