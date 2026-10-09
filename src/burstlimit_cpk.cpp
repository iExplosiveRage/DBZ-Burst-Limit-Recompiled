// Reading the game's own files from its CRI CPK archives, and decoding the
// textures of its NUT files. See burstlimit_cpk.h.
//
// CPK: a "CPK " chunk with an @UTF table (TocOffset, ContentOffset), and a
// "TOC " chunk whose @UTF table lists the files (DirName, FileName, FileSize,
// ExtractSize, FileOffset from the lower of the two offsets). @UTF tables are
// big-endian and may be XOR-encrypted. Compressed files use CRILAYLA (here
// with the magic zeroed): sizes, the data read backwards from its end, then
// the first 0x100 bytes of the file uncompressed.
//
// NTXR: "NTXR", u16 version, u16 texture count, padding to 0x10, then per
// texture a header (u32 total size, u32 data size at +8, u16 header size at
// +12, u16 format at +18 - 0 DXT1, 1 DXT3, 2 DXT5, 19 A8R8G8B8 - u16 width,
// u16 height) followed by its data: linear DXT blocks stored as big-endian
// 16-bit words, mips after the first.

#include "burstlimit_cpk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

namespace {

uint16_t Be16(const uint8_t* p) {
  return uint16_t(p[0] << 8 | p[1]);
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t Be64(const uint8_t* p) {
  return uint64_t(Be32(p)) << 32 | Be32(p + 4);
}

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32;
}

size_t TypeSize(uint8_t type) {
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

class UtfTable {
 public:
  bool Parse(std::vector<uint8_t> bytes) {
    if (bytes.size() < 32) {
      return false;
    }
    if (std::memcmp(bytes.data(), "@UTF", 4) != 0) {
      uint32_t key = 0x655F;
      for (uint8_t& b : bytes) {
        b ^= uint8_t(key);
        key *= 0x4115;
      }
      if (std::memcmp(bytes.data(), "@UTF", 4) != 0) {
        return false;
      }
    }
    data_ = std::move(bytes);
    rows_offset_ = Be32(&data_[8]) + 8;
    strings_offset_ = Be32(&data_[12]) + 8;
    const uint16_t column_count = Be16(&data_[24]);
    row_length_ = Be16(&data_[26]);
    row_count_ = Be32(&data_[28]);
    size_t pos = 32;
    uint32_t row_position = 0;
    for (uint16_t i = 0; i < column_count; ++i) {
      if (pos + 5 > data_.size()) {
        return false;
      }
      Column column;
      column.flags = data_[pos];
      if (!StringAt(Be32(&data_[pos + 1]), column.name)) {
        return false;
      }
      pos += 5;
      const size_t size = TypeSize(column.flags & 0x0F);
      if (!size) {
        return false;
      }
      switch (column.flags & 0xF0) {
        case 0x30:  // the same value in every row, stored here
          column.position = uint32_t(pos);
          pos += size;
          break;
        case 0x50:  // a value per row
          column.position = row_position;
          row_position += uint32_t(size);
          break;
        default:  // no value
          column.position = UINT32_MAX;
          break;
      }
      columns_.push_back(std::move(column));
    }
    return uint64_t(rows_offset_) + uint64_t(row_count_) * row_length_ <= data_.size();
  }

  uint32_t rows() const { return row_count_; }

  bool Int(uint32_t row, std::string_view name, uint64_t& value) const {
    size_t pos;
    const Column* column = Find(name);
    if (!column || !Position(row, *column, pos)) {
      return false;
    }
    switch (column->flags & 0x0F) {
      case 0x0:
      case 0x1:
        value = data_[pos];
        return true;
      case 0x2:
      case 0x3:
        value = Be16(&data_[pos]);
        return true;
      case 0x4:
      case 0x5:
        value = Be32(&data_[pos]);
        return true;
      case 0x6:
      case 0x7:
        value = Be64(&data_[pos]);
        return true;
      default:
        return false;
    }
  }

  bool String(uint32_t row, std::string_view name, std::string& value) const {
    size_t pos;
    const Column* column = Find(name);
    return column && (column->flags & 0x0F) == 0xA && Position(row, *column, pos) &&
           StringAt(Be32(&data_[pos]), value);
  }

 private:
  struct Column {
    uint8_t flags = 0;
    std::string name;
    uint32_t position = 0;
  };

  bool StringAt(uint32_t offset, std::string& value) const {
    const size_t start = size_t(strings_offset_) + offset;
    if (start >= data_.size()) {
      return false;
    }
    const auto end = std::find(data_.begin() + start, data_.end(), uint8_t(0));
    value.assign(data_.begin() + start, end);
    return true;
  }

  const Column* Find(std::string_view name) const {
    for (const Column& column : columns_) {
      if (column.name == name) {
        return &column;
      }
    }
    return nullptr;
  }

  bool Position(uint32_t row, const Column& column, size_t& pos) const {
    if (column.position == UINT32_MAX || row >= row_count_) {
      return false;
    }
    pos = (column.flags & 0xF0) == 0x50
              ? size_t(rows_offset_) + size_t(row) * row_length_ + column.position
              : column.position;
    return pos + TypeSize(column.flags & 0x0F) <= data_.size();
  }

  std::vector<uint8_t> data_;
  uint32_t rows_offset_ = 0;
  uint32_t strings_offset_ = 0;
  uint32_t row_count_ = 0;
  uint16_t row_length_ = 0;
  std::vector<Column> columns_;
};

bool ReadChunk(std::ifstream& file, uint64_t offset, const char* magic, UtfTable& table) {
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
  std::vector<uint8_t> bytes(size);
  return file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)) &&
         table.Parse(std::move(bytes));
}

