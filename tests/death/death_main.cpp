// Death-test harness (planset-3, plan 01).
//
// VE_ASSERT calls std::abort(), which doctest cannot trap in-process, so death
// cases run as separate processes: ctest invokes this binary with a case name
// as argv[1], the case performs exactly the offending operation, and the
// process is expected to abort with a matching assert message.
//
// Registration lives in CMakeLists.txt — one add_test per case with
// PASS_REGULAR_EXPRESSION pinned to that case's assert message. A case passes
// iff the expected message reaches stderr: a case that aborts for the *wrong*
// reason, or fails to abort at all (a clean exit, or an unknown/missing case
// name), never matches and so fails loudly.
//
// Why a SIGABRT handler instead of WILL_FAIL: VE_ASSERT calls std::abort(),
// which terminates the process by *signal*. CTest reports a signal death as
// "Subprocess aborted" — a failure that neither WILL_FAIL nor a matching
// PASS_REGULAR_EXPRESSION overrides (verified against this CMake). So we trap
// SIGABRT and convert the death into a controlled clean exit *after* the assert
// message is already on stderr; the message match is then what decides pass/
// fail. (This contradicts plan 01's assumption that WILL_FAIL inverts a SIGABRT
// — noted there.)
//
// The actual death cases beyond `sentinel` are plan 05; this file owns only the
// dispatch + the one sentinel case that proves the wiring.

#include <csignal>
#include <cstdlib>
#include <string_view>

#include <Veng/Assert.h>
#include <Veng/Log.h>

#include <fmt/format.h>

using namespace Veng;

namespace
{
    // SIGABRT lands here once VE_ASSERT has already logged + flushed its message
    // to stderr. Convert the abort into a clean exit so CTest judges the run by
    // PASS_REGULAR_EXPRESSION (the assert message) rather than reporting an
    // un-overridable "Subprocess aborted". std::_Exit is async-signal-safe and
    // skips atexit/global-dtor teardown, which is what we want after an abort.
    [[noreturn]] void OnAbort(int)
    {
        std::_Exit(0);
    }

    // Route assert output to unbuffered stderr so the message survives abort()
    // (std::abort does not flush buffered stdio) and ctest can match it. The
    // default log sink writes to (buffered) stdout, which abort() would discard.
    void InstallStderrSink()
    {
        Log::SetSink([](Log::Level, std::string_view message)
        {
            fmt::print(stderr, "{}\n", message);
        });
    }

    void RunSentinel()
    {
        VE_ASSERT(false, "sentinel death case");
    }
}

int main(int argc, char** argv)
{
    InstallStderrSink();
    std::signal(SIGABRT, OnAbort);

    if (argc < 2)
    {
        fmt::print(stderr, "death harness: no case name given\n");
        return 1;
    }

    const std::string_view name = argv[1];

    if (name == "sentinel")
        RunSentinel();
    else
        fmt::print(stderr, "death harness: unknown case '{}'\n", name);

    // Reached only if the case did not abort — exit non-zero, and (with no
    // assert message printed) the PASS_REGULAR_EXPRESSION misses: the test fails.
    return 1;
}
