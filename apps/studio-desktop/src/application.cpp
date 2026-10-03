#include <mrs/desktop.hpp>
#include <mrs/offline_device.hpp>
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
namespace mrs::desktop {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
std::string utf8(const std::filesystem::path& path) {
    auto value = path.u8string(); return {reinterpret_cast<const char*>(value.data()),value.size()};
}
std::string media_ref(const std::filesystem::path& path) {
    const auto value = path.generic_u8string(); return {reinterpret_cast<const char*>(value.data()),value.size()};
}
}
std::string_view workspace_name(Workspace w) {
    switch (w) { case Workspace::arrange: return "Arrange"; case Workspace::edit: return "Edit";
        case Workspace::mix: return "Mix"; case Workspace::live: return "Live"; }
    throw std::invalid_argument("unknown workspace");
}
void Preferences::validate() const {
    (void)workspace_name(workspace); require(rate >= 8000 && rate <= 768000,"invalid sample rate");
    require(buffer >= 8 && buffer <= 8192,"invalid buffer"); require(monitor_input >= -1 && monitor_input < 64,"invalid monitor input");
    require(!outputs.empty() && outputs.size() <= 64 && device_name.size() <= 4096,"invalid device preferences");
    std::set<int> seen; for (auto o : outputs) require(o >= 0 && o < 64 && seen.insert(o).second,"invalid/duplicate output");
}
std::string encode_preferences(const Preferences& p) {
    p.validate(); std::ostringstream out;
    out << "MRS_DESKTOP_CONFIG 2\n" << static_cast<int>(p.workspace) << ' ' << p.rate << ' ' << p.buffer << ' ' << p.monitor_input
        << ' ' << std::quoted(p.device_name) << ' ' << p.outputs.size();
    for (auto o : p.outputs) out << ' ' << o;
    out << ' ' << p.reconnect_audio << "\n"; return out.str();
}
Preferences decode_preferences(std::string_view bytes) {
    require(bytes.size() <= 16384,"config too large"); std::istringstream in{std::string(bytes)};
    std::string magic; int version{},workspace{}; std::size_t count{}; Preferences p;
    require(static_cast<bool>(in >> magic >> version) && magic == "MRS_DESKTOP_CONFIG" && (version == 1 || version == 2),"unsupported config");
    require(static_cast<bool>(in >> workspace >> p.rate >> p.buffer >> p.monitor_input >> std::quoted(p.device_name) >> count) && workspace >= 0 && workspace <= 3 && count > 0 && count <= 64,"invalid config");
    p.workspace = static_cast<Workspace>(workspace); p.outputs.clear();
    for (std::size_t i = 0; i < count; ++i) { int o{}; require(static_cast<bool>(in >> o),"truncated config"); p.outputs.push_back(o); }
    if (version == 2) {
        int enabled{}; require(static_cast<bool>(in >> enabled) && (enabled == 0 || enabled == 1),"invalid reconnect preference");
        p.reconnect_audio = enabled != 0;
    } else p.reconnect_audio = !p.device_name.empty();
    in >> std::ws; require(in.eof(),"extra config data"); p.validate(); return p;
}
std::vector<int> parse_outputs(std::string_view bytes) {
    require(!bytes.empty() && bytes.size() <= 256,"enter output numbers, e.g. 1,2");
    std::vector<int> result; std::set<int> seen;
    while (!bytes.empty()) {
        const auto comma = bytes.find(','); auto part = bytes.substr(0,comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        int n{}; const auto parsed = std::from_chars(part.data(),part.data()+part.size(),n);
        require(parsed.ec == std::errc{} && parsed.ptr == part.data()+part.size() && n >= 1 && n <= 64 && seen.insert(n).second,"invalid/duplicate physical output");
        result.push_back(n-1);
        if (comma == std::string_view::npos) break;
        bytes.remove_prefix(comma+1); require(!bytes.empty(),"trailing output comma");
    }
    return result;
}
Logger::Logger(const std::filesystem::path& path) : stream_(path,std::ios::app) {}
void Logger::write(std::string_view text) {
    if (stream_) { stream_ << text << '\n'; stream_.flush(); }
}
persistence::ProjectDocument foundation_demo() {
    auto d = persistence::demo_document(); d.project = musical_demo_project();
    d.project.folders.clear(); d.project.tracks = {{{"demo-track"},"Demo tone",TrackKind::audio,{}}};
    d.project.clips = {{{"demo-clip"},{"demo-track"},"Demo tone 220 Hz",0,32*48000,0,"mrs:demo-tone"}};
    d.project.title = "Moon River — Foundation Demo";
    d.mixer = {{{"demo-track"},1,0,false,false}}; d.actions.clear();
    d.live_notes = "Demo harmony and sections; not inferred from audio.";
    d.validate(); return d;
}
Application::Application() : engine_(std::make_shared<audio::AudioEngine>()) { demo(); }
Application::~Application() {
    if (device_) device_->close();
    // Finalize on clean shutdown; a completed file remains recoverable even if
    // the UI did not attach/save its project reference.
    if (recording_) try { (void)recording_->finish(); } catch (const std::exception&) {}
    for (auto& [key,value] : assets_) { (void)key; *value.cancel = true; }
}
void Application::workspace(Workspace w) { (void)workspace_name(w); workspace_ = w; }
void Application::replace(persistence::ProjectDocument next) {
    require_not_recording(); next.validate(); armed_.reset();
    // Build application models before releasing the old session.
    auto projects = std::make_shared<ProjectStore>(next.project);
    auto graphs = std::make_shared<processing::GraphStore>(next.graph);
    disconnect();
    engine_->prepare({next.project.sample_rate,0,2,8192},{});
    auto transport = std::make_shared<audio::EngineTransport>(engine_,Timeline(next.project.time,next.project.sample_rate));
    auto musical = std::make_unique<MusicalTimeline>(Services{projects,transport});
    for (auto& [key,value] : assets_) { (void)key; *value.cancel = true; }
    assets_.clear(); owned_media_.clear(); waveform_error_.clear();
    document_ = std::move(next);
    services_ = {projects,transport}; graphs_ = std::move(graphs);
    transport_ = std::move(transport); musical_ = std::move(musical);
    saved_project_revision_ = saved_graph_revision_ = 0; unsaved_ = true;
    engine_->prepare({document_.project.sample_rate,0,2,8192},{});
    start_empty_clock(); transport_->poll(); musical_->refresh();
}
void Application::start_empty_clock() {
    auto device = audio::make_offline_device();
    audio::DeviceConfig c{0,document_.project.sample_rate,128,{}, {0,1}};
    device->open(c,engine_); device->start(); device_ = std::move(device); device_config_ = c; audio_name_ = "Offline clock (no sound)";
}
void Application::demo() {
    require_not_recording();
    path_.clear(); asset_root_.clear(); replace(foundation_demo());
}
void Application::open_project(const std::filesystem::path& path) {
    require_not_recording();
    auto next = persistence::load_project(path); replace(std::move(next));
    path_ = std::filesystem::absolute(path).lexically_normal(); asset_root_ = path_.parent_path(); unsaved_ = false;
}
void Application::import_wav(const std::filesystem::path& path) {
    require_not_recording();
    const auto asset = audio::open_wav(path); // validate before touching the current session
    auto d = persistence::demo_document(); d.project = Project{};
    d.project.id = new_id(); d.project.title = utf8(path.stem()); d.project.sample_rate = asset.sample_rate;
    const auto track = new_id();
    d.project.tracks = {{track,"Audio",TrackKind::audio,{}}};
    d.project.clips = {{new_id(),track,utf8(path.filename()),0,asset.frames(),0,utf8(std::filesystem::absolute(path))}};
    d.mixer = {{track,1,0,false,false}}; d.actions.clear(); d.live_notes.clear();
    replace(std::move(d)); path_.clear(); asset_root_.clear();
}
persistence::ProjectDocument Application::snapshot() const {
    auto result = document_; const auto p = services_.projects->state(); const auto g = graphs_->state();
    require(p.revision <= std::numeric_limits<std::uint64_t>::max()-result.generation,"generation overflow");
    result.generation += p.revision;
    require(g.revision <= std::numeric_limits<std::uint64_t>::max()-result.generation,"generation overflow");
    result.generation += g.revision; result.project = *p.project; result.graph = *g.graph;
    // Base channel state stays available for Undo; only extant tracks serialize.
    std::erase_if(result.mixer,[&](const auto& m) {
        return std::none_of(result.project.tracks.begin(),result.project.tracks.end(),[&](const auto& t) { return t.id == m.track; });
    });
    for (const auto& t : result.project.tracks) if (std::none_of(result.mixer.begin(),result.mixer.end(),[&](const auto& m) { return m.track == t.id; }))
        result.mixer.push_back({t.id,1,0,false,false});
    for (auto& clip : result.project.clips) {
        if (const auto found = owned_media_.find(clip.source); found != owned_media_.end())
            clip.source = media_ref(found->second.lexically_relative(asset_root_));
        else if (!asset_root_.empty()) {
            const auto source = std::filesystem::path(std::u8string(clip.source.begin(),clip.source.end()));
            const auto relative = source.lexically_normal().lexically_relative(asset_root_/"Media");
            if (source.is_absolute() && !relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                clip.source = media_ref(std::filesystem::path("Media")/relative);
        }
    }
    result.validate(); return result;
}
void Application::save_project(const std::filesystem::path& requested) {
    require_not_recording();
    const auto path = std::filesystem::absolute(requested).lexically_normal();
    require(!path.filename().empty(),"choose a project filename");
    auto document = snapshot();
    // Validate identity BEFORE adding content to an existing project directory.
    if (std::filesystem::exists(path)) {
        const auto previous = persistence::load_project(path);
        require(previous.project.id == document.project.id && previous.generation <= document.generation,"project identity/generation mismatch");
    }
    const auto root = path.parent_path();
    const bool relocating = !asset_root_.empty() && root != asset_root_;
    std::set<std::string> keys, current;
    for (const auto& clip : services_.projects->state().project->clips) if (clip.source != "mrs:demo-tone") { keys.insert(clip.source); current.insert(clip.source); }
    for (const auto& [key,value] : assets_) { (void)value; if (key != "mrs:demo-tone") keys.insert(key); }
    const auto resolve = [&](const std::string& key) {
        if (const auto found = owned_media_.find(key); found != owned_media_.end()) return found->second;
        auto source = std::filesystem::path(std::u8string(key.begin(),key.end()));
        return source.is_absolute() ? source : asset_root_/source;
    };
    bool needs_copy = relocating;
    for (const auto& key : keys) {
        const auto relative = resolve(key).lexically_normal().lexically_relative(root/"Media");
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") needs_copy = true;
    }
    if (needs_copy) require_not_playing(); // media/Save As rebuilds need quiescent callbacks
    MediaCopy copies(root);
    if (relocating) { copies.content(asset_root_,"Media"); copies.content(asset_root_,"Mixdown"); }
    std::map<std::string,std::filesystem::path> owned;
    for (const auto& key : keys) {
        const auto source = resolve(key);
        if (!std::filesystem::exists(source) && !current.contains(key)) continue; // already-missing Undo media stays a missing reference
        owned[key] = copies.media(source);
    }
    for (std::size_t i=0; i<document.project.clips.size(); ++i) {
        const auto& original = services_.projects->state().project->clips[i].source;
        if (original != "mrs:demo-tone") document.project.clips[i].source = media_ref(owned.at(original).lexically_relative(root));
    }
    document.validate();
    if (copies.copied()) require_not_playing();
    persistence::save_project(path,document); copies.commit();
    path_ = path; asset_root_ = root; owned_media_ = std::move(owned);
    // Runtime keys/Undo are stable; move only their physical disk backing.
    if (copies.copied()) {
        for (auto& [key,cached] : assets_) if (cached.data->file) {
            const auto found = owned_media_.find(key); if (found == owned_media_.end()) continue;
            auto data = *cached.data; auto file = std::make_shared<audio::WavFile>(*data.file); file->path = found->second; data.file = std::move(file);
            *cached.cancel = true; cached.pending = {}; // cancelled worker stops before the old source can be retired
            cached.cancel = std::make_shared<std::atomic<bool>>(false);
            cached.data = std::make_shared<const audio::AudioData>(std::move(data));
            if (!cached.peaks) cached.pending = std::async(std::launch::async,[data=cached.data,cancel=cached.cancel] { return audio::Waveform(*data,cancel); });
        }
        rebuild_audio(); // same device handle, paused/stopped position and loop
    }
    saved_project_revision_ = services_.projects->state().revision;
    saved_graph_revision_ = graphs_->state().revision; unsaved_ = false;
}
bool Application::dirty() const {
    return unsaved_ || services_.projects->state().revision != saved_project_revision_ || graphs_->state().revision != saved_graph_revision_;
}
void Application::rename_track(const Id& id, std::string name) {
    require_not_recording();
    require(!name.empty() && name.size() <= 4096,"enter a track name");
    services_.projects->execute(RenameTrack{id,std::move(name)});
}
void Application::require_not_recording() const {
    require(!recording_,"End recording before changing project, files or device");
}
void Application::sync_arm() {
    const auto p = services_.projects->state().project;
    if (armed_ && std::none_of(p->tracks.begin(),p->tracks.end(),[&](const auto& t) { return t.id == *armed_; })) armed_.reset();
}
void Application::require_not_playing() const {
    require_not_recording();
    require(engine_->state().playback != PlaybackState::playing,"Pause or stop playback before changing tracks or importing audio");
}
void Application::new_project(std::uint32_t rate, std::string title) {
    require_not_recording();
    auto d = foundation_demo(); d.project.id = new_id(); d.project.title = std::move(title);
    d.project.sample_rate = rate; d.project.tracks.clear(); d.project.clips.clear();
    d.project.chords.clear(); d.project.sections.clear(); d.project.markers.clear(); d.mixer.clear(); d.live_notes.clear();
    d.validate(); replace(std::move(d)); path_.clear(); asset_root_.clear();
}
void Application::cache_asset(std::string source, std::shared_ptr<const audio::AudioData> data) {
    if (assets_.contains(source)) return;
    require(assets_.size() < 128,"retained source limit reached (128); start a new project to release Undo media");
    std::size_t bytes = data->samples.size()*sizeof(float);
    for (const auto& [key,cached] : assets_) { (void)key; bytes += cached.data->samples.size()*sizeof(float); }
    require(bytes <= 512*1024*1024,"project preload/cache exceeds 512 MiB");
    CachedAsset value; value.data = data;
    value.pending = std::async(std::launch::async,[data,cancel = value.cancel] { return audio::Waveform(*data,cancel); });
    assets_.emplace(std::move(source),std::move(value));
}
std::shared_ptr<const audio::AudioData> Application::asset(const std::string& source) {
    if (const auto it = assets_.find(source); it != assets_.end()) return it->second.data;
    audio::AudioData data;
    if (source == "mrs:demo-tone") data = audio::sine_fixture(document_.project.sample_rate,2,32*static_cast<Sample>(document_.project.sample_rate),220);
    else {
        require(!source.empty(),"clip has no audio source");
        auto path = std::filesystem::path(std::u8string(source.begin(),source.end()));
        if (const auto owned = owned_media_.find(source); owned != owned_media_.end()) path = owned->second;
        else if (path.is_relative()) path = asset_root_ / path;
        data = audio::open_wav(path);
    }
    auto ptr = std::make_shared<const audio::AudioData>(std::move(data)); cache_asset(source,ptr); return ptr;
}
void Application::prepare_waveforms() {
    for (const auto& clip : services_.projects->state().project->clips) (void)asset(clip.source);
}
const audio::Waveform* Application::waveform(std::string_view source) const {
    const auto it = assets_.find(std::string(source));
    return it != assets_.end() && it->second.peaks ? &*it->second.peaks : nullptr;
}
void Application::rebuild_audio() {
    if (!device_ || !device_config_) return;
    const auto c = *device_config_;
    // Retain the same open hardware handle; rebuild ONLY after callbacks stop.
    device_->stop();
    try {
        const auto position = engine_->state();
        auto graph = render(c);
        engine_->prepare({c.sample_rate,static_cast<std::uint32_t>(c.inputs.size()),static_cast<std::uint32_t>(c.outputs.size()),8192},std::move(graph),position);
        device_->start(); poll();
    } catch (...) { disconnect(); throw; }
}
void Application::edit(const ICommand& command) {
    require_not_playing();
    auto candidate = *services_.projects->state().project;
    command.apply(candidate); candidate.validate();
    require(candidate.clips.size() <= audio::max_voices,"too many playback clips");
    std::size_t streamed{}, bytes{};
    for (const auto& clip : candidate.clips) {
        const auto data = asset(clip.source);
        require(data->sample_rate == candidate.sample_rate,"WAV/project sample-rate mismatch");
        require(clip.source_offset <= data->frames() && clip.length <= data->frames()-clip.source_offset,"clip exceeds source audio");
        if (data->file) {
            ++streamed; bytes += 8*8192*static_cast<std::size_t>(data->channels)*sizeof(float);
        }
    }
    require(streamed <= 32 && bytes <= 256*1024*1024,"disk voice budget exceeded (32 voices / 256 MiB)");
    services_.projects->execute(command); sync_arm(); rebuild_audio();
}
Id Application::add_audio_track(std::string name) {
    require(!name.empty() && name.size() <= 4096,"enter a track name");
    auto id = new_id(); edit(AddTrack{{id,std::move(name),TrackKind::audio,{}}}); return id;
}
void Application::remove_track(const Id& id) { edit(RemoveTrack{id}); }
void Application::reorder_track(const Id& id, std::size_t index) { edit(ReorderTrack{id,index}); }
bool Application::undo() { require_not_playing(); const auto changed = services_.projects->undo(); if (changed) { sync_arm(); rebuild_audio(); } return changed; }
bool Application::redo() { require_not_playing(); const auto changed = services_.projects->redo(); if (changed) { sync_arm(); rebuild_audio(); } return changed; }
Sample Application::source_frames(const Id& id) {
    const auto p = services_.projects->state().project;
    const auto it = std::find_if(p->clips.begin(),p->clips.end(),[&](const auto& clip) { return clip.id == id; });
    require(it != p->clips.end(),"unknown clip");
    return asset(it->source)->frames();
}
void Application::move_clip(const Id& id, const Id& track, Sample start) { edit(MoveAudioClip{id,track,start}); }
void Application::trim_clip(const Id& id, Sample start, Sample end) {
    require_not_playing(); const auto frames = source_frames(id); edit(TrimAudioClip{id,start,end,frames});
}
Id Application::split_clip(const Id& id, Sample position) {
    require(services_.projects->state().project->clips.size() < audio::max_voices,"too many playback clips to split");
    const auto right = new_id(); edit(SplitAudioClip{id,position,right}); return right;
}
void Application::remove_clip(const Id& id) { edit(RemoveAudioClip{id}); }
void Application::import_wavs(const std::vector<std::filesystem::path>& paths) {
    require_not_playing(); require(!paths.empty() && paths.size() <= audio::max_voices,"select 1..128 WAV files");
    const auto current = services_.projects->state().project;
    require(current->clips.size()+paths.size() <= audio::max_voices,"too many playback clips");
    // Inspect the entire source batch before copying content.
    for (const auto& path : paths) require(audio::inspect_wav(path).sample_rate == current->sample_rate,"WAV/project sample-rate mismatch; import WAVs at the project rate");
    std::unique_ptr<MediaCopy> copies;
    if (!path_.empty()) copies = std::make_unique<MediaCopy>(asset_root_);
    std::vector<Track> tracks; std::vector<Clip> clips;
    std::map<std::string,std::shared_ptr<const audio::AudioData>> decoded;
    std::size_t bytes{};
    for (const auto& [key,cached] : assets_) { (void)key; bytes += cached.data->samples.size()*sizeof(float); }
    for (const auto& original : paths) {
        const auto path = copies ? copies->media(original) : std::filesystem::absolute(original).lexically_normal();
        const auto source = (copies ? media_ref(path.lexically_relative(asset_root_)) : utf8(path));
        std::shared_ptr<const audio::AudioData> data;
        if (assets_.contains(source)) data = assets_.at(source).data;
        else if (decoded.contains(source)) data = decoded.at(source);
        else {
            require(assets_.size()+decoded.size() < 128,"retained source limit reached (128); start a new project to release Undo media");
            require(bytes < 512*1024*1024,"project preload/cache exceeds 512 MiB");
            data = std::make_shared<const audio::AudioData>(audio::open_wav(path,std::min<std::size_t>(8*1024*1024,512*1024*1024-bytes)));
            bytes += data->samples.size()*sizeof(float); decoded.emplace(source,data);
        }
        const auto id = new_id();
        tracks.push_back({id,utf8(original.stem()),TrackKind::audio,{}});
        clips.push_back({new_id(),id,utf8(original.filename()),0,data->frames(),0,source});
    }
    const auto revision = services_.projects->state().revision;
    std::vector<std::string> cached;
    try {
        for (auto& [source,data] : decoded) { cache_asset(source,std::move(data)); cached.push_back(source); }
        edit(ImportAudio{std::move(tracks),std::move(clips)});
        if (copies) copies->commit();
    } catch (...) {
        if (services_.projects->state().revision != revision) { if (copies) copies->commit(); } // committed clips must retain their media
        else for (const auto& source : cached) { *assets_.at(source).cancel = true; assets_.erase(source); }
        throw;
    }
}
audio::RenderGraph Application::render(const audio::DeviceConfig& c) {
    require(c.sample_rate == services_.projects->state().project->sample_rate,"device/project rate mismatch; choose the project rate (resampling is a later stage)");
    require(!c.outputs.empty() && c.outputs.size() <= audio::max_channels && c.inputs.size() <= 1,"invalid channel selection");
    audio::RenderGraph result;
    result.recording = recording_; result.monitoring = monitoring_;
    const auto config = processing::ProcessConfig{c.sample_rate,static_cast<std::uint32_t>(c.outputs.size()),8192,c.buffer_frames};
    prepared_ = std::make_shared<processing::PreparedGraph>(graphs_->state(),config);
    result.processors = prepared_;
    const auto project = services_.projects->state().project;
    for (const auto& clip : project->clips) {
        require(result.voices.size() < audio::max_voices,"too many playback voices");
        auto asset_data = asset(clip.source);
        require(asset_data->sample_rate == c.sample_rate,"WAV/project sample-rate mismatch");
        audio::Voice voice{asset_data,clip.start,clip.source_offset,clip.length,{}};
        const auto gain = clip.source == "mrs:demo-tone" ? 0.15f : 1.0f;
        if (asset_data->channels == 1) {
            // A mono track is centered in the selected main pair, without
            // spilling into extra click/cue outputs. Single-output remains unity.
            const auto outputs = std::min<std::size_t>(2,c.outputs.size());
            for (std::uint32_t output = 0; output < outputs; ++output) voice.routes.push_back({0,output,gain});
        } else for (std::uint32_t channel = 0; channel < asset_data->channels; ++channel)
            voice.routes.push_back({channel,channel % static_cast<std::uint32_t>(c.outputs.size()),gain});
        result.voices.push_back(std::move(voice));
    }
    if (!c.inputs.empty()) for (std::uint32_t channel = 0; channel < std::min<std::size_t>(2,c.outputs.size()); ++channel) result.monitor.push_back({0,channel,1});
    return result;
}
void Application::connect(std::unique_ptr<audio::IAudioDevice> device, audio::DeviceConfig c) {
    require_not_recording();
    require(static_cast<bool>(device),"missing audio backend");
    auto infos = device->enumerate();
    const auto info = std::find_if(infos.begin(),infos.end(),[&](const auto& v) { return v.index == c.device; });
    require(info != infos.end(),"audio device no longer available"); audio::validate_device_config(*info,c);
    // Existing callbacks must be stopped before preparing or releasing graphs.
    disconnect();
    try {
        auto graph = render(c);
        engine_->prepare({c.sample_rate,static_cast<std::uint32_t>(c.inputs.size()),static_cast<std::uint32_t>(c.outputs.size()),8192},std::move(graph));
        device->open(c,engine_); device->start(); audio_name_ = info->name; device_ = std::move(device); device_config_ = c; poll();
    } catch (...) {
        device->close(); prepared_.reset();
        engine_->prepare({services_.projects->state().project->sample_rate,0,2,8192},{});
        transport_->poll(); throw;
    }
}
void Application::disconnect() {
    require_not_recording();
    if (device_) { device_->close(); device_.reset(); }
    prepared_.reset(); device_config_.reset(); audio_name_ = "Disconnected";
    if (transport_) { engine_->prepare({engine_->config().sample_rate,0,2,8192},{}); transport_->poll(); }
}
void Application::poll() {
    if (recording_ && (recording_->status().fault != audio::RecordFault::none ||
        (recording_->status().frames > 0 && engine_->state().playback != PlaybackState::playing) ||
        device_status().phase != audio::DevicePhase::running)) {
        try { (void)stop_recording(); } catch (const std::exception& e) { recording_error_ = e.what(); }
    }
    transport_->poll();
    for (auto& [key,value] : assets_) {
        (void)key;
        if (value.pending.valid() && value.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            try { value.peaks = value.pending.get(); } catch (const std::exception& e) { waveform_error_ = e.what(); }
    }
}
audio::DeviceStatus Application::device_status() { return device_ ? device_->status() : audio::DeviceStatus{}; }
bool Application::audio_running() { return device_status().phase == audio::DevicePhase::running; }
void Application::play() { require(audio_running(),"connect audio or choose Offline clock first"); transport_->play(); }
void Application::pause() { if (recording_) (void)stop_recording(); else transport_->pause(); }
void Application::stop() { if (recording_) (void)stop_recording(); transport_->stop(); }
void Application::seek(Sample sample) { require_not_recording(); transport_->seek(sample); }

void Application::arm_track(std::optional<Id> id) {
    require_not_recording();
    if (id) {
        const auto p = services_.projects->state().project;
        require(std::any_of(p->tracks.begin(),p->tracks.end(),[&](const auto& t) { return t.id == *id && t.kind == TrackKind::audio; }),"arm an existing audio track");
    }
    armed_ = std::move(id);
}
void Application::monitoring(bool enabled) {
    require(engine_->enqueue({audio::ControlKind::monitor,enabled ? 1 : 0}),"audio command queue full");
    monitoring_ = enabled;
}
audio::RecordStatus Application::recording_status() const { return recording_ ? recording_->status() : last_recording_status_; }
void Application::start_recording(const std::filesystem::path& destination) {
    require_not_playing(); sync_arm(); require(armed_.has_value(),"select an audio track and press Arm track");
    require(audio_running() && device_config_ && device_config_->inputs.size() == 1,"connect one ASIO input in Audio settings");
    require(audio_name_ != "Offline clock (no sound)","recording needs a hardware input; Offline clock cannot record");
    auto position = engine_->state();
    require(!position.loop,"turn off loop before recording");
    const auto p = services_.projects->state().project;
    require(p->clips.size() < audio::max_voices && assets_.size() < 128,"no free clip/source capacity for a recording");
    // Reserve a disk cursor for a long take before opening/writing any media.
    std::size_t disk{}, bytes{};
    for (const auto& clip : p->clips) {
        auto data = asset(clip.source); if (data->file) { ++disk; bytes += 8*8192*static_cast<std::size_t>(data->channels)*sizeof(float); }
    }
    require(disk < 32 && bytes+8*8192*sizeof(float) <= 256*1024*1024,"no disk voice capacity for a recording");
    device_->stop(); position = engine_->state();
    try {
        recording_error_.clear(); last_take_.clear(); last_recording_status_ = {};
        recording_ = std::make_shared<audio::Recorder>(destination,p->sample_rate,position.sample);
        engine_->prepare({device_config_->sample_rate,1,static_cast<std::uint32_t>(device_config_->outputs.size()),8192},render(*device_config_),position);
        transport_->play(); device_->start();
    } catch (...) {
        recording_.reset(); disconnect(); throw;
    }
}
bool Application::stop_recording() {
    if (!recording_) return false;
    try { device_->stop(); }
    catch (const std::exception& e) {
        // Device close guarantees quiescence even if the driver rejected Stop.
        recording_error_ = std::string("Audio device failed; take retained, device disconnected: ")+e.what();
        device_->close(); device_.reset(); device_config_.reset(); audio_name_ = "Disconnected";
    }
    auto position = engine_->state();
    if (position.playback == PlaybackState::playing) position.playback = PlaybackState::paused;
    auto session = std::move(recording_); // detach capture before any file/command work
    try {
        // Detach to a silent paused graph BEFORE drain/asset/file work. A missing
        // backing source must not leave the old capture graph restartable.
        engine_->prepare({session->rate(),device_config_ ? 1U : 0U,
            device_config_ ? static_cast<std::uint32_t>(device_config_->outputs.size()) : 2U,8192},{},position);
        auto result = session->finish(); last_recording_status_ = result.status;
        if (result.status.fault != audio::RecordFault::none) {
            recording_error_ = "Recording ended early (input dropout, disk backpressure, seek or size limit); valid prefix retained";
        } else if (result.status.nonfinite_samples) recording_error_ = "Non-finite input samples replaced with silence";
        if (!result.frames) { rebuild_audio(); return false; }
        last_take_ = result.path;
        const auto source = utf8(result.path);
        edit(AddRecordedClip{{new_id(),*armed_,utf8(result.path.stem()),session->start(),result.frames,0,source}});
        return true;
    } catch (const std::exception& e) {
        recording_error_ = e.what();
        if (!last_take_.empty()) recording_error_ += "; recorded file: "+utf8(last_take_);
        // No capture pointer remains; preserve a playable paused session where possible.
        if (device_ && device_config_) { try { rebuild_audio(); } catch (...) { disconnect(); } }
        throw;
    }
}
} // namespace mrs::desktop
