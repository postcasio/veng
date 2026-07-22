#include <Veng/Diagnostics/FileTraceSink.h>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>

#include "TraceFile.h"
#include "TraceFormat.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

// The identity fields compiled into this one TU (never onto the public surface): the engine version,
// and this tree's git provenance resolved at configure time. Absent defines degrade to placeholders
// rather than failing the build, so the sink compiles in a tree built without git.
#ifndef VENG_TRACE_VERSION
#define VENG_TRACE_VERSION "0.0.0"
#endif
#ifndef VENG_TRACE_GIT_SHA
#define VENG_TRACE_GIT_SHA "unknown"
#endif
#ifndef VENG_TRACE_GIT_DIRTY
#define VENG_TRACE_GIT_DIRTY 0
#endif

namespace Veng::Diagnostics
{
    using TraceFormat::ChunkHeader;
    using TraceFormat::EventRecord;
    using TraceFormat::RecordType;

    namespace
    {
        /// @brief Returns the running executable's basename — never its path.
        ///
        /// Only the basename is recorded in a capture: the absolute path embeds a developer's home
        /// directory and local tree layout, which a committed fixture would carry forever.
        path ExecutableBasename()
        {
#if defined(_WIN32)
            std::vector<wchar_t> buffer(MAX_PATH);
            for (;;)
            {
                const DWORD length =
                    GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0)
                {
                    return path();
                }
                if (length < buffer.size())
                {
                    return path(std::wstring(buffer.data(), length)).filename();
                }
                buffer.resize(buffer.size() * 2);
            }
#elif defined(__APPLE__)
            u32 size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::vector<char> buffer(size);
            if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            {
                return path();
            }
            return path(buffer.data()).filename();
#else
            std::vector<char> buffer(1024);
            for (;;)
            {
                const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
                if (length <= 0)
                {
                    return path();
                }
                if (static_cast<usize>(length) < buffer.size())
                {
                    return path(string(buffer.data(), static_cast<usize>(length))).filename();
                }
                buffer.resize(buffer.size() * 2);
            }
#endif
        }

