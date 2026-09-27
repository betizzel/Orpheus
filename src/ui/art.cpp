// art.cpp
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "art.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <sys/stat.h>
#include <vector>

namespace Art
{

/* Private helpers used in Generate() */
static uint32_t GetPixelRGBFromBuffer(
    const std::vector<unsigned char> &buf, int width, int height, int x, int y);
static int GrayscaleIndex(uint32_t rgb);

/* *
 * @brief ResolveImage path for a song (when taglib doesnt work)
 * @param song_path
 * */
std::string ResolveImage(const std::string &song_path)
{
  std::filesystem::path image_path(song_path);
  std::vector<std::string> possible_names = {"cover.jpg",
                                             "cover.png",
                                             "front.jpg",
                                             "front.png",
                                             "album.jpg",
                                             "album.png",
                                             "folder.jpg",
                                             "folder.png"};

  for (std::string name : possible_names)
  {
    image_path.replace_filename(name);
    struct stat buffer;

    if (stat(image_path.string().c_str(), &buffer) == 0)
    {
      return image_path.string();
    }
  }

  return "no image";
}

/* *
 * @brief load image from path
 * @param out our ascii image data
 * @param path where the image is
 * */
bool LoadImageFile(ImageData &out, const std::string &path)
{
  int w, h, ch;
  unsigned char *data = stbi_load(path.c_str(), &w, &h, &ch, 4);

  if (!data)
  {
    return false;
  }

  out.pixels.assign(data, data + (w * h * 4));
  out.width = w;
  out.height = h;
  out.channels = 4;
  stbi_image_free(data);

  return true;
}

/* *
 * @brief load image from memory
 * @param out our ascii image data
 * @param data image data in memory
 * @param length length of the data in memory
 * */
bool LoadImageMemory(ImageData &out, const uint8_t *data, size_t length)
{
  int w, h, ch;
  unsigned char *img = stbi_load_from_memory(data, static_cast<int>(length), &w, &h, &ch, 4);

  if (!img)
  {
    return false;
  }

  out.pixels.assign(img, img + (w * h * 4));
  out.width = w;
  out.height = h;
  out.channels = 4;
  stbi_image_free(img);

  return true;
}

/* *
 * @brief locate RGB pixel using whole image
 * @param @img : whole image
 * @param @x : location of pixel with x coord
 * @param @y : location of pixel with y coord
 * */
uint32_t GetPixelRGB(const ImageData &img, int x, int y)
{
  if (x < 0 || x >= img.width || y < 0 || y >= img.height)
  {
    return 0;
  }

  size_t idx = (static_cast<size_t>(y) * img.width + x) * 4;
  uint8_t r = img.pixels[idx];
  uint8_t g = img.pixels[idx + 1];
  uint8_t b = img.pixels[idx + 2];

  return ((static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
          static_cast<uint32_t>(b));
}

/* *
 * @brief Build a 16-step ANSI-256 ramp from the resized RGBA buffer,
 * sorted darkest -> brightest. The visualizer uses this so its colors
 * match the album art. Runs once per art generation (i.e. once per song),
 * not per frame.
 * */
static void extractPalette(const std::vector<unsigned char> &resized, int w, int h, int (&out)[16])
{
  // Aggregate per ANSI index: frequency + summed luminance (from raw RGB,
  // so luminance stays accurate even though the index is quantized).
  struct Entry
  {
    int count = 0;
    double lum_sum = 0.0;
  };

  std::map<int, Entry> buckets;

  for (int y = 0; y < h; ++y)
  {
    for (int x = 0; x < w; ++x)
    {
      size_t idx = (static_cast<size_t>(y) * w + x) * 4;
      uint8_t r = resized[idx];
      uint8_t g = resized[idx + 1];
      uint8_t b = resized[idx + 2];
      uint32_t rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;

      int ansi = RGBToANSI256(rgb);
      float lum = 0.299f * r + 0.587f * g + 0.114f * b;

      auto &e = buckets[ansi];
      e.count++;
      e.lum_sum += lum;
    }
  }

  // Sort unique ANSI colors by average luminance ascending.
  std::vector<std::pair<float, int>> sorted; // (avg_lum, ansi)
  sorted.reserve(buckets.size());

  for (const auto &kv : buckets)
  {
    sorted.emplace_back(static_cast<float>(kv.second.lum_sum / kv.second.count), kv.first);
  }

  std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
    return a.first < b.first;
  });

