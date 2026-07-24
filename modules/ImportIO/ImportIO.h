#pragma once
#include <string>
#include <vector>

// A generic "spreadsheet" of strings: first row of `rows` is NOT special --
// headers are kept separately. Every row in `rows` is padded/truncated to
// headers.size() columns.
struct ImportedSheet {
    std::string name; // sheet/tab name (for CSV, just "Sheet1")
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

// A whole imported file. A .csv always has exactly one sheet; a .xlsx can
// have several - common when different sheets hold different metadata
// columns for the same set of polygons (e.g. one sheet with material info,
// another with inspection notes), which is why every sheet is kept rather
// than just the first.
struct ImportedWorkbook {
    std::vector<ImportedSheet> sheets;
};

// Reads a .csv file into a single-sheet ImportedWorkbook (first line = headers).
bool ImportCsv(const std::string& path, ImportedWorkbook& out, std::string& errorOut);

// Reads every worksheet of a .xlsx file into an ImportedWorkbook (first row
// of each sheet = headers). Handles shared strings, inline strings, and
// numeric/boolean cells.
bool ImportXlsx(const std::string& path, ImportedWorkbook& out, std::string& errorOut);

// Dispatches to ImportCsv or ImportXlsx based on the file extension.
bool ImportWorkbookAuto(const std::string& path, ImportedWorkbook& out, std::string& errorOut);
