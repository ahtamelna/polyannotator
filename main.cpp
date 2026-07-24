// Polygon Annotator
// Load an image, pan/zoom it, draw polygons vertex-by-vertex, attach
// metadata (table + raw text) to each polygon AND each vertex, then export
// the whole thing as a self-contained, navigable HTML file.

#if defined(__APPLE__)
    #define GL_SILENCE_DEPRECATION
#endif

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder* - used once to set up the default panel layout
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "Model/Model.h"
#include "ImageLoader/ImageLoader.h"
#include "HtmlExporter/HtmlExporter.h"
#include "ProjectIO/ProjectIO.h"
#include "ImportIO/ImportIO.h"
#include "tinyfiledialogs.h"

#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <array>

// ---------------------------------------------------------------------
// Branding
// ---------------------------------------------------------------------
// Edit this to whatever you'd like credited/copyrighted.
static const char* kCopyrightText = "Polygon Annotator  \xC2\xA9 2026. All rights reserved.";
static const ImVec4 kAccent = ImVec4(0.145f, 0.435f, 0.851f, 1.0f); // blue accent

// ---------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------
struct AppState {
    Project project;
    unsigned int textureId = 0;

    // view/navigation
    ImVec2 pan = ImVec2(0, 0); // screen-space offset of image origin
    float zoom = 1.0f;
    bool viewInitialized = false;
    ImVec2 lastMouseImagePos = ImVec2(0, 0);
    bool mouseOverCanvas = false;

    // drawing state
    bool drawingPolygon = false;
    Polygon draftPolygon;

    // selection / editing
    int selectedPolygon = -1; // index into project.polygons
    int selectedVertex = -1;  // index into polygon.points
    bool draggingVertex = false;

    // UI helpers
    char statusMsg[256] = "";
    double statusMsgTime = 0.0;
    bool showShortcuts = false;
    bool darkMode = false;

    // metadata import (CSV/XLSX) - a workbook can have multiple sheets, each
    // holding different metadata columns for the same polygons (e.g. one
    // sheet with material info, another with inspection notes); every
    // sheet gets its own match column and row mapping, and Apply Import
    // merges all of them into each polygon's metadata at once.
    bool showImportDialog = false;
    ImportedWorkbook importWorkbook;
    int importActiveSheet = 0; // which sheet tab is currently shown in the dialog
    bool importColumnsArePolygons = false;
    std::vector<int> importMatchColumnPerSheet;       // [sheetIdx] -> column index
    std::vector<std::vector<int>> importRowMapPerSheet; // [sheetIdx][polygonIdx] -> row index, -1 = none
    int importActivePolygon = -1; // which polygon is "armed" - click a table row to assign it

    // Deferred actions: menu callbacks run mid-frame (after ImGui::NewFrame()
    // has already started), and creating a GL texture at that point raced
    // with in-flight rendering commands from the same frame - this caused a
    // crash under Mesa and, on other drivers, would plausibly show up as a
    // texture that silently fails to display correctly instead. Menu
    // handlers just record *what* to do; the main loop performs the actual
    // GL work once per frame, before any ImGui/GL calls for that frame have
    // started, which is always a safe, consistent point to do it.
    std::string pendingImageOpen;
    std::string pendingProjectOpen;


    // cursor: what the mouse cursor should look like this frame (decided
    // during canvas drawing, applied once after rendering)
    enum class CursorKind { Arrow, Hand, Crosshair } desiredCursor = CursorKind::Arrow;
};

static void SetStatus(AppState& app, const std::string& msg) {
    snprintf(app.statusMsg, sizeof(app.statusMsg), "%s", msg.c_str());
    app.statusMsgTime = ImGui::GetTime();
}

// ---------------------------------------------------------------------
// Image loading
// ---------------------------------------------------------------------
static bool OpenImageFile(AppState& app, const std::string& path) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!LoadImageRGBA(path, w, h, rgba)) {
        SetStatus(app, "Failed to load image: " + path);
        return false;
    }
    if (app.textureId) {
        GLuint t = app.textureId;
        glDeleteTextures(1, &t);
        app.textureId = 0;
    }
    app.project = Project(); // reset project when opening a fresh image
    app.project.imagePath = path;
    app.project.imageW = w;
    app.project.imageH = h;
    app.project.rgba = std::move(rgba);
    app.textureId = CreateGLTexture(app.project.rgba.data(), w, h);
    app.viewInitialized = false;
    app.selectedPolygon = -1;
    app.selectedVertex = -1;
    app.drawingPolygon = false;
    app.draftPolygon = Polygon();
    SetStatus(app, "Loaded image " + path);
    return true;
}

static void ReuploadTexture(AppState& app) {
    if (app.textureId) {
        GLuint t = app.textureId;
        glDeleteTextures(1, &t);
    }
    app.textureId = CreateGLTexture(app.project.rgba.data(), app.project.imageW, app.project.imageH);
}

// Clears the currently loaded image and everything derived from it
// (polygons, selection, view). Does not touch the import dialog state.
static void CloseImage(AppState& app) {
    if (app.textureId) {
        GLuint t = app.textureId;
        glDeleteTextures(1, &t);
        app.textureId = 0;
    }
    app.project = Project();
    app.viewInitialized = false;
    app.selectedPolygon = -1;
    app.selectedVertex = -1;
    app.drawingPolygon = false;
    app.draftPolygon = Polygon();
    SetStatus(app, "Image closed.");
}

// Makes sure `path` ends with `.ext` (case-insensitive check), appending it
// if not. Native save dialogs on some platforms/backends don't reliably
// enforce the filter extension, which silently produced files that
// "Open Project" (filtered to *.json) could never find - this guards
// against that regardless of which OS/dialog backend is in play.
static std::string EnsureExtension(const std::string& path, const std::string& ext) {
    if (path.size() >= ext.size()) {
        std::string tail = path.substr(path.size() - ext.size());
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::string extLower = ext;
        std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (tail == extLower) return path;
    }
    return path + ext;
}

// Case-insensitive check for whether `path` ends with `.ext`. Used as a
// belt-and-suspenders check before opening a project file: the native file
// dialog's own type filter *should* already restrict the choice, but that
// filter mechanism has proven unreliable on some platforms, so this gives a
// clear "wrong file type" message instead of a confusing parse failure if
// something other than a .polyproj file gets through.
static bool HasExtension(const std::string& path, const std::string& ext) {
    if (path.size() < ext.size()) return false;
    std::string tail = path.substr(path.size() - ext.size());
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string extLower = ext;
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return tail == extLower;
}

// ---------------------------------------------------------------------
// Metadata import (CSV/XLSX) helpers
// ---------------------------------------------------------------------
static std::string LowerTrim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string t = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return t;
}

