#include <Veng/Diagnostics/TraceSink.h>

// CapturingTestSink's bodies. They live here rather than in the header because a
// non-template inline body odr-uses everything it calls, so a body defined in the
// class definition instantiates the retained vectors' members — assign, push_back,
// resize — in every translation unit that parses TraceSink.h, whether or not it ever
// constructs the sink. TraceSink.h sits behind Profiler.h, which most of the tree
// includes.

namespace Veng::Diagnostics
{
    void CapturingTestSink::OnChunk(ThreadId thread, const u8* data, usize bytes)
    {
        CapturedChunk chunk;
        chunk.Thread = thread;
        chunk.Bytes.assign(data, data + bytes);
        m_Chunks.push_back(std::move(chunk));
    }

    void CapturingTestSink::OnStrings(const StringTableDelta& delta)
    {
        if (m_Strings.size() < static_cast<usize>(delta.FirstId) + delta.Strings.size())
        {
            m_Strings.resize(static_cast<usize>(delta.FirstId) + delta.Strings.size());
        }
        for (usize i = 0; i < delta.Strings.size(); ++i)
        {
            m_Strings[delta.FirstId + i] = string(delta.Strings[i]);
        }
    }

    void CapturingTestSink::OnTrack(const TrackDescriptor& track)
    {
        m_Tracks.push_back(CapturedTrack{.Id = track.Id,
                                         .IsVirtual = track.IsVirtual,
                                         .Role = track.Role,
                                         .Name = string(track.Name)});
    }

    string_view CapturingTestSink::GetString(NameId id) const
    {
        return id < m_Strings.size() ? string_view(m_Strings[id]) : string_view();
    }

    usize CapturingTestSink::GetStringCount() const
    {
        return m_Strings.size();
    }

    void CapturingTestSink::ClearChunks()
    {
        m_Chunks.clear();
    }
}
