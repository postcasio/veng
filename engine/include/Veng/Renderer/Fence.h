#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief A single-owner GPU fence for CPU–GPU synchronization.
    class Fence
    {
    public:
        /// @brief Creates a fence.
        ///
        /// Returns Unique, not Ref: a fence is a single-owner synchronization primitive —
        /// nothing else holds a reference.
        /// @param context  The owning context.
        /// @param name     Debug name.
        /// @param signaled Initial signaled state (default false).
        /// @return A unique-ownership fence.
        static Unique<Fence> Create(Context& context, const string& name, bool signaled = false)
        {
            return Unique<Fence>(new Fence(context, name, signaled));
        }

        /// @brief Destroys the Vulkan fence immediately (fences are single-owner and frame-synchronized).
        ~Fence();

        Fence(const Fence&) = delete;
        Fence& operator=(const Fence&) = delete;

        /// @brief Blocks until the fence is signaled.
        void Wait() const;

        /// @brief Resets the fence to the unsignaled state.
        void Reset() const;

        /// @brief Backend handle accessor. Returns a mutable ref from a const method by design —
        /// see the Native idiom in Native.h.
        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Fence(Context& context, const string& name, bool signaled);

        /// @brief The owning context; used for deferred destruction.
        Context& m_Context;
        /// @brief Debug name.
        string m_Name;
        /// @brief Backend Vulkan fence.
        Unique<Native> m_Native;
    };
}
