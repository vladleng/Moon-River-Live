#include <mrs/read_ahead.hpp>
#include <algorithm>
#include <chrono>
#include <stdexcept>
namespace mrs::audio {
ReadAhead::ReadAhead(std::shared_ptr<const WavFile> file) : file_(std::move(file)) {
    for (auto& slot : slots_) slot.samples.resize(static_cast<std::size_t>(page_frames)*file_->channels);
    worker_ = std::thread([this] { run(); });
}
ReadAhead::~ReadAhead() { quit_ = true; if (worker_.joinable()) worker_.join(); }
void ReadAhead::loop(Sample first) noexcept { loop_.store(first < 0 ? -1 : first/page_frames,std::memory_order_release); }
void ReadAhead::begin(Sample first) noexcept {
    const auto page = std::clamp(first,Sample{0},file_->frame_count-1)/page_frames;
    desired_.store(page,std::memory_order_release);
    auto warm = page;
    (void)warm_.compare_exchange_strong(warm,-1,std::memory_order_acq_rel);
    missed_ = false;
    for (std::size_t i = 0; i < pages; ++i) {
        int expected = 0;
        pinned_[i] = slots_[i].owner.compare_exchange_strong(expected,1,std::memory_order_acquire);
    }
}
bool ReadAhead::try_begin(Sample first, std::uint32_t frames) noexcept {
    const auto source = std::clamp(first,Sample{0},file_->frame_count-1);
    const auto head = source/page_frames;
    const auto last = std::min(source+static_cast<Sample>(frames)-1,file_->frame_count-1)/page_frames;
    retry_.store(head,std::memory_order_release);
    missed_ = false;
    for (std::size_t i=0; i<pages; ++i) {
        int expected = 0;
        pinned_[i] = slots_[i].owner.compare_exchange_strong(expected,1,std::memory_order_acquire);
    }
    bool first_ready{}, last_ready{};
    for (std::size_t i=0; i<pages; ++i) if (pinned_[i]) {
        const auto tag = slots_[i].page.load(std::memory_order_relaxed);
        first_ready = first_ready || tag == head;
        last_ready = last_ready || tag == last;
    }
    return first_ready && last_ready;
}
void ReadAhead::accept_seek(Sample first) noexcept {
    const auto head = std::clamp(first,Sample{0},file_->frame_count-1)/page_frames;
    desired_.store(head,std::memory_order_release);
    auto warm = head;
    (void)warm_.compare_exchange_strong(warm,-1,std::memory_order_acq_rel);
    retry_.store(-1,std::memory_order_release);
}
bool ReadAhead::read(Sample frame, std::uint32_t channel, float& value) noexcept {
    const auto page = frame/page_frames;
    for (std::size_t i = 0; i < pages; ++i) if (pinned_[i] && slots_[i].page.load(std::memory_order_relaxed) == page) {
        value = slots_[i].samples[static_cast<std::size_t>(frame%page_frames)*file_->channels+channel];
        return true;
    }
    desired_.store(page,std::memory_order_release); missed_ = true; value = 0; return false;
}
bool ReadAhead::end() noexcept {
    for (std::size_t i = 0; i < pages; ++i) if (pinned_[i]) {
        slots_[i].owner.store(0,std::memory_order_release); pinned_[i] = false;
    }
    return missed_;
}
void ReadAhead::prime(Sample first) {
    const auto target = std::clamp(first,Sample{0},file_->frame_count-1)/page_frames;
    // First quiescent preparation establishes the initial read head. During
    // playback only begin/read (the callback) may move it; the seek target stays
    // protected separately until the audio thread actually applies the command.
    Sample unprepared = -1;
    (void)desired_.compare_exchange_strong(unprepared,target,std::memory_order_acq_rel);
    warm_.store(target,std::memory_order_release);
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for (;;) {
        // Serialize the ready snapshot with worker victim selection. Otherwise
        // a worker that checked an older warm target could claim a slot just
        // after this control thread had observed it ready. Never used by RT.
        const bool inspect = !eviction_gate_.test_and_set(std::memory_order_acquire);
        bool ready = inspect;
        if (inspect) for (Sample p = target; p < target+4 && p*page_frames < file_->frame_count; ++p) {
            bool found = false;
            // Tags are published atomically after sample fill. Control only
            // reads tags, so it must not claim callback pins and cause dropouts.
            for (const auto& slot : slots_)
                if (slot.owner.load(std::memory_order_acquire) != -1 &&
                    slot.page.load(std::memory_order_acquire) == p) found = true;
            ready = ready && found;
        }
        if (inspect) eviction_gate_.clear(std::memory_order_release);
        if (ready) return;
        if (errors_.load() || std::chrono::steady_clock::now() >= until) {
            warm_ = -1; throw std::runtime_error("disk read-ahead failed: media unavailable, invalid samples or timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void ReadAhead::run() noexcept {
    while (!quit_.load()) {
        std::array<Sample,pages> wanted{}; wanted.fill(-1);
        std::size_t count{};
        const auto add = [&](Sample page) {
            if (page < 0 || page*page_frames >= file_->frame_count ||
                std::find(wanted.begin(),wanted.begin()+static_cast<std::ptrdiff_t>(count),page) != wanted.begin()+static_cast<std::ptrdiff_t>(count)) return;
            if (count < pages) wanted[count++] = page;
        };
        const auto warm = warm_.load(std::memory_order_acquire);
        const auto current = desired_.load(std::memory_order_acquire);
        const auto loop = loop_.load(std::memory_order_acquire);
        const auto retry = retry_.load(std::memory_order_acquire);
        if (retry >= 0) { add(retry); add(retry+1); }
        if (current >= 0) for (Sample n = 0; n < (warm >= 0 || retry >= 0 ? 2 : 4); ++n) add(current+n);
        if (loop >= 0) { add(loop); add(loop+1); }
        if (warm >= 0) for (Sample n = 0; n < 4; ++n) add(warm+n);
        for (std::size_t p = 0; p < count && !quit_.load(); ++p) {
            bool exists = false;
            // Only worker writes page tags; pinned tags also remain immutable.
            for (auto& slot : slots_) if (slot.page.load(std::memory_order_relaxed) == wanted[p]) exists = true;
            if (exists) continue;
            for (auto& slot : slots_) {
                if (std::find(wanted.begin(),wanted.begin()+static_cast<std::ptrdiff_t>(count),slot.page.load(std::memory_order_relaxed)) != wanted.begin()+static_cast<std::ptrdiff_t>(count)) continue;
                if (eviction_gate_.test_and_set(std::memory_order_acquire)) continue;
                // Do not even temporarily claim protected pages: the callback
                // cannot wait for a worker to release a failed victim claim.
                // A stale wanted snapshot must not make a ready seek page busy.
                const auto protected_now = [&](Sample head, Sample length) {
                    const auto tag = slot.page.load(std::memory_order_relaxed);
                    return head >= 0 && tag >= head && tag-head < length;
                };
                if (protected_now(warm_.load(std::memory_order_acquire),4) ||
                    protected_now(desired_.load(std::memory_order_acquire),warm_.load(std::memory_order_acquire) >= 0 ? 2 : 4) ||
                    protected_now(loop_.load(std::memory_order_acquire),2) ||
                    protected_now(retry_.load(std::memory_order_acquire),2)) {
                    eviction_gate_.clear(std::memory_order_release); continue;
                }
                int expected = 0;
                if (!slot.owner.compare_exchange_strong(expected,-1,std::memory_order_acquire)) {
                    eviction_gate_.clear(std::memory_order_release); continue;
                }
                // The control cursor may change after this iteration's snapshot.
                // Recheck protection while owning the victim: prime cannot observe
                // this page ready until publication, so stale work cannot evict
                // newly primed seek/loop pages after prime returns.
                const auto latest_warm = warm_.load(std::memory_order_acquire);
                const auto latest_current = desired_.load(std::memory_order_acquire);
                const auto latest_loop = loop_.load(std::memory_order_acquire);
                const auto protected_page = [&](Sample head, Sample count) {
                    const auto tag = slot.page.load(std::memory_order_relaxed);
                    return head >= 0 && tag >= head && tag-head < count;
                };
                if (protected_page(latest_warm,4) || protected_page(latest_current,latest_warm >= 0 ? 2 : 4) || protected_page(latest_loop,2) || protected_page(retry_.load(std::memory_order_acquire),2)) {
                    slot.owner.store(0,std::memory_order_release);
                    eviction_gate_.clear(std::memory_order_release); continue;
                }
                eviction_gate_.clear(std::memory_order_release);
                try {
                    const auto first = wanted[p]*page_frames;
                    const auto frames = std::min(page_frames,file_->frame_count-first);
                    file_->read(first,std::span<float>(slot.samples).first(static_cast<std::size_t>(frames)*file_->channels));
                    slot.page.store(wanted[p],std::memory_order_release);
                } catch (...) { slot.page.store(-1,std::memory_order_release); errors_.fetch_add(1); quit_ = true; }
                slot.owner.store(0,std::memory_order_release); break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}
