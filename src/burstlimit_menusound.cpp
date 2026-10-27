// The game's own menu sounds (cursor, confirm, cancel, ...) for the recomp's
// screens: the start screen and the settings menu. Read from the player's
// game files (SOUND/CSB/CMN/SYSTEM_SE.CSB in the CPK archive, never shipped)
// on a thread: the CSB's SOUND_ELEMENT table names each sound
// (".../SysID01_Cursor1_aif") and holds it as an AAX table with one ADX
// stream (4-bit ADPCM), decoded here to 16-bit PCM in a WAV in memory and
// played with PlaySound (one at a time, like the game's menu sounds).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "burstlimit_cpk.h"
#include "burstlimit_gamesound.h"

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

// A CRI ADX stream (type 3, 4-bit, unencrypted) to 16-bit PCM, interleaved.
bool AdxDecode(const uint8_t* adx, size_t size, std::vector<int16_t>& pcm, uint32_t& rate_out,
               uint32_t& channels_out) {
  if (size < 32 || adx[0] != 0x80 || adx[1] != 0 || adx[4] != 3 || adx[6] != 4) {
    return false;
  }
  const size_t data = size_t(Be16(adx + 2)) + 4;
  const uint32_t block = adx[5], channels = adx[7];
  const uint32_t rate = Be32(adx + 8), total = Be32(adx + 12);
  const uint32_t highpass = Be16(adx + 16);
  if (!block || block < 3 || !channels || channels > 2 || !rate || data >= size ||
      adx[19] != 0 /* encrypted */ || total > rate * 120u) {
    return false;
  }
  const double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979 * highpass / rate);
  const double b = std::sqrt(2.0) - 1.0;
  const double c = (a - std::sqrt((a + b) * (a - b))) / b;
  const int coef1 = int(c * 8192.0), coef2 = int(-c * c * 4096.0);
  pcm.assign(size_t(total) * channels, 0);
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
        pcm[size_t(done + i) * channels + ch] = int16_t(sample);
      }
    }
    done += per;
  }
  rate_out = rate;
  channels_out = channels;
  return true;
}

