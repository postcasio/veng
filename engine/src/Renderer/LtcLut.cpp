#include <Veng/Renderer/LtcLut.h>

#include <glm/glm.hpp>

#include <cmath>

// A CPU reproduction of the LTC fitting procedure of Heitz et al. ("Real-Time Polygonal-Light
// Shading with Linearly Transformed Cosines"): for each (roughness, view-angle) cell, fit an LTC
// matrix to the GGX microfacet BRDF by minimizing the L2 error under a dual (BRDF + LTC)
// importance-sampled estimator, warm-started along each roughness column. Pure glm; no device.

namespace Veng::Renderer
{
    namespace
    {
        constexpr f32 Pi = 3.14159265358979323846f;
        // Roughness floor: the fit is unstable at a perfect mirror, and the lighting pass clamps
        // sampled roughness above this anyway.
        constexpr f32 MinAlpha = 0.0001f;
        // Samples per axis for the one-time average-terms integral (norm/fresnel/direction).
        constexpr u32 AvgSamples = 32;
        // Samples per axis inside the fit's error estimator; smaller keeps the O(cells · iters ·
        // samples²) fit tractable at startup without visibly degrading the tables.
        constexpr u32 FitSamples = 12;
        // Nelder-Mead iteration cap per cell.
        constexpr u32 MaxIters = 48;

        // GGX microfacet BRDF in the local frame (N = +Z), Fresnel factored out (F = 1). Mirrors
        // the fitting reference: eval returns the BRDF divided by N·V (the cosine folds into the
        // estimator through the pdf), and sampling draws a half-vector in slope space.
        struct BrdfGgx
        {
            // Smith masking/shadowing lambda, transcendental-free: tan(acos(c)) = sqrt(1-c²)/c.
            static f32 Lambda(f32 alpha, f32 cosTheta)
            {
                if (cosTheta >= 1.0f)
                {
                    return 0.0f;
                }
                const f32 sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
                const f32 a = cosTheta / (alpha * std::max(sinTheta, 1e-7f));
                return 0.5f * (-1.0f + std::sqrt(1.0f + 1.0f / (a * a)));
            }

            static f32 Eval(const vec3& V, const vec3& L, f32 alpha, f32& pdf)
            {
                if (V.z <= 0.0f)
                {
                    pdf = 0.0f;
                    return 0.0f;
                }

                const f32 lambdaV = Lambda(alpha, V.z);
                f32 g2 = 0.0f;
                if (L.z > 0.0f)
                {
                    const f32 lambdaL = Lambda(alpha, L.z);
                    g2 = 1.0f / (1.0f + lambdaV + lambdaL);
                }

                const vec3 H = glm::normalize(V + L);
                const f32 slopeX = H.x / H.z;
                const f32 slopeY = H.y / H.z;
                f32 d = 1.0f / (1.0f + (slopeX * slopeX + slopeY * slopeY) / (alpha * alpha));
                d = d * d;
                d = d / (Pi * alpha * alpha * H.z * H.z * H.z * H.z);

                pdf = std::fabs(d * H.z / (4.0f * glm::dot(V, H)));
                return d * g2 / (4.0f * V.z);
            }

            static vec3 Sample(const vec3& V, f32 alpha, f32 u1, f32 u2)
            {
                const f32 phi = 2.0f * Pi * u1;
                const f32 r = alpha * std::sqrt(u2 / (1.0f - u2));
                const vec3 n = glm::normalize(vec3(r * std::cos(phi), r * std::sin(phi), 1.0f));
                return -V + 2.0f * n * glm::dot(n, V);
            }
        };

        // The linearly-transformed cosine: a clamped-cosine distribution warped by M = frame · core,
        // with the core parametrized by (m11, m22, m13). magnitude/fresnel carry the BRDF's norm and
        // Fresnel weight for the split-tint the shader applies.
        struct Ltc
        {
            f32 Magnitude = 1.0f;
            f32 Fresnel = 1.0f;
            f32 M11 = 1.0f;
            f32 M22 = 1.0f;
            f32 M13 = 0.0f;
            vec3 X{1.0f, 0.0f, 0.0f};
            vec3 Y{0.0f, 1.0f, 0.0f};
            vec3 Z{0.0f, 0.0f, 1.0f};
            mat3 M{1.0f};
            mat3 InvM{1.0f};
            f32 DetM = 1.0f;

