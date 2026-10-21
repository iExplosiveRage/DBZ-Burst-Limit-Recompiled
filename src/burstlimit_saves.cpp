// Save export / import (settings menu, SAVE tab).
//
// The game keeps one save: the content package "Game Data.dat" (type 1, saved
// game) of title 424107DC, which the runtime stores like Xenia does:
//   <user data>\<XUID>\424107DC\00000001\Game Data.dat\savegame.txt
//   <user data>\<XUID>\424107DC\Headers\00000001\Game Data.dat.header
// savegame.txt is 3184 bytes; its first big-endian word is a checksum: the
// sum of the other bytes, plus their count. The header is the
// XCONTENT_AGGREGATE_DATA the runtime (and Xenia) writes, 0x148 bytes.
//
// Export writes both files into <exe>\saves\BurstLimit-save-<time>.zip with
// the folder layout from 424107DC down, so the zip also works in Xenia.
// Import takes such a zip, a Xenia content folder or a zip of one (any depth:
// it looks for .../00000001/Game Data.dat/savegame.txt), checks it, backs the
// current save up into <exe>\saves\backup-before-import-<time>.zip and writes
// it. The running game still has the old save in memory and would write it
// back at its next autosave, so the runtime stops the guest from opening
// save content from then on (ContentManager::BlockContentAccess) until the
// game is restarted - which loads the imported save.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/net/session.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/overlay/quick_menu.h>

// Only stb_image's zlib decoder is used (zip entries Windows / 7-Zip
// compressed). Static: the runtime has its own copy.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

REXCVAR_DEFINE_BOOL(save_open_folder, true, "Saves",
                    "Open the saves folder in Explorer after a save export.");

namespace {

constexpr uint32_t kTitleId = 0x424107DC;
constexpr uint64_t kDefaultXuid = 0xB13EBABEBABEBABEull;
constexpr size_t kSaveSize = 3184;
constexpr size_t kHeaderSize = 0x148;
constexpr const char* kSaveFileName = "savegame.txt";
constexpr const char* kContentName = "Game Data.dat";
// Zip entry names / folder layout under the title folder.
constexpr const char* kSaveEntry = "424107DC/00000001/Game Data.dat/savegame.txt";
constexpr const char* kHeaderEntry = "424107DC/Headers/00000001/Game Data.dat.header";

std::filesystem::path g_exe_directory;
std::filesystem::path g_user_data_root;

// A save was written by an import: the game's save writes stay blocked.
bool g_imported = false;

std::mutex g_status_mutex;
rex::ui::QuickMenuStatus g_status;

void SetStatus(std::string text, bool error) {
  if (error) {
    REXLOG_WARN("Saves: {}", text);
  } else {
    REXLOG_INFO("Saves: {}", text);
  }
  std::lock_guard<std::mutex> lock(g_status_mutex);
  g_status.text = std::move(text);
  g_status.error = error;
}

std::filesystem::path SavesFolder() { return g_exe_directory / "saves"; }

rex::system::xam::ContentManager* GetContentManager() {
  rex::system::KernelState* kernel = rex::system::kernel_state();
  return kernel ? kernel->content_manager() : nullptr;
}

std::filesystem::path TitleFolder() {
  uint64_t xuid = kDefaultXuid;
  if (rex::system::KernelState* kernel = rex::system::kernel_state()) {
    if (kernel->user_profile()) {
      xuid = kernel->user_profile()->xuid();
    }
  }
  return g_user_data_root / fmt::format("{:016X}", xuid) / fmt::format("{:08X}", kTitleId);
}
std::filesystem::path SaveFilePath() {
  return TitleFolder() / "00000001" / kContentName / kSaveFileName;
}
std::filesystem::path HeaderFilePath() {
  return TitleFolder() / "Headers" / "00000001" / (std::string(kContentName) + ".header");
}

std::string LocalTime(const char* format) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  char text[64];
  std::strftime(text, sizeof(text), format, &local);
  return text;
}

// "saves\name" for messages.
std::string ShortName(const std::filesystem::path& path) {
  std::error_code error;
  auto relative = std::filesystem::relative(path, g_exe_directory, error);
  const std::filesystem::path& shown =
      (error || relative.empty() || relative.native().starts_with(L"..")) ? path : relative;
  return reinterpret_cast<const char*>(shown.u8string().c_str());
}

std::string Lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return result;
}

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& data,
                   size_t max_size) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > max_size) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  data.resize(size_t(size));
  file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
  return bool(file) || file.gcount() == std::streamsize(size);
}

// Writes through a temporary file, so a failed write keeps the old file.
bool WriteWholeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::filesystem::path temp = path;
  temp += L".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!file) {
      return false;
    }
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temp, error);
    return false;
  }
  return true;
}

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Empty when the save looks right, otherwise why not.
std::string CheckSave(const std::vector<uint8_t>& save) {
  if (save.size() != kSaveSize) {
    return fmt::format("savegame.txt has {} bytes, a Burst Limit save has {}.", save.size(),
                       kSaveSize);
  }
  uint32_t sum = uint32_t(save.size() - 4);
  for (size_t i = 4; i < save.size(); ++i) {
    sum += save[i];
  }
  if (sum != LoadBE32(save.data())) {
    return "savegame.txt is damaged (its checksum doesn't match).";
  }
  return {};
}

