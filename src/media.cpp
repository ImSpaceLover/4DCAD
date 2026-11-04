#include "media.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#define FD_POPEN _popen
#define FD_PCLOSE _pclose
#else
#define FD_POPEN popen
#define FD_PCLOSE pclose
#endif

namespace fd {

bool writePNG(const std::string& path, int w, int h, const Pixels& rgba) {
  if ((int)rgba.size() < w * h * 4) return false;
  return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

bool writeJPEG(const std::string& path, int w, int h, const Pixels& rgba, int quality) {
  if ((int)rgba.size() < w * h * 4) return false;
  return stbi_write_jpg(path.c_str(), w, h, 4, rgba.data(), quality) != 0;
}

Pixels downscale(const Pixels& in, int w, int h, int f) {
  if (f <= 1) return in;
  int ow = w / f, oh = h / f;
  Pixels out((size_t)ow * oh * 4);
  for (int y = 0; y < oh; y++)
    for (int x = 0; x < ow; x++)
      for (int c = 0; c < 4; c++) {
        int s = 0;
        for (int j = 0; j < f; j++)
          for (int i = 0; i < f; i++) s += in[((size_t)(y * f + j) * w + (x * f + i)) * 4 + c];
        out[((size_t)y * ow + x) * 4 + c] = (uint8_t)((s + f * f / 2) / (f * f));
      }
  return out;
}

// ============================================================ PNG sequence
namespace {

class PngSequence : public FrameWriter {
 public:
  PngSequence(std::string base, int first, int w, int h) : base_(std::move(base)), n_(first), w_(w), h_(h) {}
  bool add(const Pixels& px) override {
    std::string stem = base_, ext = ".png";
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos && stem.find_first_of("/\\", dot) == std::string::npos) stem = stem.substr(0, dot);
    char num[16];
    std::snprintf(num, sizeof num, "_%04d", n_++);
    if (!writePNG(stem + num + ext, w_, h_, px)) { error = "cannot write " + stem + num + ext; return false; }
    return true;
  }
  bool close() override { return true; }
  int w_ = 0, h_ = 0;

 private:
  std::string base_;
  int n_;
};

// ============================================================ GIF
class GifWriter : public FrameWriter {
 public:
  GifWriter(const std::string& path, int w, int h, double fps) : w_(w), h_(h) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) { error = "cannot write " + path; return; }
    delay_ = std::max(2, (int)std::lround(100.0 / std::max(1.0, fps)));
    put("GIF89a", 6);
    u16(w); u16(h);
    byte(0); byte(0); byte(0);  // no global palette
    // loop forever (NETSCAPE2.0)
    const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
    put(loop, sizeof loop);
  }
  ~GifWriter() override { if (f_) close(); }

