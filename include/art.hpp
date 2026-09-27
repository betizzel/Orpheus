// art.hpp ascii art
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Art
{

enum class ColorMode
{
  GRAYSCALE,
  ANSI_256
};

enum class RenderMode
{
  BLOCK,
  DETAILED
};

struct ImageData
{
  // RGBA data
  std::vector<unsigned char> pixels;
  int width = 0;
  int height = 0;
  int channels = 0;
};

struct AsciiCell
{
  wchar_t ch;
  int fg; // ANSI index, extended color pair, or grayscale index
  int bg;
};

struct AsciiCanvas
{
  int width = 0;
  int height = 0;
  ColorMode mode;
  std::vector<AsciiCell> cells;

  // 16-step ANSI-256 ramp extracted from the image, sorted darkest ->
  // brightest. Used by the visualizer so the EQ colors match the art.
  // `has_palette` is false when no image was available.
  int palette[16] = {};
  bool has_palette = false;
};

/**
 * @brief Resolve image path for a song when embedded art is not available.
 *  Checks for cover.jpg, cover.png, front.jpg, front.png, album.jpg, album.png
 *  in the same directory as the song.
 * @param song_path Full path to the song file.
 * @return Path to the cover image, or "no image" if none found.
 */
std::string ResolveImage(const std::string &song_path);

/**
 * @brief Load image data from a file path into ImageData.
 * @param[out] out  ImageData to populate.
 * @param[in]  path Filesystem path to the image.
 */
bool LoadImageFile(ImageData &out, const std::string &path);

/**
 * @brief Load image data from a memory buffer.
 * @param[out] out    ImageData to populate.
 * @param[in]  data   Raw bytes.
 * @param[in]  length Number of bytes.
 */
bool LoadImageMemory(ImageData &out, const uint8_t *data, size_t length);

/**
 * @brief Generate an ASCII canvas from raw image pixels.
 *
 * @param image    Source image data (RGBA).
 * @param mode     color mode (grayscale, or ANSI-256).
 * @param rmode    Rendering mode (block or detailed).
 * @param max_cols Maximum columns available in the terminal pane.
 * @param max_rows Maximum rows available in the terminal pane.
 * @return Populated AsciiCanvas ready for ncurses rendering.
 */
AsciiCanvas Generate(
    const ImageData &image, ColorMode mode, RenderMode rmode, int max_cols, int max_rows);

/**
 * @brief Get a single pixel's RGB from ImageData at (x, y).
 * @return Packed 24-bit RGB value (0xRRGGBB).
 */
uint32_t GetPixelRGB(const ImageData &img, int x, int y);

int RGBToANSI256(uint32_t rgb);

char MapRGBToChar(uint32_t rgb);

} // namespace Art