// Empty when the header names this game's save, otherwise why not.
std::string CheckHeader(const std::vector<uint8_t>& header) {
  if (header.size() < kHeaderSize) {
    return "The save's .header file is too short.";
  }
  if (LoadBE32(header.data() + 4) != 1) {
    return "The .header file isn't a saved game's.";
  }
  const uint32_t title = LoadBE32(header.data() + 0x140);
  if (title != kTitleId) {
    return fmt::format("This save is from another game (title ID {:08X}).", title);
  }
  const std::string name(reinterpret_cast<const char*>(header.data() + 0x108),
                         strnlen(reinterpret_cast<const char*>(header.data() + 0x108), 42));
  if (name != kContentName) {
    return fmt::format("The .header file is for \"{}\", not \"{}\".", name, kContentName);
  }
  return {};
}

// The header the game writes for its save (XCONTENT_AGGREGATE_DATA, big
// endian): HDD device, saved game, "Game Data", "Game Data.dat", xuid 0,
// title 424107DC.
std::vector<uint8_t> MakeHeader() {
  std::vector<uint8_t> header(kHeaderSize, 0);
  header[3] = 1;  // device_id
  header[7] = 1;  // content_type: saved game
  const char* display = "Game Data";
  for (size_t i = 0; display[i]; ++i) {
    header[8 + i * 2 + 1] = uint8_t(display[i]);
  }
  std::memcpy(header.data() + 0x108, kContentName, std::strlen(kContentName));
  header[0x140] = 0x42;
  header[0x141] = 0x41;
  header[0x142] = 0x07;
  header[0x143] = 0xDC;
  return header;
}

// ---------------------------------------------------------------- zip

uint32_t Crc32(const uint8_t* data, size_t size) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void Put16(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
}
void Put32(std::vector<uint8_t>& out, uint32_t value) {
  Put16(out, value & 0xFFFF);
  Put16(out, value >> 16);
}
uint32_t Get16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
uint32_t Get32(const uint8_t* p) { return Get16(p) | (Get16(p + 2) << 16); }

struct ZipEntry {
  std::string name;  // '/' separated
  std::vector<uint8_t> data;
};

// A zip of stored (uncompressed) entries - the saves are a few KB.
std::vector<uint8_t> MakeZip(const std::vector<ZipEntry>& entries) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  const uint32_t dos_time = uint32_t(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2);
  const uint32_t dos_date =
      uint32_t((local.tm_year - 80) << 9 | (local.tm_mon + 1) << 5 | local.tm_mday);
  std::vector<uint8_t> zip, directory;
  for (const ZipEntry& entry : entries) {
    const uint32_t offset = uint32_t(zip.size());
    const uint32_t crc = Crc32(entry.data.data(), entry.data.size());
    const uint32_t size = uint32_t(entry.data.size());
    Put32(zip, 0x04034B50);
    Put16(zip, 10);  // version needed
    Put16(zip, 0);   // flags
    Put16(zip, 0);   // stored
    Put16(zip, dos_time);
    Put16(zip, dos_date);
    Put32(zip, crc);
    Put32(zip, size);
    Put32(zip, size);
    Put16(zip, uint32_t(entry.name.size()));
    Put16(zip, 0);
    zip.insert(zip.end(), entry.name.begin(), entry.name.end());
    zip.insert(zip.end(), entry.data.begin(), entry.data.end());

    Put32(directory, 0x02014B50);
    Put16(directory, 20);  // made by
    Put16(directory, 10);
    Put16(directory, 0);
    Put16(directory, 0);
    Put16(directory, dos_time);
    Put16(directory, dos_date);
    Put32(directory, crc);
    Put32(directory, size);
    Put32(directory, size);
    Put16(directory, uint32_t(entry.name.size()));
    Put16(directory, 0);  // extra
    Put16(directory, 0);  // comment
    Put16(directory, 0);  // disk
    Put16(directory, 0);  // internal attributes
    Put32(directory, 0);  // external attributes
    Put32(directory, offset);
    directory.insert(directory.end(), entry.name.begin(), entry.name.end());
  }
  const uint32_t directory_offset = uint32_t(zip.size());
  zip.insert(zip.end(), directory.begin(), directory.end());
  Put32(zip, 0x06054B50);
  Put16(zip, 0);
  Put16(zip, 0);
  Put16(zip, uint32_t(entries.size()));
  Put16(zip, uint32_t(entries.size()));
  Put32(zip, uint32_t(directory.size()));
  Put32(zip, directory_offset);
  Put16(zip, 0);
  return zip;
}

struct ZipListing {
  std::string name;
  uint32_t method = 0, crc = 0, compressed = 0, size = 0, local_offset = 0;
};

// The entries of a zip (no zip64, no encryption). Empty error = fine.
std::string ListZip(const std::vector<uint8_t>& zip, std::vector<ZipListing>& listing) {
  if (zip.size() < 22) {
    return "The file isn't a zip.";
  }
  size_t end = std::string::npos;
  const size_t lowest = zip.size() > 0x10000 + 22 ? zip.size() - 0x10000 - 22 : 0;
  for (size_t i = zip.size() - 22 + 1; i-- > lowest;) {
    if (Get32(&zip[i]) == 0x06054B50) {
      end = i;
      break;
    }
  }
  if (end == std::string::npos) {
    return "The file isn't a zip.";
  }
  const uint32_t count = Get16(&zip[end + 10]);
  const uint32_t directory_size = Get32(&zip[end + 12]);
  uint32_t offset = Get32(&zip[end + 16]);
  if (uint64_t(offset) + directory_size > zip.size()) {
    return "The zip is damaged.";
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (uint64_t(offset) + 46 > zip.size() || Get32(&zip[offset]) != 0x02014B50) {
      return "The zip is damaged.";
    }
    ZipListing item;
    const uint32_t flags = Get16(&zip[offset + 8]);
    item.method = Get16(&zip[offset + 10]);
    item.crc = Get32(&zip[offset + 16]);
    item.compressed = Get32(&zip[offset + 20]);
    item.size = Get32(&zip[offset + 24]);
    const uint32_t name_length = Get16(&zip[offset + 28]);
    const uint32_t extra_length = Get16(&zip[offset + 30]);
    const uint32_t comment_length = Get16(&zip[offset + 32]);
    item.local_offset = Get32(&zip[offset + 42]);
    if (uint64_t(offset) + 46 + name_length > zip.size()) {
      return "The zip is damaged.";
    }
    item.name.assign(reinterpret_cast<const char*>(&zip[offset + 46]), name_length);
    std::replace(item.name.begin(), item.name.end(), '\\', '/');
    if (flags & 1) {
      return "The zip is password-protected.";
    }
    listing.push_back(std::move(item));
    offset += 46 + name_length + extra_length + comment_length;
  }
  return {};
}