// The same to a WAV in memory (for PlaySound).
bool AdxToWav(const uint8_t* adx, size_t size, std::vector<uint8_t>& wav) {
  std::vector<int16_t> pcm;
  uint32_t rate = 0, channels = 0;
  if (!AdxDecode(adx, size, pcm, rate, channels)) {
    return false;
  }
  // A bit under the game's level: these play over its music.
  for (int16_t& v : pcm) {
    v = int16_t(v * 3 / 4);
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

// ---------------------------------------------------------------- whole cue sheets
// For the achievement sound (achievement_sound = game): every cue of the
// game's system, menu and battle SE cue sheets, kept as read (the CSB file
// stays in memory, the cues point into it) and decoded when asked for.

using Rows = std::vector<std::vector<std::pair<std::string, UtfValue>>>;

struct CueLeaf {
  const uint8_t* aax = nullptr;
  size_t size = 0;
  uint32_t delay_ms = 0;
  float volume = 1.0f;
};

struct CueInfo {
  uint32_t id = 0;
  std::string name;
  float seconds = 0.0f;
  float volume = 1.0f;
  std::vector<CueLeaf> leaves;
};

struct Bank {
  const char* name;
  std::vector<uint8_t> csb;
  std::vector<CueInfo> cues;
};

std::array<Bank, 3> g_banks = {{{"SYSTEM_SE", {}, {}}, {"MENU_SE", {}, {}}, {"BATTLE_COMMON_SE", {}, {}}}};
std::atomic<bool> g_banks_loaded{false};

std::string Text(const std::vector<std::pair<std::string, UtfValue>>& row, std::string_view key) {
  const UtfValue* v = Find(row, key);
  return v ? v->text : std::string();
}
uint64_t Number(const std::vector<std::pair<std::string, UtfValue>>& row, std::string_view key) {
  const UtfValue* v = Find(row, key);
  return v ? v->number : 0;
}

// The ADX stream of a SOUND_ELEMENT's AAX table (its first segment).
bool AaxAdx(const uint8_t* aax, size_t size, const uint8_t*& adx, size_t& adx_size) {
  if (size >= 4 && aax[0] == 0x80) {
    adx = aax;
    adx_size = size;
    return true;
  }
  Rows rows;
  if (!ReadUtf(aax, size, rows) || rows.empty()) return false;
  const UtfValue* d = Find(rows[0], "data");
  if (!d || !d->data) return false;
  adx = d->data;
  adx_size = d->size;
  return true;
}

void ParseBank(Bank& bank) {
  Rows tables, cue_rows, synth_rows, element_rows;
  if (!ReadUtf(bank.csb.data(), bank.csb.size(), tables)) return;
  for (const auto& row : tables) {
    const UtfValue* table = Find(row, "utf");
    if (!table || !table->data) continue;
    const std::string name = Text(row, "name");
    if (name == "CUE") ReadUtf(table->data, table->size, cue_rows);
    if (name == "SYNTH") ReadUtf(table->data, table->size, synth_rows);
    if (name == "SOUND_ELEMENT") ReadUtf(table->data, table->size, element_rows);
  }
  std::unordered_map<std::string, size_t> synths, elements;
  for (size_t i = 0; i < synth_rows.size(); ++i) synths[Text(synth_rows[i], "synname")] = i;
  for (size_t i = 0; i < element_rows.size(); ++i) elements[Text(element_rows[i], "name")] = i;
  // A synth to its waveform leaves (track synths list their children, one a line).
  std::function<void(const std::string&, int, std::vector<std::pair<size_t, size_t>>&)> collect =
      [&](const std::string& name, int depth, std::vector<std::pair<size_t, size_t>>& out) {
        auto it = synths.find(name);
        if (it == synths.end() || depth > 8) return;
        const auto& row = synth_rows[it->second];
        const std::string link = Text(row, "lnkname");
        if (Number(row, "syntype") == 0) {
          auto e = elements.find(link);
          if (e != elements.end() && link.find("silent") == std::string::npos) {
            out.emplace_back(it->second, e->second);
          }
          return;
        }
        size_t start = 0;
        while (start < link.size()) {
          size_t end = link.find('\n', start);
          if (end == std::string::npos) end = link.size();
          if (end > start) collect(link.substr(start, end - start), depth + 1, out);
          start = end + 1;
        }
      };
  for (const auto& cue : cue_rows) {
    const std::string synth = Text(cue, "synth");
    auto top = synths.find(synth);
    if (top == synths.end()) continue;
    std::vector<std::pair<size_t, size_t>> leaves;
    collect(synth, 0, leaves);
    if (leaves.empty()) continue;
    const auto& top_row = synth_rows[top->second];
    if (Number(top_row, "cmplxtype") == 1) leaves.resize(1);  // random: the first variant
    CueInfo info;
    info.id = uint32_t(Number(cue, "id"));
    info.volume = float(Number(top_row, "volume")) / 1000.0f;
    std::string label = Text(element_rows[leaves[0].second], "name");
    label = label.substr(label.rfind('/') + 1);
    for (const char* suffix : {"_aif", "_wav"}) {
      if (label.size() > 4 && label.compare(label.size() - 4, 4, suffix) == 0) label.resize(label.size() - 4);
    }
    info.name = label;
    for (const auto& [s, e] : leaves) {
      const UtfValue* d = Find(element_rows[e], "data");
      const uint8_t* adx = nullptr;
      size_t adx_size = 0;
      if (!d || !d->data || !AaxAdx(d->data, d->size, adx, adx_size) || adx_size < 16) continue;
      CueLeaf leaf;
      leaf.aax = d->data;
      leaf.size = d->size;
      leaf.delay_ms = uint32_t(Number(synth_rows[s], "dlytim"));
      leaf.volume = float(Number(synth_rows[s], "volume")) / 1000.0f;
      const uint32_t rate = Be32(adx + 8), total = Be32(adx + 12);
      if (rate) info.seconds = std::max(info.seconds, leaf.delay_ms / 1000.0f + float(total) / rate);
      info.leaves.push_back(leaf);
    }
    if (!info.leaves.empty()) bank.cues.push_back(std::move(info));
  }
  std::sort(bank.cues.begin(), bank.cues.end(),
            [](const CueInfo& a, const CueInfo& b) { return a.id < b.id; });
}

void LoadBanks(const std::filesystem::path& game_data_root) {
  size_t cues = 0;
  for (Bank& bank : g_banks) {
    const std::string file = std::string("SOUND/CSB/CMN/") + bank.name + ".CSB";
    if (BurstLimitReadGameFile(game_data_root, file, bank.csb)) {
      ParseBank(bank);
      cues += bank.cues.size();
    }
  }
  g_banks_loaded.store(true);
  REXLOG_INFO("Game sounds: {} cues read from the game's SE cue sheets", cues);
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
  LoadBanks(game_data_root);
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

// ---------------------------------------------------------------- achievement_sound = game

std::vector<BurstLimitGameCue> BurstLimitListGameCues() {
  std::vector<BurstLimitGameCue> out;
  if (!g_banks_loaded.load()) return out;
  for (const Bank& bank : g_banks) {
    for (const CueInfo& cue : bank.cues) {
      out.push_back({bank.name, cue.id, cue.name, cue.seconds});
    }
  }
  return out;
}

bool BurstLimitDecodeGameCue(std::string_view key, std::vector<float>& mono, uint32_t& rate,
                             std::string& what) {
  const size_t colon = key.find(':');
  if (colon == std::string_view::npos) {
    what = "achievement_game_sound should be BANK:cue, like SYSTEM_SE:17";
    return false;
  }
  if (!g_banks_loaded.load()) {
    what = "the game's sounds aren't read yet";
    return false;
  }
  const std::string_view bank_name = key.substr(0, colon);
  const uint32_t id = uint32_t(std::strtoul(std::string(key.substr(colon + 1)).c_str(), nullptr, 10));
  for (const Bank& bank : g_banks) {
    if (bank_name != bank.name) continue;
    for (const CueInfo& cue : bank.cues) {
      if (cue.id != id) continue;
      mono.clear();
      rate = 0;
      for (const CueLeaf& leaf : cue.leaves) {
        const uint8_t* adx = nullptr;
        size_t adx_size = 0;
        std::vector<int16_t> pcm;
        uint32_t r = 0, ch = 0;
        if (!AaxAdx(leaf.aax, leaf.size, adx, adx_size) || !AdxDecode(adx, adx_size, pcm, r, ch)) continue;
        if (!rate) rate = r;
        if (r != rate) continue;
        const size_t at = size_t(uint64_t(leaf.delay_ms) * rate / 1000), frames = pcm.size() / ch;
        if (mono.size() < at + frames) mono.resize(at + frames, 0.0f);
        for (size_t i = 0; i < frames; ++i) {
          float v = 0.0f;
          for (uint32_t c = 0; c < ch; ++c) v += pcm[i * ch + c];
          mono[at + i] += v / float(ch) / 32768.0f * leaf.volume * cue.volume;
        }
      }
      if (mono.empty()) {
        what = std::string(key) + ": can't decode it";
        return false;
      }
      float peak = 0.0f;
      for (float v : mono) peak = std::max(peak, std::abs(v));
      if (peak > 1.0f) {
        for (float& v : mono) v /= peak;
      }
      what = std::string(bank.name) + ":" + std::to_string(cue.id) + " " + cue.name;
      return true;
    }
  }
  what = std::string(key) + ": no such cue in SYSTEM_SE, MENU_SE or BATTLE_COMMON_SE";
  return false;
}
