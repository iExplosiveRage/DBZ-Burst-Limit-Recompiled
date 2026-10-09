// Mods: packs of replacement game files, switched on or off in a "Mods" entry
// added to the main menu under Options.
//
// A mod is a folder in <burstlimit.exe folder>\mods\. Its files are named
// like the files inside the game's archive LONG2DATA_US.CPK (e.g.
// BCGHM000.NUX, or a path such as PAC\CMN\CH\BCGHM\BCGHM000.NUX when a name
// isn't enough) and replace them. An optional mod.toml gives it a name:
//   name = "Teen Gohan (Super Hero)"
//   author = "..."
//   version = "1.0"
//   description = "..."
// mods\mods.toml lists the mods that are on:
//   enabled = ["Teen_Gohan_Super_Hero"]
// The menu stages switches until they're applied; then the archive serves the
// new files at once and the game's copy of its TOC is updated, so each file
// loads with the change from its next load (one already loaded keeps its old
// data until then).
// (the game reads the archive's table of contents once, at boot).
//
// The archive itself is never changed: the guest is served a virtual
// LONG2DATA_US.CPK (rex/filesystem/file_overlay.h) - the original bytes, with
// the TOC rows of the replaced files (FileOffset, FileSize, ExtractSize)
// pointing past the end of the original file, where the mod files follow
// (stored uncompressed: FileSize == ExtractSize, aligned like the archive).
// The ITOC (file sizes by ID) is left alone; the game opens its files by name.
// When two mods replace the same file, the one later in the list (folder
// names in alphabetical order) wins.
//
// Online, a different set of files is a desync, so the mods in use are part of
// the online version string: rooms only match between identical mod sets
// (nothing changes without mods).
//
// The main menu (sub_82251868, state 4 = sub_82251108) is a list widget of
// the game (sub_8228C1C0 makes it, sub_82288A70 adds an item, the cursor at
// +3592, its input in sub_8228DDA0). The entry is added after Options by
// hooks below; A on it opens the Mods panel, an ImGui overlay drawn in the
// game's style (its font and buttons from the archive) while the game keeps
// the main menu on screen without input.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/file_overlay.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/net/session.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/overlay_text.h>

#include "burstlimit_cpk.h"

REXCVAR_DEFINE_BOOL(mods_enabled, true, "Patches",
                    "Load the mods switched on in the Mods menu (mods folder next to "
                    "burstlimit.exe). false starts the game without them")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// burstlimit_online.cpp: sets the online version again (the mods are part of it).
void BurstLimitOnlineUpdateVersion(const std::string& mods);
// burstlimit_buttons.cpp: the button icons of the controller in use.
int BurstLimitButtonStyle();
bool BurstLimitRestyleWindowIcon(int index, int style, BurstLimitTexture& image);

namespace {

constexpr const char* kArchiveName = "LONG2DATA_US.CPK";
constexpr const char* kListFile = "mods.toml";
constexpr const char* kInfoFile = "mod.toml";

// ---------------------------------------------------------------- helpers

std::string Upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = c == '\\' ? '/' : char(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string PathUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}

uint16_t Be16(const uint8_t* p) {
  return uint16_t(p[0] << 8 | p[1]);
}
uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t Be64(const uint8_t* p) {
  return uint64_t(Be32(p)) << 32 | Be32(p + 4);
}
uint64_t Le64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = value << 8 | p[i];
  }
  return value;
}
void PutBe(uint8_t* p, uint64_t value, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    p[size - 1 - i] = uint8_t(value >> (8 * i));
  }
}

struct Fnv {
  uint64_t hash = 0xCBF29CE484222325ull;
  void Add(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 0x100000001B3ull;
    }
  }
  void Add(std::string_view text) {
    Add(text.data(), text.size());
    const uint8_t zero = 0;
    Add(&zero, 1);
  }
};

// A minimal reader of TOML string values: `key = "value"` lines (and arrays
// of strings for `enabled`).
std::string UnescapeToml(std::string_view text, size_t& pos) {
  std::string out;
  const char quote = text[pos++];
  while (pos < text.size() && text[pos] != quote) {
    char c = text[pos++];
    if (quote == '"' && c == '\\' && pos < text.size()) {
      const char e = text[pos++];
      switch (e) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        default:
          c = e;
          break;
      }
    }
    out += c;
  }
  ++pos;
  return out;
}

std::string EscapeToml(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + "\"";
}

bool ReadText(const std::filesystem::path& path, std::string& text) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB &&
      uint8_t(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  return true;
}

// key -> value for `key = "value"` lines; `key = [ "a", "b" ]` gives the
// strings joined by '\n'.
std::map<std::string, std::string> ParseToml(const std::string& text) {
  std::map<std::string, std::string> values;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string_view line(text.data() + pos, end - pos);
    size_t next = end + 1;
    const size_t eq = line.find('=');
    const size_t first = line.find_first_not_of(" \t\r");
    if (eq != std::string_view::npos && first != std::string_view::npos && line[first] != '#' &&
        line[first] != '[') {
      std::string key(line.substr(first, eq - first));
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
        key.pop_back();
      }
      size_t v = pos + eq + 1;
      while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) {
        ++v;
      }
      if (v < text.size() && (text[v] == '"' || text[v] == '\'')) {
        values[key] = UnescapeToml(text, v);
      } else if (v < text.size() && text[v] == '[') {
        std::string joined;
        ++v;
        while (v < text.size() && text[v] != ']') {
          if (text[v] == '"' || text[v] == '\'') {
            if (!joined.empty()) {
              joined += '\n';
            }
            joined += UnescapeToml(text, v);
          } else if (text[v] == '#') {
            while (v < text.size() && text[v] != '\n') {
              ++v;
            }
          } else {
            ++v;
          }
        }
        values[key] = joined;
        next = std::max(next, std::min(text.size(), v + 1));
      }
    }
    pos = next;
  }
  return values;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= text.size() && !text.empty()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    if (end > pos) {
      out.emplace_back(text.substr(pos, end - pos));
    }
    pos = end + 1;
  }
  return out;
}

// ---------------------------------------------------------------- the archive's TOC

// The parts of a CPK this needs: where the TOC's @UTF table is, the per-row
// columns of the files, and each file's path.
struct Archive {
  std::filesystem::path path;
  uint64_t size = 0;
  uint64_t align = 0x800;
  uint64_t base = 0;            // FileOffset is relative to this
  uint64_t table_position = 0;  // the TOC's @UTF table in the file
  std::vector<uint8_t> table;   // its bytes (not encrypted)
  uint32_t rows_offset = 0, row_length = 0, row_count = 0;
  struct Column {
    uint32_t offset = 0;  // in the row
    uint8_t type = 0;
  };
  Column file_size, extract_size, file_offset;
  std::vector<std::string> paths;  // per row, upper case, '/'
  std::map<std::string, std::vector<uint32_t>> by_path, by_name;

  static size_t TypeSize(uint8_t type) {
    switch (type) {
      case 0x0:
      case 0x1:
        return 1;
      case 0x2:
      case 0x3:
        return 2;
      case 0x4:
      case 0x5:
      case 0x8:
      case 0xA:
        return 4;
      case 0x6:
      case 0x7:
      case 0xB:
        return 8;
      default:
        return 0;
    }
  }

  struct Utf {
    std::vector<uint8_t> data;
    uint32_t rows_offset = 0, strings_offset = 0, row_length = 0, row_count = 0;
    struct Col {
      std::string name;
      uint8_t storage = 0, type = 0;
      uint32_t position = 0;  // in the row (0x50) or in the data (0x30)
    };
    std::vector<Col> columns;

    bool Parse(std::vector<uint8_t> bytes) {
      if (bytes.size() < 32 || std::memcmp(bytes.data(), "@UTF", 4) != 0) {
        return false;  // encrypted tables aren't handled
      }
      data = std::move(bytes);
      rows_offset = Be32(&data[8]) + 8;
      strings_offset = Be32(&data[12]) + 8;
      const uint16_t column_count = Be16(&data[24]);
      row_length = Be16(&data[26]);
      row_count = Be32(&data[28]);
      size_t pos = 32;
      uint32_t row_position = 0;
      for (uint16_t i = 0; i < column_count; ++i) {
        if (pos + 5 > data.size()) {
          return false;
        }
        Col col;
        col.storage = data[pos] & 0xF0;
        col.type = data[pos] & 0x0F;
        if (!String(Be32(&data[pos + 1]), col.name)) {
          return false;
        }
        pos += 5;
        const size_t size = TypeSize(col.type);
        if (!size) {
          return false;
        }
        if (col.storage == 0x30) {
          col.position = uint32_t(pos);
          pos += size;
        } else if (col.storage == 0x50) {
          col.position = row_position;
          row_position += uint32_t(size);
        } else {
          col.position = UINT32_MAX;
        }
        columns.push_back(std::move(col));
      }
      return row_position == row_length &&
             uint64_t(rows_offset) + uint64_t(row_count) * row_length <= data.size();
    }
    bool String(uint32_t offset, std::string& out) const {
      const size_t start = size_t(strings_offset) + offset;
      if (start >= data.size()) {
        return false;
      }
      const auto end = std::find(data.begin() + start, data.end(), uint8_t(0));
      out.assign(data.begin() + start, end);
      return true;
    }
    const Col* Find(std::string_view name) const {
      for (const Col& col : columns) {
        if (col.name == name) {
          return &col;
        }
      }
      return nullptr;
    }
    size_t At(uint32_t row, const Col& col) const {
      return col.storage == 0x50 ? size_t(rows_offset) + size_t(row) * row_length + col.position
                                 : col.position;
    }
    bool Int(uint32_t row, std::string_view name, uint64_t& value) const {
      const Col* col = Find(name);
      if (!col || col->position == UINT32_MAX) {
        return false;
      }
      const uint8_t* p = &data[At(row, *col)];
      switch (TypeSize(col->type)) {
        case 1:
          value = p[0];
          return true;
        case 2:
          value = Be16(p);
          return true;
        case 4:
          value = Be32(p);
          return true;
        case 8:
          value = Be64(p);
          return true;
        default:
          return false;
      }
    }
    bool Text(uint32_t row, std::string_view name, std::string& value) const {
      const Col* col = Find(name);
      if (!col || col->type != 0xA || col->position == UINT32_MAX) {
        value.clear();
        return false;
      }
      return String(Be32(&data[At(row, *col)]), value);
    }
  };

