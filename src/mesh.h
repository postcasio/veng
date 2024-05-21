#ifndef _MESH_H_
#define _MESH_H_

#include "gfxcommon.h"

#include "transform_uniforms.h"
#include "texture.h"
#include "gpu/command_buffer.h"

#include <vector>

struct Vertex
{
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 tangent;
    glm::vec3 color;
    glm::vec2 texCoord;

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 5> getAttributeDescriptions();
};

class Mesh
{
public:
    Mesh(std::vector<Vertex> vertices, std::vector<uint16_t> indices, uint32_t materialIndex);
    ~Mesh();

    uint32_t materialIndex;

    void draw(CommandBuffer &commandBuffer);

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;

    std::unique_ptr<BufferAllocation> vertexBufferAllocation;
    std::unique_ptr<BufferAllocation> indexBufferAllocation;

private:
    void createVertexBuffer();
    void createIndexBuffer();
};

#endif