        /// @brief Decodes one profiler chunk (the internal fixed-width encoding) into absolute records.
        TraceFileFormat::ChunkData DecodeInternalChunk(ThreadId thread, const u8* data, usize bytes)
        {
            TraceFileFormat::ChunkData out;
            out.Thread = thread;
            if (bytes < sizeof(ChunkHeader))
            {
                return out;
            }
            ChunkHeader header;
            std::memcpy(&header, data, sizeof(header));
            out.SequenceNumber = header.SequenceNumber;
            out.TimestampBase = header.TimestampBase;

            usize offset = header.FirstRecordOffset;
            const usize end = std::min<usize>(bytes, static_cast<usize>(header.FirstRecordOffset) +
                                                         header.RecordBytes);
            while (offset + sizeof(EventRecord) <= end)
            {
                EventRecord record;
                std::memcpy(&record, data + offset, sizeof(record));
                offset += sizeof(EventRecord);

                TraceFileFormat::EventRecord decoded;
                decoded.Type = record.Type;
                decoded.Track = record.Track;
                decoded.Name = record.Name;
                decoded.Frame = record.Frame;
                decoded.BeginTicks = header.TimestampBase + record.BeginDelta;
                if (record.Type == static_cast<u8>(RecordType::ScopeComplete))
                {
                    decoded.EndTicks = header.TimestampBase + record.EndOrValue;
                }
                else if (record.Type == static_cast<u8>(RecordType::Counter))
                {
                    f64 value;
                    std::memcpy(&value, &record.EndOrValue, sizeof(value));
                    decoded.Value = value;
                    decoded.EndTicks = decoded.BeginTicks;
                }
                else
                {
                    decoded.EndTicks = decoded.BeginTicks;
                }
                out.Records.push_back(decoded);
            }
            return out;
        }
    }

    /// @brief One unit of work handed to the writer thread.
    struct WriterItem
    {
        /// @brief What the item carries.
        enum class Kind
        {
            Chunk,
            Strings,
            Track,
            Close,
        };

        /// @brief The item kind.
        Kind What = Kind::Close;
        /// @brief For a Close item: whether the close is clean (writes a trailer) or a dropped sink.
        bool Clean = false;
        /// @brief For a Chunk item: the track it came from.
        ThreadId Thread = 0;
        /// @brief For a Chunk item: a copy of the chunk bytes.
        vector<u8> Bytes;
        /// @brief For a Strings item: the id of the first string.
        NameId FirstId = 0;
        /// @brief For a Strings item: the new strings.
        vector<string> Strings;
        /// @brief For a Track item: the track's id in its kind's space.
        u32 TrackIdValue = 0;
        /// @brief For a Track item: whether the id is a virtual TrackId (else a thread ThreadId).
        bool TrackVirtual = false;
        /// @brief For a Track item: the kind of work the track carries.
        TrackRole TrackRoleValue = TrackRole::Cpu;
        /// @brief For a Track item: the track's display name.
        string TrackName;
    };

    struct FileTraceSink::Impl
    {
        path FilePath;
        bool RingDump = false;
        u64 TickFrequency = 0;
        u64 TickBase = 0;
        string EngineVersion;
        string ExecutableName;

        std::atomic<u64> DroppedEvents{0};
        std::atomic<u64> DroppedThreads{0};

        std::mutex QueueMutex;
        std::condition_variable QueueCondition;
        std::deque<WriterItem> Queue;
        bool Closed = false;

        std::thread Writer;

        // Writer-thread-owned accumulation. Built up as items drain, encoded at close.
        vector<TraceFileFormat::ChunkData> Chunks;
        vector<string> StringTable; // indexed by (id - 1)

        /// @brief One track descriptor delivered through OnTrack, retained until encode.
        struct DeliveredTrack
        {
            u32 Id = 0;
            bool IsVirtual = false;
            TrackRole Role = TrackRole::Cpu;
            string Name;
        };
        vector<DeliveredTrack> DeliveredTracks;

        /// @brief Finds a delivered descriptor for a track id of the given kind, or nullptr.
        [[nodiscard]] const DeliveredTrack* FindDeliveredTrack(u32 id, bool isVirtual) const
        {
            for (const DeliveredTrack& track : DeliveredTracks)
            {
                if (track.Id == id && track.IsVirtual == isVirtual)
                {
                    return &track;
                }
            }
            return nullptr;
        }

        void Enqueue(WriterItem&& item)
        {
            {
                const std::scoped_lock lock(QueueMutex);
                Queue.push_back(std::move(item));
            }
            QueueCondition.notify_one();
        }

        void Run()
        {
            bool clean = false;
            for (;;)
            {
                WriterItem item;
                {
                    std::unique_lock lock(QueueMutex);
                    QueueCondition.wait(lock, [this] { return !Queue.empty(); });
                    item = std::move(Queue.front());
                    Queue.pop_front();
                }

                if (item.What == WriterItem::Kind::Chunk)
                {
                    TraceFileFormat::ChunkData chunk =
                        DecodeInternalChunk(item.Thread, item.Bytes.data(), item.Bytes.size());
                    if (!chunk.Records.empty())
                    {
                        Chunks.push_back(std::move(chunk));
                    }
                }
                else if (item.What == WriterItem::Kind::Strings && !item.Strings.empty())
                {
                    // Ids run FirstId..FirstId+count-1, so the table must hold index FirstId+count-2.
                    const usize maxId = static_cast<usize>(item.FirstId) + item.Strings.size() - 1;
                    if (StringTable.size() < maxId)
                    {
                        StringTable.resize(maxId);
                    }
                    for (usize i = 0; i < item.Strings.size(); ++i)
                    {
                        StringTable[item.FirstId + i - 1] = std::move(item.Strings[i]);
                    }
                }
                else if (item.What == WriterItem::Kind::Track)
                {
                    // Last delivery wins: a thread renamed after its first descriptor updates in place.
                    DeliveredTrack* existing = nullptr;
                    for (DeliveredTrack& track : DeliveredTracks)
                    {
                        if (track.Id == item.TrackIdValue && track.IsVirtual == item.TrackVirtual)
                        {
                            existing = &track;
                            break;
                        }
                    }
                    if (existing == nullptr)
                    {
                        DeliveredTracks.push_back(
                            DeliveredTrack{.Id = item.TrackIdValue,
                                           .IsVirtual = item.TrackVirtual,
                                           .Role = item.TrackRoleValue,
                                           .Name = std::move(item.TrackName)});
                    }
                    else
                    {
                        existing->Role = item.TrackRoleValue;
                        existing->Name = std::move(item.TrackName);
                    }
                }
                else
                {
                    clean = item.Clean;
                    break;
                }
            }
            Finish(clean);
        }

        void Finish(bool clean)
        {
            using namespace TraceFileFormat;

            const BuildConfig config =
#if defined(NDEBUG)
                BuildConfig::Release;
#else
                BuildConfig::Debug;
#endif
            const bool profileEnabled =
#if defined(VE_PROFILE) && VE_PROFILE
                true;
#else
                false;
#endif
            TraceWriter writer(RingDump ? CaptureMode::RingDump : CaptureMode::Triggered, config,
                               profileEnabled, TickFrequency, TickBase);

            // Only the enumerated identity fields — no absolute path, env, command line, or host name.
            const TraceWriter::Provenance engineProvenance{.Name = "engine",
                                                           .ShortSha = VENG_TRACE_GIT_SHA,
                                                           .Dirty = (VENG_TRACE_GIT_DIRTY != 0)};
            writer.WriteMetadata(EngineVersion,
                                 ExecutableName.empty() ? string_view() : ExecutableName,
                                 std::span(&engineProvenance, 1));

            // Every interned string as one full table. The sink accumulates the whole table (the sink
            // is handed the current table on attach and every later delta), so a ring dump — whose
            // early chunks may be gone — still resolves ids interned in those discarded chunks.
            NameId highest = 0;
            for (usize i = 0; i < StringTable.size(); ++i)
            {
                if (!StringTable[i].empty())
                {
                    highest = static_cast<NameId>(i + 1);
                }
            }
            if (highest > 0)
            {
                vector<string_view> views;
                views.reserve(highest);
                for (NameId id = 1; id <= highest; ++id)
                {
                    views.emplace_back(StringTable[id - 1]);
                }
                writer.WriteStringTable(1, views, true);
            }

            // Track descriptors derived from what the chunks reference: one thread track per producing
            // ThreadId, one virtual track per referenced non-zero TrackId. Names are unknown to the
            // sink (the profiler does not deliver them here), so a decoder falls back to the id.
            vector<u32> threadTracks;
            vector<u32> virtualTracks;
            for (const ChunkData& chunk : Chunks)
            {
                if (std::ranges::find(threadTracks, chunk.Thread) == threadTracks.end())
                {
                    threadTracks.push_back(chunk.Thread);
                }
                for (const TraceFileFormat::EventRecord& record : chunk.Records)
                {
                    if (record.Track != 0 &&
                        std::ranges::find(virtualTracks, record.Track) == virtualTracks.end())
                    {
                        virtualTracks.push_back(record.Track);
                    }
                }
            }
            // A referenced track the profiler also delivered a descriptor for carries that name and
            // role; one it did not (a lazily attached, never-named thread) falls back to its id.
            for (const u32 id : threadTracks)
            {
                const DeliveredTrack* meta = FindDeliveredTrack(id, false);
                writer.WriteTrack(TraceFileFormat::TrackDescriptor{
                    .Kind = TrackKind::Thread,
                    .Id = id,
                    .Role = meta != nullptr ? meta->Role : TrackRole::Cpu,
                    .Name = meta != nullptr ? meta->Name : string()});
            }
            for (const u32 id : virtualTracks)
            {
                const DeliveredTrack* meta = FindDeliveredTrack(id, true);
                writer.WriteTrack(TraceFileFormat::TrackDescriptor{
                    .Kind = TrackKind::Virtual,
                    .Id = id,
                    .Role = meta != nullptr ? meta->Role : TrackRole::Custom,
                    .Name = meta != nullptr ? meta->Name : string()});
            }

            for (const ChunkData& chunk : Chunks)
            {
                writer.WriteChunk(chunk);
            }

            writer.WriteAccounting(DroppedEvents.load(std::memory_order_relaxed),
                                   DroppedThreads.load(std::memory_order_relaxed));
            // The trailer is written only on a clean close; a dropped sink leaves the capture without
            // one, so a reader sees it as truncated.
            if (clean)
            {
                writer.WriteTrailer();
            }

            const vector<u8>& bytes = writer.GetBytes();
            const VoidResult written = WriteFileAtomic(FilePath, std::span<const u8>(bytes));
            if (!written)
            {
                Log::Error("FileTraceSink: failed to commit capture '{}': {}", FilePath.string(),
                           written.error());
            }
        }
    };

    FileTraceSink::FileTraceSink() : m_Impl(CreateUnique<Impl>()) {}

    Unique<FileTraceSink> FileTraceSink::Create(const path& filePath, bool ringDump)
    {
        auto sink = Unique<FileTraceSink>(new FileTraceSink());
        Impl& impl = *sink->m_Impl;
        impl.FilePath = filePath;
        impl.RingDump = ringDump;
        impl.TickFrequency = TraceTickFrequency();
        impl.TickBase = NowTicks();
        impl.EngineVersion = VENG_TRACE_VERSION;
        impl.ExecutableName = ExecutableBasename().string();

        std::error_code ec;
        if (filePath.has_parent_path())
        {
            std::filesystem::create_directories(filePath.parent_path(), ec);
        }

        impl.Writer = std::thread([&impl] { impl.Run(); });
        return sink;
    }

    FileTraceSink::~FileTraceSink()
    {
        // If OnClose was never called (e.g. the sink was dropped without a clean profiler teardown),
        // stop the writer here — no trailer is written, so the capture reads as truncated.
        if (m_Impl->Writer.joinable())
        {
            {
                const std::scoped_lock lock(m_Impl->QueueMutex);
                if (!m_Impl->Closed)
                {
                    m_Impl->Closed = true;
                    m_Impl->Queue.push_back(
                        WriterItem{.What = WriterItem::Kind::Close, .Clean = false});
                }
            }
            m_Impl->QueueCondition.notify_one();
            m_Impl->Writer.join();
        }
    }

    void FileTraceSink::OnChunk(ThreadId thread, const u8* data, usize bytes)
    {
        WriterItem item;
        item.What = WriterItem::Kind::Chunk;
        item.Thread = thread;
        item.Bytes.assign(data, data + bytes);
        m_Impl->Enqueue(std::move(item));
    }

    void FileTraceSink::OnStrings(const StringTableDelta& delta)
    {
        WriterItem item;
        item.What = WriterItem::Kind::Strings;
        item.FirstId = delta.FirstId;
        item.Strings.reserve(delta.Strings.size());
        for (const string_view text : delta.Strings)
        {
            item.Strings.emplace_back(text);
        }
        m_Impl->Enqueue(std::move(item));
    }

    void FileTraceSink::OnTrack(const TrackDescriptor& track)
    {
        WriterItem item;
        item.What = WriterItem::Kind::Track;
        item.TrackIdValue = track.Id;
        item.TrackVirtual = track.IsVirtual;
        item.TrackRoleValue = track.Role;
        item.TrackName = string(track.Name);
        m_Impl->Enqueue(std::move(item));
    }

    void FileTraceSink::OnFlush() {}

    void FileTraceSink::OnClose()
    {
        bool join = false;
        {
            const std::scoped_lock lock(m_Impl->QueueMutex);
            if (!m_Impl->Closed)
            {
                m_Impl->Closed = true;
                m_Impl->Queue.push_back(WriterItem{.What = WriterItem::Kind::Close, .Clean = true});
                join = true;
            }
        }
        if (join)
        {
            m_Impl->QueueCondition.notify_one();
            if (m_Impl->Writer.joinable())
            {
                m_Impl->Writer.join();
            }
        }
    }

    void FileTraceSink::SetAccounting(u64 droppedEvents, u64 droppedThreads)
    {
        m_Impl->DroppedEvents.store(droppedEvents, std::memory_order_relaxed);
        m_Impl->DroppedThreads.store(droppedThreads, std::memory_order_relaxed);
    }

    const path& FileTraceSink::GetPath() const
    {
        return m_Impl->FilePath;
    }
}