  const int N = static_cast<int>(sorted.size());
  if (N == 0)
  {
    for (int i = 0; i < 16; ++i)
    {
      out[i] = 235;
    }

    return;
  }

  // Pick 16 entries: if we have <= 16 unique colors, use them all and pad
  // by repeating the last. If > 16, stride evenly across the luminance-
  // sorted list so the ramp spans the art's full dark->bright range.
  for (int i = 0; i < 16; ++i)
  {
    int pick;

    if (N <= 16)
    {
      pick = std::min(i, N - 1);
    }
    else
    {
      pick = (i * N) / 16;
    }

    out[i] = sorted[pick].second;
  }
}

/* *
 * @brief generate the canvas for the ascii art, so it can be drawn to the ui
 * @param @image : image data
 * @param @mode  : GRAYSCALE or ANSI_256 Color modes
 * @param @max_cols : max cols of displayable screen space
 * @param @max_rows : max rows of displayable screen space
 * */
AsciiCanvas Generate(
    const ImageData &image, ColorMode mode, RenderMode rmode, int max_cols, int max_rows)
{
  AsciiCanvas canvas;
  canvas.mode = mode;

  // no image data
  if (image.pixels.empty() || image.width == 0 || image.height == 0)
  {
    return canvas;
  }

  // Ensure image fits its aspect ratio
  float img_aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
  float char_aspect = 0.5f;
  int target_w = max_cols;
  int target_h = static_cast<int>(static_cast<float>(target_w) * char_aspect / img_aspect);

  // clamp target_h
  if (target_h > max_rows)
  {
    target_h = max_rows;
    target_w = static_cast<int>(static_cast<float>(target_h) * img_aspect / char_aspect);
  }

  // no image size
  if (target_w <= 0 || target_h <= 0)
  {
    return canvas;
  }

  canvas.width = target_w;
  canvas.height = target_h;

  int resize_h = target_h * 2;
  std::vector<unsigned char> resized(target_w * resize_h * 4);

  // resize the image according to oure desired target sizes
  stbir_resize_uint8_linear(image.pixels.data(),
                            image.width,
                            image.height,
                            0,
                            resized.data(),
                            target_w,
                            resize_h,
                            0,
                            static_cast<stbir_pixel_layout>(4));

  // preallocate memory for the image area
  canvas.cells.reserve(target_w * target_h);

  // loop through the image and assign either the color or grayscale of it to the ▀ cell
  // I want to create a mode for just plain ▀ or ascii like .,/|\!@#$%*, though can you
  // add color to those characters ?
  for (int y = 0; y < target_h; ++y)
  {
    for (int x = 0; x < target_w; ++x)
    {
      int top_y = y * 2;
      int bot_y = y * 2 + 1;

      // foreground and background of the cells
      uint32_t top_rgb = GetPixelRGBFromBuffer(resized, target_w, resize_h, x, top_y);
      uint32_t bot_rgb = GetPixelRGBFromBuffer(resized, target_w, resize_h, x, bot_y);

      AsciiCell cell;

      switch (rmode)
      {
      case Art::RenderMode::BLOCK:
        // simple block based rendering
        cell.ch = L'▀';

        switch (mode)
        {
        case ColorMode::GRAYSCALE:
          cell.fg = GrayscaleIndex(top_rgb);
          cell.bg = GrayscaleIndex(bot_rgb);
          break;

        case ColorMode::ANSI_256:
          cell.fg = RGBToANSI256(top_rgb);
          cell.bg = RGBToANSI256(bot_rgb);
          break;
        }
        break;

      case Art::RenderMode::DETAILED:
        cell.ch = MapRGBToChar(top_rgb);

        // detailed .,:/|!#@$*
        switch (mode)
        {
        case ColorMode::GRAYSCALE:
          cell.fg = 255;
          cell.bg = 0;
          break;

        case ColorMode::ANSI_256:
          cell.fg = RGBToANSI256(top_rgb);
          cell.bg = 0;
          break;
        }
        break;
      }

      canvas.cells.push_back(cell);
    }
  }

  // Build the visualizer palette from the resized pixels so the EQ colors
  // match the art. Runs once per song.
  extractPalette(resized, target_w, resize_h, canvas.palette);
  canvas.has_palette = true;

  return canvas;
}

