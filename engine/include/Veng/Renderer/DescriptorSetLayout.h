#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class Sampler;

    /// @brief Describes a single binding within a descriptor set layout.
    struct DescriptorBinding
    {
        /// @brief Binding number (sparse-safe — gaps are allowed).
        u32 Binding = 0;
        /// @brief Descriptor type (uniform buffer, sampled image, etc.).
        DescriptorType Type{};
        /// @brief Descriptor array size.
        u32 Count = 1;
        /// @brief Shader stages that can access this binding.
        ShaderStage Stages{};

        /// @brief Immutable samplers baked into the layout for this binding (one per descriptor in Count).
        ///
        /// Set for a SampledImage binding sampled through a fixed sampler the layout owns —
        /// e.g. a comparison sampler for hardware SampleCmp — so a write supplies only the
        /// image, never a sampler. Empty for the common case.
        vector<Ref<Sampler>> ImmutableSamplers{};

        /// @brief When false (default), the binding is static: written at setup or between frames,
        /// then bound — no descriptor-indexing flags. When true, the binding is bindless: may
        /// be updated while a set is bound (e.g. WriteArray into a streaming table) — opts into
        /// UpdateAfterBind/partiallyBound. The engine derives the Vulkan flags from this intent.
        bool Bindless = false;
    };

    /// @brief Creation parameters for a descriptor set layout.
    struct DescriptorSetLayoutInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief The binding declarations.
        vector<DescriptorBinding> Bindings;
    };

    /// @brief A Vulkan descriptor set layout describing the shape of a descriptor set.
    class DescriptorSetLayout
    {
    public:
        /// @brief Creates a descriptor set layout from the given parameters.
        /// @param context The owning context.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new layout.
        static Ref<DescriptorSetLayout> Create(Context& context, const DescriptorSetLayoutInfo& info)
        {
            return Ref<DescriptorSetLayout>(new DescriptorSetLayout(context, info));
        }

        /// @brief Destroys the Vulkan descriptor set layout.
        ~DescriptorSetLayout();

        DescriptorSetLayout(const DescriptorSetLayout&) = delete;
        DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

        /// @brief Returns the layout's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns all binding declarations in declaration order.
        [[nodiscard]] const vector<DescriptorBinding>& GetBindings() const { return m_Bindings; }

        /// @brief Returns the descriptor type of the given binding number.
        ///
        /// Sparse-safe — works for layouts like 0, 2, 5. Fatal if the binding does not exist.
        /// @param binding The binding number to look up.
        [[nodiscard]] DescriptorType GetBindingType(u32 binding) const;

        /// @brief Returns the descriptor array size (count) of the given binding number.
        ///
        /// Fatal if the binding does not exist.
        /// @param binding The binding number to look up.
        [[nodiscard]] u32 GetBindingCount(u32 binding) const;

        /// @brief Opaque backend handle; defined in DescriptorSetLayout.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        DescriptorSetLayout(Context& context, const DescriptorSetLayoutInfo& info);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        string m_Name;
        vector<DescriptorBinding> m_Bindings;
        /// @brief Binding lookup by number (sparse-safe).
        map<u32, DescriptorBinding> m_BindingsByNumber;
        Unique<Native> m_Native;
    };
}
