#include "sample_library.hpp"

#include "oscillate/application/browser_preview.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>

namespace oscillate::quick {
namespace {

[[nodiscard]] bool audio_extension(const std::filesystem::path& path, bool encoded) {
  auto extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension == ".wav")
    return true;
  constexpr std::array<std::string_view, 4> others{".aif", ".aiff", ".flac", ".mp3"};
  return encoded && std::ranges::find(others, extension) != others.end();
}

} // namespace

std::vector<SampleEntry> scan_sample_folder(const std::filesystem::path& root, bool encoded,
                                            const std::stop_token& stop) {
  namespace fs = std::filesystem;
  std::vector<SampleEntry> out;
  std::error_code error;
  if (!fs::is_directory(root, error))
    return out;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
  if (error)
    return out;
  for (; it != fs::recursive_directory_iterator(); it.increment(error)) {
    if (error || stop.stop_requested() || out.size() >= max_library_samples)
      break;
    const auto& entry = *it;
    const auto name = entry.path().filename().string();
    if (name.starts_with('.')) {
      if (entry.is_directory(error))
        it.disable_recursion_pending();
      continue;
    }
    if (entry.is_symlink(error) && entry.is_directory(error)) {
      it.disable_recursion_pending();
      continue;
    }
    if (it.depth() >= max_library_depth && entry.is_directory(error)) {
      it.disable_recursion_pending();
      continue;
    }
    if (!entry.is_regular_file(error) || !audio_extension(entry.path(), encoded))
      continue;
    auto stem = entry.path().stem().string();
    auto folder = entry.path().parent_path().lexically_relative(root).generic_string();
    if (folder == ".")
      folder.clear();
    auto bpm = application::tempo_from_file_name(stem);
    out.push_back({entry.path(), std::move(stem), std::move(folder), bpm});
  }
  std::ranges::sort(out, [](const SampleEntry& a, const SampleEntry& b) {
    return a.folder != b.folder ? a.folder < b.folder : a.name < b.name;
  });
  return out;
}

void SampleLibrary::scan(std::filesystem::path root, bool encoded) {
  scanning_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(shared_->mutex);
    shared_->ready.reset();
  }
  // Assigning stops and joins the previous scan first.
  worker_ = std::jthread(
      [this, shared = shared_, root = std::move(root), encoded](const std::stop_token& stop) {
        std::vector<SampleEntry> found;
        try {
          found = scan_sample_folder(root, encoded, stop);
        } catch (const std::exception&) {
          found.clear();
        }
        if (stop.stop_requested())
          return;
        {
          std::lock_guard lock(shared->mutex);
          shared->ready = std::move(found);
        }
        scanning_.store(false, std::memory_order_release);
      });
}

std::optional<std::vector<SampleEntry>> SampleLibrary::take() {
  std::lock_guard lock(shared_->mutex);
  return std::exchange(shared_->ready, std::nullopt);
}

} // namespace oscillate::quick
