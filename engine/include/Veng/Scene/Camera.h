#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    // A plain CPU value type that builds the view + projection matrices a scene
    // renderer draws through — the thing the future SceneView will carry. Pure
    // math, no backend handles; the sample uses it directly in place of a
    // hand-rolled MVP.
    //
    // Projection follows the engine's clip conventions: a column-major GLM
    // perspective with the Y axis flipped (Vulkan clip space has Y pointing
    // down). The matrices are recomputed on demand from the stored parameters.
    class Camera
    {
    public:
        // fovYRadians is the vertical field of view; near/far are positive clip
        // distances. aspect is width / height.
        void SetPerspective(f32 fovYRadians, f32 aspect, f32 near, f32 far)
        {
            mat4 projection = glm::perspective(fovYRadians, aspect, near, far);
            projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
            m_Projection = projection;
        }

        void SetView(vec3 eye, vec3 target, vec3 up)
        {
            m_View = glm::lookAt(eye, target, up);
        }

        // Sets the view from a camera entity's world matrix: the view is its
        // inverse (world places the camera; view brings the world into camera
        // space).
        void SetViewFromWorld(const mat4& world)
        {
            m_View = glm::inverse(world);
        }

        [[nodiscard]] mat4 View() const { return m_View; }
        [[nodiscard]] mat4 Projection() const { return m_Projection; }
        [[nodiscard]] mat4 ViewProjection() const { return m_Projection * m_View; }

        // The camera's world-space position: the translation column of the view's
        // inverse (the world matrix that placed the camera).
        [[nodiscard]] vec3 GetPosition() const { return vec3(glm::inverse(m_View)[3]); }

    private:
        mat4 m_View{1.0f};
        mat4 m_Projection{1.0f};
    };

    // A camera that lives on an entity; its view derives from the entity's world
    // transform. Registered like every other component for the future scene
    // renderer; the sample wires a bare Camera and registers this for
    // completeness.
    struct CameraComponent
    {
        f32 FovY = glm::radians(60.0f);
        f32 Near = 0.1f;
        f32 Far = 100.0f;
    };

    // Builds a Camera from a CameraComponent, an aspect ratio, and the camera
    // entity's world matrix (WorldMatrix from Transforms.h).
    [[nodiscard]] inline Camera MakeCamera(const CameraComponent& camera, f32 aspect, const mat4& world)
    {
        Camera result;
        result.SetPerspective(camera.FovY, aspect, camera.Near, camera.Far);
        result.SetViewFromWorld(world);
        return result;
    }
}

VE_REFLECT(::Veng::CameraComponent, 0x6598EF5F5C0A7B10ULL)
    VE_FIELD(FovY, .DisplayName = "Field of View", .Min = 0.01)
    VE_FIELD(Near, .DisplayName = "Near", .Min = 0.001)
    VE_FIELD(Far, .DisplayName = "Far")
VE_REFLECT_END();
