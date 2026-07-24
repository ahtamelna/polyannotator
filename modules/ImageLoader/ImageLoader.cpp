#include "ImageLoader.h"
#if defined(__APPLE__)
    #define GL_SILENCE_DEPRECATION
#endif
// Use the same GL function loader ImGui's OpenGL3 backend uses, rather than
// the legacy system <GL/gl.h>/<OpenGL/gl.h> headers (which only expose very
// old GL 1.1-era entry points on most platforms). We need modern functions
// like glBindBuffer/GL_PIXEL_UNPACK_BUFFER here too - see the comment in
// CreateGLTexture() below for why.
#include "imgui_impl_opengl3_loader.h"
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

bool LoadImageRGBA(const std::string& path, int& w, int& h, std::vector<unsigned char>& outRgba) {
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) return false;
    outRgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

unsigned int CreateGLTexture(const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return 0;

    // If a pixel-unpack buffer happens to be bound at this point (ImGui's
    // own backend binds/unbinds one around its font texture uploads), the
    // last argument of glTexImage2D below would be reinterpreted as a byte
    // OFFSET into that buffer instead of a client-memory pointer - reading
    // garbage and crashing. This reproduced reliably when opening an image
    // mid-session (e.g. via File > Open Image) rather than at startup.
    // Explicitly unbinding first makes glTexImage2D always read from `rgba`
    // as intended, regardless of what state a prior draw call left behind.
    GLint prevPixelUnpackBuffer = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevPixelUnpackBuffer);
    if (prevPixelUnpackBuffer != 0) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // Some GPU drivers' single-shot glTexImage2D upload path reads pixel
    // data based on internally rounded/tiled dimensions rather than the
    // exact width/height requested, over-reading past a tightly-packed
    // source buffer (confirmed via AddressSanitizer - and no fixed amount
    // of end padding reliably absorbed it, since the overread scaled with
    // the driver's internal tiling, not our buffer size). The standard,
    // well-tested fix: allocate the texture's storage with a NULL upload
    // (no source read happens at all), then upload the real pixel data via
    // glTexSubImage2D, which is the well-optimized "partial update" path
    // and takes an explicit, exact width/height/row-stride rather than
    // relying on the driver's own tiling math.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (prevPixelUnpackBuffer != 0) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prevPixelUnpackBuffer);
    return tex;
}

// --- base64 ---
static const char* kB64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kB64Chars[(n >> 18) & 0x3F]);
        out.push_back(kB64Chars[(n >> 12) & 0x3F]);
        out.push_back(kB64Chars[(n >> 6) & 0x3F]);
        out.push_back(kB64Chars[n & 0x3F]);
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        unsigned int n = data[i] << 16;
        out.push_back(kB64Chars[(n >> 18) & 0x3F]);
        out.push_back(kB64Chars[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        unsigned int n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(kB64Chars[(n >> 18) & 0x3F]);
        out.push_back(kB64Chars[(n >> 12) & 0x3F]);
        out.push_back(kB64Chars[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

static void PngWriteCallback(void* context, void* data, int size) {
    std::vector<unsigned char>* buf = (std::vector<unsigned char>*)context;
    unsigned char* bytes = (unsigned char*)data;
    buf->insert(buf->end(), bytes, bytes + size);
}

std::string EncodePNGBase64(const unsigned char* rgba, int w, int h) {
    std::vector<unsigned char> pngBuf;
    stbi_write_png_to_func(PngWriteCallback, &pngBuf, w, h, 4, rgba, w * 4);
    return Base64Encode(pngBuf.data(), pngBuf.size());
}

static int Base64CharValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<unsigned char> Base64Decode(const std::string& s) {
    std::vector<unsigned char> out;
    out.reserve((s.size() / 4) * 3);
    int buffer = 0, bits = 0;
    for (char c : s) {
        int v = Base64CharValue(c);
        if (v < 0) continue; // skips '=' padding and any whitespace
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

bool DecodePNGBase64(const std::string& base64Png, int& w, int& h, std::vector<unsigned char>& outRgba) {
    std::vector<unsigned char> pngBytes = Base64Decode(base64Png);
    if (pngBytes.empty()) return false;
    int channels = 0;
    unsigned char* data = stbi_load_from_memory(pngBytes.data(), (int)pngBytes.size(), &w, &h, &channels, 4);
    if (!data) return false;
    outRgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}
