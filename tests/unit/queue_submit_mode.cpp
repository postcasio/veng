// Queue-submit mode precedence: pure CPU, no Context. A user's environment variable decides over
// the context's request and is never overridden by a setting the context passes; the request
// decides where the driver can be told; a driver that cannot be told runs its synchronous default.

#include <doctest/doctest.h>

#include <Veng/Renderer/QueueSubmitMode.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("queue submit mode: the request applies where the driver can be told")
{
    const ResolvedQueueSubmitMode byDefault =
        ResolveQueueSubmitMode(QueueSubmitMode::Asynchronous, nullptr, true);
    CHECK(byDefault.Mode == QueueSubmitMode::Asynchronous);
    CHECK(byDefault.Source == QueueSubmitModeSource::Requested);
    CHECK(byDefault.ApplySetting);

    // Forcing synchronous wins over the asynchronous default.
    const ResolvedQueueSubmitMode forced =
        ResolveQueueSubmitMode(QueueSubmitMode::Synchronous, nullptr, true);
    CHECK(forced.Mode == QueueSubmitMode::Synchronous);
    CHECK(forced.Source == QueueSubmitModeSource::Requested);
    CHECK(forced.ApplySetting);
}

TEST_CASE("queue submit mode: the environment wins and is left for the driver to read")
{
    // Either value overrides either request, and no setting is passed that would override it.
    const ResolvedQueueSubmitMode synchronous =
        ResolveQueueSubmitMode(QueueSubmitMode::Asynchronous, "1", true);
    CHECK(synchronous.Mode == QueueSubmitMode::Synchronous);
    CHECK(synchronous.Source == QueueSubmitModeSource::Environment);
    CHECK_FALSE(synchronous.ApplySetting);

    const ResolvedQueueSubmitMode asynchronous =
        ResolveQueueSubmitMode(QueueSubmitMode::Synchronous, "0", true);
    CHECK(asynchronous.Mode == QueueSubmitMode::Asynchronous);
    CHECK(asynchronous.Source == QueueSubmitModeSource::Environment);
    CHECK_FALSE(asynchronous.ApplySetting);

    // Read as the driver reads it: a number, non-zero meaning synchronous, anything else zero.
    CHECK(ResolveQueueSubmitMode(QueueSubmitMode::Asynchronous, "2", true).Mode ==
          QueueSubmitMode::Synchronous);
    CHECK(ResolveQueueSubmitMode(QueueSubmitMode::Synchronous, "false", true).Mode ==
          QueueSubmitMode::Asynchronous);

    // The environment is the driver's own input, so it decides even where no setting can be passed.
    CHECK(ResolveQueueSubmitMode(QueueSubmitMode::Synchronous, "0", false).Mode ==
          QueueSubmitMode::Asynchronous);
}

TEST_CASE("queue submit mode: a driver that cannot be told runs its synchronous default")
{
    const ResolvedQueueSubmitMode resolved =
        ResolveQueueSubmitMode(QueueSubmitMode::Asynchronous, nullptr, false);
    CHECK(resolved.Mode == QueueSubmitMode::Synchronous);
    CHECK(resolved.Source == QueueSubmitModeSource::DriverDefault);
    CHECK_FALSE(resolved.ApplySetting);
}

TEST_CASE("queue submit mode: names")
{
    CHECK(QueueSubmitModeName(QueueSubmitMode::Asynchronous) == "asynchronous");
    CHECK(QueueSubmitModeName(QueueSubmitMode::Synchronous) == "synchronous");
    CHECK(QueueSubmitModeSourceName(QueueSubmitModeSource::Environment) == "environment");
}