// Transposes a sheet for the "polygons are in columns" layout: the first
// row holds polygon identifiers (one per column) and the first column
// holds property names, e.g.
//     Property   | Poly A | Poly B
//     Material   | Steel  | Wood
//     Depth (mm) | 3      | 5
// becomes an effective row-oriented sheet with one row per polygon.
static ImportedSheet TransposeSheet(const ImportedSheet& s) {
    size_t nCols = s.headers.size();
    size_t nRows = 1 + s.rows.size(); // headers row counts as row 0 of the raw grid
    auto cell = [&](size_t r, size_t c) -> std::string {
        if (r == 0) return c < s.headers.size() ? s.headers[c] : "";
        size_t ri = r - 1;
        return (ri < s.rows.size() && c < s.rows[ri].size()) ? s.rows[ri][c] : "";
    };
    ImportedSheet out;
    std::string idLabel = cell(0, 0);
    out.headers.push_back(idLabel.empty() ? "Name" : idLabel);
    for (size_t r = 1; r < nRows; ++r) out.headers.push_back(cell(r, 0));
    for (size_t c = 1; c < nCols; ++c) {
        std::vector<std::string> row;
        row.push_back(cell(0, c));
        for (size_t r = 1; r < nRows; ++r) row.push_back(cell(r, c));
        out.rows.push_back(std::move(row));
    }
    return out;
}

// Picks a sensible default "match column" - one whose header contains "name",
// otherwise column 0.
static int GuessMatchColumn(const ImportedSheet& sheet) {
    for (int i = 0; i < (int)sheet.headers.size(); ++i) {
        if (LowerTrim(sheet.headers[i]).find("name") != std::string::npos) return i;
    }
    return 0;
}

// Matches each polygon to a row in ONE sheet by comparing the polygon's name
// against that sheet's chosen match column, case-insensitively. Leaves an
// entry at -1 if no row matches.
static void AutoMapSheet(AppState& app, const ImportedSheet& sheet, int sheetIdx) {
    std::vector<int>& rowMap = app.importRowMapPerSheet[sheetIdx];
    rowMap.assign(app.project.polygons.size(), -1);
    int matchCol = app.importMatchColumnPerSheet[sheetIdx];
    for (size_t i = 0; i < app.project.polygons.size(); ++i) {
        std::string pname = LowerTrim(app.project.polygons[i].meta.name);
        if (pname.empty()) continue;
        for (size_t r = 0; r < sheet.rows.size(); ++r) {
            if ((size_t)matchCol < sheet.rows[r].size() && LowerTrim(sheet.rows[r][matchCol]) == pname) {
                rowMap[i] = (int)r;
                break;
            }
        }
    }
}

// Auto-matches every sheet in the workbook at once (each against its own
// guessed match column) - the common case when a workbook's sheets all use
// the same polygon names/identifiers.
static void AutoMapAllSheets(AppState& app) {
    for (int s = 0; s < (int)app.importWorkbook.sheets.size(); ++s) {
        const ImportedSheet& raw = app.importWorkbook.sheets[s];
        ImportedSheet effective = app.importColumnsArePolygons ? TransposeSheet(raw) : raw;
        app.importMatchColumnPerSheet[s] = GuessMatchColumn(effective);
        AutoMapSheet(app, effective, s);
    }
}

// Applies one sheet's current row mapping: for every polygon with an
// assigned row, each non-empty cell becomes (or updates) a metadata table
// entry keyed by its column header. Safe to call once per sheet in a row -
// each call merges into whatever the previous sheet's call already wrote,
// which is exactly how multiple sheets' metadata end up combined.
static bool ApplySheetMapping(Polygon& poly, const ImportedSheet& sheet, int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)sheet.rows.size()) return false;
    const std::vector<std::string>& row = sheet.rows[rowIdx];
    for (size_t c = 0; c < sheet.headers.size() && c < row.size(); ++c) {
        if (row[c].empty()) continue;
        const std::string& key = sheet.headers[c];
        bool found = false;
        for (auto& existing : poly.meta.table) {
            if (LowerTrim(existing.key) == LowerTrim(key)) {
                existing.value = row[c];
                found = true;
                break;
            }
        }
        if (!found) poly.meta.table.push_back(MetaRow{key, row[c]});
    }
    return true;
}

// Applies every sheet's mapping to every polygon, in order. Returns how many
// distinct polygons received at least one field from any sheet.
static int ApplyAllSheetMappings(AppState& app) {
    std::vector<bool> touched(app.project.polygons.size(), false);
    for (int s = 0; s < (int)app.importWorkbook.sheets.size(); ++s) {
        const ImportedSheet& raw = app.importWorkbook.sheets[s];
        ImportedSheet effective = app.importColumnsArePolygons ? TransposeSheet(raw) : raw;
        const std::vector<int>& rowMap = app.importRowMapPerSheet[s];
        for (size_t i = 0; i < app.project.polygons.size() && i < rowMap.size(); ++i) {
            if (ApplySheetMapping(app.project.polygons[i], effective, rowMap[i])) touched[i] = true;
        }
    }
    int count = 0;
    for (bool t : touched) if (t) ++count;
    return count;
}

// ---------------------------------------------------------------------
// Coordinate transforms: image-space (pixels) <-> screen-space
// ---------------------------------------------------------------------
static ImVec2 ImageToScreen(const AppState& app, ImVec2 canvasOrigin, ImVec2 imgPt) {
    return ImVec2(canvasOrigin.x + app.pan.x + imgPt.x * app.zoom,
                   canvasOrigin.y + app.pan.y + imgPt.y * app.zoom);
}
static ImVec2 ScreenToImage(const AppState& app, ImVec2 canvasOrigin, ImVec2 screenPt) {
    return ImVec2((screenPt.x - canvasOrigin.x - app.pan.x) / app.zoom,
                   (screenPt.y - canvasOrigin.y - app.pan.y) / app.zoom);
}

static void FitImageToCanvas(AppState& app, ImVec2 canvasSize) {
    if (!app.project.hasImage()) return;
    float pad = 20.0f;
    float availW = std::max(10.0f, canvasSize.x - pad * 2);
    float availH = std::max(10.0f, canvasSize.y - pad * 2);
    float sx = availW / app.project.imageW;
    float sy = availH / app.project.imageH;
    app.zoom = std::max(0.02f, std::min(sx, sy));
    app.pan.x = (canvasSize.x - app.project.imageW * app.zoom) / 2.0f;
    app.pan.y = (canvasSize.y - app.project.imageH * app.zoom) / 2.0f;
}