  static bool ReadChunk(std::ifstream& file, uint64_t offset, const char* magic,
                        std::vector<uint8_t>& out) {
    uint8_t head[16];
    file.seekg(std::streamoff(offset));
    if (!file.read(reinterpret_cast<char*>(head), sizeof(head)) ||
        std::memcmp(head, magic, 4) != 0) {
      return false;
    }
    const uint64_t size = Le64(head + 8);
    if (size < 32 || size > (64u << 20)) {
      return false;
    }
    out.resize(size_t(size));
    return bool(file.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)));
  }

  bool Open(const std::filesystem::path& archive_path, std::string& error) {
    path = archive_path;
    std::error_code ec;
    size = std::filesystem::file_size(path, ec);
    if (ec) {
      error = "can't read " + PathUtf8(path);
      return false;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes;
    Utf header;
    if (!file || !ReadChunk(file, 0, "CPK ", bytes) || !header.Parse(std::move(bytes))) {
      error = "not a CPK archive it can read";
      return false;
    }
    uint64_t toc_offset = 0, content_offset = 0, align = 0;
    if (!header.Int(0, "TocOffset", toc_offset)) {
      error = "the archive has no TOC";
      return false;
    }
    header.Int(0, "ContentOffset", content_offset);
    if (header.Int(0, "Align", align) && align) {
      this->align = align;
    }
    base = content_offset ? std::min(toc_offset, content_offset) : toc_offset;
    Utf toc;
    if (!ReadChunk(file, toc_offset, "TOC ", bytes) || !toc.Parse(std::move(bytes))) {
      error = "can't read the archive's TOC";
      return false;
    }
    auto column = [&](const char* name, Column& out) {
      const Utf::Col* col = toc.Find(name);
      if (!col || col->storage != 0x50 || (col->type != 0x4 && col->type != 0x5 &&
                                           col->type != 0x6 && col->type != 0x7)) {
        return false;
      }
      out.offset = col->position;
      out.type = col->type;
      return true;
    };
    if (!column("FileSize", file_size) || !column("ExtractSize", extract_size) ||
        !column("FileOffset", file_offset)) {
      error = "unexpected TOC layout";
      return false;
    }
    table_position = toc_offset + 16;
    rows_offset = toc.rows_offset;
    row_length = toc.row_length;
    row_count = toc.row_count;
    paths.resize(row_count);
    std::string dir, name;
    for (uint32_t row = 0; row < row_count; ++row) {
      toc.Text(row, "DirName", dir);
      toc.Text(row, "FileName", name);
      if (name.empty()) {
        continue;
      }
      paths[row] = Upper(dir.empty() ? name : dir + "/" + name);
      by_path[paths[row]].push_back(row);
      by_name[Upper(name)].push_back(row);
    }
    table = std::move(toc.data);
    return true;
  }

  // The rows a mod file named `relative` (upper case, '/') replaces.
  std::vector<uint32_t> Match(const std::string& relative) const {
    // The whole path, or a tail of it starting at a folder (mods may keep the
    // archive's folders under folders of their own).
    for (size_t start = 0; start < relative.size();) {
      auto it = by_path.find(relative.substr(start));
      if (it != by_path.end()) {
        return it->second;
      }
      const size_t slash = relative.find('/', start);
      if (slash == std::string::npos) {
        break;
      }
      start = slash + 1;
    }
    const size_t slash = relative.rfind('/');
    auto it = by_name.find(slash == std::string::npos ? relative : relative.substr(slash + 1));
    return it != by_name.end() ? it->second : std::vector<uint32_t>{};
  }

  // A TOC row's bytes in `table` (this table's layout).
  std::vector<uint8_t> Row(const std::vector<uint8_t>& t, uint32_t row) const {
    const size_t at = size_t(rows_offset) + size_t(row) * row_length;
    return std::vector<uint8_t>(t.begin() + at, t.begin() + at + row_length);
  }
  void CopyRow(const std::vector<uint8_t>& from, std::vector<uint8_t>& to, uint32_t row) const {
    const size_t at = size_t(rows_offset) + size_t(row) * row_length;
    std::memcpy(&to[at], &from[at], row_length);
  }

  void SetRow(std::vector<uint8_t>& patched, uint32_t row, uint64_t offset, uint64_t length) const {
    uint8_t* p = &patched[size_t(rows_offset) + size_t(row) * row_length];
    PutBe(p + file_offset.offset, offset - base, TypeSize(file_offset.type));
    PutBe(p + file_size.offset, length, TypeSize(file_size.type));
    PutBe(p + extract_size.offset, length, TypeSize(extract_size.type));
  }
};

// ---------------------------------------------------------------- the virtual archive

// The size the virtual archive reports (room for mods added while the game
// runs): 2 GiB less one alignment unit, so it fits a signed 32-bit size.
constexpr uint64_t kReportedSize = 0x7FFFF800ull;

class ModdedArchive final : public rex::filesystem::FileOverlay {
 public:
  // A mod file served past the archive's end.
  struct Piece {
    uint64_t position = 0;  // 0 until first mapped
    uint64_t size = 0;
    std::filesystem::path path;
    // The contents when they're served from memory (converted files).
    std::shared_ptr<const std::vector<uint8_t>> data;
    std::unique_ptr<rex::filesystem::FileHandle> handle;
    uint64_t hash = 0;  // of the contents as served
  };

  bool Open(const Archive& archive) {
    original_ = rex::filesystem::FileHandle::OpenExisting(
        archive.path, rex::filesystem::FileAccess::kFileReadData);
    if (!original_) {
      return false;
    }
    original_size_ = archive.size;
    table_position_ = archive.table_position;
    table_ = archive.table;
    align_ = archive.align;
    size_ = original_size_;
    // The game (CRI's file system) takes the archive's size when it opens it
    // at boot and doesn't read past it, so mods switched on later need room
    // there from the start: the archive reports a size with space to grow
    // (zeros until something is mapped there), below 2 GiB.
    reported_size_ = std::max<uint64_t>(original_size_, kReportedSize);
    return true;
  }

  // Points the TOC rows in `rows` at their pieces (new pieces are appended
  // after everything served so far, so data the game may still be reading
  // never moves) and every row mapped before but not now back at the
  // original file. Returns the rows whose TOC values changed.
  std::vector<uint32_t> Map(const Archive& archive,
                            const std::map<uint32_t, std::shared_ptr<Piece>>& rows) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [row, piece] : rows) {
      if (piece->position) {
        continue;
      }
      if (!piece->data && !piece->handle) {
        piece->handle = rex::filesystem::FileHandle::OpenExisting(
            piece->path, rex::filesystem::FileAccess::kFileReadData);
        if (!piece->handle) {
          REXLOG_ERROR("Mods: can't open {}", PathUtf8(piece->path));
          continue;
        }
      }
      const uint64_t position = (size_ + align_ - 1) / align_ * align_;
      if (position + piece->size > reported_size_) {
        REXLOG_ERROR("Mods: no room left for {} in the archive until the game restarts",
                     PathUtf8(piece->path));
        continue;
      }
      piece->position = position;
      size_ = (piece->position + piece->size + align_ - 1) / align_ * align_;
      pieces_.push_back(piece);
    }
    std::set<uint32_t> touched;
    for (const auto& [row, piece] : mapped_) {
      touched.insert(row);
    }
    for (const auto& [row, piece] : rows) {
      touched.insert(row);
    }
    std::vector<uint32_t> changed;
    std::map<uint32_t, std::shared_ptr<Piece>> mapped;
    for (uint32_t row : touched) {
      std::vector<uint8_t> before = archive.Row(table_, row);
      auto it = rows.find(row);
      if (it != rows.end() && it->second->position) {
        archive.SetRow(table_, row, it->second->position, it->second->size);
        mapped[row] = it->second;
      } else {
        archive.CopyRow(archive.table, table_, row);  // back to the original
      }
      if (archive.Row(table_, row) != before) {
        changed.push_back(row);
      }
    }
    mapped_ = std::move(mapped);
    return changed;
  }

  // The TOC row as the archive now has it.
  std::vector<uint8_t> Row(const Archive& archive, uint32_t row) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return archive.Row(table_, row);
  }

  size_t mapped() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return mapped_.size();
  }

  uint64_t size() const override { return reported_size_; }

  // Where the served data ends.
  uint64_t data_size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return size_;
  }

  bool Read(uint64_t offset, void* buffer, size_t length, size_t* out_bytes_read) override {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    *out_bytes_read = 0;
    if (offset >= reported_size_) {
      return true;
    }
    length = size_t(std::min<uint64_t>(length, reported_size_ - offset));
    auto* out = static_cast<uint8_t*>(buffer);
    const uint64_t end = offset + length;
    // The original file, with the patched TOC table over it.
    if (offset < original_size_) {
      const size_t count = size_t(std::min<uint64_t>(end, original_size_) - offset);
      size_t read = 0;
      if (!original_->Read(size_t(offset), out, count, &read) || read != count) {
        return false;
      }
      const uint64_t table_end = table_position_ + table_.size();
      const uint64_t from = std::max(offset, table_position_);
      const uint64_t to = std::min(offset + count, table_end);
      if (from < to) {
        std::memcpy(out + (from - offset), table_.data() + (from - table_position_),
                    size_t(to - from));
      }
    }
    // The mod files after it (zeros between them).
    if (end > original_size_) {
      const uint64_t from = std::max(offset, original_size_);
      std::memset(out + (from - offset), 0, size_t(end - from));
      for (const auto& piece : pieces_) {
        const uint64_t a = std::max(from, piece->position);
        const uint64_t b = std::min(end, piece->position + piece->size);
        if (a >= b) {
          continue;
        }
        if (piece->data) {
          std::memcpy(out + (a - offset), piece->data->data() + (a - piece->position),
                      size_t(b - a));
          continue;
        }
        size_t read = 0;
        if (!piece->handle ||
            !piece->handle->Read(size_t(a - piece->position), out + (a - offset), size_t(b - a),
                                 &read)) {
          REXLOG_ERROR("Mods: reading {} failed", PathUtf8(piece->path));
          return false;
        }
      }
    }
    *out_bytes_read = length;
    return true;
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unique_ptr<rex::filesystem::FileHandle> original_;
  uint64_t original_size_ = 0;
  uint64_t size_ = 0;           // the end of the data served
  uint64_t reported_size_ = 0;  // what the guest is told
  uint64_t align_ = 0x800;
  uint64_t table_position_ = 0;
  std::vector<uint8_t> table_;
  std::vector<std::shared_ptr<Piece>> pieces_;
  std::map<uint32_t, std::shared_ptr<Piece>> mapped_;
};

// ---------------------------------------------------------------- the mods

struct ModFile {
  std::filesystem::path path;
  std::string relative;  // as in the mod folder, '/'
  uint64_t size = 0;
  std::vector<uint32_t> rows;  // the archive's files it replaces
  bool ps3 = false;            // made for the PS3 version: converted as it loads
};

struct Mod {
  std::string folder;
  std::string name, author, version, description;
  std::vector<ModFile> files;         // replacing game files
  std::vector<std::string> unknown;   // matching no game file (ignored)
  bool loaded = false;  // on at this start: its files are in the game now
  bool saved = false;   // on in mods.toml
  bool staged = false;  // on in the menu, maybe not applied yet
};

struct Conflict {
  std::string file;          // the archive path
  std::vector<size_t> mods;  // in list order; the last one wins
};

struct ModsState {
  std::mutex mutex;
  std::filesystem::path directory;
  std::vector<Mod> mods;
  std::vector<std::string> archive_paths;  // per TOC row
  std::string archive_error;
  bool enabled = true;  // mods_enabled at this start
  std::string online_tag;
  std::shared_ptr<ModdedArchive> overlay;
  std::filesystem::path archive_path;
  std::unique_ptr<Archive> archive;
  // The served mod files, by mod folder + path (kept so re-applying a mod
  // serves the same data at the same place).
  std::map<std::string, std::shared_ptr<ModdedArchive::Piece>> pieces;
  // TOC rows changed since the game's copy of the TOC was last updated.
  std::vector<uint32_t> pending_rows;
  uint32_t guest_toc = 0;
  bool guest_toc_missing = false;
};

ModsState& State() {
  static ModsState* state = new ModsState();
  return *state;
}

std::vector<std::string> ReadEnabledList(const std::filesystem::path& directory) {
  std::string text;
  if (!ReadText(directory / kListFile, text)) {
    return {};
  }
  const auto values = ParseToml(text);
  auto it = values.find("enabled");
  return it != values.end() ? SplitLines(it->second) : std::vector<std::string>{};
}

bool WriteEnabledList(const std::filesystem::path& directory,
                      const std::vector<std::string>& folders) {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const std::filesystem::path path = directory / kListFile;
  const std::filesystem::path temp = directory / "mods.toml.tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file << "# Mods that are switched on: folder names in this folder. The Mods entry of the\n"
            "# main menu writes this file.\n"
            "# When two mods replace the same file, the one later in alphabetical order wins.\n";
    file << "enabled = [";
    for (size_t i = 0; i < folders.size(); ++i) {
      file << (i ? ", " : "") << EscapeToml(folders[i]);
    }
    file << "]\n";
    if (!file) {
      return false;
    }
  }
  std::filesystem::rename(temp, path, ec);
  return !ec;
}

std::string DisplayName(const std::string& folder) {
  std::string name = folder;
  std::replace(name.begin(), name.end(), '_', ' ');
  return name;
}

bool IsPs3ModelFile(const std::filesystem::path& path);

void ScanMods(ModsState& state, const Archive* archive) {
  std::error_code ec;
  std::vector<std::filesystem::path> folders;
  for (auto it = std::filesystem::directory_iterator(state.directory, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec)) {
      folders.push_back(it->path());
    }
  }
  std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) {
    return Upper(PathUtf8(a.filename())) < Upper(PathUtf8(b.filename()));
  });
  for (const auto& folder : folders) {
    Mod mod;
    mod.folder = PathUtf8(folder.filename());
    std::string text;
    if (ReadText(folder / kInfoFile, text)) {
      const auto values = ParseToml(text);
      auto get = [&](const char* key) {
        auto it = values.find(key);
        return it != values.end() ? it->second : std::string();
      };
      mod.name = get("name");
      mod.author = get("author");
      mod.version = get("version");
      mod.description = get("description");
    }
    if (mod.name.empty()) {
      mod.name = DisplayName(mod.folder);
    }
    std::vector<ModFile> files;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        continue;
      }
      ModFile file;
      file.path = it->path();
      file.relative = PathUtf8(it->path().lexically_relative(folder));
      if (Upper(file.relative) == Upper(kInfoFile)) {
        continue;
      }
      file.size = it->file_size(ec);
      file.ps3 = IsPs3ModelFile(file.path);
      if (archive) {
        file.rows = archive->Match(Upper(file.relative));
      }
      if (file.rows.empty()) {
        mod.unknown.push_back(file.relative);
      } else {
        files.push_back(std::move(file));
      }
    }
    std::sort(files.begin(), files.end(),
              [](const ModFile& a, const ModFile& b) { return a.relative < b.relative; });
    mod.files = std::move(files);
    state.mods.push_back(std::move(mod));
  }
}

