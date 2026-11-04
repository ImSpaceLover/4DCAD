// Image / video output for renders: PNG, JPEG, animated GIF, AVI (Motion-JPEG), and MP4 / WebM
// through an external ffmpeg. Pixels are RGBA8, top row first.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fd {

using Pixels = std::vector<uint8_t>;  // RGBA8, rows top to bottom

bool writePNG(const std::string& path, int w, int h, const Pixels& rgba);
bool writeJPEG(const std::string& path, int w, int h, const Pixels& rgba, int quality = 92);
/** Box-filter downscale by an integer factor (anti-aliasing of supersampled renders). */
Pixels downscale(const Pixels& rgba, int w, int h, int factor);

/** Writes frames to a file; close() finishes it. */
class FrameWriter {
 public:
  virtual ~FrameWriter() = default;
  virtual bool add(const Pixels& rgba) = 0;
  virtual bool close() = 0;
  std::string error;
};

/** Animated GIF: per-frame 256-colour palette (median cut) with ordered dithering, LZW coded. */
std::unique_ptr<FrameWriter> makeGifWriter(const std::string& path, int w, int h, double fps);
/** AVI with Motion-JPEG frames (plays in most players, no codecs needed). */
std::unique_ptr<FrameWriter> makeAviWriter(const std::string& path, int w, int h, double fps, int quality = 90);
/** Numbered PNG images: base_0001.png, ... */
std::unique_ptr<FrameWriter> makePngSequence(const std::string& basePath, int firstNumber, int w, int h);
/** Pipes raw frames into ffmpeg (codec "h264" -> .mp4, "vp9" -> .webm). */
std::unique_ptr<FrameWriter> makeFfmpegWriter(const std::string& ffmpeg, const std::string& path, int w, int h, double fps, const std::string& codec);
/** ffmpeg can be run (at this path or command name). */
bool ffmpegAvailable(const std::string& ffmpeg);

}  // namespace fd