/* *
 * @brief Extracts the pixels RGB color from the buffer
 * @param @buf : buffer we're extracting a pixels color from
 * @param @width : width of image
 * @param @height : height of image
 * @param @x : location of pixel on x coord
 * @param @y : location of pixel on y coord
 * */
static uint32_t GetPixelRGBFromBuffer(
    const std::vector<unsigned char> &buf, int width, int height, int x, int y)
{
  // skip out of bounds
  if (x < 0 || x >= width || y < 0 || y >= height)
  {
    return 0;
  }

  // find the location
  size_t idx = (static_cast<size_t>(y) * width + x) * 4;

  // extract it in 3 different variables
  uint8_t r = buf[idx];
  uint8_t g = buf[idx + 1];
  uint8_t b = buf[idx + 2];

  // return it as a cast for one unint32_t var holding all of the information
  // (shifted by 16, 8, 0) for the data to fit
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

/* *
 * @brief 0-9 grayscale using
 * @param rgb color pixel
 * */
static int GrayscaleIndex(uint32_t rgb)
{
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;

  float lum = 0.299f * r + 0.587f * g + 0.114f * b;
  int idx = static_cast<int>((lum / 255.0f) * 9.0f);

  if (idx > 9)
  {
    idx = 9;
  }

  return idx;
}

/* *
 * @brief 6x6x6 color cube for ANSI256 color
 * @param rgb color pixel
 * */
int RGBToANSI256(uint32_t rgb)
{
  // unpack pixel data
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;

  // black
  if (r == 0 && g == 0 && b == 0)
  {
    return 235;
  }

  // 0-5 range for colors, and then later we'll round back to ansi colors
  int ri = (r * 5 + 127) / 255;
  int gi = (g * 5 + 127) / 255;
  int bi = (b * 5 + 127) / 255;

  // convert 0-5 range to ANSI colors
  return 16 + 36 * ri + 6 * gi + bi;
}

/* *
 * @brief Maps intesity meter to a char character  .,:;/!|#@$%
 * @param
 * */
char MapRGBToChar(uint32_t rgb)
{
  // some sort of RGB -> intesity
  // how is this done ?
  unsigned int r = (rgb >> 16) & 0xFF;
  unsigned int g = (rgb >> 8) & 0xFF;
  unsigned int b = (rgb) & 0xFF;

  // Gray Scale weights: R=0.299, G=0.587, B=0.114
  float intensity = (r * 0.299f) + (g * 0.587f) + (b * 0.114f);

  // convert integer index 0-10 chars
  int index = static_cast<int>((intensity / 255.0f) * 10.0f);

  if (index < 0)
  {
    index = 0;
  }

  if (index > 9)
  {
    index = 9;
  }

  // lookup table to immediatly return the char needed
  static const char charset[] = {'.', ',', ';', ':', '/', '!', '|', '#', '@', '$', '%'};
  return charset[index];
}

} // namespace Art