std::string ReadZipEntry(const std::vector<uint8_t>& zip, const ZipListing& item,
                         std::vector<uint8_t>& data, uint32_t max_size = 16 * 1024 * 1024) {
  if (item.size > max_size || uint64_t(item.local_offset) + 30 > zip.size() ||
      Get32(&zip[item.local_offset]) != 0x04034B50) {
    return "The zip is damaged.";
  }
  const uint64_t start = uint64_t(item.local_offset) + 30 + Get16(&zip[item.local_offset + 26]) +
                         Get16(&zip[item.local_offset + 28]);
  if (start + item.compressed > zip.size()) {
    return "The zip is damaged.";
  }
  const uint8_t* source = zip.data() + start;
  data.assign(item.size, 0);
  if (item.method == 0) {
    if (item.compressed != item.size) {
      return "The zip is damaged.";
    }
    std::memcpy(data.data(), source, item.size);
  } else if (item.method == 8) {
    const int written = stbi_zlib_decode_noheader_buffer(
        reinterpret_cast<char*>(data.data()), int(data.size()),
        reinterpret_cast<const char*>(source), int(item.compressed));
    if (written != int(item.size)) {
      return "The zip is damaged.";
    }
  } else {
    return fmt::format("The zip uses a compression this can't read (method {}). Zip it with "
                       "Windows (Send to > Compressed folder) instead.",
                       item.method);
  }
  if (Crc32(data.data(), data.size()) != item.crc) {
    return "The zip is damaged (CRC mismatch).";
  }
  return {};
}

// ---------------------------------------------------------------- import source

// The save found in a zip or folder.
struct FoundSave {
  std::vector<uint8_t> save;
  std::optional<std::vector<uint8_t>> header;
};

// Looks at a '/' path that ends in "Game Data.dat/savegame.txt": what the save's
// root is (everything before "00000001/Game Data.dat/..."), and an error when
// the folder above names another title.
bool MatchSavePath(const std::string& path, std::string& root, std::string& error) {
  const std::string lower = Lower(path);
  const std::string tail = "00000001/game data.dat/savegame.txt";
  if (lower.size() < tail.size() || lower.compare(lower.size() - tail.size(), tail.size(), tail)) {
    // Also a bare "Game Data.dat/savegame.txt" (the folder itself picked).
    const std::string short_tail = "game data.dat/savegame.txt";
    if (lower == short_tail || (lower.size() > short_tail.size() &&
                                lower.compare(lower.size() - short_tail.size() - 1,
                                              short_tail.size() + 1, "/" + short_tail) == 0)) {
      root.clear();
      return true;
    }
    return false;
  }
  if (lower.size() > tail.size() && lower[lower.size() - tail.size() - 1] != '/') {
    return false;
  }
  root = path.substr(0, lower.size() - tail.size());
  // The title folder right above, when there's one.
  std::string parent = root;
  if (!parent.empty() && parent.back() == '/') {
    parent.pop_back();
  }
  const size_t slash = parent.find_last_of('/');
  const std::string title = slash == std::string::npos ? parent : parent.substr(slash + 1);
  if (title.size() == 8 && title.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos &&
      Lower(title) != "424107dc") {
    error = fmt::format("This is a save of another game (title ID {}), not Burst Limit (424107DC).",
                        title);
  }
  return true;
}

// Picks the one save out of the candidate paths; error when none or several.
std::string PickSave(const std::vector<std::string>& paths, std::string& picked,
                     std::string& root) {
  std::vector<std::pair<std::string, std::string>> found;  // path, root
  std::string title_error;
  for (const std::string& path : paths) {
    std::string candidate_root, error;
    if (!MatchSavePath(path, candidate_root, error)) {
      continue;
    }
    if (!error.empty()) {
      title_error = error;
      continue;
    }
    found.emplace_back(path, candidate_root);
  }
  if (found.empty()) {
    return !title_error.empty()
               ? title_error
               : "No Burst Limit save in it (looked for 00000001\\Game Data.dat\\savegame.txt).";
  }
  if (found.size() > 1) {
    return fmt::format("It has {} saves (several profiles?). Pick the folder of one of them.",
                       found.size());
  }
  picked = found.front().first;
  root = found.front().second;
  return {};
}

std::string HeaderPathFor(const std::string& root) {
  return root + "Headers/00000001/Game Data.dat.header";
}