            void Update()
            {
                const mat3 frame(X, Y, Z);
                const mat3 core(M11, 0.0f, 0.0f, 0.0f, M22, 0.0f, M13, 0.0f, 1.0f);
                M = frame * core;
                InvM = glm::inverse(M);
                DetM = std::fabs(glm::determinant(M));
            }

            [[nodiscard]] f32 Eval(const vec3& L) const
            {
                const vec3 lOriginal = glm::normalize(InvM * L);
                const vec3 lTransformed = M * lOriginal;
                const f32 len = glm::length(lTransformed);
                const f32 jacobian = DetM / (len * len * len);
                const f32 d = (1.0f / Pi) * std::max(0.0f, lOriginal.z);
                return Magnitude * d / jacobian;
            }

            [[nodiscard]] vec3 Sample(f32 u1, f32 u2) const
            {
                const f32 theta = std::acos(std::sqrt(u1));
                const f32 phi = 2.0f * Pi * u2;
                return glm::normalize(M * vec3(std::sin(theta) * std::cos(phi),
                                               std::sin(theta) * std::sin(phi), std::cos(theta)));
            }
        };

        void ComputeAvgTerms(const vec3& V, f32 alpha, f32& norm, f32& fresnel, vec3& averageDir)
        {
            norm = 0.0f;
            fresnel = 0.0f;
            averageDir = vec3(0.0f);

            for (u32 j = 0; j < AvgSamples; ++j)
            {
                for (u32 i = 0; i < AvgSamples; ++i)
                {
                    const f32 u1 = (static_cast<f32>(i) + 0.5f) / AvgSamples;
                    const f32 u2 = (static_cast<f32>(j) + 0.5f) / AvgSamples;

                    const vec3 L = BrdfGgx::Sample(V, alpha, u1, u2);
                    f32 pdf = 0.0f;
                    const f32 eval = BrdfGgx::Eval(V, L, alpha, pdf);
                    if (pdf > 0.0f)
                    {
                        const f32 weight = eval / pdf;
                        const vec3 H = glm::normalize(V + L);
                        norm += weight;
                        fresnel += weight * std::pow(1.0f - std::max(glm::dot(V, H), 0.0f), 5.0f);
                        averageDir += weight * L;
                    }
                }
            }

            norm /= static_cast<f32>(AvgSamples * AvgSamples);
            fresnel /= static_cast<f32>(AvgSamples * AvgSamples);
            averageDir.y = 0.0f;
            averageDir = glm::normalize(averageDir);
        }

        f32 ComputeError(const Ltc& ltc, const vec3& V, f32 alpha)
        {
            f64 error = 0.0;
            for (u32 j = 0; j < FitSamples; ++j)
            {
                for (u32 i = 0; i < FitSamples; ++i)
                {
                    const f32 u1 = (static_cast<f32>(i) + 0.5f) / FitSamples;
                    const f32 u2 = (static_cast<f32>(j) + 0.5f) / FitSamples;

                    // The dual estimator: importance-sample the LTC and the BRDF, weighting the
                    // cubed error by the summed pdfs (multiple importance sampling).
                    {
                        const vec3 L = ltc.Sample(u1, u2);
                        f32 pdfBrdf = 0.0f;
                        const f32 evalBrdf = BrdfGgx::Eval(V, L, alpha, pdfBrdf);
                        const f32 evalLtc = ltc.Eval(L);
                        const f32 pdfLtc = evalLtc / ltc.Magnitude;
                        f64 e = std::fabs(evalBrdf - evalLtc);
                        e = e * e * e;
                        error += e / (pdfLtc + pdfBrdf);
                    }
                    {
                        const vec3 L = BrdfGgx::Sample(V, alpha, u1, u2);
                        f32 pdfBrdf = 0.0f;
                        const f32 evalBrdf = BrdfGgx::Eval(V, L, alpha, pdfBrdf);
                        const f32 evalLtc = ltc.Eval(L);
                        const f32 pdfLtc = evalLtc / ltc.Magnitude;
                        f64 e = std::fabs(evalBrdf - evalLtc);
                        e = e * e * e;
                        error += e / (pdfLtc + pdfBrdf);
                    }
                }
            }
            return static_cast<f32>(error / static_cast<f64>(FitSamples * FitSamples));
        }

