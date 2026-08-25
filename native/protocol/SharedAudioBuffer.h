#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Windows shared memory (page-file-backed file mapping — true shared memory,
// not a disk temp file) matching the BSM1 layout in PROTOCOL.md:
//   uint32 magic "BSM1", uint32 sampleRate, uint32 channels, uint32 frames,
//   then frames*channels float32 interleaved samples.
//
// One side creates (the desktop process, since it owns IPC endpoint /
// shared-memory lifetime per PROTOCOL.md), the other opens by name. Used by
// both native/BeatShoreDesktop and native/BeatShoreBridge (the plugin) --
// lives in native/protocol/ as the one place both build targets share.
class SharedAudioBuffer
{
public:
    ~SharedAudioBuffer() { close(); }

    // Plugin side: create a new mapping sized for the given audio and fill
    // it in (the plugin is the side that actually captured the audio, so it
    // owns creation -- see PROTOCOL.md). Returns false on failure (name
    // collision, out of memory, etc).
    bool create(const std::string& name, uint32_t sampleRate, uint32_t channels, uint32_t frames)
    {
        close();
        const uint64_t dataBytes = uint64_t(frames) * channels * sizeof(float);
        const uint64_t totalBytes = 16 + dataBytes;
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      DWORD(totalBytes >> 32), DWORD(totalBytes & 0xFFFFFFFF), name.c_str());
        if (mapping == nullptr) return false;
        view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size_t(totalBytes));
        if (view == nullptr) { CloseHandle(mapping); mapping = nullptr; return false; }
        auto* bytes = static_cast<uint8_t*>(view);
        std::memcpy(bytes, "BSM1", 4);
        writeU32(bytes + 4, sampleRate);
        writeU32(bytes + 8, channels);
        writeU32(bytes + 12, frames);
        mappedName = name;
        totalSize = totalBytes;
        return true;
    }

    // Desktop side: open a mapping the plugin already created and named in
    // an ANALYSIS_REQUEST. Validates the header's claimed size against
    // what was *actually* mapped, not just that the magic bytes match --
    // without this, a plugin (buggy or otherwise) whose header claims more
    // frames than the mapping actually has room for would let samples()
    // combined with frames()*channels() read past the mapped view: an
    // access violation at best, garbage data silently fed to Node at
    // worst. VirtualQuery reports the real committed region size
    // independent of anything the header itself claims, so this can't be
    // spoofed by the header alone.
    bool open(const std::string& name)
    {
        close();
        mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (mapping == nullptr) return false;
        // map a generous max first isn't needed on Windows -- MapViewOfFile with
        // dwNumberOfBytesToMap=0 maps the entire committed region of the mapping.
        view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (view == nullptr) { CloseHandle(mapping); mapping = nullptr; return false; }
        auto* bytes = static_cast<uint8_t*>(view);
        if (std::memcmp(bytes, "BSM1", 4) != 0) { close(); return false; }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0) { close(); return false; }
        const uint32_t claimedChannels = readU32(8);
        const uint32_t claimedFrames = readU32(12);
        const uint64_t claimedTotalBytes = 16ULL + uint64_t(claimedFrames) * claimedChannels * sizeof(float);
        if (claimedTotalBytes > uint64_t(mbi.RegionSize)) { close(); return false; }

        mappedName = name;
        return true;
    }

    bool isOpen() const { return view != nullptr; }
    uint32_t sampleRate() const { return readU32(4); }
    uint32_t channels() const { return readU32(8); }
    uint32_t frames() const { return readU32(12); }
    float* samples() { return reinterpret_cast<float*>(static_cast<uint8_t*>(view) + 16); }
    const std::string& name() const { return mappedName; }

    // Writes interleaved float32 samples into an already-created mapping.
    void writeSamples(const float* interleaved, uint32_t sampleCount)
    {
        if (!isOpen()) return;
        std::memcpy(samples(), interleaved, size_t(sampleCount) * sizeof(float));
    }

    void close()
    {
        if (view) { UnmapViewOfFile(view); view = nullptr; }
        if (mapping) { CloseHandle(mapping); mapping = nullptr; }
        mappedName.clear();
    }

private:
    static void writeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
    uint32_t readU32(size_t offset) const
    {
        uint32_t v = 0;
        std::memcpy(&v, static_cast<uint8_t*>(view) + offset, 4);
        return v;
    }

    HANDLE mapping = nullptr;
    void* view = nullptr;
    std::string mappedName;
    uint64_t totalSize = 0;
};