std::string LoadFromZip(const std::vector<uint8_t>& zip, FoundSave& found) {
  std::vector<ZipListing> listing;
  if (std::string error = ListZip(zip, listing); !error.empty()) {
    return error;
  }
  std::vector<std::string> names;
  for (const ZipListing& item : listing) {
    names.push_back(item.name);
  }
  std::string picked, root;
  if (std::string error = PickSave(names, picked, root); !error.empty()) {
    return error;
  }
  const std::string header_name = Lower(HeaderPathFor(root));
  for (const ZipListing& item : listing) {
    if (item.name == picked) {
      if (std::string error = ReadZipEntry(zip, item, found.save); !error.empty()) {
        return error;
      }
    } else if (!root.empty() && Lower(item.name) == header_name) {
      std::vector<uint8_t> header;
      if (std::string error = ReadZipEntry(zip, item, header); !error.empty()) {
        return error;
      }
      found.header = std::move(header);
    }
  }
  return {};
}

std::filesystem::path FromUtf8(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string PathString(const std::filesystem::path& path) {
  std::string text = reinterpret_cast<const char*>(path.generic_u8string().c_str());
  return text;
}

std::string LoadFromFolder(const std::filesystem::path& folder, FoundSave& found) {
  // Paths relative to the folder's parent, so a picked "424107DC" or
  // "Game Data.dat" folder still shows in them.
  const std::filesystem::path base = folder.parent_path();
  std::vector<std::string> names;
  std::error_code error;
  size_t visited = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (++visited > 20000 || it.depth() > 8) {
      if (it.depth() > 8) {
        it.disable_recursion_pending();
        continue;
      }
      return "The folder is too big to search. Pick the save's own folder.";
    }
    if (it->is_regular_file(error) && Lower(PathString(it->path().filename())) == kSaveFileName) {
      names.push_back(PathString(std::filesystem::relative(it->path(), base, error)));
    }
  }
  // The title folder may be above the picked one.
  std::string prefix;
  for (const auto& part : base) {
    prefix += PathString(part);
    if (!prefix.empty() && prefix.back() != '/') {
      prefix += '/';
    }
  }
  std::vector<std::string> full_names;
  for (const std::string& name : names) {
    full_names.push_back(prefix + name);
  }
  std::string picked, root;
  if (std::string pick_error = PickSave(full_names, picked, root); !pick_error.empty()) {
    return pick_error;
  }
  const std::filesystem::path save_path = FromUtf8(picked);
  if (!ReadWholeFile(save_path, found.save, 1024 * 1024)) {
    return "Can't read " + PathString(save_path) + ".";
  }
  if (!root.empty()) {
    const std::filesystem::path header_path = FromUtf8(HeaderPathFor(root));
    std::vector<uint8_t> header;
    if (std::filesystem::exists(header_path, error)) {
      if (!ReadWholeFile(header_path, header, 64 * 1024)) {
        return "Can't read " + PathString(header_path) + ".";
      }
      found.header = std::move(header);
    }
  }
  return {};
}

// ---------------------------------------------------------------- actions

// The current save as zip entries; error when there's none or it's damaged.
std::string CurrentSaveEntries(std::vector<ZipEntry>& entries) {
  std::vector<uint8_t> save, header;
  std::error_code error;
  if (!std::filesystem::exists(SaveFilePath(), error)) {
    return "There's no save yet.";
  }
  if (!ReadWholeFile(SaveFilePath(), save, 1024 * 1024)) {
    return "Can't read the save file.";
  }
  if (std::string check = CheckSave(save); !check.empty()) {
    return "The current save can't be exported: " + check;
  }
  if (!ReadWholeFile(HeaderFilePath(), header, 64 * 1024) || !CheckHeader(header).empty()) {
    header = MakeHeader();
  }
  entries.push_back({kSaveEntry, std::move(save)});
  entries.push_back({kHeaderEntry, std::move(header)});
  return {};
}

std::filesystem::path FreeName(const std::string& stem) {
  std::filesystem::path path = SavesFolder() / (stem + ".zip");
  for (int i = 2; std::filesystem::exists(path) && i < 100; ++i) {
    path = SavesFolder() / fmt::format("{}-{}.zip", stem, i);
  }
  return path;
}