        // Downhill-simplex (Nelder-Mead) over the three LTC core parameters, minimizing ComputeError.
        // Isotropic cells (normal incidence) tie m22 to m11 and pin m13 = 0.
        void Fit(Ltc& ltc, const vec3& V, f32 alpha, bool isotropic)
        {
            const auto apply = [&](const f32* p)
            {
                const f32 m11 = std::max(p[0], 1e-7f);
                const f32 m22 = std::max(p[1], 1e-7f);
                const f32 m13 = p[2];
                if (isotropic)
                {
                    ltc.M11 = m11;
                    ltc.M22 = m11;
                    ltc.M13 = 0.0f;
                }
                else
                {
                    ltc.M11 = m11;
                    ltc.M22 = m22;
                    ltc.M13 = m13;
                }
                ltc.Update();
            };
            const auto objective = [&](const f32* p) -> f32
            {
                apply(p);
                return ComputeError(ltc, V, alpha);
            };

            constexpr int Dim = 3;
            constexpr f32 Reflect = 1.0f;
            constexpr f32 Expand = 2.0f;
            constexpr f32 Contract = 0.5f;
            constexpr f32 Shrink = 0.5f;
            constexpr f32 Delta = 0.05f;
            constexpr f32 Tolerance = 1e-5f;

            f32 s[Dim + 1][Dim];
            f32 fval[Dim + 1];
            const f32 start[Dim] = {ltc.M11, ltc.M22, ltc.M13};
            for (int i = 0; i <= Dim; ++i)
            {
                for (int k = 0; k < Dim; ++k)
                {
                    s[i][k] = start[k];
                }
                if (i > 0)
                {
                    s[i][i - 1] += Delta;
                }
                fval[i] = objective(s[i]);
            }

            int lo = 0;
            for (u32 iter = 0; iter < MaxIters; ++iter)
            {
                lo = 0;
                int hi = 0;
                int nh = 0;
                for (int i = 1; i <= Dim; ++i)
                {
                    if (fval[i] < fval[lo])
                    {
                        lo = i;
                    }
                    if (fval[i] > fval[hi])
                    {
                        nh = hi;
                        hi = i;
                    }
                    else if (fval[i] > fval[nh])
                    {
                        nh = i;
                    }
                }

                const f32 a = std::fabs(fval[lo]);
                const f32 b = std::fabs(fval[hi]);
                if (2.0f * std::fabs(a - b) < (a + b) * Tolerance)
                {
                    break;
                }

                f32 o[Dim] = {0.0f, 0.0f, 0.0f};
                for (int i = 0; i <= Dim; ++i)
                {
                    if (i == hi)
                    {
                        continue;
                    }
                    for (int k = 0; k < Dim; ++k)
                    {
                        o[k] += s[i][k];
                    }
                }
                for (int k = 0; k < Dim; ++k)
                {
                    o[k] /= static_cast<f32>(Dim);
                }

                f32 r[Dim];
                for (int k = 0; k < Dim; ++k)
                {
                    r[k] = o[k] + Reflect * (o[k] - s[hi][k]);
                }
                const f32 fr = objective(r);

                if (fr < fval[nh])
                {
                    if (fr < fval[lo])
                    {
                        f32 e[Dim];
                        for (int k = 0; k < Dim; ++k)
                        {
                            e[k] = o[k] + Expand * (o[k] - s[hi][k]);
                        }
                        const f32 fe = objective(e);
                        if (fe < fr)
                        {
                            for (int k = 0; k < Dim; ++k)
                            {
                                s[hi][k] = e[k];
                            }
                            fval[hi] = fe;
                            continue;
                        }
                    }
                    for (int k = 0; k < Dim; ++k)
                    {
                        s[hi][k] = r[k];
                    }
                    fval[hi] = fr;
                    continue;
                }

                f32 c[Dim];
                for (int k = 0; k < Dim; ++k)
                {
                    c[k] = o[k] - Contract * (o[k] - s[hi][k]);
                }
                const f32 fc = objective(c);
                if (fc < fval[hi])
                {
                    for (int k = 0; k < Dim; ++k)
                    {
                        s[hi][k] = c[k];
                    }
                    fval[hi] = fc;
                    continue;
                }

                for (int k = 0; k <= Dim; ++k)
                {
                    if (k == lo)
                    {
                        continue;
                    }
                    for (int i = 0; i < Dim; ++i)
                    {
                        s[k][i] = s[lo][i] + Shrink * (s[k][i] - s[lo][i]);
                    }
                    fval[k] = objective(s[k]);
                }
            }

            apply(s[lo]);
        }
    }

