#include "ImportIO.h"
#include "miniz.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

// =========================== CSV ==========================================

static std::vector<std::string> ParseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur.push_back('"'); ++i; }
                else inQuotes = false;
            } else cur.push_back(c);
        } else {
            if (c == '"') inQuotes = true;
            else if (c == ',') { fields.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
    }
    fields.push_back(cur);
    return fields;
}

bool ImportCsv(const std::string& path, ImportedWorkbook& out, std::string& errorOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        errorOut = "Could not open file: " + path;
        return false;
    }
    out = ImportedWorkbook();
    ImportedSheet sheet;
    sheet.name = "Sheet1";
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() && first) continue;
        std::vector<std::string> fields = ParseCsvLine(line);
        if (first) {
            sheet.headers = fields;
            first = false;
        } else {
            fields.resize(sheet.headers.size());
            sheet.rows.push_back(fields);
        }
    }
    if (sheet.headers.empty()) {
        errorOut = "CSV file appears to be empty.";
        return false;
    }
    out.sheets.push_back(std::move(sheet));
    return true;
}

// =========================== XLSX ==========================================

static std::string UnescapeXml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out.push_back('&'); i = semi; continue; }
                if (ent == "lt") { out.push_back('<'); i = semi; continue; }
                if (ent == "gt") { out.push_back('>'); i = semi; continue; }
                if (ent == "quot") { out.push_back('"'); i = semi; continue; }
                if (ent == "apos") { out.push_back('\''); i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    int code = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                        code = (int)strtol(ent.c_str() + 2, nullptr, 16);
                    else
                        code = (int)strtol(ent.c_str() + 1, nullptr, 10);
                    if (code > 0 && code < 128) { out.push_back((char)code); i = semi; continue; }
                }
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

static std::string GetAttr(const std::string& tag, const std::string& attr) {
    // tag is the raw text between < and > (or the whole opening tag string)
    std::string key = attr + "=\"";
    size_t pos = tag.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = tag.find('"', pos);
    if (end == std::string::npos) return "";
    return tag.substr(pos, end - pos);
}

static bool ReadZipEntryText(mz_zip_archive& zip, const std::string& name, std::string& out) {
    int idx = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
    if (idx < 0) return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &size, 0);
    if (!data) return false;
    out.assign((const char*)data, size);
    mz_free(data);
    return true;
}

static std::vector<std::string> ParseSharedStrings(const std::string& xml) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (true) {
        size_t siStart = xml.find("<si>", pos);
        size_t siStartAlt = xml.find("<si ", pos);
        if (siStartAlt != std::string::npos && (siStart == std::string::npos || siStartAlt < siStart)) siStart = siStartAlt;
        if (siStart == std::string::npos) break;
        size_t siOpenEnd = xml.find('>', siStart);
        size_t siEnd = xml.find("</si>", siOpenEnd);
        if (siEnd == std::string::npos) break;
        std::string block = xml.substr(siOpenEnd + 1, siEnd - siOpenEnd - 1);

        std::string text;
        size_t tpos = 0;
        while (true) {
            size_t tStart = block.find("<t", tpos);
            if (tStart == std::string::npos) break;
            size_t tOpenEnd = block.find('>', tStart);
            if (tOpenEnd == std::string::npos) break;
            if (block[tOpenEnd - 1] == '/') { tpos = tOpenEnd + 1; continue; } // self-closing <t/>
            size_t tEnd = block.find("</t>", tOpenEnd);
            if (tEnd == std::string::npos) break;
            text += UnescapeXml(block.substr(tOpenEnd + 1, tEnd - tOpenEnd - 1));
            tpos = tEnd + 4;
        }
        result.push_back(text);
        pos = siEnd + 5;
    }
    return result;
}

static int ColumnLettersToIndex(const std::string& ref) {
    int col = 0;
    for (char c : ref) {
        if (c >= 'A' && c <= 'Z') col = col * 26 + (c - 'A' + 1);
        else if (c >= 'a' && c <= 'z') col = col * 26 + (c - 'a' + 1);
        else break; // stop at first digit
    }
    return col - 1; // zero-based
}