  bool add(const Pixels& px) override {
    if (!f_) return false;
    std::array<std::array<uint8_t, 3>, 256> pal{};
    int count = palette(px, pal);
    // graphic control: delay
    const uint8_t gce[] = {0x21, 0xF9, 0x04, 0x04, (uint8_t)(delay_ & 255), (uint8_t)(delay_ >> 8), 0x00, 0x00};
    put(gce, sizeof gce);
    byte(0x2C);
    u16(0); u16(0); u16(w_); u16(h_);
    byte(0x87);  // local palette, 256 entries
    for (int i = 0; i < 256; i++) put(pal[i].data(), 3);
    // map to the palette (ordered dither: smooth gradients without bands); cache by 15-bit colour
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    std::vector<int16_t> cache(32768, -1);
    std::vector<uint8_t> idx((size_t)w_ * h_);
    for (int y = 0; y < h_; y++)
      for (int x = 0; x < w_; x++) {
        const uint8_t* p = &px[((size_t)y * w_ + x) * 4];
        int d = (bayer[y & 3][x & 3] - 8) / 2;
        int r = std::clamp(p[0] + d, 0, 255), g = std::clamp(p[1] + d, 0, 255), b = std::clamp(p[2] + d, 0, 255);
        int key = (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
        if (cache[key] < 0) {
          int best = 0, bd = 1 << 30;
          for (int i = 0; i < count; i++) {
            int dr = pal[i][0] - r, dg = pal[i][1] - g, db = pal[i][2] - b, dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (dd < bd) { bd = dd; best = i; }
          }
          cache[key] = (int16_t)best;
        }
        idx[(size_t)y * w_ + x] = (uint8_t)cache[key];
      }
    lzw(idx);
    return true;
  }

  bool close() override {
    if (!f_) return false;
    byte(0x3B);
    bool ok = std::fclose(f_) == 0;
    f_ = nullptr;
    return ok;
  }

 private:
  FILE* f_ = nullptr;
  int w_, h_, delay_ = 4;
  void put(const void* d, size_t n) { std::fwrite(d, 1, n, f_); }
  void byte(int b) { uint8_t c = (uint8_t)b; put(&c, 1); }
  void u16(int v) { byte(v & 255); byte((v >> 8) & 255); }

  // median cut: split the box with the widest colour range at its median until 256 boxes
  int palette(const Pixels& px, std::array<std::array<uint8_t, 3>, 256>& pal) {
    std::vector<std::array<uint8_t, 3>> cols;
    size_t n = (size_t)w_ * h_, stride = std::max<size_t>(1, n / 65536);
    for (size_t i = 0; i < n; i += stride) cols.push_back({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]});
    struct Box { size_t a, b; };
    std::vector<Box> boxes{{0, cols.size()}};
    auto range = [&](const Box& bx, int& ch) {
      int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
      for (size_t i = bx.a; i < bx.b; i++)
        for (int c = 0; c < 3; c++) { lo[c] = std::min<int>(lo[c], cols[i][c]); hi[c] = std::max<int>(hi[c], cols[i][c]); }
      int best = -1;
      for (int c = 0; c < 3; c++) if (hi[c] - lo[c] > best) { best = hi[c] - lo[c]; ch = c; }
      return best;
    };
    while (boxes.size() < 256) {
      int bi = -1, br = 0, bch = 0;
      for (int i = 0; i < (int)boxes.size(); i++) {
        if (boxes[i].b - boxes[i].a < 2) continue;
        int ch, r = range(boxes[i], ch);
        if (r > br) { br = r; bi = i; bch = ch; }
      }
      if (bi < 0) break;
      Box bx = boxes[bi];
      size_t mid = (bx.a + bx.b) / 2;
      std::nth_element(cols.begin() + bx.a, cols.begin() + mid, cols.begin() + bx.b, [&](auto& p, auto& q) { return p[bch] < q[bch]; });
      boxes[bi] = {bx.a, mid};
      boxes.push_back({mid, bx.b});
    }
    for (size_t i = 0; i < boxes.size(); i++) {
      long s[3] = {0, 0, 0};
      size_t m = std::max<size_t>(1, boxes[i].b - boxes[i].a);
      for (size_t k = boxes[i].a; k < boxes[i].b; k++) for (int c = 0; c < 3; c++) s[c] += cols[k][c];
      for (int c = 0; c < 3; c++) pal[i][c] = (uint8_t)(s[c] / (long)m);
    }
    return std::max<int>(1, (int)boxes.size());
  }

