#pragma once
#include <mrs/core.hpp>
#include <filesystem>
#include <map>
namespace mrs::desktop {
struct StudioFolders {
    std::filesystem::path root;
    std::filesystem::path projects() const { return root/"Projects"; }
    std::filesystem::path lives() const { return root/"Lives"; }
    void ensure() const;
};
// A save picker selects <parent>/<name>.mrsproject; the project owns <parent>/<name>/.
std::filesystem::path project_folder_file(const std::filesystem::path& selected);
void ensure_project_folders(const std::filesystem::path& project_file);
// Control thread only. Copy media without overwrite; roll back only files we created
// until the archive/command commits. Never delete user originals or existing files.
class MediaCopy {
public:
    explicit MediaCopy(std::filesystem::path project_folder);
    ~MediaCopy();
    MediaCopy(const MediaCopy&) = delete;
    std::filesystem::path media(const std::filesystem::path& source);
    void content(const std::filesystem::path& old_root, const char* directory);
    bool copied() const { return !created_.empty(); }
    void commit() noexcept { committed_ = true; }
private:
    std::filesystem::path root_;
    std::map<std::filesystem::path,std::filesystem::path> destinations_;
    std::vector<std::filesystem::path> created_;
    bool committed_{};
    std::filesystem::path copy(const std::filesystem::path&, const std::filesystem::path& relative);
};
}