bool Crilayla(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  static const uint8_t kZeroMagic[8] = {};
  if (in.size() < 16 + 0x100 || (std::memcmp(in.data(), "CRILAYLA", 8) != 0 &&
                                 std::memcmp(in.data(), kZeroMagic, 8) != 0)) {
    return false;
  }
  const uint32_t size = Le32(&in[8]);
  const uint32_t compressed = Le32(&in[12]);
  if (uint64_t(16) + compressed + 0x100 != in.size() || size > (256u << 20)) {
    return false;
  }
  out.assign(size_t(size) + 0x100, 0);
  std::memcpy(out.data(), &in[16 + compressed], 0x100);
  const uint8_t* src = &in[16];
  int64_t src_pos = int64_t(compressed) - 1;
  uint32_t pool = 0;
  int pool_bits = 0;
  bool underflow = false;
  auto bits = [&](int count) {
    uint32_t value = 0;
    while (count > 0) {
      if (!pool_bits) {
        if (src_pos < 0) {
          underflow = true;
          return 0u;
        }
        pool = src[src_pos--];
        pool_bits = 8;
      }
      const int take = std::min(pool_bits, count);
      value = (value << take) | ((pool >> (pool_bits - take)) & ((1u << take) - 1));
      pool_bits -= take;
      count -= take;
    }
    return value;
  };
  static const int kLengthBits[4] = {2, 3, 5, 8};
  int64_t write = int64_t(size) + 0x100 - 1;
  while (write >= 0x100 && !underflow) {
    if (bits(1)) {
      int64_t from = write + 3 + bits(13);
      int64_t length = 3;
      for (int level = 0;; ++level) {
        const int count = kLengthBits[std::min(level, 3)];
        const uint32_t part = bits(count);
        length += part;
        if (underflow || part != (1u << count) - 1) {
          break;
        }
      }
      for (; length > 0; --length) {
        if (write < 0 || from >= int64_t(out.size())) {
          return false;
        }
        out[size_t(write--)] = out[size_t(from--)];
      }
    } else {
      out[size_t(write--)] = uint8_t(bits(8));
    }
  }
  return !underflow;
}

