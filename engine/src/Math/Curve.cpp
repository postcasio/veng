#include <Veng/Math/Curve.h>

#include <algorithm>

namespace Veng
{
    f32 Curve1D::Evaluate(const f32 time) const
    {
        if (Keys.empty())
        {
            return 0.0f;
        }
        if (time <= Keys.front().Time)
        {
            return Keys.front().Value;
        }
        if (time >= Keys.back().Time)
        {
            return Keys.back().Value;
        }

        // The first key strictly after the time, so the segment [prev, next) has a positive span
        // even where two keys share a time.
        const auto next = std::ranges::upper_bound(Keys, time, {}, &CurveKey::Time);
        const CurveKey& b = *next;
        const CurveKey& a = *(next - 1);

        const f32 u = (time - a.Time) / (b.Time - a.Time);
        switch (a.Interp)
        {
        case CurveInterp::Step:
            return a.Value;
        case CurveInterp::Smooth:
            return a.Value + ((b.Value - a.Value) * u * u * (3.0f - (2.0f * u)));
        case CurveInterp::Linear:
            break;
        }
        return a.Value + ((b.Value - a.Value) * u);
    }
}
