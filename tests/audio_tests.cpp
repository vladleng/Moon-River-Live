#include <mrs/audio.hpp>
#include <mrs/device.hpp>
#include <mrs/read_ahead.hpp>
#include <mrs/recording.hpp>
#include <mrs/waveform.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace allocation_check {
thread_local bool enabled = false;
std::atomic<std::uint64_t> count{};
}
void* operator new(std::size_t bytes) {
    if (allocation_check::enabled) allocation_check::count.fetch_add(1);
    if (auto p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace {
using namespace mrs;
using namespace mrs::audio;
int assertions = 0;
void check(bool value, const char* text, int line) {
    ++assertions;
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + text);
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
template<class F> void rejects(F action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    CHECK(failed);
}
std::shared_ptr<const AudioData> data() {
    return std::make_shared<const AudioData>(AudioData{48000, 2, {0.1F,0.2F, 0.3F,0.4F, 0.5F,0.6F, 0.7F,0.8F}});
}
RenderGraph basic() {
    return {{{data(), 0, 0, 4, {{0,0,1}, {1,1,1}}}}, {{0,0,0.5F}, {0,1,0.5F}}};
}
void render() {
    AudioEngine engine;
    engine.prepare({48000, 1, 2, 8}, basic());
    std::array<float, 8> out{};
    std::array<float, 4> in{0.1F,0.1F,0.1F,0.1F};
    engine.process(in.data(), out.data(), 4);
    CHECK(std::abs(out[0] - 0.05F) < 1e-6F); // monitoring while stopped
    CHECK(engine.state().sample == 0);
    CHECK(engine.enqueue({ControlKind::play}));
    const auto before_allocations = allocation_check::count.load();
    allocation_check::enabled = true;
    engine.process(in.data(), out.data(), 4);
    engine.observe(1000, 4, 0);
    allocation_check::enabled = false;
    CHECK(allocation_check::count.load() == before_allocations);
    CHECK(std::abs(out[0] - 0.15F) < 1e-6F && std::abs(out[7] - 0.85F) < 1e-6F);
    CHECK(engine.state().sample == 4);
    engine.process(in.data(), out.data(), 4); // past end -> silence + monitoring
    CHECK(std::abs(out[0] - 0.05F) < 1e-6F);
    auto graph = basic();
    graph.voices[0].start = 2; graph.voices[0].source_offset = 1; graph.voices[0].length = 2;
    graph.monitor.clear();
    engine.prepare({48000,0,2,8}, graph);
    CHECK(engine.enqueue({ControlKind::play}));
    engine.process(nullptr, out.data(), 4);
    CHECK(out[0] == 0 && out[3] == 0 && out[4] == 0.3F && out[7] == 0.6F);
    graph.voices[0].routes = {{0,0,16}, {1,1,16}};
    engine.prepare({48000,0,2,8}, graph);
    CHECK(engine.enqueue({ControlKind::play}));
    engine.process(nullptr, out.data(), 4);
    CHECK(out[4] == 1 && engine.metrics().clipped_samples > 0);
    graph.voices[0].routes[0].output_channel = 2;
    rejects([&] { engine.prepare({48000,0,2,8}, graph); });
    graph = basic();
    rejects([&] { engine.prepare({44100,1,2,8}, graph); });
    graph.voices[0].length = 5;
    rejects([&] { engine.prepare({48000,1,2,8}, graph); });
    graph = basic(); graph.monitor[0].input_channel = 1;
    rejects([&] { engine.prepare({48000,1,2,8}, graph); });
    graph = basic(); graph.voices[0].asset.reset();
    rejects([&] { engine.prepare({48000,1,2,8}, graph); });
    engine.prepare({48000,1,2,8}, basic());
    engine.process(nullptr, out.data(), 4);
    CHECK(engine.metrics().missing_inputs == 1);
    std::array<float, 18> oversized;
    oversized.fill(1);
    engine.process(nullptr, oversized.data(), 9);
    CHECK(engine.metrics().invalid_blocks == 1 && oversized.back() == 0);
    engine.process(nullptr, nullptr, 1);
    CHECK(engine.metrics().invalid_blocks == 2);
    AudioData nonfinite{48000,1,{std::numeric_limits<float>::quiet_NaN()}};
    rejects([&] { nonfinite.validate(); });
}
void transport() {
    auto engine = std::make_shared<AudioEngine>();
    engine->prepare({48000,1,2,8}, basic());
    auto service = std::make_shared<EngineTransport>(engine, Timeline{TimeMap{}, 48000});
    std::shared_ptr<ITransport> arrange = service, live = service;
    std::vector<TransportState> events;
    auto connection = live->subscribe([&](const auto& s) { events.push_back(s); });
    std::array<float, 8> output{};
    live->set_loop(LoopRange{0,4}); arrange->play();
    CHECK(live->state().playback == PlaybackState::stopped); // applied at callback boundary
    engine->process(nullptr, output.data(), 4); service->poll();
    CHECK(live->state().sample == 0 && events.size() == 1);
    CHECK(output[0] == 0.1F && output[7] == 0.8F);
    live->seek(3); engine->process(nullptr, output.data(), 4); service->poll();
    CHECK(arrange->state().sample == 3);
    CHECK(output[0] == 0.7F && output[2] == 0.1F);
    live->pause(); engine->process(nullptr, output.data(), 4); service->poll();
    CHECK(arrange->state().playback == PlaybackState::paused && arrange->state().sample == 3);
    CHECK(output[0] == 0);
    live->stop(); engine->process(nullptr, output.data(), 4); service->poll();
    CHECK(arrange->state().sample == 0 && arrange->state().loop.has_value());
    live->set_loop(std::nullopt); engine->process(nullptr, output.data(), 4); service->poll();
    CHECK(!arrange->state().loop);
    rejects([&] { live->seek(-1); });
    rejects([&] { live->set_loop(LoopRange{4,4}); });
    for (int i = 0; i < 63; ++i) live->play();
    rejects([&] { live->play(); }); // bounded backpressure
    engine->process(nullptr, output.data(), 4); service->poll();
    live->pause(); engine->process(nullptr, output.data(), 4);
    CHECK(live->state().playback == PlaybackState::paused);
}
void queue() {
    SpscQueue<std::uint64_t, 8> queue;
    for (std::uint64_t n = 0; n < 7; ++n) CHECK(queue.push(n));
    CHECK(!queue.push(8));
    std::uint64_t value = 0;
    for (std::uint64_t n = 0; n < 7; ++n) CHECK(queue.pop(value) && value == n);
    CHECK(!queue.pop(value));
    constexpr std::uint64_t total = 100000;
    std::atomic<bool> correct{true};
    std::thread consumer([&] {
        for (std::uint64_t n = 0; n < total; ++n) {
            std::uint64_t received = 0;
            while (!queue.pop(received)) std::this_thread::yield();
            if (received != n) correct = false;
        }
    });
    for (std::uint64_t n = 0; n < total; ++n)
        while (!queue.push(n)) std::this_thread::yield();
    consumer.join();
    CHECK(correct.load());
}
void put16(std::string& bytes, std::uint16_t word) {
    bytes.push_back(static_cast<char>(word & 255U)); bytes.push_back(static_cast<char>(word >> 8));
}
void put32(std::string& bytes, std::uint32_t word) {
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((word >> shift) & 255U));
}
std::string wav_bytes(std::uint16_t format, std::uint16_t bits, const std::string& payload) {
    std::string bytes = "RIFF"; put32(bytes, static_cast<std::uint32_t>(36 + payload.size()));
    bytes += "WAVEfmt "; put32(bytes,16); put16(bytes,format); put16(bytes,1); put32(bytes,48000);
    put32(bytes,48000 * (bits / 8)); put16(bytes,static_cast<std::uint16_t>(bits / 8)); put16(bytes,bits);
    bytes += "data"; put32(bytes,static_cast<std::uint32_t>(payload.size())); bytes += payload;
    return bytes;
}
struct TempFile {
    std::filesystem::path path;
    TempFile() {
        path = std::filesystem::temp_directory_path() / std::filesystem::path{u8"mrs-аудио-"};
        path += new_id().value + ".wav";
    }
    void write(const std::string& bytes) { std::ofstream out(path,std::ios::binary); out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); }
    ~TempFile() { std::error_code error; std::filesystem::remove(path,error); }
};
void wav() {
    TempFile file;
    std::string payload;
    put16(payload,0); put16(payload,32767); put16(payload,32768); put16(payload,16384);
    const auto original = wav_bytes(1,16,payload);
    file.write(original);
    const auto asset = load_wav(file.path);
    CHECK(asset.channels == 1 && asset.frames() == 4 && asset.sample_rate == 48000);
    CHECK(asset.samples[0] == 0 && asset.samples[2] == -1 && asset.samples[3] == 0.5F);
    rejects([&] { (void)load_wav(file.path,4); });
    payload.clear(); put32(payload, std::bit_cast<std::uint32_t>(0.25F)); put32(payload, std::bit_cast<std::uint32_t>(-0.5F));
    file.write(wav_bytes(3,32,payload));
    CHECK(load_wav(file.path).samples[1] == -0.5F);
    payload.clear(); put32(payload,0x80000000); put32(payload,0x40000000);
    file.write(wav_bytes(1,32,payload));
    CHECK(load_wav(file.path).samples[0] == -1 && load_wav(file.path).samples[1] == 0.5F);
    payload = std::string("\0\0\x80\0\0\x40",6);
    file.write(wav_bytes(1,24,payload));
    CHECK(load_wav(file.path).samples[0] == -1 && load_wav(file.path).samples[1] == 0.5F);
    file.write(original.substr(0,original.size()-1));
    rejects([&] { (void)load_wav(file.path); });
    auto bad = original; bad[0] = 'X'; file.write(bad);
    rejects([&] { (void)load_wav(file.path); });
    bad = original; bad[20] = 7; file.write(bad); // unknown format
    rejects([&] { (void)load_wav(file.path); });
    bad = original; bad[32] = 0; file.write(bad); // invalid block alignment
    rejects([&] { (void)load_wav(file.path); });
    payload.clear(); put32(payload,std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()));
    file.write(wav_bytes(3,32,payload));
    rejects([&] { (void)load_wav(file.path); });
    rejects([&] { (void)load_wav(file.path.string()+".missing"); });
}
void streaming() {
    TempFile file; std::string payload;
    constexpr Sample total = 160123;
    for (Sample f = 0; f < total; ++f) put32(payload,std::bit_cast<std::uint32_t>(static_cast<float>(f%127)/256.0F));
    file.write(wav_bytes(3,32,payload));
    auto asset = std::make_shared<const AudioData>(open_wav(file.path,0));
    CHECK(asset->file && asset->samples.empty() && asset->frames() == total);
    CHECK(open_wav(file.path).samples.size() == static_cast<std::size_t>(total));
    std::array<float,7> decoded{}; asset->file->read(8190,decoded);
    for (std::size_t n = 0; n < decoded.size(); ++n) CHECK(decoded[n] == static_cast<float>((8190+static_cast<Sample>(n))%127)/256.0F);
    rejects([&] { asset->file->read(total-1,decoded); });
    const Waveform peaks{*asset};
    const auto peak = peaks.range(0,total,0);
    CHECK(peak.minimum == 0 && peak.maximum == 126.0F/256.0F);
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    rejects([&] { (void)Waveform(*asset,cancel); });
    auto engine = std::make_shared<AudioEngine>();
    RenderGraph graph; graph.voices.push_back({asset,11,8188,100000,{{0,0,1}}});
    engine->prepare({48000,0,1,128},graph);
    EngineTransport transport{engine,Timeline{TimeMap{},48000}};
    std::array<float,128> output{}; transport.play();
    for (int b = 0; b < 300; ++b) {
        const auto before = allocation_check::count.load(); allocation_check::enabled = true;
        engine->process(nullptr,output.data(),128); allocation_check::enabled = false;
        CHECK(allocation_check::count.load() == before);
        for (Sample f = 0; f < 128; ++f) {
            const auto time = static_cast<Sample>(b)*128+f;
            CHECK(output[static_cast<std::size_t>(f)] == (time < 11 ? 0.0F : static_cast<float>((time-11+8188)%127)/256.0F));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(engine->metrics().disk_underruns == 0 && engine->metrics().disk_errors == 0);
    transport.pause(); engine->process(nullptr,output.data(),128);
    transport.seek(90000); engine->process(nullptr,output.data(),128);
    CHECK(engine->state().playback == PlaybackState::paused && engine->state().sample == 90000 && output[0] == 0);
    transport.set_loop(LoopRange{20,90008}); transport.play();
    engine->process(nullptr,output.data(),128);
    for (Sample f = 0; f < 128; ++f) {
        const auto time = f < 8 ? 90000+f : 20+f-8;
        CHECK(output[static_cast<std::size_t>(f)] == static_cast<float>((time-11+8188)%127)/256.0F);
    }
    CHECK(engine->metrics().disk_underruns == 0);
    transport.seek(60000); engine->process(nullptr,output.data(),128);
    for (Sample f = 0; f < 128; ++f) CHECK(output[static_cast<std::size_t>(f)] == static_cast<float>((60000+f-11+8188)%127)/256.0F);
    CHECK(engine->metrics().disk_underruns == 0);
    graph.voices = {{asset,0,8190,6,{{0,0,1}}},{asset,6,8196,10,{{0,0,1}}}};
    engine->prepare({48000,0,1,128},graph); transport.play(); engine->process(nullptr,output.data(),128);
    for (Sample f = 0; f < 16; ++f) CHECK(output[static_cast<std::size_t>(f)] == static_cast<float>((8190+f)%127)/256.0F);
    CHECK(output[16] == 0);
    graph.voices = {{asset,100,total-3,3,{{0,0,1}}}};
    engine->prepare({48000,0,1,128},graph); transport.play(); engine->process(nullptr,output.data(),128);
    CHECK(output[99] == 0 && output[103] == 0);
    for (Sample f = 0; f < 3; ++f) CHECK(output[static_cast<std::size_t>(100+f)] == static_cast<float>((total-3+f)%127)/256.0F);
    graph.voices = {{asset,0,0,total,{{0,0,1}}}};
    engine->prepare({48000,0,1,128},graph);
    transport.play();
    std::atomic<bool> running{true}, correct{true};
    Sample mismatch_frame{}; float mismatch_actual{}, mismatch_expected{};
    std::thread callback([&] {
        std::array<float,128> block{};
        while (running.load()) {
            allocation_check::enabled = true;
            engine->process(nullptr,block.data(),128);
            allocation_check::enabled = false;
            const auto first = engine->state().sample-128;
            for (Sample f = 0; f < 128; ++f)
                if (const auto expected = first+f < total ? static_cast<float>((first+f)%127)/256.0F : 0.0F; block[static_cast<std::size_t>(f)] != expected && correct.exchange(false)) {
                    mismatch_frame = first+f; mismatch_actual = block[static_cast<std::size_t>(f)]; mismatch_expected = expected;
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    try {
        for (int cycle=0; cycle<3; ++cycle) for (const auto target : {120000,50000,130000,1000,140000,70000}) {
            transport.seek(target);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } catch (...) { running = false; callback.join(); throw; }
    running = false; callback.join();
    if (!correct.load()) std::cerr << "stream mismatch: frame=" << mismatch_frame << " actual=" << mismatch_actual << " expected=" << mismatch_expected << " underruns=" << engine->metrics().disk_underruns << '\n';
    CHECK(correct.load()); CHECK(engine->metrics().disk_underruns == 0); CHECK(allocation_check::count.load() == 0);
    // With the callback quiescent, queued UI seeks coalesce to the last ready
    // target. Do not allow a retry implementation to silently ignore seeks.
    for (const auto target : {120000,50000,130000,1000,140000,70000}) transport.seek(target);
    engine->process(nullptr,output.data(),128);
    CHECK(engine->state().sample == 70128);
    for (Sample f=0; f<128; ++f) CHECK(output[static_cast<std::size_t>(f)] == static_cast<float>((70000+f)%127)/256.0F);
    CHECK(engine->metrics().disk_underruns == 0);
    transport.seek(1000); transport.stop(); engine->process(nullptr,output.data(),128);
    CHECK(engine->state().sample == 0 && engine->state().playback == PlaybackState::stopped);
    std::filesystem::remove(file.path);
    rejects([&] { transport.seek(40000); });
    CHECK(engine->enqueue({ControlKind::seek,40000})); CHECK(engine->enqueue({ControlKind::play}));
    engine->process(nullptr,output.data(),128);
    CHECK(std::all_of(output.begin(),output.end(),[](float v) { return v == 0; }));
    CHECK(engine->metrics().disk_underruns == 1 && engine->state().sample == 40128);
    rejects([&] { engine->prepare({48000,0,1,128},graph); });
}


void recording() {
    TempFile file;
    auto recorder = std::make_shared<Recorder>(file.path,48000,100);
    auto engine = std::make_shared<AudioEngine>();
    auto graph = basic(); graph.voices.front().start = 100; graph.recording = recorder;
    engine->prepare({48000,1,2,8},graph,{PlaybackState::paused,100,{}});
    EngineTransport transport{engine,Timeline{TimeMap{},48000}};
    std::array<float,4> input{0.25f,-0.5f,0.75f,0.125f};
    std::array<float,8> output{};
    engine->process(input.data(),output.data(),4);
    CHECK(recorder->status().frames == 0 && output[0] == 0.125f && engine->metrics().input_peak == 0.75f);
    transport.play();
    const auto before = allocation_check::count.load();
    allocation_check::enabled = true;
    engine->process(input.data(),output.data(),4);
    allocation_check::enabled = false;
    CHECK(allocation_check::count.load() == before);
    CHECK(std::abs(output[0]-0.225f) < 1e-6f && recorder->status().frames == 4);
    CHECK(engine->enqueue({ControlKind::monitor,0}));
    engine->process(input.data(),output.data(),4);
    CHECK(std::all_of(output.begin(),output.end(),[](float value) { return value == 0; }));
    CHECK(recorder->status().frames == 8);
    CHECK(engine->enqueue({ControlKind::pause}));
    engine->process(input.data(),output.data(),4);
    CHECK(recorder->status().frames == 8);
    graph.recording.reset();
    engine->prepare({48000,1,2,8},graph);
    auto result = recorder->finish();
    CHECK(result.frames == 8 && result.status.fault == RecordFault::none && !result.path.empty());
    const auto data = load_wav(file.path);
    CHECK(data.channels == 1 && data.sample_rate == 48000 && data.frames() == 8);
    for (std::size_t n=0; n<data.samples.size(); ++n) CHECK(data.samples[n] == input[n%4]); // raw, not monitor/backing mix
    rejects([&] { (void)recorder->finish(); });
    rejects([&] { Recorder existing(file.path,48000,0); });
    CHECK(load_wav(file.path).samples == data.samples);
    for (const auto flag : {1U,2U}) {
        TempFile prefix; auto take = std::make_shared<Recorder>(prefix.path,48000,0);
        RenderGraph capture; capture.recording = take;
        engine->prepare({48000,1,2,8},capture); transport.play();
        engine->process(input.data(),output.data(),4);
        engine->process(input.data(),output.data(),4,flag);
        CHECK(take->status().frames == 4 && take->status().missing_blocks == 1 && take->status().fault == RecordFault::missing_input);
        engine->process(input.data(),output.data(),4);
        engine->prepare({48000,0,2,8},{});
        CHECK(take->finish().frames == 4 && load_wav(prefix.path).frames() == 4);
    }

    for (const auto change : {Control{ControlKind::seek,50},Control{ControlKind::loop,0,100}}) {
        TempFile jump; auto take = std::make_shared<Recorder>(jump.path,48000,0);
        RenderGraph capture; capture.recording = take;
        engine->prepare({48000,1,2,8},capture); transport.play();
        engine->process(input.data(),output.data(),4);
        CHECK(engine->enqueue(change)); engine->process(input.data(),output.data(),4);
        CHECK(take->status().fault == RecordFault::discontinuity && take->status().discontinuities == 1);
        CHECK(engine->state().sample == 8 && !engine->state().loop);
        engine->prepare({48000,0,2,8},{});
        CHECK(take->finish().frames == 4);
    }
    {
        TempFile missing; Recorder take(missing.path,48000,0);
        take.capture(input.data(),1,4,0); take.capture(nullptr,1,4,4); take.capture(input.data(),1,4,8);
        CHECK(take.status().fault == RecordFault::missing_input && take.status().frames == 4);
        CHECK(take.finish().frames == 4);
    }
    {
        TempFile jump; Recorder take(jump.path,48000,0);
        take.capture(input.data(),1,4,0); take.capture(input.data(),1,4,10);
        CHECK(take.status().fault == RecordFault::discontinuity && take.status().discontinuities == 1);
        CHECK(take.finish().frames == 4);
    }
    {
        TempFile over; Recorder take(over.path,48000,0);
        take.capture(input.data(),1,static_cast<std::uint32_t>(Recorder::capacity+1),0);
        CHECK(take.status().fault == RecordFault::overflow && take.status().dropped_blocks == 1);
        CHECK(take.finish().frames == 0 && !std::filesystem::exists(over.path));
    }
    {
        TempFile finite; Recorder take(finite.path,48000,0);
        std::array<float,4> samples{std::numeric_limits<float>::infinity(),0.25f,std::numeric_limits<float>::quiet_NaN(),-0.5f};
        take.capture(samples.data(),1,4,0);
        CHECK(take.finish().status.nonfinite_samples == 2);
        CHECK(load_wav(finite.path).samples == std::vector<float>({0,0.25f,0,-0.5f}));
    }
    {
        TempFile limit; Recorder take(limit.path,48000,max_sample-2);
        take.capture(input.data(),1,4,max_sample-2);
        CHECK(take.status().fault == RecordFault::size_limit && take.finish().frames == 0);
    }
    {
        // More than one ring revolution: drain concurrently, preserve order.
        TempFile wrap; Recorder take(wrap.path,48000,0);
        std::array<float,8192> block{};
        Sample position{};
        for (int n=0; n<40; ++n) {
            std::fill(block.begin(),block.end(),static_cast<float>(n)/64);
            take.capture(block.data(),1,static_cast<std::uint32_t>(block.size()),position);
            position += static_cast<Sample>(block.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto saved = take.finish(); CHECK(saved.status.fault == RecordFault::none && saved.frames == position);
        const auto roundtrip = load_wav(wrap.path);
        for (int n=0; n<40; ++n) {
            CHECK(roundtrip.samples[static_cast<std::size_t>(n)*8192] == static_cast<float>(n)/64);
            CHECK(roundtrip.samples[static_cast<std::size_t>(n+1)*8192-1] == static_cast<float>(n)/64);
        }
    }
}

void device() {
    DeviceInfo d{5,"Vendor",{"Input 1","Input 2"},{"Out 1","Out 2"},32,1024,128,-1};
    CHECK(supports_buffer(d,32) && supports_buffer(d,64) && supports_buffer(d,128));
    CHECK(!supports_buffer(d,96) && !supports_buffer(d,0) && !supports_buffer(d,2048));
    DeviceConfig c; c.device = 5; c.inputs = {0};
    validate_device_config(d,c);
    c.inputs = {0,0}; rejects([&] { validate_device_config(d,c); });
    c.inputs = {2}; rejects([&] { validate_device_config(d,c); });
    c.inputs.clear(); c.outputs = {2}; rejects([&] { validate_device_config(d,c); });
    c.outputs.clear(); rejects([&] { validate_device_config(d,c); });
    c.outputs = {0,1}; c.buffer_frames = 96; rejects([&] { validate_device_config(d,c); });
    d.granularity = 32; CHECK(supports_buffer(d,96) && !supports_buffer(d,100));
    d.granularity = 0; CHECK(supports_buffer(d,128) && !supports_buffer(d,64));
    c.buffer_frames = 128; c.device = 6; rejects([&] { validate_device_config(d,c); });
}
void metrics() {
    AudioEngine engine;
    std::array<float,256> output{};
    for (int i=0; i<3; ++i) engine.process(nullptr,output.data(),128);
    engine.observe(100000,128,0);
    engine.observe(200000,128,2|4);
    engine.observe(3000000,128,0);
    const auto m = engine.metrics();
    CHECK(m.callbacks == 3 && m.measured_callbacks == 3 && m.max_callback_ns == 3000000);
    CHECK(m.input_overflows == 1 && m.output_underflows == 1 && m.deadline_misses == 1);
    CHECK(m.min_frames == 128 && m.max_frames == 128);
    CHECK(m.p50_load_percent == 8 && m.p99_load_percent == 100);
}
void independence() {
    auto engine = std::make_shared<AudioEngine>();
    engine->prepare({48000,1,2,128}, basic());
    EngineTransport control{engine, Timeline{TimeMap{},48000}};
    control.set_loop(LoopRange{0,4}); control.play();
    std::atomic<bool> run{true};
    std::thread audio([&] {
        std::array<float,256> output{};
        const auto before = allocation_check::count.load();
        allocation_check::enabled = true;
        while (run.load()) engine->process(nullptr,output.data(),128);
        allocation_check::enabled = false;
        if (allocation_check::count.load() != before) run = false;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto before = engine->metrics().callbacks;
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // frozen UI/control pump
    const auto after = engine->metrics().callbacks;
    run = false; audio.join();
    CHECK(after > before);
    CHECK(allocation_check::count.load() == 0);
    control.poll();
    CHECK(control.state().playback == PlaybackState::playing);
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("expected suite");
        const std::string suite = argv[1];
        if (suite=="render") render(); else if (suite=="transport") transport();
        else if (suite=="queue") queue(); else if (suite=="wav") wav();
        else if (suite=="device") device(); else if (suite=="metrics") metrics();
        else if (suite=="streaming") streaming(); else if (suite=="recording") recording();
        else if (suite=="independence") independence(); else throw std::invalid_argument("unknown suite");
        std::cout << "PASS " << suite << ": " << assertions << " checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