static bool PointInPolygon(const std::vector<VertexPoint>& pts, float px, float py) {
    bool inside = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        float xi = pts[i].x, yi = pts[i].y;
        float xj = pts[j].x, yj = pts[j].y;
        bool intersect = ((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

// ---------------------------------------------------------------------
// Polygon triangulation (ear clipping) - needed because ImGui's
// AddConvexPolyFilled only renders correctly for CONVEX polygons; anything
// concave (very common in real annotation work) came out visibly wrong.
// This handles convex AND concave simple (non-self-intersecting) polygons.
// ---------------------------------------------------------------------
static float Cross2(const ImVec2& o, const ImVec2& a, const ImVec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static bool PointInTriangle(const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    float d1 = Cross2(a, b, p);
    float d2 = Cross2(b, c, p);
    float d3 = Cross2(c, a, p);
    bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(hasNeg && hasPos);
}

static std::vector<std::array<int, 3>> TriangulatePolygon(const std::vector<ImVec2>& pts) {
    std::vector<std::array<int, 3>> tris;
    int n = (int)pts.size();
    if (n < 3) return tris;
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = i;

    float area = 0.0f;
    for (int i = 0; i < n; ++i) {
        const ImVec2& a = pts[idx[i]];
        const ImVec2& b = pts[idx[(i + 1) % n]];
        area += (a.x * b.y - b.x * a.y);
    }
    if (area < 0.0f) std::reverse(idx.begin(), idx.end());

    int guard = 0;
    int maxGuard = n * n + 8;
    while ((int)idx.size() > 3 && guard++ < maxGuard) {
        bool clipped = false;
        int m = (int)idx.size();
        for (int i = 0; i < m; ++i) {
            int i0 = idx[(i - 1 + m) % m];
            int i1 = idx[i];
            int i2 = idx[(i + 1) % m];
            const ImVec2& a = pts[i0];
            const ImVec2& b = pts[i1];
            const ImVec2& c = pts[i2];
            if (Cross2(a, b, c) <= 0.0f) continue; // reflex/degenerate corner - not a valid ear

            bool anyInside = false;
            for (int j = 0; j < m; ++j) {
                int pj = idx[j];
                if (pj == i0 || pj == i1 || pj == i2) continue;
                if (PointInTriangle(pts[pj], a, b, c)) { anyInside = true; break; }
            }
            if (anyInside) continue;

            tris.push_back({i0, i1, i2});
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) break; // degenerate/self-intersecting input - stop safely rather than loop forever
    }
    if (idx.size() == 3) tris.push_back({idx[0], idx[1], idx[2]});
    return tris;
}

// ---------------------------------------------------------------------
// Canvas: image navigation + polygon drawing/selection/editing
// ---------------------------------------------------------------------
static void DrawCanvasContent(AppState& app) {
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    if (canvasSize.x <= 0 || canvasSize.y <= 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bgCol = app.darkMode ? IM_COL32(35, 37, 43, 255) : IM_COL32(224, 227, 232, 255);
    dl->AddRectFilled(canvasOrigin, ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y), bgCol);

    ImGui::InvisibleButton("canvas_input", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool wantKeyboard = ImGui::GetIO().WantTextInput; // true while a text field is focused elsewhere
    app.mouseOverCanvas = hovered;

    if (app.project.hasImage()) {
        if (!app.viewInitialized) {
            FitImageToCanvas(app, canvasSize);
            app.viewInitialized = true;
        }

        if (hovered) app.lastMouseImagePos = ScreenToImage(app, canvasOrigin, ImGui::GetIO().MousePos);

        // Zoom with mouse wheel, anchored at cursor.
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            ImVec2 beforeImg = ScreenToImage(app, canvasOrigin, mouse);
            float factor = powf(1.12f, ImGui::GetIO().MouseWheel);
            app.zoom = std::max(0.01f, std::min(60.0f, app.zoom * factor));
            ImVec2 afterScreen = ImageToScreen(app, canvasOrigin, beforeImg);
            app.pan.x += mouse.x - afterScreen.x;
            app.pan.y += mouse.y - afterScreen.y;
        }

        // Pan with right-mouse, middle-mouse, or Space+left-mouse drag.
        bool spaceDown = ImGui::IsKeyDown(ImGuiKey_Space);
        bool spacePan = active && spaceDown && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        bool isPanning = active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || spacePan);
        if (isPanning) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            app.pan.x += delta.x;
            app.pan.y += delta.y;
        }

        // Keyboard shortcuts (only when no text field elsewhere has focus).
        if (!wantKeyboard) {
            float kbdZoomFactor = 0.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) kbdZoomFactor = 1.2f;
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) kbdZoomFactor = 1.0f / 1.2f;
            if (kbdZoomFactor != 0.0f) {
                ImVec2 center = ImVec2(canvasOrigin.x + canvasSize.x * 0.5f, canvasOrigin.y + canvasSize.y * 0.5f);
                ImVec2 beforeImg = ScreenToImage(app, canvasOrigin, center);
                app.zoom = std::max(0.01f, std::min(60.0f, app.zoom * kbdZoomFactor));
                ImVec2 afterScreen = ImageToScreen(app, canvasOrigin, beforeImg);
                app.pan.x += center.x - afterScreen.x;
                app.pan.y += center.y - afterScreen.y;
            }
            float panStep = 40.0f;
            if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) app.pan.x += panStep;
            if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) app.pan.x -= panStep;
            if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) app.pan.y += panStep;
            if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) app.pan.y -= panStep;
            if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_Home)) {
                app.viewInitialized = false;
            }
        }

        // Draw the image.
        ImVec2 imgMin = ImageToScreen(app, canvasOrigin, ImVec2(0, 0));
        ImVec2 imgMax = ImageToScreen(app, canvasOrigin, ImVec2((float)app.project.imageW, (float)app.project.imageH));
        dl->PushClipRect(canvasOrigin, ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y), true);
        if (app.textureId) {
            dl->AddImage((ImTextureID)(intptr_t)app.textureId, imgMin, imgMax);
        }

        // --- Vertex hit-test helper (used for hover cursor, click-select, and drag) ---
        auto hitTestVertex = [&](ImVec2 mouse, int& outPoly, int& outVertex) {
            outPoly = -1; outVertex = -1;
            float hitRadiusScreen = 8.0f;
            for (int pi = (int)app.project.polygons.size() - 1; pi >= 0; --pi) {
                Polygon& poly = app.project.polygons[pi];
                for (int vi = 0; vi < (int)poly.points.size(); ++vi) {
                    ImVec2 sp = ImageToScreen(app, canvasOrigin, ImVec2(poly.points[vi].x, poly.points[vi].y));
                    float dx = sp.x - mouse.x, dy = sp.y - mouse.y;
                    if (dx * dx + dy * dy <= hitRadiusScreen * hitRadiusScreen) { outPoly = pi; outVertex = vi; return; }
                }
            }
        };

        // --- Draw finished polygons ---
        for (int pi = 0; pi < (int)app.project.polygons.size(); ++pi) {
            Polygon& poly = app.project.polygons[pi];
            if (poly.points.empty()) continue;
            ImU32 col = IM_COL32((int)(poly.colorR * 255), (int)(poly.colorG * 255), (int)(poly.colorB * 255), 255);
            ImU32 fillCol = IM_COL32((int)(poly.colorR * 255), (int)(poly.colorG * 255), (int)(poly.colorB * 255), pi == app.selectedPolygon ? 90 : 45);

            std::vector<ImVec2> screenPts;
            screenPts.reserve(poly.points.size());
            ImVec2 centroid(0, 0);
            for (auto& v : poly.points) {
                ImVec2 sp = ImageToScreen(app, canvasOrigin, ImVec2(v.x, v.y));
                screenPts.push_back(sp);
                centroid.x += sp.x;
                centroid.y += sp.y;
            }
            centroid.x /= (float)screenPts.size();
            centroid.y /= (float)screenPts.size();

            if (screenPts.size() >= 3) {
                // Ear-clipping triangulation handles concave polygons correctly,
                // unlike AddConvexPolyFilled (which silently mis-renders them).
                std::vector<std::array<int, 3>> tris = TriangulatePolygon(screenPts);
                for (auto& t : tris) dl->AddTriangleFilled(screenPts[t[0]], screenPts[t[1]], screenPts[t[2]], fillCol);
            }
            for (size_t i = 0; i < screenPts.size(); ++i) {
                ImVec2 a = screenPts[i];
                ImVec2 b = screenPts[(i + 1) % screenPts.size()];
                if (screenPts.size() >= 2 && (i + 1 < screenPts.size() || poly.closed))
                    dl->AddLine(a, b, col, pi == app.selectedPolygon ? 3.0f : 2.0f);
            }
            for (size_t i = 0; i < screenPts.size(); ++i) {
                bool vSel = (pi == app.selectedPolygon && (int)i == app.selectedVertex);
                dl->AddCircleFilled(screenPts[i], vSel ? 6.0f : 4.0f, vSel ? IM_COL32(255, 255, 255, 255) : col);
                dl->AddCircle(screenPts[i], vSel ? 6.0f : 4.0f, IM_COL32(0, 0, 0, 160));
            }

            if (!poly.meta.name.empty()) {
                ImVec2 textSize = ImGui::CalcTextSize(poly.meta.name.c_str());
                ImVec2 textPos(centroid.x - textSize.x * 0.5f, centroid.y - textSize.y * 0.5f);
                dl->AddRectFilled(ImVec2(textPos.x - 5, textPos.y - 2), ImVec2(textPos.x + textSize.x + 5, textPos.y + textSize.y + 2),
                                   IM_COL32(20, 22, 26, 180), 3.0f);
                dl->AddText(textPos, IM_COL32(255, 255, 255, 255), poly.meta.name.c_str());
            }
        }

        // --- Draw in-progress polygon (with snap-to-close guide on the first vertex) ---
        bool nearFirstVertex = false;
        ImVec2 firstVtxScreen(0, 0);
        if (app.drawingPolygon) {
            std::vector<ImVec2> screenPts;
            for (auto& v : app.draftPolygon.points) screenPts.push_back(ImageToScreen(app, canvasOrigin, ImVec2(v.x, v.y)));

            ImVec2 mouse = ImGui::GetIO().MousePos;
            if (screenPts.size() >= 3 && hovered) {
                firstVtxScreen = screenPts[0];
                float dx = mouse.x - firstVtxScreen.x, dy = mouse.y - firstVtxScreen.y;
                nearFirstVertex = (dx * dx + dy * dy) <= (12.0f * 12.0f);
            }

            for (size_t i = 0; i + 1 < screenPts.size(); ++i)
                dl->AddLine(screenPts[i], screenPts[i + 1], IM_COL32(255, 170, 30, 255), 2.0f);
            if (!screenPts.empty() && hovered) {
                ImVec2 snapTarget = nearFirstVertex ? firstVtxScreen : mouse;
                dl->AddLine(screenPts.back(), snapTarget, IM_COL32(255, 170, 30, 150), 1.5f);
            }
            for (auto& p : screenPts) dl->AddCircleFilled(p, 4.0f, IM_COL32(255, 170, 30, 255));

            if (nearFirstVertex) {
                dl->AddCircle(firstVtxScreen, 10.0f, IM_COL32(60, 190, 90, 255), 0, 3.0f);
                ImVec2 tip = ImVec2(firstVtxScreen.x + 14, firstVtxScreen.y - 18);
                dl->AddRectFilled(ImVec2(tip.x - 4, tip.y - 2), ImVec2(tip.x + 118, tip.y + 16), IM_COL32(20, 22, 26, 210), 3.0f);
                dl->AddText(tip, IM_COL32(150, 255, 160, 255), "Click to close");
            }
        }

        dl->PopClipRect();

        // --- Cursor shape ---
        if (hovered) {
            if (app.drawingPolygon) app.desiredCursor = AppState::CursorKind::Crosshair;
            else if (isPanning || spaceDown) app.desiredCursor = AppState::CursorKind::Hand;
            else {
                int hp, hv;
                hitTestVertex(ImGui::GetIO().MousePos, hp, hv);
                app.desiredCursor = (hv >= 0) ? AppState::CursorKind::Hand : AppState::CursorKind::Arrow;
            }
        }

        // --- Click handling ---
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !spaceDown && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            ImVec2 imgPt = ScreenToImage(app, canvasOrigin, mouse);

            if (app.drawingPolygon) {
                if (nearFirstVertex) {
                    app.draftPolygon.id = app.project.nextId++;
                    app.draftPolygon.meta.name = "Polygon " + std::to_string(app.project.polygons.size() + 1);
                    app.project.polygons.push_back(app.draftPolygon);
                    app.selectedPolygon = (int)app.project.polygons.size() - 1;
                    app.selectedVertex = -1;
                    SetStatus(app, "Polygon closed with " + std::to_string(app.draftPolygon.points.size()) + " vertices.");
                    app.drawingPolygon = false;
                    app.draftPolygon = Polygon();
                } else {
                    VertexPoint v;
                    v.x = imgPt.x;
                    v.y = imgPt.y;
                    v.id = app.project.nextId++;
                    v.meta.name = "Point " + std::to_string(app.draftPolygon.points.size() + 1);
                    app.draftPolygon.points.push_back(v);
                }
            } else {
                int hitPoly, hitVertex;
                hitTestVertex(mouse, hitPoly, hitVertex);
                if (hitPoly < 0) {
                    for (int pi = (int)app.project.polygons.size() - 1; pi >= 0; --pi) {
                        if (app.project.polygons[pi].points.size() >= 3 &&
                            PointInPolygon(app.project.polygons[pi].points, imgPt.x, imgPt.y)) {
                            hitPoly = pi;
                            break;
                        }
                    }
                }
                app.selectedPolygon = hitPoly;
                app.selectedVertex = hitVertex;
                app.draggingVertex = (hitVertex >= 0); // arm a possible drag; a plain click just re-selects the same vertex
            }
        }

        // --- Vertex dragging (move a vertex by grabbing it) ---
        if (!app.drawingPolygon) {
            if (app.draggingVertex && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                app.selectedPolygon >= 0 && app.selectedPolygon < (int)app.project.polygons.size()) {
                Polygon& poly = app.project.polygons[app.selectedPolygon];
                if (app.selectedVertex >= 0 && app.selectedVertex < (int)poly.points.size()) {
                    ImVec2 mouse = ImGui::GetIO().MousePos;
                    ImVec2 imgPt = ScreenToImage(app, canvasOrigin, mouse);
                    poly.points[app.selectedVertex].x = imgPt.x;
                    poly.points[app.selectedVertex].y = imgPt.y;
                    app.desiredCursor = AppState::CursorKind::Hand;
                }
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                app.draggingVertex = false;
            }
        }

        // Finish polygon: double left-click or Enter
        if (app.drawingPolygon &&
            ((hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) || ImGui::IsKeyPressed(ImGuiKey_Enter))) {
            if (app.draftPolygon.points.size() >= 3) {
                app.draftPolygon.id = app.project.nextId++;
                app.draftPolygon.meta.name = "Polygon " + std::to_string(app.project.polygons.size() + 1);
                app.project.polygons.push_back(app.draftPolygon);
                app.selectedPolygon = (int)app.project.polygons.size() - 1;
                app.selectedVertex = -1;
                SetStatus(app, "Polygon created with " + std::to_string(app.draftPolygon.points.size()) + " vertices.");
            } else {
                SetStatus(app, "Need at least 3 vertices to finish a polygon.");
            }
            app.drawingPolygon = false;
            app.draftPolygon = Polygon();
        }
        // Escape: cancel drafting, or deselect if nothing is being drawn.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (app.drawingPolygon) {
                app.drawingPolygon = false;
                app.draftPolygon = Polygon();
                SetStatus(app, "Cancelled polygon drawing.");
            } else if (app.selectedPolygon >= 0) {
                app.selectedPolygon = -1;
                app.selectedVertex = -1;
            }
        }
        // Undo last vertex while drawing
        if (app.drawingPolygon && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !app.draftPolygon.points.empty()) {
            app.draftPolygon.points.pop_back();
        }
        // Delete selected polygon
        if (!app.drawingPolygon && app.selectedPolygon >= 0 && !wantKeyboard && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            app.project.polygons.erase(app.project.polygons.begin() + app.selectedPolygon);
            app.selectedPolygon = -1;
            app.selectedVertex = -1;
        }
    } else {
        ImVec2 textSize = ImGui::CalcTextSize("File > Open Image to begin");
        ImVec2 textPos = ImVec2(canvasOrigin.x + canvasSize.x / 2 - textSize.x / 2, canvasOrigin.y + canvasSize.y / 2 - textSize.y / 2);
        ImU32 hintCol = app.darkMode ? IM_COL32(150, 154, 162, 255) : IM_COL32(120, 124, 132, 255);
        dl->AddText(textPos, hintCol, "File > Open Image to begin");
    }

    // Reserve the space so the window's scroll extents match what we drew into.
    ImGui::Dummy(canvasSize);
}

