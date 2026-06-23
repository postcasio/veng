#include <Veng/Asset/Skeleton.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Veng
{
    mat4 Skeleton::BindLocalMatrix(const usize bone) const
    {
        const Bone& b = Bones[bone];
        const mat4 translation = glm::translate(mat4(1.0f), b.LocalPosition);
        const mat4 rotation = glm::mat4_cast(b.LocalRotation);
        const mat4 scale = glm::scale(mat4(1.0f), b.LocalScale);
        return translation * rotation * scale;
    }

    void Skeleton::ComputeSkinningMatrices(std::span<const mat4> localPose, vector<mat4>& out) const
    {
        const usize count = Bones.size();
        out.resize(count);

        // modelBone(b) composes the local poses down the parent chain. Bones are topological,
        // so a parent's model matrix is always computed before its children's.
        vector<mat4> model(count);
        for (usize i = 0; i < count; ++i)
        {
            const mat4 local = i < localPose.size() ? localPose[i] : BindLocalMatrix(i);
            const i32 parent = Bones[i].Parent;
            model[i] = parent >= 0 ? model[static_cast<usize>(parent)] * local : local;
            out[i] = GlobalInverse * model[i] * Bones[i].InverseBind;
        }
    }

    void Skeleton::ComputeBindPoseMatrices(vector<mat4>& out) const
    {
        vector<mat4> local(Bones.size());
        for (usize i = 0; i < Bones.size(); ++i)
        {
            local[i] = BindLocalMatrix(i);
        }
        ComputeSkinningMatrices(local, out);
    }
}
