#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief A binary GPU semaphore for present/acquire synchronization.
    ///
    /// Distinct from TimelineSemaphore: a binary semaphore has no counter and is used
    /// exclusively for swapchain present/acquire sync. Single-owner (Unique) — nothing
    /// else holds a reference.
    class Semaphore
    {
    public:
        /// @brief Creates a binary semaphore.
        /// @param context The owning context.
        /// @param name    Debug name.
        /// @return A unique owner of the new semaphore.
        static Unique<Semaphore> Create(Context& context, const string& name)
        {
            return Unique<Semaphore>(new Semaphore(context, name));
        }

        /// @brief Destroys the underlying Vulkan semaphore immediately.
        ///
        /// Binary semaphores are per-synchronization-frame and are always destroyed after
        /// the frame fence has been waited, so no deferred destruction is needed.
        ~Semaphore();

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        /// @brief Opaque backend handle; defined in the matching .cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        Semaphore(Context& context, const string& name);

        /// @brief Context this semaphore was created with; must outlive it.
        Context& m_Context;
        /// @brief Debug name.
        string m_Name;
        /// @brief Backend Vulkan semaphore.
        Unique<Native> m_Native;
    };
}