bool SameName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           const auto lower = [](char c) {
             return c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
           };
           return lower(x) == lower(y);
         });
}

bool ReadFromCpk(const std::filesystem::path& cpk, std::string_view name,
                 std::vector<uint8_t>& data) {
  std::ifstream file(cpk, std::ios::binary);
  UtfTable header, toc;
  uint64_t toc_offset = 0, content_offset = 0;
  if (!file || !ReadChunk(file, 0, "CPK ", header) || !header.Int(0, "TocOffset", toc_offset) ||
      !ReadChunk(file, toc_offset, "TOC ", toc)) {
    return false;
  }
  header.Int(0, "ContentOffset", content_offset);
  const uint64_t base = content_offset ? std::min(toc_offset, content_offset) : toc_offset;
  std::string dir, file_name;
  for (uint32_t row = 0; row < toc.rows(); ++row) {
    if (!toc.String(row, "FileName", file_name)) {
      continue;
    }
    toc.String(row, "DirName", dir);
    if (!SameName(dir.empty() ? file_name : dir + "/" + file_name, name)) {
      continue;
    }
    uint64_t offset = 0, size = 0, extract_size = 0;
    if (!toc.Int(row, "FileOffset", offset) || !toc.Int(row, "FileSize", size) ||
        size > (256u << 20)) {
      return false;
    }
    toc.Int(row, "ExtractSize", extract_size);
    std::vector<uint8_t> stored(size);
    file.seekg(std::streamoff(base + offset));
    if (!file.read(reinterpret_cast<char*>(stored.data()), std::streamsize(size))) {
      return false;
    }
    if (extract_size && extract_size != size) {
      return Crilayla(stored, data) && data.size() == extract_size;
    }
    data = std::move(stored);
    return true;
  }
  return false;
}

void DecodeColors(const uint8_t* block, bool dxt1, uint8_t pixels[16][4]) {
  const uint16_t c0 = uint16_t(block[0] | block[1] << 8);
  const uint16_t c1 = uint16_t(block[2] | block[3] << 8);
  const uint32_t indices = Le32(block + 4);
  uint8_t palette[4][4];
  const auto expand = [](uint16_t c, uint8_t* rgba) {
    rgba[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgba[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgba[2] = uint8_t((c & 31) * 255 / 31);
    rgba[3] = 255;
  };
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  const bool four = c0 > c1 || !dxt1;
  for (int i = 0; i < 3; ++i) {
    palette[2][i] = uint8_t(four ? (2 * palette[0][i] + palette[1][i]) / 3
                                 : (palette[0][i] + palette[1][i]) / 2);
    palette[3][i] = uint8_t(four ? (palette[0][i] + 2 * palette[1][i]) / 3 : 0);
  }
  palette[2][3] = 255;
  palette[3][3] = four ? 255 : 0;
  for (int i = 0; i < 16; ++i) {
    std::memcpy(pixels[i], palette[(indices >> (2 * i)) & 3], 4);
  }
}

void DecodeAlpha(const uint8_t* block, int format, uint8_t pixels[16][4]) {
  if (format == 1) {
    for (int i = 0; i < 16; ++i) {
      pixels[i][3] = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 15) * 17);
    }
    return;
  }
  const int a0 = block[0], a1 = block[1];
  int palette[8] = {a0, a1};
  for (int i = 1; i < 7; ++i) {
    palette[i + 1] = a0 > a1 ? ((7 - i) * a0 + i * a1) / 7 : 0;
  }
  if (a0 <= a1) {
    for (int i = 1; i < 5; ++i) {
      palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
    }
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) {
    indices |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (int i = 0; i < 16; ++i) {
    pixels[i][3] = uint8_t(palette[(indices >> (3 * i)) & 7]);
  }
}

bool DecodeDxt(const uint8_t* data, size_t size, int format, uint32_t width, uint32_t height,
               BurstLimitTexture& texture) {
  const uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
  const size_t block_size = format == 0 ? 8 : 16;
  if (size < size_t(blocks_x) * blocks_y * block_size) {
    return false;
  }
  texture.width = width;
  texture.height = height;
  texture.rgba.assign(size_t(width) * height * 4, 0);
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      // Big-endian 16-bit words -> the usual little-endian block.
      const uint8_t* src = data + (size_t(by) * blocks_x + bx) * block_size;
      uint8_t block[16];
      for (size_t i = 0; i < block_size; i += 2) {
        block[i] = src[i + 1];
        block[i + 1] = src[i];
      }
      uint8_t pixels[16][4];
      DecodeColors(block + block_size - 8, format == 0, pixels);
      if (format != 0) {
        DecodeAlpha(block, format, pixels);
      }
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < width && y < height) {
          std::memcpy(&texture.rgba[(size_t(y) * width + x) * 4], pixels[i], 4);
        }
      }
    }
  }
  return true;
}

}  // namespace

