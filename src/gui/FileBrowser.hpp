#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Minimal ImGui file dialog (modal popup).
//
//   if (ImGui::Button("..."))
//       browser.open("Select DBC", FileBrowser::Mode::Open, {".dbc"}, current_path);
//   if (auto p = browser.render())   // every frame, same ID scope as open()
//       use(*p);
class FileBrowser {
public:
    enum class Mode { Open, Save };

    // extensions: e.g. {".dbc"}; empty = all files. `start` may be a file
    // (its directory is opened and its name preselected) or a directory.
    // content (optional): files with another extension are shown too if it returns true (e.g. ELF magic);
    // content_label describes it in the filter line ("ELF executables").
    void open(std::string title, Mode mode, std::vector<std::string> extensions,
              const std::filesystem::path& start = {},
              std::function<bool(const std::filesystem::path&)> content = {}, std::string content_label = {});

    // True if the file starts with the ELF magic (0x7F 'E' 'L' 'F').
    static bool is_elf(const std::filesystem::path& p);

    // Returns the chosen path once, on confirmation.
    std::optional<std::filesystem::path> render();

    bool is_open() const { return visible_; }

private:
    struct Entry {
        std::string name;
        bool        is_dir;
    };

    void refresh();
    bool matches(const std::filesystem::path& p) const;

    std::string                title_;
    Mode                       mode_ = Mode::Open;
    std::vector<std::string>   extensions_;
    std::function<bool(const std::filesystem::path&)> content_;
    std::string                content_label_;
    std::filesystem::path      dir_;
    std::vector<Entry>         entries_;
    std::string                file_name_;
    std::string                dir_input_;
    std::string                error_;
    bool                       show_all_  = false;
    bool                       visible_   = false;
    bool                       open_next_ = false;
    bool                       confirm_overwrite_ = false;
};
