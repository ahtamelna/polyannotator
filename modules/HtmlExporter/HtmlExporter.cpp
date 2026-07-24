#include "HtmlExporter.h"
#include "ImageLoader/ImageLoader.h"
#include "json.hpp"
#include <fstream>
#include <sstream>

using json = nlohmann::json;

static json MetaToJson(const Metadata& m) {
    json j;
    j["name"] = m.name;
    j["rawText"] = m.rawText;
    json rows = json::array();
    for (auto& r : m.table) {
        rows.push_back({{"key", r.key}, {"value", r.value}});
    }
    j["table"] = rows;
    return j;
}

static json ProjectToJson(const Project& p) {
    json root;
    root["imageW"] = p.imageW;
    root["imageH"] = p.imageH;
    json polys = json::array();
    for (auto& poly : p.polygons) {
        json jp = MetaToJson(poly.meta);
        jp["id"] = poly.id;
        jp["color"] = {poly.colorR, poly.colorG, poly.colorB};
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
    return root;
}

// The viewer is intentionally dependency-free (no external libs) so the
// exported HTML file works completely offline, double-click-to-open.
static const char* kHtmlTemplate = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Polygon Annotation Viewer</title>
<style>
  :root {
    --bg: #eef1f5;
    --panel: #ffffff;
    --panel-alt: #f2f5fa;
    --border: #d7dee8;
    --text: #232830;
    --text-dim: #6b7280;
    --accent: #2f6fed;
  }
  html[data-theme="dark"] {
    --bg: #171a21;
    --panel: #1e222b;
    --panel-alt: #262b35;
    --border: #383f4c;
    --text: #e6e9ef;
    --text-dim: #8b93a3;
    --accent: #5b9bff;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; padding: 0; height: 100%; background: var(--bg); color: var(--text);
               font-family: -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif; overflow: hidden; }
  #app { display: flex; flex-direction: column; width: 100vw; height: 100vh; }
  #main { display: flex; flex: 1 1 auto; min-height: 0; }
  #canvasWrap { position: relative; flex: 1 1 auto; overflow: hidden; cursor: grab; }
  #canvasWrap.dragging { cursor: grabbing; }
  canvas { display: block; background: repeating-conic-gradient(color-mix(in srgb, var(--bg) 85%, var(--text) 15%) 0% 25%, var(--bg) 0% 50%) 50% / 24px 24px; }
  #hint { position: absolute; left: 12px; bottom: 12px; font-size: 12px; color: var(--text-dim);
          background: var(--panel); opacity: 0.9; padding: 6px 10px; border-radius: 6px; pointer-events: none;
          border: 1px solid var(--border); }
  #themeToggle { position: absolute; right: 12px; top: 12px; font-size: 12px; color: var(--text);
                 background: var(--panel); padding: 6px 12px; border-radius: 6px; cursor: pointer;
                 border: 1px solid var(--border); }
  #themeToggle:hover { border-color: var(--accent); }
  #inspector { width: 360px; flex: 0 0 360px; background: var(--panel); border-left: 1px solid var(--border);
               padding: 16px; overflow-y: auto; }
  #inspector h2 { margin: 0 0 4px 0; font-size: 16px; color: var(--accent); }
  #inspector .sub { color: var(--text-dim); font-size: 12px; margin-bottom: 14px; }
  .section-title { font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em; color: var(--text-dim);
                    margin: 16px 0 6px 0; }
  table.meta { width: 100%; border-collapse: collapse; font-size: 13px; }
  table.meta td { border: 1px solid var(--border); padding: 5px 7px; vertical-align: top; }
  table.meta td.k { color: var(--accent); width: 40%; font-weight: 600; }
  pre.rawtext { white-space: pre-wrap; word-break: break-word; background: var(--panel-alt);
                border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-size: 12.5px;
                margin: 0; max-height: 180px; overflow-y: auto; }
  .vlist { list-style: none; margin: 0; padding: 0; }
  .vlist li { padding: 7px 8px; border: 1px solid var(--border); border-radius: 6px; margin-bottom: 6px;
              cursor: pointer; font-size: 13px; background: var(--panel-alt); }
  .vlist li:hover { border-color: var(--accent); }
  .vlist li.active { border-color: var(--accent); background: color-mix(in srgb, var(--accent) 15%, transparent); }
  details.vgroup { margin-top: 16px; }
  details.vgroup summary { cursor: pointer; font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em;
                            color: var(--text-dim); list-style: none; padding: 4px 0; user-select: none; }
  details.vgroup summary::-webkit-details-marker { display: none; }
  details.vgroup summary::before { content: '\25B8'; display: inline-block; width: 12px; margin-right: 4px;
                                     transition: transform 0.15s ease; }
  details.vgroup[open] summary::before { transform: rotate(90deg); }
  details.vgroup .vlist { margin-top: 8px; }
  .empty { color: var(--text-dim); font-size: 13px; margin-top: 40px; text-align: center; }
  .swatch { display: inline-block; width: 10px; height: 10px; border-radius: 2px; margin-right: 6px; }
  #backBtn { display:none; margin-bottom:10px; background: var(--panel-alt); color: var(--text); border: 1px solid var(--border);
             border-radius: 6px; padding: 5px 10px; cursor: pointer; font-size: 12px; }
  #backBtn:hover { border-color: var(--accent); }
  #footer { flex: 0 0 auto; padding: 5px 12px; font-size: 11px; color: var(--text-dim);
            background: var(--panel); border-top: 1px solid var(--border); text-align: right; }