void OpenSavesFolder(const std::filesystem::path& select) {
  std::error_code error;
  std::filesystem::create_directories(SavesFolder(), error);
  if (!select.empty()) {
    const std::wstring arguments = L"/select,\"" + select.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
  } else {
    ShellExecuteW(nullptr, L"open", SavesFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

void ExportSave() {
  if (rex::net::IsGameSessionOpen()) {
    SetStatus("Not during an online session.", true);
    return;
  }
  std::vector<ZipEntry> entries;
  if (std::string error = CurrentSaveEntries(entries); !error.empty()) {
    SetStatus(error, true);
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(SavesFolder(), error);
  const std::filesystem::path path = FreeName("BurstLimit-save-" + LocalTime("%Y%m%d-%H%M"));
  if (!WriteWholeFile(path, MakeZip(entries))) {
    SetStatus("Can't write " + ShortName(path) + ".", true);
    return;
  }
  const bool open = REXCVAR_GET(save_open_folder);
  if (open) {
    OpenSavesFolder(path);
  }
  SetStatus("Save exported to " + ShortName(path) + (open ? " (folder opened)." : "."), false);
}

void ImportSave(const std::string& name) {
  if (name.empty()) {
    SetStatus("Put a save in the saves folder first.", true);
    return;
  }
  if (rex::net::IsGameSessionOpen()) {
    SetStatus("Not during an online session.", true);
    return;
  }
  std::filesystem::path source = FromUtf8(name);
  if (source.is_relative()) {
    source = SavesFolder() / source;
  }
  std::error_code error;
  FoundSave found;
  std::string problem;
  if (std::filesystem::is_directory(source, error)) {
    problem = LoadFromFolder(source, found);
  } else if (std::filesystem::is_regular_file(source, error)) {
    std::vector<uint8_t> file;
    if (!ReadWholeFile(source, file, 256 * 1024 * 1024)) {
      problem = "Can't read the file.";
    } else if (file.size() >= 4 && (!std::memcmp(file.data(), "CON ", 4) ||
                                    !std::memcmp(file.data(), "LIVE", 4) ||
                                    !std::memcmp(file.data(), "PIRS", 4))) {
      problem = "Xbox 360 save packages (CON) can't be imported yet - use a zip or a Xenia save.";
    } else {
      problem = LoadFromZip(file, found);
    }
  } else {
    problem = "It's gone.";
  }
  if (problem.empty()) {
    problem = CheckSave(found.save);
  }
  if (problem.empty() && found.header) {
    problem = CheckHeader(*found.header);
  }
  const std::string shown = PathString(source.filename());
  if (!problem.empty()) {
    SetStatus("Not imported (" + shown + "): " + problem, true);
    return;
  }

  rex::system::xam::ContentManager* content = GetContentManager();
  if (!content) {
    SetStatus("Not imported: the game isn't running yet.", true);
    return;
  }
  // From here the game can't open (and so can't write) its save until it
  // restarts. Not while it has it open: it's saving or loading right now.
  if (!content->BlockContentAccess()) {
    SetStatus("The game is saving right now - try again in a moment.", true);
    return;
  }
  // Writes stay blocked once a save was imported; before that, a failure
  // that left the save alone gives them back.
  auto fail = [content](std::string text) {
    if (!g_imported) {
      content->UnblockContentAccess();
    }
    SetStatus(std::move(text), true);
  };

  // Back the current save up first.
  std::string backup_text = "there was no save before";
  std::vector<ZipEntry> entries;
  if (std::filesystem::exists(SaveFilePath(), error)) {
    std::vector<uint8_t> save, header;
    if (!ReadWholeFile(SaveFilePath(), save, 1024 * 1024)) {
      fail("Not imported: can't read the current save to back it up.");
      return;
    }
    // Backed up as it is, even if damaged.
    if (!ReadWholeFile(HeaderFilePath(), header, 64 * 1024)) {
      header = MakeHeader();
    }
    entries.push_back({kSaveEntry, std::move(save)});
    entries.push_back({kHeaderEntry, std::move(header)});
    std::filesystem::create_directories(SavesFolder(), error);
    const std::filesystem::path backup =
        FreeName("backup-before-import-" + LocalTime("%Y%m%d-%H%M%S"));
    if (!WriteWholeFile(backup, MakeZip(entries))) {
      fail("Not imported: can't write the backup " + ShortName(backup) + ".");
      return;
    }
    backup_text = "old save backed up to " + ShortName(backup);
  }

  if (!WriteWholeFile(SaveFilePath(), found.save)) {
    fail("Import failed while writing the save; the old one is still there.");
    return;
  }
  g_imported = true;
  if (!WriteWholeFile(HeaderFilePath(), MakeHeader())) {
    SetStatus("Save imported but its .header couldn't be written (" + backup_text +
                  "). Restart the game.",
              true);
    return;
  }
  SetStatus("Save imported from " + shown + "; " + backup_text +
                ". Restart the game to load it - saving is off until then.",
            false);
}

std::vector<std::pair<std::string, std::string>> ListSavesFolder() {
  std::vector<std::pair<std::filesystem::file_time_type, std::string>> items;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(SavesFolder(), error)) {
    const std::string name = PathString(entry.path().filename());
    const bool folder = entry.is_directory(error);
    if (!folder && Lower(entry.path().extension().string()) != ".zip") {
      // Zips and folders; also extension-less files (a CON package, refused
      // with a message).
      if (entry.path().has_extension() && Lower(entry.path().extension().string()) != ".dat") {
        continue;
      }
    }
    items.emplace_back(entry.last_write_time(error), name);
  }
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::pair<std::string, std::string>> list;
  for (auto& item : items) {
    list.emplace_back(item.second, item.second);
  }
  return list;
}

// ---------------------------------------------------------------- unlock everything
//
// GAME tab "Unlock everything": a true 100% - everything a 100% save has
// (characters, forms, costumes, stages, modes, Z Chronicles episodes and their
// clears, Drama Pieces, trial records) - while the player's own options, play
// time and status counters stay. The game keeps its save image at
// [0x825205E8] + 16 (0x84140F00) in the same layout as savegame.txt.
// Compared with a fresh save, a 100% save differs in (how each part is merged):
//   0x000        checksum                                     (recomputed)
//   0x004-0x04F  options                                      (player's)
//   0x050-0x46F  Z Chronicles battle records, one u32 each,
//                0x80000000 = cleared + grade / score bits     (100% where empty)
//   0x560-0x86F  Trial record tables (counts, scores, dates),
//                three tables 0x560 / 0x680 / 0x7B0             (100% where the
//                                                               player's is empty)
//   0x870-0x8AF  Trial / completion counters, u32             (the higher one)
//   0x8B0-0x8CF  Status screen: play time, battles, attacks,
//                guards, sparks, ultimates, transformations    (player's)
//   0x8D0-0xA8F  unlock flags (characters, costumes, Drama
//                Pieces...), 0 locked / 1-2 unlocked; forms at
//                0x978 + 5 * character + form                  (the higher one)
//   0xBFC-0xC60  Z Chronicles episode flags                    (the higher one)
// Records only fill empty entries: a cleared battle / filled table of the
// player's is kept as it is (the grade bits' order isn't known).

constexpr uint32_t kSavePointer = 0x825205E8;  // -> save block; the image is at +16
constexpr uint32_t kSaveImageOffset = 16;

// The 100% save's bytes from 0x050 on (records, trial tables, counters and
// flags; not its status block 0x8B0-0x8CF): 22 runs, 1100 bytes.
struct TemplateRun {
  uint16_t offset;
  uint16_t size;
};
constexpr TemplateRun kTemplateRuns[] = {
    {0x054, 4}, {0x070, 4}, {0x164, 4}, {0x200, 216}, {0x3B0, 180}, {0x560, 1}, {0x572, 2},
    {0x5E9, 3}, {0x660, 33}, {0x6A1, 3}, {0x6B5, 3}, {0x719, 3}, {0x72D, 3}, {0x7B0, 6},
    {0x7CE, 6}, {0x7E5, 3}, {0x7F9, 3}, {0x80D, 3}, {0x830, 22}, {0x877, 49}, {0x8D0, 448},
    {0xBFC, 101},
};
constexpr uint8_t kTemplateBytes[1100] = {
    128, 0, 45, 108, 128, 0, 66, 229, 128, 0, 100, 231, 128, 0, 71, 233, 128, 0, 14, 65, 128, 0, 43, 105,
    128, 0, 35, 105, 128, 0, 51, 236, 128, 0, 231, 110, 128, 0, 93, 251, 128, 0, 3, 233, 128, 0, 244, 247,
    128, 0, 62, 229, 128, 0, 15, 232, 128, 0, 66, 236, 128, 0, 66, 225, 128, 0, 212, 231, 128, 0, 38, 231,
    128, 0, 33, 233, 128, 0, 59, 235, 128, 0, 55, 173, 128, 0, 57, 235, 128, 0, 137, 237, 128, 0, 12, 231,
    128, 0, 23, 237, 128, 0, 67, 172, 128, 0, 37, 237, 128, 0, 53, 239, 128, 0, 15, 170, 128, 0, 19, 235,
    128, 0, 17, 235, 128, 0, 19, 235, 128, 0, 3, 234, 128, 0, 3, 236, 128, 0, 27, 235, 128, 0, 9, 233,
    128, 0, 19, 237, 128, 0, 53, 173, 128, 0, 75, 239, 128, 0, 7, 201, 128, 0, 39, 235, 128, 0, 31, 41,
    128, 0, 49, 232, 128, 0, 51, 232, 128, 0, 37, 249, 128, 0, 67, 171, 128, 0, 25, 235, 128, 0, 55, 238,
    128, 0, 59, 235, 128, 0, 11, 203, 128, 0, 17, 235, 128, 0, 43, 235, 128, 0, 17, 235, 128, 0, 19, 235,
    128, 0, 49, 234, 128, 0, 60, 227, 128, 0, 61, 235, 128, 0, 71, 234, 128, 0, 12, 226, 128, 0, 32, 227,
    128, 0, 34, 178, 128, 0, 16, 186, 128, 0, 134, 130, 128, 0, 0, 162, 128, 0, 16, 227, 128, 0, 252, 242,
    128, 0, 41, 202, 0, 0, 0, 0, 128, 0, 2, 66, 128, 0, 96, 66, 128, 0, 86, 225, 128, 0, 116, 226,
    128, 0, 40, 99, 128, 0, 60, 67, 128, 0, 116, 199, 128, 0, 40, 226, 128, 0, 18, 99, 128, 0, 12, 114,
    128, 0, 16, 225, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 54, 193, 0, 0, 0, 0, 128, 0, 8, 66,
    0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 22, 99, 0, 0, 0, 0, 128, 0, 24, 194, 128, 0, 10, 66,
    128, 0, 2, 67, 128, 0, 5, 251, 0, 0, 0, 0, 128, 0, 38, 99, 0, 0, 0, 0, 128, 0, 22, 65,
    0, 0, 0, 0, 128, 0, 18, 65, 128, 0, 20, 80, 128, 0, 16, 97, 0, 0, 0, 0, 128, 0, 44, 226,
    3, 217, 3, 8, 6, 16, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 3, 9, 236, 160, 7, 216, 52, 8, 6, 18,
    8, 6, 18, 19, 0, 0, 0, 0, 19, 1, 0, 0, 1, 39, 82, 2, 54, 242, 8, 6, 16, 8, 6, 16,
    19, 0, 0, 0, 0, 3, 0, 0, 0, 0, 38, 0, 0, 0, 0, 65, 0, 0, 0, 0, 1, 2, 22, 0,
    0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 5, 0,
    0, 0, 0, 0, 0, 0, 31, 0, 0, 19, 136, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 20, 2,
    2, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 2, 2, 2, 2, 2,
    2, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1,
    1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2, 1, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 1, 1, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 1, 1,
    2, 2, 2, 1, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1,
    2, 0, 1, 1, 0, 1, 1, 0, 2, 1, 0, 2, 1, 0, 1, 1, 0, 2, 2, 0, 1, 2, 0, 1,
    1, 2, 0, 0, 1, 1, 0, 0, 0, 1, 1, 2, 0, 0, 1, 1, 2, 0, 0, 2, 2, 0, 0, 0,
    1, 1, 2, 0, 0, 1, 1, 2, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 2, 1, 0, 0,
    0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1, 1, 2, 0, 1, 0, 0, 0, 0, 2, 0,
    0, 0, 0, 2, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 2, 2, 1,
    2, 1, 2, 1, 2, 1, 2, 2, 2, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2,
    2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 1, 0, 0,
    0, 1, 2, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0,
    0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 2, 1, 2,
    2, 2, 2, 0, 0, 2, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 1, 1, 2, 2, 2, 2, 2, 2, 1, 0, 1, 0, 0, 2, 1, 1, 1, 0, 0, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 0, 2, 1, 1, 1, 1,
};

std::mutex g_unlock_status_mutex;
rex::ui::QuickMenuStatus g_unlock_status;
// The first A arms the item until this time; a second A before it unlocks.
std::chrono::steady_clock::time_point g_unlock_armed_until;

void SetUnlockStatus(std::string text, bool error) {
  if (error) {
    REXLOG_WARN("Unlock everything: {}", text);
  } else {
    REXLOG_INFO("Unlock everything: {}", text);
  }
  std::lock_guard<std::mutex> lock(g_unlock_status_mutex);
  g_unlock_status.text = std::move(text);
  g_unlock_status.error = error;
}

// The 100% save as a whole image (zeros where the table has nothing).
const std::array<uint8_t, kSaveSize>& Template() {
  static const std::array<uint8_t, kSaveSize> image = [] {
    std::array<uint8_t, kSaveSize> out{};
    size_t at = 0;
    for (const TemplateRun& run : kTemplateRuns) {
      std::memcpy(out.data() + run.offset, kTemplateBytes + at, run.size);
      at += run.size;
    }
    return out;
  }();
  return image;
}

// Merges the 100% save into `save` (see the table above); returns how many
// bytes changed.
size_t ApplyUnlocks(uint8_t* save) {
  const std::array<uint8_t, kSaveSize>& full = Template();
  size_t changed = 0;
  auto set = [&](size_t offset, uint8_t value) {
    if (save[offset] != value) {
      save[offset] = value;
      ++changed;
    }
  };
  // Z Chronicles battle records: empty (cleared bit not set) -> the 100% one.
  for (size_t offset = 0x050; offset < 0x470; offset += 4) {
    if (!(save[offset] & 0x80) && LoadBE32(full.data() + offset)) {
      for (size_t i = 0; i < 4; ++i) {
        set(offset + i, full[offset + i]);
      }
    }
  }
  // Trial record tables: the 100% one when the player's has nothing of its own.
  constexpr std::pair<size_t, size_t> kTables[] = {{0x560, 0x680}, {0x680, 0x7B0}, {0x7B0, 0x870}};
  for (const auto& [begin, end] : kTables) {
    bool empty = true;
    for (size_t i = begin; i < end && empty; ++i) {
      empty = save[i] == 0 || save[i] == full[i];
    }
    if (empty) {
      for (size_t i = begin; i < end; ++i) {
        set(i, full[i]);
      }
    }
  }
  // Counters: at least the 100% save's.
  for (size_t offset = 0x870; offset < 0x8B0; offset += 4) {
    if (LoadBE32(full.data() + offset) > LoadBE32(save + offset)) {
      for (size_t i = 0; i < 4; ++i) {
        set(offset + i, full[offset + i]);
      }
    }
  }
  // Unlock and episode flags: the higher value (forms 2 like the 100% save).
  for (size_t offset = 0x8D0; offset < kSaveSize; ++offset) {
    if (full[offset] > save[offset]) {
      set(offset, full[offset]);
    }
  }
  return changed;
}

void StoreSaveChecksum(uint8_t* save) {
  uint32_t sum = uint32_t(kSaveSize - 4);
  for (size_t i = 4; i < kSaveSize; ++i) {
    sum += save[i];
  }
  save[0] = uint8_t(sum >> 24);
  save[1] = uint8_t(sum >> 16);
  save[2] = uint8_t(sum >> 8);
  save[3] = uint8_t(sum);
}

void UnlockEverything() {
  if (rex::net::IsGameSessionOpen()) {
    SetUnlockStatus("Not during an online session.", true);
    return;
  }
  if (g_imported) {
    SetUnlockStatus("A save was imported: restart the game first.", true);
    return;
  }
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
  rex::system::xam::ContentManager* content = GetContentManager();
  if (!memory || !content) {
    SetUnlockStatus("The game isn't running yet.", true);
    return;
  }
  const uint32_t block =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kSavePointer));
  if (!block) {
    SetUnlockStatus("The game hasn't loaded its save yet - try again in the menus.", true);
    return;
  }
  uint8_t* live = memory->TranslateVirtual<uint8_t*>(block + kSaveImageOffset);

  // The game can't open (save or load) its save while this runs. Not while
  // it has it open: it's saving or loading right now.
  if (!content->BlockContentAccess()) {
    SetUnlockStatus("The game is saving right now - try again in a moment.", true);
    return;
  }
  struct Unblock {
    rex::system::xam::ContentManager* content;
    ~Unblock() { content->UnblockContentAccess(); }
  } unblock{content};

  // The save on disk: backed up as it is, then given the same unlocks.
  const std::filesystem::path path = SaveFilePath();
  std::error_code error;
  std::vector<uint8_t> disk;
  const bool on_disk = std::filesystem::exists(path, error);
  std::filesystem::path backup = path;
  backup += ".bak_unlock";
  if (on_disk) {
    if (!ReadWholeFile(path, disk, 1024 * 1024)) {
      SetUnlockStatus("Can't read the save file - nothing changed.", true);
      return;
    }
    if (std::string check = CheckSave(disk); !check.empty()) {
      SetUnlockStatus("Nothing changed: " + check, true);
      return;
    }
    if (!WriteWholeFile(backup, disk)) {
      SetUnlockStatus("Can't write the backup " + PathString(backup.filename()) +
                          " - nothing changed.",
                      true);
      return;
    }
  }

  // The save in memory: what the game shows and writes at its next save.
  const size_t changed = ApplyUnlocks(live);
  StoreSaveChecksum(live);

  if (!on_disk) {
    if (changed) {
      SetUnlockStatus("Everything is unlocked for this session. There's no save on this PC "
                      "(no storage was picked at the title), so it isn't kept.",
                      true);
    } else {
      SetUnlockStatus("Everything was already unlocked.", false);
    }
    return;
  }
  const size_t changed_on_disk = ApplyUnlocks(disk.data());
  if (changed_on_disk) {
    StoreSaveChecksum(disk.data());
    if (!WriteWholeFile(path, disk)) {
      SetUnlockStatus("Unlocked for now, but the save couldn't be written - it's kept after "
                      "the game's next save.",
                      true);
      return;
    }
  }
  if (!changed && !changed_on_disk) {
    SetUnlockStatus("Everything was already unlocked.", false);
    return;
  }
  SetUnlockStatus(fmt::format("Everything is unlocked (100%) and saved. Backup: {}.",
                              PathString(backup.filename())),
                  false);
}

void UnlockEverythingCommand() { UnlockEverything(); }

void SaveExportCommand() { ExportSave(); }
void SaveImportCommand(std::string_view args) { ImportSave(std::string(args)); }

}  // namespace

