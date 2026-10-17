#include "image_io.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace lsl {
namespace {

// Reads the next header token, skipping whitespace and '#' comments.
std::string nextToken(FILE* f) {
    std::string tok;
    int c;
    for (;;) {
        c = std::fgetc(f);
        if (c == EOF) break;
        if (c == '#') {
            while (c != EOF && c != '\n') c = std::fgetc(f);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        break;
    }
    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        tok.push_back(char(c));
        c = std::fgetc(f);
    }
    return tok;
}

int tokenInt(FILE* f, const char* what) {
    std::string t = nextToken(f);
    if (t.empty()) throw std::runtime_error(std::string("truncated image header: ") + what);
    long v = std::strtol(t.c_str(), nullptr, 10);
    if (v <= 0 || v > 65535)
        throw std::runtime_error(std::string("bad image header value for ") + what + ": " + t);
    return int(v);
}

}  // namespace

Image loadImage(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);

    std::string magic = nextToken(f);
    bool greyscale = false;
    if (magic == "P6")      greyscale = false;
    else if (magic == "P5") greyscale = true;
    else {
        std::fclose(f);
        throw std::runtime_error(path + ": not a P5/P6 image (magic='" + magic + "')");
    }

    int w = tokenInt(f, "width");
    int h = tokenInt(f, "height");
    int maxv = tokenInt(f, "maxval");
    if (maxv != 255) {
        std::fclose(f);
        throw std::runtime_error(path + ": only maxval 255 supported");
    }
    // single whitespace already consumed by nextToken

    const size_t need = size_t(w) * size_t(h) * (greyscale ? 1 : 3);
    std::vector<uint8_t> raw(need);
    if (std::fread(raw.data(), 1, need, f) != need) {
        std::fclose(f);
        throw std::runtime_error(path + ": truncated pixel data");
    }
    std::fclose(f);

    Image img;
    img.w = uint32_t(w);
    img.h = uint32_t(h);
    img.rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        uint8_t r, g, b;
        if (greyscale) {
            r = g = b = raw[i];
        } else {
            r = raw[i * 3 + 0];
            g = raw[i * 3 + 1];
            b = raw[i * 3 + 2];
        }
        img.rgba[i * 4 + 0] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

void savePPM(const std::string& path, const Image& img) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fprintf(f, "P6\n%u %u\n255\n", img.w, img.h);
    std::vector<uint8_t> row(size_t(img.w) * 3);
    for (uint32_t y = 0; y < img.h; ++y) {
        for (uint32_t x = 0; x < img.w; ++x) {
            const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
            row[x * 3 + 0] = p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[2];
        }
        if (std::fwrite(row.data(), 1, row.size(), f) != row.size()) {
            std::fclose(f);
            throw std::runtime_error("short write: " + path);
        }
    }
    std::fclose(f);
}

void saveImage(const std::string& path, const Image& img) { savePPM(path, img); }

Image makeTestPattern(uint32_t w, uint32_t h) {
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.resize(size_t(w) * h * 4);

    const float cx = float(w) * 0.5f, cy = float(h) * 0.5f;
    const float rmax = std::sqrt(cx * cx + cy * cy);

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = float(x) / float(w);
            const float fy = float(y) / float(h);
            float r = 0, g = 0, b = 0;

            // horizontal gradient
            r = fx;

            // vertical gradient into green
            g = fy;

            // concentric rings + a checker block: high-frequency content that
            // exposes ringing and aliasing in cubic/Lanczos kernels
            const float dx = float(x) - cx, dy = float(y) - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            const bool rings = (std::fmod(d, 24.0f) < 12.0f);
            if (rings) b = 1.0f;

            // 8px checker in the lower-left quadrant
            if (x < w / 3 && y > h * 2 / 3) {
                const bool c = (((x / 8) + (y / 8)) & 1) != 0;
                r = g = b = c ? 1.0f : 0.0f;
            }

            // thin white diagonal lines (edge detail)
            if ((int(x) + int(y)) % 64 < 2) { r = g = b = 1.0f; }

            // solid disc
            if (d < rmax * 0.18f) { r = 0.1f; g = 0.8f; b = 0.4f; }

            uint8_t* p = &img.rgba[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(std::lround(std::fmin(std::fmax(r, 0.0f), 1.0f) * 255.0f));
            p[1] = uint8_t(std::lround(std::fmin(std::fmax(g, 0.0f), 1.0f) * 255.0f));
            p[2] = uint8_t(std::lround(std::fmin(std::fmax(b, 0.0f), 1.0f) * 255.0f));
            p[3] = 255;
        }
    }
    return img;
}

}  // namespace lsl