</style>
</head>
<body>
<div id="app">
  <div id="main">
    <div id="canvasWrap">
      <canvas id="viewer"></canvas>
      <div id="hint">Scroll to zoom &middot; Drag to pan &middot; Click a polygon to inspect &middot; Click empty space or press Esc to deselect</div>
      <div id="themeToggle">&#9789; Dark mode</div>
    </div>
    <div id="inspector">
      <div id="inspectorContent"><div class="empty">Click a polygon to see its metadata</div></div>
    </div>
  </div>
  <div id="footer">Polygon Annotator &copy; 2026. All rights reserved.</div>
</div>
<script>
const PROJECT = __PROJECT_JSON__;
const IMAGE_DATA_URI = "data:image/png;base64,__IMAGE_BASE64__";

const canvas = document.getElementById('viewer');
const ctx = canvas.getContext('2d');
const wrap = document.getElementById('canvasWrap');
const inspectorContent = document.getElementById('inspectorContent');

let img = new Image();
let imgLoaded = false;
let scale = 1, offX = 0, offY = 0;
let dragging = false, dragMoved = false, lastX = 0, lastY = 0;
let selectedPolyId = null, selectedVertexId = null;

function resizeCanvas() {
  canvas.width = wrap.clientWidth;
  canvas.height = wrap.clientHeight;
  draw();
}
window.addEventListener('resize', resizeCanvas);

img.onload = function() {
  imgLoaded = true;
  fitToScreen();
  draw();
};
img.src = IMAGE_DATA_URI;

function fitToScreen() {
  const pad = 40;
  const availW = canvas.width - pad * 2;
  const availH = canvas.height - pad * 2;
  const sx = availW / PROJECT.imageW;
  const sy = availH / PROJECT.imageH;
  scale = Math.max(0.02, Math.min(sx, sy));
  offX = (canvas.width - PROJECT.imageW * scale) / 2;
  offY = (canvas.height - PROJECT.imageH * scale) / 2;
}

function worldToScreen(x, y) { return [x * scale + offX, y * scale + offY]; }
function screenToWorld(x, y) { return [(x - offX) / scale, (y - offY) / scale]; }

function pointInPolygon(px, py, pts) {
  let inside = false;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
    const xi = pts[i].x, yi = pts[i].y;
    const xj = pts[j].x, yj = pts[j].y;
    const intersect = ((yi > py) !== (yj > py)) &&
      (px < (xj - xi) * (py - yi) / (yj - yi) + xi);
    if (intersect) inside = !inside;
  }
  return inside;
}

