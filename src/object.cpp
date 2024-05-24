#include "gfxcommon.h"

#include <vector>
#include <iostream>
#include "object.h"

Object::Object(std::shared_ptr<Model> model)
{
    this->model = model;
}

std::shared_ptr<Model> Object::getModel()
{
    return model;
}

void Object::setModel(std::shared_ptr<Model> model)
{
    this->model = model;
}

void Object::draw(CommandBuffer &commandBuffer)
{
    if (model == nullptr)
    {
        return;
    }

    for (std::shared_ptr<Mesh> mesh : model->getMeshes())
    {
        mesh->draw(commandBuffer);
    }
}
void Object::add(std::shared_ptr<Object> object)
{
    children.push_back(object);
    object->parent = this;
}

void Object::remove(std::shared_ptr<Object> object)
{
    children.erase(std::remove(children.begin(), children.end(), object));
    object->parent = nullptr;
}

void Object::updateMatrix()
{
    // Create a transformation matrix from the position
    glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), position);

    // Create a transformation matrix from the quaternion
    glm::mat4 rotationMatrix = glm::mat4_cast(quaternion);

    // Create a transformation matrix from the scale
    glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);

    // Multiply the matrices together to get the final transformation matrix
    // matrix = rotationMatrix * translationMatrix * scaleMatrix;
    // matrix = translationMatrix * rotationMatrix * scaleMatrix;
    // matrix = translationMatrix * scaleMatrix * rotationMatrix;
    matrix = scaleMatrix * translationMatrix * rotationMatrix;
    // matrix = scaleMatrix * rotationMatrix * translationMatrix;
    matrixDirty = false;
    worldMatrixDirty = true;
}

void Object::updateWorldMatrix()
{
    worldMatrixUpdatedThisFrame = false;

    if (matrixDirty)
    {
        updateMatrix();
    }

    if (worldMatrixDirty)
    {
        if (parent == nullptr)
        {
            worldMatrix = matrix;
        }
        else
        {
            worldMatrix = parent->worldMatrix * matrix;
        }

        inverseWorldMatrix = glm::inverse(worldMatrix);

        for (auto child : children)
        {
            child->updateWorldMatrix();
        }

        worldMatrixDirty = false;
        worldMatrixUpdatedThisFrame = true;
    }
}

bool Object::hasModel()
{
    return model != nullptr;
}

void Object::updateUniforms(uint32_t currentFrame)
{
    // if (model != nullptr)
    // {
    //     for (auto mesh : model->getMeshes())
    //     {
    //         mesh->uniforms.updateUniformBuffer(currentFrame);
    //     }
    // }

    // for (auto child : children)
    // {
    //     child->updateUniforms(currentFrame);
    // }
}

void Object::lookAt(glm::vec3 target)
{
    glm::mat4 lookAtMatrix = glm::inverse(glm::lookAt(position, target, UP)); // position is the object's position

    // Extract the rotation matrix part of the view matrix
    glm::mat3 rotationMatrix = glm::mat3(lookAtMatrix);

    // Convert the rotation matrix to a quaternion
    quaternion = glm::quat(rotationMatrix);

    matrixDirty = true;
}
void Object::translateOnAxis(glm::vec3 axis, float distance)
{
    position += this->quaternion * glm::normalize(axis) * distance;

    matrixDirty = true;
}
void Object::translateX(float distance)
{
    translateOnAxis(glm::vec3(1.0f, 0.0f, 0.0f), distance);
}
void Object::translateY(float distance)
{
    translateOnAxis(glm::vec3(0.0f, 1.0f, 0.0f), distance);
}
void Object::translateZ(float distance)
{
    translateOnAxis(glm::vec3(0.0f, 0.0f, 1.0f), distance);
}
void Object::rotateOnAxis(glm::vec3 axis, float angle)
{
    quaternion = glm::angleAxis(angle, glm::normalize(axis)) * quaternion;

    matrixDirty = true;
}
void Object::rotateX(float angle)
{
    rotateOnAxis(glm::vec3(1.0f, 0.0f, 0.0f), angle);
}
void Object::rotateY(float angle)
{
    rotateOnAxis(glm::vec3(0.0f, 1.0f, 0.0f), angle);
}
void Object::rotateZ(float angle)
{
    rotateOnAxis(glm::vec3(0.0f, 0.0f, 1.0f), angle);
}