// Which mod each archive file comes from, for mods that are on (`on`), the
// later in the list winning.
std::map<uint32_t, std::pair<size_t, size_t>> Winners(const ModsState& state,
                                                      const std::vector<bool>& on) {
  std::map<uint32_t, std::pair<size_t, size_t>> winners;
  for (size_t m = 0; m < state.mods.size(); ++m) {
    if (!on[m]) {
      continue;
    }
    for (size_t f = 0; f < state.mods[m].files.size(); ++f) {
      for (uint32_t row : state.mods[m].files[f].rows) {
        winners[row] = {m, f};
      }
    }
  }
  return winners;
}

std::vector<Conflict> Conflicts(const ModsState& state, const std::vector<bool>& on) {
  std::map<uint32_t, std::vector<size_t>> users;
  for (size_t m = 0; m < state.mods.size(); ++m) {
    if (!on[m]) {
      continue;
    }
    for (const ModFile& file : state.mods[m].files) {
      for (uint32_t row : file.rows) {
        auto& list = users[row];
        if (list.empty() || list.back() != m) {
          list.push_back(m);
        }
      }
    }
  }
  std::vector<Conflict> conflicts;
  for (const auto& [row, mods] : users) {
    if (mods.size() > 1) {
      Conflict conflict;
      conflict.file = row < state.archive_paths.size() ? state.archive_paths[row] : "?";
      conflict.mods = mods;
      conflicts.push_back(std::move(conflict));
    }
  }
  return conflicts;
}

bool HashFile(const std::filesystem::path& path, Fnv& fnv) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<char> buffer(1 << 20);
  while (file) {
    file.read(buffer.data(), std::streamsize(buffer.size()));
    fnv.Add(buffer.data(), size_t(file.gcount()));
  }
  return true;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

// Model packs made for the PS3 version of the game: the same #NUX files, but
// the textures are "NTP3" (DXT blocks in the usual byte order) and the model
// "NDP3". The Xbox 360 version wants "NTXR" (DXT data as big-endian 16-bit
// words) and "NDXR"; the rest of the layout is the same.
bool IsPs3ModelFile(const std::filesystem::path& path) {
  uint8_t head[16] = {};
  std::ifstream file(path, std::ios::binary);
  if (!file.read(reinterpret_cast<char*>(head), 16)) {
    return false;
  }
  if (std::memcmp(head, "NTP3", 4) == 0) {
    return true;
  }
  if (std::memcmp(head, "#NUX", 4) != 0) {
    return false;
  }
  const uint32_t nut = Be32(head + 4);
  uint8_t magic[4] = {};
  file.seekg(std::streamoff(nut));
  return file.read(reinterpret_cast<char*>(magic), 4) && std::memcmp(magic, "NTP3", 4) == 0;
}

bool ConvertPs3Textures(std::vector<uint8_t>& d, size_t base) {
  if (base + 0x10 > d.size() || std::memcmp(&d[base], "NTP3", 4) != 0) {
    return false;
  }
  std::memcpy(&d[base], "NTXR", 4);
  const uint16_t count = Be16(&d[base + 6]);
  size_t pos = base + 0x10;
  for (uint16_t i = 0; i < count && pos + 24 <= d.size(); ++i) {
    const uint32_t total = Be32(&d[pos]);
    const uint32_t data_size = Be32(&d[pos + 8]);
    const uint16_t header_size = Be16(&d[pos + 12]);
    const uint16_t format = Be16(&d[pos + 18]);
    const size_t start = pos + header_size;
    if (format <= 2 && start + data_size <= d.size()) {
      // DXT1/3/5.
      for (size_t b = start; b + 1 < start + data_size; b += 2) {
        std::swap(d[b], d[b + 1]);
      }
    } else {
      REXLOG_WARN("Mods: PS3 texture {} has format {} - left as it is", i, format);
    }
    if (!total) {
      break;
    }
    pos += total;
  }
  return true;
}

// What a PS3 model needed to become an Xbox 360 one, counted for the log.
struct Ps3Conversion {
  int models = 0;
  int materials = 0;
  int textures = 0;
  int vertices = 0;
  std::set<std::string> unknown;  // values without a known Xbox 360 equivalent
};

uint16_t ReadBe16(const std::vector<uint8_t>& d, size_t at) {
  return at + 2 <= d.size() ? Be16(&d[at]) : 0;
}
uint32_t ReadBe32(const std::vector<uint8_t>& d, size_t at) {
  return at + 4 <= d.size() ? Be32(&d[at]) : 0;
}
void WriteBe16(std::vector<uint8_t>& d, size_t at, uint16_t value) {
  if (at + 2 <= d.size()) {
    d[at] = uint8_t(value >> 8);
    d[at + 1] = uint8_t(value);
  }
}

// The PS3 version's material enums (the RSX's, as cellGcm names them) -> the
// Xbox 360 version's, from the same character exported for both. See
// C:\rex\_mods_test\PS3_TO_X360.md.
uint16_t CullModeToXbox(uint16_t ps3, Ps3Conversion& log) {
  switch (ps3) {
    case 0x0000:
      return 0;  // no culling
    case 0x0405:
      return 2;  // CELL_GCM_BACK
    case 0x0404:
      return 1;  // CELL_GCM_FRONT
    default:
      log.unknown.insert(fmt::format("cull {:04X}", ps3));
      return ps3 > 0xFF ? 2 : ps3;
  }
}
uint8_t WrapToXbox(uint8_t ps3, Ps3Conversion& log) {
  switch (ps3) {
    case 1:  // WRAP
      return 0;
    case 2:  // MIRROR
      return 1;
    case 3:  // CLAMP_TO_EDGE
    case 4:  // BORDER
    case 5:  // CLAMP
      return 2;
    default:
      log.unknown.insert(fmt::format("wrap {}", ps3));
      return 2;
  }
}
uint8_t MinFilterToXbox(uint8_t ps3, Ps3Conversion& log) {
  switch (ps3) {
    case 1:  // NEAREST
    case 3:  // NEAREST_NEAREST
    case 5:  // NEAREST_LINEAR
      return 1;
    case 2:  // LINEAR
    case 4:  // LINEAR_NEAREST
    case 6:  // LINEAR_LINEAR
      return 2;
    default:
      log.unknown.insert(fmt::format("min filter {}", ps3));
      return 2;
  }
}
uint8_t MagFilterToXbox(uint8_t ps3, Ps3Conversion& log) {
  switch (ps3) {
    case 1:  // NEAREST
      return 0;
    case 2:  // LINEAR
    case 4:  // CONVOLUTION
      return 1;
    default:
      log.unknown.insert(fmt::format("mag filter {}", ps3));
      return 1;
  }
}

// A NUD (model) at `s`: "NDP3" -> "NDXR", its materials' enums, and the
// vertex colors' byte order (PS3 R G B A, Xbox 360 A R G B).
//   header: magic, size, u16 version, u16 objects, u16 type, u16 bones, the
//     sizes of the descriptor block (from +0x30), index, vertex and
//     vertex-add clumps;
//   objects (0x30 each from +0x30): ... +0x2A u16 polygon groups, +0x2C their
//     descriptors' offset;
//   polygon groups (0x30): +4 vertex start (in the vertex clump), +12 u16
//     vertex count, +14 vertex flags (high nibble: bone weights, which put
//     the positions in the vertex-add clump), +15 UV flags (high nibble: UV
//     count, low nibble: color type, 2 = 4 bytes), +16 four material offsets;
//   materials: u32 flags, pad, u16 src blend, u16 texture count, u16 dst
//     blend, u8 alpha test, u8 alpha func, u16 ref, u16 cull mode, ... (32
//     bytes), then 24 bytes per texture: u32 id, ..., +12 u8 wrap s, wrap t,
//     min filter, mag filter, mip mode, +17 u8 mip flags.
bool ConvertPs3Nud(std::vector<uint8_t>& d, size_t s, Ps3Conversion& log) {
  if (s + 0x30 > d.size() || std::memcmp(&d[s], "NDP3", 4) != 0) {
    return false;
  }
  std::memcpy(&d[s], "NDXR", 4);
  ++log.models;
  const uint16_t objects = ReadBe16(d, s + 10);
  const size_t vertex_clump = s + 0x30 + ReadBe32(d, s + 16) + ReadBe32(d, s + 20);
  const uint32_t vertex_clump_size = ReadBe32(d, s + 24);
  struct Group {
    uint32_t start, count;
    uint8_t vertex_flags, uv_flags;
  };
  std::vector<Group> groups;
  std::set<uint32_t> materials;
  for (uint16_t o = 0; o < objects; ++o) {
    const size_t object = s + 0x30 + size_t(o) * 0x30;
    if (object + 0x30 > d.size()) {
      break;
    }
    const uint16_t count = ReadBe16(d, object + 0x2A);
    const uint32_t descriptors = ReadBe32(d, object + 0x2C);
    for (uint16_t g = 0; g < count; ++g) {
      const size_t group = s + descriptors + size_t(g) * 0x30;
      if (group + 0x30 > d.size()) {
        break;
      }
      groups.push_back({ReadBe32(d, group + 4), ReadBe16(d, group + 12), d[group + 14],
                        d[group + 15]});
      for (int m = 0; m < 4; ++m) {
        if (const uint32_t material = ReadBe32(d, group + 16 + m * 4)) {
          materials.insert(material);
        }
      }
    }
  }
  for (uint32_t offset : materials) {
    const size_t material = s + offset;
    if (material + 32 > d.size()) {
      continue;
    }
    ++log.materials;
    WriteBe16(d, material + 18, CullModeToXbox(ReadBe16(d, material + 18), log));
    const uint16_t textures = ReadBe16(d, material + 10);
    for (uint16_t t = 0; t < textures; ++t) {
      const size_t texture = material + 32 + size_t(t) * 24;
      if (texture + 24 > d.size()) {
        break;
      }
      ++log.textures;
      d[texture + 12] = WrapToXbox(d[texture + 12], log);
      d[texture + 13] = WrapToXbox(d[texture + 13], log);
      d[texture + 14] = MinFilterToXbox(d[texture + 14], log);
      d[texture + 15] = MagFilterToXbox(d[texture + 15], log);
      // Mip mode: the PS3's 4 is the Xbox's 1; its 2 is 1 with flags 2.
      const uint8_t mip = d[texture + 16];
      if (mip == 2) {
        d[texture + 16] = 1;
        d[texture + 17] = 2;
      } else if (mip == 4 || mip == 1) {
        d[texture + 16] = 1;
      } else if (mip != 0) {
        log.unknown.insert(fmt::format("mip mode {}", mip));
        d[texture + 16] = 1;
      }
    }
  }
  // Vertex colors: the color and the UVs end every vertex in the vertex
  // clump (the whole vertex without bone weights, only them with), so each
  // group's stride comes from where the next one starts.
  std::vector<Group> ordered = groups;
  std::sort(ordered.begin(), ordered.end(),
            [](const Group& a, const Group& b) { return a.start < b.start; });
  for (size_t i = 0; i < ordered.size(); ++i) {
    const Group& group = ordered[i];
    const uint32_t color_type = group.uv_flags & 0x0F;
    if (!group.count || !color_type) {
      continue;
    }
    if (color_type != 2) {
      log.unknown.insert(fmt::format("vertex color type {}", color_type));
      continue;
    }
    const uint32_t end = i + 1 < ordered.size() ? ordered[i + 1].start : vertex_clump_size;
    if (end <= group.start) {
      continue;
    }
    const uint32_t stride = (end - group.start) / group.count;
    const uint32_t tail = 4 + (group.uv_flags >> 4) * 4;
    if (stride < tail || stride > 256) {
      log.unknown.insert(fmt::format("vertex layout {:02X}/{:02X}", group.vertex_flags,
                                     group.uv_flags));
      continue;
    }
    for (uint32_t v = 0; v < group.count; ++v) {
      const size_t color = vertex_clump + group.start + size_t(v) * stride + (stride - tail);
      if (color + 4 > d.size()) {
        break;
      }
      // R G B A -> A R G B
      std::rotate(d.begin() + color, d.begin() + color + 3, d.begin() + color + 4);
      ++log.vertices;
    }
  }
  return true;
}

