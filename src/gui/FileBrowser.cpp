#include "FileBrowser.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace fs = std::filesystem;

void FileBrowser::open(std::string title, Mode mode, std::vector<std::string> extensions,
                       const fs::path& start, std::function<bool(const fs::path&)> content, std::string content_label)
{
    title_         = std::move(title);
    mode_          = mode;
    extensions_    = std::move(extensions);
    content_       = std::move(content);
    content_label_ = std::move(content_label);
    file_name_.clear();
    error_.clear();
    confirm_overwrite_ = false;

    std::error_code ec;
    fs::path p = start.empty() ? fs::current_path(ec) : fs::absolute(start, ec);
    if (!fs::is_directory(p, ec)) {
        file_name_ = p.filename().string();
        p          = p.parent_path();
    }
    if (!fs::is_directory(p, ec))
        p = fs::current_path(ec);
    dir_ = p.lexically_normal();
    refresh();

    visible_   = true;
    open_next_ = true;
}

bool FileBrowser::matches(const fs::path& p) const
{
    if (show_all_ || extensions_.empty())
        return true;
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (std::ranges::find(extensions_, ext) != extensions_.end())
        return true;
    return content_ && content_(p);
}

bool FileBrowser::is_elf(const fs::path& p)
{
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    std::ifstream f(p, std::ios::binary);
    char magic[4] = {};
    return f.read(magic, 4) && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

void FileBrowser::refresh()
{
    entries_.clear();
    error_.clear();
    dir_input_ = dir_.string();
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (name.starts_with('.'))
            continue;
        std::error_code ec2;
        const bool is_dir = it->is_directory(ec2);
        if (!is_dir && !matches(it->path()))
            continue;
        entries_.push_back({name, is_dir});
    }
    if (ec)
        error_ = ec.message();
    std::ranges::sort(entries_, [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
}

std::optional<fs::path> FileBrowser::render()
{
    if (!visible_)
        return std::nullopt;

    const std::string popup_id = title_ + "###file_browser";
    if (open_next_) {
        ImGui::OpenPopup(popup_id.c_str());
        open_next_ = false;
    }

    std::optional<fs::path> result;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 42, em * 30), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popup_id.c_str(), &visible_)) {
        return std::nullopt;
    }

    // ── directory bar ──
    if (ImGui::Button("Up") && dir_.has_parent_path()) {
        dir_ = dir_.parent_path();
        refresh();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home")) {
        if (const char* home = std::getenv("HOME")) {
            dir_ = home;
            refresh();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Project")) {
        std::error_code ec;
        dir_ = fs::current_path(ec);
        refresh();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##dir", &dir_input_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        if (fs::is_directory(dir_input_, ec)) {
            dir_ = fs::path(dir_input_).lexically_normal();
            refresh();
        } else {
            error_ = "Not a directory: " + dir_input_;
        }
    }

    // ── listing ──
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.6f;
    bool        accept = false;
    if (ImGui::BeginChild("##entries", ImVec2(0, -footer), ImGuiChildFlags_Border)) {
        std::optional<fs::path> enter_dir;
        for (const auto& e : entries_) {
            const std::string label = (e.is_dir ? "[dir]  " : "       ") + e.name;
            const bool selected = !e.is_dir && e.name == file_name_;
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (e.is_dir) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        enter_dir = dir_ / e.name;
                } else {
                    file_name_ = e.name;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        accept = true;
                }
            }
        }
        if (entries_.empty())
            ImGui::TextDisabled("(empty)");
        if (enter_dir) {
            dir_ = enter_dir->lexically_normal();
            refresh();
        }
    }
    ImGui::EndChild();

    // ── file name + buttons ──
    ImGui::TextUnformatted("File:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-em * 10);
    if (ImGui::InputText("##file", &file_name_, ImGuiInputTextFlags_EnterReturnsTrue))
        accept = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("All files", &show_all_))
        refresh();

    if (!error_.empty())
        ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.1f, 1.0f), "%s", error_.c_str());
    else if (!extensions_.empty() && !show_all_) {
        std::string filter;
        for (const auto& e : extensions_) filter += (filter.empty() ? "*" : ", *") + e;
        if (!content_label_.empty()) filter += ", " + content_label_;
        ImGui::TextDisabled("Filter: %s", filter.c_str());
    } else {
        ImGui::NewLine();
    }

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - em * 9);
    if (ImGui::Button(mode_ == Mode::Open ? "Open" : "Save", ImVec2(em * 4.5f, 0)))
        accept = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 4.5f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        visible_ = false;
        ImGui::CloseCurrentPopup();
    }

    if (accept && !file_name_.empty()) {
        fs::path chosen = fs::path(file_name_).is_absolute() ? fs::path(file_name_) : dir_ / file_name_;
        // Save dialogs: append the default extension if none was typed.
        if (mode_ == Mode::Save && !extensions_.empty() && chosen.extension().empty())
            chosen += extensions_.front();

        std::error_code ec;
        if (fs::is_directory(chosen, ec)) {
            dir_ = chosen.lexically_normal();
            file_name_.clear();
            refresh();
        } else if (mode_ == Mode::Open && !fs::exists(chosen, ec)) {
            error_ = "File does not exist: " + chosen.filename().string();
        } else if (mode_ == Mode::Save && fs::exists(chosen, ec) && !confirm_overwrite_) {
            error_ = chosen.filename().string() + " exists - press Save again to overwrite";
            confirm_overwrite_ = true;
        } else {
            // Prefer paths relative to the working directory: configs stay portable.
            const fs::path rel = chosen.lexically_relative(fs::current_path(ec));
            result   = (!rel.empty() && !rel.string().starts_with("..")) ? rel : chosen;
            visible_ = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
    return result;
}
