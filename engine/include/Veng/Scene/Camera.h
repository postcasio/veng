#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief Plain CPU value type that builds the view and projection matrices a SceneView carries.
    ///
    /// The render-ready view-projection the renderer consumes through SceneView, the
    /// resolved side of the recipe→resolved pairing (a Camera component produces a
    /// CameraView, as a Primitive produces a Mesh). Pure math, no backend handles.
    /// Projection follows the engine's Vulkan clip conventions: a column-major GLM
    /// perspective with Y flipped (Vulkan clip space has Y pointing down). Matrices are
    /// recomputed on demand from stored parameters.
    class CameraView
    {
    public:
        /// @brief Sets a perspective projection.
        /// @param fovYRadians  Vertical field of view in radians.
        /// @param aspect       Width divided by height.
        /// @param near         Positive near clip distance.
        /// @param far          Positive far clip distance.
        void SetPerspective(f32 fovYRadians, f32 aspect, f32 near, f32 far)
        {
            mat4 projection = glm::perspective(fovYRadians, aspect, near, far);
            projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
            m_Projection = projection;
            m_Near = near;
            m_Far = far;
        }

        /// @brief Sets an orthographic projection.
        ///
        /// Parallel projection: no perspective foreshortening, so a face directly
        /// facing the camera projects at uniform scale regardless of depth. Follows
        /// the same Vulkan clip conventions as SetPerspective (Y flipped, ZO depth).
        /// @param halfWidth   Half the view volume's width in world units.
        /// @param halfHeight  Half the view volume's height in world units.
        /// @param near        Near clip distance.
        /// @param far         Far clip distance.
        void SetOrthographic(f32 halfWidth, f32 halfHeight, f32 near, f32 far)
        {
            mat4 projection = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, near, far);
            projection[1][1] *= -1.0f; // Vulkan's clip space has Y pointing down.
            m_Projection = projection;
            m_Near = near;
            m_Far = far;
        }

        /// @brief Sets the view matrix from eye, target, and up vectors.
        void SetView(vec3 eye, vec3 target, vec3 up) { m_View = glm::lookAt(eye, target, up); }

        /// @brief Sets the view matrix from a camera entity's world matrix.
        ///
        /// The view is the inverse of the world matrix: world places the camera,
        /// view brings the world into camera space.
        void SetViewFromWorld(const mat4& world) { m_View = glm::inverse(world); }

        /// @brief Returns the view matrix.
        [[nodiscard]] mat4 View() const { return m_View; }
        /// @brief Returns the projection matrix.
        [[nodiscard]] mat4 Projection() const { return m_Projection; }
        /// @brief Returns the combined view-projection matrix.
        [[nodiscard]] mat4 ViewProjection() const { return m_Projection * m_View; }

        /// @brief Returns the camera's world-space position (the translation column of the view's inverse).
        [[nodiscard]] vec3 GetPosition() const { return vec3(glm::inverse(m_View)[3]); }

        /// @brief Returns the near clip distance.
        ///
        /// Recovering near/far from the projection matrix is fiddly under the Y-flip,
        /// so the camera stores them. SetPerspective sets both; a camera whose
        /// projection was set by another path keeps the defaults (0.1 / 100.0).
        [[nodiscard]] f32 GetNear() const { return m_Near; }
        /// @brief Returns the far clip distance.
        [[nodiscard]] f32 GetFar() const { return m_Far; }

    private:
        /// @brief View matrix (world-to-camera).
        mat4 m_View{1.0f};
        /// @brief Projection matrix (Y-flipped for Vulkan clip space).
        mat4 m_Projection{1.0f};
        /// @brief Near clip distance, mirroring the value passed to SetPerspective.
        f32 m_Near = 0.1f;
        /// @brief Far clip distance, mirroring the value passed to SetPerspective.
        f32 m_Far = 100.0f;
    };

    /// @brief Camera component for an entity whose view derives from its world transform.
    struct Camera
    {
        /// @brief Vertical field of view in radians.
        f32 FovY = glm::radians(60.0f);
        /// @brief Near clip distance.
        f32 Near = 0.1f;
        /// @brief Far clip distance.
        f32 Far = 100.0f;
    };

    /// @brief Builds a CameraView from a Camera component, an aspect ratio, and the camera entity's world matrix.
    /// @param camera  The component supplying FovY/Near/Far.
    /// @param aspect  Viewport width divided by height.
    /// @param world   The camera entity's world matrix (from WorldMatrix in Transforms.h).
    [[nodiscard]] inline CameraView MakeCameraView(const Camera& camera, f32 aspect,
                                                   const mat4& world)
    {
        CameraView result;
        result.SetPerspective(camera.FovY, aspect, camera.Near, camera.Far);
        result.SetViewFromWorld(world);
        return result;
    }
}

VE_REFLECT(::Veng::Camera, 0x6598EF5F5C0A7B10ULL)
VE_FIELD(FovY, .DisplayName = "Field of View", .Min = 0.01)
VE_FIELD(Near, .DisplayName = "Near", .Min = 0.001)
VE_FIELD(Far, .DisplayName = "Far")
VE_REFLECT_END();
