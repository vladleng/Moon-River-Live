#include <mrs/desktop.hpp>
#include <mrs/offline_device.hpp>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
using namespace mrs; using namespace mrs::desktop;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)),#__VA_ARGS__)
template<class F> void rejects(F f) { bool bad{}; try { f(); } catch (const std::exception&) { bad = true; } CHECK(bad); }
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path()/("mrs-shell-"+new_id().value);
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path,error); }
};
template<class F> void eventually(F f) {
    auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (std::chrono::steady_clock::now()<end) {
        try { if (f()) return; } catch (const std::runtime_error&) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("timed out");
}
void workspaces() {
    Application app; auto services = app.services(); auto graph = app.graphs(); auto engine = app.engine();
    for (auto w : {Workspace::arrange,Workspace::mix,Workspace::edit,Workspace::live}) {
        app.workspace(w); CHECK(app.workspace() == w && app.services().projects == services.projects &&
            app.services().transport == services.transport && app.graphs() == graph && app.engine() == engine);
    }
    app.rename_track(services.projects->state().project->tracks.front().id,"From Arrange");
    CHECK(app.musical().state().project->tracks.front().name == "From Arrange");
    CHECK(services.projects->undo() && app.musical().state().project->tracks.front().name == "Demo tone");
    CHECK(services.projects->redo()); rejects([&] { app.workspace(static_cast<Workspace>(10)); });
}
void transport() {
    Application app; app.play();
    // Do not pump the application/UI: audio callback must continue independently.
    eventually([&] { return app.engine()->state().sample >= 1024; });
    app.poll(); CHECK(app.musical().state().transport.playback == PlaybackState::playing);
    app.pause(); eventually([&] { return app.engine()->state().playback == PlaybackState::paused; });
    const auto sample = app.engine()->state().sample; std::this_thread::sleep_for(std::chrono::milliseconds(10)); CHECK(app.engine()->state().sample == sample);
    app.musical().seek_section({"section-verse"}); eventually([&] { return app.engine()->state().sample > sample; });
    app.poll(); CHECK(app.musical().state().current_section->name == "Verse");
    app.musical().loop_section({"section-verse"}); eventually([&] { return app.engine()->state().loop.has_value(); });
    app.stop(); eventually([&] { return app.engine()->state().sample == 0 && app.engine()->state().playback == PlaybackState::stopped; });
    app.poll(); CHECK(app.musical().state().transport.sample == 0);
    rejects([&] { app.seek(-1); });
}
void files() {
    Directory dir; Application app; auto path = dir.path / std::filesystem::path(std::u8string(u8"Песня.mrsproject"));
    auto id = app.services().projects->state().project->id; app.save_project(path); CHECK(!app.dirty());
    app.rename_track(app.services().projects->state().project->tracks.front().id,"Saved"); CHECK(app.dirty()); app.save_project(path);
    auto graph = app.graphs()->state().graph; app.workspace(Workspace::live); app.open_project(path);
    CHECK(app.services().projects->state().project->id == id && app.services().projects->state().project->tracks.front().name == "Saved");
    CHECK(app.graphs()->state().graph->id == graph->id && app.workspace() == Workspace::live && !app.dirty());
    CHECK(app.services().transport->state().playback == PlaybackState::stopped);
    auto snapshot = app.snapshot(); snapshot.extensions = {{"FUTR","opaque"}};
    auto second = dir.path / "future.mrsproject"; persistence::save_project(second,snapshot);
    app.open_project(second); app.rename_track(app.services().projects->state().project->tracks.front().id,"Future"); app.save_project(second);
    CHECK(persistence::load_project(second).extensions == snapshot.extensions);
    auto before = app.services().projects; rejects([&] { app.open_project(dir.path / "missing"); }); CHECK(app.services().projects == before);
    { std::ofstream out(dir.path / "broken"); out << "bad"; }
    rejects([&] { app.open_project(dir.path / "broken"); }); CHECK(app.services().projects == before);
    app.demo(); CHECK(app.dirty() && app.path().empty());
}
void wav(const std::filesystem::path& path, std::uint16_t channels = 2) {
    // Exact PCM16 stereo RIFF fixture at a non-default sample rate.
    std::ofstream f(path,std::ios::binary);
    auto u16 = [&](std::uint16_t value) { for (int i = 0; i < 2; ++i) f.put(static_cast<char>((value >> (8*i)) & 255)); };
    auto u32 = [&](std::uint32_t value) { for (int i = 0; i < 4; ++i) f.put(static_cast<char>((value >> (8*i)) & 255)); };
    f.write("RIFF",4); u32(36+4000); f.write("WAVEfmt ",8); u32(16); u16(1); u16(channels); u32(44100); u32(44100*channels*2); u16(static_cast<std::uint16_t>(channels*2)); u16(16);
    f.write("data",4); u32(4000); for (int i = 0; i < 2000; ++i) u16(4000);
}
class ManualDevice final : public audio::IAudioDevice {
public:
    std::vector<audio::DeviceInfo> enumerate() override { return {{0,"Manual render test",{"Mic"},{"L","R","Cue 1","Cue 2"},32,2048,128,-1}}; }
    void control_panel(int) override {}
    void open(const audio::DeviceConfig&,std::shared_ptr<audio::AudioEngine>) override { phase_ = audio::DevicePhase::open; }
    void start() override { phase_ = audio::DevicePhase::running; }
    void stop() override { if (fail_stop_) throw std::runtime_error("test driver stop failure"); phase_ = audio::DevicePhase::stopped; }
    void fail_driver() { phase_ = audio::DevicePhase::error; fail_stop_ = true; }
    void close() noexcept override { phase_ = audio::DevicePhase::closed; }
    audio::DeviceStatus status() override { return {phase_,44100,0,0,0,{}}; }
private:
    audio::DevicePhase phase_{audio::DevicePhase::closed};
    bool fail_stop_{};
};
void assets() {
    Directory dir; auto file = dir.path / "music.wav"; wav(file); Application app; app.import_wav(file);
    const auto p = app.services().projects->state().project;
    CHECK(p->sample_rate == 44100 && p->clips.front().length == 1000 && p->chords.empty() && p->sections.empty());
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    app.play(); std::array<float,256> output{};
    app.engine()->process(nullptr,output.data(),128); // manual device has no concurrent consumer
    CHECK(output[0] == 0.06103515625f && output[1] == 0.06103515625f); // PCM16 4000/32768 through shared native gain 0.5
    app.poll(); CHECK(app.musical().state().transport.sample == 128);
    app.disconnect(); CHECK(app.engine()->state().sample == 0);
    auto services = app.services(); rejects([&] { app.import_wav(dir.path / "absent.wav"); }); CHECK(app.services().projects == services.projects);
}
void mono_route() {
    Directory dir; const auto file = dir.path / "mono.wav"; wav(file,1);
    Application app; app.import_wav(file);
    CHECK(audio::inspect_wav(file).channels == 1);
    for (const auto& outputs : {std::vector<int>{0},std::vector<int>{0,1},std::vector<int>{0,1,2,3}}) {
        app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{},outputs});
        app.play(); std::array<float,512> buffer{};
        app.engine()->process(nullptr,buffer.data(),128);
        for (std::size_t frame = 0; frame < 128; ++frame) for (std::size_t channel = 0; channel < outputs.size(); ++channel)
            CHECK(buffer[frame*outputs.size()+channel] == (channel < 2 ? 0.06103515625f : 0.0f));
    }
}

