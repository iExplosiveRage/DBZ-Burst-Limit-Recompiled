// The game's own menu sounds (cursor, confirm, cancel, ...) for the recomp's
// screens: the start screen and the settings menu. Read from the player's
// game files (SOUND/CSB/CMN/SYSTEM_SE.CSB in the CPK archive, never shipped)
// on a thread: the CSB's SOUND_ELEMENT table names each sound
// (".../SysID01_Cursor1_aif") and holds it as an AAX table with one ADX
// stream (4-bit ADPCM), decoded here to 16-bit PCM in a WAV in memory and
// played with PlaySound (one at a time, like the game's menu sounds).

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "burstlimit_cpk.h"

#pragma comment(lib, "winmm.lib")

REXCVAR_DEFINE_BOOL(menu_sounds, true, "Patches",
                    "The game's menu sounds in the start screen and the settings menu");

namespace {

// BurstLimitPlayMenuSound's sounds and their SOUND_ELEMENT names.
constexpr std::array<const char*, 7> kSoundNames = {
    "SysID01_Cursor1",       // 0 move
    "SysID03_OKsound1",      // 1 confirm
    "SysID08_CancelSound",   // 2 back
    "SysID09_UnSelectable",  // 3 not possible
    "SysID04_Menu_result",   // 4 start the game
    "SysID10_WindowOpen",    // 5 a menu opens
    "SysID11_WindowClose",   // 6 a menu closes
};

std::mutex g_mutex;
std::array<std::vector<uint8_t>, kSoundNames.size()> g_wavs;  // RIFF WAV files
std::atomic<bool> g_loaded{false};

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p) {
  return uint16_t((p[0] << 8) | p[1]);
}

