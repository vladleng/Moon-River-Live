#include <mrs/project_folders.hpp>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace mrs::desktop {
namespace fs = std::filesystem;
namespace {
void directory(const fs::path& path) {
    if (fs::is_symlink(path)) throw std::invalid_argument("project content directory must not be a symbolic link");
    fs::create_directories(path);
}
bool inside(const fs::path& path, const fs::path& parent) {
    const auto rel = path.lexically_relative(parent);
    return !rel.empty() && !rel.is_absolute() && *rel.begin() != "..";
}
void publish(const fs::path& partial, const fs::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(partial.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot publish copied project media (existing files are never overwritten)");
#else
    fs::create_hard_link(partial,destination);
    std::error_code ec; fs::remove(partial,ec);
#endif
}
}
void StudioFolders::ensure() const { directory(root); directory(projects()); directory(lives()); }
fs::path project_folder_file(const fs::path& selected) {
    if (selected.filename().empty() || selected.stem().empty() || selected.stem() == "." || selected.stem() == "..")
        throw std::invalid_argument("choose a project filename");
    auto file = fs::absolute(selected).lexically_normal(); file.replace_extension(".mrsproject");
    if (file.parent_path().filename() == file.stem() ||
        (fs::exists(file) && fs::is_directory(file.parent_path()/"Media") && fs::is_directory(file.parent_path()/"Mixdown"))) return file;
    return file.parent_path()/file.stem()/file.filename();
}
void ensure_project_folders(const fs::path& project_file) {
    const auto root = fs::absolute(project_file).parent_path();
    directory(root); directory(root/"Media"); directory(root/"Mixdown");
}
MediaCopy::MediaCopy(fs::path folder) : root_(fs::absolute(folder).lexically_normal()) {
    directory(root_); directory(root_/"Media"); directory(root_/"Mixdown");
}
MediaCopy::~MediaCopy() {
    if (!committed_) for (const auto& path : created_) { std::error_code ec; fs::remove(path,ec); }
}
fs::path MediaCopy::copy(const fs::path& source, const fs::path& relative) {
    if (!fs::is_regular_file(source)) throw std::runtime_error("project media is missing or is not a regular file");
    const auto actual = fs::canonical(source);
    if (const auto found = destinations_.find(actual); found != destinations_.end()) return found->second;
    auto destination = root_/relative;
    directory(destination.parent_path());
    if (!inside(fs::weakly_canonical(destination.parent_path()),fs::canonical(root_)))
        throw std::invalid_argument("project content path escapes the project folder");
    if (fs::exists(destination) && fs::equivalent(actual,destination)) return destinations_[actual] = destination;
    if (fs::exists(destination)) {
        auto name = fs::path(new_id().value+"-"); name += destination.filename().native();
        destination = destination.parent_path()/name;
    }
    auto temporary = destination.parent_path()/("mrs-copy-"+new_id().value+".partial");
    bool made{}, published{};
    try {
        made = fs::copy_file(actual,temporary,fs::copy_options::none);
        if (!made || fs::file_size(actual) != fs::file_size(temporary)) throw std::runtime_error("project media changed during copy");
        // Record the intended publication before it can become externally visible.
        created_.push_back(destination);
        publish(temporary,destination); published = true; made = false;
        destinations_[actual] = destination; return destination;
    } catch (...) {
        // A failed publish never owns an existing destination.
        if (!published && !created_.empty() && created_.back() == destination) created_.pop_back();
        if (made) { std::error_code ec; fs::remove(temporary,ec); }
        throw;
    }
}
fs::path MediaCopy::media(const fs::path& source) {
    const auto actual = fs::canonical(source);
    const auto media_root = fs::canonical(root_/"Media");
    if (inside(actual,media_root)) {
        auto destination = root_/"Media"/actual.lexically_relative(media_root);
        destinations_[actual] = destination; return destination;
    }
    return copy(actual,fs::path("Media")/source.filename());
}
void MediaCopy::content(const fs::path& old_root, const char* name) {
    const auto source = old_root/name;
    if (!fs::exists(source)) return;
    if (fs::equivalent(source,root_/name)) return;
    if (fs::is_symlink(source)) throw std::invalid_argument("content directory symbolic links cannot be copied");
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_symlink()) throw std::invalid_argument("content symbolic links cannot be copied");
        const auto relative = fs::path(name)/entry.path().lexically_relative(source);
        if (entry.is_directory()) directory(root_/relative);
        else if (entry.is_regular_file()) (void)copy(entry.path(),relative);
        else throw std::invalid_argument("unsupported project content file");
    }
}
}