REXCVAR_DEFINE_COMMAND(save_export, SaveExportCommand, "Saves",
                       "Export the save to <exe>\\saves (like the settings menu)");
REXCVAR_DEFINE_COMMAND_ARGS(save_import, SaveImportCommand, "Saves",
                            "Import a save: <file or folder in saves, or a full path>");
REXCVAR_DEFINE_COMMAND(unlock_everything, UnlockEverythingCommand, "Saves",
                       "Unlock everything (the settings menu item, without its confirm step)");

void BurstLimitSavesSetup(const std::filesystem::path& exe_directory,
                          const std::filesystem::path& user_data_root) {
  g_exe_directory = exe_directory;
  g_user_data_root = user_data_root;
}

void BurstLimitSavesMenu(rex::ui::QuickMenuConfig& menu) {
  using Item = rex::ui::QuickMenuItem;
  rex::ui::QuickMenuSection& section = menu.sections.emplace_back();
  section.title = "SAVE";
  section.status = []() {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
  };
  Item& export_item = section.items.emplace_back();
  export_item.kind = Item::Kind::kAction;
  export_item.label = "Export save";
  export_item.action_label = "Export";
  export_item.help =
      "Copies your save into a zip in the saves folder next to burstlimit.exe and opens the "
      "folder. The zip also works in Xenia.";
  export_item.action = [](const std::string&) { ExportSave(); };

  Item& import_item = section.items.emplace_back();
  import_item.kind = Item::Kind::kAction;
  import_item.label = "Import save";
  import_item.empty_text = "Saves folder is empty";
  import_item.help =
      "Pick a save zip (exported here or from Xenia) or a Xenia save folder from the saves "
      "folder with left / right, then A. Your current save is backed up there first.";
  import_item.list = ListSavesFolder;
  import_item.action = [](const std::string& value) { ImportSave(value); };

  Item& open_item = section.items.emplace_back();
  open_item.kind = Item::Kind::kAction;
  open_item.label = "Saves folder";
  open_item.action_label = "Open";
  open_item.help = "Opens the saves folder next to burstlimit.exe in Explorer.";
  open_item.action = [](const std::string&) {
    OpenSavesFolder({});
    SetStatus("Opened " + ShortName(SavesFolder()) + ".", false);
  };
}