void audio_settings() {
    Application app;
    app.connect(audio::make_offline_device(),{0,48000,128,{}, {0,1}}); CHECK(app.audio_running());
    app.connect(audio::make_offline_device(),{0,48000,64,{0}, {0,1}}); CHECK(app.audio_running() && app.engine()->config().input_channels == 1);
    rejects([&] { app.connect(audio::make_offline_device(),{0,48000,63,{}, {0,1}}); }); CHECK(app.audio_running()); // validate before closing
    rejects([&] { app.connect(audio::make_offline_device(),{0,44100,128,{}, {0,1}}); }); CHECK(!app.audio_running());
    rejects([&] { app.play(); });
    app.connect(audio::make_offline_device(),{0,48000,128,{}, {0,1}}); CHECK(app.audio_running());
    app.disconnect(); CHECK(!app.audio_running() && app.engine()->state().sample == 0);
}
void arrangement() {
    Directory dir; auto a = dir.path / "one.wav", b = dir.path / "two.wav"; wav(a); wav(b);
    Application app; app.new_project(44100);
    const auto service = app.services().projects; const auto graph = app.graphs(); const auto id = service->state().project->id;
    app.import_wavs({a,b});
    CHECK(service == app.services().projects && graph == app.graphs());
    CHECK(service->state().project->id == id && service->state().project->tracks.size() == 2);
    CHECK(service->state().project->clips.size() == 2);
    CHECK(app.undo() && service->state().project->tracks.empty());
    CHECK(app.redo() && service->state().project->tracks.size() == 2);
    auto before = *service->state().project;
    rejects([&] { app.import_wavs({a,dir.path / "missing.wav"}); });
    CHECK(*service->state().project == before);
    app.reorder_track(before.tracks.front().id,1);
    CHECK(service->state().project->tracks.back().id == before.tracks.front().id);
    app.remove_track(before.tracks.front().id);
    CHECK(service->state().project->tracks.size() == 1 && service->state().project->clips.size() == 1);
    auto saved = dir.path / "arrange.mrsproject"; app.save_project(saved);
    CHECK(persistence::load_project(saved).project.tracks.size() == 1);
    CHECK(app.undo() && service->state().project->tracks.size() == 2);
    CHECK(app.snapshot().mixer.size() == 2);
    auto third = app.add_audio_track("Empty"); CHECK(service->state().project->tracks.back().id == third);
    app.prepare_waveforms();
    const auto source = service->state().project->clips.front().source;
    eventually([&] { app.poll(); return app.waveform(source) != nullptr; });
    CHECK(app.waveform(source)->frames() == 1000);
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    app.remove_track(third); // same opened backend, rebuilt renderer
    CHECK(app.audio_running());
    app.play(); std::array<float,256> output{}; app.engine()->process(nullptr,output.data(),128);
    CHECK(output[0] == 0.1220703125f); // BOTH imported tracks, shared gain 0.5
    rejects([&] { app.add_audio_track("While playing"); });
    rejects([&] { app.undo(); });
    app.stop(); app.engine()->process(nullptr,output.data(),128); app.poll();
    app.save_project(saved); auto snapshot = app.snapshot(); app.open_project(saved);
    CHECK(app.snapshot().project == snapshot.project);
    Application other; auto p = *other.services().projects->state().project;
    rejects([&] { other.import_wavs({a}); }); CHECK(*other.services().projects->state().project == p); // rate mismatch
}
void nonplaying_edits() {
    Directory dir; auto file = dir.path / "edit.wav"; wav(file);
    Application app; app.new_project(44100);
    app.import_wavs({file});
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    std::array<float,256> output{};
    // Seeking away from zero while stopped must not prevent edits or reset position.
    app.seek(400); app.engine()->process(nullptr,output.data(),128);
    const auto empty = app.add_audio_track("Remove while stopped");
    CHECK(app.engine()->state().sample == 400 && app.engine()->state().playback == PlaybackState::stopped);
    app.remove_track(empty);
    CHECK(app.engine()->state().sample == 400);
    CHECK(app.undo() && app.engine()->state().sample == 400);
    CHECK(app.redo() && app.engine()->state().sample == 400);
    // Pause is an idle transport at a useful edit position, not a playing session.
    app.play(); app.engine()->process(nullptr,output.data(),128); app.pause();
    app.engine()->process(nullptr,output.data(),128);
    const auto paused = app.engine()->state(); CHECK(paused.playback == PlaybackState::paused && paused.sample == 528);
    auto track = app.services().projects->state().project->tracks.front().id;
    app.remove_track(track); CHECK(app.services().projects->state().project->clips.empty());
    CHECK(app.audio_running() && app.engine()->state().sample == paused.sample && app.engine()->state().playback == PlaybackState::paused);
    CHECK(app.undo() && app.services().projects->state().project->clips.size() == 1);
    CHECK(app.engine()->state().sample == paused.sample && app.engine()->state().playback == PlaybackState::paused);
    app.import_wavs({file}); CHECK(app.engine()->state().sample == paused.sample && app.engine()->state().playback == PlaybackState::paused);
    app.engine()->process(nullptr,output.data(),128);
    CHECK(output[0] == 0 && app.engine()->state().sample == paused.sample); // no accidental restart
    app.play(); app.engine()->process(nullptr,output.data(),128);
    CHECK(output[0] == 0.1220703125f && app.engine()->state().sample == paused.sample+128);
    rejects([&] { app.remove_track(track); }); // actual playback remains protected
    app.disconnect();
    // Invalid quiescent state is rejected atomically; prepare never restores Play.
    audio::AudioEngine engine;
    engine.prepare({}, {}, {PlaybackState::paused,123,LoopRange{100,400}});
    CHECK(engine.state().sample == 123 && engine.state().loop == std::optional<LoopRange>{{100,400}});
    rejects([&] { engine.prepare({}, {}, {PlaybackState::playing,100,{}}); });
    rejects([&] { engine.prepare({}, {}, {PlaybackState::stopped,-1,{}}); });
    rejects([&] { engine.prepare({}, {}, {PlaybackState::stopped,0,LoopRange{400,100}}); });
    CHECK(engine.state().sample == 123 && engine.state().playback == PlaybackState::paused);
}
void clip_edits() {
    Directory dir; auto file = dir.path / "source.wav"; wav(file);
    Application app; app.new_project(44100); app.import_wavs({file});
    const auto projects = app.services().projects;
    const auto original = projects->state().project->clips.front();
    const auto destination = app.add_audio_track("Destination");
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    app.seek(700); std::array<float,256> output{}; app.engine()->process(nullptr,output.data(),128);
    app.move_clip(original.id,destination,200);
    auto clip = projects->state().project->clips.front();
    CHECK(clip.start == 200 && clip.track == destination && clip.source == original.source && clip.source_offset == 0);
    CHECK(app.engine()->state().sample == 700);
    app.trim_clip(original.id,300,1000);
    clip = projects->state().project->clips.front();
    CHECK(clip.start == 300 && clip.length == 700 && clip.source_offset == 100);
    auto right = app.split_clip(original.id,600);
    const auto split = projects->state().project;
    CHECK(split->clips.size() == 2 && split->clips[0].length == 300);
    CHECK(split->clips[1].id == right && split->clips[1].start == 600 && split->clips[1].length == 400 && split->clips[1].source_offset == 400);
    CHECK(split->clips[1].source == original.source && split->clips[1].track == destination);
    // Boundary playback: no gap/double amplitude at a split, silence outside trim.
    app.seek(599); app.play(); app.engine()->process(nullptr,output.data(),128);
    for (auto value : output) CHECK(value == 0.06103515625f);
    app.pause(); app.engine()->process(nullptr,output.data(),128);
    const auto position = app.engine()->state().sample;
    app.remove_clip(right); CHECK(projects->state().project->clips.size() == 1);
    CHECK(app.engine()->state().playback == PlaybackState::paused && app.engine()->state().sample == position);
    CHECK(app.undo() && projects->state().project->clips.size() == 2);
    CHECK(app.undo() && projects->state().project->clips.size() == 1 && projects->state().project->clips.front().length == 700);
    CHECK(app.redo() && projects->state().project->clips.back().id == right);
    auto before = *projects->state().project;
    for (auto sample : {Sample{300},Sample{600},Sample{-1}}) rejects([&] { app.split_clip(original.id,sample); });
    rejects([&] { app.trim_clip(original.id,0,1000); });
    rejects([&] { app.trim_clip(original.id,300,5000); });
    rejects([&] { app.move_clip(original.id,{"missing"},0); });
    CHECK(*projects->state().project == before);
    app.trim_clip(right,600,1200); // recover hidden source at right end
    CHECK(projects->state().project->clips.back().length == 600 && projects->state().project->clips.back().source_offset == 400);
    app.seek(1199); app.play(); app.engine()->process(nullptr,output.data(),128);
    CHECK(output[0] == 0.06103515625f && output[2] == 0);
    app.pause(); app.engine()->process(nullptr,output.data(),128);
    app.save_project(dir.path / "clips.mrsproject"); auto saved = app.snapshot();
    app.open_project(dir.path / "clips.mrsproject"); CHECK(app.snapshot().project == saved.project);
    CHECK(audio::load_wav(file).frames() == 1000);
    Project p = saved.project; p.time = TimeMap{}; // explicit 120 BPM, not demo's 72 BPM
    CHECK(snap_to_grid(p,5513) == 5513);
    CHECK(snap_to_grid(p,5000) == 5513);
    p.time.tempos = {{0,60},{ppq,120}};
    CHECK(snap_to_grid(p,10000) == 11025);
    CHECK(snap_to_grid(p,49000) == 49613);
}
void waveform() {
    audio::AudioData data{48000,2,std::vector<float>(2050,0)};
    data.samples[600] = 0.75f; data.samples[601] = -0.9f;
    data.samples[2048] = -0.4f; data.samples[2049] = 0.3f;
    audio::Waveform peaks(data);
    CHECK(peaks.channels() == 2 && peaks.frames() == 1025);
    auto l = peaks.range(0,1025,0), r = peaks.range(0,1025,1);
    CHECK(l.maximum == 0.75f && l.minimum == -0.4f);
    CHECK(r.minimum == -0.9f && r.maximum == 0.3f);
    CHECK(peaks.range(1024,1025,0).minimum == -0.4f);
    CHECK(peaks.range(0,0,0).maximum == 0);
    CHECK(peaks.range(-10,99999,1).minimum == -0.9f);
    rejects([&] { (void)peaks.range(0,1,2); });
}
void streaming() {
    Directory dir; const auto file = dir.path / "long.wav";
    constexpr std::uint32_t frames = 40'000'003;
    {
        std::ofstream out(file,std::ios::binary);
        const auto u16 = [&](std::uint16_t n) { for (int i=0; i<2; ++i) out.put(static_cast<char>((n>>(8*i))&255)); };
        const auto u32 = [&](std::uint32_t n) { for (int i=0; i<4; ++i) out.put(static_cast<char>((n>>(8*i))&255)); };
        out.write("RIFF",4); u32(36+frames*4); out.write("WAVEfmt ",8); u32(16);
        u16(1); u16(2); u32(44100); u32(176400); u16(4); u16(16);
        out.write("data",4); u32(frames*4);
        out.seekp(static_cast<std::streamoff>(44)+frames*4-1); out.put(0);
    }
    const auto source = audio::open_wav(file);
    CHECK(source.file && source.samples.empty() && source.frames() == frames);
    rejects([&] { (void)audio::load_wav(file); }); // exceeds old decoded preload cap
    Application app; app.new_project(44100);
    const auto revision = app.services().projects->state().revision;
    rejects([&] { app.import_wavs(std::vector<std::filesystem::path>(33,file)); });
    CHECK(app.services().projects->state().revision == revision && app.services().projects->state().project->clips.empty());
    app.import_wavs({file});
    const auto p = app.services().projects->state().project;
    CHECK(p->clips.size() == 1 && p->clips.front().length == frames);
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    app.seek(frames-1000);
    std::array<float,256> out{};
    app.engine()->process(nullptr,out.data(),128);
    app.play(); app.engine()->process(nullptr,out.data(),128);
    app.pause(); app.engine()->process(nullptr,out.data(),128);
    const auto position = app.engine()->state().sample;
    CHECK(position == frames-872);
    const auto clip = p->clips.front().id;
    app.trim_clip(clip,100,frames-100);
    CHECK(app.engine()->state().sample == position && app.engine()->state().playback == PlaybackState::paused);
    CHECK(app.undo() && app.source_frames(clip) == frames);
    const auto saved = dir.path / "long.mrsproject"; app.save_project(saved); app.open_project(saved);
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    CHECK(app.services().projects->state().project->clips.front().length == frames);
    CHECK(app.engine()->metrics().disk_underruns == 0);
    app.new_project(); // cancels the long waveform scan before clearing retained media
    CHECK(app.services().projects->state().project->clips.empty());
}


