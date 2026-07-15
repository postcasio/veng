#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Gui/Driver.h>

namespace Veng
{
    /// @brief A registered driver's catalog entry: its identity and how to build it.
    ///
    /// Holds the GuiDriverId and display name read off the driver's VengGuiDriver trait at
    /// registration, so the catalog enumerates the available drivers without instantiating any.
    struct GuiDriverEntry
    {
        /// @brief The driver's stable identity, the catalog key.
        GuiDriverId Id = GuiDriverId::Null;
        /// @brief The driver's display name (logs/editor display).
        string Name;
        /// @brief Default-constructs one instance of the driver.
        function<Unique<GuiDriver>()> Factory;
    };

    /// @brief The catalog of GuiDrivers a module registers during VengModuleRegister.
    ///
    /// Host-owned and borrowed, mirroring the SystemRegistry: the host (launcher or editor)
    /// constructs it, threads it through VengModuleHost, and the module registers its drivers into
    /// it. Each Register<T>() reads the driver's GuiDriverId + name off its VengGuiDriver trait and
    /// stores them beside the factory, so a consumer enumerates the available drivers and resolves an
    /// id to a factory without instantiating anything. GuiOverlay::Drive resolves an overlay's Driver
    /// id against it. Registration is GPU-free — constructing a driver touches no Context/device,
    /// preserving the headless/cooker contract.
    class VE_API GuiDriverRegistry
    {
    public:
        /// @brief Registers driver type T into the catalog under its authored GuiDriverId.
        ///
        /// Reads GuiDriverIdOf<T>() and the display name off T's VengGuiDriver trait (so a driver
        /// without a VE_GUI_DRIVER fails to compile here), and stores `{ GuiDriverId, Name, factory }`.
        /// @tparam T The concrete GuiDriver subclass to register.
        /// @pre No other driver already claims T's GuiDriverId.
        /// @warning Registering two drivers under the same GuiDriverId is a fatal collision assert.
        template <class T>
        void Register()
        {
            constexpr GuiDriverId id = GuiDriverIdOf<T>();
            static_assert(id != GuiDriverId::Null,
                          "VengGuiDriver<T>::Id must be a non-zero authored id");

            for (const GuiDriverEntry& entry : m_Entries)
            {
                VE_ASSERT(entry.Id != id,
                          "GuiDriverId collision: '{}' and '{}' both claim GuiDriverId {:#018x}",
                          GuiDriverNameOf<T>(), entry.Name, static_cast<u64>(id));
            }

            m_Entries.emplace_back(GuiDriverEntry{
                .Id = id,
                .Name = GuiDriverNameOf<T>(),
                .Factory = [] { return Unique<GuiDriver>(new T()); },
            });
        }

        /// @brief Read-only view over the catalog entries, in registration order.
        /// @return The registered entries, in registration order.
        [[nodiscard]] const vector<GuiDriverEntry>& Entries() const { return m_Entries; }

        /// @brief Resolves a GuiDriverId to the driver it builds.
        /// @param id  The GuiDriverId to resolve.
        /// @return A freshly built driver, or nullptr if no entry claims the id.
        [[nodiscard]] Unique<GuiDriver> Instantiate(GuiDriverId id) const;

        /// @brief Returns the number of registered drivers.
        [[nodiscard]] usize Count() const;

    private:
        /// @brief The registered catalog entries, in registration order.
        vector<GuiDriverEntry> m_Entries;
    };
}
