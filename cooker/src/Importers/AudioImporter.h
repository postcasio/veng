#pragma once

#include <Veng/Cook/Importer.h>

namespace Veng::Cook
{
    /// @brief Cooks a *.audio.json source into a CookedAudioHeader plus its audio payload.
    ///
    /// The source JSON names a `.wav`/`.ogg` file (relative to the source JSON's directory) and a
    /// `mode`. `"sample"` decodes the source to PCM offline (a WAV through dr_wav, an Ogg through
    /// stb_vorbis) — the short, overlapping-playable form. `"stream"` keeps the source's encoded
    /// Vorbis bytes for incremental decode at play time — the long-music form; a `stream` source
    /// must already be `.ogg`, since no Vorbis encoder is vendored and a WAV cannot be transcoded.
    ///
    /// dr_wav and stb_vorbis are cooker-only decoders here (stb_vorbis also ships in libveng, which
    /// decodes the stream form at runtime); the offline toolchain vendors both like stb/assimp.
    class AudioImporter final : public AssetImporter
    {
    public:
        /// @brief Returns AssetTypes::AudioClip.
        [[nodiscard]] AssetTypeId Type() const override;

        /// @brief Returns ImporterConcurrency::Serialized — the decoders are the safe default band.
        ///
        /// dr_wav and stb_vorbis are driven per call over the importer's own buffers, but their
        /// reentrancy is not established here, so the importer stays serialized rather than assumed
        /// safe — the same posture the other single-decoder-library importers take.
        [[nodiscard]] ImporterConcurrency Concurrency() const override;

        /// @brief Cooks the audio clip described by `entry` into a binary blob.
        [[nodiscard]] Result<vector<u8>> Cook(const CookContext& context,
                                              const json& entry) const override;
    };
}