static std::vector<std::vector<std::string>> ParseSheetGrid(const std::string& xml, const std::vector<std::string>& sharedStrings) {
    std::vector<std::vector<std::string>> grid;
    size_t maxCols = 0;
    size_t pos = 0;
    while (true) {
        size_t rowStart = xml.find("<row", pos);
        if (rowStart == std::string::npos) break;
        size_t rowOpenEnd = xml.find('>', rowStart);
        if (rowOpenEnd == std::string::npos) break;
        bool selfClosingRow = xml[rowOpenEnd - 1] == '/';
        size_t rowContentEnd = rowOpenEnd;
        std::string rowBlock;
        if (!selfClosingRow) {
            size_t rowEnd = xml.find("</row>", rowOpenEnd);
            if (rowEnd == std::string::npos) break;
            rowBlock = xml.substr(rowOpenEnd + 1, rowEnd - rowOpenEnd - 1);
            rowContentEnd = rowEnd + 6;
        } else {
            rowContentEnd = rowOpenEnd + 1;
        }
        pos = rowContentEnd;

        std::vector<std::string> rowVals;
        size_t cpos = 0;
        while (true) {
            size_t cStart = rowBlock.find("<c", cpos);
            if (cStart == std::string::npos) break;
            size_t cOpenEnd = rowBlock.find('>', cStart);
            if (cOpenEnd == std::string::npos) break;
            std::string cOpenTag = rowBlock.substr(cStart, cOpenEnd - cStart + 1);
            bool selfClosingCell = rowBlock[cOpenEnd - 1] == '/';
            std::string ref = GetAttr(cOpenTag, "r");
            std::string type = GetAttr(cOpenTag, "t");
            int colIdx = ref.empty() ? (int)rowVals.size() : ColumnLettersToIndex(ref);
            if (colIdx < 0) colIdx = (int)rowVals.size();

            std::string value;
            if (!selfClosingCell) {
                size_t cEnd = rowBlock.find("</c>", cOpenEnd);
                if (cEnd == std::string::npos) break;
                std::string cellBlock = rowBlock.substr(cOpenEnd + 1, cEnd - cOpenEnd - 1);
                cpos = cEnd + 4;

                if (type == "inlineStr") {
                    size_t tStart = cellBlock.find("<t");
                    if (tStart != std::string::npos) {
                        size_t tOpenEnd = cellBlock.find('>', tStart);
                        size_t tEnd = cellBlock.find("</t>", tOpenEnd);
                        if (tOpenEnd != std::string::npos && tEnd != std::string::npos)
                            value = UnescapeXml(cellBlock.substr(tOpenEnd + 1, tEnd - tOpenEnd - 1));
                    }
                } else {
                    size_t vStart = cellBlock.find("<v>");
                    size_t vLen = 3;
                    if (vStart == std::string::npos) { vStart = cellBlock.find("<v "); }
                    if (vStart != std::string::npos) {
                        size_t vOpenEnd = cellBlock.find('>', vStart);
                        size_t vEnd = cellBlock.find("</v>", vOpenEnd);
                        if (vOpenEnd != std::string::npos && vEnd != std::string::npos) {
                            std::string raw = cellBlock.substr(vOpenEnd + 1, vEnd - vOpenEnd - 1);
                            if (type == "s") {
                                int idx = atoi(raw.c_str());
                                if (idx >= 0 && idx < (int)sharedStrings.size()) value = sharedStrings[idx];
                            } else if (type == "b") {
                                value = (raw == "1") ? "TRUE" : "FALSE";
                            } else {
                                value = UnescapeXml(raw);
                            }
                        }
                    }
                    (void)vLen;
                }
            } else {
                cpos = cOpenEnd + 1;
            }

            if ((int)rowVals.size() <= colIdx) rowVals.resize(colIdx + 1);
            rowVals[colIdx] = value;
        }
        maxCols = std::max(maxCols, rowVals.size());
        grid.push_back(std::move(rowVals));
    }
    for (auto& r : grid) r.resize(maxCols);
    return grid;
}

// Resolves a workbook-relationship id (like "rId3") to its target path
// inside the zip (like "xl/worksheets/sheet2.xml"), using xl/_rels/workbook.xml.rels.
static std::string ResolveRelTarget(const std::string& relsXml, const std::string& rid) {
    if (rid.empty() || relsXml.empty()) return "";
    size_t relPos = 0;
    while (true) {
        size_t relStart = relsXml.find("<Relationship ", relPos);
        if (relStart == std::string::npos) break;
        size_t relEnd = relsXml.find('>', relStart);
        std::string relTag = relsXml.substr(relStart, relEnd - relStart + 1);
        relPos = relEnd + 1;
        if (GetAttr(relTag, "Id") == rid) {
            std::string target = GetAttr(relTag, "Target");
            if (target.empty()) return "";
            if (target.rfind("/xl/", 0) == 0) return target.substr(1);
            if (target.rfind("xl/", 0) == 0) return target;
            return "xl/" + target;
        }
    }
    return "";
}