void recording() {
    Directory dir; Application app; app.new_project(44100);
    const auto backing = dir.path/"Backing.wav"; wav(backing);
    app.import_wavs({backing});
    const auto armed = app.add_audio_track("Vocal");
    const auto project_file = dir.path/std::filesystem::path(std::u8string(u8"Запись.mrsproject"));
    app.save_project(project_file);
    rejects([&] { app.start_recording(dir.path/"No-arm.wav"); });
    app.arm_track(armed);
    rejects([&] { app.start_recording(dir.path/"Offline.wav"); });
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{0},{0,1,2,3}});
    std::array<float,128> input{}; input.fill(0.25f);
    std::array<float,512> output{};
    app.seek(500); app.engine()->process(input.data(),output.data(),128);
    CHECK(output[0] == 0.125f && output[1] == 0.125f && output[2] == 0 && output[3] == 0);
    const auto project = app.services().projects;
    const auto revision = project->state().revision;
    const auto take = dir.path/"Take.wav";
    app.start_recording(take); CHECK(app.recording());
    app.engine()->process(input.data(),output.data(),128);
    CHECK(output[0] == 0.18603515625f && output[1] == 0.18603515625f); // backing + monitor through gain
    app.monitoring(false); app.engine()->process(input.data(),output.data(),128);
    CHECK(output[0] == 0.06103515625f); // backing stays audible, input still captured
    rejects([&] { app.new_project(); }); rejects([&] { app.save_project(project_file); });
    rejects([&] { app.disconnect(); }); rejects([&] { app.seek(0); });
    rejects([&] { app.undo(); }); rejects([&] { app.arm_track({}); });
    rejects([&] { app.remove_track(armed); }); rejects([&] { app.rename_track(armed,"During rec"); });
    CHECK(project->state().revision == revision && app.recording());
    CHECK(app.stop_recording() && !app.recording() && app.audio_running());
    CHECK(project->state().revision == revision+1 && app.dirty());
    CHECK(project->state().project->tracks.size() == 2 && project->state().project->clips.size() == 2);
    const auto clip = project->state().project->clips.back();
    CHECK(clip.track == armed && clip.start == 500 && clip.length == 256 && clip.source_offset == 0);
    CHECK(app.engine()->state().sample == 756 && app.engine()->state().playback == PlaybackState::paused);
    const auto raw = audio::load_wav(take); CHECK(raw.channels == 1 && raw.frames() == 256);
    for (auto value : raw.samples) CHECK(value == 0.25f);
    CHECK(app.recording_status().frames == 256 && app.recording_error().empty());
    CHECK(app.undo() && project->state().project->clips.size() == 1 && std::filesystem::exists(take));
    CHECK(app.redo() && project->state().project->clips.back() == clip);
    // Integrated import/edit/record/processor state/unknown-chunk save/load.
    const auto imported = project->state().project->clips.front().id;
    app.trim_clip(imported,100,900);
    auto snapshot = app.snapshot(); snapshot.extensions = {{"FUTR","preserve with take"}};
    persistence::save_project(project_file,snapshot);
    app.open_project(project_file);
    CHECK(app.snapshot().project == snapshot.project && app.snapshot().extensions == snapshot.extensions);
    CHECK(app.snapshot().graph == snapshot.graph);
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{0},{0,1,2,3}});
    app.seek(500); app.engine()->process(input.data(),output.data(),128);
    app.play(); app.engine()->process(input.data(),output.data(),128);
    CHECK(output[0] == 0.18603515625f && output[1] == output[0]); // recorded mono centered with backing
    CHECK(output[2] == 0 && output[3] == 0);
    app.pause(); app.engine()->process(input.data(),output.data(),128);
    app.save_project(project_file); CHECK(!app.dirty());
    app.arm_track(armed); app.start_recording(dir.path/"Dropout.wav");
    app.engine()->process(input.data(),output.data(),128);
    app.engine()->process(nullptr,output.data(),128);
    app.poll();
    CHECK(!app.recording() && !app.recording_error().empty() && app.audio_running());
    CHECK(app.recording_status().missing_blocks == 1);
    // project pointer was replaced by Open, so inspect current store.
    CHECK(app.services().projects->state().project->clips.back().length == 128);
    CHECK(audio::load_wav(dir.path/"Dropout.wav").frames() == 128);
    const auto count = app.services().projects->state().project->clips.size();
    app.start_recording(dir.path/"Empty.wav"); CHECK(!app.stop_recording());
    CHECK(app.services().projects->state().project->clips.size() == count && !std::filesystem::exists(dir.path/"Empty.wav"));
    app.services().transport->set_loop(LoopRange{0,2000});
    app.engine()->process(input.data(),output.data(),128);
    rejects([&] { app.start_recording(dir.path/"Loop.wav"); });
    app.services().transport->set_loop({}); app.engine()->process(input.data(),output.data(),128);
    app.start_recording(dir.path/"Stop.wav"); app.engine()->process(input.data(),output.data(),128);
    app.stop(); CHECK(!app.recording() && app.services().projects->state().project->clips.size() == count+1);
    app.engine()->process(input.data(),output.data(),128); CHECK(app.engine()->state().sample == 0);
    app.remove_track(armed); CHECK(!app.armed_track()); CHECK(app.undo());
    app.arm_track(armed); app.redo(); CHECK(!app.armed_track());
    CHECK(app.undo()); app.arm_track(armed);
    auto broken = std::make_unique<ManualDevice>(); auto* driver = broken.get();
    app.connect(std::move(broken),{0,44100,128,{0},{0,1}});
    app.start_recording(dir.path/"Driver-failure.wav");
    app.engine()->process(input.data(),output.data(),128);
    driver->fail_driver(); app.poll();
    CHECK(!app.recording() && !app.audio_running() && app.audio_name() == "Disconnected");
    CHECK(!app.recording_error().empty() && audio::load_wav(dir.path/"Driver-failure.wav").frames() == 128);
    CHECK(app.services().projects->state().project->clips.back().length == 128);
    {
        // Lost streamed backing must not prevent finalization of the captured take.
        const auto missing = dir.path/"Streamed-backing.wav";
        constexpr std::uint32_t frames = 3'000'000;
        {
            std::ofstream out(missing,std::ios::binary);
            const auto u16 = [&](std::uint16_t n) { for (int i=0; i<2; ++i) out.put(static_cast<char>((n>>(8*i))&255)); };
            const auto u32 = [&](std::uint32_t n) { for (int i=0; i<4; ++i) out.put(static_cast<char>((n>>(8*i))&255)); };
            out.write("RIFF",4); u32(36+frames*4); out.write("WAVEfmt ",8); u32(16);
            u16(3); u16(1); u32(44100); u32(176400); u16(4); u16(32);
            out.write("data",4); u32(frames*4);
            out.seekp(static_cast<std::streamoff>(44)+frames*4-1); out.put(0);
        }
        Application failure; failure.new_project(44100); failure.import_wavs({missing});
        const auto source = failure.services().projects->state().project->clips.front().source;
        eventually([&] { failure.poll(); return failure.waveform(source) != nullptr; });
        const auto target = failure.add_audio_track("Captured");
        failure.arm_track(target);
        failure.connect(std::make_unique<ManualDevice>(),{0,44100,128,{0},{0,1}});
        const auto final = dir.path/"Preserved-take.wav";
        failure.start_recording(final); failure.engine()->process(input.data(),output.data(),128);
        CHECK(std::filesystem::remove(missing));
        rejects([&] { (void)failure.stop_recording(); });
        CHECK(!failure.recording() && !failure.audio_running() && !failure.recording_error().empty());
        CHECK(failure.last_take() == std::filesystem::absolute(final) && audio::load_wav(final).frames() == 128);
        CHECK(failure.services().projects->state().project->clips.back().track == target);
    }
}


