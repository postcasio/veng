#include "EditorCamera.h"

#include <cmath>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        /// @brief World up axis the view is built against.
        const vec3 WorldUp{0.0f, 1.0f, 0.0f};

        /// @brief Radians of yaw/pitch per pixel of mouse motion for look/orbit/turn.
        static constexpr f32 LookSensitivity = 0.005f;
        /// @brief World units of dolly per pixel of LMB vertical drag.
        static constexpr f32 DollySensitivity = 0.02f;
        /// @brief World units of pan per pixel of MMB drag, scaled by distance.
        static constexpr f32 PanSensitivity = 0.0015f;
        /// @brief Fraction of distance the wheel dolly-zoom covers per scroll unit.
        static constexpr f32 ZoomSensitivity = 0.15f;
        /// @brief Fractional change in fly speed per scroll unit while flying.
        static constexpr f32 FlySpeedScrollStep = 0.1f;
        /// @brief Speed multiplier applied while Shift is held in fly mode.
        static constexpr f32 FlyBoost = 4.0f;
        /// @brief Smallest allowed orbit/zoom distance, so the camera never crosses the pivot.
        static constexpr f32 MinDistance = 0.05f;
        /// @brief Smallest allowed fly speed.
        static constexpr f32 MinFlySpeed = 0.05f;
        /// @brief Pitch clamp just under ±90°, avoiding the gimbal flip at the pole.
        static constexpr f32 MaxPitch = glm::radians(89.0f);
    }

    EditorCamera::EditorCamera()
    {
        // Derive the initial yaw/pitch so the default pose looks from m_Position at m_Pivot.
        const vec3 toPivot = m_Pivot - m_Position;
        m_Distance = glm::length(toPivot);
        if (m_Distance > MinDistance)
        {
            const vec3 dir = toPivot / m_Distance;
            m_Pitch = glm::clamp(std::asin(dir.y), -MaxPitch, MaxPitch);
            m_Yaw = std::atan2(dir.x, -dir.z);
        }
        RebuildView(1.0f);
    }

    vec3 EditorCamera::Forward() const
    {
        const f32 cp = glm::cos(m_Pitch);
        // yaw=0, pitch=0 looks down -Z, matching the orbit reference's +Z eye offset at target.
        return vec3{glm::sin(m_Yaw) * cp, glm::sin(m_Pitch), -glm::cos(m_Yaw) * cp};
    }

    vec3 EditorCamera::Right() const
    {
        return glm::normalize(glm::cross(Forward(), WorldUp));
    }

    vec3 EditorCamera::Up() const
    {
        return glm::cross(Right(), Forward());
    }

    void EditorCamera::RebuildView(f32 aspect)
    {
        m_View.SetPerspective(m_FovY, aspect, m_Near, m_Far);
        m_View.SetView(m_Position, m_Position + Forward(), WorldUp);
    }

    bool EditorCamera::Update(const EditorCameraInput& in, f32 dt)
    {
        bool lockCursor = false;

        if (in.Focused)
        {
            if (in.MouseRight)
            {
                // RMB fly mode: mouse-look + WASDQE movement; lock the cursor for the drag.
                lockCursor = true;

                m_Yaw += in.MouseDelta.x * LookSensitivity;
                m_Pitch =
                    glm::clamp(m_Pitch - in.MouseDelta.y * LookSensitivity, -MaxPitch, MaxPitch);

                // Scroll adjusts the persisted fly speed rather than zooming while flying.
                if (in.ScrollDelta.y != 0.0f)
                {
                    m_FlySpeed *= (1.0f + in.ScrollDelta.y * FlySpeedScrollStep);
                    m_FlySpeed = glm::max(m_FlySpeed, MinFlySpeed);
                }

                f32 speed = m_FlySpeed;
                if (in.Shift)
                {
                    speed *= FlyBoost;
                }

                vec3 move{0.0f};
                const vec3 forward = Forward();
                const vec3 right = Right();
                if (in.Forward)
                {
                    move += forward;
                }
                if (in.Back)
                {
                    move -= forward;
                }
                if (in.Right)
                {
                    move += right;
                }
                if (in.Left)
                {
                    move -= right;
                }
                if (in.Up)
                {
                    move += WorldUp;
                }
                if (in.Down)
                {
                    move -= WorldUp;
                }
                if (glm::dot(move, move) > 0.0f)
                {
                    m_Position += glm::normalize(move) * speed * dt;
                }

                // Keep the pivot consistent so a later orbit/zoom revolves around what's ahead.
                m_Pivot = m_Position + forward * m_Distance;
            }
            else if (in.Alt && in.MouseLeft)
            {
                // Alt+LMB orbit: revolve yaw/pitch around the pivot at the current distance.
                m_Yaw += in.MouseDelta.x * LookSensitivity;
                m_Pitch =
                    glm::clamp(m_Pitch - in.MouseDelta.y * LookSensitivity, -MaxPitch, MaxPitch);
                m_Position = m_Pivot - Forward() * m_Distance;
            }
            else if (in.MouseLeft)
            {
                // LMB dolly + yaw: vertical drag dollies along forward, horizontal yaws in place.
                const vec3 forward = Forward();
                m_Position -= forward * (in.MouseDelta.y * DollySensitivity);
                m_Yaw += in.MouseDelta.x * LookSensitivity;
                m_Pivot = m_Position + Forward() * m_Distance;
            }
            else if (in.MouseMiddle)
            {
                // MMB pan: translate along right/up and carry the pivot along.
                const f32 scale = PanSensitivity * glm::max(m_Distance, 1.0f);
                const vec3 pan =
                    Right() * (-in.MouseDelta.x * scale) + Up() * (in.MouseDelta.y * scale);
                m_Position += pan;
                m_Pivot += pan;
            }
        }

        // Wheel dolly-zoom when not flying: move toward/away from the pivot, clamping distance.
        if ((in.Hovered || in.Focused) && !in.MouseRight && in.ScrollDelta.y != 0.0f)
        {
            m_Distance *= (1.0f - in.ScrollDelta.y * ZoomSensitivity);
            m_Distance = glm::max(m_Distance, MinDistance);
            m_Position = m_Pivot - Forward() * m_Distance;
        }

        RebuildView(in.Aspect);
        return lockCursor;
    }

    Veng::CameraView EditorCamera::GetView() const
    {
        return m_View;
    }

    void EditorCamera::Frame(vec3 center, f32 radius)
    {
        m_Pivot = center;
        // Fit the bounding sphere in the vertical FOV, with a small margin so it isn't edge-to-edge.
        const f32 fit = radius / glm::sin(m_FovY * 0.5f);
        m_Distance = glm::max(fit * 1.1f, MinDistance);
        m_Position = m_Pivot - Forward() * m_Distance;
    }

    f32 EditorCamera::GetFlySpeed() const
    {
        return m_FlySpeed;
    }

    void EditorCamera::SetFlySpeed(f32 speed)
    {
        m_FlySpeed = glm::max(speed, MinFlySpeed);
    }

    f32 EditorCamera::GetFovY() const
    {
        return m_FovY;
    }

    void EditorCamera::SetFovY(f32 fovY)
    {
        m_FovY = fovY;
    }
}