// One value of a CRI @UTF table row: a number, a string or a data blob.
struct UtfValue {
  uint64_t number = 0;
  std::string text;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// The rows of an @UTF table (unencrypted), as column name -> value.
bool ReadUtf(const uint8_t* table, size_t size,
             std::vector<std::vector<std::pair<std::string, UtfValue>>>& rows) {
  if (size < 32 || std::memcmp(table, "@UTF", 4) != 0) {
    return false;
  }
  const size_t table_size = Be32(table + 4) + 8;
  if (table_size > size) {
    return false;
  }
  const size_t rows_offset = Be32(table + 8) + 8;
  const size_t strings_offset = Be32(table + 12) + 8;
  const size_t data_offset = Be32(table + 16) + 8;
  const uint32_t columns = Be16(table + 24);
  const uint32_t row_length = Be16(table + 26);
  const uint32_t row_count = Be32(table + 28);
  auto text_at = [&](uint32_t offset) {
    std::string out;
    for (size_t i = strings_offset + offset; i < table_size && table[i]; ++i) {
      out += char(table[i]);
    }
    return out;
  };
  struct Column {
    std::string name;
    uint8_t storage, type;
    size_t constant;  // position of a constant's value
  };
  auto type_size = [](uint8_t type) -> size_t {
    switch (type) {
      case 0x0: case 0x1: return 1;
      case 0x2: case 0x3: return 2;
      case 0x4: case 0x5: case 0x8: case 0xA: return 4;
      case 0x6: case 0x7: case 0xB: return 8;
      default: return 0;
    }
  };
  auto read = [&](uint8_t type, size_t at) {
    UtfValue value;
    if (at + type_size(type) > table_size) {
      return value;
    }
    switch (type) {
      case 0x0: case 0x1: value.number = table[at]; break;
      case 0x2: case 0x3: value.number = Be16(table + at); break;
      case 0x4: case 0x5: case 0x8: value.number = Be32(table + at); break;
      case 0x6: case 0x7: value.number = (uint64_t(Be32(table + at)) << 32) | Be32(table + at + 4); break;
      case 0xA: value.text = text_at(Be32(table + at)); break;
      case 0xB: {
        const size_t offset = data_offset + Be32(table + at), length = Be32(table + at + 4);
        if (offset + length <= table_size) {
          value.data = table + offset;
          value.size = length;
        }
        break;
      }
    }
    return value;
  };
  std::vector<Column> cols;
  size_t p = 32;
  for (uint32_t c = 0; c < columns; ++c) {
    if (p + 5 > table_size) return false;
    Column column;
    column.storage = table[p] & 0xF0;
    column.type = table[p] & 0x0F;
    column.name = text_at(Be32(table + p + 1));
    p += 5;
    column.constant = p;
    if (column.storage == 0x30) {
      if (!type_size(column.type)) return false;
      p += type_size(column.type);
    }
    cols.push_back(std::move(column));
  }
  for (uint32_t r = 0; r < row_count; ++r) {
    size_t at = rows_offset + size_t(r) * row_length;
    std::vector<std::pair<std::string, UtfValue>> row;
    for (const Column& column : cols) {
      if (column.storage == 0x50) {
        if (!type_size(column.type)) return false;
        row.emplace_back(column.name, read(column.type, at));
        at += type_size(column.type);
      } else if (column.storage == 0x30) {
        row.emplace_back(column.name, read(column.type, column.constant));
      }
    }
    rows.push_back(std::move(row));
  }
  return true;
}

const UtfValue* Find(const std::vector<std::pair<std::string, UtfValue>>& row,
                     std::string_view name) {
  for (const auto& [key, value] : row) {
    if (key == name) return &value;
  }
  return nullptr;
}

// A CRI ADX stream (type 3, 4-bit, unencrypted) to 16-bit PCM in a WAV.
bool AdxToWav(const uint8_t* adx, size_t size, std::vector<uint8_t>& wav) {
  if (size < 32 || adx[0] != 0x80 || adx[1] != 0 || adx[4] != 3 || adx[6] != 4) {
    return false;
  }
  const size_t data = size_t(Be16(adx + 2)) + 4;
  const uint32_t block = adx[5], channels = adx[7];
  const uint32_t rate = Be32(adx + 8), total = Be32(adx + 12);
  const uint32_t highpass = Be16(adx + 16);
  if (!block || block < 3 || !channels || channels > 2 || !rate || data >= size ||
      adx[19] != 0 /* encrypted */) {
    return false;
  }
  const double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979 * highpass / rate);
  const double b = std::sqrt(2.0) - 1.0;
  const double c = (a - std::sqrt((a + b) * (a - b))) / b;
  const int coef1 = int(c * 8192.0), coef2 = int(-c * c * 4096.0);
  std::vector<int16_t> pcm(size_t(total) * channels);
  int hist[2][2] = {};
  size_t pos = data;
  for (uint32_t done = 0; done < total && pos + block * channels <= size;) {
    const uint32_t per = (block - 2) * 2;
    for (uint32_t ch = 0; ch < channels; ++ch, pos += block) {
      const int scale = Be16(adx + pos);
      int& h1 = hist[ch][0];
      int& h2 = hist[ch][1];
      for (uint32_t i = 0; i < per && done + i < total; ++i) {
        const uint8_t byte = adx[pos + 2 + i / 2];
        int nibble = (i & 1) ? (byte & 15) : (byte >> 4);
        if (nibble >= 8) nibble -= 16;
        int sample = nibble * scale + ((coef1 * h1 + coef2 * h2) >> 12);
        sample = sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample;
        h2 = h1;
        h1 = sample;
        // A bit under the game's level: these play over its music.
        pcm[size_t(done + i) * channels + ch] = int16_t(sample * 3 / 4);
      }
    }
    done += per;
  }
  const uint32_t bytes = uint32_t(pcm.size() * 2);
  wav.resize(44 + bytes);
  auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
  auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
  std::memcpy(&wav[0], "RIFF", 4);
  put32(4, 36 + bytes);
  std::memcpy(&wav[8], "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, uint16_t(channels));
  put32(24, rate);
  put32(28, rate * channels * 2);
  put16(32, uint16_t(channels * 2));
  put16(34, 16);
  std::memcpy(&wav[36], "data", 4);
  put32(40, bytes);
  std::memcpy(&wav[44], pcm.data(), bytes);
  return true;
}

void Load(std::filesystem::path game_data_root) {
  std::vector<uint8_t> csb;
  if (!BurstLimitReadGameFile(game_data_root, "SOUND/CSB/CMN/SYSTEM_SE.CSB", csb)) {
    REXLOG_WARN("Menu sounds: SYSTEM_SE.CSB not found");
    return;
  }
  std::vector<std::vector<std::pair<std::string, UtfValue>>> tables, elements;
  if (!ReadUtf(csb.data(), csb.size(), tables)) {
    REXLOG_WARN("Menu sounds: SYSTEM_SE.CSB isn't a table it can read");
    return;
  }
  for (const auto& row : tables) {
    const UtfValue* name = Find(row, "name");
    const UtfValue* table = Find(row, "utf");
    if (name && table && table->data && name->text == "SOUND_ELEMENT") {
      ReadUtf(table->data, table->size, elements);
    }
  }
  int found = 0;
  for (const auto& row : elements) {
    const UtfValue* name = Find(row, "name");
    const UtfValue* data = Find(row, "data");
    if (!name || !data || !data->data) continue;
    for (size_t i = 0; i < kSoundNames.size(); ++i) {
      if (name->text.find(kSoundNames[i]) == std::string::npos) continue;
      std::vector<std::vector<std::pair<std::string, UtfValue>>> aax;
      std::vector<uint8_t> wav;
      if (ReadUtf(data->data, data->size, aax) && !aax.empty()) {
        const UtfValue* adx = Find(aax[0], "data");
        if (adx && adx->data && AdxToWav(adx->data, adx->size, wav)) {
          std::lock_guard<std::mutex> lock(g_mutex);
          g_wavs[i] = std::move(wav);
          ++found;
        }
      }
    }
  }
  g_loaded.store(true);
  REXLOG_INFO("Menu sounds: {} of {} read from the game", found, kSoundNames.size());
}

}  // namespace

void BurstLimitMenuSoundsSetup(const std::filesystem::path& game_data_root) {
  static std::atomic<bool> started{false};
  if (game_data_root.empty() || started.exchange(true)) {
    return;
  }
  std::thread(Load, game_data_root).detach();
}

// 0 move, 1 confirm, 2 back, 3 not possible, 4 start the game, 5 a menu
// opens, 6 a menu closes. Any thread.
void BurstLimitPlayMenuSound(int sound) {
  if (!REXCVAR_GET(menu_sounds) || !g_loaded.load() || sound < 0 ||
      size_t(sound) >= kSoundNames.size()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::vector<uint8_t>& wav = g_wavs[size_t(sound)];
  if (!wav.empty()) {
    // The buffers are never freed or changed after loading: safe for async.
    const BOOL played = PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr,
                                   SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    REXLOG_DEBUG("Menu sounds: {} {}", kSoundNames[size_t(sound)], played ? "played" : "failed");
  }
}
