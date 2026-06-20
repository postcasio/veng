#pragma once

// Veng.h — the foundational header every other veng header builds on.
//
// Pulls in the standard-library and glm pieces the engine leans on and defines
// the house-style aliases (Veng::string, vector<T>, Ref<T>, u32, vec3, ...).
// These aliases are part of veng's public identity and are written throughout
// the public API. They live here so the vocabulary is defined in exactly one place.
//
// Threading contract: the render thread is single. Context::BeginFrame/EndFrame,
// draw recording, ImGui, input, and Time are driven from the one thread that owns
// the Context. Work runs off the main thread only through TaskSystem (transfer
// queue decode+upload, result delivered via the continuation pump). Direct
// concurrent calls into veng APIs from outside the task system are illegal.

#include <filesystem>
#include <map>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <optional>
#include <memory>
#include <cstdint>
#include <type_traits>
#include <fmt/format.h>
#include <functional>


#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RIGHT_HANDED
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>

/// @brief Shared-library export/import annotation for public veng symbols.
///
/// On macOS/Linux veng uses default symbol visibility, so this only matters
/// on Windows (dllexport when building the library, dllimport when consuming it).
#if defined(_WIN32)
  #if defined(VE_BUILD_SHARED)
    #define VE_API __declspec(dllexport)
  #elif defined(VE_USE_SHARED)
    #define VE_API __declspec(dllimport)
  #else
    #define VE_API
  #endif
#else
  #define VE_API __attribute__((visibility("default")))
#endif

/// @brief Export annotation for a module's C-ABI entry point.
///
/// A module always defines and exports VengModuleRegister, so this must be
/// unconditionally dllexport on Windows. VE_API cannot be reused: a consumer
/// build defines it as dllimport, which would produce a link error on the symbol
/// the module itself defines.
#if defined(_WIN32)
  #define VE_MODULE_EXPORT __declspec(dllexport)
#else
  #define VE_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace Veng
{
    /// @brief House alias for std::filesystem::path.
    using path = std::filesystem::path;
    /// @brief House alias for std::string.
    using string = std::string;
    /// @brief House alias for std::string_view.
    using string_view = std::string_view;
    /// @brief House alias for std::vector<T>.
    template <typename T>
    using vector = std::vector<T>;
    /// @brief House alias for std::set<T>.
    template <typename T>
    using set = std::set<T>;
    /// @brief House alias for std::map<K,V>.
    template <typename K, typename V>
    using map = std::map<K, V>;
    /// @brief House alias for std::unordered_map<K,V>.
    template <typename K, typename V>
    using unordered_map = std::unordered_map<K, V>;
    /// @brief House alias for std::optional<T>.
    template <typename T>
    using optional = std::optional<T>;
    /// @brief House alias for std::function<Sig>.
    template <typename Sig>
    using function = std::function<Sig>;

    /// @brief Concept satisfied when T is derived from U.
    template <class T, class U>
    concept Derived = std::is_base_of_v<U, T>;

    /// @brief Single-owner smart pointer; use for primitives nothing else references.
    template <typename T>
    using Unique = std::unique_ptr<T>;

    /// @brief Constructs a Unique<T> via std::make_unique.
    template <typename T, typename... Args>
    constexpr Unique<T> CreateUnique(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    /// @brief Shared-ownership smart pointer for GPU resources that multiple objects reference.
    ///
    /// X::Create() returns Ref<X> for shared GPU resources (buffers, images, views,
    /// samplers, shaders, pipelines, descriptor sets/layouts). Single-owner primitives
    /// (Fence, Semaphore) return Unique<X> instead.
    template <typename T>
    using Ref = std::shared_ptr<T>;

    /// @brief Constructs a Ref<T> via std::make_shared.
    template <typename T, typename... Args>
    constexpr Ref<T> CreateRef(Args&&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    /// @brief Non-owning reference to a Ref<T>; does not extend lifetime.
    template <typename T>
    using WeakRef = std::weak_ptr<T>;

    /// @brief Creates a WeakRef<T> from a Ref<T>.
    template <typename T>
    constexpr WeakRef<T> CreateWeakRef(const Ref<T>& ref)
    {
        return std::weak_ptr<T>(ref);
    }

    /// @brief Signed 8-bit integer.
    using i8 = int8_t;
    /// @brief Unsigned 8-bit integer.
    using u8 = uint8_t;
    /// @brief Signed 16-bit integer.
    using i16 = int16_t;
    /// @brief Unsigned 16-bit integer.
    using u16 = uint16_t;
    /// @brief Signed 32-bit integer.
    using i32 = int32_t;
    /// @brief Unsigned 32-bit integer.
    using u32 = uint32_t;
    /// @brief Signed 64-bit integer.
    using i64 = int64_t;
    /// @brief Unsigned 64-bit integer.
    using u64 = uint64_t;
    /// @brief 32-bit floating point.
    using f32 = float;
    /// @brief 64-bit floating point.
    using f64 = double;

    /// @brief 2-component float vector.
    using vec2 = glm::vec2;
    /// @brief 3-component float vector.
    using vec3 = glm::vec3;
    /// @brief 4-component float vector.
    using vec4 = glm::vec4;
    /// @brief 2x2 float matrix.
    using mat2 = glm::mat2;
    /// @brief 3x3 float matrix.
    using mat3 = glm::mat3;
    /// @brief 4x4 float matrix.
    using mat4 = glm::mat4;
    /// @brief Quaternion.
    using quat = glm::quat;
    /// @brief 2-component signed integer vector.
    using ivec2 = glm::ivec2;
    /// @brief 3-component signed integer vector.
    using ivec3 = glm::ivec3;
    /// @brief 4-component signed integer vector.
    using ivec4 = glm::ivec4;
    /// @brief 2-component unsigned integer vector.
    using uvec2 = glm::uvec2;
    /// @brief 3-component unsigned integer vector.
    using uvec3 = glm::uvec3;
    /// @brief 4-component unsigned integer vector.
    using uvec4 = glm::uvec4;

    /// @brief Unsigned size type.
    using usize = std::size_t;
    /// @brief Signed difference type.
    using isize = std::ptrdiff_t;
}