    LtcLut GenerateLtcLut()
    {
        constexpr int N = static_cast<int>(LtcLut::Size);

        LtcLut lut;
        lut.Matrix.assign(static_cast<usize>(N) * N, vec4(0.0f));
        lut.Magnitude.assign(static_cast<usize>(N) * N, vec4(0.0f));

        // Per-column fitted M matrices, so the next roughness row can warm-start from the last.
        std::vector<mat3> tab(static_cast<usize>(N) * N, mat3(1.0f));

        Ltc ltc;
        for (int a = N - 1; a >= 0; --a)
        {
            for (int t = 0; t <= N - 1; ++t)
            {
                const f32 x = static_cast<f32>(t) / static_cast<f32>(N - 1);
                const f32 ct = 1.0f - x * x;
                const f32 theta = std::min(1.57f, std::acos(ct));
                const vec3 V(std::sin(theta), 0.0f, std::cos(theta));

                const f32 roughness = static_cast<f32>(a) / static_cast<f32>(N - 1);
                const f32 alpha = std::max(roughness * roughness, MinAlpha);

                vec3 averageDir;
                ComputeAvgTerms(V, alpha, ltc.Magnitude, ltc.Fresnel, averageDir);

                bool isotropic = false;
                if (t == 0)
                {
                    ltc.X = vec3(1.0f, 0.0f, 0.0f);
                    ltc.Y = vec3(0.0f, 1.0f, 0.0f);
                    ltc.Z = vec3(0.0f, 0.0f, 1.0f);
                    if (a == N - 1)
                    {
                        ltc.M11 = 1.0f;
                        ltc.M22 = 1.0f;
                    }
                    else
                    {
                        const mat3& prev =
                            tab[static_cast<usize>(a + 1) + static_cast<usize>(t) * N];
                        ltc.M11 = prev[0][0];
                        ltc.M22 = prev[1][1];
                    }
                    ltc.M13 = 0.0f;
                    ltc.Update();
                    isotropic = true;
                }
                else
                {
                    const vec3 L = averageDir;
                    ltc.X = vec3(L.z, 0.0f, -L.x);
                    ltc.Y = vec3(0.0f, 1.0f, 0.0f);
                    ltc.Z = L;
                    ltc.Update();
                    isotropic = false;
                }

                Fit(ltc, V, alpha, isotropic);

                const usize index = static_cast<usize>(a) + static_cast<usize>(t) * N;
                tab[index] = ltc.M;

                // Pack the inverse transform (normalized so InvM[1][1] = 1) and the norm/Fresnel.
                mat3 invM = glm::inverse(ltc.M);
                invM /= invM[1][1];
                lut.Matrix[index] = vec4(invM[0][0], invM[0][2], invM[2][0], invM[2][2]);
                lut.Magnitude[index] = vec4(ltc.Magnitude, ltc.Fresnel, 0.0f, 0.0f);
            }
        }

        return lut;
    }
}
