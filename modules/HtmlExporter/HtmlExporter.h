#pragma once
#include <string>
#include "Model/Model.h"

// Writes a single self-contained HTML file to outPath that embeds the
// project's image (base64 PNG) and all polygons/metadata, with a JS
// viewer supporting pan/zoom and click-to-inspect on the polygons.
// Returns true on success.
bool ExportProjectToHtml(const Project& project, const std::string& outPath, std::string& errorOut);
