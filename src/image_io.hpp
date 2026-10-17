#pragma once
// PPM (P6, RGB8) and PGM (P5, grey8) I/O. PPM is chosen deliberately: zero
// dependencies, readable by every viewer, and enough to eyeball scaler output
// and to compute PSNR/SSIM. Internal processing is RGBA8 (alpha = 255).

#include <cstdint>
#include <string>
#include <vector>

namespace lsl {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4
    bool empty() const { return w == 0 || h == 0; }
};

Image makeTestPattern(uint32_t w, uint32_t h);
Image loadImage(const std::string& path);   // ppm/pgm -> rgba
void  saveImage(const std::string& path, const Image& img);  // .ppm or .pgm by ext
void  savePPM(const std::string& path, const Image& img);

}  // namespace lsl
