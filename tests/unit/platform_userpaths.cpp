// UserDataDir/UserConfigDir/UserCacheDir: the per-OS writable-directory resolvers.
// Each redirects the platform base env var(s) to a scratch tree under
// temp_directory_path(), so the suite exercises the real resolver logic without
// touching the machine's actual home/config/cache. Covers locate-and-ensure
// (the directory exists after the call), the Linux XDG-vs-fallback split, and
// the unwritable-base recoverable-error path.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

#include <Veng/Platform/UserPaths.h>

using namespace Veng;

namespace
{
    /// @brief RAII scoped override of an environment variable, restoring the prior
    /// value (or absence) on destruction — keeps each test from leaking state into
    /// the next.
    class ScopedEnv
    {
    public:
        /// @brief Sets @p name to @p value for the scope's lifetime.
        ScopedEnv(const char* name, const string& value) : m_Name(name)
        {
            CapturePrior();
#if defined(_WIN32)
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        }

        /// @brief Removes @p name from the environment for the scope's lifetime.
        explicit ScopedEnv(const char* name) : m_Name(name)
        {
            CapturePrior();
#if defined(_WIN32)
            _putenv_s(name, "");
#else
            unsetenv(name);
#endif
        }

        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;

        ~ScopedEnv()
        {
#if defined(_WIN32)
            _putenv_s(m_Name, m_Prior.has_value() ? m_Prior->c_str() : "");
#else
            if (m_Prior.has_value())
            {
                setenv(m_Name, m_Prior->c_str(), 1);
            }
            else
            {
                unsetenv(m_Name);
            }
#endif
        }

    private:
        void CapturePrior()
        {
            const char* prior = std::getenv(m_Name);
            if (prior != nullptr)
            {
                m_Prior = string(prior);
            }
        }

        const char* m_Name;
        optional<string> m_Prior;
    };

    /// @brief A fresh scratch directory under the system temp tree, removed on scope exit.
    class ScratchDir
    {
    public:
        explicit ScratchDir(const string& label)
            : m_Path(std::filesystem::temp_directory_path() / ("veng_userpaths_" + label))
        {
            std::filesystem::remove_all(m_Path);
            std::filesystem::create_directories(m_Path);
        }

        ~ScratchDir() { std::filesystem::remove_all(m_Path); }

        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;

        [[nodiscard]] const path& Get() const { return m_Path; }

    private:
        path m_Path;
    };
}

TEST_CASE(
    "platform_userpaths: UserDataDir resolves and creates a directory under a redirected home")
{
    const ScratchDir scratch("data_home");
#if defined(_WIN32)
    const ScopedEnv appData("APPDATA", scratch.Get().string());
#elif defined(__APPLE__)
    const ScopedEnv home("HOME", scratch.Get().string());
#else
    const ScopedEnv xdgData("XDG_DATA_HOME", scratch.Get().string());
#endif

    const Result<path> result = UserDataDir("veng-user-paths-test");
    REQUIRE(result.has_value());
    CHECK(std::filesystem::is_directory(*result));
#if defined(__APPLE__) && !defined(_WIN32)
    CHECK(result->string().find("Library/Application Support") != string::npos);
#endif
}

TEST_CASE(
    "platform_userpaths: UserConfigDir resolves and creates a directory under a redirected home")
{
    const ScratchDir scratch("config_home");
#if defined(_WIN32)
    const ScopedEnv appData("APPDATA", scratch.Get().string());
#elif defined(__APPLE__)
    const ScopedEnv home("HOME", scratch.Get().string());
#else
    const ScopedEnv xdgConfig("XDG_CONFIG_HOME", scratch.Get().string());
#endif

    const Result<path> result = UserConfigDir("veng-user-paths-test");
    REQUIRE(result.has_value());
    CHECK(std::filesystem::is_directory(*result));
}

TEST_CASE(
    "platform_userpaths: UserCacheDir resolves and creates a directory under a redirected home")
{
    const ScratchDir scratch("cache_home");
#if defined(_WIN32)
    const ScopedEnv appData("APPDATA", scratch.Get().string());
#elif defined(__APPLE__)
    const ScopedEnv home("HOME", scratch.Get().string());
#else
    const ScopedEnv xdgCache("XDG_CACHE_HOME", scratch.Get().string());
#endif

    const Result<path> result = UserCacheDir("veng-user-paths-test");
    REQUIRE(result.has_value());
    CHECK(std::filesystem::is_directory(*result));
}

TEST_CASE("platform_userpaths: the application segment is appended to the resolved base")
{
    const ScratchDir scratch("segment");
#if defined(_WIN32)
    const ScopedEnv appData("APPDATA", scratch.Get().string());
#elif defined(__APPLE__)
    const ScopedEnv home("HOME", scratch.Get().string());
#else
    const ScopedEnv xdgData("XDG_DATA_HOME", scratch.Get().string());
#endif

    const Result<path> result = UserDataDir("my-app-segment");
    REQUIRE(result.has_value());
    CHECK(result->filename().string() == "my-app-segment");
}

#if !defined(_WIN32)

TEST_CASE("platform_userpaths: Linux UserDataDir falls back to ~/.local/share when "
          "XDG_DATA_HOME is unset")
{
#if !defined(__APPLE__)
    const ScratchDir scratch("fallback_home");
    const ScopedEnv home("HOME", scratch.Get().string());
    const ScopedEnv clearedXdg("XDG_DATA_HOME");

    const Result<path> result = UserDataDir("veng-fallback-test");
    REQUIRE(result.has_value());
    CHECK(result->string().find((scratch.Get() / ".local" / "share").string()) == 0);
#endif
}

TEST_CASE("platform_userpaths: an unwritable base yields a recoverable Result error, not a crash")
{
    const ScratchDir scratch("unwritable");
    // Strip write permission from the scratch dir itself so create_directories()
    // fails trying to create anything inside it — the recoverable-failure arm of
    // the locate-and-ensure contract. HOME points straight at the read-only
    // scratch dir on every non-Windows platform, so the base resolves but the
    // ensure-directory step fails regardless of the per-OS subpath beneath it.
    std::filesystem::permissions(scratch.Get(), std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::remove);

#if defined(__APPLE__)
    const ScopedEnv home("HOME", scratch.Get().string());
#else
    // UserDataDir appends the application segment directly onto XDG_DATA_HOME (no
    // per-OS subpath to traverse first), so pointing it straight at the read-only
    // scratch dir is what makes create_directories() fail inside it.
    const ScopedEnv xdgData("XDG_DATA_HOME", scratch.Get().string());
#endif

    const Result<path> result = UserDataDir("veng-unwritable-test");
    CHECK_FALSE(result.has_value());

    // Restore write permission so the ScratchDir destructor can clean up.
    std::filesystem::permissions(scratch.Get(), std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add);
}

#endif

TEST_CASE("platform_userpaths: a resolved base with no HOME/APPDATA set yields a recoverable error")
{
#if defined(_WIN32)
    const ScopedEnv appData("APPDATA");
#else
    const ScopedEnv home("HOME");
#if !defined(__APPLE__)
    const ScopedEnv xdgData("XDG_DATA_HOME");
#endif
#endif

    const Result<path> result = UserDataDir("veng-no-home-test");
    CHECK_FALSE(result.has_value());
}