  // GIF LZW with 8-bit pixels, variable code size, written in 255-byte sub-blocks
  void lzw(const std::vector<uint8_t>& data) {
    const int minBits = 8, clear = 1 << minBits, eoi = clear + 1;
    byte(minBits);
    std::vector<uint8_t> block;
    uint32_t acc = 0;
    int nacc = 0, bits = minBits + 1, next = eoi + 1;
    auto emit = [&](int code) {
      acc |= (uint32_t)code << nacc;
      nacc += bits;
      while (nacc >= 8) {
        block.push_back((uint8_t)(acc & 255));
        acc >>= 8;
        nacc -= 8;
        if (block.size() == 255) { byte(255); put(block.data(), 255); block.clear(); }
      }
    };
    std::unordered_map<uint32_t, int> dict;  // (prefix code << 8 | byte) -> code
    dict.reserve(8192);
    emit(clear);
    int prefix = data.empty() ? 0 : data[0];
    for (size_t i = 1; i < data.size(); i++) {
      uint32_t k = (uint32_t)prefix << 8 | data[i];
      auto it = dict.find(k);
      if (it != dict.end()) { prefix = it->second; continue; }
      emit(prefix);
      if (next < 4096) {
        dict[k] = next++;
        if (next > (1 << bits) && bits < 12) bits++;
      } else {  // table full: start over
        emit(clear);
        dict.clear();
        bits = minBits + 1;
        next = eoi + 1;
      }
      prefix = data[i];
    }
    emit(prefix);
    emit(eoi);
    if (nacc > 0) block.push_back((uint8_t)(acc & 255));
    if (!block.empty()) { byte((int)block.size()); put(block.data(), block.size()); }
    byte(0);
  }
};

// ============================================================ AVI (Motion-JPEG)
class AviWriter : public FrameWriter {
 public:
  AviWriter(const std::string& path, int w, int h, double fps, int quality) : w_(w), h_(h), fps_(fps), q_(quality) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) { error = "cannot write " + path; return; }
    fourcc("RIFF"); riffSize_ = pos(); u32(0); fourcc("AVI ");
    fourcc("LIST"); long hdrlSize = pos(); u32(0); fourcc("hdrl");
    fourcc("avih"); u32(56);
    u32((uint32_t)std::lround(1e6 / fps)); u32(0); u32(0); u32(0x10 /* has index */);
    framesAt_ = pos(); u32(0); u32(0); u32(1); u32(0); u32(w); u32(h); u32(0); u32(0); u32(0); u32(0);
    fourcc("LIST"); long strlSize = pos(); u32(0); fourcc("strl");
    fourcc("strh"); u32(56); fourcc("vids"); fourcc("MJPG"); u32(0); u32(0); u32(0);
    u32(1000); u32((uint32_t)std::lround(fps * 1000)); u32(0); lengthAt_ = pos(); u32(0); u32(0); u32(0xFFFFFFFFu); u32(0);
    u16(0); u16(0); u16(w); u16(h);
    fourcc("strf"); u32(40); u32(40); u32(w); u32(h); u16(1); u16(24); fourcc("MJPG"); u32(w * h * 3); u32(0); u32(0); u32(0); u32(0);
    patch(strlSize, pos() - strlSize - 4);
    patch(hdrlSize, pos() - hdrlSize - 4);
    fourcc("LIST"); moviSize_ = pos(); u32(0); moviStart_ = pos(); fourcc("movi");
  }
  ~AviWriter() override { if (f_) close(); }

  bool add(const Pixels& px) override {
    if (!f_) return false;
    std::vector<uint8_t> jpg;
    stbi_write_jpg_to_func([](void* ctx, void* data, int size) {
      auto* v = (std::vector<uint8_t>*)ctx;
      v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
    }, &jpg, w_, h_, 4, px.data(), q_);
    long at = pos();
    fourcc("00dc");
    u32((uint32_t)jpg.size());
    std::fwrite(jpg.data(), 1, jpg.size(), f_);
    if (jpg.size() & 1) { uint8_t z = 0; std::fwrite(&z, 1, 1, f_); }  // chunks are word aligned
    index_.push_back({(uint32_t)(at - moviStart_), (uint32_t)jpg.size()});
    frames_++;
    return true;
  }

  bool close() override {
    if (!f_) return false;
    patch(moviSize_, pos() - moviSize_ - 4);
    fourcc("idx1");
    u32((uint32_t)(index_.size() * 16));
    for (auto& [off, size] : index_) { fourcc("00dc"); u32(0x10); u32(off); u32(size); }
    patch(riffSize_, pos() - 8);
    patch(framesAt_, frames_);
    patch(lengthAt_, frames_);
    bool ok = std::fclose(f_) == 0;
    f_ = nullptr;
    return ok;
  }

 private:
  FILE* f_ = nullptr;
  int w_, h_;
  double fps_;
  int q_;
  long riffSize_ = 0, moviSize_ = 0, moviStart_ = 0, framesAt_ = 0, lengthAt_ = 0;
  uint32_t frames_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> index_;
  long pos() { return std::ftell(f_); }
  void fourcc(const char* s) { std::fwrite(s, 1, 4, f_); }
  void u32(uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; std::fwrite(b, 1, 4, f_); }
  void u16(int v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; std::fwrite(b, 1, 2, f_); }
  void patch(long at, long value) {
    long here = pos();
    std::fseek(f_, at, SEEK_SET);
    u32((uint32_t)value);
    std::fseek(f_, here, SEEK_SET);
  }
};