// GAME tab: "Unlock everything", with a confirm step (A twice) and its own
// result line.
void BurstLimitUnlockMenu(rex::ui::QuickMenuSection& section) {
  using Item = rex::ui::QuickMenuItem;
  section.status = []() {
    std::lock_guard<std::mutex> lock(g_unlock_status_mutex);
    return g_unlock_status;
  };
  Item& item = section.items.emplace_back();
  item.kind = Item::Kind::kAction;
  item.label = "Unlock everything";
  item.empty_text = "Offline only";
  item.help =
      "Unlocks all characters, forms, stages and extras (like a 100% save). Your records and "
      "progress are kept. A backup of the save is made first.";
  // One entry: "Unlock", then "Press A again" while armed; none online.
  item.list = []() -> std::vector<std::pair<std::string, std::string>> {
    if (rex::net::IsGameSessionOpen()) {
      return {};
    }
    if (std::chrono::steady_clock::now() < g_unlock_armed_until) {
      return {{"confirm", "Press A again"}};
    }
    return {{"unlock", "Unlock"}};
  };
  item.action = [](const std::string&) {
    if (rex::net::IsGameSessionOpen()) {
      SetUnlockStatus("Not during an online session.", true);
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_unlock_armed_until) {
      g_unlock_armed_until = now + std::chrono::seconds(10);
      SetUnlockStatus("Press A again to unlock everything (a backup of the save is made first).",
                      false);
      return;
    }
    g_unlock_armed_until = {};
    UnlockEverything();
  };
}

// burstlimit_launcher.cpp (the self-update): every file entry of a zip with its
// data, in zip order; `each` returns an error to stop. Empty = all fine.
std::string BurstLimitUnzip(
    const std::vector<uint8_t>& zip,
    const std::function<std::string(const std::string& name, const std::vector<uint8_t>& data)>&
        each) {
  std::vector<ZipListing> listing;
  if (std::string error = ListZip(zip, listing); !error.empty()) {
    return error;
  }
  std::vector<uint8_t> data;
  for (const ZipListing& item : listing) {
    if (!item.name.empty() && item.name.back() == '/') {
      continue;  // a folder
    }
    if (std::string error = ReadZipEntry(zip, item, data, 512u * 1024 * 1024); !error.empty()) {
      return item.name + ": " + error;
    }
    if (std::string error = each(item.name, data); !error.empty()) {
      return error;
    }
  }
  return {};
}