bool ConvertPs3ModelFile(std::vector<uint8_t>& d, Ps3Conversion& log) {
  if (d.size() >= 16 && std::memcmp(d.data(), "NTP3", 4) == 0) {
    return ConvertPs3Textures(d, 0);
  }
  if (d.size() < 16 || std::memcmp(d.data(), "#NUX", 4) != 0) {
    return false;
  }
  bool converted = ConvertPs3Textures(d, Be32(&d[4]));
  // The #NUB: the body's model 0x80 bytes in, then a table (+8 count, from
  // +0x24 u32 id, u32 offset) of more models (the face's expressions), each
  // 0x80 bytes after its offset.
  const size_t nub = Be32(&d[8]);
  if (nub + 0x24 <= d.size() && std::memcmp(&d[nub], "#NUB", 4) == 0) {
    std::vector<size_t> models = {nub + 0x80};
    const uint32_t count = ReadBe32(d, nub + 8);
    for (uint32_t i = 0; i < count && i < 256; ++i) {
      const uint32_t offset = ReadBe32(d, nub + 0x24 + i * 8 + 4);
      if (offset) {
        models.push_back(nub + offset + 0x80);
      }
    }
    for (size_t model : models) {
      // Tolerate a different header size: the magic within the first 0x100.
      for (size_t p = model - std::min<size_t>(model - nub, 0x80);
           p + 4 <= std::min(d.size(), model + 0x100); p += 4) {
        if (std::memcmp(&d[p], "NDP3", 4) == 0) {
          converted |= ConvertPs3Nud(d, p, log);
          break;
        }
      }
    }
    // Any model the table didn't list.
    for (size_t p = nub; p + 4 <= d.size(); p += 16) {
      if (std::memcmp(&d[p], "NDP3", 4) == 0 && ReadBe16(d, p + 8) == 0x0200) {
        converted |= ConvertPs3Nud(d, p, log);
      }
    }
  }
  return converted;
}

bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
  if (Upper(PathUtf8(a.lexically_normal())) == Upper(PathUtf8(b.lexically_normal()))) {
    return true;
  }
  std::error_code ec;
  return std::filesystem::equivalent(a, b, ec) && !ec;
}

// ---------------------------------------------------------------- main menu

constexpr uint32_t kListItems = 3524;   // the items' container: +0 count
constexpr uint32_t kListCursor = 3592;
constexpr uint32_t kModsItem = 6;       // after Options (5)
constexpr uint32_t kOptionsItem = 5;
// The description box keeps the id of the message it shows (and skips the
// same id); the main menu's are 7-12, so this one can't clash.
constexpr uint32_t kModsHelpId = 0x4D4F44;
constexpr const char16_t* kModsLabel = u"Mods";
constexpr const char16_t* kModsHelp = u"Turn mods on or off.";

std::atomic<uint32_t> g_main_list{0};
std::atomic<bool> g_open_request{false};

// The game's menu sounds (sub_8210C198, bank 0, as its lists play them in
// sub_8228DDA0): queued by the panel, played on the game's thread by the
// main menu hook below.
enum MenuSound : uint32_t {
  kSoundMove = 1,      // the cursor moved
  kSoundConfirm = 3,   // an item confirmed
  kSoundError = 9,     // an item that can't be picked
  kSoundCancel = 10,   // back
};
std::mutex g_sound_mutex;
std::vector<uint32_t> g_sounds;

void PlayMenuSound(uint32_t sound) {
  std::lock_guard<std::mutex> lock(g_sound_mutex);
  if (g_sounds.size() < 8) {
    g_sounds.push_back(sound);
  }
}
uint32_t g_label = 0, g_help = 0;
bool g_adding = false;

rex::memory::Memory* GuestMemory() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->memory() : nullptr;
}

uint32_t R32(rex::memory::Memory* memory, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(address));
}

uint32_t GuestText(rex::memory::Memory* memory, const char16_t* text) {
  uint32_t length = 0;
  while (text[length]) {
    ++length;
  }
  const uint32_t address = memory->SystemHeapAlloc((length + 1) * 2);
  if (!address) {
    return 0;
  }
  for (uint32_t c = 0; c <= length; ++c) {
    rex::memory::store_and_swap<uint16_t>(memory->TranslateVirtual<uint8_t*>(address + c * 2),
                                          uint16_t(text[c]));
  }
  return address;
}

bool IsModsList(rex::memory::Memory* memory, uint32_t list) {
  if (!memory || !list || list != g_main_list.load(std::memory_order_relaxed)) {
    return false;
  }
  const uint32_t items = R32(memory, list + kListItems);
  return items && R32(memory, items) == kModsItem + 1;
}

// The mods that are on (`on`), into the archive: each winning file becomes a
// piece (made once and kept - PS3 files converted), the overlay maps the
// rows, and the rows whose TOC values changed are queued for the copy of the
// TOC in guest memory. Also sets the online tag. Called with state.mutex held.
void MapMods(ModsState& state, const std::vector<bool>& on) {
  if (!state.overlay || !state.archive) {
    return;
  }
  const Archive& archive = *state.archive;
  for (const Conflict& conflict : Conflicts(state, on)) {
    REXLOG_WARN("Mods: {} is in {} mods - {} wins", conflict.file, conflict.mods.size(),
                state.mods[conflict.mods.back()].folder);
  }
  std::map<uint32_t, std::shared_ptr<ModdedArchive::Piece>> rows;
  Fnv fnv;
  for (const auto& [row, source] : Winners(state, on)) {
    const Mod& mod = state.mods[source.first];
    const ModFile& file = mod.files[source.second];
    const std::string key = mod.folder + "/" + file.relative;
    std::shared_ptr<ModdedArchive::Piece>& piece = state.pieces[key];
    if (!piece) {
      piece = std::make_shared<ModdedArchive::Piece>();
      piece->size = file.size;
      piece->path = file.path;
      if (file.ps3) {
        auto data = std::make_shared<std::vector<uint8_t>>();
        Ps3Conversion log;
        if (ReadFile(file.path, *data) && ConvertPs3ModelFile(*data, log)) {
          REXLOG_INFO("Mods:     {} is made for the PS3 version - converted: textures, {} "
                      "model(s), {} material(s), {} texture setting(s), {} vertex color(s)",
                      file.relative, log.models, log.materials, log.textures, log.vertices);
          for (const std::string& unknown : log.unknown) {
            REXLOG_WARN("Mods:     {}: no known Xbox 360 equivalent for PS3 {} - guessed",
                        file.relative, unknown);
          }
          piece->size = data->size();
          piece->data = std::move(data);
        }
      }
      Fnv contents;
      if (piece->data) {
        contents.Add(piece->data->data(), piece->data->size());
      } else {
        HashFile(file.path, contents);
      }
      piece->hash = contents.hash;
    }
    rows[row] = piece;
    // The online tag: which archive file has which contents (as served).
    fnv.Add(archive.paths[row]);
    fnv.Add(&piece->hash, sizeof(piece->hash));
  }
  const std::vector<uint32_t> changed = state.overlay->Map(archive, rows);
  for (uint32_t row : changed) {
    if (std::find(state.pending_rows.begin(), state.pending_rows.end(), row) ==
        state.pending_rows.end()) {
      state.pending_rows.push_back(row);
    }
  }
  if (rows.empty()) {
    state.online_tag.clear();
  } else {
    char tag[32];
    std::snprintf(tag, sizeof(tag), "mods.%08x", uint32_t(fnv.hash ^ (fnv.hash >> 32)));
    state.online_tag = tag;
  }
  REXLOG_INFO("Mods: {} file(s) replaced, {} TOC row(s) changed; the game reads {} as {} "
              "bytes ({} on disk); online tag {}",
              rows.size(), changed.size(), kArchiveName, state.overlay->data_size(), archive.size,
              state.online_tag.empty() ? "(none)" : state.online_tag);
  for (Mod& mod : state.mods) {
    mod.loaded = false;
  }
  for (size_t m = 0; m < state.mods.size(); ++m) {
    state.mods[m].loaded = on[m] && !state.mods[m].files.empty();
  }
}

// The guest's copy of the archive's TOC (the @UTF table the game read at
// boot and looks files up in): found once by its first bytes.
uint32_t FindGuestToc(const Archive& archive) {
  auto* memory = GuestMemory();
  if (!memory || archive.table.size() < 64) {
    return 0;
  }
  const uint8_t* want = archive.table.data();
  uint64_t address = 0x10000;
  while (address < 0x100000000ull) {
    auto* heap = memory->LookupHeap(uint32_t(address));
    if (!heap) {
      address += 0x10000;
      continue;
    }
    rex::memory::HeapAllocationInfo info = {};
    if (!heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
      address += heap->page_size();
      continue;
    }
    const uint64_t region_end = uint64_t(info.base_address) + info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & rex::memory::kMemoryProtectRead) &&
        region_end - address >= archive.table.size()) {
      const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(uint32_t(address));
      const size_t size = size_t(region_end - address);
      for (size_t offset = 0; offset + archive.table.size() <= size; offset += 4) {
        if (host[offset] == '@' && std::memcmp(host + offset, want, 64) == 0) {
          return uint32_t(address + offset);
        }
      }
    }
    address = std::max(region_end, address + 4);
  }
  return 0;
}

}  // namespace

// On the game's thread, at the main menu (BurstLimitModsMenuSound): the
// changed TOC rows into the guest's copy of the TOC, so the next time the
// game loads one of those files it reads the new data. Files already loaded
// keep what they have until loaded again.
void BurstLimitModsPatchGuestToc() {
  ModsState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.pending_rows.empty() || !state.overlay || !state.archive) {
    return;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return;
  }
  if (!state.guest_toc && !state.guest_toc_missing) {
    state.guest_toc = FindGuestToc(*state.archive);
    state.guest_toc_missing = !state.guest_toc;
    if (state.guest_toc) {
      REXLOG_INFO("Mods: the game's copy of the TOC is at {:08X}", state.guest_toc);
    } else {
      REXLOG_ERROR("Mods: the game's copy of the TOC wasn't found - changes load after a "
                   "restart");
    }
  }
  if (state.guest_toc) {
    const Archive& archive = *state.archive;
    for (uint32_t row : state.pending_rows) {
      const std::vector<uint8_t> bytes = state.overlay->Row(archive, row);
      uint8_t* guest = memory->TranslateVirtual<uint8_t*>(
          state.guest_toc + archive.rows_offset + row * archive.row_length);
      std::memcpy(guest, bytes.data(), bytes.size());
    }
    REXLOG_INFO("Mods: {} TOC row(s) updated in the game", state.pending_rows.size());
  }
  state.pending_rows.clear();
}

namespace {
}  // namespace

// ---------------------------------------------------------------- hooks

// Mid-asm hook at 0x8224F7B0 in sub_8224F690 (the main menu's list is made:
// r30 = the list, its 6 items just added), before it's measured: adds "Mods"
// by running the last add again (bl sub_82288A70 at 0x8224F7AC with r3 = the
// list, r4 = the text), which comes back here.
bool BurstLimitModsMenuAddItem(PPCRegister& r3, PPCRegister& r4, PPCRegister& r30) {
  if (g_adding) {
    g_adding = false;
    return false;
  }
  auto* memory = GuestMemory();
  if (!memory || !r30.u32) {
    return false;
  }
  if (!g_label) {
    g_label = GuestText(memory, kModsLabel);
  }
  const uint32_t items = R32(memory, r30.u32 + kListItems);
  if (!g_label || !items || R32(memory, items) != kModsItem) {
    return false;
  }
  g_main_list.store(r30.u32, std::memory_order_relaxed);
  g_adding = true;
  REXLOG_INFO("Mods menu: entry added to the main menu (list {:08X})", r30.u32);
  r3.u64 = r30.u32;
  r4.u64 = g_label;
  return true;
}

// Mid-asm hook at 0x82251278 in sub_82251108 (the main menu, the cursor moved:
// r30 = the item): the pictures and the slogan behind the list come from
// 6-entry tables - Mods keeps Options' ones.
void BurstLimitModsMenuArtCursor(PPCRegister& r30) {
  if (r30.s32 == int32_t(kModsItem)) {
    r30.u64 = kOptionsItem;
  }
}

// Mid-asm hook at 0x82251794 in sub_82251108 (r3 = the item, picking its
// description, only for items 0-5): Mods gets its own (jumps to 0x822517B4
// with r30 = its id, r3 = 0).
bool BurstLimitModsMenuHelpIndex(PPCRegister& r3, PPCRegister& r30) {
  if (r3.u32 != kModsItem) {
    return false;
  }
  r30.u64 = kModsHelpId;
  r3.u64 = 0;
  return true;
}

