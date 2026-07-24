#pragma once
#include <string>
#include <vector>
#include <memory>

// A single key/value row in a metadata "table"
struct MetaRow {
    std::string key;
    std::string value;
};

// Metadata shared by both vertices (points) and polygons:
// a table of key/value rows + a free-form raw text block.
struct Metadata {
    std::string name;                  // display name
    std::vector<MetaRow> table;        // table rows
    std::string rawText;               // free-form notes
};

// A single vertex ("point object") of a polygon.
struct VertexPoint {
    float x = 0.f;   // image-space coordinates (pixels, original image resolution)
    float y = 0.f;
    Metadata meta;
    int id = 0;
};

// A polygon made of ordered vertices, with its own metadata too.
struct Polygon {
    std::vector<VertexPoint> points;
    Metadata meta;
    float colorR = 0.99f, colorG = 0.35f, colorB = 0.25f; // stroke color (0-1)
    int id = 0;
    bool closed = true; // polygons are closed shapes once finished
};

// The whole project: one base image + all polygons drawn on it.
struct Project {
    std::string imagePath;       // original path (for reference / re-saving project)
    int imageW = 0, imageH = 0;  // pixel dims of the loaded image
    std::vector<unsigned char> rgba; // decoded pixel data (RGBA8), used for export
    std::vector<Polygon> polygons;
    int nextId = 1;

    bool hasImage() const { return imageW > 0 && imageH > 0 && !rgba.empty(); }
};