bool BurstLimitReadGameFile(const std::filesystem::path& game_data_root, std::string_view name,
                            std::vector<uint8_t>& data) {
  std::error_code error;
  const std::filesystem::path folder = game_data_root / "LONG2DATA";
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    const std::string file = entry.path().filename().string();
    if (entry.is_regular_file(error) && file.size() > 4 &&
        SameName(file.substr(file.size() - 4), ".cpk") && ReadFromCpk(entry.path(), name, data)) {
      return true;
    }
  }
  return false;
}

bool BurstLimitListNut(const std::vector<uint8_t>& nut,
                       std::vector<BurstLimitNutTexture>& textures) {
  if (nut.size() < 0x10 || std::memcmp(nut.data(), "NTXR", 4) != 0) {
    return false;
  }
  const uint16_t count = Be16(&nut[6]);
  size_t pos = 0x10;
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 24 > nut.size()) {
      return false;
    }
    const uint32_t total = Be32(&nut[pos]);
    const uint32_t data_size = Be32(&nut[pos + 8]);
    const uint16_t header_size = Be16(&nut[pos + 12]);
    if (!total || pos + header_size + uint64_t(data_size) > nut.size()) {
      return false;
    }
    BurstLimitNutTexture texture;
    texture.format = Be16(&nut[pos + 18]);
    texture.width = Be16(&nut[pos + 20]);
    texture.height = Be16(&nut[pos + 22]);
    texture.data = &nut[pos + header_size];
    const size_t blocks = size_t((texture.width + 3) / 4) * ((texture.height + 3) / 4);
    switch (texture.format) {
      case 0:
        texture.base_size = blocks * 8;
        break;
      case 1:
      case 2:
        texture.base_size = blocks * 16;
        break;
      case 19:
        texture.base_size = size_t(texture.width) * texture.height * 4;
        break;
      default:
        texture.base_size = 0;
        break;
    }
    texture.base_size = std::min<size_t>(texture.base_size, data_size);
    textures.push_back(texture);
    pos += total;
  }
  return true;
}

bool BurstLimitDecodeNutTexture(const BurstLimitNutTexture& texture, BurstLimitTexture& image) {
  if (!texture.width || !texture.height || !texture.data) {
    return false;
  }
  if (texture.format <= 2) {
    return DecodeDxt(texture.data, texture.base_size, int(texture.format), texture.width,
                     texture.height, image);
  }
  if (texture.format == 19 && texture.base_size == size_t(texture.width) * texture.height * 4) {
    image.width = texture.width;
    image.height = texture.height;
    image.rgba.resize(texture.base_size);
    for (size_t i = 0; i < texture.base_size; i += 4) {
      // A R G B -> R G B A
      image.rgba[i] = texture.data[i + 1];
      image.rgba[i + 1] = texture.data[i + 2];
      image.rgba[i + 2] = texture.data[i + 3];
      image.rgba[i + 3] = texture.data[i];
    }
    return true;
  }
  return false;
}

