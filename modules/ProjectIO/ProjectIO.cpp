#include "ProjectIO.h"
#include "ImageLoader/ImageLoader.h"
#include "json.hpp"
#include <fstream>

using json = nlohmann::json;

static json MetaToJson(const Metadata& m) {
    json j;
    j["name"] = m.name;
    j["rawText"] = m.rawText;
    json rows = json::array();
    for (auto& r : m.table) rows.push_back({{"key", r.key}, {"value", r.value}});
    j["table"] = rows;
    return j;
}

static Metadata MetaFromJson(const json& j) {
    Metadata m;
    m.name = j.value("name", std::string());
    m.rawText = j.value("rawText", std::string());
    if (j.contains("table")) {
        for (auto& row : j["table"]) {
            MetaRow r;
            r.key = row.value("key", std::string());
            r.value = row.value("value", std::string());
            m.table.push_back(r);
        }
    }
    return m;
}

bool SaveProjectJson(const Project& project, const std::string& path, std::string& errorOut) {
    json root;
    root["imagePath"] = project.imagePath; // kept for reference/display only
    root["nextId"] = project.nextId;
    // The image is embedded by its actual pixel data (as a base64 PNG), not
    // just a path to wherever it happened to live on disk - so the project
    // file is self-contained and still opens correctly if the original
    // image file is moved, renamed, or deleted, or the project is shared
    // with someone else / opened on a different machine.
    if (project.hasImage()) {
        root["imageW"] = project.imageW;
        root["imageH"] = project.imageH;
        root["imageDataPng"] = EncodePNGBase64(project.rgba.data(), project.imageW, project.imageH);
    }
    json polys = json::array();
    for (auto& poly : project.polygons) {
        json jp = MetaToJson(poly.meta);
        jp["id"] = poly.id;
        jp["color"] = {poly.colorR, poly.colorG, poly.colorB};
        jp["closed"] = poly.closed;
        json pts = json::array();
        for (auto& v : poly.points) {
            json jv = MetaToJson(v.meta);
            jv["id"] = v.id;
            jv["x"] = v.x;
            jv["y"] = v.y;
            pts.push_back(jv);
        }
        jp["points"] = pts;
        polys.push_back(jp);
    }
    root["polygons"] = polys;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        errorOut = "Could not open file for writing: " + path;
        return false;
    }
    out << root.dump(2);
    return true;
}

bool LoadProjectJson(Project& project, const std::string& path, std::string& errorOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        errorOut = "Could not open file: " + path;
        return false;
    }
    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        errorOut = std::string("Invalid project file: ") + e.what();
        return false;
    }

    Project loaded;
    loaded.imagePath = root.value("imagePath", std::string());
    loaded.nextId = root.value("nextId", 1);

    if (root.contains("imageDataPng") && !root["imageDataPng"].get<std::string>().empty()) {
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        if (!DecodePNGBase64(root["imageDataPng"].get<std::string>(), w, h, rgba)) {
            errorOut = "Project's embedded image data is corrupt.";
            return false;
        }
        loaded.imageW = w;
        loaded.imageH = h;
        loaded.rgba = std::move(rgba);
    } else if (!loaded.imagePath.empty()) {
        // Backward compatibility with project files saved before image data
        // was embedded: fall back to re-reading the original file path.
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        if (!LoadImageRGBA(loaded.imagePath, w, h, rgba)) {
            errorOut = "Project references image that could not be loaded: " + loaded.imagePath;
            return false;
        }
        loaded.imageW = w;
        loaded.imageH = h;
        loaded.rgba = std::move(rgba);
    }

    if (root.contains("polygons")) {
        for (auto& jp : root["polygons"]) {
            Polygon poly;
            poly.meta = MetaFromJson(jp);
            poly.id = jp.value("id", 0);
            poly.closed = jp.value("closed", true);
            if (jp.contains("color") && jp["color"].is_array() && jp["color"].size() == 3) {
                poly.colorR = jp["color"][0].get<float>();
                poly.colorG = jp["color"][1].get<float>();
                poly.colorB = jp["color"][2].get<float>();
            }
            if (jp.contains("points")) {
                for (auto& jv : jp["points"]) {
                    VertexPoint v;
                    v.meta = MetaFromJson(jv);
                    v.id = jv.value("id", 0);
                    v.x = jv.value("x", 0.f);
                    v.y = jv.value("y", 0.f);
                    poly.points.push_back(v);
                }
            }
            loaded.polygons.push_back(poly);
        }
    }

    project = std::move(loaded);
    return true;
}