// Mid-asm hook at 0x822517C8 in sub_82251108, before the description's text
// is looked up (bl sub_822803E0, r30 = its id): Mods' text (jumps to
// 0x822517CC with r3 = it).
bool BurstLimitModsMenuHelpText(PPCRegister& r3, PPCRegister& r30) {
  if (r30.u32 != kModsHelpId) {
    return false;
  }
  auto* memory = GuestMemory();
  if (!memory) {
    return false;
  }
  if (!g_help) {
    g_help = GuestText(memory, kModsHelp);
  }
  if (!g_help) {
    return false;
  }
  r3.u64 = g_help;
  return true;
}

// Mid-asm hook at 0x8228E8DC in sub_8228DDA0 (a list's input, its cursor
// on an item: beq when the confirm button isn't pressed, r31 = the list). A
// on Mods opens the Mods panel instead of confirming the main menu.
void BurstLimitModsMenuConfirm(PPCCRRegister& cr6, PPCRegister& r31) {
  if (cr6.eq) {
    return;
  }
  auto* memory = GuestMemory();
  if (!IsModsList(memory, r31.u32) || R32(memory, r31.u32 + kListCursor) != kModsItem) {
    return;
  }
  cr6.eq = 1;
  if (!g_open_request.exchange(true)) {
    REXLOG_INFO("Mods menu: opened from the main menu");
  }
}

// Mid-asm hook at 0x82251620 in sub_82251108 (the main menu, every frame,
// after its own optional sound call - bl sub_8210C198 at 0x8225161C - and
// before r3/r4/r6/f1 are set again): plays the Mods panel's queued sounds by
// running that call again (r3 = bank 0, r4 = sound, r6 = 0, f1 = 1.0), which
// comes back here for the next one.
bool BurstLimitModsMenuSound(PPCRegister& r3, PPCRegister& r4, PPCRegister& r6,
                             PPCRegister& f1) {
  BurstLimitModsPatchGuestToc();
  uint32_t sound;
  {
    std::lock_guard<std::mutex> lock(g_sound_mutex);
    if (g_sounds.empty()) {
      return false;
    }
    sound = g_sounds.front();
    g_sounds.erase(g_sounds.begin());
  }
  REXLOG_INFO("Mods menu: sound {}", sound);
  r3.u64 = 0;
  r4.u64 = sound;
  r6.u64 = 0;
  f1.f64 = 1.0;
  return true;
}

// ---------------------------------------------------------------- setup

// At startup (OnPreSetup, before the game's files are mounted): reads the
// mods and, with any switched on, serves the modded archive.
void BurstLimitModsSetup(const std::filesystem::path& exe_directory,
                         const std::filesystem::path& game_data_root) {
  ModsState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.directory = exe_directory / "mods";
  state.enabled = REXCVAR_GET(mods_enabled);
  state.archive_path = game_data_root / "LONG2DATA" / kArchiveName;
  Archive archive;
  const bool have_archive = archive.Open(state.archive_path, state.archive_error);
  if (have_archive) {
    state.archive_paths = archive.paths;
  } else {
    REXLOG_WARN("Mods: {} ({})", state.archive_error, PathUtf8(state.archive_path));
  }
  ScanMods(state, have_archive ? &archive : nullptr);
  const std::vector<std::string> enabled = ReadEnabledList(state.directory);
  for (Mod& mod : state.mods) {
    mod.saved = std::find(enabled.begin(), enabled.end(), mod.folder) != enabled.end();
    mod.staged = mod.saved;
    mod.loaded = mod.saved && state.enabled && have_archive && !mod.files.empty();
  }
  REXLOG_INFO("Mods: {} in {} ({} on){}", state.mods.size(), PathUtf8(state.directory),
              std::count_if(state.mods.begin(), state.mods.end(),
                            [](const Mod& mod) { return mod.saved; }),
              state.enabled ? "" : " - not loaded: mods_enabled is false");
  for (const Mod& mod : state.mods) {
    REXLOG_INFO("Mods:   {} [{}]: {} game file(s), {} other file(s){}", mod.folder,
                mod.saved ? "on" : "off", mod.files.size(), mod.unknown.size(),
                mod.loaded ? " - loaded" : "");
    for (const ModFile& file : mod.files) {
      std::string targets;
      for (uint32_t row : file.rows) {
        targets += (targets.empty() ? "" : ", ") + archive.paths[row];
      }
      REXLOG_INFO("Mods:     {} ({} bytes) -> {}", file.relative, file.size, targets);
    }
  }
  if (!state.enabled || !have_archive) {
    return;
  }
  // The archive is always served through the overlay (with no mods on it's
  // the file as it is), so mods switched on later can be added while the
  // game runs.
  auto overlay = std::make_shared<ModdedArchive>();
  if (!overlay->Open(archive)) {
    REXLOG_ERROR("Mods: can't open {} - starting without mods", PathUtf8(state.archive_path));
    for (Mod& mod : state.mods) {
      mod.loaded = false;
    }
    return;
  }
  state.overlay = overlay;
  state.archive = std::make_unique<Archive>(std::move(archive));
  std::vector<bool> on(state.mods.size());
  for (size_t m = 0; m < state.mods.size(); ++m) {
    on[m] = state.mods[m].loaded;
  }
  MapMods(state, on);
  // The game reads the TOC from the file at boot: nothing to patch in memory.
  state.pending_rows.clear();
  rex::filesystem::SetFileOverlayProvider(
      [overlay, archive_path = state.archive_path](const std::filesystem::path& host_path)
          -> std::shared_ptr<rex::filesystem::FileOverlay> {
        if (Upper(PathUtf8(host_path.filename())) != kArchiveName ||
            !SameFile(host_path, archive_path)) {
          return nullptr;
        }
        return overlay;
      });
}

// For the online version string: "" without mods, else "mods.<hash>" of the
// files in use.
std::string BurstLimitModsOnlineTag() {
  ModsState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.online_tag;
}

// ---------------------------------------------------------------- the panel

namespace {

constexpr uint16_t kPadUp = 0x0001;
constexpr uint16_t kPadDown = 0x0002;
constexpr uint16_t kPadLeft = 0x0004;
constexpr uint16_t kPadRight = 0x0008;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr int16_t kStickThreshold = 16000;
constexpr double kRepeatDelay = 0.4;
constexpr double kRepeatInterval = 0.1;

// The game's colors, sampled from the main menu's list box: a beige rim, a
// body from nearly black at the top to purple at the bottom, a purple
// highlight bar lit at the left.
constexpr ImU32 kTextWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kTextDim = IM_COL32(150, 136, 172, 255);
constexpr ImU32 kRim = IM_COL32(194, 180, 142, 255);
constexpr ImU32 kBodyTop = IM_COL32(16, 6, 10, 250);
constexpr ImU32 kBodyBottom = IM_COL32(64, 10, 98, 250);
constexpr ImU32 kBarBlack = IM_COL32(0, 0, 0, 255);
// The picture behind the items (black in its texture), faint over the purple.
constexpr ImU32 kWatermark = IM_COL32(255, 255, 255, 70);
constexpr ImU32 kHighlightLeft = IM_COL32(150, 120, 214, 255);
constexpr ImU32 kHighlightMid = IM_COL32(89, 47, 131, 255);
constexpr ImU32 kHighlightRight = IM_COL32(54, 9, 85, 120);
constexpr ImU32 kBandTop = IM_COL32(10, 3, 12, 255);
constexpr ImU32 kBandBottom = IM_COL32(86, 24, 98, 255);
constexpr ImU32 kArrowYellow = IM_COL32(255, 222, 0, 255);

// The Options sub-panels' place on the 1280x720 screen.
constexpr float kPanelLeft = 697.0f;
constexpr float kPanelRight = 1222.0f;
constexpr float kPanelTop = 52.0f;
constexpr float kPanelBottom = 673.0f;

// UICMN_WINDOW.NUT: the A and B buttons, an up arrow, the window pieces
// (rounded caps for the title bar and footer). UIOPT_SET.NUT: the radar.
constexpr int kButtonA = 0;
constexpr int kButtonB = 1;
constexpr int kArrowUp = 8;
constexpr int kWindowPieces = 20;
constexpr size_t kOptionsRadar = 19;

constexpr const char* kRegions[] = {"US", "UK", "FR", "IT", "SP", "DU", "JP"};

enum class View { kMain, kBrowse, kReview };
enum class Filter { kAll, kOn, kOff };

class ModsPanel final : public rex::ui::ImGuiDialog {
 public:
  ModsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
            std::filesystem::path game_data_root,
            std::function<void(std::function<void()>)> defer)
      : ImGuiDialog(drawer),
        immediate_drawer_(immediate_drawer),
        game_data_root_(std::move(game_data_root)),
        defer_(std::move(defer)),
        pad_(std::make_shared<SharedPad>()) {}

  ~ModsPanel() override {
    if (loader_.joinable()) {
      loader_.join();
    }
    if (open_) {
      Close();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!open_) {
      if (!g_open_request.load(std::memory_order_relaxed)) {
        return;
      }
      Open();
    }
    LoadArt();
    RefreshButtonIcons();
    ImGui::SetCurrentContext(io.Ctx);
    const double now = ImGui::GetTime();
    if (open_time_ < 0.0) {
      open_time_ = now;
    }
    // The controllers are read after the paint, for the next frame.
    if (!pad_->pending && defer_) {
      pad_->pending = true;
      defer_([pad = pad_]() {
        pad->state = ReadPad();
        pad->valid = true;
        pad->pending = false;
      });
    }
    HandleInput(now);
    if (!open_) {
      return;
    }
    Draw(io, now);
  }

 private:
  struct PadState {
    uint16_t buttons = 0;
    int16_t thumb_lx = 0, thumb_ly = 0;
  };
  struct SharedPad {
    PadState state;
    bool valid = false;
    bool pending = false;
  };
  struct Glyph {
    ImVec2 uv0, uv1;
    float width = 0.0f, height = 0.0f;
    float left = 0.0f, right = 0.0f;
    float top = 0.0f;
  };
  struct Row {
    std::string label;
    std::string value;
    ImU32 label_color = kTextWhite;
    ImU32 value_color = kTextWhite;
  };

  static rex::input::InputSystem* Input() {
    auto* runtime = rex::Runtime::instance();
    return runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  }