// Parses one already-located worksheet XML blob into an ImportedSheet.
// Returns false (with the sheet left empty) if the sheet has no data rows -
// callers skip those rather than treating them as a hard error, since a
// workbook with e.g. one populated sheet and one blank placeholder sheet is
// completely normal.
static bool ParseOneSheet(const std::string& sheetXml, const std::string& sheetName,
                           const std::vector<std::string>& sharedStrings, ImportedSheet& out) {
    std::vector<std::vector<std::string>> grid = ParseSheetGrid(sheetXml, sharedStrings);
    if (grid.empty()) return false;

    out = ImportedSheet();
    out.name = sheetName;
    out.headers = grid[0];
    for (auto& h : out.headers) if (h.empty()) h = "(column)";
    for (size_t i = 1; i < grid.size(); ++i) {
        auto row = grid[i];
        row.resize(out.headers.size());
        bool allEmpty = true;
        for (auto& v : row) if (!v.empty()) { allEmpty = false; break; }
        if (!allEmpty) out.rows.push_back(std::move(row));
    }
    return true;
}

bool ImportXlsx(const std::string& path, ImportedWorkbook& out, std::string& errorOut) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        errorOut = "Could not open .xlsx (not a valid zip): " + path;
        return false;
    }

    std::string workbookXml, relsXml;
    if (!ReadZipEntryText(zip, "xl/workbook.xml", workbookXml)) {
        errorOut = "xl/workbook.xml not found - not a valid .xlsx file.";
        mz_zip_reader_end(&zip);
        return false;
    }
    ReadZipEntryText(zip, "xl/_rels/workbook.xml.rels", relsXml);

    std::vector<std::string> sharedStrings;
    std::string sharedStringsXml;
    if (ReadZipEntryText(zip, "xl/sharedStrings.xml", sharedStringsXml)) {
        sharedStrings = ParseSharedStrings(sharedStringsXml);
    }

    // A workbook can contain multiple sheets, each holding different
    // metadata columns for the same set of polygons (e.g. one sheet with
    // material info, another with inspection notes) - every <sheet> entry
    // in workbook.xml is parsed, not just the first.
    out = ImportedWorkbook();
    size_t pos = 0;
    int sheetNumberFallback = 1;
    while (true) {
        size_t sheetTagStart = workbookXml.find("<sheet ", pos);
        if (sheetTagStart == std::string::npos) break;
        size_t sheetTagEnd = workbookXml.find('>', sheetTagStart);
        if (sheetTagEnd == std::string::npos) break;
        std::string sheetTag = workbookXml.substr(sheetTagStart, sheetTagEnd - sheetTagStart + 1);
        pos = sheetTagEnd + 1;

        std::string sheetName = GetAttr(sheetTag, "name");
        std::string rid = GetAttr(sheetTag, "r:id");
        if (rid.empty()) rid = GetAttr(sheetTag, "id");
        std::string target = ResolveRelTarget(relsXml, rid);
        if (target.empty()) target = "xl/worksheets/sheet" + std::to_string(sheetNumberFallback) + ".xml";
        if (sheetName.empty()) sheetName = "Sheet" + std::to_string(sheetNumberFallback);
        sheetNumberFallback++;

        std::string sheetXml;
        if (!ReadZipEntryText(zip, target, sheetXml)) continue; // skip sheets we can't locate rather than failing the whole import

        ImportedSheet parsed;
        if (ParseOneSheet(sheetXml, sheetName, sharedStrings, parsed)) {
            out.sheets.push_back(std::move(parsed));
        }
    }

    mz_zip_reader_end(&zip);

    if (out.sheets.empty()) {
        errorOut = "No sheets with data were found in this workbook.";
        return false;
    }
    return true;
}

bool ImportWorkbookAuto(const std::string& path, ImportedWorkbook& out, std::string& errorOut) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".csv") {
        return ImportCsv(path, out, errorOut);
    }
    if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".xlsx") {
        return ImportXlsx(path, out, errorOut);
    }
    errorOut = "Unsupported file type (expected .csv or .xlsx).";
    return false;
}
