#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mrs/desktop.hpp>
#include <mrs/version.hpp>
#include <mrs/offline_device.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {
using namespace mrs;
using namespace mrs::desktop;
constexpr COLORREF background = RGB(32,32,32), panel = RGB(44,44,44), border = RGB(65,65,65);
constexpr COLORREF ink = RGB(229,233,245), muted = RGB(154,167,190), accent = RGB(118,104,237), amber = RGB(246,193,97);
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (count <= 0) throw std::runtime_error("invalid UTF-8 text");
    std::wstring out(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),count); return out;
}
std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if (count <= 0) throw std::runtime_error("invalid Unicode text");
    std::string out(static_cast<std::size_t>(count),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count,nullptr,nullptr); return out;
}
std::wstring control_text(HWND control) {
    const int n = GetWindowTextLengthW(control); std::wstring out(static_cast<std::size_t>(n)+1,L'\0');
    GetWindowTextW(control,out.data(),n+1); out.resize(static_cast<std::size_t>(n)); return out;
}
std::uint32_t number(HWND control) {
    auto text = narrow(control_text(control));
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("Enter a whole positive number");
    const auto value = std::stoull(text); if (value > 768000) throw std::invalid_argument("Number out of range");
    return static_cast<std::uint32_t>(value);
}
std::filesystem::path data_folder() {
    std::array<wchar_t,32768> buffer{};
    const auto n = GetEnvironmentVariableW(L"LOCALAPPDATA",buffer.data(),static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) throw std::runtime_error("LOCALAPPDATA is unavailable");
    auto path = std::filesystem::path(buffer.data()) / L"MoonRiverStudio"; std::filesystem::create_directories(path); return path;
}
std::filesystem::path studio_folder() {
    PWSTR documents{};
    const auto result = SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&documents);
    if (FAILED(result)) throw std::runtime_error("Windows Documents folder is unavailable");
    auto path = std::filesystem::path(documents)/L"MR Studio"; CoTaskMemFree(documents); return path;
}
std::filesystem::path pick(HWND owner, bool save, bool wav = false, const std::filesystem::path& initial = {}, std::wstring_view suggested = {}) {
    std::array<wchar_t,32768> path{};
    if (suggested.size() >= path.size()) throw std::invalid_argument("project filename too long");
    std::copy(suggested.begin(),suggested.end(),path.begin());
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
    ofn.lpstrFile = path.data(); ofn.nMaxFile = static_cast<DWORD>(path.size());
    ofn.lpstrFilter = wav ? L"WAV audio\0*.wav\0\0" : L"Moon River project\0*.mrsproject\0All files\0*.*\0\0";
    ofn.lpstrDefExt = wav ? L"wav" : L"mrsproject";
    ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) {
        if (CommDlgExtendedError() != 0) throw std::runtime_error("File dialog failed"); return {};
    }
    return std::filesystem::path(path.data());
}
std::vector<std::filesystem::path> pick_wavs(HWND owner) {
    std::array<wchar_t,65536> paths{};
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
    ofn.lpstrFile = paths.data(); ofn.nMaxFile = static_cast<DWORD>(paths.size());
    ofn.lpstrFilter = L"WAV audio\0*.wav\0\0";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        if (CommDlgExtendedError() != 0) throw std::runtime_error("WAV selection failed"); return {};
    }
    std::filesystem::path first(paths.data()); auto p = paths.data()+wcslen(paths.data())+1;
    if (*p == 0) return {first};
    std::vector<std::filesystem::path> result;
    while (*p != 0) { result.push_back(first / p); p += wcslen(p)+1; }
    return result;
}
enum ControlId {
    nav_arrange = 100, nav_edit, nav_mix, nav_live, play = 110, pause, stop,
    previous, next, loop, undo, redo, open, save, save_as, demo, import,
    audio_settings, tracks = 140, rename_edit, rename,
    new_project_button = 160, import_batch, add_track, delete_track, track_up, track_down, zoom_in, zoom_out, zoom_fit, split_clip_button, delete_clip_button, snap_button,
    record_button = 172, arm_button, monitor_button, files_exit = 180, studio_folder_button,
    device_combo = 200, rate_edit, buffer_edit, outputs_edit, input_edit,
    connect_button, disconnect_button, panel_button, refresh_button
};
struct UI {
    Application app;
    Preferences prefs;
    std::filesystem::path folder;
    Logger log;
    StudioFolders studio;
    HWND window{}, settings{};
    HFONT normal{}, heading{}, big{};
    HBRUSH panel_brush{CreateSolidBrush(panel)};
    UINT dpi{96}, settings_dpi{96};
    HFONT settings_font{};
    bool smoke{};
    int smoke_step{};
    unsigned error_count{};
    std::vector<audio::DeviceInfo> devices;
    std::string device_error;
    RECT canvas{};
    double visible_seconds{32}, view_start{};
    bool fit_view{true};
    std::size_t first_track{};
    std::optional<Id> selected_track, selected_clip;
    bool snap{};
    enum class DragMode { move, left, right };
    struct Drag { Clip original, preview; DragMode mode; Sample anchor{}, frames{}; POINT origin{}; bool changed{}; };
    std::optional<Drag> drag;
    explicit UI(bool test) : folder(data_folder()), log(folder / L"studio.log"), studio{studio_folder()}, smoke(test) {
        studio.ensure();
        if (!smoke) {
            try {
                std::ifstream file(folder / L"desktop.cfg",std::ios::binary);
                if (file) {
                    std::string bytes; std::array<char,16385> data{}; file.read(data.data(),static_cast<std::streamsize>(data.size()));
                    if (file.gcount() > 16384) throw std::runtime_error("config too large");
                    bytes.assign(data.data(),static_cast<std::size_t>(file.gcount())); prefs = decode_preferences(bytes);
                }
            } catch (const std::exception& e) { log.write(e.what()); }
        }
        if (prefs.workspace == Workspace::live) prefs.workspace = Workspace::arrange;
        app.workspace(prefs.workspace); log.write("Studio shell started");
    }
    ~UI() { if (normal) DeleteObject(normal); if (heading) DeleteObject(heading); if (big) DeleteObject(big); if (settings_font) DeleteObject(settings_font); DeleteObject(panel_brush); }
    int s(int dip) const { return MulDiv(dip,static_cast<int>(dpi),96); }
    int ss(int dip) const { return MulDiv(dip,static_cast<int>(settings_dpi),96); }
    void settings_fonts() {
        if (settings_font) DeleteObject(settings_font);
        settings_font = CreateFontW(-ss(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if (settings) for (auto c = GetWindow(settings,GW_CHILD); c; c = GetWindow(c,GW_HWNDNEXT)) SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(settings_font),TRUE);
    }
    HWND child(int id, bool audio = false) const { return GetDlgItem(audio ? settings : window,id); }
    HWND create(HWND parent, const wchar_t* cls, const wchar_t* name, int id, DWORD extra = 0) {
        auto result = CreateWindowExW(0,cls,name,WS_CHILD | WS_VISIBLE | extra,0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if (!result) throw std::runtime_error("Cannot create window control"); return result;
    }
    void button(HWND parent, const wchar_t* text, int id) { create(parent,L"BUTTON",text,id,BS_OWNERDRAW | WS_TABSTOP); }
    void fonts() {
        if (normal) DeleteObject(normal); if (heading) DeleteObject(heading); if (big) DeleteObject(big);
        normal = CreateFontW(-s(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        heading = CreateFontW(-s(23),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        big = CreateFontW(-s(34),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for (HWND parent : {window}) if (parent)
            for (auto control = GetWindow(parent,GW_CHILD); control; control = GetWindow(control,GW_HWNDNEXT)) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(normal),TRUE);
    }
    void file_menu() {
        const auto bar = CreateMenu(), files = CreatePopupMenu();
        if (!bar || !files) {
            if (bar) DestroyMenu(bar); if (files) DestroyMenu(files);
            throw std::runtime_error("Cannot create Files menu");
        }
        bool ok = true;
        const auto item = [&](UINT id, const wchar_t* label) { ok = AppendMenuW(files,MF_STRING,id,label) != FALSE && ok; };
        const auto separator = [&] { ok = AppendMenuW(files,MF_SEPARATOR,0,nullptr) != FALSE && ok; };
        item(new_project_button,L"&New project\tCtrl+N"); item(open,L"&Open project...\tCtrl+O");
        separator(); item(save,L"&Save\tCtrl+S"); item(save_as,L"Save &as...\tCtrl+Shift+S");
        separator(); item(import_batch,L"&Import WAVs...\tCtrl+I"); item(import,L"Open &WAV as new project...");
        item(demo,L"Open &demo project"); separator(); item(studio_folder_button,L"Open studio &folder"); item(files_exit,L"E&xit");
        if (!ok || !AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(files),L"&Files")) {
            DestroyMenu(files); DestroyMenu(bar); throw std::runtime_error("Cannot populate Files menu");
        }
        if (!SetMenu(window,bar)) { DestroyMenu(bar); throw std::runtime_error("Cannot attach Files menu"); }
        DrawMenuBar(window); // native thin row: keyboard navigation and DPI handling
    }
    void initialize() {
        file_menu();
        for (auto [id,label] : std::array<std::pair<int,const wchar_t*>,12>{{
            {nav_arrange,L"Arrange"},{nav_edit,L"Edit"},{nav_mix,L"Mix"},
            {play,L"Play"},{pause,L"Pause"},{stop,L"Stop"},{previous,L"< Section"},{next,L"Section >"},
            {loop,L"Loop section"},{undo,L"Undo"},{redo,L"Redo"},{audio_settings,L"Audio settings"}}}) button(window,label,id);
        for (auto [id,label] : std::array<std::pair<int,const wchar_t*>,7>{{
            {add_track,L"+ Track"},{delete_track,L"Delete"},{track_up,L"Up"},{track_down,L"Down"},
            {zoom_in,L"Zoom +"},{zoom_out,L"Zoom -"},{zoom_fit,L"Fit"}}}) button(window,label,id);
        button(window,L"Record (R)",record_button); button(window,L"Arm track",arm_button); button(window,L"Monitor on",monitor_button);
        button(window,L"Split (S)",split_clip_button); button(window,L"Del clip",delete_clip_button); button(window,L"Snap off",snap_button);
        create(window,L"LISTBOX",L"",tracks,WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT);
        create(window,L"EDIT",L"",rename_edit,WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER);
        SendMessageW(child(rename_edit),EM_SETLIMITTEXT,1024,0); button(window,L"Rename track",rename);
        dpi = GetDpiForWindow(window); fonts(); refresh_models(); layout();
        SetTimer(window,1,33,nullptr); if (smoke) SetTimer(window,2,100,nullptr);
        else PostMessageW(window,WM_APP+1,0,0);
    }
    void move(int id, int x, int y, int width, int height) { MoveWindow(child(id),s(x),s(y),s(width),s(height),TRUE); }
    void layout() {
        RECT area{}; GetClientRect(window,&area); const int width = MulDiv(area.right,96,static_cast<int>(dpi));
        const int height = MulDiv(area.bottom,96,static_cast<int>(dpi));
        for (int i = 0; i < 3; ++i) move(nav_arrange+i,240+i*100,18,92,34);
        move(audio_settings,width-166,18,150,34);
        int x = 20; for (auto id : {play,pause,stop,previous,next,loop}) { int w = id >= previous ? 115 : 76; move(id,x,80,w,34); x += w+8; }
        move(record_button,650,80,90,34); move(arm_button,748,80,100,34); move(monitor_button,856,80,108,34);
        move(split_clip_button,700,196,80,32); move(delete_clip_button,788,196,80,32); move(snap_button,876,196,88,32);
        move(undo,20,140,80,32); move(redo,108,140,80,32);
        int ax = 220;
        for (auto id : {add_track,delete_track,track_up,track_down,zoom_in,zoom_out,zoom_fit}) { move(id,ax,244,84,32); ax += 92; }
        move(tracks,20,228,168,std::max(70,height-388));
        move(rename_edit,20,height-148,168,32); move(rename,20,height-108,168,32);
        canvas = {s(220),s(290),area.right-s(20),area.bottom-s(68)};
        InvalidateRect(window,nullptr,FALSE);
    }
    void refresh_models() {
        const auto project = app.services().projects->state().project;
        app.prepare_waveforms();
        SendMessageW(child(tracks),LB_RESETCONTENT,0,0);
        for (const auto& t : project->tracks) {
            auto name = (app.armed_track() && t.id == *app.armed_track() ? L"[R] " : L"") + wide(t.name); SendMessageW(child(tracks),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        }
        if (!project->tracks.empty()) {
            auto found = std::find_if(project->tracks.begin(),project->tracks.end(),[&](const auto& t) { return selected_track && t.id == *selected_track; });
            if (found == project->tracks.end()) found = project->tracks.begin();
            selected_track = found->id;
            SendMessageW(child(tracks),LB_SETCURSEL,static_cast<WPARAM>(found-project->tracks.begin()),0);
            SetWindowTextW(child(rename_edit),wide(found->name).c_str());
        } else { selected_track.reset(); SetWindowTextW(child(rename_edit),L""); }
        first_track = std::min(first_track,project->tracks.empty() ? std::size_t{0} : project->tracks.size()-1);
        if (selected_clip && std::none_of(project->clips.begin(),project->clips.end(),[&](const auto& c) { return c.id == *selected_clip; })) selected_clip.reset();
        EnableWindow(child(split_clip_button),selected_clip.has_value() && !app.recording()); EnableWindow(child(delete_clip_button),selected_clip.has_value() && !app.recording());
        prefs.rate = project->sample_rate;
        if (settings) SetWindowTextW(child(rate_edit,true),std::to_wstring(prefs.rate).c_str());
        EnableWindow(child(undo),app.services().projects->state().can_undo && !app.recording()); EnableWindow(child(redo),app.services().projects->state().can_redo && !app.recording());
        for (auto id : {play,previous,next,loop,add_track,delete_track,track_up,track_down,rename,rename_edit,audio_settings}) EnableWindow(child(id),!app.recording());
        EnableWindow(child(arm_button),selected_track.has_value() && !app.recording());
        EnableWindow(child(record_button),app.recording() || (app.armed_track().has_value() && app.has_input() && app.audio_running()));
        EnableWindow(child(monitor_button),app.has_input() && app.audio_running());
        SetWindowTextW(child(record_button),app.recording() ? L"End rec (R)" : L"Record (R)");
        const bool armed = selected_track && app.armed_track() && *selected_track == *app.armed_track();
        SetWindowTextW(child(arm_button),armed ? L"Disarm" : L"Arm track");
        SetWindowTextW(child(monitor_button),app.monitoring() ? L"Monitor on" : L"Monitor off");
        const auto files = GetSubMenu(GetMenu(window),0);
        for (auto id : {new_project_button,open,save,save_as,import_batch,import,demo})
            EnableMenuItem(files,static_cast<UINT>(id),MF_BYCOMMAND | (app.recording() ? MF_GRAYED : MF_ENABLED));
        DrawMenuBar(window);
        std::wstring title = L"Moon River Studio " + wide(application_version) + L" — " + wide(project->title) + (app.dirty() ? L" *" : L"");
        SetWindowTextW(window,title.c_str()); InvalidateRect(window,nullptr,FALSE);
    }
    bool discard() {
        if (!app.dirty() || smoke) return true;
        const auto answer = MessageBoxW(window,L"Save changes before replacing the current project?",L"Moon River Studio",MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return false;
        if (answer == IDYES) return save_current(false); return true;
    }
    bool save_current(bool as) {
        auto path = as || app.path().empty() ? pick(window,true,false,studio.projects(),
            app.path().empty() ? wide(app.services().projects->state().project->title)+L".mrsproject" : app.path().filename().wstring()) : app.path();
        if (path.empty()) return false;
        const bool managed = !app.path().empty() &&
            (app.path().parent_path().filename() == app.path().stem() ||
             (std::filesystem::is_directory(app.path().parent_path()/"Media") && std::filesystem::is_directory(app.path().parent_path()/"Mixdown")));
        if (as || !managed) path = project_folder_file(path);
        if (path != app.path() && std::filesystem::exists(path)) throw std::runtime_error("This project folder already exists. Choose a new project name.");
        app.save_project(path); log.write("Project and Media saved"); refresh_models(); return true;
    }
    void preferences() {
        prefs.workspace = app.workspace();
        // Only preferences, not the project. Invalid partial edits are never committed.
        auto bytes = encode_preferences(prefs);
        std::ofstream out(folder / L"desktop.cfg",std::ios::binary | std::ios::trunc);
        out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("Cannot save desktop preferences");
    }
    void cancel_drag() {
        drag.reset(); if (GetCapture() == window) ReleaseCapture(); InvalidateRect(window,nullptr,FALSE);
    }
    int audio_top() const {
        const auto p = app.services().projects->state().project;
        return canvas.top+s(p->chords.empty() && p->sections.empty() ? 70 : 185);
    }
    int sample_x(Sample sample) const {
        const auto rate = app.services().projects->state().project->sample_rate;
        return canvas.left+static_cast<int>(std::clamp((static_cast<double>(sample)/rate-view_start)/visible_seconds*(canvas.right-canvas.left),-100000.0,100000.0));
    }
    Sample sample_at(int x) const {
        const auto rate = app.services().projects->state().project->sample_rate;
        const auto seconds = view_start+static_cast<double>(x-canvas.left)/(canvas.right-canvas.left)*visible_seconds;
        return static_cast<Sample>(std::clamp(seconds*rate,0.0,static_cast<double>(max_sample)));
    }
    std::optional<Id> track_at(int y) const {
        if (y < audio_top()) return {};
        const auto index = first_track+static_cast<std::size_t>((y-audio_top())/s(92));
        const auto p = app.services().projects->state().project;
        return index < p->tracks.size() ? std::optional<Id>{p->tracks[index].id} : std::nullopt;
    }
    std::optional<Clip> hit_clip(POINT point) const {
        if (!PtInRect(&canvas,point) || point.y < audio_top()) return {};
        const auto row = (point.y-audio_top())%s(92);
        if (row < s(26) || row >= s(82)) return {};
        const auto track = track_at(point.y); if (!track) return {};
        const auto p = app.services().projects->state().project;
        for (auto it = p->clips.rbegin(); it != p->clips.rend(); ++it)
            if (it->track == *track && point.x >= sample_x(it->start) && point.x <= sample_x(it->start+it->length)) return *it;
        return {};
    }
    Sample grid(Sample value) const {
        value = std::clamp(value,Sample{0},max_sample);
        return snap && !(GetKeyState(VK_SHIFT)&0x8000) ? snap_to_grid(*app.services().projects->state().project,value) : value;
    }
    void mouse_down(POINT point) {
        if (app.recording() || app.workspace() != Workspace::arrange || !PtInRect(&canvas,point)) return;
        SetFocus(window);
        if (auto clip = hit_clip(point)) {
            selected_clip = clip->id; selected_track = clip->track; refresh_models();
            if (app.engine()->state().playback == PlaybackState::playing) return; // selection is always available
            auto mode = DragMode::move;
            if (std::abs(point.x-sample_x(clip->start)) <= s(7)) mode = DragMode::left;
            else if (std::abs(point.x-sample_x(clip->start+clip->length)) <= s(7)) mode = DragMode::right;
            fit_view = false;
            drag = Drag{*clip,*clip,mode,sample_at(point.x),app.source_frames(clip->id),point,false};
            SetCapture(window);
        } else app.seek(grid(sample_at(point.x))); // ruler/empty space keeps clip selection for Split
    }
    void mouse_move(POINT point) {
        if (!drag) return;
        auto& d = *drag;
        if (!d.changed && std::abs(point.x-d.origin.x) < s(3) && std::abs(point.y-d.origin.y) < s(3)) return;
        d.changed = true; d.preview = d.original;
        const auto delta = sample_at(point.x)-d.anchor;
        if (d.mode == DragMode::move) {
            d.preview.start = std::clamp(grid(std::max(Sample{0},d.original.start+delta)),Sample{0},max_sample-d.original.length);
            if (auto target = track_at(point.y)) {
                const auto p = app.services().projects->state().project;
                auto it = std::find_if(p->tracks.begin(),p->tracks.end(),[&](const auto& t) { return t.id == *target; });
                if (it != p->tracks.end() && it->kind == TrackKind::audio) d.preview.track = *target;
            }
        } else if (d.mode == DragMode::left) {
            const auto finish = d.original.start+d.original.length;
            const auto lower = std::max(Sample{0},d.original.start-d.original.source_offset);
            d.preview.start = std::clamp(grid(std::max(Sample{0},d.original.start+delta)),lower,finish-1);
            d.preview.source_offset += d.preview.start-d.original.start; d.preview.length = finish-d.preview.start;
        } else {
            const auto maximum = std::min(max_sample,d.original.start+d.frames-d.original.source_offset);
            const auto finish = std::clamp(grid(std::max(Sample{0},d.original.start+d.original.length+delta)),d.original.start+1,maximum);
            d.preview.length = finish-d.original.start;
        }
        InvalidateRect(window,nullptr,FALSE);
    }
    void mouse_up(POINT point) {
        if (!drag) return;
        mouse_move(point); auto d = *drag; cancel_drag();
        if (!d.changed || d.preview == d.original) return;
        if (d.mode == DragMode::move) app.move_clip(d.original.id,d.preview.track,d.preview.start);
        else app.trim_clip(d.original.id,d.preview.start,d.preview.start+d.preview.length);
        selected_track = d.preview.track; refresh_models();
    }
    void command(int id, int notification) {
        if (app.recording()) {
            for (auto blocked : {play,previous,next,loop,undo,redo,rename,new_project_button,import_batch,add_track,delete_track,track_up,track_down,split_clip_button,delete_clip_button,open,import,demo,save,save_as,audio_settings,arm_button})
                if (id == blocked) return;
        }
        if (drag) cancel_drag();
        if (id >= nav_arrange && id <= nav_mix) { app.workspace(static_cast<Workspace>(id-nav_arrange)); for (int i = nav_arrange; i <= nav_mix; ++i) InvalidateRect(child(i),nullptr,TRUE); InvalidateRect(window,nullptr,FALSE); return; }
        if (id == tracks && notification == LBN_SELCHANGE) {
            const auto selection = SendMessageW(child(tracks),LB_GETCURSEL,0,0);
            const auto project = app.services().projects->state().project;
            if (selection >= 0 && static_cast<std::size_t>(selection) < project->tracks.size()) {
                selected_track = project->tracks[static_cast<std::size_t>(selection)].id;
                first_track = static_cast<std::size_t>(selection);
                const bool armed = app.armed_track() && *selected_track == *app.armed_track();
                SetWindowTextW(child(arm_button),armed ? L"Disarm" : L"Arm track");
                SetWindowTextW(child(rename_edit),wide(project->tracks[static_cast<std::size_t>(selection)].name).c_str());
                InvalidateRect(window,nullptr,FALSE);
            }
            return;
        }
        switch (id) {
        case play: app.play(); break; case pause: app.pause(); refresh_models(); break; case stop: app.stop(); refresh_models(); break;
        case arm_button:
            if (selected_track) app.arm_track(app.armed_track() == selected_track ? std::nullopt : selected_track);
            refresh_models(); break;
        case monitor_button: app.monitoring(!app.monitoring()); refresh_models(); break;
        case record_button:
            if (app.recording()) (void)app.stop_recording();
            else {
                if (app.path().empty() && !save_current(false)) break;
                auto audio_folder = std::filesystem::absolute(app.path()).parent_path()/L"Media";
                std::filesystem::create_directories(audio_folder);
                app.start_recording(audio_folder/("Take-"+new_id().value+".wav"));
            }
            refresh_models(); break;
        case previous: app.musical().previous_section(); break; case next: app.musical().next_section(); break;
        case loop:
            if (app.services().transport->state().loop) app.musical().clear_loop();
            else if (auto section = app.musical().state().current_section) app.musical().loop_section(section->id);
            break;
        case undo: app.undo(); refresh_models(); break;
        case redo: app.redo(); refresh_models(); break;
        case rename: {
            const auto selection = SendMessageW(child(tracks),LB_GETCURSEL,0,0); const auto project = app.services().projects->state().project;
            if (selection >= 0 && static_cast<std::size_t>(selection) < project->tracks.size()) app.rename_track(project->tracks[static_cast<std::size_t>(selection)].id,narrow(control_text(child(rename_edit))));
            refresh_models(); break;
        }
        case new_project_button: if (discard()) {
            const auto selected = pick(window,true,false,studio.projects(),L"Untitled.mrsproject");
            if (!selected.empty()) {
                const auto path = project_folder_file(selected);
                if (std::filesystem::exists(path)) throw std::runtime_error("This project already exists. Choose a new name.");
                app.new_project(prefs.rate,narrow(path.stem().wstring())); app.save_project(path);
                fit_view = true; view_start = 0; first_track = 0; refresh_models(); restore_audio();
            }
        } break;
        case import_batch: {
            if (app.path().empty() && !save_current(false)) break;
            auto paths = pick_wavs(window);
            if (!paths.empty()) { app.import_wavs(paths); fit_view = true; refresh_models(); }
            break;
        }
        case add_track: selected_track = app.add_audio_track("Audio "+std::to_string(app.services().projects->state().project->tracks.size()+1)); first_track = app.services().projects->state().project->tracks.size()-1; refresh_models(); break;
        case delete_track:
            if (selected_track && MessageBoxW(window,L"Delete the selected track and its clips? Undo restores them.",L"Moon River Studio",MB_YESNO | MB_ICONQUESTION) == IDYES) {
                app.remove_track(*selected_track); refresh_models();
            }
            break;
        case track_up: case track_down: {
            const auto p = app.services().projects->state().project;
            auto it = std::find_if(p->tracks.begin(),p->tracks.end(),[&](const auto& t) { return selected_track && t.id == *selected_track; });
            if (it != p->tracks.end()) {
                auto index = static_cast<std::size_t>(it-p->tracks.begin());
                if (id == track_up && index > 0) --index;
                else if (id == track_down && index+1 < p->tracks.size()) ++index;
                app.reorder_track(it->id,index); first_track = index; refresh_models();
            }
            break;
        }
        case split_clip_button:
            if (selected_clip) { (void)app.split_clip(*selected_clip,app.services().transport->state().sample); refresh_models(); }
            break;
        case delete_clip_button:
            if (selected_clip) { app.remove_clip(*selected_clip); refresh_models(); }
            break;
        case snap_button:
            snap = !snap; SetWindowTextW(child(snap_button),snap ? L"Snap 1/16" : L"Snap off"); break;
        case zoom_in: fit_view = false; visible_seconds = std::max(0.25,visible_seconds/2); break;
        case zoom_out: fit_view = false; visible_seconds = std::min(86400.0,visible_seconds*2); break;
        case zoom_fit: fit_view = true; view_start = 0; break;
        case open: if (discard()) { auto path = pick(window,false,false,studio.projects()); if (!path.empty()) { app.open_project(path); refresh_models(); restore_audio(); } } break;
        case import: if (discard()) { auto path = pick(window,false,true); if (!path.empty()) { app.import_wav(path); refresh_models(); restore_audio(); (void)save_current(false); } } break;
        case demo: if (discard()) { app.demo(); refresh_models(); restore_audio(); } break;
        case save: (void)save_current(false); break; case save_as: (void)save_current(true); break;
        case audio_settings: show_settings(); break;
        case studio_folder_button:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",studio.root.c_str(),nullptr,nullptr,SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Cannot open studio folder");
            break;
        case files_exit: PostMessageW(window,WM_CLOSE,0,0); break;
        }
        InvalidateRect(window,nullptr,FALSE);
    }
    void fill(HDC dc, RECT rect, COLORREF color) {
        const auto brush = CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush);
    }
    void text(HDC dc, int x, int y, int width, int height, std::wstring value, HFONT font = nullptr, COLORREF color = ink) {
        auto previous_font = SelectObject(dc,font ? font : normal); SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT);
        RECT rect{x,y,x+width,y+height}; DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&rect,DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc,previous_font);
    }
    void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color = border) {
        auto pen = CreatePen(PS_SOLID,1,color); auto old = SelectObject(dc,pen);
        MoveToEx(dc,x1,y1,nullptr); LineTo(dc,x2,y2); SelectObject(dc,old); DeleteObject(pen);
    }
    void draw_button(const DRAWITEMSTRUCT& item) {
        bool selected = item.CtlID >= nav_arrange && item.CtlID <= nav_mix && item.CtlID-nav_arrange == static_cast<UINT>(app.workspace());
        const bool rec = item.CtlID == record_button && app.recording();
        fill(item.hDC,item.rcItem,rec ? RGB(148,46,46) : selected ? RGB(88,88,88) : (item.itemState & ODS_SELECTED) ? border : panel);
        auto label = control_text(item.hwndItem); auto old = SelectObject(item.hDC,GetParent(item.hwndItem) == settings ? settings_font : normal);
        SetBkMode(item.hDC,TRANSPARENT); SetTextColor(item.hDC,(item.itemState & ODS_DISABLED) ? muted : ink);
        RECT rect = item.rcItem; DrawTextW(item.hDC,label.c_str(),static_cast<int>(label.size()),&rect,DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (item.itemState & ODS_FOCUS) { InflateRect(&rect,-3,-3); DrawFocusRect(item.hDC,&rect); }
        SelectObject(item.hDC,old);
    }
    void timeline(HDC dc, RECT rect, bool moving) {
        const auto& context = app.musical().state(); const auto project = context.project;
        Timeline time(project->time,project->sample_rate);
        Sample end = 32*static_cast<Sample>(project->sample_rate);
        for (const auto& c : project->clips) end = std::max(end,c.start+c.length);
        for (const auto& section : project->sections) end = std::max(end,time.to_samples(section.end));
        if (fit_view) { visible_seconds = static_cast<double>(end)/project->sample_rate; view_start = 0; }
        const int width = rect.right-rect.left;
        const auto start_tick = std::max<Tick>(0,context.tick-8*ppq);
        const auto pixel = [](double x) { return static_cast<int>(std::clamp(x,-100000.0,100000.0)); };
        const auto x_sample = [&](Sample sample) { return rect.left + pixel((static_cast<double>(sample)/project->sample_rate-view_start)/visible_seconds*width); };
        const auto x_tick = [&](Tick tick) {
            if (moving) return rect.left + pixel(static_cast<double>(tick-start_tick)/(16*ppq)*width);
            return x_sample(time.to_samples(tick));
        };
        fill(dc,rect,panel); const int saved = SaveDC(dc); IntersectClipRect(dc,rect.left,rect.top,rect.right,rect.bottom);
        const int top = rect.top;
        text(dc,rect.left+s(12),top+s(4),width-s(24),s(30),moving ? L"Chord track — shared transport" : L"Drag body: move / edges: trim / S: split / Del: delete",normal,muted);
        int last_label = rect.left-s(64), last_grid = rect.left-s(8);
        for (int bar = 1; bar <= 256; ++bar) {
            const auto tick = time.to_ticks({bar,1,0}); const int x = x_tick(tick);
            if (x > rect.right) break; if (x < rect.left) continue;
            if (x-last_grid >= s(8)) { line(dc,x,top+s(38),x,rect.bottom); last_grid = x; }
            if (x-last_label >= s(64)) {
                text(dc,x+s(6),top+s(36),s(58),s(25),std::to_wstring(bar),normal,muted); last_label = x;
            }
        }
        for (const auto& c : project->chords) {
            RECT block{x_tick(c.start)+1,top+s(76),x_tick(c.end)-1,top+s(138)};
            if (block.right <= rect.left || block.left >= rect.right) continue;
            fill(dc,block,context.current_chord && context.current_chord->id == c.id ? accent : RGB(46,48,75));
            text(dc,block.left+s(8),block.top,block.right-block.left-s(16),block.bottom-block.top,wide(c.symbol),moving ? big : heading);
        }
        for (const auto& section : project->sections) {
            const auto color = RGB((section.color >> 16)&255,(section.color >> 8)&255,section.color&255);
            RECT block{x_tick(section.start)+1,top+s(148),x_tick(section.end)-1,top+s(183)}; fill(dc,block,color);
            text(dc,block.left+s(8),block.top,block.right-block.left-s(16),block.bottom-block.top,wide(section.name));
        }
        if (!moving) {
            int y = top+s(project->chords.empty() && project->sections.empty() ? 70 : 185);
            for (std::size_t ti = first_track; ti < project->tracks.size(); ++ti) {
                const auto& track = project->tracks[ti];
                if (y+s(30) > rect.bottom) break;
                if (selected_track && track.id == *selected_track) fill(dc,{rect.left,y,rect.right,y+s(88)},RGB(34,42,62));
                text(dc,rect.left+s(12),y,width-s(24),s(24),(app.armed_track() && track.id == *app.armed_track() ? L"[R] " : L"") + wide(track.name),normal,muted); y += s(26);
                for (const auto& stored_clip : project->clips) {
                    const auto clip = drag && drag->original.id == stored_clip.id ? drag->preview : stored_clip;
                    if (clip.track != track.id) continue;
                    RECT block{x_sample(clip.start)+1,y,x_sample(clip.start+clip.length)-1,y+s(56)};
                    if (block.right <= rect.left || block.left >= rect.right) continue;
                    fill(dc,block,RGB(38,66,86));
                    const auto peaks = app.waveform(clip.source);
                    if (peaks) {
                        const auto channel_count = std::min<std::uint32_t>(peaks->channels(),2);
                        const int left = std::max(block.left,rect.left), right = std::min(block.right,rect.right);
                        auto peak_pen = CreatePen(PS_SOLID,1,RGB(116,190,214)); auto previous_pen = SelectObject(dc,peak_pen);
                        for (std::uint32_t c = 0; c < channel_count; ++c) {
                            const int ch = s(56)/static_cast<int>(channel_count), mid = y+static_cast<int>(c)*ch+ch/2;
                            line(dc,left,mid,right,mid,RGB(60,89,106));
                            for (int x = left; x < right; ++x) {
                                const auto begin = clip.source_offset+static_cast<Sample>((view_start+static_cast<double>(x-rect.left)/width*visible_seconds)*project->sample_rate)-clip.start;
                                const auto finish = clip.source_offset+static_cast<Sample>((view_start+static_cast<double>(x+1-rect.left)/width*visible_seconds)*project->sample_rate)-clip.start;
                                const auto peak = peaks->range(begin,std::max(begin+1,finish),c);
                                MoveToEx(dc,x,mid-static_cast<int>(std::clamp(peak.maximum,-1.0f,1.0f)*(ch/2-2)),nullptr);
                                LineTo(dc,x,mid-static_cast<int>(std::clamp(peak.minimum,-1.0f,1.0f)*(ch/2-2))+1);
                            }
                        }
                        SelectObject(dc,previous_pen); DeleteObject(peak_pen);
                    } else text(dc,block.left+s(8),y,block.right-block.left-s(16),s(56),L"Building waveform...",normal,muted);
                    if (selected_clip && clip.id == *selected_clip) {
                        line(dc,block.left,block.top,block.right,block.top,amber);
                        line(dc,block.left,block.bottom-1,block.right,block.bottom-1,amber);
                        fill(dc,{block.left,block.top,block.left+s(4),block.bottom},amber);
                        fill(dc,{block.right-s(4),block.top,block.right,block.bottom},amber);
                    }
                    text(dc,block.left+s(7),block.top,block.right-block.left-s(14),s(18),wide(clip.name),normal,ink);
                }
                y += s(66);
            }
        }
        if (!moving && app.recording() && app.armed_track()) {
            const auto track = std::find_if(project->tracks.begin(),project->tracks.end(),[&](const auto& t) { return t.id == *app.armed_track(); });
            const auto row = static_cast<std::size_t>(track-project->tracks.begin());
            if (track != project->tracks.end() && row >= first_track) {
                const auto frames = static_cast<Sample>(app.recording_status().frames);
                const auto start = app.recording_start();
                const int y = audio_top()+static_cast<int>(row-first_track)*s(92)+s(26);
                RECT take{x_sample(start),y,x_sample(start+frames),y+s(56)};
                if (take.right > take.left) { fill(dc,take,RGB(112,43,43)); text(dc,take.left+s(8),y,take.right-take.left-s(16),s(24),L"Recording..."); }
            }
        }
        const auto position = moving ? x_tick(context.tick) : x_sample(context.transport.sample);
        line(dc,position,top+s(62),position,rect.bottom,amber);
        RestoreDC(dc,saved);
    }
    void paint(HDC dc) {
        RECT area{}; GetClientRect(window,&area); fill(dc,area,background);
        fill(dc,{0,s(126),s(204),area.bottom},RGB(36,36,36));
        text(dc,s(20),s(16),s(214),s(38),L"Moon River Studio",heading);
        line(dc,0,s(64),area.right,s(64));
        text(dc,s(220),s(146),area.right-s(240),s(40),wide(app.services().projects->state().project->title),heading);
        const auto& context = app.musical().state();
        std::wostringstream position;
        position << L"Bar " << context.transport.musical.bar << L"  Beat " << context.transport.musical.beat
            << L"    " << std::fixed << std::setprecision(2) << static_cast<double>(context.transport.sample)/context.project->sample_rate << L" s";
        text(dc,s(220),s(196),s(470),s(35),position.str(),heading,amber);
        text(dc,s(20),s(194),s(168),s(28),L"Project tracks",normal,muted);
        auto workspace = app.workspace();
        for (auto id : {add_track,delete_track,track_up,track_down,zoom_in,zoom_out,zoom_fit,split_clip_button,delete_clip_button,snap_button}) ShowWindow(child(id),workspace == Workspace::arrange ? SW_SHOW : SW_HIDE);
        if (workspace == Workspace::arrange) {
            timeline(dc,canvas,false);
        } else {
            fill(dc,canvas,panel); int y = canvas.top+s(14);
            text(dc,canvas.left+s(16),y,canvas.right-canvas.left-s(32),s(36),workspace == Workspace::mix ? L"Shared processor graph" : L"Clip inspector",heading); y += s(52);
            if (workspace == Workspace::mix) {
                const auto graph = app.graphs()->state().graph;
                text(dc,canvas.left+s(16),y,canvas.right-canvas.left-s(32),s(30),wide(graph->patch_name)); y += s(44);
                for (const auto& n : graph->nodes) {
                    text(dc,canvas.left+s(16),y,canvas.right-canvas.left-s(32),s(30),wide(n.processor_id)+(n.bypass ? L"  [bypass]" : L""),heading); y += s(36);
                    for (const auto& p : n.parameters) { text(dc,canvas.left+s(16),y,canvas.right-canvas.left-s(32),s(25),L"Parameter "+std::to_wstring(p.id)+L" = "+std::to_wstring(p.value),normal,muted); y += s(28); }
                }
                text(dc,canvas.left+s(16),y+s(20),canvas.right-canvas.left-s(32),s(30),L"Processor and mixer state view. Editing follows in the Mix stage.",normal,muted);
            } else {
                for (const auto& c : context.project->clips) {
                    text(dc,canvas.left+s(16),y,canvas.right-canvas.left-s(32),s(32),wide(c.name),heading); y += s(34);
                    text(dc,canvas.left+s(16),y,canvas.right-canvas.left-s(32),s(28),L"Start: "+std::to_wstring(c.start)+L"   Length: "+std::to_wstring(c.length)+L" samples",normal,muted); y += s(38);
                }
                text(dc,canvas.left+s(16),y+s(20),canvas.right-canvas.left-s(32),s(30),L"Shared clip state. Detailed editing follows in the Arrangement stage.",normal,muted);
            }
        }
        const auto status = app.device_status(); const auto metrics = app.engine()->metrics();
        std::wostringstream bottom; bottom << wide(app.audio_name()) << L"  |  " << (context.transport.playback == PlaybackState::playing ? L"Playing" : context.transport.playback == PlaybackState::paused ? L"Paused" : L"Stopped")
            << L"  |  " << context.project->sample_rate << L" Hz  |  callbacks " << metrics.callbacks << L"  |  underruns " << metrics.output_underflows
            << L"  |  disk underruns " << metrics.disk_underruns << L" / errors " << metrics.disk_errors;
        std::wostringstream input_status;
        input_status << L"Input " << std::fixed << std::setprecision(1) << (metrics.input_peak > 0 ? 20*std::log10(metrics.input_peak) : -120.0f) << L" dBFS";
        if (app.recording()) input_status << L"  |  Recording " << static_cast<double>(app.recording_status().frames)/context.project->sample_rate << L" s";
        if (!app.recording_error().empty()) input_status << L"  |  " << wide(app.recording_error());
        text(dc,s(650),s(117),area.right-s(670),s(24),input_status.str(),normal,app.recording_error().empty() ? muted : RGB(242,100,100));
        if (!app.waveform_error().empty()) bottom << L"  |  waveform: " << wide(app.waveform_error());
        line(dc,0,area.bottom-s(48),area.right,area.bottom-s(48));
        text(dc,s(20),area.bottom-s(44),area.right-s(40),s(36),bottom.str(),normal,(status.phase == audio::DevicePhase::error || metrics.disk_errors || metrics.disk_underruns) ? RGB(242,100,100) : muted);
    }
    void restore_audio();
    void show_settings();
    void settings_command(int);
    void settings_layout();
    void enumerate_devices();
    void error(const std::exception& e) { ++error_count; log.write(e.what()); if (!smoke) MessageBoxW(window,wide(e.what()).c_str(),L"Moon River Studio",MB_OK | MB_ICONERROR); }
};
LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
void UI::restore_audio() {
    if (!prefs.reconnect_audio || prefs.device_name.empty()) return;
#ifdef MRS_HAS_ASIO
    auto device = audio::make_asio_device();
    const auto available = device->enumerate();
    const auto found = std::find_if(available.begin(),available.end(),[&](const auto& info) { return info.name == prefs.device_name; });
    if (found == available.end()) throw std::runtime_error("Saved ASIO device is unavailable. Choose a device in Audio settings.");
    // Resolve the saved name afresh: enumeration indices may change between sessions.
    audio::DeviceConfig config{found->index,app.services().projects->state().project->sample_rate,prefs.buffer,{},prefs.outputs};
    if (prefs.monitor_input >= 0) config.inputs = {prefs.monitor_input};
    app.connect(std::move(device),config); prefs.rate = config.sample_rate; preferences();
    refresh_models(); log.write("Saved ASIO connection restored; transport stopped");
    if (settings) {
        SetWindowTextW(child(rate_edit,true),std::to_wstring(prefs.rate).c_str());
        InvalidateRect(settings,nullptr,FALSE);
    }
#else
    throw std::runtime_error("Saved ASIO connection requires the ASIO build.");
#endif
}
void UI::enumerate_devices() {
    devices.clear(); device_error.clear();
    SendMessageW(child(device_combo,true),CB_RESETCONTENT,0,0);
    SendMessageW(child(device_combo,true),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Offline clock (no sound)"));
#ifdef MRS_HAS_ASIO
    try { auto asio = audio::make_asio_device(); devices = asio->enumerate(); }
    catch (const std::exception& e) { device_error = e.what(); log.write(e.what()); }
#else
    device_error = "ASIO support is not included in this build.";
#endif
    int selected{};
    for (std::size_t i = 0; i < devices.size(); ++i) {
        auto label = wide(devices[i].name); SendMessageW(child(device_combo,true),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if (devices[i].name == prefs.device_name) selected = static_cast<int>(i)+1;
    }
    SendMessageW(child(device_combo,true),CB_SETCURSEL,static_cast<WPARAM>(selected),0);
}
void UI::show_settings() {
    if (settings) { ShowWindow(settings,SW_SHOW); SetForegroundWindow(settings); return; }
    settings = CreateWindowExW(WS_EX_CONTROLPARENT,L"MRStudioAudio",L"Audio settings",WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,s(600),s(475),window,nullptr,GetModuleHandleW(nullptr),this);
    if (!settings) throw std::runtime_error("Cannot open audio settings");
    settings_dpi = GetDpiForWindow(settings);
    create(settings,L"COMBOBOX",L"",device_combo,WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    for (auto id : {rate_edit,buffer_edit,outputs_edit,input_edit}) { create(settings,L"EDIT",L"",id,WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER); SendMessageW(child(id,true),EM_SETLIMITTEXT,256,0); }
    for (auto [id,label] : std::array<std::pair<int,const wchar_t*>,4>{{{connect_button,L"Connect"},{disconnect_button,L"Disconnect"},{panel_button,L"ASIO panel"},{refresh_button,L"Refresh"}}}) button(settings,label,id);
    SetWindowTextW(child(rate_edit,true),std::to_wstring(prefs.rate).c_str()); SetWindowTextW(child(buffer_edit,true),std::to_wstring(prefs.buffer).c_str());
    std::wstring outputs; for (auto o : prefs.outputs) { if (!outputs.empty()) outputs += L","; outputs += std::to_wstring(o+1); }
    SetWindowTextW(child(outputs_edit,true),outputs.c_str()); SetWindowTextW(child(input_edit,true),std::to_wstring(prefs.monitor_input+1).c_str());
    settings_fonts(); enumerate_devices(); settings_layout();
    SetWindowPos(settings,nullptr,0,0,ss(600),ss(475),SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(settings,SW_SHOW);
}
void UI::settings_layout() {
    auto move = [&](int id, int x, int y, int w, int h) { MoveWindow(child(id,true),ss(x),ss(y),ss(w),ss(h),TRUE); };
    move(device_combo,190,24,355,250); move(rate_edit,190,76,150,30); move(buffer_edit,190,118,150,30);
    move(outputs_edit,190,160,150,30); move(input_edit,190,202,150,30);
    move(connect_button,20,252,110,34); move(disconnect_button,140,252,120,34); move(panel_button,270,252,120,34); move(refresh_button,400,252,110,34);
    InvalidateRect(settings,nullptr,FALSE);
}
void UI::settings_command(int id) {
    if (app.recording()) return;
    if (id == refresh_button) { enumerate_devices(); InvalidateRect(settings,nullptr,FALSE); return; }
    if (id == disconnect_button) { app.disconnect(); prefs.reconnect_audio = false; preferences(); refresh_models(); InvalidateRect(settings,nullptr,FALSE); return; }
    // Edit/combo initialization and typing notifications are not device actions.
    if (id != connect_button && id != panel_button) return;
    const auto selection = SendMessageW(child(device_combo,true),CB_GETCURSEL,0,0);
    if (selection < 0 || static_cast<std::size_t>(selection) > devices.size()) throw std::runtime_error("Select an available audio device");
    if (id == panel_button) {
#ifdef MRS_HAS_ASIO
        if (selection == 0) throw std::runtime_error("Offline clock has no ASIO panel");
        // Panel/reconfigure is quiescent; user reconnects after driver changes.
        app.disconnect(); auto device = audio::make_asio_device(); device->control_panel(devices[static_cast<std::size_t>(selection)-1].index);
        InvalidateRect(settings,nullptr,FALSE); return;
#else
        throw std::runtime_error("This build has no ASIO backend");
#endif
    }
    if (id != connect_button) return;
    auto next_prefs = prefs; next_prefs.rate = number(child(rate_edit,true)); next_prefs.buffer = number(child(buffer_edit,true));
    next_prefs.outputs = parse_outputs(narrow(control_text(child(outputs_edit,true))));
    const auto input = number(child(input_edit,true)); if (input > 64) throw std::invalid_argument("Input must be 0 (off) or a channel 1..64");
    next_prefs.monitor_input = static_cast<int>(input)-1;
    next_prefs.device_name = selection == 0 ? "" : devices[static_cast<std::size_t>(selection)-1].name; next_prefs.reconnect_audio = selection != 0; next_prefs.validate();
    audio::DeviceConfig config{selection == 0 ? 0 : devices[static_cast<std::size_t>(selection)-1].index,next_prefs.rate,next_prefs.buffer,{},next_prefs.outputs};
    if (next_prefs.monitor_input >= 0) config.inputs = {next_prefs.monitor_input};
    std::unique_ptr<audio::IAudioDevice> device;
    if (selection == 0) device = audio::make_offline_device();
#ifdef MRS_HAS_ASIO
    else device = audio::make_asio_device();
#endif
    app.connect(std::move(device),config); prefs = std::move(next_prefs); preferences(); refresh_models(); log.write("Audio connected");
    InvalidateRect(settings,nullptr,FALSE);
}
LRESULT CALLBACK settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto ui = reinterpret_cast<UI*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message == WM_NCCREATE) { ui = static_cast<UI*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(ui)); }
    if (!ui) return DefWindowProcW(hwnd,message,wparam,lparam);
    try {
        switch (message) {
        case WM_DPICHANGED: {
            ui->settings_dpi = HIWORD(wparam); const auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER | SWP_NOACTIVATE);
            ui->settings_fonts(); ui->settings_layout(); return 0;
        }
        case WM_COMMAND: ui->settings_command(LOWORD(wparam)); return 0;
        case WM_DRAWITEM: ui->draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wparam),ink); SetBkColor(reinterpret_cast<HDC>(wparam),panel); return reinterpret_cast<LRESULT>(ui->panel_brush);
        case WM_PAINT: {
            PAINTSTRUCT ps{}; auto dc = BeginPaint(hwnd,&ps); RECT area{}; GetClientRect(hwnd,&area); ui->fill(dc,area,background);
            const std::array<const wchar_t*,5> labels{L"Audio backend",L"Sample rate (Hz)",L"Buffer (frames)",L"Physical outputs",L"Monitor input"};
            const std::array<int,5> ys{24,76,118,160,202};
            for (std::size_t i = 0; i < labels.size(); ++i) ui->text(dc,ui->ss(20),ui->ss(ys[i]),ui->ss(166),ui->ss(30),labels[i],ui->settings_font);
            auto status = ui->app.device_status(); 
            ui->text(dc,ui->ss(20),ui->ss(305),ui->ss(540),ui->ss(30),wide(ui->app.audio_name()),ui->settings_font,muted);
            ui->text(dc,ui->ss(20),ui->ss(337),ui->ss(540),ui->ss(30),L"Output latency: "+std::to_wstring(status.output_latency_ms)+L" ms   CPU: "+std::to_wstring(status.cpu_load*100)+L"%",ui->settings_font,muted);
            ui->text(dc,ui->ss(20),ui->ss(370),ui->ss(540),ui->ss(30),ui->device_error.empty() ? L"Outputs: 1,2. Monitor input: 0 = off. Connect stops/reset transport." : wide(ui->device_error),ui->settings_font,muted);
            EndPaint(hwnd,&ps); return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: ui->settings = nullptr; return 0;
        }
    } catch (const std::exception& e) { ui->error(e); }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto ui = reinterpret_cast<UI*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        ui = static_cast<UI*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); ui->window = hwnd;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(ui));
    }
    if (!ui) return DefWindowProcW(hwnd,message,wparam,lparam);
    try {
        switch (message) {
        case WM_CREATE: ui->initialize(); return 0;
        case WM_APP+1: ui->restore_audio(); InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_SIZE: if (ui->normal) ui->layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize = {ui->s(1000),ui->s(620)}; return 0;
        }
        case WM_DPICHANGED: {
            ui->dpi = HIWORD(wparam); const auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER | SWP_NOACTIVATE);
            ui->fonts(); ui->layout(); if (ui->settings) ui->settings_layout(); return 0;
        }
        case WM_COMMAND: ui->command(LOWORD(wparam),HIWORD(wparam)); return 0;
        case WM_DRAWITEM: ui->draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wparam),ink); SetBkColor(reinterpret_cast<HDC>(wparam),panel); return reinterpret_cast<LRESULT>(ui->panel_brush);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; auto dc = BeginPaint(hwnd,&ps); RECT area{}; GetClientRect(hwnd,&area);
            auto buffer = CreateCompatibleDC(dc); auto bitmap = CreateCompatibleBitmap(dc,std::max<LONG>(1,area.right),std::max<LONG>(1,area.bottom));
            auto old = SelectObject(buffer,bitmap); ui->paint(buffer); BitBlt(dc,0,0,area.right,area.bottom,buffer,0,0,SRCCOPY);
            SelectObject(buffer,old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(hwnd,&ps); return 0;
        }
        case WM_TIMER:
            if (wparam == 1) {
                try { const bool recording = ui->app.recording(); ui->app.poll(); if (recording != ui->app.recording()) ui->refresh_models(); } catch (const std::runtime_error&) { return 0; } // bounded mailbox can be busy
                InvalidateRect(hwnd,nullptr,FALSE); if (ui->settings) InvalidateRect(ui->settings,nullptr,FALSE); return 0;
            }
            if (wparam == 2 && ui->smoke) {
                ++ui->smoke_step;
                if (ui->smoke_step <= 3) ui->command(nav_arrange+ui->smoke_step-1,BN_CLICKED);
                if (ui->smoke_step == 5) {
                    ui->command(nav_arrange,BN_CLICKED);
                    const auto revision = ui->app.services().projects->state().revision;
                    POINT origin{(ui->canvas.left+ui->canvas.right)/2,ui->audio_top()+ui->s(42)};
                    POINT destination{origin.x+ui->s(30),origin.y};
                    ui->mouse_down(origin); ui->mouse_move(destination);
                    if (!ui->drag || ui->app.services().projects->state().revision != revision)
                        throw std::runtime_error("Drag preview unexpectedly changed project state");
                    ui->mouse_up(destination);
                    if (ui->app.services().projects->state().revision != revision+1)
                        throw std::runtime_error("Drag did not commit exactly one edit");
                    ui->command(undo,0);
                    const auto undone = ui->app.services().projects->state().revision;
                    ui->mouse_down(origin); ui->mouse_move(destination); ui->cancel_drag();
                    if (ui->app.services().projects->state().revision != undone)
                        throw std::runtime_error("Cancelled drag changed project state");
                    ui->command(nav_mix,BN_CLICKED);
                    const auto bar = GetMenu(hwnd), files = GetSubMenu(bar,0);
                    if (!bar || !files || GetMenuItemID(files,0) != new_project_button || GetMenuItemID(files,3) != save)
                        throw std::runtime_error("Files menu did not retain project commands");
                    if (GetMenuItemID(files,10) != studio_folder_button || !std::filesystem::is_directory(ui->studio.projects()) || !std::filesystem::is_directory(ui->studio.lives()))
                        throw std::runtime_error("Studio content folders/menu missing");
                    for (auto id : {record_button,arm_button,monitor_button}) if (!ui->child(id)) throw std::runtime_error("Recording controls missing");
                    ui->command(arm_button,0);
                    if (!ui->app.armed_track()) throw std::runtime_error("Arm track did not use shared application");
                    ui->command(arm_button,0);
                    if (ui->app.armed_track()) throw std::runtime_error("Disarm did not clear shared application");
                    for (auto id : {nav_live,open,save,save_as,demo,import,new_project_button,import_batch})
                        if (ui->child(id)) throw std::runtime_error("File/Live controls still occupy the workspace");
                    ui->app.rename_track(ui->app.services().projects->state().project->tracks.front().id,"Smoke track"); ui->refresh_models();
                    ui->show_settings(); // offline CI build skips enumeration of physical ASIO
                }
                if (ui->smoke_step == 6) {
                    // Editing incomplete values, including an unselected combo, must not
                    // validate/open a device until the explicit Connect action.
                    SendMessageW(ui->child(device_combo,true),CB_SETCURSEL,static_cast<WPARAM>(-1),0);
                    SetWindowTextW(ui->child(rate_edit,true),L"");
                    SetWindowTextW(ui->child(outputs_edit,true),L"1,");
                    SendMessageW(ui->settings,WM_COMMAND,MAKEWPARAM(device_combo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(ui->child(device_combo,true)));
                    SendMessageW(ui->child(device_combo,true),CB_SETCURSEL,0,0);
                    SetWindowTextW(ui->child(rate_edit,true),L"48000");
                    SetWindowTextW(ui->child(outputs_edit,true),L"1,2");
                    if (!ui->child(play) || !ui->child(device_combo,true) || ui->app.workspace() != Workspace::mix || ui->error_count != 0)
                        throw std::runtime_error("GUI initialization/typing produced an unexpected error");
                    // Exercise DPI layout with the same path as a monitor change.
                    ui->dpi = 144; ui->fonts();
                    SetWindowPos(hwnd,nullptr,0,0,ui->s(1000),ui->s(620),SWP_NOMOVE | SWP_NOZORDER);
                    ui->layout(); ui->settings_dpi = 144; ui->settings_fonts();
                    SetWindowPos(ui->settings,nullptr,0,0,ui->ss(600),ui->ss(475),SWP_NOMOVE | SWP_NOZORDER); ui->settings_layout();
                    for (auto parent : {hwnd,ui->settings}) {
                        RECT area{}; GetClientRect(parent,&area);
                        for (auto control = GetWindow(parent,GW_CHILD); control; control = GetWindow(control,GW_HWNDNEXT)) {
                            RECT rect{}; GetWindowRect(control,&rect); MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rect),2);
                            if (rect.left < 0 || rect.top < 0 || rect.right > area.right || rect.bottom > area.bottom)
                                throw std::runtime_error("DPI layout extends outside the client area");
                        }
                    }
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            if (ui->drag || ui->app.workspace() != Workspace::arrange) break;
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd,&point);
            if (!PtInRect(&ui->canvas,point)) break;
            const int direction = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
            if (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) {
                ui->fit_view = false; ui->view_start = std::max(0.0,ui->view_start+direction*ui->visible_seconds/5);
            } else {
                const auto count = ui->app.services().projects->state().project->tracks.size();
                if (direction < 0 && ui->first_track > 0) --ui->first_track;
                if (direction > 0 && ui->first_track+1 < count) ++ui->first_track;
            }
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        case WM_LBUTTONDOWN: ui->mouse_down({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
        case WM_MOUSEMOVE: ui->mouse_move({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
        case WM_LBUTTONUP: ui->mouse_up({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
        case WM_CAPTURECHANGED: ui->drag.reset(); InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && ui->app.workspace() == Workspace::arrange) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd,&point);
                if (auto clip = ui->hit_clip(point)) {
                    const bool edge = std::abs(point.x-ui->sample_x(clip->start)) <= ui->s(7) || std::abs(point.x-ui->sample_x(clip->start+clip->length)) <= ui->s(7);
                    SetCursor(LoadCursorW(nullptr,edge ? IDC_SIZEWE : IDC_SIZEALL)); return TRUE;
                }
            }
            break;
        case WM_CLOSE:
            if (ui->app.recording()) { (void)ui->app.stop_recording(); ui->refresh_models(); }
            if (ui->discard()) { ui->preferences(); DestroyWindow(hwnd); } return 0;
        case WM_DESTROY:
            KillTimer(hwnd,1); KillTimer(hwnd,2); if (ui->settings) DestroyWindow(ui->settings);
            ui->app.disconnect(); PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& e) {
        ui->error(e);
        if (ui->smoke || message == WM_CREATE) { PostQuitMessage(1); if (message == WM_CREATE) return -1; }
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    try {
        const bool smoke = std::wstring_view(command_line).find(L"--smoke-test") != std::wstring_view::npos;
        UI ui(smoke);
        WNDCLASSW main{}; main.lpfnWndProc = main_proc; main.hInstance = instance; main.lpszClassName = L"MRStudioDesktop";
        main.hCursor = LoadCursorW(nullptr,IDC_ARROW);
        if (!RegisterClassW(&main)) throw std::runtime_error("Cannot register main window");
        WNDCLASSW settings = main; settings.lpfnWndProc = settings_proc; settings.lpszClassName = L"MRStudioAudio";
        if (!RegisterClassW(&settings)) throw std::runtime_error("Cannot register settings window");
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT,main.lpszClassName,L"Moon River Studio",WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,CW_USEDEFAULT,1280,850,nullptr,nullptr,instance,&ui);
        if (!window) throw std::runtime_error("Cannot create Studio window");
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor); GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        SetWindowPos(window,nullptr,monitor.rcWork.left,monitor.rcWork.top,std::min<LONG>(ui.s(1280),monitor.rcWork.right-monitor.rcWork.left),std::min<LONG>(ui.s(850),monitor.rcWork.bottom-monitor.rcWork.top),SWP_NOZORDER);
        ShowWindow(window,show); UpdateWindow(window);
        MSG msg{};
        for (;;) {
            const auto result = GetMessageW(&msg,nullptr,0,0); if (result < 0) return 1; if (result == 0) break;
            if (msg.message == WM_KEYDOWN && !(ui.settings && IsChild(ui.settings,msg.hwnd))) { try {
                const bool editing = msg.hwnd == ui.child(rename_edit);
                if (!editing && msg.wParam == VK_ESCAPE && ui.drag) { ui.cancel_drag(); continue; }
                if (!editing && ui.app.workspace() == Workspace::arrange && !(GetKeyState(VK_CONTROL)&0x8000)) {
                    if (msg.wParam == 'R') { ui.command(record_button,0); continue; }
                    if (msg.wParam == 'S') { ui.command(split_clip_button,0); continue; }
                    if (msg.wParam == VK_DELETE) { ui.command(delete_clip_button,0); continue; }
                }
                if (!editing && msg.wParam == VK_SPACE) { ui.command(ui.app.services().transport->state().playback == PlaybackState::playing ? pause : play,0); continue; }
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    int id{}; if (msg.wParam == 'S') id = (GetKeyState(VK_SHIFT)&0x8000) ? save_as : save;
                    if (!editing && msg.wParam == 'N') id = new_project_button;
                    if (!editing && msg.wParam == 'O') id = open;
                    if (!editing && msg.wParam == 'I') id = import_batch; if (!editing && msg.wParam == 'Z') id = undo; if (!editing && msg.wParam == 'Y') id = redo;
                    if (id) { ui.command(id,0); continue; }
                }
            } catch (const std::exception& e) { ui.error(e); continue; } }
            if (ui.settings && IsDialogMessageW(ui.settings,&msg)) continue;
            if (IsDialogMessageW(window,&msg)) continue;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    } catch (const std::exception& e) {
        if (std::wstring_view(command_line).find(L"--smoke-test") == std::wstring_view::npos) MessageBoxW(nullptr,wide(e.what()).c_str(),L"Moon River Studio",MB_OK | MB_ICONERROR);
        return 1;
    }
}