std::size_t file_count(const std::filesystem::path& folder) {
    std::size_t count{}; for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) if (entry.is_regular_file()) ++count;
    return count;
}
void project_folders() {
    Directory dir, originals; StudioFolders studio{dir.path/"MR Studio"}; studio.ensure();
    CHECK(std::filesystem::is_directory(studio.projects()) && std::filesystem::is_directory(studio.lives()));
    const auto name = std::filesystem::path(std::u8string(u8"Песня"));
    const auto file = project_folder_file(studio.projects()/(name.native()+std::filesystem::path(".mrsproject").native()));
    CHECK(file.parent_path() == studio.projects()/name && file.filename().stem() == name);
    CHECK(project_folder_file(file) == file);
    Application app; app.new_project(44100,"Song"); app.save_project(file);
    CHECK(std::filesystem::exists(file) && std::filesystem::is_directory(file.parent_path()/"Media") && std::filesystem::is_directory(file.parent_path()/"Mixdown"));
    std::filesystem::create_directory(originals.path/"A"); std::filesystem::create_directory(originals.path/"B");
    const auto a = originals.path/"A"/"same.wav", b = originals.path/"B"/"same.wav"; wav(a,1); wav(b);
    app.import_wavs({a,b,a});
    const auto clips = app.services().projects->state().project->clips;
    CHECK(clips.size() == 3 && clips[0].source == clips[2].source && clips[0].source != clips[1].source);
    CHECK(file_count(file.parent_path()/"Media") == 2 && std::filesystem::exists(a) && std::filesystem::exists(b));
    const auto count = file_count(file.parent_path()/"Media");
    rejects([&] { app.import_wavs({a,originals.path/"missing.wav"}); });
    CHECK(file_count(file.parent_path()/"Media") == count && app.services().projects->state().project->clips.size() == 3);
    app.save_project(file);
    const auto saved = persistence::load_project(file);
    for (const auto& clip : saved.project.clips) {
        const auto source = std::filesystem::path(std::u8string(clip.source.begin(),clip.source.end()));
        CHECK(source.is_relative() && *source.begin() == "Media" && std::filesystem::exists(file.parent_path()/source));
    }
    CHECK(std::filesystem::remove(a) && std::filesystem::remove(b)); // imported media is independent
    std::filesystem::copy_file(file.parent_path()/std::filesystem::path(std::u8string(clips[0].source.begin(),clips[0].source.end())),file.parent_path()/"Media"/"Unused.wav");
    { std::ofstream export_file(file.parent_path()/"Mixdown"/"render.txt"); export_file << "future export content"; }
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    std::array<float,256> output{};
    app.seek(600); app.engine()->process(nullptr,output.data(),128);
    app.play(); app.engine()->process(nullptr,output.data(),128);
    app.save_project(file); CHECK(app.engine()->state().playback == PlaybackState::playing); // ordinary save remains possible while playing
    app.pause(); app.engine()->process(nullptr,output.data(),128);
    const auto position = app.engine()->state().sample;
    const auto copy = project_folder_file(studio.projects()/"Copy.mrsproject");
    app.save_project(copy);
    CHECK(app.path() == copy && app.audio_running() && app.engine()->state().sample == position && app.engine()->state().playback == PlaybackState::paused);
    CHECK(file_count(copy.parent_path()/"Media") == 3 && std::filesystem::exists(copy.parent_path()/"Mixdown"/"render.txt"));
    CHECK(app.undo() && app.services().projects->state().project->clips.empty());
    CHECK(app.redo() && app.services().projects->state().project->clips.size() == 3);
    app.save_project(copy); const auto snapshot = app.snapshot();
    app.new_project(44100); // close device/workers before moving folders
    const auto moved = dir.path/"Portable";
    std::filesystem::rename(copy.parent_path(),moved);
    app.open_project(moved/copy.filename()); CHECK(app.snapshot().project == snapshot.project);
    app.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}}); app.play();
    app.engine()->process(nullptr,output.data(),128);
    CHECK(output[0] == 0.18310546875f && output[1] == output[0]);
    app.pause(); app.engine()->process(nullptr,output.data(),128);
    const auto foreign = project_folder_file(studio.projects()/"Foreign.mrsproject");
    Application other; other.save_project(foreign);
    const auto identity = persistence::load_project(foreign).project.id;
    rejects([&] { app.save_project(foreign); });
    CHECK(app.path() == moved/copy.filename() && persistence::load_project(foreign).project.id == identity);

    // Upgrade an external long source, then Save As again without the original.
    const auto long_file = originals.path/"Long.wav";
    constexpr std::uint32_t frames = 3'000'000;
    {
        std::ofstream out(long_file,std::ios::binary);
        const auto u16 = [&](std::uint16_t n) { for (int i=0; i<2; ++i) out.put(static_cast<char>((n>>(8*i))&255)); };
        const auto u32 = [&](std::uint32_t n) { for (int i=0; i<4; ++i) out.put(static_cast<char>((n>>(8*i))&255)); };
        out.write("RIFF",4); u32(36+frames*4); out.write("WAVEfmt ",8); u32(16);
        u16(3); u16(1); u32(44100); u32(176400); u16(4); u16(32);
        out.write("data",4); u32(frames*4); u32(0x3e800000); // first frame 0.25f
        out.seekp(static_cast<std::streamoff>(44)+frames*4-1); out.put(0);
    }
    Application legacy; legacy.import_wav(long_file);
    const auto legacy_source = legacy.services().projects->state().project->clips.front().source;
    const auto long_project = project_folder_file(studio.projects()/"Long.mrsproject");
    legacy.save_project(long_project);
    CHECK(persistence::load_project(long_project).project.clips.front().source.starts_with("Media/"));
    CHECK(std::filesystem::remove(long_file));
    legacy.connect(std::make_unique<ManualDevice>(),{0,44100,128,{}, {0,1}});
    legacy.play(); legacy.engine()->process(nullptr,output.data(),128); CHECK(output[0] == 0.125f);
    legacy.pause(); legacy.engine()->process(nullptr,output.data(),128);
    const auto long_copy = project_folder_file(studio.projects()/"Long-copy.mrsproject");
    legacy.save_project(long_copy);
    CHECK(legacy.services().projects->state().project->clips.front().source == legacy_source); // stable source aliases/Undo
    legacy.seek(0); legacy.play(); legacy.engine()->process(nullptr,output.data(),128); CHECK(output[0] == 0.125f);
    legacy.pause(); legacy.engine()->process(nullptr,output.data(),128);
    legacy.open_project(long_copy); CHECK(legacy.snapshot().project == persistence::load_project(long_copy).project);
}

