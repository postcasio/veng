#include <Veng/Net/AccountId.h>

#include <random>

namespace Veng::Net
{
    AccountId GenerateAccountId()
    {
        // Seeded from the OS entropy source per call; the id must be valid, so the degenerate
        // all-zero draw re-rolls.
        static std::mt19937_64 generator = []
        {
            std::random_device device;
            const u64 seed = (static_cast<u64>(device()) << 32) ^ static_cast<u64>(device());
            return std::mt19937_64(seed);
        }();

        AccountId id;
        do
        {
            id.Lo = generator();
            id.Hi = generator();
        } while (!id.IsValid());
        return id;
    }
}