bool BurstLimitDecodeNut(const std::vector<uint8_t>& nut,
                         std::vector<BurstLimitTexture>& textures) {
  std::vector<BurstLimitNutTexture> listed;
  if (!BurstLimitListNut(nut, listed)) {
    return false;
  }
  for (const BurstLimitNutTexture& texture : listed) {
    BurstLimitTexture image;
    BurstLimitDecodeNutTexture(texture, image);
    textures.push_back(std::move(image));
  }
  return true;
}

bool BurstLimitParseNfh(const std::vector<uint8_t>& nfh, std::vector<BurstLimitGlyph>& glyphs) {
  // "NFH", the glyph count at 8, then from 0x450 an entry of 32 bytes per
  // glyph: atlas x and y at +4, the visible glyph's start (negated) and end in
  // the cell at +8 and +12 (signed, 1/64 pixel), cell size at +20 and the
  // character at +22. Fonts with tight boxes (FUCHINASHI) have the bearing at
  // +8, the top above the baseline at +10 and the advance at +12.
  constexpr size_t kGlyphs = 0x450;
  constexpr size_t kGlyphSize = 32;
  if (nfh.size() < kGlyphs || std::memcmp(nfh.data(), "NFH", 3) != 0) {
    return false;
  }
  const uint32_t count = Be32(&nfh[8]);
  for (uint32_t i = 0; i < count && kGlyphs + (i + 1) * kGlyphSize <= nfh.size(); ++i) {
    const uint8_t* entry = &nfh[kGlyphs + i * kGlyphSize];
    BurstLimitGlyph glyph;
    glyph.x = Be16(entry + 4);
    glyph.y = Be16(entry + 6);
    glyph.left = -float(int16_t(Be16(entry + 8))) / 64.0f;
    glyph.right = float(int16_t(Be16(entry + 12))) / 64.0f;
    glyph.top = float(int16_t(Be16(entry + 10))) / 64.0f;
    glyph.width = entry[20];
    glyph.height = entry[21];
    glyph.code = Be16(entry + 22);
    glyphs.push_back(glyph);
  }
  return !glyphs.empty();
}

static uint16_t To565(const float c[3]) {
  const int r = std::clamp(int(std::lround(c[0] * 31.0f / 255.0f)), 0, 31);
  const int g = std::clamp(int(std::lround(c[1] * 63.0f / 255.0f)), 0, 63);
  const int b = std::clamp(int(std::lround(c[2] * 31.0f / 255.0f)), 0, 31);
  return uint16_t(r << 11 | g << 5 | b);
}

static void From565(uint16_t v, float c[3]) {
  c[0] = float((v >> 11) & 31) * 255.0f / 31.0f;
  c[1] = float((v >> 5) & 63) * 255.0f / 63.0f;
  c[2] = float(v & 31) * 255.0f / 31.0f;
}

