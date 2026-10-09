#pragma once

// Reading the game's own files from its CRI CPK archives (LONG2DATA_*.CPK)
// and decoding the textures of its NUT files (NTXR, Xbox 360).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

// Reads a file such as "PAC/CMN/FIX/BUFAC/BUFAC_GOK.NUT" from the CPK archives
// in <game_data_root>/LONG2DATA, decompressed. False when it isn't found.
bool BurstLimitReadGameFile(const std::filesystem::path& game_data_root, std::string_view name,
                            std::vector<uint8_t>& data);

struct BurstLimitTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // R8G8B8A8, width * height * 4
};

// A texture inside an NTXR file: its first mip as stored (and as the game
// loads it), pointing into the file's data.
struct BurstLimitNutTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;  // 0 DXT1, 1 DXT3, 2 DXT5, 19 A8R8G8B8
  const uint8_t* data = nullptr;
  size_t base_size = 0;  // bytes of the first mip
};

// Lists the textures of an NTXR file.
bool BurstLimitListNut(const std::vector<uint8_t>& nut, std::vector<BurstLimitNutTexture>& textures);

// Decodes a texture's first mip; false for unsupported formats.
bool BurstLimitDecodeNutTexture(const BurstLimitNutTexture& texture, BurstLimitTexture& image);

// Decodes the first mip of each texture of an NTXR file (empty for
// unsupported formats).
bool BurstLimitDecodeNut(const std::vector<uint8_t>& nut, std::vector<BurstLimitTexture>& textures);

// A glyph of the game's fonts (an NFH header beside the NUT atlas): its cell
// in the atlas and where the visible glyph starts and ends in the cell.
struct BurstLimitGlyph {
  uint16_t code = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  float left = 0.0f;
  float right = 0.0f;
  // Tight-box fonts (FUCHINASHI): the glyph's top above the baseline.
  float top = 0.0f;
};

bool BurstLimitParseNfh(const std::vector<uint8_t>& nfh, std::vector<BurstLimitGlyph>& glyphs);

// One DXT5 block (the usual little-endian layout: alpha endpoints and indices, then
// the color endpoints and indices) from 16 RGBA8 pixels, row by row. The NUT
// files keep it as big-endian 16-bit words (swap each byte pair).
void BurstLimitEncodeDxt5Block(const uint8_t pixels[16][4], uint8_t block[16]);