// ---------------------------------------------------------------------
// Metadata editor widget (used for both polygon and vertex metadata)
// ---------------------------------------------------------------------
static void DrawMetadataEditor(Metadata& meta, const char* idScope) {
    ImGui::PushID(idScope);
    char nameBuf[256];
    snprintf(nameBuf, sizeof(nameBuf), "%s", meta.name.c_str());
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) meta.name = nameBuf;

    ImGui::TextDisabled("Metadata table");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Add row")) meta.table.push_back(MetaRow{"key", "value"});

    if (ImGui::BeginTable("meta_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        int deleteIdx = -1;
        for (int i = 0; i < (int)meta.table.size(); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char kbuf[128]; snprintf(kbuf, sizeof(kbuf), "%s", meta.table[i].key.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##k", kbuf, sizeof(kbuf))) meta.table[i].key = kbuf;
            ImGui::TableSetColumnIndex(1);
            char vbuf[512]; snprintf(vbuf, sizeof(vbuf), "%s", meta.table[i].value.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##v", vbuf, sizeof(vbuf))) meta.table[i].value = vbuf;
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("x")) deleteIdx = i;
            ImGui::PopID();
        }
        if (deleteIdx >= 0) meta.table.erase(meta.table.begin() + deleteIdx);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Raw text / notes");
    char rawBuf[4096];
    snprintf(rawBuf, sizeof(rawBuf), "%s", meta.rawText.c_str());
    if (ImGui::InputTextMultiline("##rawtext", rawBuf, sizeof(rawBuf), ImVec2(-1, 120))) meta.rawText = rawBuf;
    ImGui::PopID();
}

// ---------------------------------------------------------------------
// Inspector panel content (dockable "Inspector" window)
// ---------------------------------------------------------------------
static void DrawInspectorContent(AppState& app) {
    ImGui::TextColored(kAccent, "Inspector");
    ImGui::Separator();

    if (app.selectedPolygon < 0 || app.selectedPolygon >= (int)app.project.polygons.size()) {
        ImGui::TextWrapped("Select a polygon on the canvas to edit its metadata and vertices.");
        return;
    }

    Polygon& poly = app.project.polygons[app.selectedPolygon];

    ImGui::ColorEdit3("Color", &poly.colorR, ImGuiColorEditFlags_NoInputs);
    DrawMetadataEditor(poly.meta, "polymeta");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Vertices (%d) - click on canvas & drag to move, or expand below", (int)poly.points.size());

    int deleteVertexIdx = -1;
    for (int i = 0; i < (int)poly.points.size(); ++i) {
        VertexPoint& v = poly.points[i];
        ImGui::PushID(i);
        bool isSelected = (app.selectedVertex == i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
        if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
        std::string label = v.meta.name.empty() ? ("Point " + std::to_string(i + 1)) : v.meta.name;
        bool open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked()) app.selectedVertex = i;
        if (open) {
            ImGui::TextDisabled("Image position: (%.1f, %.1f)", v.x, v.y);
            bool canDelete = poly.points.size() > 3;
            if (!canDelete) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Delete vertex")) deleteVertexIdx = i;
            if (!canDelete) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(polygon needs at least 3 vertices)");
            }
            ImGui::Spacing();
            DrawMetadataEditor(v.meta, ("vertexmeta" + std::to_string(i)).c_str());
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (deleteVertexIdx >= 0) {
        poly.points.erase(poly.points.begin() + deleteVertexIdx);
        if (app.selectedVertex == deleteVertexIdx) app.selectedVertex = -1;
        else if (app.selectedVertex > deleteVertexIdx) app.selectedVertex--;
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Delete Polygon", ImVec2(-1, 0))) {
        app.project.polygons.erase(app.project.polygons.begin() + app.selectedPolygon);
        app.selectedPolygon = -1;
        app.selectedVertex = -1;
    }
}

// ---------------------------------------------------------------------
// Shortcuts panel (toggled from the toolbar)
// ---------------------------------------------------------------------
static void DrawShortcutsPanel(AppState& app) {
    if (!app.showShortcuts) return;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Keyboard & Mouse Shortcuts", &app.showShortcuts, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTable("shortcuts_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Shortcut");
            ImGui::TableHeadersRow();
            auto row = [](const char* action, const char* keys) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(action);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(keys);
            };
            row("Zoom in / out", "Scroll wheel, or = / -");
            row("Pan", "Right-drag, middle-drag, or Space+drag");
            row("Pan (fine)", "Arrow keys");
            row("Fit image to window", "F, or Home");
            row("Start new polygon", "Toolbar button");
            row("Add vertex", "Left click (while drawing)");
            row("Close polygon", "Click near 1st vertex, Enter, or double-click");
            row("Undo last vertex", "Backspace (while drawing)");
            row("Cancel drawing", "Escape");
            row("Select polygon / vertex", "Left click");
            row("Move a vertex", "Click and drag a vertex handle");
            row("Deselect", "Escape");
            row("Delete selected polygon", "Delete");
            row("Delete a single vertex", "Inspector > expand vertex > Delete vertex");
            row("Toggle light / dark mode", "Toolbar button");
            row("Dock/undock panels", "Drag a panel's title tab");
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------
// Import metadata from CSV/XLSX: click a polygon, then click its row.
// ---------------------------------------------------------------------
static void DrawImportDialog(AppState& app) {
    if (!app.showImportDialog) return;
    if (app.importWorkbook.sheets.empty()) { app.showImportDialog = false; return; }
    ImGui::SetNextWindowSize(ImVec2(960, 660), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Import Metadata", &open, ImGuiWindowFlags_NoCollapse)) {
        int sheetCount = (int)app.importWorkbook.sheets.size();
        if (sheetCount > 1) {
            ImGui::TextWrapped("This workbook has %d sheets. Each one can be mapped separately - switch tabs below, "
                                "assign rows on each sheet, then Apply Import merges everything from every sheet "
                                "into your polygons at once.", sheetCount);
        } else {
            ImGui::TextWrapped("Click a polygon below, then click its row in the table to assign it.");
        }

        ImGui::Spacing();
        ImGui::Text("Layout:");
        ImGui::SameLine();
        bool wasColumns = app.importColumnsArePolygons;
        if (ImGui::RadioButton("Each row is a polygon", !app.importColumnsArePolygons)) app.importColumnsArePolygons = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Each column is a polygon", app.importColumnsArePolygons)) app.importColumnsArePolygons = true;
        if (wasColumns != app.importColumnsArePolygons) AutoMapAllSheets(app);

        if (sheetCount > 1) {
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(16, 0));
            ImGui::SameLine();
            if (ImGui::Button("Auto-match ALL sheets by name")) AutoMapAllSheets(app);
        }

        if (sheetCount > 1) {
            ImGui::Spacing();
            if (ImGui::BeginTabBar("import_sheet_tabs")) {
                for (int s = 0; s < sheetCount; ++s) {
                    std::string label = app.importWorkbook.sheets[s].name;
                    if (label.empty()) label = "Sheet " + std::to_string(s + 1);
                    // Show a small marker once a sheet has at least one row assigned.
                    bool anyAssigned = false;
                    for (int v : app.importRowMapPerSheet[s]) if (v >= 0) { anyAssigned = true; break; }
                    if (anyAssigned) label += "  \xE2\x97\x8F"; // filled circle
                    if (ImGui::BeginTabItem(label.c_str())) {
                        app.importActiveSheet = s;
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }

        const ImportedSheet& raw = app.importWorkbook.sheets[app.importActiveSheet];
        ImportedSheet effective = app.importColumnsArePolygons ? TransposeSheet(raw) : raw;
        std::vector<int>& rowMap = app.importRowMapPerSheet[app.importActiveSheet];
        int& matchColumn = app.importMatchColumnPerSheet[app.importActiveSheet];

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(kAccent, "Optional shortcut: auto-match this sheet by name");
        ImGui::TextWrapped("If this sheet has a column with the same names as your polygons, pick it here and click "
                            "Auto-match to fill in the assignments below automatically. You can still review, change, "
                            "or clear any assignment by hand afterwards - this is just a time-saver, not required.");
        ImGui::SetNextItemWidth(220);
        std::string preview = (matchColumn >= 0 && matchColumn < (int)effective.headers.size())
            ? effective.headers[matchColumn] : "(none)";
        if (ImGui::BeginCombo("Column to compare against polygon names", preview.c_str())) {
            for (int c = 0; c < (int)effective.headers.size(); ++c) {
                bool sel = (c == matchColumn);
                if (ImGui::Selectable(effective.headers[c].c_str(), sel)) matchColumn = c;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Auto-match this sheet")) AutoMapSheet(app, effective, app.importActiveSheet);
        ImGui::Separator();

        float leftWidth = 250.0f;
        ImGui::BeginChild("PolygonList", ImVec2(leftWidth, 400), true);
        ImGui::TextDisabled("1. Click a polygon");
        ImGui::Separator();
        for (int i = 0; i < (int)app.project.polygons.size(); ++i) {
            ImGui::PushID(i);
            int mapped = rowMap[i];
            bool isActive = (app.importActivePolygon == i);
            std::string label = app.project.polygons[i].meta.name;
            if (label.empty()) label = "Polygon " + std::to_string(i + 1);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
            if (ImGui::Selectable(label.c_str(), isActive, 0, ImVec2(leftWidth - 55, 0))) {
                app.importActivePolygon = i;
            }
            ImGui::PopStyleVar();
            ImGui::SameLine();
            if (mapped >= 0 && mapped < (int)effective.rows.size()) {
                ImGui::TextColored(kAccent, "R%d", mapped + 1);
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) rowMap[i] = -1;
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("PreviewTable", ImVec2(0, 400), true);
        if (app.importActivePolygon < 0 || app.importActivePolygon >= (int)app.project.polygons.size()) {
            ImGui::TextDisabled("2. Select a polygon on the left, then click a row below to assign it.");
        } else {
            ImGui::TextDisabled("2. Click a row to assign it to: ");
            ImGui::SameLine();
            ImGui::TextColored(kAccent, "%s", app.project.polygons[app.importActivePolygon].meta.name.c_str());
        }
        ImGui::Separator();
        if (ImGui::BeginTable("import_preview", (int)effective.headers.size() + 2,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, -1))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
            ImGui::TableSetupColumn("Assigned to", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            for (auto& h : effective.headers) ImGui::TableSetupColumn(h.c_str());
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (int r = 0; r < (int)effective.rows.size(); ++r) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                int assignedTo = -1;
                for (int p = 0; p < (int)rowMap.size(); ++p)
                    if (rowMap[p] == r) { assignedTo = p; break; }

                char rowLabel[16];
                snprintf(rowLabel, sizeof(rowLabel), "%d", r + 1);
                bool rowIsActiveAssignment = (assignedTo == app.importActivePolygon && app.importActivePolygon >= 0);
                ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
                bool clickable = app.importActivePolygon >= 0 && app.importActivePolygon < (int)app.project.polygons.size();
                if (!clickable) ImGui::BeginDisabled();
                if (ImGui::Selectable(rowLabel, rowIsActiveAssignment, flags)) {
                    rowMap[app.importActivePolygon] = r;
                    // Auto-advance to the next unassigned polygon so a full sheet
                    // can be mapped with a quick sequence of clicks.
                    for (int next = app.importActivePolygon + 1; next < (int)app.project.polygons.size(); ++next) {
                        if (rowMap[next] < 0) { app.importActivePolygon = next; break; }
                    }
                }
                if (!clickable) ImGui::EndDisabled();

                ImGui::TableSetColumnIndex(1);
                if (assignedTo >= 0) {
                    std::string aname = app.project.polygons[assignedTo].meta.name;
                    if (aname.empty()) aname = "Polygon " + std::to_string(assignedTo + 1);
                    ImGui::TextColored(kAccent, "%s", aname.c_str());
                } else {
                    ImGui::TextDisabled("-");
                }

                for (int c = 0; c < (int)effective.headers.size(); ++c) {
                    ImGui::TableSetColumnIndex(c + 2);
                    if (c < (int)effective.rows[r].size()) ImGui::TextUnformatted(effective.rows[r][c].c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Apply Import", ImVec2(160, 0))) {
            int n = ApplyAllSheetMappings(app);
            std::string msg = sheetCount > 1
                ? ("Imported metadata from " + std::to_string(sheetCount) + " sheets into " + std::to_string(n) + " polygon(s).")
                : ("Imported metadata into " + std::to_string(n) + " polygon(s).");
            SetStatus(app, msg);
            app.showImportDialog = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) app.showImportDialog = false;
    }
    ImGui::End();
    if (!open) app.showImportDialog = false;
}

// ---------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------
static void ApplyTheme(AppState& app) {
    ImGuiStyle& style = ImGui::GetStyle();
    if (app.darkMode) ImGui::StyleColorsDark(); else ImGui::StyleColorsLight();

    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    // Clear, visible borders everywhere - the previous light theme relied on
    // near-invisible color-only contrast, which made control boundaries hard
    // to read. Explicit border widths + a solid border color fix that.
    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBarBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    if (app.darkMode) {
        colors[ImGuiCol_WindowBg]  = ImVec4(0.098f, 0.106f, 0.129f, 1.00f);
        colors[ImGuiCol_ChildBg]   = ImVec4(0.122f, 0.133f, 0.161f, 1.00f);
        colors[ImGuiCol_PopupBg]   = ImVec4(0.110f, 0.118f, 0.145f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.078f, 0.086f, 0.106f, 1.00f);
        colors[ImGuiCol_FrameBg]   = ImVec4(0.165f, 0.180f, 0.216f, 1.00f);
        colors[ImGuiCol_Border]    = ImVec4(0.30f, 0.33f, 0.40f, 1.00f);
        colors[ImGuiCol_Text]      = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    } else {
        colors[ImGuiCol_WindowBg]  = ImVec4(0.925f, 0.933f, 0.945f, 1.00f);
        colors[ImGuiCol_ChildBg]   = ImVec4(0.980f, 0.983f, 0.990f, 1.00f);
        colors[ImGuiCol_PopupBg]   = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.878f, 0.890f, 0.910f, 1.00f);
        colors[ImGuiCol_FrameBg]   = ImVec4(1.000f, 1.000f, 1.000f, 1.00f);
        colors[ImGuiCol_Border]    = ImVec4(0.60f, 0.64f, 0.70f, 1.00f);
        colors[ImGuiCol_Text]      = ImVec4(0.11f, 0.13f, 0.16f, 1.00f);
    }
    colors[ImGuiCol_Header]           = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.70f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.85f);
    colors[ImGuiCol_Button]           = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.60f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.85f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(kAccent.x, kAccent.y, kAccent.z, 1.00f);
    colors[ImGuiCol_CheckMark]        = kAccent;
    colors[ImGuiCol_SliderGrab]       = kAccent;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(kAccent.x * 0.8f, kAccent.y * 0.8f, kAccent.z * 0.8f, 1.0f);
    colors[ImGuiCol_Tab]              = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    colors[ImGuiCol_TabHovered]       = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.70f);
    colors[ImGuiCol_TabSelected]      = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.32f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.60f);
    colors[ImGuiCol_SeparatorActive]  = kAccent;
    colors[ImGuiCol_ResizeGripHovered]= ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.60f);
    colors[ImGuiCol_ResizeGripActive] = kAccent;
    colors[ImGuiCol_DockingPreview]   = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f);
}

// ---------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------
static void DrawToolbar(AppState& app) {
    if (!app.drawingPolygon) {
        bool disabled = !app.project.hasImage();
        if (disabled) ImGui::BeginDisabled();
        if (ImGui::Button("+ New Polygon")) {
            app.drawingPolygon = true;
            app.draftPolygon = Polygon();
            app.selectedPolygon = -1;
            app.selectedVertex = -1;
        }
        if (disabled) ImGui::EndDisabled();
    } else {
        ImVec4 warn = app.darkMode ? ImVec4(1.0f, 0.75f, 0.35f, 1.0f) : ImVec4(0.75f, 0.5f, 0.0f, 1.0f);
        ImGui::TextColored(warn, "Drawing polygon: click to add vertices (%d so far)", (int)app.draftPolygon.points.size());
        ImGui::SameLine();
        if (ImGui::Button("Finish (Enter)")) {
            if (app.draftPolygon.points.size() >= 3) {
                app.draftPolygon.id = app.project.nextId++;
                app.draftPolygon.meta.name = "Polygon " + std::to_string(app.project.polygons.size() + 1);
                app.project.polygons.push_back(app.draftPolygon);
                app.selectedPolygon = (int)app.project.polygons.size() - 1;
            }
            app.drawingPolygon = false;
            app.draftPolygon = Polygon();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel (Esc)")) {
            app.drawingPolygon = false;
            app.draftPolygon = Polygon();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("| Backspace: undo last point");
    }

    if (app.statusMsg[0] != '\0' && ImGui::GetTime() - app.statusMsgTime < 4.0) {
        ImGui::SameLine();
        ImVec4 okCol = app.darkMode ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(0.1f, 0.5f, 0.15f, 1.0f);
        ImGui::TextColored(okCol, "  %s", app.statusMsg);
    }

    DrawShortcutsPanel(app);
    DrawImportDialog(app);
}

// ---------------------------------------------------------------------
// Status bar (CAD-style coordinate/zoom readout)
// ---------------------------------------------------------------------
static void DrawStatusBar(AppState& app) {
    if (app.project.hasImage() && app.mouseOverCanvas) {
        ImGui::Text("X: %.1f  Y: %.1f px", app.lastMouseImagePos.x, app.lastMouseImagePos.y);
    } else {
        ImGui::TextDisabled("X: --  Y: --");
    }
    ImGui::SameLine(0, 24);
    ImGui::Text("Zoom: %.0f%%", app.zoom * 100.0f);
    ImGui::SameLine(0, 24);
    ImGui::Text("Polygons: %d", (int)app.project.polygons.size());
    if (app.selectedPolygon >= 0 && app.selectedPolygon < (int)app.project.polygons.size()) {
        ImGui::SameLine(0, 24);
        ImGui::Text("Selected: %s (%d vertices)", app.project.polygons[app.selectedPolygon].meta.name.c_str(),
                    (int)app.project.polygons[app.selectedPolygon].points.size());
    }
    float copyrightWidth = ImGui::CalcTextSize(kCopyrightText).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - copyrightWidth - 16 > 0 ? ImGui::GetWindowWidth() - copyrightWidth - 16 : 0);
    ImGui::TextDisabled("%s", kCopyrightText);
}

// ---------------------------------------------------------------------
// Menu bar (File actions)
// ---------------------------------------------------------------------
static void DrawMenuBar(AppState& app, GLFWwindow* window) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Image...")) {
                const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga"};
                const char* path = tinyfd_openFileDialog("Open Image (PNG, JPG, BMP, TGA)", "", 5, filters, "Image files", 0);
                if (path) app.pendingImageOpen = path; // actually loaded at the top of the next frame - see main()
            }
            if (ImGui::MenuItem("Close Image", nullptr, false, app.project.hasImage())) {
                CloseImage(app);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Project As...", nullptr, false, app.project.hasImage())) {
                const char* filters[] = {"*.polyproj"};
                const char* path = tinyfd_saveFileDialog("Save Project", "project.polyproj", 1, filters, "Polygon Annotator project (*.polyproj)");
                if (path) {
                    std::string fixedPath = EnsureExtension(path, ".polyproj");
                    std::string err;
                    if (SaveProjectJson(app.project, fixedPath, err)) SetStatus(app, "Project saved to " + fixedPath);
                    else SetStatus(app, "Save failed: " + err);
                }
            }
            if (ImGui::MenuItem("Open Project...")) {
                // Unlike the CSV/XLSX/image filters elsewhere in this menu, this
                // one IS applied. macOS's "choose file of type" is known to gray
                // out files for short/ambiguous extensions (confirmed earlier
                // for the plain 4-letter "json"), but a distinctive, unambiguous
                // extension like "polyproj" is much less likely to be misread as
                // a legacy 4-character file type code. As a safety net in case
                // it still misbehaves on some setup, the actual load (see
                // pendingProjectOpen handling below) also double-checks the
                // extension itself and gives a clear error either way.
                const char* filters[] = {"*.polyproj"};
                const char* path = tinyfd_openFileDialog("Open Project (.polyproj)", "", 1, filters, "Polygon Annotator project (*.polyproj)", 0);
                if (path) app.pendingProjectOpen = path; // actually loaded at the top of the next frame - see main()
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Metadata (CSV/XLSX)...", nullptr, false, app.project.hasImage())) {
                // No filter list here either, for the same reason as Open
                // Project above (avoids macOS graying out .csv/.xlsx files).
                const char* path = tinyfd_openFileDialog("Import Metadata (.csv or .xlsx)", "", 0, nullptr, nullptr, 0);
                if (path) {
                    ImportedWorkbook wb;
                    std::string err;
                    if (ImportWorkbookAuto(path, wb, err)) {
                        app.importWorkbook = std::move(wb);
                        app.importColumnsArePolygons = false;
                        app.importActiveSheet = 0;
                        int sheetCount = (int)app.importWorkbook.sheets.size();
                        app.importMatchColumnPerSheet.assign(sheetCount, 0);
                        app.importRowMapPerSheet.assign(sheetCount, std::vector<int>());
                        app.importActivePolygon = app.project.polygons.empty() ? -1 : 0;
                        AutoMapAllSheets(app);
                        app.showImportDialog = true;
                        SetStatus(app, "Loaded " + std::to_string(sheetCount) + " sheet(s) for import.");
                    } else {
                        SetStatus(app, "Import failed: " + err);
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export as HTML...", nullptr, false, app.project.hasImage())) {
                const char* filters[] = {"*.html"};
                const char* path = tinyfd_saveFileDialog("Export HTML", "annotations.html", 1, filters, "HTML file");
                if (path) {
                    std::string fixedPath = EnsureExtension(path, ".html");
                    std::string err;
                    if (ExportProjectToHtml(app.project, fixedPath, err)) SetStatus(app, "Exported HTML to " + fixedPath);
                    else SetStatus(app, "Export failed: " + err);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window, 1);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Fit to window", nullptr, false, app.project.hasImage())) {
                app.viewInitialized = false;
            }
            if (ImGui::MenuItem("Keyboard & Mouse Shortcuts")) {
                app.showShortcuts = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Dark Mode", nullptr, app.darkMode)) {
                app.darkMode = !app.darkMode;
                ApplyTheme(app);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
static void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1400, 900, "Polygon Annotator", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Standard cursors, swapped in based on what the user is doing (panning,
    // drawing, grabbing a vertex) - a small but very "CAD software" touch.
    GLFWcursor* cursorArrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
    GLFWcursor* cursorHand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    GLFWcursor* cursorCrosshair = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // A cleaner, more professional-looking typeface than ImGui's built-in
    // pixel font. Falls back to the default font silently if missing.
    ImFont* customFont = io.Fonts->AddFontFromFileTTF("Inter-Variable.ttf", 17.0f);
    if (!customFont) customFont = io.Fonts->AddFontDefault();

    AppState app;
    ApplyTheme(app);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    if (argc > 1) {
        OpenImageFile(app, argv[1]);
    }

    bool dockLayoutInitialized = false;

    while (!glfwWindowShouldClose(window)) {
        // Block until an input event arrives or ~60ms elapse, instead of
        // spinning the loop as fast as possible - an uncapped render loop
        // burns CPU/GPU continuously even when nothing on screen changes.
        // Dragging, typing, scrolling etc. all still wake it up immediately.
        glfwWaitEventsTimeout(0.06);

        // Handle anything queued by last frame's menu clicks now, before any
        // GL/ImGui calls for this frame have happened - see the comment on
        // these fields in AppState for why this has to be deferred rather
        // than done directly inside the menu item callback.
        if (!app.pendingImageOpen.empty()) {
            OpenImageFile(app, app.pendingImageOpen);
            app.pendingImageOpen.clear();
        }
        if (!app.pendingProjectOpen.empty()) {
            if (!HasExtension(app.pendingProjectOpen, ".polyproj")) {
                SetStatus(app, "Please select a .polyproj project file.");
            } else {
                std::string err;
                Project loaded;
                if (LoadProjectJson(loaded, app.pendingProjectOpen, err)) {
                    app.project = std::move(loaded);
                    ReuploadTexture(app);
                    app.viewInitialized = false;
                    app.selectedPolygon = -1;
                    app.selectedVertex = -1;
                    SetStatus(app, "Project loaded.");
                } else {
                    SetStatus(app, "Load failed: " + err);
                }
            }
            app.pendingProjectOpen.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.desiredCursor = AppState::CursorKind::Arrow;

        DrawMenuBar(app, window);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        float toolbarHeight = 42.0f;
        float statusBarHeight = 26.0f;

        // Fixed toolbar strip (not part of the dockspace).
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, toolbarHeight));
        ImGui::Begin("##ToolbarStrip", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking);
        DrawToolbar(app);
        ImGui::End();

        // Fixed status bar strip (not part of the dockspace).
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
        ImGui::Begin("##StatusBarStrip", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking);
        DrawStatusBar(app);
        ImGui::End();

        // Dockspace host, filling the space between the toolbar and status bar.
        // "Canvas" and "Inspector" are real dockable windows inside it - drag
        // their tabs to rearrange, split, float, or re-dock them like any CAD
        // or DAW-style tool.
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeight));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - toolbarHeight - statusBarHeight));
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##DockHost", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking);
        ImGui::PopStyleVar(3);

        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

        if (!dockLayoutInitialized) {
            dockLayoutInitialized = true;
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);
            ImGuiID dockMainId = dockspaceId;
            ImGuiID dockInspectorId = ImGui::DockBuilderSplitNode(dockMainId, ImGuiDir_Right, 0.24f, nullptr, &dockMainId);
            ImGui::DockBuilderDockWindow("Canvas", dockMainId);
            ImGui::DockBuilderDockWindow("Inspector", dockInspectorId);
            ImGui::DockBuilderFinish(dockspaceId);
        }
        ImGui::End(); // ##DockHost

        ImGui::PushFont(customFont);

        ImGui::Begin("Canvas", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawCanvasContent(app);
        ImGui::End();

        ImGui::Begin("Inspector");
        DrawInspectorContent(app);
        ImGui::End();

        ImGui::PopFont();

        // Apply the cursor shape decided while drawing the canvas this frame.
        switch (app.desiredCursor) {
            case AppState::CursorKind::Hand:      glfwSetCursor(window, cursorHand); break;
            case AppState::CursorKind::Crosshair: glfwSetCursor(window, cursorCrosshair); break;
            default:                              glfwSetCursor(window, cursorArrow); break;
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        ImVec4 clearCol = app.darkMode ? ImVec4(0.098f, 0.106f, 0.129f, 1.0f) : ImVec4(0.925f, 0.933f, 0.945f, 1.0f);
        glClearColor(clearCol.x, clearCol.y, clearCol.z, clearCol.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    glfwDestroyCursor(cursorArrow);
    glfwDestroyCursor(cursorHand);
    glfwDestroyCursor(cursorCrosshair);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