void BurstLimitEncodeDxt5Block(const uint8_t pixels[16][4], uint8_t block[16]) {
  // Alpha: 8-value mode between the block's extremes.
  uint8_t a_min = 255, a_max = 0;
  for (int i = 0; i < 16; ++i) {
    a_min = std::min(a_min, pixels[i][3]);
    a_max = std::max(a_max, pixels[i][3]);
  }
  block[0] = a_max;
  block[1] = a_min;
  uint64_t alpha_bits = 0;
  if (a_max > a_min) {
    float palette[8];
    palette[0] = a_max;
    palette[1] = a_min;
    for (int k = 1; k <= 6; ++k) {
      palette[k + 1] = float((7 - k) * a_max + k * a_min) / 7.0f;
    }
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      float best_error = 1e9f;
      for (int k = 0; k < 8; ++k) {
        const float error = std::fabs(palette[k] - pixels[i][3]);
        if (error < best_error) {
          best_error = error;
          best = k;
        }
      }
      alpha_bits |= uint64_t(best) << (3 * i);
    }
  }
  for (int k = 0; k < 6; ++k) {
    block[2 + k] = uint8_t(alpha_bits >> (8 * k));
  }

  // Color: the endpoints on the main axis of the visible pixels' colors.
  float mean[3] = {0, 0, 0};
  int count = 0;
  for (int i = 0; i < 16; ++i) {
    if (pixels[i][3] >= 8) {
      for (int c = 0; c < 3; ++c) {
        mean[c] += pixels[i][c];
      }
      ++count;
    }
  }
  const bool use_all = count == 0;
  if (use_all) {
    for (int i = 0; i < 16; ++i) {
      for (int c = 0; c < 3; ++c) {
        mean[c] += pixels[i][c];
      }
    }
    count = 16;
  }
  for (float& m : mean) {
    m /= float(count);
  }
  float cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    if (!use_all && pixels[i][3] < 8) {
      continue;
    }
    const float d[3] = {pixels[i][0] - mean[0], pixels[i][1] - mean[1], pixels[i][2] - mean[2]};
    cov[0] += d[0] * d[0];
    cov[1] += d[0] * d[1];
    cov[2] += d[0] * d[2];
    cov[3] += d[1] * d[1];
    cov[4] += d[1] * d[2];
    cov[5] += d[2] * d[2];
  }
  float axis[3] = {0.577f, 0.577f, 0.577f};
  for (int iteration = 0; iteration < 8; ++iteration) {
    const float next[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                           cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                           cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
    const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
    if (length < 1e-6f) {
      break;
    }
    for (int c = 0; c < 3; ++c) {
      axis[c] = next[c] / length;
    }
  }
  float t_min = 1e9f, t_max = -1e9f;
  for (int i = 0; i < 16; ++i) {
    if (!use_all && pixels[i][3] < 8) {
      continue;
    }
    const float t = (pixels[i][0] - mean[0]) * axis[0] + (pixels[i][1] - mean[1]) * axis[1] +
                    (pixels[i][2] - mean[2]) * axis[2];
    t_min = std::min(t_min, t);
    t_max = std::max(t_max, t);
  }
  float e0[3], e1[3];
  for (int c = 0; c < 3; ++c) {
    e0[c] = std::clamp(mean[c] + axis[c] * t_max, 0.0f, 255.0f);
    e1[c] = std::clamp(mean[c] + axis[c] * t_min, 0.0f, 255.0f);
  }
  uint16_t c0 = To565(e0), c1 = To565(e1);
  if (c0 < c1) {
    std::swap(c0, c1);
  }
  float palette[4][3];
  From565(c0, palette[0]);
  From565(c1, palette[1]);
  for (int c = 0; c < 3; ++c) {
    palette[2][c] = (2.0f * palette[0][c] + palette[1][c]) / 3.0f;
    palette[3][c] = (palette[0][c] + 2.0f * palette[1][c]) / 3.0f;
  }
  uint32_t color_bits = 0;
  if (c0 != c1) {
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      float best_error = 1e9f;
      for (int k = 0; k < 4; ++k) {
        float error = 0.0f;
        for (int c = 0; c < 3; ++c) {
          const float d = palette[k][c] - pixels[i][c];
          error += d * d;
        }
        if (error < best_error) {
          best_error = error;
          best = k;
        }
      }
      color_bits |= uint32_t(best) << (2 * i);
    }
  }
  block[8] = uint8_t(c0);
  block[9] = uint8_t(c0 >> 8);
  block[10] = uint8_t(c1);
  block[11] = uint8_t(c1 >> 8);
  for (int k = 0; k < 4; ++k) {
    block[12 + k] = uint8_t(color_bits >> (8 * k));
  }
}