void config() {
    Preferences p; p.workspace = Workspace::live; p.device_name = "Komplete Audio ASIO Driver"; p.reconnect_audio = true;
    CHECK(decode_preferences(encode_preferences(p)) == p);
    auto disabled = p; disabled.reconnect_audio = false;
    CHECK(decode_preferences(encode_preferences(disabled)) == disabled);
    const auto legacy = decode_preferences("MRS_DESKTOP_CONFIG 1\n0 48000 128 -1 \"Komplete Audio ASIO Driver\" 2 0 1\n");
    CHECK(legacy.reconnect_audio && legacy.device_name == p.device_name);
    CHECK(!decode_preferences("MRS_DESKTOP_CONFIG 1\n0 48000 128 -1 \"\" 2 0 1\n").reconnect_audio);
    rejects([&] { (void)decode_preferences("MRS_DESKTOP_CONFIG 2\n0 48000 128 -1 \"\" 2 0 1 2\n"); });
    CHECK(parse_outputs("1, 2,6") == std::vector<int>({0,1,5}));
    for (auto text : {"","1,","0","1,1","65","x","1,,2","-1"}) rejects([&] { (void)parse_outputs(text); });
    rejects([&] { (void)decode_preferences("MRS_DESKTOP_CONFIG 2"); });
    rejects([&] { (void)decode_preferences(encode_preferences(p)+"extra"); });
    auto bad = p; bad.outputs = {0,0}; rejects([&] { (void)encode_preferences(bad); });
    Directory dir; Logger log(dir.path / "app.log"); log.write("control-thread log");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected suite");
        std::string name = argv[1];
        if (name == "workspaces") workspaces(); else if (name == "transport") transport();
        else if (name == "files") files(); else if (name == "assets") assets();
        else if (name == "mono_route") mono_route(); else if (name == "audio") audio_settings(); else if (name == "config") config(); else if (name == "arrangement") arrangement(); else if (name == "waveform") waveform(); else if (name == "nonplaying_edits") nonplaying_edits(); else if (name == "streaming") streaming(); else if (name == "clip_edits") clip_edits(); else if (name == "recording") recording(); else if (name == "project_folders") project_folders(); else throw std::runtime_error("unknown suite");
        std::cout << "PASS desktop " << name << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
