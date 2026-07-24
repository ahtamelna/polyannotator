#pragma once
#include <string>
#include "Model/Model.h"

// Loads an image file (png/jpg/bmp/etc via stb_image) into RGBA8 pixels.
// Returns true on success and fills w,h,outRgba.
bool LoadImageRGBA(const std::string& path, int& w, int& h, std::vector<unsigned char>& outRgba);

// Creates (or re-creates) an OpenGL texture from RGBA8 pixel data.
// Returns the GL texture id (0 on failure).
unsigned int CreateGLTexture(const unsigned char* rgba, int w, int h);

// Encodes RGBA8 pixel data as a PNG in-memory, then returns a base64 string
// suitable for embedding as a data URI (image/png).
std::string EncodePNGBase64(const unsigned char* rgba, int w, int h);

// Reverse of the above: decodes a base64-encoded PNG (as produced by
// EncodePNGBase64) back into RGBA8 pixels. Returns true on success.
bool DecodePNGBase64(const std::string& base64Png, int& w, int& h, std::vector<unsigned char>& outRgba);