// ============================================================ ffmpeg
std::string quoteArg(const std::string& s) {
#ifdef _WIN32
  return "\"" + s + "\"";
#else
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
#endif
}

class FfmpegWriter : public FrameWriter {
 public:
  FfmpegWriter(const std::string& exe, const std::string& path, int w, int h, double fps, const std::string& codec) : w_(w), h_(h) {
    char size[32], rate[32];
    std::snprintf(size, sizeof size, "%dx%d", w, h);
    std::snprintf(rate, sizeof rate, "%.6g", fps);
    std::string enc = codec == "vp9" ? "-c:v libvpx-vp9 -b:v 0 -crf 32 -pix_fmt yuv420p" : "-c:v libx264 -crf 18 -preset medium -pix_fmt yuv420p";
    std::string cmd = quoteArg(exe) + " -y -loglevel error -f rawvideo -pix_fmt rgba -s " + size + " -r " + rate +
                      " -i - -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" " + enc + " " + quoteArg(path);
#ifdef _WIN32
    p_ = FD_POPEN(cmd.c_str(), "wb");
#else
    p_ = FD_POPEN(cmd.c_str(), "w");
#endif
    if (!p_) error = "cannot start ffmpeg";
  }
  ~FfmpegWriter() override { if (p_) close(); }
  bool add(const Pixels& px) override {
    if (!p_) return false;
    size_t n = (size_t)w_ * h_ * 4;
    if (std::fwrite(px.data(), 1, n, p_) != n) { error = "ffmpeg stopped (see its messages)"; return false; }
    return true;
  }
  bool close() override {
    if (!p_) return false;
    int r = FD_PCLOSE(p_);
    p_ = nullptr;
    if (r != 0) error = "ffmpeg failed (exit code " + std::to_string(r) + ")";
    return r == 0;
  }

 private:
  FILE* p_ = nullptr;
  int w_, h_;
};

}  // namespace

std::unique_ptr<FrameWriter> makeGifWriter(const std::string& path, int w, int h, double fps) { return std::make_unique<GifWriter>(path, w, h, fps); }
std::unique_ptr<FrameWriter> makeAviWriter(const std::string& path, int w, int h, double fps, int quality) {
  return std::make_unique<AviWriter>(path, w, h, fps, quality);
}
std::unique_ptr<FrameWriter> makePngSequence(const std::string& basePath, int firstNumber, int w, int h) {
  return std::make_unique<PngSequence>(basePath, firstNumber, w, h);
}
std::unique_ptr<FrameWriter> makeFfmpegWriter(const std::string& ffmpeg, const std::string& path, int w, int h, double fps, const std::string& codec) {
  return std::make_unique<FfmpegWriter>(ffmpeg, path, w, h, fps, codec);
}

bool ffmpegAvailable(const std::string& ffmpeg) {
#ifdef _WIN32
  std::string cmd = quoteArg(ffmpeg) + " -version >NUL 2>&1";
#else
  std::string cmd = quoteArg(ffmpeg) + " -version >/dev/null 2>&1";
#endif
  FILE* p = FD_POPEN(cmd.c_str(), "r");
  if (!p) return false;
  char buf[256];
  while (std::fgets(buf, sizeof buf, p)) {}
  return FD_PCLOSE(p) == 0;
}

}  // namespace fd
