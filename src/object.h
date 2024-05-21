#ifndef _OBJECT_H_
#define _OBJECT_H_

#include "gpu/command_buffer.h"

enum class ObjectType
{
    Object,
    PointLight,
    Camera
};

#include "gfxcommon.h"

#include <vector>

#include "model.h"

const glm::vec3 UP = glm::vec3(0.0f, 1.0f, 0.0f);

class Object
{
public:
    Object(std::shared_ptr<Model> model);

    ObjectType type = ObjectType::Object;

    std::shared_ptr<Model> getModel();
    Object *parent = nullptr;
    void setModel(std::shared_ptr<Model> model);
    bool hasModel();

    void draw(CommandBuffer &commandBuffer);

    std::vector<std::shared_ptr<Object>> children;

    void add(std::shared_ptr<Object> object);

    void remove(std::shared_ptr<Object> object);

    glm::mat4 matrix;
    glm::mat4 worldMatrix;
    glm::mat4 inverseWorldMatrix;

    glm::vec3 position = glm::vec3(0.0f);
    glm::quat quaternion = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);

    bool visible = true;

    void updateMatrix();
    void updateWorldMatrix();
    void updateUniforms(uint32_t currentFrame);

    bool worldMatrixDirty = true;
    bool matrixDirty = true;

    void lookAt(glm::vec3 target);

    void translateOnAxis(glm::vec3 axis, float distance);
    void translateX(float distance);
    void translateY(float distance);
    void translateZ(float distance);
    void rotateOnAxis(glm::vec3 axis, float angle);
    void rotateX(float angle);
    void rotateY(float angle);
    void rotateZ(float angle);

private:
    std::shared_ptr<Model> model;
};

#endif