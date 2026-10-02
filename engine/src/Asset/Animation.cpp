#include <Veng/Asset/Animation.h>

#include <glm/common.hpp>

namespace Veng
{
    i32 FindAnimatedRootBone(const Animation& animation, const usize boneLimit)
    {
        constexpr f32 VaryEpsilon = 1e-4f;

        i32 best = -1;
        for (const AnimationChannel& channel : animation.Channels)
        {
            if (channel.Position.size() < 2 || static_cast<usize>(channel.BoneIndex) >= boneLimit)
            {
                continue;
            }

            vec3 lo = channel.Position.front().Value;
            vec3 hi = lo;
            for (const Vec3Key& key : channel.Position)
            {
                lo = glm::min(lo, key.Value);
                hi = glm::max(hi, key.Value);
            }

            const vec3 range = hi - lo;
            if (range.x <= VaryEpsilon && range.y <= VaryEpsilon && range.z <= VaryEpsilon)
            {
                continue;
            }

            const auto bone = static_cast<i32>(channel.BoneIndex);
            if (best < 0 || bone < best)
            {
                best = bone;
            }
        }
        return best;
    }
}
