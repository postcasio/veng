#pragma once
#include <span>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>

namespace Veng::Renderer
{
    class Context;
    template <typename T>
    class UniformBuffer;
    template <typename T>
    class StorageBuffer;

    /// @brief Creation parameters for a descriptor set.
    struct DescriptorSetInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief Layout describing the set's bindings.
        Ref<DescriptorSetLayout> Layout;
    };

    /// @brief A Vulkan descriptor set with typed write helpers and per-binding resource ownership.
    class DescriptorSet
    {
    public:
        /// @brief Creates a descriptor set from the given layout.
        /// @param context The owning context.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new set.
        static Ref<DescriptorSet> Create(Context& context, const DescriptorSetInfo& info)
        {
            return Ref<DescriptorSet>(new DescriptorSet(context, info));
        }

        /// @brief Defers destruction of the underlying Vulkan descriptor set until the GPU is done with it.
        ~DescriptorSet();

        DescriptorSet(const DescriptorSet&) = delete;
        DescriptorSet& operator=(const DescriptorSet&) = delete;

        /// @brief Writes a combined image sampler into the given binding.
        ///
        /// The descriptor type is inferred from the layout — the caller never restates it.
        /// A payload kind that doesn't match the binding's type, or a binding the layout
        /// doesn't have, is a fatal assert. Each write updates the set immediately and
        /// retains the written resources so a bound view or buffer cannot dangle while the
        /// set is still bound in a future frame.
        /// @param binding The binding number to write.
        /// @param view    The image view to bind.
        /// @param sampler The sampler to bind alongside the view.
        void Write(u32 binding, const Ref<ImageView>& view, const Ref<Sampler>& sampler);

        /// @brief Writes a sampled image or storage image into the given binding.
        ///
        /// The descriptor type (sampled vs. storage) is disambiguated by the layout;
        /// the image layout follows from the type.
        /// @param binding The binding number to write.
        /// @param view    The image view to bind.
        void Write(u32 binding, const Ref<ImageView>& view);

        /// @brief Writes a plain sampler (no image) into the given binding.
        ///
        /// Not needed for an immutable-sampler binding (the layout owns the sampler).
        /// @param binding The binding number to write.
        /// @param sampler The sampler to bind.
        void Write(u32 binding, const Ref<Sampler>& sampler);

        /// @brief Writes a uniform or storage buffer (whole range) into the given binding.
        /// @param binding The binding number to write.
        /// @param buffer  The buffer to bind.
        void Write(u32 binding, const Ref<Buffer>& buffer);

        /// @brief Writes a uniform or storage buffer with an explicit byte range.
        /// @param binding The binding number to write.
        /// @param buffer  The buffer to bind.
        /// @param offset  Byte offset into the buffer.
        /// @param range   Byte range to expose.
        void Write(u32 binding, const Ref<Buffer>& buffer, u64 offset, u64 range);

        /// @brief Writes a typed uniform buffer. Forwards to the Ref<Buffer> writer.
        template <typename T>
        void Write(u32 binding, const UniformBuffer<T>& buffer);

        /// @brief Writes a typed storage buffer. Forwards to the Ref<Buffer> writer.
        template <typename T>
        void Write(u32 binding, const StorageBuffer<T>& buffer);

        /// @brief Writes an array of combined image samplers (bindless-style).
        /// @param binding      The binding number to write.
        /// @param views        The image views to write.
        /// @param sampler      The sampler to pair with every view.
        /// @param firstElement Starting array element (default 0).
        void WriteArray(u32 binding, std::span<const Ref<ImageView>> views,
                        const Ref<Sampler>& sampler, u32 firstElement = 0);

        /// @brief Returns the set's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Opaque backend handle; defined in DescriptorSet.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        DescriptorSet(Context& context, const DescriptorSetInfo& info);

        /// @brief The owning context; used for deferred destruction. A resource must not outlive its context.
        Context& m_Context;
        string m_Name;
        Unique<Native> m_Native;

        /// @brief Resources currently written into each binding, keyed by binding number.
        ///
        /// Re-writing a binding releases exactly what it replaced, preventing dangling descriptors.
        /// Distinct from the per-frame deferred-destruction queue (Context::AcquireNextFrame).
        map<u32, vector<Ref<void>>> m_BoundPerBinding;
        Ref<DescriptorSetLayout> m_Layout;
    };
}