  static PadState ReadPad() {
    PadState pad;
    auto* input = Input();
    if (!input) {
      return pad;
    }
    int distance = 0;
    for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; ++user) {
      rex::input::X_INPUT_STATE state = {};
      if (input->GetStateForUI(user, &state) != rex::X_RESULT(0)) {
        continue;
      }
      pad.buttons |= uint16_t(state.gamepad.buttons);
      const int16_t lx = state.gamepad.thumb_lx, ly = state.gamepad.thumb_ly;
      const int d = std::abs(int(lx)) + std::abs(int(ly));
      if (d > distance) {
        distance = d;
        pad.thumb_lx = lx;
        pad.thumb_ly = ly;
      }
    }
    return pad;
  }

  void Open() {
    {
      std::lock_guard<std::mutex> lock(g_sound_mutex);
      g_sounds.clear();
    }
    open_ = true;
    open_time_ = -1.0;
    view_ = View::kMain;
    selected_ = 0;
    scroll_ = 0;
    message_.clear();
    pad_seen_ = false;
    back_buttons_ = 0;
    // The last read is from the previous time the panel was open: wait for a
    // fresh one, so the A that opened it now isn't taken as a press.
    pad_->valid = false;
    if (auto* input = Input()) {
      input->AddUIInputBlocker();
      blocking_ = true;
    }
  }

  void Close() {
    open_ = false;
    g_open_request.store(false, std::memory_order_relaxed);
    if (blocking_) {
      blocking_ = false;
      if (auto* input = Input()) {
        input->RemoveUIInputBlocker();
      }
    }
    REXLOG_INFO("Mods menu: closed");
  }

  // ---- data (the shared state; changed only here, on the UI thread)

  static size_t PendingChanges(const ModsState& state) {
    return size_t(std::count_if(state.mods.begin(), state.mods.end(),
                                [](const Mod& mod) { return mod.staged != mod.saved; }));
  }

  std::vector<size_t> FilteredMods(const ModsState& state) const {
    std::vector<size_t> out;
    for (size_t i = 0; i < state.mods.size(); ++i) {
      const bool on = state.mods[i].staged;
      if (filter_ == Filter::kAll || (filter_ == Filter::kOn) == on) {
        out.push_back(i);
      }
    }
    return out;
  }

  static const char* FilterName(Filter filter) {
    switch (filter) {
      case Filter::kOn:
        return "On";
      case Filter::kOff:
        return "Off";
      default:
        return "All";
    }
  }

  std::vector<std::string> ReviewLines(const ModsState& state) const {
    std::vector<std::string> lines;
    for (const Mod& mod : state.mods) {
      if (mod.staged != mod.saved) {
        lines.push_back(mod.name + (mod.staged ? ": OFF -> ON" : ": ON -> OFF"));
      }
    }
    if (lines.empty()) {
      lines.push_back("No changes to apply.");
    }
    std::vector<bool> on(state.mods.size());
    for (size_t m = 0; m < state.mods.size(); ++m) {
      on[m] = state.mods[m].staged;
    }
    const std::vector<Conflict> conflicts = Conflicts(state, on);
    if (!conflicts.empty()) {
      lines.push_back("Conflicts (the later mod in the list wins):");
      for (const Conflict& conflict : conflicts) {
        const size_t slash = conflict.file.rfind('/');
        std::string line =
            "  " + (slash == std::string::npos ? conflict.file : conflict.file.substr(slash + 1)) +
            ": " + state.mods[conflict.mods.back()].name + " wins";
        lines.push_back(line);
      }
    }
    return lines;
  }

  void Apply(ModsState& state) {
    if (!PendingChanges(state)) {
      message_ = "Nothing to apply.";
      return;
    }
    // Both players must have the same files: not while a session is open.
    if (rex::net::IsGameSessionOpen()) {
      message_ = "Not during an online session. Apply after leaving it.";
      REXLOG_INFO("Mods menu: apply refused - an online session is open");
      return;
    }
    std::vector<std::string> folders;
    for (const Mod& mod : state.mods) {
      if (mod.staged) {
        folders.push_back(mod.folder);
      }
    }
    if (!WriteEnabledList(state.directory, folders)) {
      message_ = "Couldn't save " + PathUtf8(state.directory / kListFile) + ".";
      REXLOG_ERROR("Mods menu: can't write {}", PathUtf8(state.directory / kListFile));
      return;
    }
    for (Mod& mod : state.mods) {
      if (mod.staged != mod.saved) {
        REXLOG_INFO("Mods menu: applied {} = {}", mod.folder, mod.staged ? "on" : "off");
      }
      mod.saved = mod.staged;
    }
    if (state.overlay) {
      // Live: the archive serves the new files now; the game's copy of the
      // TOC is updated on its own thread next frame (BurstLimitModsMenuSound).
      std::vector<bool> on(state.mods.size());
      for (size_t m = 0; m < state.mods.size(); ++m) {
        on[m] = state.mods[m].staged;
      }
      MapMods(state, on);
      BurstLimitOnlineUpdateVersion(state.online_tag);
      message_ = "Applied. Files load with the change from their next load (the next "
                 "fight); what's loaded now stays until then.";
    } else {
      message_ = "Saved. The changes load the next time the game starts (mods are off: "
                 "mods_enabled = false).";
    }
  }

  void Discard(ModsState& state) {
    const size_t pending = PendingChanges(state);
    for (Mod& mod : state.mods) {
      mod.staged = mod.saved;
    }
    message_ = pending ? "Changes discarded." : "Nothing to discard.";
    REXLOG_INFO("Mods menu: discarded {} change(s)", pending);
  }

  // ---- input

  void HandleInput(double now) {
    int move = 0, change = 0;
    bool activate = false, back = false;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) change = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) change = 1;
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        !rex::cvar::Query<bool>("mnk_mode")) {
      activate = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) back = true;

    if (pad_->valid) {
      const PadState& pad = pad_->state;
      uint16_t buttons = pad.buttons;
      if (pad.thumb_ly > kStickThreshold) buttons |= kPadUp;
      else if (pad.thumb_ly < -kStickThreshold) buttons |= kPadDown;
      if (pad.thumb_lx > kStickThreshold) buttons |= kPadRight;
      else if (pad.thumb_lx < -kStickThreshold) buttons |= kPadLeft;
      int direction = 0;
      if (buttons & kPadUp) direction = 1;
      else if (buttons & kPadDown) direction = 2;
      else if (buttons & kPadLeft) direction = 3;
      else if (buttons & kPadRight) direction = 4;
      if (!pad_seen_) {
        // What's held as the panel opens (the A that opened it) isn't a press.
        pad_seen_ = true;
        last_buttons_ = buttons;
        repeat_direction_ = direction;
        repeat_time_ = DBL_MAX;
      }
      const uint16_t pressed = buttons & ~last_buttons_;
      last_buttons_ = buttons;
      if (pressed & kPadA) activate = true;
      if (pressed & kPadB) back_buttons_ |= kPadB;
      // Closed once B is let go, so the game doesn't get it as a press.
      if (back_buttons_ && !(buttons & back_buttons_)) {
        back_buttons_ = 0;
        back = true;
      }
      bool fire = false;
      if (direction != repeat_direction_) {
        repeat_direction_ = direction;
        repeat_time_ = now + kRepeatDelay;
        fire = direction != 0;
      } else if (direction && now >= repeat_time_) {
        repeat_time_ = now + kRepeatInterval;
        fire = true;
      }
      if (fire) {
        if (direction == 1) move = -1;
        if (direction == 2) move = 1;
        if (direction == 3) change = -1;
        if (direction == 4) change = 1;
      }
    }

    ModsState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const size_t count = RowCount(state);
    if (move && count > 1 && view_ != View::kReview) {
      selected_ = size_t((int(selected_) + move + int(count)) % int(count));
      PlayMenuSound(kSoundMove);
    }
    if (!move && !change && !activate && !back) {
      return;
    }
    switch (view_) {
      case View::kMain:
        if (back) {
          PlayMenuSound(kSoundCancel);
          Close();
          return;
        }
        if (change && selected_ == 1) {
          PlayMenuSound(kSoundMove);
          CycleFilter(change);
        }
        if (activate) {
          ActivateMain(state);
        }
        break;
      case View::kBrowse: {
        if (back) {
          PlayMenuSound(kSoundCancel);
          view_ = View::kMain;
          selected_ = 0;
          return;
        }
        const std::vector<size_t> mods = FilteredMods(state);
        if ((activate || change) && selected_ >= mods.size()) {
          if (activate) {
            PlayMenuSound(kSoundError);
          }
        } else if (activate || change) {
          Mod& mod = state.mods[mods[selected_]];
          const bool on = change ? change > 0 : !mod.staged;
          if (on == mod.staged) {
            break;
          }
          PlayMenuSound(activate ? kSoundConfirm : kSoundMove);
          mod.staged = on;
          REXLOG_INFO("Mods menu: {} switched {} (staged)", mod.folder,
                      mod.staged ? "on" : "off");
          const std::vector<size_t> after = FilteredMods(state);
          if (selected_ >= after.size()) {
            selected_ = after.empty() ? 0 : after.size() - 1;
          }
        }
        break;
      }
      case View::kReview:
        if (back || activate) {
          PlayMenuSound(back ? kSoundCancel : kSoundConfirm);
          view_ = View::kMain;
          selected_ = 2;
        }
        break;
    }
  }

  size_t RowCount(const ModsState& state) const {
    switch (view_) {
      case View::kMain:
        return 6;
      case View::kBrowse:
        return FilteredMods(state).size();
      case View::kReview:
        return 1;
    }
    return 0;
  }

  void CycleFilter(int direction) {
    filter_ = Filter((int(filter_) + 3 + (direction < 0 ? -1 : 1)) % 3);
    REXLOG_INFO("Mods menu: filter {}", FilterName(filter_));
  }

  void ActivateMain(ModsState& state) {
    message_.clear();
    const size_t pending = PendingChanges(state);
    // Apply / Discard with nothing to do buzz, like the game's disabled items.
    if (((selected_ == 3 || selected_ == 4) && !pending) ||
        (selected_ == 3 && rex::net::IsGameSessionOpen())) {
      PlayMenuSound(kSoundError);
    } else {
      PlayMenuSound(selected_ == 5 ? kSoundCancel : kSoundConfirm);
    }
    switch (selected_) {
      case 0:
        view_ = View::kBrowse;
        selected_ = 0;
        scroll_ = 0;
        REXLOG_INFO("Mods menu: browse");
        break;
      case 1:
        CycleFilter(1);
        break;
      case 2:
        view_ = View::kReview;
        selected_ = 0;
        REXLOG_INFO("Mods menu: review ({} change(s))", PendingChanges(state));
        break;
      case 3:
        Apply(state);
        break;
      case 4:
        Discard(state);
        break;
      case 5:
        Close();
        break;
    }
  }

  // ---- art from the game's files

  // A font of the game: an NFH glyph table beside its NUT atlas. The outlined
  // RPRO_EB_FUCHI40 has 40-pixel cells (left / right = the visible glyph in
  // its cell); FUCHINASHI (the panels' titles) has tight boxes with a bearing
  // (left, negated), the top above the baseline and an advance (right).
  struct GameFont {
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    std::array<Glyph, 256> glyphs{};
    bool cells = true;
    float native = 40.0f;  // pixels of a line in the atlas
    float ascent = 0.0f;   // tight boxes: baseline below the line's top
    float space = 9.0f;
    float overlap = 2.0f;
  };

  struct DecodedFont {
    std::vector<BurstLimitGlyph> glyphs;
    BurstLimitTexture atlas;
  };

  bool ReadFixFile(const char* file, std::vector<uint8_t>& data) const {
    for (const char* region : kRegions) {
      if (BurstLimitReadGameFile(game_data_root_, std::string("PAC/") + region + "/FIX/" + file,
                                 data)) {
        return true;
      }
    }
    return false;
  }

  static bool DecodeFont(const std::vector<uint8_t>& nfh, const std::vector<uint8_t>& nut,
                         DecodedFont& out) {
    std::vector<BurstLimitTexture> atlas;
    if (!BurstLimitParseNfh(nfh, out.glyphs) || !BurstLimitDecodeNut(nut, atlas) ||
        atlas.empty()) {
      return false;
    }
    out.atlas = std::move(atlas[0]);
    return true;
  }

  // One texture of a NUT file, decoded.
  static bool DecodeOne(const std::vector<uint8_t>& nut, size_t index, BurstLimitTexture& out) {
    std::vector<BurstLimitNutTexture> textures;
    return BurstLimitListNut(nut, textures) && index < textures.size() &&
           BurstLimitDecodeNutTexture(textures[index], out);
  }

  void DecodeArt() {
    std::vector<uint8_t> nfh, nut;
    if (BurstLimitReadGameFile(game_data_root_, "PAC/CMN/CMN/RPRO_EB_FUCHI40_ALPHA.NFH", nfh) &&
        BurstLimitReadGameFile(game_data_root_, "PAC/CMN/CMN/RPRO_EB_FUCHI40_ALPHA.NUT", nut)) {
      DecodeFont(nfh, nut, decoded_menu_font_);
    }
    if (ReadFixFile("FUCHINASHI.NFH", nfh) && ReadFixFile("FUCHINASHI.NUT", nut)) {
      DecodeFont(nfh, nut, decoded_title_font_);
    }
    // UICMN_WINDOW.NUT: buttons, the arrow and the window pieces.
    if (ReadFixFile("UICMN_WINDOW.NUT", nut)) {
      for (int index : {kButtonA, kButtonB, kArrowUp, kWindowPieces}) {
        BurstLimitTexture image;
        if (DecodeOne(nut, size_t(index), image)) {
          decoded_icons_[index] = std::move(image);
        }
      }
    }
    // UIOPT_SET.NUT: the picture behind the Options panels' items (the dragon
    // radar of Data Management).
    if (ReadFixFile("UIOPT_SET.NUT", nut)) {
      DecodeOne(nut, kOptionsRadar, decoded_watermark_);
    }
  }

  std::unique_ptr<rex::ui::ImmediateTexture> MakeTexture(const BurstLimitTexture& image) const {
    if (image.rgba.empty()) {
      return nullptr;
    }
    return immediate_drawer_->CreateTexture(image.width, image.height,
                                            rex::ui::ImmediateTextureFilter::kLinear, false,
                                            image.rgba.data());
  }

  void MakeFont(DecodedFont& decoded, GameFont& font) {
    font.texture = MakeTexture(decoded.atlas);
    if (!font.texture) {
      return;
    }
    for (const BurstLimitGlyph& glyph : decoded.glyphs) {
      if (glyph.code >= font.glyphs.size()) {
        continue;
      }
      Glyph& g = font.glyphs[glyph.code];
      g.uv0 = ImVec2(float(glyph.x) / float(font.texture->width),
                     float(glyph.y) / float(font.texture->height));
      g.uv1 = ImVec2(float(glyph.x + glyph.width) / float(font.texture->width),
                     float(glyph.y + glyph.height) / float(font.texture->height));
      g.width = glyph.width;
      g.height = glyph.height;
      g.left = glyph.left;
      g.right = glyph.right;
      g.top = glyph.top;
    }
    decoded = {};
  }

  void LoadArt() {
    if (!immediate_drawer_ || art_uploaded_) {
      return;
    }
    if (!loader_.joinable()) {
      loader_ = std::thread([this] {
        DecodeArt();
        art_decoded_.store(true, std::memory_order_release);
      });
    }
    if (!art_decoded_.load(std::memory_order_acquire)) {
      return;
    }
    loader_.join();
    art_uploaded_ = true;
    MakeFont(decoded_menu_font_, menu_font_);
    menu_font_.cells = true;
    menu_font_.native = 40.0f;
    menu_font_.space = 10.0f;
    menu_font_.overlap = 2.0f;
    MakeFont(decoded_title_font_, title_font_);
    title_font_.cells = false;
    title_font_.native = 36.0f;
    title_font_.ascent = 29.0f;
    title_font_.space = 8.0f;
    title_font_.overlap = 0.0f;
    for (auto& [index, image] : decoded_icons_) {
      icons_[index] = MakeTexture(image);
      if (index == kButtonA || index == kButtonB) {
        game_buttons_[index] = image;  // the game's own, to redraw for other controllers
      }
    }
    buttons_style_ = -1;
    watermark_ = MakeTexture(decoded_watermark_);
    decoded_icons_.clear();
    decoded_watermark_ = {};
    if (!menu_font_.texture || !title_font_.texture || !icons_[kWindowPieces]) {
      REXLOG_WARN("Mods menu: some of the game's UI art couldn't be read - using stand-ins");
    }
  }

  // The A / B buttons of the prompts in the button_icons style (UI thread).
  void RefreshButtonIcons() {
    const int style = BurstLimitButtonStyle();
    if (!art_uploaded_ || style == buttons_style_) {
      return;
    }
    buttons_style_ = style;
    for (const auto& [index, original] : game_buttons_) {
      BurstLimitTexture image = original;
      BurstLimitRestyleWindowIcon(index, style, image);
      if (auto texture = MakeTexture(image)) {
        icons_[index] = std::move(texture);
      }
    }
  }

  // ---- drawing

  static bool Covers(const GameFont& font, std::string_view text) {
    if (!font.texture) {
      return false;
    }
    for (char c : text) {
      const uint8_t code = uint8_t(c);
      if (code != ' ' && font.glyphs[code].width == 0.0f) {
        return false;
      }
    }
    return true;
  }

  // Text in one of the game's fonts, `size` pixels a line, its line's top at
  // `position`, `xscale` narrowing it (the condensed titles). Returns the
  // width; draws only with a draw list. The overlay font stands in for text
  // the game's font lacks.
  float Text(ImDrawList* draw_list, const GameFont& font, ImVec2 position, float size,
             std::string_view text, ImU32 color, float xscale = 1.0f) const {
    if (!Covers(font, text)) {
      const float fallback = size * 0.62f;
      const ImVec2 measured = rex::ui::overlay_text::Measure(fallback, text);
      if (draw_list) {
        const ImVec2 at(position.x, position.y + (size - measured.y) * 0.5f);
        rex::ui::overlay_text::Draw(draw_list, fallback, ImVec2(at.x + 2, at.y + 2),
                                    IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xFF), text);
        rex::ui::overlay_text::Draw(draw_list, fallback, at, color, text);
      }
      return measured.x;
    }
    const float s = size / font.native;
    const float sx = s * xscale;
    float pen = 0.0f;
    for (char c : text) {
      const uint8_t code = uint8_t(c);
      if (code == ' ') {
        pen += font.space * sx;
        continue;
      }
      const Glyph& g = font.glyphs[code];
      if (font.cells) {
        if (draw_list) {
          const float x = position.x + pen - g.left * sx;
          draw_list->AddImage(reinterpret_cast<ImTextureID>(font.texture.get()),
                              ImVec2(x, position.y),
                              ImVec2(x + g.width * sx, position.y + g.height * s), g.uv0, g.uv1,
                              color);
        }
        pen += (g.right - g.left - font.overlap) * sx;
      } else {
        if (draw_list) {
          const float x = position.x + pen - g.left * sx;
          const float y = position.y + (font.ascent - g.top) * s;
          draw_list->AddImage(reinterpret_cast<ImTextureID>(font.texture.get()), ImVec2(x, y),
                              ImVec2(x + g.width * sx, y + g.height * s), g.uv0, g.uv1, color);
        }
        pen += g.right * sx;
      }
    }
    return pen + (font.cells ? font.overlap * sx : 0.0f);
  }

  // Text cut with "..." to fit `width`.
  std::string Fit(const GameFont& font, float size, std::string text, float width) const {
    if (Text(nullptr, font, ImVec2(), size, text, 0) <= width) {
      return text;
    }
    while (!text.empty() && Text(nullptr, font, ImVec2(), size, text + "...", 0) > width) {
      text.pop_back();
    }
    return text + "...";
  }

  // Words wrapped to `width`.
  std::vector<std::string> Wrap(const GameFont& font, float size, const std::string& text,
                                float width) const {
    std::vector<std::string> lines;
    std::string line;
    size_t pos = 0;
    while (pos < text.size()) {
      if (text[pos] == '\n') {
        lines.push_back(line);
        line.clear();
        ++pos;
        continue;
      }
      size_t end = text.find_first_of(" \n", pos);
      if (end == std::string::npos) end = text.size();
      const std::string word = text.substr(pos, end - pos);
      const std::string candidate = line.empty() ? word : line + " " + word;
      if (!line.empty() && Text(nullptr, font, ImVec2(), size, candidate, 0) > width) {
        lines.push_back(line);
        line = word;
      } else {
        line = candidate;
      }
      pos = end < text.size() && text[end] == ' ' ? end + 1 : end;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
  }

  rex::ui::ImmediateTexture* Texture(int index) const {
    auto it = icons_.find(index);
    return it != icons_.end() ? it->second.get() : nullptr;
  }

  // An icon, `angle` quarter turns clockwise (the arrow points up).
  void Icon(ImDrawList* draw_list, int index, ImVec2 min, float size, ImU32 tint,
            int quarter_turns = 0) const {
    rex::ui::ImmediateTexture* texture = Texture(index);
    if (!texture) {
      return;
    }
    const ImVec2 corners[4] = {min, ImVec2(min.x + size, min.y),
                               ImVec2(min.x + size, min.y + size), ImVec2(min.x, min.y + size)};
    const ImVec2 uvs[4] = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1)};
    const int r = ((quarter_turns % 4) + 4) % 4;
    draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(texture), corners[0], corners[1],
                            corners[2], corners[3], uvs[(4 - r) % 4], uvs[(5 - r) % 4],
                            uvs[(6 - r) % 4], uvs[(7 - r) % 4], tint);
  }

  // A 9-slice of the window texture's round pieces (`src0`-`src1`, `corner`
  // texture pixels kept at the corners), into `min`-`max`, `scale` screen
  // pixels per texture pixel, colored from `top` at min.y to `bottom` at
  // max.y (the main menu box's vertical gradient).
  void NineSlice(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ImVec2 src0, ImVec2 src1,
                 float corner, float scale, ImU32 top, ImU32 bottom) const {
    rex::ui::ImmediateTexture* texture = Texture(kWindowPieces);
    if (!texture || max.x <= min.x || max.y <= min.y) {
      return;
    }
    const float w = float(texture->width), h = float(texture->height);
    const float cs = std::min({corner * scale, (max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f});
    const float xs[4] = {min.x, min.x + cs, max.x - cs, max.x};
    const float ys[4] = {min.y, min.y + cs, max.y - cs, max.y};
    const float us[4] = {src0.x / w, (src0.x + corner) / w, (src1.x - corner) / w, src1.x / w};
    const float vs[4] = {src0.y / h, (src0.y + corner) / h, (src1.y - corner) / h, src1.y / h};
    const ImVec4 t = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
    auto color = [&](float y) {
      const float f = std::clamp((y - min.y) / (max.y - min.y), 0.0f, 1.0f);
      return ImGui::ColorConvertFloat4ToU32(ImVec4(t.x + (b.x - t.x) * f, t.y + (b.y - t.y) * f,
                                                   t.z + (b.z - t.z) * f, t.w + (b.w - t.w) * f));
    };
    draw_list->PushTextureID(reinterpret_cast<ImTextureID>(texture));
    for (int row = 0; row < 3; ++row) {
      if (ys[row + 1] <= ys[row]) continue;
      const ImU32 c0 = color(ys[row]), c1 = color(ys[row + 1]);
      for (int col = 0; col < 3; ++col) {
        if (xs[col + 1] <= xs[col]) continue;
        draw_list->PrimReserve(6, 4);
        const ImDrawIdx base = ImDrawIdx(draw_list->_VtxCurrentIdx);
        draw_list->PrimWriteIdx(base);
        draw_list->PrimWriteIdx(ImDrawIdx(base + 1));
        draw_list->PrimWriteIdx(ImDrawIdx(base + 2));
        draw_list->PrimWriteIdx(base);
        draw_list->PrimWriteIdx(ImDrawIdx(base + 2));
        draw_list->PrimWriteIdx(ImDrawIdx(base + 3));
        draw_list->PrimWriteVtx(ImVec2(xs[col], ys[row]), ImVec2(us[col], vs[row]), c0);
        draw_list->PrimWriteVtx(ImVec2(xs[col + 1], ys[row]), ImVec2(us[col + 1], vs[row]), c0);
        draw_list->PrimWriteVtx(ImVec2(xs[col + 1], ys[row + 1]), ImVec2(us[col + 1], vs[row + 1]),
                                c1);
        draw_list->PrimWriteVtx(ImVec2(xs[col], ys[row + 1]), ImVec2(us[col], vs[row + 1]), c1);
      }
    }
    draw_list->PopTextureID();
  }

  void Draw(ImGuiIO& io, double now) {
    ModsState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    // The game's 16:9 picture, letterboxed in the window; 720p units.
    const float width = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f);
    const float height = width * 9.0f / 16.0f;
    const ImVec2 origin((io.DisplaySize.x - width) * 0.5f, (io.DisplaySize.y - height) * 0.5f);
    const float u = height / 720.0f;
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * u, origin.y + y * u); };
    const float fade = float(std::clamp((now - open_time_) / 0.12, 0.0, 1.0));
    auto alpha = [fade](ImU32 color) {
      const uint32_t a = (color >> IM_COL32_A_SHIFT) & 0xFF;
      return (color & ~IM_COL32_A_MASK) | (uint32_t(float(a) * fade) << IM_COL32_A_SHIFT);
    };
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const size_t pending = PendingChanges(state);

    // What the current view shows.
    std::string title;
    std::vector<Row> rows;
    std::string help;
    switch (view_) {
      case View::kMain: {
        title = "Mods (" + std::to_string(state.mods.size()) + ")";
        const ImU32 pending_color = pending ? kTextWhite : kTextDim;
        rows = {{"Browse mods", "", kTextWhite, kTextWhite},
                {"Filter", FilterName(filter_), kTextWhite, kTextWhite},
                {"Review changes (" + std::to_string(pending) + ")", "", pending_color,
                 kTextWhite},
                {"Apply changes (" + std::to_string(pending) + ")", "", pending_color, kTextWhite},
                {"Discard changes (" + std::to_string(pending) + ")", "", pending_color,
                 kTextWhite},
                {"Back", "", kTextWhite, kTextWhite}};
        static const char* const kHelp[6] = {
            "Switch packs on or off.",
            "Show all mods, or only the ones that are on or off.",
            "See what changes when you apply.",
            "Use the switches now: files load with them from their next load (the next fight).",
            "Undo the switches you haven't applied.",
            "Return to the main menu."};
        help = selected_ < 6 ? kHelp[selected_] : "";
        if (selected_ == 0) {
          help += " Switches are staged until you Apply.";
        }
        if (state.mods.empty() && selected_ == 0) {
          help = "No mods yet: put each mod in its own folder in the mods folder next to "
                 "burstlimit.exe.";
        }
        if (!state.enabled) {
          help += " Mods are turned off (mods_enabled = false).";
        }
        break;
      }
      case View::kBrowse: {
        const std::vector<size_t> mods = FilteredMods(state);
        title = std::string("Browse mods");
        for (size_t index : mods) {
          const Mod& mod = state.mods[index];
          Row row;
          row.label = mod.name;
          row.value = mod.staged ? "ON" : "OFF";
          row.value_color = mod.staged ? kTextWhite : kTextDim;
          if (mod.staged != mod.saved) {
            row.value = "* " + row.value;
          }
          if (mod.files.empty()) {
            row.label_color = kTextDim;
          }
          rows.push_back(std::move(row));
        }
        if (mods.empty()) {
          help = state.mods.empty() ? "No mods found." : "No mods match the filter.";
        } else if (selected_ < mods.size()) {
          const Mod& mod = state.mods[mods[selected_]];
          std::string details;
          if (!mod.description.empty()) details += mod.description + " ";
          if (!mod.author.empty()) details += "By " + mod.author + ". ";
          details += std::to_string(mod.files.size()) + " game file(s)";
          if (std::any_of(mod.files.begin(), mod.files.end(),
                          [](const ModFile& file) { return file.ps3; })) {
            details += ", made for PS3 (converted)";
          }
          details += mod.loaded ? ". In use now." : ".";
          if (mod.files.empty()) {
            details += " None of its files is a game file.";
          }
          help = details;
        }
        break;
      }
      case View::kReview: {
        title = "Review changes";
        for (const std::string& line : ReviewLines(state)) {
          rows.push_back({line, "", kTextWhite, kTextWhite});
        }
        help = "Apply uses these switches: files load with them from their next load.";
        break;
      }
    }
    if (!message_.empty() && view_ == View::kMain) {
      help = message_;
    }

    // The description, in the main menu's band at the bottom (over the
    // game's own line for Mods), like the game's descriptions.
    const float band_top = 526.0f, band_bottom = 677.0f;
    draw_list->AddRectFilledMultiColor(at(0, band_top), at(kPanelLeft - 4, band_bottom),
                                       alpha(kBandTop), alpha(kBandTop), alpha(kBandBottom),
                                       alpha(kBandBottom));
    const float help_size = 33.0f;
    const std::vector<std::string> lines =
        Wrap(menu_font_, help_size * u, help, (kPanelLeft - 130.0f) * u);
    for (size_t i = 0; i < lines.size() && i < 3; ++i) {
      Text(draw_list, menu_font_, at(104, 552 + float(i) * 34.0f), help_size * u, lines[i],
           alpha(kTextWhite));
    }

    // The panel, in the main menu box's style: its purple gradient body and
    // beige rounded rim (UICMN_WINDOW's round pieces: the disc for the body,
    // the ring for the rim, tinted like the game tints them), with a black
    // title bar and footer like the Options sub-panels.
    const float x0 = kPanelLeft, x1 = kPanelRight, y0 = kPanelTop, y1 = kPanelBottom;
    const float title_bottom = 108.0f, footer_top = 632.0f;
    const float rim = 3.0f;
    const float round_scale = 0.62f * u;  // the pieces' 22-pixel quarter circles -> 14 px
    const bool pieces = Texture(kWindowPieces) != nullptr;
    if (pieces) {
      // Disc (filled circle) at 45..87 x 1..43; ring (outline) at 1..43 x 1..43.
      NineSlice(draw_list, at(x0, y0), at(x1, y1), ImVec2(45, 1), ImVec2(88, 44), 21.5f,
                round_scale, alpha(kBodyTop), alpha(kBodyBottom));
    } else {
      draw_list->AddRectFilledMultiColor(at(x0, y0), at(x1, y1), alpha(kBodyTop),
                                         alpha(kBodyTop), alpha(kBodyBottom), alpha(kBodyBottom));
    }
    if (watermark_) {
      draw_list->PushClipRect(at(x0 + rim, title_bottom), at(x1 - rim, footer_top), true);
      const float wm_w = float(watermark_->width), wm_h = float(watermark_->height);
      draw_list->AddImage(reinterpret_cast<ImTextureID>(watermark_.get()),
                          at(x1 - wm_w + 1.0f, 145.0f), at(x1 + 1.0f, 145.0f + wm_h),
                          ImVec2(0, 0), ImVec2(1, 1), alpha(kWatermark));
      draw_list->PopClipRect();
    }

    // Rows: first at 134, 60 apart, the highlight 38 tall.
    const float first_row = 134.0f, pitch = 60.0f, bar_height = 38.0f;
    const float item_size = 34.0f;  // "Select Storage" is 226 px wide in the game
    const size_t visible = view_ == View::kReview ? 12 : 8;
    const float review_pitch = 40.0f;
    if (view_ != View::kReview) {
      if (selected_ < scroll_) scroll_ = selected_;
      if (selected_ >= scroll_ + visible) scroll_ = selected_ + 1 - visible;
    } else {
      scroll_ = 0;
    }
    draw_list->PushClipRect(at(x0 + rim, title_bottom), at(x1 - rim, footer_top), true);
    for (size_t i = 0; i < visible && scroll_ + i < rows.size(); ++i) {
      const size_t index = scroll_ + i;
      const Row& row = rows[index];
      const bool review = view_ == View::kReview;
      const float top = review ? first_row + float(i) * review_pitch : first_row + float(i) * pitch;
      const float size = review ? item_size * 0.8f : item_size;
      if (!review && index == selected_) {
        // The game's highlight: light at the left, fading into the body.
        const float mid = x0 + (x1 - x0) * 0.45f;
        draw_list->AddRectFilledMultiColor(at(x0 + rim, top), at(mid, top + bar_height),
                                           alpha(kHighlightLeft), alpha(kHighlightMid),
                                           alpha(kHighlightMid), alpha(kHighlightLeft));
        draw_list->AddRectFilledMultiColor(at(mid, top), at(x1 - rim, top + bar_height),
                                           alpha(kHighlightMid), alpha(kHighlightRight),
                                           alpha(kHighlightRight), alpha(kHighlightMid));
      }
      const float text_top = top + (bar_height - size) * 0.5f + 1.0f;
      const float value_width =
          row.value.empty() ? 0.0f : Text(nullptr, menu_font_, ImVec2(), size * u, row.value, 0) / u;
      const float room = (x1 - 62.0f) - (x0 + 21.0f) - value_width - 16.0f;
      Text(draw_list, menu_font_, at(x0 + 21.0f, text_top), size * u,
           Fit(menu_font_, size * u, row.label, room * u), alpha(row.label_color));
      if (!row.value.empty()) {
        Text(draw_list, menu_font_, at(x1 - 62.0f - value_width, text_top), size * u, row.value,
             alpha(row.value_color));
      }
    }
    draw_list->PopClipRect();
    if (view_ != View::kReview && rows.size() > visible) {
      if (scroll_ > 0) {
        Icon(draw_list, kArrowUp, at(x1 - 40, title_bottom + 4), 22.0f * u, alpha(kArrowYellow));
      }
      if (scroll_ + visible < rows.size()) {
        Icon(draw_list, kArrowUp, at(x1 - 40, footer_top - 26), 22.0f * u, alpha(kArrowYellow), 2);
      }
    }

    // Black title bar and footer (the disc piece, black), then the rim over
    // everything (the ring piece, beige).
    if (pieces) {
      draw_list->PushClipRect(at(x0, y0), at(x1, title_bottom), true);
      NineSlice(draw_list, at(x0, y0), at(x1, title_bottom + 30.0f), ImVec2(45, 1),
                ImVec2(88, 44), 21.5f, round_scale, alpha(kBarBlack), alpha(kBarBlack));
      draw_list->PopClipRect();
      draw_list->PushClipRect(at(x0, footer_top), at(x1, y1), true);
      NineSlice(draw_list, at(x0, footer_top - 30.0f), at(x1, y1), ImVec2(45, 1),
                ImVec2(88, 44), 21.5f, round_scale, alpha(kBarBlack), alpha(kBarBlack));
      draw_list->PopClipRect();
      NineSlice(draw_list, at(x0 - 1.0f, y0 - 1.0f), at(x1 + 1.0f, y1 + 1.0f), ImVec2(1, 1),
                ImVec2(44, 44), 21.5f, round_scale, alpha(kRim), alpha(kRim));
    } else {
      draw_list->AddRectFilled(at(x0, y0), at(x1, title_bottom), alpha(kBarBlack), 14.0f * u,
                               ImDrawFlags_RoundCornersTop);
      draw_list->AddRectFilled(at(x0, footer_top), at(x1, y1), alpha(kBarBlack), 14.0f * u,
                               ImDrawFlags_RoundCornersBottom);
      draw_list->AddRect(at(x0, y0), at(x1, y1), alpha(kRim), 14.0f * u, 0, 3.0f * u);
    }
    Text(draw_list, title_font_, at(x0 + 21.0f, y0 + 12.0f), 34.0f * u, title, alpha(kTextWhite),
         0.8f);

    // The footer's button prompts, right-aligned like the game's.
    struct Prompt {
      int icon;  // -1 the up/down arrows, -2 left/right
      const char* text;
    };
    std::vector<Prompt> prompts = {{-1, "Select"}};
    if (view_ == View::kMain) {
      prompts.push_back({-2, "Filter"});
    }
    prompts.push_back({kButtonA, view_ == View::kBrowse ? "Switch" : "Confirm"});
    prompts.push_back({kButtonB, "Back"});
    // The game's size, smaller when the prompts wouldn't fit the footer.
    float prompt_size = 33.0f, button = 28.0f, arrow = 22.0f;
    auto prompt_width = [&](const Prompt& prompt) {
      return (prompt.icon < 0 ? 2.0f * arrow + 2.0f : button + 1.0f) +
             Text(nullptr, menu_font_, ImVec2(), prompt_size * u, prompt.text, 0) / u + 8.0f;
    };
    float total = 0.0f;
    for (const Prompt& prompt : prompts) {
      total += prompt_width(prompt);
    }
    const float room = (x1 - x0) - 64.0f;
    if (total > room) {
      const float fit = room / total;
      prompt_size *= fit;
      button *= fit;
      arrow *= fit;
      total = 0.0f;
      for (const Prompt& prompt : prompts) {
        total += prompt_width(prompt);
      }
    }
    float x = x1 - 46.0f - total;
    const float mid_y = (footer_top + y1) * 0.5f;
    for (const Prompt& prompt : prompts) {
      if (prompt.icon == -1) {
        Icon(draw_list, kArrowUp, at(x, mid_y - arrow * 0.5f), arrow * u, alpha(kArrowYellow));
        Icon(draw_list, kArrowUp, at(x + arrow, mid_y - arrow * 0.5f), arrow * u,
             alpha(kArrowYellow), 2);
        x += 2.0f * arrow + 2.0f;
      } else if (prompt.icon == -2) {
        Icon(draw_list, kArrowUp, at(x, mid_y - arrow * 0.5f), arrow * u, alpha(kArrowYellow), 3);
        Icon(draw_list, kArrowUp, at(x + arrow, mid_y - arrow * 0.5f), arrow * u,
             alpha(kArrowYellow), 1);
        x += 2.0f * arrow + 2.0f;
      } else {
        Icon(draw_list, prompt.icon, at(x, mid_y - button * 0.5f), button * u, alpha(kTextWhite));
        x += button + 1.0f;
      }
      x += Text(draw_list, menu_font_, at(x, mid_y - prompt_size * 0.5f), prompt_size * u,
                prompt.text, alpha(kTextWhite)) / u + 8.0f;
    }
  }

  rex::ui::ImmediateDrawer* immediate_drawer_;
  std::filesystem::path game_data_root_;
  std::function<void(std::function<void()>)> defer_;
  std::shared_ptr<SharedPad> pad_;

  bool open_ = false;
  bool blocking_ = false;
  double open_time_ = -1.0;
  View view_ = View::kMain;
  Filter filter_ = Filter::kAll;
  size_t selected_ = 0;
  size_t scroll_ = 0;
  std::string message_;

  bool pad_seen_ = false;
  uint16_t last_buttons_ = 0;
  uint16_t back_buttons_ = 0;
  int repeat_direction_ = 0;
  double repeat_time_ = 0.0;

  std::thread loader_;
  std::atomic<bool> art_decoded_{false};
  bool art_uploaded_ = false;
  DecodedFont decoded_menu_font_, decoded_title_font_;
  std::map<int, BurstLimitTexture> decoded_icons_;
  BurstLimitTexture decoded_watermark_;
  GameFont menu_font_, title_font_;
  std::map<int, std::unique_ptr<rex::ui::ImmediateTexture>> icons_;
  std::unique_ptr<rex::ui::ImmediateTexture> watermark_;
  // UICMN_WINDOW's A and B as the game has them, and the style icons_ show.
  std::map<int, BurstLimitTexture> game_buttons_;
  int buttons_style_ = -1;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> BurstLimitCreateModsPanel(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
    const std::filesystem::path& game_data_root,
    std::function<void(std::function<void()>)> defer) {
  return std::make_unique<ModsPanel>(drawer, immediate_drawer, game_data_root, std::move(defer));
}
