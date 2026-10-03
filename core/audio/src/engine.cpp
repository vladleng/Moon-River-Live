#include <mrs/audio.hpp>
#include <mrs/read_ahead.hpp>
#include <mrs/recording.hpp>
#include <mrs/processing.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mrs::audio {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<Sample>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
Sample AudioData::frames() const {
    return file ? file->frame_count : (channels ? static_cast<Sample>(samples.size() / channels) : 0);
}
void AudioData::validate() const {
    if (file) {
        if (!samples.empty() || file->sample_rate != sample_rate || file->channels != channels ||
            sample_rate < 8000 || sample_rate > 768000 || !channels || channels > max_channels || frames() <= 0 || frames() > max_sample)
            throw std::invalid_argument("invalid disk audio asset");
        return;
    }
    if (sample_rate < 8000 || sample_rate > 768000 || channels == 0 || channels > max_channels ||
        samples.empty() || samples.size() % channels != 0 || frames() > max_sample)
        throw std::invalid_argument("invalid audio asset");
    for (const auto sample : samples)
        if (!std::isfinite(sample)) throw std::invalid_argument("non-finite audio sample");
}
AudioEngine::AudioEngine() { prepare(RenderConfig{}, {}); }
void AudioEngine::prepare(RenderConfig config, RenderGraph graph, RealtimeState initial) {
    if ((initial.playback != PlaybackState::stopped && initial.playback != PlaybackState::paused) ||
        initial.sample < 0 || initial.sample > max_sample ||
        (initial.loop && (initial.loop->start < 0 || initial.loop->start >= initial.loop->end || initial.loop->end > max_sample)))
        throw std::invalid_argument("invalid quiescent transport state");

    if (config.sample_rate < 8000 || config.sample_rate > 768000 ||
        config.input_channels > max_channels || config.output_channels == 0 ||
        config.output_channels > max_channels || config.max_block == 0 || config.max_block > 65536 ||
        graph.voices.size() > max_voices || graph.monitor.size() > max_channels * max_channels)
        throw std::invalid_argument("invalid render configuration");
    for (const auto& voice : graph.voices) {
        if (!voice.asset) throw std::invalid_argument("missing asset");
        voice.asset->validate();
        if (voice.asset->sample_rate != config.sample_rate)
            throw std::invalid_argument("asset/device sample-rate mismatch (no realtime resampling)");
        if (voice.start < 0 || voice.start > max_sample || voice.length <= 0 ||
            voice.length > max_sample - voice.start || voice.source_offset < 0 ||
            voice.source_offset > voice.asset->frames() || voice.length > voice.asset->frames() - voice.source_offset ||
            voice.routes.size() > max_channels * max_channels)
            throw std::invalid_argument("invalid voice range");
        for (const auto& route : voice.routes)
            if (route.source_channel >= voice.asset->channels || route.output_channel >= config.output_channels ||
                !std::isfinite(route.gain) || std::abs(route.gain) > 16)
                throw std::invalid_argument("invalid playback route");
    }
    for (const auto& route : graph.monitor)
        if (route.input_channel >= config.input_channels || route.output_channel >= config.output_channels ||
            !std::isfinite(route.gain) || std::abs(route.gain) > 16)
            throw std::invalid_argument("invalid monitoring route");
    if (graph.processors) {
        const auto processor = graph.processors->config();
        if (processor.sample_rate != config.sample_rate || processor.channels != config.output_channels ||
            processor.max_block < config.max_block)
            throw std::invalid_argument("processor/audio render configuration mismatch");
    }
    if (graph.recording && (config.input_channels != 1 || graph.recording->rate() != config.sample_rate ||
        graph.recording->start() != initial.sample || initial.loop))
        throw std::invalid_argument("recording needs one input, matching start/rate and no loop");
    std::size_t stream_bytes{}, stream_count{};
    for (auto& voice : graph.voices) if (voice.asset->file) {
        if (config.max_block > static_cast<std::uint32_t>(ReadAhead::page_frames))
            throw std::invalid_argument("streaming supports callback blocks up to 8192 frames");
        stream_bytes += ReadAhead::pages*static_cast<std::size_t>(ReadAhead::page_frames)*voice.asset->channels*sizeof(float);
        if (++stream_count > 32 || stream_bytes > 256*1024*1024)
            throw std::invalid_argument("disk voice budget exceeded (32 voices / 256 MiB)");
    }
    for (auto& voice : graph.voices) if (voice.asset->file) {
        voice.stream = std::make_shared<ReadAhead>(voice.asset->file);
        voice.stream->prime(voice.source_offset+std::clamp(initial.sample-voice.start,Sample{0},voice.length-1));
        if (initial.loop && initial.loop->start < voice.start+voice.length && initial.loop->end > voice.start) {
            const auto source = voice.source_offset+std::max(Sample{0},initial.loop->start-voice.start);
            voice.stream->loop(source); voice.stream->prime(source);
        }
    }
    config_ = config;
    graph_ = std::move(graph);
    monitor_enabled_ = graph_.monitoring; input_peak_ = 0;
    Control discarded;
    while (controls_.pop(discarded)) {}
    rt_ = initial; pending_seek_.reset();
    callbacks_ = 0; input_overflows_ = 0; input_underflows_ = 0;
    output_underflows_ = 0; output_overflows_ = 0; deadlines_ = 0;
    invalid_blocks_ = 0; clipped_ = 0; missing_inputs_ = 0; disk_underruns_ = 0;
    max_ns_ = 0; measured_ = 0; min_frames_ = 0; max_frames_ = 0;
    for (auto& bin : load_histogram_) bin = 0;
    publish();
}
void AudioEngine::prime_streams(Sample position) {
    for (auto& voice : graph_.voices) if (voice.stream)
        voice.stream->prime(voice.source_offset+std::clamp(position-voice.start,Sample{0},voice.length-1));
}
void AudioEngine::prime_loop(std::optional<LoopRange> loop) {
    for (auto& voice : graph_.voices) if (voice.stream) {
        if (loop && loop->start < voice.start+voice.length && loop->end > voice.start) {
            const auto source = voice.source_offset+std::max(Sample{0},loop->start-voice.start);
            voice.stream->loop(source); voice.stream->prime(source);
        } else voice.stream->loop(-1);
    }
}
bool AudioEngine::enqueue(Control control) noexcept { return controls_.push(control); }
void AudioEngine::publish() noexcept {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    published_sample_.store(rt_.sample, std::memory_order_relaxed);
    published_loop_start_.store(rt_.loop ? rt_.loop->start : 0, std::memory_order_relaxed);
    published_loop_end_.store(rt_.loop ? rt_.loop->end : 0, std::memory_order_relaxed);
    published_playback_.store(static_cast<int>(rt_.playback), std::memory_order_relaxed);
    sequence_.fetch_add(1, std::memory_order_release);
}
RealtimeState AudioEngine::state() const {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto before = sequence_.load(std::memory_order_acquire);
        if (before & 1U) continue;
        RealtimeState result;
        result.sample = published_sample_.load(std::memory_order_relaxed);
        result.playback = static_cast<PlaybackState>(published_playback_.load(std::memory_order_relaxed));
        const auto start = published_loop_start_.load(std::memory_order_relaxed);
        const auto end = published_loop_end_.load(std::memory_order_relaxed);
        if (end > start) result.loop = LoopRange{start, end};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (before == sequence_.load(std::memory_order_acquire)) return result;
    }
    throw std::runtime_error("audio state busy; poll again");
}
void AudioEngine::process(const float* input, float* output, std::uint32_t frames, std::uint32_t input_flags) noexcept {
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    if (!output || frames == 0 || frames > config_.max_block) {
        if (graph_.recording) graph_.recording->input_dropout();
        invalid_blocks_.fetch_add(1, std::memory_order_relaxed);
        // Silence an oversized but valid device buffer without indexing assets.
        if (output) std::fill_n(output, static_cast<std::size_t>(frames) * config_.output_channels, 0.0F);
        return;
    }
    auto minimum = min_frames_.load(std::memory_order_relaxed);
    if (minimum == 0 || frames < minimum) min_frames_.store(frames, std::memory_order_relaxed);
    if (frames > max_frames_.load(std::memory_order_relaxed)) max_frames_.store(frames, std::memory_order_relaxed);
    Control control;
    for (int i = 0; i < 63 && controls_.pop(control); ++i) {
        switch (control.kind) {
        case ControlKind::monitor: monitor_enabled_ = control.a != 0; break;
        case ControlKind::play: rt_.playback = PlaybackState::playing; break;
        case ControlKind::pause:
            if (rt_.playback == PlaybackState::playing) rt_.playback = PlaybackState::paused;
            break;
        case ControlKind::stop:
            pending_seek_.reset(); rt_.playback = PlaybackState::stopped; rt_.sample = 0;
            if (graph_.processors) graph_.processors->panic();
            break;
        case ControlKind::prepared_seek:
            if (graph_.recording) { graph_.recording->discontinuity(); break; }
            if (control.a >= 0 && control.a <= max_sample) pending_seek_ = control.a;
            break;
        case ControlKind::seek:
            pending_seek_.reset();
            if (graph_.recording) { graph_.recording->discontinuity(); break; }
            if (control.a >= 0 && control.a <= max_sample) {
                rt_.sample = control.a;
                if (graph_.processors) graph_.processors->panic();
            }
            break;
        case ControlKind::loop:
            if (graph_.recording) { graph_.recording->discontinuity(); break; }
            if (control.a >= 0 && control.a < control.b && control.b <= max_sample)
                rt_.loop = LoopRange{control.a, control.b};
            else if (control.a == 0 && control.b == 0) rt_.loop.reset();
            break;
        }
    }
    // A later control prime can replace an earlier queued seek's warm target.
    // Pin/verify the candidate before changing RT position. If unavailable, keep
    // rendering the current head and retry next block; callback never waits.
    bool seek_pinned = false;
    if (pending_seek_) {
        bool ready = true;
        for (auto& voice : graph_.voices) if (voice.stream) {
            const auto source = voice.source_offset+std::clamp(*pending_seek_-voice.start,Sample{0},voice.length-1);
            if (!voice.stream->try_begin(source,frames)) ready = false;
        }
        if (ready) {
            rt_.sample = *pending_seek_; pending_seek_.reset(); seek_pinned = true;
            for (auto& voice : graph_.voices) if (voice.stream)
                voice.stream->accept_seek(voice.source_offset+std::clamp(rt_.sample-voice.start,Sample{0},voice.length-1));
            if (graph_.processors) graph_.processors->panic();
        } else {
            for (auto& voice : graph_.voices) if (voice.stream) (void)voice.stream->end();
        }
    }
    std::fill_n(output, static_cast<std::size_t>(frames) * config_.output_channels, 0.0F);
    float peak{};
    if (input) for (std::size_t n=0; n<static_cast<std::size_t>(frames)*config_.input_channels; ++n)
        if (std::isfinite(input[n])) peak = std::max(peak,std::abs(input[n]));
    input_peak_.store(peak,std::memory_order_relaxed);
    if (graph_.recording && rt_.playback == PlaybackState::playing) {
        if (input_flags & 3U) graph_.recording->input_dropout();
        graph_.recording->capture(input,config_.input_channels,frames,rt_.sample);
    }
    if (!input && monitor_enabled_ && !graph_.monitor.empty()) missing_inputs_.fetch_add(1, std::memory_order_relaxed);
    if (!seek_pinned) for (auto& voice : graph_.voices) if (voice.stream) {
        if (!pending_seek_) voice.stream->cancel_seek();
        voice.stream->begin(voice.source_offset+std::clamp(rt_.sample-voice.start,Sample{0},voice.length-1));
    }
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto out = static_cast<std::size_t>(frame) * config_.output_channels;
        // Monitoring is independent of transport and playback source density.
        if (input && monitor_enabled_) {
            const auto in = static_cast<std::size_t>(frame) * config_.input_channels;
            for (const auto& route : graph_.monitor)
                output[out + route.output_channel] += input[in + route.input_channel] * route.gain;
        }
        if (rt_.playback == PlaybackState::playing) {
            if (rt_.loop && rt_.sample >= rt_.loop->end)
                rt_.sample = rt_.loop->start + (rt_.sample - rt_.loop->start) % (rt_.loop->end - rt_.loop->start);
            for (const auto& voice : graph_.voices) {
                if (rt_.sample < voice.start || rt_.sample - voice.start >= voice.length) continue;
                const auto source_frame = rt_.sample - voice.start + voice.source_offset;
                const auto source = static_cast<std::size_t>(source_frame)*voice.asset->channels;
                for (const auto& route : voice.routes) {
                    float sample{};
                    if (voice.stream) (void)voice.stream->read(source_frame,route.source_channel,sample);
                    else sample = voice.asset->samples[source+route.source_channel];
                    output[out+route.output_channel] += sample*route.gain;
                }
            }
            if (rt_.sample < max_sample) ++rt_.sample;
            else rt_.playback = PlaybackState::stopped;
        }

    }
    for (auto& voice : graph_.voices) if (voice.stream && voice.stream->end())
        disk_underruns_.fetch_add(1,std::memory_order_relaxed);
    const auto output_samples = static_cast<std::size_t>(frames) * config_.output_channels;
    for (std::size_t i = 0; i < output_samples; ++i)
        if (!std::isfinite(output[i])) output[i] = 0;
    if (graph_.processors) graph_.processors->process(output,frames);
    for (std::size_t i = 0; i < output_samples; ++i) {
        auto& sample = output[i];
        if (!std::isfinite(sample)) sample = 0;
        if (sample > 1 || sample < -1) {
            clipped_.fetch_add(1,std::memory_order_relaxed);
            sample = std::clamp(sample,-1.0F,1.0F);
        }
    }
    // Normalize at an exact block boundary for coherent displayed loop position.
    if (rt_.loop && rt_.playback == PlaybackState::playing && rt_.sample >= rt_.loop->end)
        rt_.sample = rt_.loop->start + (rt_.sample - rt_.loop->start) % (rt_.loop->end - rt_.loop->start);
    publish();
}
void AudioEngine::observe(std::uint64_t ns, std::uint32_t frames, std::uint32_t flags) noexcept {
    // Backend-neutral bits: 1 input underflow, 2 input overflow,
    // 4 output underflow, 8 output overflow.
    if (flags & 1U) input_underflows_.fetch_add(1, std::memory_order_relaxed);
    if (flags & 2U) input_overflows_.fetch_add(1, std::memory_order_relaxed);
    if (flags & 4U) output_underflows_.fetch_add(1, std::memory_order_relaxed);
    if (flags & 8U) output_overflows_.fetch_add(1, std::memory_order_relaxed);
    measured_.fetch_add(1, std::memory_order_relaxed);
    if (ns > max_ns_.load(std::memory_order_relaxed)) max_ns_.store(ns, std::memory_order_relaxed);
    if (!frames) return;
    const double load = static_cast<double>(ns) * config_.sample_rate / (static_cast<double>(frames) * 1e9);
    if (load >= 1) deadlines_.fetch_add(1, std::memory_order_relaxed);
    const auto bin = static_cast<std::size_t>(std::clamp(std::ceil(load * 100), 0.0, 100.0));
    load_histogram_[bin].fetch_add(1, std::memory_order_relaxed);
}
Metrics AudioEngine::metrics() const {
    Metrics m;
    m.input_peak = input_peak_.load();
    m.disk_underruns = disk_underruns_.load();
    for (const auto& voice : graph_.voices) if (voice.stream) m.disk_errors += voice.stream->errors();
    m.callbacks = callbacks_.load(); m.input_overflows = input_overflows_.load();
    m.input_underflows = input_underflows_.load(); m.output_underflows = output_underflows_.load();
    m.output_overflows = output_overflows_.load(); m.deadline_misses = deadlines_.load();
    m.invalid_blocks = invalid_blocks_.load(); m.clipped_samples = clipped_.load();
    m.missing_inputs = missing_inputs_.load(); m.max_callback_ns = max_ns_.load();
    m.measured_callbacks = measured_.load(); m.min_frames = min_frames_.load(); m.max_frames = max_frames_.load();
    const auto percentile = [this](double fraction) {
        std::array<std::uint64_t, 101> bins{};
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < bins.size(); ++i) { bins[i] = load_histogram_[i].load(); total += bins[i]; }
        if (!total) return 0.0;
        const auto wanted = static_cast<std::uint64_t>(std::ceil(static_cast<double>(total) * fraction));
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < bins.size(); ++i) { sum += bins[i]; if (sum >= wanted) return static_cast<double>(i); }
        return 100.0;
    };
    m.p50_load_percent = percentile(0.5); m.p95_load_percent = percentile(0.95); m.p99_load_percent = percentile(0.99);
    return m;
}
EngineTransport::EngineTransport(std::shared_ptr<AudioEngine> engine, Timeline timeline)
    : engine_(std::move(engine)), timeline_(std::move(timeline)) {
    if (!engine_) throw std::invalid_argument("missing shared audio engine");
    last_ = state();
}
TransportState EngineTransport::state() const {
    const auto s = engine_->state();
    return {s.playback, s.sample, timeline_.musical_position(timeline_.to_ticks(s.sample)), s.loop};
}
void EngineTransport::rebind_timeline(Timeline timeline) {
    if (timeline.sample_rate() != engine_->config().sample_rate)
        throw std::invalid_argument("timeline sample rate must match audio device");
    (void)timeline.to_ticks(engine_->state().sample);
    timeline_ = std::move(timeline);
    poll();
}
void EngineTransport::send(Control control) {
    if (!engine_->enqueue(control)) throw std::runtime_error("audio command queue full; retry on control thread");
}
void EngineTransport::play() { send({ControlKind::play}); }
void EngineTransport::pause() { send({ControlKind::pause}); }
void EngineTransport::stop() {
    send({ControlKind::stop}); // stopping must work even when media disappears
    try { engine_->prime_streams(0); } catch (const std::exception&) {}
}
void EngineTransport::seek(Sample sample) {
    if (sample < 0 || sample > max_sample) throw std::invalid_argument("invalid seek");
    (void)timeline_.to_ticks(sample);
    engine_->prime_streams(sample);
    send({ControlKind::prepared_seek, sample});
}
void EngineTransport::set_loop(std::optional<LoopRange> loop) {
    if (loop && (loop->start < 0 || loop->end > max_sample || loop->start >= loop->end))
        throw std::invalid_argument("invalid loop");
    if (loop) { (void)timeline_.to_ticks(loop->start); (void)timeline_.to_ticks(loop->end); }
    engine_->prime_loop(loop);
    send({ControlKind::loop, loop ? loop->start : 0, loop ? loop->end : 0});
}
Connection EngineTransport::subscribe(std::function<void(const TransportState&)> callback) {
    return changes_.subscribe(std::move(callback));
}
void EngineTransport::poll() {
    auto next = state();
    if (next == last_) return;
    last_ = next;
    changes_.publish(last_);
}
} // namespace mrs::audio
