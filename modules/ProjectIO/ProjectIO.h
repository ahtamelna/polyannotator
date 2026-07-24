#pragma once
#include <string>
#include "Model/Model.h"

// Saves the project (image path reference + polygons/metadata) as JSON.
// Pixel data itself is NOT embedded in the project file; the image is
// re-loaded from imagePath when the project is opened again.
bool SaveProjectJson(const Project& project, const std::string& path, std::string& errorOut);

// Loads a project JSON file. Re-loads the referenced image from disk into
// project.rgba/imageW/imageH. Returns false (with errorOut set) on failure.
bool LoadProjectJson(Project& project, const std::string& path, std::string& errorOut);