function draw() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (imgLoaded) {
    ctx.drawImage(img, offX, offY, PROJECT.imageW * scale, PROJECT.imageH * scale);
  }
  for (const poly of PROJECT.polygons) {
    if (poly.points.length < 2) continue;
    const [r, g, b] = poly.color.map(c => Math.round(c * 255));
    const isSelected = poly.id === selectedPolyId;
    ctx.beginPath();
    poly.points.forEach((p, i) => {
      const [sx, sy] = worldToScreen(p.x, p.y);
      if (i === 0) ctx.moveTo(sx, sy); else ctx.lineTo(sx, sy);
    });
    ctx.closePath();
    ctx.fillStyle = `rgba(${r},${g},${b},${isSelected ? 0.35 : 0.18})`;
    ctx.fill();
    ctx.strokeStyle = `rgba(${r},${g},${b},1)`;
    ctx.lineWidth = isSelected ? 2.5 : 1.5;
    ctx.stroke();
    // Note: vertex dots are intentionally not drawn here - the exported
    // viewer shows clean polygon outlines only. Vertices are still listed
    // (and clickable) in the inspector panel on the right.

    if (poly.name) {
      let cx = 0, cy = 0;
      poly.points.forEach(p => { const [sx, sy] = worldToScreen(p.x, p.y); cx += sx; cy += sy; });
      cx /= poly.points.length; cy /= poly.points.length;
      ctx.font = '13px -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif';
      const textW = ctx.measureText(poly.name).width;
      ctx.fillStyle = 'rgba(20,22,26,0.72)';
      ctx.fillRect(cx - textW / 2 - 5, cy - 9, textW + 10, 18);
      ctx.fillStyle = '#ffffff';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(poly.name, cx, cy + 1);
      ctx.textAlign = 'start';
      ctx.textBaseline = 'alphabetic';
    }
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

function renderMetaTable(meta) {
  if (!meta.table || meta.table.length === 0) return '';
  let rows = meta.table.map(r => `<tr><td class="k">${escapeHtml(r.key)}</td><td>${escapeHtml(r.value)}</td></tr>`).join('');
  return `<div class="section-title">Table</div><table class="meta">${rows}</table>`;
}

function renderRawText(meta) {
  if (!meta.rawText) return '';
  return `<div class="section-title">Notes</div><pre class="rawtext">${escapeHtml(meta.rawText)}</pre>`;
}

function showPolygon(poly) {
  selectedPolyId = poly.id;
  selectedVertexId = null;
  renderInspector();
  draw();
}

function showVertex(poly, vertex) {
  selectedVertexId = vertex.id;
  renderInspector();
  draw();
}

function renderInspector() {
  const poly = PROJECT.polygons.find(p => p.id === selectedPolyId);
  if (!poly) {
    inspectorContent.innerHTML = '<div class="empty">Click a polygon to see its metadata</div>';
    return;
  }
  const vertex = poly.points.find(v => v.id === selectedVertexId);
  if (vertex) {
    const rgb = poly.color.map(c => Math.round(c*255));
    let html = `<button id="backBtn" style="display:inline-block">&larr; Back to polygon</button>`;
    html += `<h2>${escapeHtml(vertex.name || ('Point ' + vertex.id))}</h2>`;
    html += `<div class="sub"><span class="swatch" style="background:rgb(${rgb.join(',')})"></span>Vertex of "${escapeHtml(poly.name || 'Polygon')}" &middot; (${vertex.x.toFixed(1)}, ${vertex.y.toFixed(1)})</div>`;
    html += renderMetaTable(vertex);
    html += renderRawText(vertex);
    inspectorContent.innerHTML = html;
    document.getElementById('backBtn').onclick = () => { selectedVertexId = null; renderInspector(); draw(); };
    return;
  }

  const rgb = poly.color.map(c => Math.round(c*255));
  let html = `<h2>${escapeHtml(poly.name || ('Polygon ' + poly.id))}</h2>`;
  html += `<div class="sub"><span class="swatch" style="background:rgb(${rgb.join(',')})"></span>${poly.points.length} vertices</div>`;
  html += renderMetaTable(poly);
  html += renderRawText(poly);
  html += `<details class="vgroup"><summary>Vertices (${poly.points.length})</summary><ul class="vlist">`;
  poly.points.forEach((v, idx) => {
    html += `<li data-vid="${v.id}">${escapeHtml(v.name || ('Point ' + (idx+1)))} <span style="color:var(--text-dim); font-size:11px;">(${v.x.toFixed(0)}, ${v.y.toFixed(0)})</span></li>`;
  });
  html += `</ul></details>`;
  inspectorContent.innerHTML = html;
  inspectorContent.querySelectorAll('.vlist li').forEach(li => {
    li.onclick = () => {
      const vid = parseInt(li.getAttribute('data-vid'));
      const v = poly.points.find(p => p.id === vid);
      if (v) showVertex(poly, v);
    };
  });
}

function pickPolygonAt(sx, sy) {
  const [wx, wy] = screenToWorld(sx, sy);
  // iterate topmost (last-drawn) first
  for (let i = PROJECT.polygons.length - 1; i >= 0; i--) {
    const poly = PROJECT.polygons[i];
    if (poly.points.length >= 3 && pointInPolygon(wx, wy, poly.points)) return poly;
  }
  return null;
}

wrap.addEventListener('wheel', (e) => {
  e.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const mx = e.clientX - rect.left, my = e.clientY - rect.top;
  const [wx, wy] = screenToWorld(mx, my);
  const factor = Math.pow(1.0015, -e.deltaY);
  scale = Math.max(0.01, Math.min(40, scale * factor));
  offX = mx - wx * scale;
  offY = my - wy * scale;
  draw();
}, { passive: false });

wrap.addEventListener('mousedown', (e) => {
  dragging = true; dragMoved = false;
  lastX = e.clientX; lastY = e.clientY;
  wrap.classList.add('dragging');
});
window.addEventListener('mousemove', (e) => {
  if (!dragging) return;
  const dx = e.clientX - lastX, dy = e.clientY - lastY;
  if (Math.abs(dx) > 3 || Math.abs(dy) > 3) dragMoved = true;
  if (dragMoved) {
    offX += dx; offY += dy;
    lastX = e.clientX; lastY = e.clientY;
    draw();
  }
});
window.addEventListener('mouseup', (e) => {
  if (dragging && !dragMoved) {
    const rect = canvas.getBoundingClientRect();
    const sx = e.clientX - rect.left, sy = e.clientY - rect.top;
    const poly = pickPolygonAt(sx, sy);
    if (poly) showPolygon(poly);
    else deselectAll(); // clicked empty canvas space - deselect
  }
  dragging = false;
  wrap.classList.remove('dragging');
});

function deselectAll() {
  if (selectedPolyId === null && selectedVertexId === null) return;
  selectedPolyId = null;
  selectedVertexId = null;
  renderInspector();
  draw();
}

window.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') deselectAll();
});

