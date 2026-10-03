#pragma once
#include <mrs/audio.hpp>
#include <thread>
namespace mrs::audio {
// Eight 8192-frame pages per voice. Worker writes only unpinned slots.
// Callback pins once per block; no waiting, allocation or I/O.
class ReadAhead {
public:
    static constexpr Sample page_frames = 8192;
    static constexpr std::size_t pages = 8;
    explicit ReadAhead(std::shared_ptr<const WavFile>);
    ~ReadAhead();
    ReadAhead(const ReadAhead&) = delete;
    ReadAhead& operator=(const ReadAhead&) = delete;
    void prime(Sample); // control-only bounded wait, throws on media error/timeout
    void loop(Sample first) noexcept;
    void begin(Sample first) noexcept;
    bool try_begin(Sample first, std::uint32_t frames) noexcept;
    void accept_seek(Sample first) noexcept;
    void cancel_seek() noexcept { retry_.store(-1,std::memory_order_release); }
    bool read(Sample frame, std::uint32_t channel, float& value) noexcept;
    bool end() noexcept; // true if this callback missed any source frames
    std::uint64_t errors() const noexcept { return errors_.load(); }
    std::size_t bytes() const { return pages*static_cast<std::size_t>(page_frames)*file_->channels*sizeof(float); }
private:
    struct Slot {
        std::atomic<int> owner{}; // 0 free, -1 worker, +1 callback
        std::atomic<Sample> page{-1}; // published tag; control reads without pinning callback data
        std::vector<float> samples;
    };
    std::shared_ptr<const WavFile> file_;
    std::array<Slot,pages> slots_;
    std::array<bool,pages> pinned_{};
    std::atomic<Sample> desired_{-1}, warm_{-1}, loop_{-1}, retry_{-1};
    // Control/worker handoff only; the audio callback never touches this gate.
    std::atomic_flag eviction_gate_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> quit_{};
    std::atomic<std::uint64_t> errors_{};
    std::thread worker_;
    bool missed_{};
    void run() noexcept;
};
}
