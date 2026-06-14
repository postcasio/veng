#pragma once

// Veng.h — the foundational header every other veng header builds on. It pulls
// in the standard-library and glm pieces the engine leans on and defines the
// house-style aliases (Veng::string, vector<T>, Ref<T>, u32, vec3, ...).
//
// House-style aliases: part of veng's public identity, written throughout the
// public API and sample app. They live here, in one self-contained header, so
// the vocabulary is defined in exactly one place.
//
// Threading contract: the render thread is single. Context::BeginFrame /
// EndFrame, draw recording, ImGui, input, and Time are driven from the one
// thread that owns the Context — never call those concurrently. Work runs off
// the main thread only through the TaskSystem: a job may decode and upload a
// resource on a worker (the transfer queue), and its result lands back on the
// main thread through the continuation pump. Direct concurrent calls into veng
// APIs from outside the task system are illegal.

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

// Shared-library export annotation. On macOS/Linux veng is built with default
// symbol visibility, so this is only meaningful for a future Windows port.
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

namespace Veng
{
    using path = std::filesystem::path;
    using string = std::string;
    using string_view = std::string_view;
    template <typename T>
    using vector = std::vector<T>;
    template <typename T>
    using set = std::set<T>;
    template <typename K, typename V>
    using map = std::map<K, V>;
    template <typename K, typename V>
    using unordered_map = std::unordered_map<K, V>;
    template <typename T>
    using optional = std::optional<T>;
    template <typename Sig>
    using function = std::function<Sig>;

    template <class T, class U>
    concept Derived = std::is_base_of_v<U, T>;

    template <typename T>
    using Unique = std::unique_ptr<T>;

    template <typename T, typename... Args>
    constexpr Unique<T> CreateUnique(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    // Renderer::X::Create() return-type rule: a shared GPU resource that other
    // objects hold references to (buffers, images, views, samplers, shaders,
    // pipelines, descriptor sets/layouts, pipeline layouts, command buffers)
    // returns Ref<X> — multiple owners are normal (e.g. a pipeline holds
    // Ref<ShaderModule>, a descriptor set holds Ref<ImageView>). A single-owner
    // synchronization primitive (Fence, Semaphore) returns Unique<X> — nothing
    // else ever holds a reference to one.
    template <typename T>
    using Ref = std::shared_ptr<T>;

    template <typename T, typename... Args>
    constexpr Ref<T> CreateRef(Args&&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    template <typename T>
    using WeakRef = std::weak_ptr<T>;

    template <typename T>
    constexpr WeakRef<T> CreateWeakRef(const Ref<T>& ref)
    {
        return std::weak_ptr<T>(ref);
    }


    using i8 = int8_t;
    using u8 = uint8_t;
    using i16 = int16_t;
    using u16 = uint16_t;
    using i32 = int32_t;
    using u32 = uint32_t;
    using i64 = int64_t;
    using u64 = uint64_t;
    using f32 = float;
    using f64 = double;

    using vec2 = glm::vec2;
    using vec3 = glm::vec3;
    using vec4 = glm::vec4;
    using mat2 = glm::mat2;
    using mat3 = glm::mat3;
    using mat4 = glm::mat4;
    using quat = glm::quat;
    using ivec2 = glm::ivec2;
    using ivec3 = glm::ivec3;
    using ivec4 = glm::ivec4;
    using uvec2 = glm::uvec2;
    using uvec3 = glm::uvec3;
    using uvec4 = glm::uvec4;

    using usize = std::size_t;
    using isize = std::ptrdiff_t;
}