// --- Light / dark theme toggle ---
const themeToggleBtn = document.getElementById('themeToggle');
function applyTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  themeToggleBtn.innerHTML = theme === 'dark' ? '&#9728; Light mode' : '&#9789; Dark mode';
  try { localStorage.setItem('polyAnnotatorTheme', theme); } catch (e) { /* ignore (e.g. file:// restrictions) */ }
  draw();
}
themeToggleBtn.addEventListener('click', () => {
  const current = document.documentElement.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
  applyTheme(current === 'dark' ? 'light' : 'dark');
});
let savedTheme = 'light';
try { savedTheme = localStorage.getItem('polyAnnotatorTheme') || 'light'; } catch (e) { /* ignore */ }
applyTheme(savedTheme);

resizeCanvas();
</script>
</body>
</html>
)HTML";

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

bool ExportProjectToHtml(const Project& project, const std::string& outPath, std::string& errorOut) {
    if (!project.hasImage()) {
        errorOut = "No image loaded in the project.";
        return false;
    }

    std::string base64Png = EncodePNGBase64(project.rgba.data(), project.imageW, project.imageH);
    json projectJson = ProjectToJson(project);
    std::string jsonStr = projectJson.dump();

    std::string html = kHtmlTemplate;
    ReplaceAll(html, "__PROJECT_JSON__", jsonStr);
    ReplaceAll(html, "__IMAGE_BASE64__", base64Png);

    std::ofstream out(outPath, std::ios::binary);
    if (!out.is_open()) {
        errorOut = "Could not open output file for writing: " + outPath;
        return false;
    }
    out << html;
    out.close();
    return true;
}
