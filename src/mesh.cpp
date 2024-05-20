#include "gfxcommon.h"

#include "mesh.h"
#include "engine.h"

#include <vector>
#include <iostream>

VkVertexInputBindingDescription Vertex::getBindingDescription()
{
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    return bindingDescription;
}

std::array<VkVertexInputAttributeDescription, 5> Vertex::getAttributeDescriptions()
{
    std::array<VkVertexInputAttributeDescription, 5> attributeDescriptions{};

    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, pos);

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, normal);

    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex, tangent);

    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[3].offset = offsetof(Vertex, color);

    attributeDescriptions[4].binding = 0;
    attributeDescriptions[4].location = 4;
    attributeDescriptions[4].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[4].offset = offsetof(Vertex, texCoord);

    return attributeDescriptions;
}

void Mesh::createVertexBuffer()
{
    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

    vertexBufferAllocation = std::make_unique<BufferAllocation>(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vertexBufferAllocation->copyMemoryToAllocation(vertices.data(), 0, bufferSize);
}

void Mesh::createIndexBuffer()
{
    VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

    indexBufferAllocation = std::make_unique<BufferAllocation>(bufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    indexBufferAllocation->copyMemoryToAllocation(indices.data(), 0, bufferSize);
}

Mesh::Mesh(std::vector<Vertex> vertices, std::vector<uint16_t> indices, uint32_t materialIndex)
{
    this->vertices = vertices;
    this->indices = indices;
    this->materialIndex = materialIndex;

    createVertexBuffer();
    createIndexBuffer();
}

Mesh::~Mesh()
{
    indexBufferAllocation.reset();
    vertexBufferAllocation.reset();
}

void Mesh::draw(VkCommandBuffer commandBuffer)
{
    VkBuffer vertexBuffers[] = {vertexBufferAllocation->buffer};
    VkDeviceSize offsets[] = {0};

    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindIndexBuffer(commandBuffer, indexBufferAllocation->buffer, 0, VK_INDEX_TYPE_UINT16);

    vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
}
