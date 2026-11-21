// Pearlescent Colors - AML mod untuk GTA SA Android v2.10 (arm64-v8a)
// Warna body utama (primary) mobil bergeser halus antara warna A dan B.
#include <mod/amlmod.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <jni.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <set>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>

MYMOD(net.pearl.gtasa, Pearlescent Colors, 1.0, Pearl)

static std::string g_logPath;
struct SimpleLogger {
    void SetTag(const char*) {}
    void Write(int prio, const char* tag, const char* f, va_list a) {
        char buf[512];
        vsnprintf(buf, sizeof buf, f, a);
        __android_log_print(prio, "Pearlescent", "%s", buf);
        if (!g_logPath.empty()) {
            FILE* fp = fopen(g_logPath.c_str(), "a");
            if (fp) { fprintf(fp, "[%s] %s\n", tag, buf); fclose(fp); }
        }
    }
    void Info(const char* f, ...) { va_list a; va_start(a, f); Write(ANDROID_LOG_INFO, "INFO", f, a); va_end(a); }
    void Error(const char* f, ...) { va_list a; va_start(a, f); Write(ANDROID_LOG_ERROR, "ERROR", f, a); va_end(a); }
};
static SimpleLogger g_logger;
#define logger (&g_logger)

struct RGB { int r, g, b; };
struct Template { std::string name; RGB a{255,255,255}, b{255,255,255}, c{255,255,255}; bool hasC = false; float intensity = 1.0f; };
struct RwRGBA { unsigned char r, g, b, a; };

// ---- state ----
static std::map<std::string, Template> g_templates;
static Template g_tpl;
static float g_speed = 0.6f, g_intensity = 1.0f;
static std::string g_iniPath;
static time_t g_mtime = 0;
static double g_lastCheck = 0;
static bool g_ready = false;

// ---- RenderWare / game functions (diambil dari ekspor libGTASA.so) ----
typedef void* (*RpAtomicCB)(void*, void*);
typedef void* (*RpMaterialCB)(void*, void*);
typedef void  (*SetEditableMaterials_t)(void*);
static void* (*RpClumpForAllAtomics)(void*, RpAtomicCB, void*);
static void* (*RpGeometryForAllMaterials)(void*, RpMaterialCB, void*);
static void* (*RpAtomicGetGeometry_fn)(void*) = nullptr;
static SetEditableMaterials_t SetEditableMaterials_orig = nullptr;
struct PrimMat { void* mat; void* frame; };
static std::vector<PrimMat> g_primary;
static void* g_curFrame = nullptr;
static float g_partSpread = 2.5f;

// ---- pilihan per-mobil ----
static void* (*GetClumpModelInfo)(void*) = nullptr;
static void* (*GetModelInfoByName)(const char*, int*) = nullptr;
static void** g_modelInfoPtrs = nullptr;               // array 20000 pointer CModelInfo
static bool g_whitelist = false;                       // true = hanya mobil di [cars]
static std::map<std::string, std::string> g_carsCfg;   // "buffalo"/"402" -> nama template
static std::map<int, std::string> g_carById;
static std::map<void*, int> g_miIdCache;
static bool g_needResolve = true;
static std::set<int> g_loggedIds;
static int g_geomOff = (sizeof(void*) == 8) ? 48 : 24;  // RpAtomic: object, repEntry, geometry
static int g_matSeen = 0;

// ---- efek berdasarkan sudut kamera-mobil ----
static float* (*RwFrameGetLTM_fn)(void*) = nullptr;
static void** g_scene = nullptr;     // struct Scene { RpWorld*, RwCamera* }
static int g_shiftMode = 0;          // 0 = angle, 1 = time, 2 = both
static float g_angleGain = 1.0f, g_elevGain = 1.5f;

// ---- mode pixel (patch shader kendaraan) ----
static bool  g_shaderOK = false;          // hook glShaderSource terpasang
static bool  g_shaderPatchEnabled = true; // ini: shader_patch
static float g_chamGain = 1.6f, g_chamFlow = 0.25f, g_chamBias = 0.0f;
static float g_chamReflect = 0.22f, g_chamFresnel = 0.5f, g_chamDepth = 0.25f, g_chamFlake = 0.08f, g_chamBright = 0.9f;
static std::vector<Template> g_palette;   // dibekukan saat game mulai (maks 32)
static int g_shaderPatched = 0;
static unsigned g_uniVersion = 1;         // naik setiap parameter shader live berubah

// ---- menu in-game ----
static bool  g_menuEnabled = true;        // ini: menu
static bool  g_menuOpen = false, g_menuReady = false, g_menuFailed = false;
static float g_menuScale = 0.0f;          // 0 = otomatis
static float g_btnX = 0.44f, g_btnY = 0.02f;
static bool  g_enabled = true;            // master on/off
struct UiRect { float x0, y0, x1, y1; bool valid; };
static UiRect g_uiRects[3] = { {0, 0, 0, 0, false}, {0, 0, 0, 0, false}, {0, 0, 0, 0, false} };
static bool  g_autoSave = true;
static float g_btnAlpha = 1.0f, g_panelAlpha = 0.97f;   // transparansi tombol & panel menu
struct PresetSlot { bool used = false; RGB a{0, 0, 0}, b{0, 0, 0}, c{0, 0, 0}; };
static PresetSlot g_presets[6];
static uint64_t g_hashVal = 0;
static bool g_hashInit = false, g_dirty = false;
static double g_dirtyT = 0;
static bool g_latch[2] = {false, false}, g_prevDown[2] = {false, false};
static int  (*GetButton_orig)(int, int) = nullptr;
static int  (*GetCoords_orig)(int, int*, int*, float*) = nullptr;
static void (*SwapBuffers_orig)() = nullptr;
static void* (*FindPlayerVehicle_fn)(int, bool) = nullptr;
static void (*glShaderSource_orig)(unsigned, int, const char* const*, const int*) = nullptr;

static int PaletteIndex(const std::string& name) {
    for (size_t i = 0; i < g_palette.size(); i++) if (g_palette[i].name == name) return (int)i;
    return 0;
}

static const char* kDefaultIni = R"(; Pearlescent Colors (dibuat otomatis)
[settings]
mode=all
active=Purple-Gold
shift_mode=angle
angle_gain=2.0
elev_gain=1.5
part_spread=2.5
speed=0.5
intensity=1.0

[cars]
;buffalo=Purple-Gold

[custom]
a=40,120,255
b=255,60,200
intensity=1.0

[template:Purple-Gold]
a=120,40,200
b=230,180,60
[template:Blue-Green]
a=20,80,220
b=20,220,150
[template:Red-Orange]
a=200,20,40
b=255,140,20
[template:Sunset]
a=255,80,60
b=160,60,220
)";

static double NowSec() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static bool ParseRGB(const std::string& s, RGB& o) {
    int r, g, b;
    if (sscanf(s.c_str(), "%d , %d , %d", &r, &g, &b) != 3) return false;
    o = { Clamp255(r), Clamp255(g), Clamp255(b) };
    return true;
}

static void LoadConfig() {
    FILE* f = fopen(g_iniPath.c_str(), "r");
    if (!f) {
        logger->Error("pearlescent.ini tidak ditemukan di %s, membuat file default", g_iniPath.c_str());
        FILE* w = fopen(g_iniPath.c_str(), "w");
        if (w) { fputs(kDefaultIni, w); fclose(w); }
        f = fopen(g_iniPath.c_str(), "r");
        if (!f) { logger->Error("Gagal membuat/membaca ini"); return; }
    }
    std::map<std::string, Template> t;
    std::string active = "Purple-Gold", sec;
    float speed = 0.6f, inten = 1.0f;
    bool whitelist = false;
    int geomOff = g_geomOff;
    int shiftMode = g_shiftMode;
    float angleGain = g_angleGain, elevGain = g_elevGain, partSpread = g_partSpread;
    float chamGain = g_chamGain, chamFlow = g_chamFlow, chamBias = g_chamBias;
    float chamReflect = g_chamReflect, chamFresnel = g_chamFresnel, chamDepth = g_chamDepth, chamFlake = g_chamFlake, chamBright = g_chamBright;
    bool shaderPatch = g_shaderPatchEnabled, menuOn = g_menuEnabled, enabledV = g_enabled, autoSaveV = g_autoSave;
    PresetSlot pre[6];
    PresetSlot* curPre = nullptr;
    float menuScale = g_menuScale, btnX = g_btnX, btnY = g_btnY, btnAlpha = g_btnAlpha, panelAlpha = g_panelAlpha;
    std::map<std::string, std::string> cars;
    Template* cur = nullptr;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            size_t e = s.find(']');
            sec = s.substr(1, e == std::string::npos ? std::string::npos : e - 1);
            cur = nullptr;
            curPre = nullptr;
            if (sec.rfind("preset:", 0) == 0) { int pn = atoi(sec.c_str() + 7); if (pn >= 1 && pn <= 6) curPre = &pre[pn - 1]; }
            std::string name;
            if (sec.rfind("template:", 0) == 0) name = Trim(sec.substr(9));
            else if (sec == "custom") name = "custom";
            if (!name.empty()) { cur = &t[name]; cur->name = name; }
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(s.substr(0, eq)), v = Trim(s.substr(eq + 1));
        if (sec == "settings") {
            if (k == "active") active = v;
            else if (k == "speed") speed = (float)atof(v.c_str());
            else if (k == "intensity") inten = (float)atof(v.c_str());
            else if (k == "mode") whitelist = (v == "whitelist");
            else if (k == "geom_offset") geomOff = atoi(v.c_str());
            else if (k == "shift_mode") shiftMode = (v == "time") ? 1 : (v == "both" ? 2 : (v == "pixel" ? 3 : 0));
            else if (k == "angle_gain") angleGain = (float)atof(v.c_str());
            else if (k == "elev_gain") elevGain = (float)atof(v.c_str());
            else if (k == "part_spread") partSpread = (float)atof(v.c_str());
            else if (k == "cham_gain") chamGain = (float)atof(v.c_str());
            else if (k == "cham_flow") chamFlow = (float)atof(v.c_str());
            else if (k == "cham_bias") chamBias = (float)atof(v.c_str());
            else if (k == "cham_reflect") chamReflect = (float)atof(v.c_str());
            else if (k == "cham_fresnel") chamFresnel = (float)atof(v.c_str());
            else if (k == "cham_depth") chamDepth = (float)atof(v.c_str());
            else if (k == "cham_flake") chamFlake = (float)atof(v.c_str());
            else if (k == "cham_brightness") chamBright = (float)atof(v.c_str());
            else if (k == "shader_patch") shaderPatch = (atoi(v.c_str()) != 0);
            else if (k == "menu") menuOn = (atoi(v.c_str()) != 0);
            else if (k == "enabled") enabledV = (atoi(v.c_str()) != 0);
            else if (k == "autosave") autoSaveV = (atoi(v.c_str()) != 0);
            else if (k == "menu_scale") menuScale = (float)atof(v.c_str());
            else if (k == "menu_button_x") btnX = (float)atof(v.c_str());
            else if (k == "menu_button_y") btnY = (float)atof(v.c_str());
            else if (k == "menu_button_alpha") btnAlpha = (float)atof(v.c_str());
            else if (k == "menu_panel_alpha") panelAlpha = (float)atof(v.c_str());
        } else if (sec == "cars") {
            cars[k] = v;
        } else if (curPre) {
            if (k == "a") { ParseRGB(v, curPre->a); curPre->used = true; }
            else if (k == "b") ParseRGB(v, curPre->b);
            else if (k == "c") ParseRGB(v, curPre->c);
        } else if (cur) {
            if (k == "a") ParseRGB(v, cur->a);
            else if (k == "b") ParseRGB(v, cur->b);
            else if (k == "c") { if (ParseRGB(v, cur->c)) cur->hasC = true; }
            else if (k == "intensity") cur->intensity = (float)atof(v.c_str());
        }
    }
    fclose(f);
    if (t.empty()) return;
    g_templates = t;
    auto it = g_templates.find(active);
    g_tpl = (it != g_templates.end()) ? it->second : g_templates.begin()->second;
    g_speed = speed; g_intensity = inten;
    g_geomOff = geomOff;
    g_shiftMode = shiftMode; g_angleGain = angleGain; g_elevGain = elevGain; g_partSpread = partSpread;
    g_chamGain = chamGain; g_chamFlow = chamFlow; g_chamBias = chamBias;
    g_chamReflect = chamReflect; g_chamFresnel = chamFresnel; g_chamDepth = chamDepth; g_chamFlake = chamFlake; g_chamBright = chamBright; g_shaderPatchEnabled = shaderPatch;
    g_menuEnabled = menuOn; g_menuScale = menuScale; g_btnX = btnX; g_btnY = btnY;
    g_btnAlpha = btnAlpha < 0.1f ? 0.1f : (btnAlpha > 1.0f ? 1.0f : btnAlpha);
    g_panelAlpha = panelAlpha < 0.4f ? 0.4f : (panelAlpha > 1.0f ? 1.0f : panelAlpha);
    g_enabled = enabledV; g_autoSave = autoSaveV;
    for (int i = 0; i < 6; i++) g_presets[i] = pre[i];
    g_hashInit = false;
    g_uniVersion++;
    g_whitelist = whitelist; g_carsCfg = cars; g_carById.clear(); g_needResolve = true;
    struct stat st; if (stat(g_iniPath.c_str(), &st) == 0) g_mtime = st.st_mtime;
    logger->Info("Config dimuat. Template aktif: %s", g_tpl.name.c_str());
}

static void MaybeReload() {
    double now = NowSec();
    if (now - g_lastCheck < 1.0) return;
    g_lastCheck = now;
    struct stat st;
    if (stat(g_iniPath.c_str(), &st) == 0 && st.st_mtime != g_mtime) LoadConfig();
}

static bool ViewAngles(void* clump, void* partFrame, float& az, float& el) {
    if (!RwFrameGetLTM_fn || !g_scene || !clump) return false;
    void* cam = g_scene[1];
    if (!cam) return false;
    void* vf = *(void**)((char*)clump + sizeof(void*));   // RwObject.parent = frame
    void* cf = *(void**)((char*)cam + sizeof(void*));
    if (!vf || !cf) return false;
    const float* v = RwFrameGetLTM_fn(vf);
    const float* c = RwFrameGetLTM_fn(cf);
    if (!v || !c) return false;
    const float* pp = v;                                   // posisi bagian (atau mobil)
    if (partFrame) { const float* q = RwFrameGetLTM_fn(partFrame); if (q) pp = q; }
    float dx = c[12] - pp[12], dy = c[13] - pp[13], dz = c[14] - pp[14];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.001f) return false;
    dx /= len; dy /= len; dz /= len;
    float x = dx * v[0] + dy * v[1] + dz * v[2];     // sumbu kanan mobil
    float z = dx * v[4] + dy * v[5] + dz * v[6];     // sumbu atas mobil
    float y = dx * v[8] + dy * v[9] + dz * v[10];    // sumbu depan mobil
    az = atan2f(x, y);                               // 0 = kamera di depan mobil
    el = asinf(z < -1 ? -1 : (z > 1 ? 1 : z));       // sudut elevasi kamera
    return true;
}

static RGB Ramp(const Template& tp, float k) {
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    const RGB *p, *q; float u;
    if (tp.hasC) {
        if (k < 0.5f) { p = &tp.a; q = &tp.b; u = k * 2.0f; }
        else          { p = &tp.b; q = &tp.c; u = (k - 0.5f) * 2.0f; }
    } else { p = &tp.a; q = &tp.b; u = k; }
    return { Clamp255((int)(p->r + (q->r - p->r) * u)),
             Clamp255((int)(p->g + (q->g - p->g) * u)),
             Clamp255((int)(p->b + (q->b - p->b) * u)) };
}

// Warna untuk satu bagian body (partFrame) pada kendaraan clump
static RGB Pearl(void* clump, void* partFrame, const Template& tp) {
    float az0 = 0, el0 = 0, arg = 0;
    int mode = (g_shiftMode == 3) ? 0 : g_shiftMode;
    bool ok = (mode != 1) && ViewAngles(clump, nullptr, az0, el0);
    if (ok) {
        float az1 = az0, el1 = el0;
        if (partFrame) ViewAngles(clump, partFrame, az1, el1);
        float dAz = az1 - az0;
        if (dAz > 3.14159265f) dAz -= 6.2831853f;
        if (dAz < -3.14159265f) dAz += 6.2831853f;
        float az = az0 + dAz * g_partSpread, el = el0 + (el1 - el0) * g_partSpread;
        arg += az * g_angleGain + el * g_elevGain;
    }
    if (mode != 0 || !ok) {
        float phase = (float)(((uintptr_t)clump >> 4) % 1000) / 1000.0f * 6.2831853f;
        arg += (float)(NowSec() * g_speed * 6.2831853) + phase;
    }
    float t = 0.5f + 0.5f * sinf(arg);
    static int logged = 0;
    if (logged < 8) { logged++; logger->Info("pearl: angle_ok=%d az0=%.2f el0=%.2f arg=%.2f t=%.2f bagian=%p", (int)ok, az0, el0, arg, t, partFrame); }
    return Ramp(tp, t * tp.intensity * g_intensity);
}

static void* OnMaterial(void* mat, void*) {
    RwRGBA* c = (RwRGBA*)((char*)mat + sizeof(void*)); // RpMaterial: texture* lalu color
    g_matSeen++;
    if (c->r == 60 && c->g == 255 && c->b == 0) g_primary.push_back({mat, g_curFrame});
    return mat;
}
static void* OnAtomic(void* atomic, void*) {
    void* geom = RpAtomicGetGeometry_fn ? RpAtomicGetGeometry_fn(atomic)
               : *(void**)((char*)atomic + g_geomOff);
    g_curFrame = *(void**)((char*)atomic + sizeof(void*));   // frame bagian body ini
    if (geom) RpGeometryForAllMaterials(geom, OnMaterial, nullptr);
    return atomic;
}

static int ModelIdOf(void* clump) {
    if (!GetClumpModelInfo || !g_modelInfoPtrs) return -1;
    void* mi = GetClumpModelInfo(clump);
    if (!mi) return -1;
    auto it = g_miIdCache.find(mi);
    if (it != g_miIdCache.end()) return it->second;
    int id = -1;
    for (int i = 0; i < 20000; i++) if (g_modelInfoPtrs[i] == mi) { id = i; break; }
    g_miIdCache[mi] = id;
    return id;
}

static void ResolveCars() {
    g_carById.clear();
    for (auto& kv : g_carsCfg) {
        const std::string& k = kv.first;
        int id = -1;
        if (!k.empty() && isdigit((unsigned char)k[0])) id = atoi(k.c_str());
        else if (GetModelInfoByName) { int out = -1; if (GetModelInfoByName(k.c_str(), &out) && out >= 0) id = out; }
        if (id >= 0) g_carById[id] = kv.second;
        else logger->Error("Mobil tidak ditemukan: %s", k.c_str());
    }
    g_needResolve = false;
}

static void SetEditableMaterials_hook(void* clump) {
    g_primary.clear();
    g_matSeen = 0;
    if (g_ready && clump) { MaybeReload(); RpClumpForAllAtomics(clump, OnAtomic, nullptr); }
    SetEditableMaterials_orig(clump);
    if (g_ready && clump && g_loggedIds.size() < 40) {
        int mid = ModelIdOf(clump);
        if (g_loggedIds.insert(mid).second)
            logger->Info("kendaraan model id=%d, material dilihat=%d, primary ditemukan=%d, whitelist=%d, geom_offset=%d", mid, g_matSeen, (int)g_primary.size(), (int)g_whitelist, g_geomOff);
    }
    if (g_enabled && !g_primary.empty()) {
        if (g_needResolve) ResolveCars();
        const Template* tp = &g_tpl;
        auto car = g_carById.find(ModelIdOf(clump));
        if (car != g_carById.end()) {
            auto tt = g_templates.find(car->second);
            if (tt != g_templates.end()) tp = &tt->second;
        } else if (g_whitelist) {
            return;  // mobil lain tidak diubah
        }
        if (g_shiftMode == 3 && g_shaderOK) {
            // tandai material primary; shader menghitung warna per piksel
            int idx = PaletteIndex(tp->name);
            for (const PrimMat& pm : g_primary) {
                RwRGBA* c = (RwRGBA*)((char*)pm.mat + sizeof(void*));
                c->r = 1; c->g = 253; c->b = (unsigned char)(idx * 8);
            }
            return;
        }
        void* lastFrame = (void*)1; RGB p{0, 0, 0};
        for (const PrimMat& pm : g_primary) {
            if (pm.frame != lastFrame) { p = Pearl(clump, pm.frame, *tp); lastFrame = pm.frame; }
            RwRGBA* c = (RwRGBA*)((char*)pm.mat + sizeof(void*));
            c->r = p.r; c->g = p.g; c->b = p.b;
        }
    }
}

// ================= patch shader (efek per piksel) =================
static std::string Fl(float v) { char b[32]; snprintf(b, sizeof b, "%.4f", v); return b; }
static std::string V3(const RGB& c) { return "vec3(" + Fl(c.r / 255.0f) + "," + Fl(c.g / 255.0f) + "," + Fl(c.b / 255.0f) + ")"; }

static std::string BuildPaletteFn() {
    std::string s = "lowp vec3 ChamPal(float idx, float t) {\n"
                    " mediump float u1 = smoothstep(0.0, 1.0, clamp(t * 2.0, 0.0, 1.0));\n"
                    " mediump float u2 = smoothstep(0.0, 1.0, clamp(t * 2.0 - 1.0, 0.0, 1.0));\n"
                    " mediump float us = smoothstep(0.0, 1.0, t);\n";
    for (size_t i = 0; i < g_palette.size(); i++) {
        const Template& tp = g_palette[i];
        s += " if (idx < " + Fl((float)i + 0.5f) + ") return ";
        if (tp.name == "custom") s += "mix(mix(ChamA,ChamB,u1),ChamC,u2);\n";
        else if (tp.hasC) s += "mix(mix(" + V3(tp.a) + "," + V3(tp.b) + ",u1)," + V3(tp.c) + ",u2);\n";
        else         s += "mix(" + V3(tp.a) + "," + V3(tp.b) + ",us);\n";
    }
    s += " return vec3(1.0);\n}\n";
    return s;
}

static void ReplaceAll(std::string& s, const std::string& a, const std::string& b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}

static bool PatchShader(std::string& src) {
    const size_t np = std::string::npos;
    const std::string varyings = "varying mediump float Out_Cham;\nvarying mediump vec3 Out_WN;\nvarying mediump vec3 Out_VV;\n";
    const std::string uniforms = "uniform mediump vec4 ChamP0;\nuniform mediump vec4 ChamP1;\nuniform mediump vec3 ChamA;\nuniform mediump vec3 ChamB;\nuniform mediump vec3 ChamC;\n";
    size_t m = src.find("void main()");
    if (m == np) return false;

    if (src.find("gl_Position") != np) {                       // vertex shader
        const std::string anchor = "vec3 reflVector = normalize(WorldPos.xyz - CameraPosition.xyz);";
        size_t a = src.find(anchor);
        if (a == np || src.find("WorldNormal") == np) return false;
        std::string expr = "0.0";
        if (src.find("uniform lowp vec4 MaterialDiffuse") != np)
            expr = "(abs(MaterialDiffuse.x * 255.0 - 1.0) < 1.5 && abs(MaterialDiffuse.y * 255.0 - 253.0) < 1.5) ? floor(MaterialDiffuse.z * 255.0 / 8.0 + 0.5) + 1.0 : 0.0";
        src.replace(a, anchor.size(), anchor + "\nOut_WN = WorldNormal; Out_VV = reflVector; Out_Cham = " + expr + ";\n");
        src.insert(m, varyings);
        return true;
    }
    if (src.find("gl_FragColor") != np) {                      // fragment shader
        if (src.find("EnvMapCoefficient") == np || src.find("Out_Color") == np) return false;
        size_t br = src.find('{', m);
        if (br == np) return false;
        std::string head = src.substr(0, m), mid = src.substr(m, br + 1 - m), body = src.substr(br + 1);
        ReplaceAll(body, "Out_Color", "CCol");
        ReplaceAll(body, "EnvMapCoefficient", "CEnv");
        std::string block =
            "lowp vec4 CCol = Out_Color;\n"
            "lowp float CEnv = EnvMapCoefficient;\n"
            "if (Out_Cham > 0.5) {\n"
            " mediump vec3 cN = normalize(Out_WN);\n"
            " mediump vec3 cV = normalize(Out_VV);\n"
            " mediump float cF = 1.0 - abs(dot(cV, cN));\n"
            " mediump vec3 cR = cV - 2.0 * dot(cV, cN) * cN;\n"
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            " highp float cG = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
            "#else\n"
            " mediump float cG = 0.5;\n"
            "#endif\n"
            " mediump float cT = clamp(cF * ChamP0.x + cR.z * ChamP0.y + ChamP0.z + (cG - 0.5) * ChamP1.z * 0.6, 0.0, 1.0);\n"
            " mediump float cL = clamp(Out_Color.y * 1.008, 0.0, 1.0);\n"
            " mediump float cS = mix(1.0 - ChamP0.w, 1.0, cT);\n"
            " CCol.xyz = ChamPal(Out_Cham - 1.0, cT) * (cL * cS * ChamP1.w) * (1.0 + (cG - 0.5) * ChamP1.z);\n"
            " CEnv = clamp(max(EnvMapCoefficient, ChamP1.x) + cF * cF * ChamP1.y, 0.0, 0.7);\n"
            "}\n";
        src = head + varyings + uniforms + BuildPaletteFn() + mid + "\n" + block + body;
        return true;
    }
    return false;
}

static void glShaderSource_hook(unsigned shader, int count, const char* const* str, const int* len) {
    if (count >= 1 && str) {
        std::string src;
        for (int i = 0; i < count; i++) {
            if (!str[i]) continue;
            if (len && len[i] >= 0) src.append(str[i], (size_t)len[i]); else src.append(str[i]);
        }
        if (PatchShader(src)) {
            g_shaderPatched++;
            if (g_shaderPatched <= 4) {
                std::string dump = g_logPath.substr(0, g_logPath.find_last_of('/') + 1) + "pearlescent_shaders.txt";
                FILE* fp = fopen(dump.c_str(), g_shaderPatched == 1 ? "w" : "a");
                if (fp) { fprintf(fp, "===== shader #%d =====\n%s\n", g_shaderPatched, src.c_str()); fclose(fp); }
            }
            logger->Info("Shader dipatch (#%d)", g_shaderPatched);
            const char* ptr = src.c_str();
            glShaderSource_orig(shader, 1, &ptr, nullptr);
            return;
        }
    }
    glShaderSource_orig(shader, count, str, len);
}

// ---------- hook GOT generik (libGTASA v2.10 arm64) ----------
// expectOff != 0: fungsi milik libGTASA sendiri (slot harus berisi base+expectOff)
// expectOff == 0: fungsi import dari libGLESv2 (slot harus terisi dan bukan milik libGTASA)
static bool InstallGotHook(void* anySym, uintptr_t slotOff, uintptr_t expectOff, void* hook, void** orig, const char* name) {
    Dl_info di;
    if (!anySym || !dladdr(anySym, &di) || !di.dli_fbase) { logger->Error("dladdr gagal (%s)", name); return false; }
    uintptr_t base = (uintptr_t)di.dli_fbase;
    void** pslot = (void**)(base + slotOff);
    void* cur = *pslot;
    bool inSelf = ((uintptr_t)cur >= base && (uintptr_t)cur < base + 0x1200000);
    bool valid = expectOff ? ((uintptr_t)cur == base + expectOff) : (cur && !inSelf);
    if (!valid) { logger->Error("Slot GOT %s tidak valid (cur=%p)", name, cur); return false; }
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t pg = (base + slotOff) & ~((uintptr_t)ps - 1);
    if (mprotect((void*)pg, (size_t)ps, PROT_READ | PROT_WRITE) != 0) { logger->Error("mprotect gagal (%s)", name); return false; }
    *orig = cur;
    *pslot = hook;
    mprotect((void*)pg, (size_t)ps, PROT_READ);
    logger->Info("Hook %s terpasang", name);
    return true;
}

// ---------- uniform live untuk shader chameleon ----------
static void (*glUseProgram_orig)(unsigned) = nullptr;
static void (*glLinkProgram_orig)(unsigned) = nullptr;
struct ProgLocs { int p0 = -1, p1 = -1, a = -1, b = -1, c = -1; unsigned ver = 0; bool known = false; };
static std::map<unsigned, ProgLocs> g_progs;

static void SetChamUniforms(const ProgLocs& L) {
    glUniform4f(L.p0, g_chamGain, g_chamFlow, g_chamBias, g_chamDepth);
    if (L.p1 >= 0) glUniform4f(L.p1, g_chamReflect, g_chamFresnel, g_chamFlake, g_chamBright);
    RGB a{255, 255, 255}, b = a, c = a;
    auto it = g_templates.find("custom");
    if (it != g_templates.end()) { a = it->second.a; b = it->second.b; c = it->second.hasC ? it->second.c : it->second.b; }
    if (L.a >= 0) glUniform3f(L.a, a.r / 255.0f, a.g / 255.0f, a.b / 255.0f);
    if (L.b >= 0) glUniform3f(L.b, b.r / 255.0f, b.g / 255.0f, b.b / 255.0f);
    if (L.c >= 0) glUniform3f(L.c, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f);
}

static void glUseProgram_hook(unsigned prog) {
    glUseProgram_orig(prog);
    if (!prog || g_shaderPatched == 0) return;
    ProgLocs& L = g_progs[prog];
    if (!L.known) {
        L.p0 = glGetUniformLocation(prog, "ChamP0");
        L.p1 = glGetUniformLocation(prog, "ChamP1");
        L.a = glGetUniformLocation(prog, "ChamA");
        L.b = glGetUniformLocation(prog, "ChamB");
        L.c = glGetUniformLocation(prog, "ChamC");
        L.known = true;
    }
    if (L.p0 < 0 || L.ver == g_uniVersion) return;
    SetChamUniforms(L);
    L.ver = g_uniVersion;
}

static void glLinkProgram_hook(unsigned prog) {
    glLinkProgram_orig(prog);
    g_progs.erase(prog);
}

// ================= simpan config =================
static const char* ShiftName(int m) { return m == 1 ? "time" : (m == 2 ? "both" : (m == 3 ? "pixel" : "angle")); }

static void WriteTpl(FILE* f, const Template& t) {
    if (t.name == "custom") fprintf(f, "\n[custom]\n"); else fprintf(f, "\n[template:%s]\n", t.name.c_str());
    fprintf(f, "a=%d,%d,%d\nb=%d,%d,%d\n", t.a.r, t.a.g, t.a.b, t.b.r, t.b.g, t.b.b);
    if (t.hasC) fprintf(f, "c=%d,%d,%d\n", t.c.r, t.c.g, t.c.b);
    if (t.intensity != 1.0f) fprintf(f, "intensity=%.2f\n", t.intensity);
}

static void SaveConfig() {
    FILE* f = fopen(g_iniPath.c_str(), "w");
    if (!f) { logger->Error("Gagal menyimpan %s", g_iniPath.c_str()); return; }
    fprintf(f, "; Pearlescent Colors - disimpan dari menu in-game\n[settings]\n");
    fprintf(f, "enabled=%d\nautosave=%d\n", g_enabled ? 1 : 0, g_autoSave ? 1 : 0);
    fprintf(f, "mode=%s\nactive=%s\nshift_mode=%s\n", g_whitelist ? "whitelist" : "all", g_tpl.name.c_str(), ShiftName(g_shiftMode));
    fprintf(f, "angle_gain=%.2f\nelev_gain=%.2f\npart_spread=%.2f\nspeed=%.2f\nintensity=%.2f\n", g_angleGain, g_elevGain, g_partSpread, g_speed, g_intensity);
    fprintf(f, "shader_patch=%d\ngeom_offset=%d\n", g_shaderPatchEnabled ? 1 : 0, g_geomOff);
    fprintf(f, "cham_gain=%.2f\ncham_flow=%.2f\ncham_bias=%.2f\ncham_reflect=%.2f\ncham_fresnel=%.2f\ncham_depth=%.2f\ncham_flake=%.2f\ncham_brightness=%.2f\n",
            g_chamGain, g_chamFlow, g_chamBias, g_chamReflect, g_chamFresnel, g_chamDepth, g_chamFlake, g_chamBright);
    fprintf(f, "menu=%d\nmenu_scale=%.2f\nmenu_button_x=%.2f\nmenu_button_y=%.2f\nmenu_button_alpha=%.2f\nmenu_panel_alpha=%.2f\n", g_menuEnabled ? 1 : 0, g_menuScale, g_btnX, g_btnY, g_btnAlpha, g_panelAlpha);
    fprintf(f, "\n[cars]\n");
    for (auto& kv : g_carsCfg) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    auto cu = g_templates.find("custom");
    if (cu != g_templates.end()) WriteTpl(f, cu->second);
    for (auto& kv : g_templates) if (kv.first != "custom") WriteTpl(f, kv.second);
    for (int i = 0; i < 6; i++) {
        if (!g_presets[i].used) continue;
        const PresetSlot& ps = g_presets[i];
        fprintf(f, "\n[preset:%d]\na=%d,%d,%d\nb=%d,%d,%d\nc=%d,%d,%d\n", i + 1, ps.a.r, ps.a.g, ps.a.b, ps.b.r, ps.b.g, ps.b.b, ps.c.r, ps.c.g, ps.c.b);
    }
    fclose(f);
    struct stat st; if (stat(g_iniPath.c_str(), &st) == 0) g_mtime = st.st_mtime;
    logger->Info("Config disimpan");
}

// ================= clipboard (JNI) =================
typedef jint (*GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
static JavaVM* g_jvm = nullptr;

static JNIEnv* JEnv() {
    if (!g_jvm) {
        GetCreatedJavaVMs_t fn = nullptr;
        const char* libs[] = {"libnativehelper.so", "libart.so", nullptr};
        for (int i = 0; libs[i] && !fn; i++) {
            void* h = dlopen(libs[i], RTLD_NOW);
            if (h) fn = (GetCreatedJavaVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
        }
        if (!fn) fn = (GetCreatedJavaVMs_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        JavaVM* vm = nullptr; jsize n = 0;
        if (fn && fn(&vm, 1, &n) == JNI_OK && n > 0) g_jvm = vm;
    }
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) return env;
    return nullptr;
}

static bool JFail(JNIEnv* e) { if (e->ExceptionCheck()) { e->ExceptionClear(); return true; } return false; }

static jobject ClipMgr(JNIEnv* env) {
    jclass at = env->FindClass("android/app/ActivityThread");
    if (!at || JFail(env)) return nullptr;
    jmethodID cur = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
    if (!cur || JFail(env)) return nullptr;
    jobject app = env->CallStaticObjectMethod(at, cur);
    if (!app || JFail(env)) return nullptr;
    jclass ctx = env->FindClass("android/content/Context");
    jmethodID gss = env->GetMethodID(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!gss || JFail(env)) return nullptr;
    jobject cm = env->CallObjectMethod(app, gss, env->NewStringUTF("clipboard"));
    if (!cm || JFail(env)) return nullptr;
    return cm;
}

static std::string ClipboardGet() {
    JNIEnv* env = JEnv();
    if (!env) return "";
    env->PushLocalFrame(64);
    std::string r;
    do {
        jobject cm = ClipMgr(env);
        if (!cm) break;
        jclass cmc = env->FindClass("android/content/ClipboardManager");
        jmethodID gpc = env->GetMethodID(cmc, "getPrimaryClip", "()Landroid/content/ClipData;");
        if (!gpc || JFail(env)) break;
        jobject clip = env->CallObjectMethod(cm, gpc);
        if (!clip || JFail(env)) break;
        jclass cdc = env->FindClass("android/content/ClipData");
        jmethodID gic = env->GetMethodID(cdc, "getItemCount", "()I");
        jmethodID gia = env->GetMethodID(cdc, "getItemAt", "(I)Landroid/content/ClipData$Item;");
        if (!gic || !gia || JFail(env)) break;
        if (env->CallIntMethod(clip, gic) < 1) break;
        jobject item = env->CallObjectMethod(clip, gia, 0);
        if (!item || JFail(env)) break;
        jclass itc = env->FindClass("android/content/ClipData$Item");
        jmethodID gt = env->GetMethodID(itc, "getText", "()Ljava/lang/CharSequence;");
        if (!gt || JFail(env)) break;
        jobject cs = env->CallObjectMethod(item, gt);
        if (!cs || JFail(env)) break;
        jclass csc = env->FindClass("java/lang/CharSequence");
        jmethodID ts = env->GetMethodID(csc, "toString", "()Ljava/lang/String;");
        jstring js = (jstring)env->CallObjectMethod(cs, ts);
        if (!js || JFail(env)) break;
        const char* u = env->GetStringUTFChars(js, nullptr);
        if (u) { r = u; env->ReleaseStringUTFChars(js, u); }
    } while (false);
    JFail(env);
    env->PopLocalFrame(nullptr);
    return r;
}

static bool ClipboardSet(const std::string& text) {
    JNIEnv* env = JEnv();
    if (!env) return false;
    env->PushLocalFrame(64);
    bool ok = false;
    do {
        jobject cm = ClipMgr(env);
        if (!cm) break;
        jclass cd = env->FindClass("android/content/ClipData");
        jmethodID np = env->GetStaticMethodID(cd, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
        if (!np || JFail(env)) break;
        jobject clip = env->CallStaticObjectMethod(cd, np, env->NewStringUTF("chameleon"), env->NewStringUTF(text.c_str()));
        if (!clip || JFail(env)) break;
        jclass cmc = env->FindClass("android/content/ClipboardManager");
        jmethodID spc = env->GetMethodID(cmc, "setPrimaryClip", "(Landroid/content/ClipData;)V");
        if (!spc || JFail(env)) break;
        env->CallVoidMethod(cm, spc, clip);
        if (JFail(env)) break;
        ok = true;
    } while (false);
    JFail(env);
    env->PopLocalFrame(nullptr);
    return ok;
}

// ================= parsing kode warna =================
static std::string Trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::string HexStr(const RGB& c) { char b[16]; snprintf(b, sizeof b, "%02X%02X%02X", c.r, c.g, c.b); return b; }

static bool ParseHexToken(std::string s, RGB& o) {
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
    if (s.empty()) return false;
    for (char ch : s) if (!isxdigit((unsigned char)ch)) return false;
    if (s.size() == 3) { std::string e; for (char ch : s) { e += ch; e += ch; } s = e; }
    if (s.size() == 8) s = s.substr(0, 6);
    if (s.size() != 6) return false;
    unsigned v = (unsigned)strtoul(s.c_str(), nullptr, 16);
    o = { (int)((v >> 16) & 255), (int)((v >> 8) & 255), (int)(v & 255) };
    return true;
}

static bool ParseColorText(const std::string& text, RGB& o) {
    std::string t = Trimmed(text);
    size_t nl = t.find_first_of("\r\n");
    if (nl != std::string::npos) t = Trimmed(t.substr(0, nl));
    if (t.empty()) return false;
    if (t.find(',') != std::string::npos || t.find('(') != std::string::npos) {
        int r = 0, g = 0, b = 0;
        if (sscanf(t.c_str(), "%*[^0-9]%d%*[^0-9]%d%*[^0-9]%d", &r, &g, &b) == 3 ||
            sscanf(t.c_str(), "%d%*[^0-9]%d%*[^0-9]%d", &r, &g, &b) == 3) {
            o = { Clamp255(r), Clamp255(g), Clamp255(b) };
            return true;
        }
        return false;
    }
    return ParseHexToken(t, o);
}

static int ParsePalette(const std::string& text, RGB out[3]) {
    std::vector<std::string> toks;
    std::string cur;
    for (char ch : text) {
        if (isspace((unsigned char)ch) || ch == ';' || ch == '|') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
        else cur += ch;
    }
    if (!cur.empty()) toks.push_back(cur);
    int n = 0;
    for (const std::string& tk : toks) {
        RGB c;
        if (n < 3 && ParseHexToken(tk, c)) out[n++] = c; else return 0;
    }
    return n;
}

// ================= auto-save =================
struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    void b(const void* p, size_t n) { const unsigned char* c = (const unsigned char*)p; for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= 1099511628211ULL; } }
    template <class T> void v(const T& x) { b(&x, sizeof x); }
    void s(const std::string& x) { b(x.data(), x.size()); b("|", 1); }
};
static void HashRGB(Fnv& f, const RGB& c) { f.v(c.r); f.v(c.g); f.v(c.b); }

static uint64_t SettingsHash() {
    Fnv f;
    f.v(g_enabled); f.v(g_autoSave); f.v(g_whitelist); f.v(g_shiftMode); f.s(g_tpl.name);
    f.v(g_angleGain); f.v(g_elevGain); f.v(g_partSpread); f.v(g_speed); f.v(g_intensity);
    f.v(g_chamGain); f.v(g_chamFlow); f.v(g_chamBias); f.v(g_chamReflect); f.v(g_chamFresnel); f.v(g_chamDepth); f.v(g_chamFlake); f.v(g_chamBright);
    f.v(g_btnX); f.v(g_btnY); f.v(g_btnAlpha); f.v(g_panelAlpha);
    for (auto& kv : g_carsCfg) { f.s(kv.first); f.s(kv.second); }
    auto cu = g_templates.find("custom");
    if (cu != g_templates.end()) { HashRGB(f, cu->second.a); HashRGB(f, cu->second.b); HashRGB(f, cu->second.c); f.v(cu->second.hasC); }
    for (int i = 0; i < 6; i++) { f.v(g_presets[i].used); HashRGB(f, g_presets[i].a); HashRGB(f, g_presets[i].b); HashRGB(f, g_presets[i].c); }
    return f.h;
}

// ================= menu in-game (ImGui) =================
static bool IsDownVal(int v) { return (v | 1) == 3; }   // LIB_PointerGetButton: 2/3 = sedang ditekan

static bool PtInUi(float x, float y) {
    for (const UiRect& r : g_uiRects)
        if (r.valid && x >= r.x0 - 8 && x <= r.x1 + 8 && y >= r.y0 - 8 && y <= r.y1 + 8) return true;
    return false;
}

// sentuhan yang dimulai di atas menu tidak diteruskan ke game
static int GetButton_hook(int idx, int btn) {
    int v = GetButton_orig(idx, btn);
    if (btn != 0 || idx < 0 || idx > 1 || !g_menuReady) return v;
    if (!IsDownVal(v)) {
        bool was = g_latch[idx];
        g_latch[idx] = false; g_prevDown[idx] = false;
        return was ? 0 : v;
    }
    if (!g_prevDown[idx]) {
        g_prevDown[idx] = true;
        int x = 0, y = 0; float pr = 0;
        if (GetCoords_orig) GetCoords_orig(idx, &x, &y, &pr);
        g_latch[idx] = PtInUi((float)x, (float)y);
    }
    return g_latch[idx] ? 0 : v;
}

static float g_sc = 1.0f;
static bool  g_noScroll = false;
static int   g_tab = 0, g_editSlot = -1, g_pickSlot = -1, g_presetMode = 0;
static bool  g_hexFresh = false;
static std::string g_hexBuf, g_toast;
static double g_toastT = -10.0;
static const ImVec4 kAccent(0.56f, 0.36f, 0.98f, 1.0f);
static const ImVec4 kAccent2(0.16f, 0.74f, 0.96f, 1.0f);
static const ImVec4 kPanel(0.17f, 0.17f, 0.23f, 1.0f);
static const ImVec4 kPanelHi(0.23f, 0.23f, 0.31f, 1.0f);

static void Toast(const std::string& t) { g_toast = t; g_toastT = NowSec(); }

// ---- setelan awal ----
static void ResetEffects() {
    g_shiftMode = 3;
    g_chamGain = 1.6f; g_chamFlow = 0.25f; g_chamBias = 0.0f; g_chamDepth = 0.25f;
    g_chamReflect = 0.22f; g_chamFresnel = 0.5f; g_chamFlake = 0.08f; g_chamBright = 0.9f;
    g_angleGain = 2.0f; g_elevGain = 1.5f; g_partSpread = 2.5f; g_speed = 0.5f; g_intensity = 1.0f;
    g_uniVersion++;
}

static void ResetAll() {
    ResetEffects();
    g_whitelist = false; g_carsCfg.clear(); g_needResolve = true;
    g_enabled = true; g_autoSave = true;
    g_btnX = 0.44f; g_btnY = 0.02f; g_btnAlpha = 1.0f; g_panelAlpha = 0.97f;
    Template& cu = g_templates["custom"];
    cu.name = "custom"; cu.a = {40, 120, 255}; cu.b = {255, 60, 200}; cu.c = {60, 220, 160}; cu.hasC = true; cu.intensity = 1.0f;
    for (int i = 0; i < 6; i++) g_presets[i] = PresetSlot();
    auto it = g_templates.find("Purple-Blue-Green");
    g_tpl = (it != g_templates.end()) ? it->second : cu;
    g_editSlot = -1; g_pickSlot = -1;
    g_uniVersion++;
}

static void SaveRect(int i) {
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    g_uiRects[i] = { p.x, p.y, p.x + sz.x, p.y + sz.y, true };
}

static bool Dragged() {
    float lim = 10.0f * g_sc;
    return ImGui::GetIO().MouseDragMaxDistanceSqr[0] > lim * lim;
}
static bool Btn(const char* label, const ImVec2& size = ImVec2(0, 0)) { bool r = ImGui::Button(label, size); return r && !Dragged(); }

static void TouchScroll() {
    if (g_noScroll) return;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseDragging(0, 8.0f * g_sc))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
}

static RGB ToRGB(const float* c) {
    return { Clamp255((int)(c[0] * 255.0f + 0.5f)), Clamp255((int)(c[1] * 255.0f + 0.5f)), Clamp255((int)(c[2] * 255.0f + 0.5f)) };
}

static ImU32 C32(const RGB& c, int a = 255) { return IM_COL32(c.r, c.g, c.b, a); }
static RGB MixRGB(const RGB& a, const RGB& b, float u) {
    return { Clamp255((int)(a.r + (b.r - a.r) * u)), Clamp255((int)(a.g + (b.g - a.g) * u)), Clamp255((int)(a.b + (b.b - a.b) * u)) };
}
static float Smooth(float x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return x * x * (3.0f - 2.0f * x); }
static RGB RampAt(const Template& t, float k) {   // sama dengan perhitungan di shader
    if (t.hasC || t.name == "custom") {
        RGB ab = MixRGB(t.a, t.b, Smooth(k * 2.0f));
        return MixRGB(ab, t.hasC ? t.c : t.b, Smooth(k * 2.0f - 1.0f));
    }
    return MixRGB(t.a, t.b, Smooth(k));
}
static void DrawRamp(const Template& t, const ImVec2& p0, const ImVec2& p1, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int N = 36;
    float w = p1.x - p0.x;
    for (int i = 0; i < N; i++) {
        float x0 = p0.x + w * i / N, x1 = p0.x + w * (i + 1) / N;
        ImDrawFlags fl = (i == 0) ? ImDrawFlags_RoundCornersLeft : (i == N - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
        float r = (i == 0 || i == N - 1) ? rounding : 0.0f;
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(i == N - 1 ? x1 : x1 + 1.0f, p1.y), C32(RampAt(t, (i + 0.5f) / N)), r, fl);
    }
}

static bool Toggle(const char* id, bool* v) {
    float h = ImGui::GetFrameHeight() * 0.95f, w = h * 1.8f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h)) && !Dragged();
    if (pressed) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), *v ? ImGui::GetColorU32(kAccent) : IM_COL32(72, 72, 92, 255), h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + (*v ? w - h * 0.5f : h * 0.5f), p.y + h * 0.5f), h * 0.38f, IM_COL32(255, 255, 255, 255));
    return pressed;
}

static bool ToggleRow(const char* label, bool* v) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    float tw = ImGui::GetFrameHeight() * 0.95f * 1.8f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - tw);
    ImGui::PushID(label);
    bool r = Toggle("##t", v);
    ImGui::PopID();
    return r;
}

static bool Segmented(const char* id, const char* const* items, int n, int* sel) {
    bool changed = false;
    float avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    float w = (avail - sp * (n - 1)) / n;
    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine();
        bool on = (*sel == i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kPanel);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccent : kPanelHi);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent2);
        if (Btn(items[i], ImVec2(w, ImGui::GetFrameHeight() * 1.2f)) && !on) { *sel = i; changed = true; }
        ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    return changed;
}

static bool SliderRow(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(label);
    bool ch = ImGui::SliderFloat("##s", v, lo, hi, fmt);
    if (ImGui::IsItemActive()) g_noScroll = true;
    ImGui::PopID();
    return ch;
}

static void SectionTitle(const char* t) {
    ImGui::Spacing();
    ImGui::TextColored(kAccent2, "%s", t);
    ImGui::Separator();
    ImGui::Spacing();
}

static bool TemplateCard(const std::string& title, const Template& t, bool sel, float w, float h) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool tap = ImGui::InvisibleButton(title.c_str(), ImVec2(w, h)) && !Dragged();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 12.0f * g_sc;
    DrawRamp(t, p, ImVec2(p.x + w, p.y + h), r);
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.5f), ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));
    std::string shown = title;
    for (char& ch : shown) if (ch == '-') ch = ' ';
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddText(ImVec2(p.x + 10.0f * g_sc, p.y + h - ImGui::GetTextLineHeight() - 6.0f * g_sc), IM_COL32(255, 255, 255, 255), shown.c_str());
    dl->PopClipRect();
    if (sel) dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 255), r, 0, 3.0f * g_sc);
    return tap;
}

static void ApplyCustom(int slot, const RGB& c) {
    Template& cu = g_templates["custom"];
    cu.name = "custom";
    if (!cu.hasC) { cu.c = cu.b; cu.hasC = true; }
    if (slot == 0) cu.a = c; else if (slot == 1) cu.b = c; else cu.c = c;
    g_tpl = cu;
    g_uniVersion++;
}

static void ColorRow(int slot, const char* label) {
    Template& cu = g_templates["custom"];
    cu.name = "custom";
    RGB cur = slot == 0 ? cu.a : (slot == 1 ? cu.b : (cu.hasC ? cu.c : cu.b));
    bool editing = (g_editSlot == slot);
    bool picking = (g_pickSlot == slot);
    ImGui::PushID(slot);
    ImGui::TextUnformatted(label);
    float sz = ImGui::GetFrameHeight() * 1.5f;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool tapSw = ImGui::InvisibleButton("##sw", ImVec2(sz, sz)) && !Dragged();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), C32(cur), 10.0f * g_sc);
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + sz, p.y + sz), picking ? IM_COL32(143, 92, 250, 255) : IM_COL32(255, 255, 255, 90), 10.0f * g_sc, 0, picking ? 4.0f : 2.0f);
    ImGui::SameLine();
    float bw = ImGui::GetFrameHeight() * 2.6f;
    float fw = ImGui::GetContentRegionAvail().x - (bw * 2 + sp * 2);
    std::string shown = editing ? ("#" + g_hexBuf + std::string(g_hexBuf.size() < 6 ? 6 - g_hexBuf.size() : 0, '_')) : ("#" + HexStr(cur));
    ImGui::PushStyleColor(ImGuiCol_Button, editing ? kAccent : kPanel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, editing ? kAccent : kPanelHi);
    bool tapField = Btn(shown.c_str(), ImVec2(fw, sz));
    ImGui::PopStyleColor(2);
    if (tapSw) { if (picking) g_pickSlot = -1; else { g_pickSlot = slot; g_editSlot = -1; } }
    if (tapField) {
        if (editing) g_editSlot = -1;
        else { g_editSlot = slot; g_pickSlot = -1; g_hexBuf = HexStr(cur); g_hexFresh = true; }
    }
    ImGui::SameLine();
    if (Btn("Salin", ImVec2(bw, sz))) Toast(ClipboardSet("#" + HexStr(cur)) ? "Kode warna disalin" : "Clipboard tidak tersedia");
    ImGui::SameLine();
    if (Btn("Tempel", ImVec2(bw, sz))) {
        RGB c;
        if (ParseColorText(ClipboardGet(), c)) { ApplyCustom(slot, c); Toast("Warna ditempel"); }
        else Toast("Clipboard kosong / kode tidak valid");
    }
    if (editing) {
        static const char* keys = "0123456789ABCDEF";
        float kw = (ImGui::GetContentRegionAvail().x - sp * 7) / 8.0f, kh = ImGui::GetFrameHeight() * 1.35f;
        for (int i = 0; i < 16; i++) {
            if (i % 8) ImGui::SameLine();
            char lb[2] = { keys[i], 0 };
            if (Btn(lb, ImVec2(kw, kh))) {
                if (g_hexFresh) { g_hexBuf.clear(); g_hexFresh = false; }
                if (g_hexBuf.size() < 6) g_hexBuf += lb;
                RGB c;
                if (g_hexBuf.size() == 6 && ParseHexToken(g_hexBuf, c)) ApplyCustom(slot, c);
            }
        }
        float kw4 = (ImGui::GetContentRegionAvail().x - sp * 3) / 4.0f;
        if (Btn("Hapus", ImVec2(kw4, kh))) { if (g_hexFresh) { g_hexBuf.clear(); g_hexFresh = false; } else if (!g_hexBuf.empty()) g_hexBuf.pop_back(); }
        ImGui::SameLine();
        if (Btn("Bersih", ImVec2(kw4, kh))) { g_hexBuf.clear(); g_hexFresh = false; }
        ImGui::SameLine();
        if (Btn("Tempel kode", ImVec2(kw4, kh))) {
            RGB c;
            if (ParseColorText(ClipboardGet(), c)) { ApplyCustom(slot, c); g_hexBuf = HexStr(c); g_hexFresh = true; Toast("Warna ditempel"); }
            else Toast("Clipboard kosong / kode tidak valid");
        }
        ImGui::SameLine();
        if (Btn("OK", ImVec2(kw4, kh))) {
            RGB c;
            if ((g_hexBuf.size() == 3 || g_hexBuf.size() == 6) && ParseHexToken(g_hexBuf, c)) ApplyCustom(slot, c);
            g_editSlot = -1;
        }
    }
    if (g_pickSlot == slot) {
        // picker RGB / HSV (inline, tanpa popup)
        RGB c0 = slot == 0 ? cu.a : (slot == 1 ? cu.b : (cu.hasC ? cu.c : cu.b));
        float col[3] = { c0.r / 255.0f, c0.g / 255.0f, c0.b / 255.0f };
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.70f);
        if (ImGui::ColorPicker3("##pick", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_PickerHueBar))
            ApplyCustom(slot, ToRGB(col));
        if (ImGui::IsAnyItemActive()) g_noScroll = true;
        RGB c1 = slot == 0 ? cu.a : (slot == 1 ? cu.b : (cu.hasC ? cu.c : cu.b));
        int rgb[3] = { c1.r, c1.g, c1.b };
        static const char* fm[3] = { "R   %d", "G   %d", "B   %d" };
        bool chg = false;
        for (int i = 0; i < 3; i++) {
            ImGui::PushID(100 + i);
            ImGui::SetNextItemWidth(-1.0f);
            chg |= ImGui::SliderInt("##rgb", &rgb[i], 0, 255, fm[i]);
            if (ImGui::IsItemActive()) g_noScroll = true;
            ImGui::PopID();
        }
        if (chg) ApplyCustom(slot, { Clamp255(rgb[0]), Clamp255(rgb[1]), Clamp255(rgb[2]) });
    }
    ImGui::PopID();
    ImGui::Spacing();
}

static int CurrentVehicleModel() {
    if (!FindPlayerVehicle_fn) return -1;
    void* v = FindPlayerVehicle_fn(-1, false);
    if (!v) return -1;
    int id = -1;
    void* clump = *(void**)((char*)v + 0x20);               // CEntity::m_pRwObject (64-bit)
    if (clump) id = ModelIdOf(clump);
    if (id < 0) id = (int)*(uint16_t*)((char*)v + 0x2E);    // CEntity::m_nModelIndex (64-bit)
    return (id >= 0 && id < 20000) ? id : -1;
}

static void TabWarna() {
    float sp = ImGui::GetStyle().ItemSpacing.x;
    // pratinjau template aktif
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() * 2.2f;
        ImGui::Dummy(ImVec2(w, h));
        DrawRamp(g_tpl, p, ImVec2(p.x + w, p.y + h), 14.0f * g_sc);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.4f), ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));
        std::string nm = g_tpl.name;
        for (char& ch : nm) if (ch == '-') ch = ' ';
        dl->AddText(ImVec2(p.x + 12.0f * g_sc, p.y + h - ImGui::GetTextLineHeight() - 8.0f * g_sc), IM_COL32(255, 255, 255, 255), nm.c_str());
    }
    SectionTitle("TEMPLATE");
    {
        float cw = (ImGui::GetContentRegionAvail().x - sp) / 2.0f, ch = ImGui::GetFrameHeight() * 2.1f;
        std::vector<const Template*> list;
        auto cu = g_templates.find("custom");
        if (cu != g_templates.end()) list.push_back(&cu->second);
        for (auto& kv : g_templates) if (kv.first != "custom") list.push_back(&kv.second);
        int idx = 0;
        for (const Template* t : list) {
            if (idx % 2 == 1) ImGui::SameLine();
            std::string title = (t->name == "custom") ? std::string("Custom (milikmu)") : t->name;
            if (TemplateCard(title, *t, t->name == g_tpl.name, cw, ch)) g_tpl = *t;
            idx++;
        }
    }
    SectionTitle("MIX MANUAL / KODE WARNA");
    ImGui::TextWrapped("Ketuk KOTAK WARNA untuk membuka picker RGB. Ketuk KOLOM KODE untuk mengetik hex (keypad). Tempel/Salin memakai clipboard (#RRGGBB, RGB, atau 120,40,200).");
    ImGui::Spacing();
    ColorRow(0, "Warna A (menghadap kamera)");
    ColorRow(1, "Warna B (tengah)");
    ColorRow(2, "Warna C (tepi)");
    {
        float bw = (ImGui::GetContentRegionAvail().x - sp * 2) / 3.0f, bh = ImGui::GetFrameHeight() * 1.3f;
        Template& cu = g_templates["custom"];
        if (Btn("Salin palet", ImVec2(bw, bh))) {
            RGB c3 = cu.hasC ? cu.c : cu.b;
            Toast(ClipboardSet("#" + HexStr(cu.a) + " #" + HexStr(cu.b) + " #" + HexStr(c3)) ? "Palet disalin" : "Clipboard tidak tersedia");
        }
        ImGui::SameLine();
        if (Btn("Tempel palet", ImVec2(bw, bh))) {
            RGB pal[3];
            int n = ParsePalette(ClipboardGet(), pal);
            if (n >= 2) { for (int i = 0; i < n; i++) ApplyCustom(i, pal[i]); Toast("Palet ditempel"); }
            else Toast("Format palet: #RRGGBB #RRGGBB #RRGGBB");
        }
        ImGui::SameLine();
        if (Btn("Salin dari aktif", ImVec2(bw, bh))) {
            if (g_tpl.name != "custom") { Template src = g_tpl; ApplyCustom(0, src.a); ApplyCustom(1, src.b); ApplyCustom(2, src.hasC ? src.c : src.b); g_tpl = g_templates["custom"]; Toast("Template disalin ke custom"); }
        }
    }
    SectionTitle("SLOT PRESET");
    {
        static const char* pm[] = {"Muat", "Simpan"};
        Segmented("pm", pm, 2, &g_presetMode);
        ImGui::Spacing();
        float cw = (ImGui::GetContentRegionAvail().x - sp * 2) / 3.0f, ch = ImGui::GetFrameHeight() * 1.9f;
        for (int i = 0; i < 6; i++) {
            if (i % 3) ImGui::SameLine();
            PresetSlot& ps = g_presets[i];
            char nm[32]; snprintf(nm, sizeof nm, "Slot %d", i + 1);
            bool tap;
            if (ps.used) {
                Template tmp; tmp.name = nm; tmp.a = ps.a; tmp.b = ps.b; tmp.c = ps.c; tmp.hasC = true;
                tap = TemplateCard(nm, tmp, false, cw, ch);
            } else {
                ImVec2 p = ImGui::GetCursorScreenPos();
                tap = ImGui::InvisibleButton(nm, ImVec2(cw, ch)) && !Dragged();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(40, 40, 54, 255), 12.0f * g_sc);
                dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(110, 110, 140, 255), 12.0f * g_sc, 0, 2.0f);
                std::string t = std::string(nm) + " (kosong)";
                dl->AddText(ImVec2(p.x + 10.0f * g_sc, p.y + ch * 0.5f - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(170, 170, 200, 255), t.c_str());
            }
            if (tap) {
                Template& cu = g_templates["custom"];
                if (g_presetMode == 1) {
                    ps.used = true; ps.a = cu.a; ps.b = cu.b; ps.c = cu.hasC ? cu.c : cu.b;
                    Toast(std::string("Custom disimpan ke ") + nm);
                } else if (ps.used) {
                    ApplyCustom(0, ps.a); ApplyCustom(1, ps.b); ApplyCustom(2, ps.c);
                    Toast(std::string(nm) + " dimuat ke custom");
                }
            }
        }
    }
}

static void TabEfek() {
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.26f, 0.38f, 1.0f));
        if (Btn("Kembalikan efek ke default", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.4f))) { ResetEffects(); Toast("Efek dikembalikan ke default"); }
        ImGui::PopStyleColor(2);
    }
    SectionTitle("MODE");
    static const char* kModes[] = {"Pixel", "Sudut", "Waktu", "Gabungan"};
    static const int kModeVal[] = {3, 0, 1, 2};
    int mi = 0;
    for (int i = 0; i < 4; i++) if (kModeVal[i] == g_shiftMode) mi = i;
    if (Segmented("mode", kModes, 4, &mi)) g_shiftMode = kModeVal[mi];
    if (g_shiftMode == 3 && !g_shaderOK) ImGui::TextWrapped("Pixel belum aktif (shader_patch=0 atau hook gagal), dipakai mode sudut.");

    SectionTitle("EFEK PER PIKSEL (live)");
    bool ch = false;
    ch |= SliderRow("Gain sudut", &g_chamGain, 0.2f, 4.0f);
    ch |= SliderRow("Aliran pantulan", &g_chamFlow, -1.0f, 1.0f);
    ch |= SliderRow("Geser awal", &g_chamBias, -0.6f, 0.6f);
    ch |= SliderRow("Kedalaman", &g_chamDepth, 0.0f, 0.8f);
    ch |= SliderRow("Kilap minimum", &g_chamReflect, 0.0f, 0.6f);
    ch |= SliderRow("Fresnel", &g_chamFresnel, 0.0f, 1.0f);
    ch |= SliderRow("Glitter", &g_chamFlake, 0.0f, 0.5f);
    ch |= SliderRow("Kecerahan", &g_chamBright, 0.3f, 1.5f);
    if (ch) g_uniVersion++;

    SectionTitle("MODE SUDUT / WAKTU");
    SliderRow("Gain sudut keliling", &g_angleGain, 0.5f, 6.0f);
    SliderRow("Gain elevasi", &g_elevGain, 0.0f, 4.0f);
    SliderRow("Beda antar bagian", &g_partSpread, 0.0f, 6.0f);
    SliderRow("Kecepatan (waktu)", &g_speed, 0.0f, 3.0f);
    SliderRow("Intensitas", &g_intensity, 0.0f, 1.0f);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.26f, 0.38f, 1.0f));
    if (Btn("Kembalikan efek ke default ", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.4f))) { ResetEffects(); Toast("Efek dikembalikan ke default"); }
    ImGui::PopStyleColor(2);
}

static void TabMobil() {
    SectionTitle("TARGET");
    static const char* kTg[] = {"Semua mobil", "Hanya terdaftar"};
    int tg = g_whitelist ? 1 : 0;
    if (Segmented("tg", kTg, 2, &tg)) g_whitelist = (tg == 1);
    SectionTitle("MOBIL YANG DIKENDARAI");
    int cur = CurrentVehicleModel();
    if (cur >= 0) {
        ImGui::Text("Model ID: %d", cur);
        float sp = ImGui::GetStyle().ItemSpacing.x;
        float bw = (ImGui::GetContentRegionAvail().x - sp) / 2.0f, bh = ImGui::GetFrameHeight() * 1.3f;
        if (Btn("Daftarkan (template aktif)", ImVec2(bw, bh))) { g_carsCfg[std::to_string(cur)] = g_tpl.name; g_needResolve = true; Toast("Mobil didaftarkan"); }
        ImGui::SameLine();
        if (Btn("Hapus dari daftar", ImVec2(bw, bh))) { g_carsCfg.erase(std::to_string(cur)); g_needResolve = true; Toast("Mobil dihapus"); }
    } else {
        ImGui::TextWrapped("Naiki sebuah mobil untuk mendaftarkannya.");
    }
    SectionTitle("DAFTAR MOBIL");
    if (g_carsCfg.empty()) ImGui::TextWrapped("Belum ada mobil terdaftar.");
    std::string del;
    for (auto& kv : g_carsCfg) {
        ImGui::PushID(kv.first.c_str());
        if (Btn("X", ImVec2(ImGui::GetFrameHeight() * 1.3f, ImGui::GetFrameHeight() * 1.2f))) del = kv.first;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  ->  %s", kv.first.c_str(), kv.second.c_str());
        ImGui::PopID();
    }
    if (!del.empty()) { g_carsCfg.erase(del); g_needResolve = true; }
}

static void TabSistem() {
    SectionTitle("PENYIMPANAN");
    ToggleRow("Simpan otomatis", &g_autoSave);
    ImGui::TextWrapped("Semua pengaturan (template, warna custom, slider, daftar mobil, preset) tersimpan otomatis ke pearlescent.ini sehingga tidak perlu diatur ulang.");
    ImGui::Spacing();
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float bw = (ImGui::GetContentRegionAvail().x - sp) / 2.0f, bh = ImGui::GetFrameHeight() * 1.3f;
    if (Btn("Simpan sekarang", ImVec2(bw, bh))) { SaveConfig(); g_hashInit = false; Toast("Pengaturan disimpan"); }
    ImGui::SameLine();
    if (Btn("Muat ulang dari ini", ImVec2(bw, bh))) { LoadConfig(); g_uniVersion++; Toast("Pengaturan dimuat ulang"); }
    SectionTitle("TOMBOL MENU");
    SliderRow("Posisi X", &g_btnX, 0.0f, 0.95f);
    SliderRow("Posisi Y", &g_btnY, 0.0f, 0.9f);
    SliderRow("Transparansi tombol (kecil = lebih transparan)", &g_btnAlpha, 0.10f, 1.0f);
    SliderRow("Transparansi panel menu", &g_panelAlpha, 0.40f, 1.0f);
    SectionTitle("RESET GLOBAL");
    ImGui::TextWrapped("Mengembalikan SEMUA pengaturan ke setelan awal: efek, mode, warna custom, preset, daftar mobil, posisi dan transparansi tombol.");
    {
        static double confirmT = -10.0;
        bool armed = (NowSec() - confirmT) < 3.0;
        ImGui::PushStyleColor(ImGuiCol_Button, armed ? ImVec4(0.80f, 0.20f, 0.25f, 1.0f) : ImVec4(0.55f, 0.20f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.26f, 0.32f, 1.0f));
        if (Btn(armed ? "Ketuk lagi untuk KONFIRMASI reset" : "Kembalikan semua ke setelan awal", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.5f))) {
            if (armed) { ResetAll(); confirmT = -10.0; Toast("Semua pengaturan dikembalikan ke setelan awal"); }
            else confirmT = NowSec();
        }
        ImGui::PopStyleColor(2);
    }
    SectionTitle("STATUS");
    ImGui::TextWrapped("Shader per-piksel: %s", g_shaderOK ? "aktif" : "tidak aktif");
    ImGui::TextWrapped("Template: %d | Palet shader: %d", (int)g_templates.size(), (int)g_palette.size());
    ImGui::TextWrapped("Config: %s", g_iniPath.c_str());
}

static void DrawToast(float W, float H) {
    double age = NowSec() - g_toastT;
    if (age > 2.2 || g_toast.empty()) return;
    float a = age > 1.6 ? (float)(1.0 - (age - 1.6) / 0.6) : 1.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize(g_toast.c_str());
    float padX = 18.0f * g_sc, padY = 10.0f * g_sc;
    ImVec2 p0(W * 0.5f - ts.x * 0.5f - padX, H * 0.86f), p1(W * 0.5f + ts.x * 0.5f + padX, H * 0.86f + ts.y + padY * 2);
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 36, (int)(235 * a)), 14.0f * g_sc);
    dl->AddRect(p0, p1, IM_COL32(143, 92, 250, (int)(255 * a)), 14.0f * g_sc, 0, 2.0f);
    dl->AddText(ImVec2(p0.x + padX, p0.y + padY), IM_COL32(255, 255, 255, (int)(255 * a)), g_toast.c_str());
}

static void BuildUI(float W, float H) {
    g_noScroll = false;
    ImDrawList* fdl = nullptr; (void)fdl;
    // ---- tombol melayang ----
    {
        float d = 44.0f * g_sc;
        ImGui::SetNextWindowPos(ImVec2(W * g_btnX, H * g_btnY), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##pcbtn", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##fab", ImVec2(d, d)) && !Dragged()) g_menuOpen = !g_menuOpen;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
        float al = g_btnAlpha;
        ImVec4 rc = g_menuOpen ? kAccent2 : kAccent; rc.w = al;
        dl->AddCircleFilled(c, d * 0.5f, IM_COL32(20, 20, 32, (int)(235 * al)));
        dl->AddCircle(c, d * 0.5f - 2.0f * g_sc, ImGui::GetColorU32(rc), 0, 3.0f * g_sc);
        ImVec2 ts = ImGui::CalcTextSize("PC");
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, (int)(255 * al)), "PC");
        dl->AddCircleFilled(ImVec2(c.x + d * 0.34f, c.y - d * 0.34f), d * 0.09f, g_enabled ? IM_COL32(60, 220, 120, (int)(255 * al)) : IM_COL32(235, 70, 70, (int)(255 * al)));
        SaveRect(0);
        ImGui::End();
        ImGui::PopStyleVar();
    }
    g_uiRects[1].valid = false;
    g_uiRects[2].valid = false;
    if (g_menuOpen) {
        float mw = W * 0.64f, mh = H * 0.90f;
        ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(g_panelAlpha);
        ImGui::Begin("##pcmain", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        SaveRect(1);
        // header
        ImGui::AlignTextToFramePadding();
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextColored(kAccent, "CHAMELEON");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        ImGui::TextDisabled(" Pearlescent");
        float tw = ImGui::GetFrameHeight() * 0.95f * 1.8f, xw = ImGui::GetFrameHeight() * 1.5f, sp = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - (tw + xw + sp * 2));
        Toggle("##master", &g_enabled);
        ImGui::SameLine();
        if (Btn("X", ImVec2(xw, ImGui::GetFrameHeight() * 0.95f))) g_menuOpen = false;
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilledMultiColor(p, ImVec2(p.x + w * 0.5f, p.y + 3.0f * g_sc), ImGui::GetColorU32(kAccent), ImGui::GetColorU32(kAccent2), ImGui::GetColorU32(kAccent2), ImGui::GetColorU32(kAccent));
            dl->AddRectFilledMultiColor(ImVec2(p.x + w * 0.5f, p.y), ImVec2(p.x + w, p.y + 3.0f * g_sc), ImGui::GetColorU32(kAccent2), IM_COL32(255, 90, 170, 255), IM_COL32(255, 90, 170, 255), ImGui::GetColorU32(kAccent2));
            ImGui::Dummy(ImVec2(w, 3.0f * g_sc));
        }
        ImGui::Spacing();
        static const char* kTabs[] = {"Warna", "Efek", "Mobil", "Sistem"};
        Segmented("tabs", kTabs, 4, &g_tab);
        ImGui::Spacing();
        ImGui::BeginChild("##content", ImVec2(0, 0), 0, 0);
        switch (g_tab) {
            case 0: TabWarna(); break;
            case 1: TabEfek(); break;
            case 2: TabMobil(); break;
            default: TabSistem(); break;
        }
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
        TouchScroll();
        ImGui::EndChild();
        ImGui::End();
    }
    DrawToast(W, H);
}

static void ApplyModernStyle(float sc) {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 18.0f; st.ChildRounding = 12.0f; st.FrameRounding = 10.0f; st.GrabRounding = 10.0f;
    st.ScrollbarRounding = 10.0f; st.PopupRounding = 12.0f;
    st.WindowPadding = ImVec2(16, 14); st.FramePadding = ImVec2(12, 8); st.ItemSpacing = ImVec2(10, 10);
    st.ItemInnerSpacing = ImVec2(8, 6); st.ScrollbarSize = 7.0f; st.GrabMinSize = 22.0f;
    st.WindowBorderSize = 1.0f; st.FrameBorderSize = 0.0f; st.ChildBorderSize = 0.0f;
    st.ScaleAllSizes(sc);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.98f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.66f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.11f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.15f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.56f, 0.36f, 0.98f, 0.45f);
    c[ImGuiCol_FrameBg] = kPanel;
    c[ImGuiCol_FrameBgHovered] = kPanelHi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.38f, 1.0f);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent2;
    c[ImGuiCol_CheckMark] = kAccent2;
    c[ImGuiCol_Button] = kPanel;
    c[ImGuiCol_ButtonHovered] = kPanelHi;
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.56f, 0.36f, 0.98f, 0.55f);
    c[ImGuiCol_ScrollbarGrabHovered] = kAccent;
    c[ImGuiCol_ScrollbarGrabActive] = kAccent2;
}

static bool InitMenu() {
    if (g_menuFailed) return false;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] < 200 || vp[3] < 200) return false;
    float sc = g_menuScale > 0.0f ? g_menuScale : (float)vp[3] / 480.0f;
    if (sc < 1.0f) sc = 1.0f;
    g_sc = sc;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // font sistem Android (Roboto) agar tampilan modern; cadangan: font bawaan ImGui
    static const char* fonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/Roboto[wdth,wght].ttf", "/system/fonts/DroidSans.ttf", nullptr};
    bool fontOk = false;
    for (int i = 0; fonts[i] && !fontOk; i++) {
        if (access(fonts[i], R_OK) == 0) fontOk = (io.Fonts->AddFontFromFileTTF(fonts[i], 15.0f * sc) != nullptr);
    }
    if (!fontOk) { ImFontConfig fc; fc.SizePixels = 13.0f * sc; io.Fonts->AddFontDefault(&fc); }
    ApplyModernStyle(sc);
    if (!ImGui_ImplOpenGL3_Init("#version 100")) { g_menuFailed = true; logger->Error("Init ImGui gagal"); return false; }
    g_menuReady = true;
    logger->Info("Menu ImGui siap (skala %.2f, layar %dx%d, font %s)", sc, vp[2], vp[3], fontOk ? "sistem" : "bawaan");
    return true;
}

struct AttribBackup { GLint en, size, type, norm, stride, buf; void* ptr; };

static void DrawMenuFrame() {
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] < 200 || vp[3] < 200) return;
    float W = (float)vp[2], H = (float)vp[3];

    // simpan state vertex yang tidak dipulihkan backend ImGui
    GLint prevArray = 0, prevElem = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElem);
    AttribBackup ab[8];
    for (int i = 0; i < 8; i++) {
        ab[i] = {0, 4, GL_FLOAT, 0, 0, 0, nullptr};
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &ab[i].en);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &ab[i].size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ab[i].type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &ab[i].norm);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &ab[i].stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &ab[i].buf);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ab[i].ptr);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(W, H);
    static double lastT = 0;
    double now = NowSec();
    io.DeltaTime = (lastT > 0 && now > lastT) ? (float)(now - lastT) : (1.0f / 60.0f);
    lastT = now;

    // input sentuh: pointer 0
    static bool wasDown = false;
    int v = GetButton_orig ? GetButton_orig(0, 0) : 0;
    bool down = IsDownVal(v);
    int px = 0, py = 0; float pr = 0;
    if (GetCoords_orig) GetCoords_orig(0, &px, &py, &pr);
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (down) io.AddMousePosEvent((float)px, (float)py);
    if (down != wasDown) io.AddMouseButtonEvent(0, down);
    if (!down && wasDown) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    wasDown = down;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    BuildUI(W, H);

    // simpan otomatis: tunggu pengaturan stabil 1 detik dan jari sudah dilepas
    uint64_t hsh = SettingsHash();
    if (!g_hashInit) { g_hashVal = hsh; g_hashInit = true; g_dirty = false; }
    else if (hsh != g_hashVal) { g_hashVal = hsh; g_dirty = true; g_dirtyT = now; }
    if (g_dirty && g_autoSave && !down && now - g_dirtyT > 1.0) {
        SaveConfig();
        g_dirty = false;
        Toast("Tersimpan otomatis");
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // pulihkan state vertex
    for (int i = 0; i < 8; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)ab[i].buf);
        glVertexAttribPointer((GLuint)i, ab[i].size, (GLenum)ab[i].type, (GLboolean)ab[i].norm, ab[i].stride, ab[i].ptr);
        if (ab[i].en) glEnableVertexAttribArray((GLuint)i); else glDisableVertexAttribArray((GLuint)i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArray);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElem);
    while (glGetError() != GL_NO_ERROR) {}
}

static void SwapBuffers_hook() {
    if (g_menuEnabled && !g_menuFailed && (g_menuReady || InitMenu())) DrawMenuFrame();
    SwapBuffers_orig();
}

ON_MOD_PRELOAD() {
    logger->SetTag("Pearlescent");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { logger->Error("libGTASA.so tidak ditemukan"); return; }

    std::string cfg = aml->GetConfigPath();
    if (!cfg.empty() && cfg.back() != '/') cfg += '/';
    g_iniPath = cfg + "pearlescent.ini";
    g_logPath = cfg + "pearlescent_log.txt";
    { FILE* c = fopen(g_logPath.c_str(), "w"); if (c) fclose(c); }

    void* hook_target = (void*)aml->GetSym(h, "_ZN17CVehicleModelInfo20SetEditableMaterialsEP7RpClump");
    RpClumpForAllAtomics = (decltype(RpClumpForAllAtomics))aml->GetSym(h, "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_");
    RpGeometryForAllMaterials = (decltype(RpGeometryForAllMaterials))aml->GetSym(h, "_Z25RpGeometryForAllMaterialsP10RpGeometryPFP10RpMaterialS2_PvES3_");
    GetClumpModelInfo = (decltype(GetClumpModelInfo))aml->GetSym(h, "_ZN18CVisibilityPlugins17GetClumpModelInfoEP7RpClump");
    GetModelInfoByName = (decltype(GetModelInfoByName))aml->GetSym(h, "_ZN10CModelInfo12GetModelInfoEPKcPi");
    g_modelInfoPtrs = (void**)aml->GetSym(h, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    if (!GetClumpModelInfo || !g_modelInfoPtrs) logger->Error("Fitur per-mobil tidak tersedia (simbol tidak ada)");
    RwFrameGetLTM_fn = (decltype(RwFrameGetLTM_fn))aml->GetSym(h, "_Z13RwFrameGetLTMP7RwFrame");
    g_scene = (void**)aml->GetSym(h, "Scene");
    if (!RwFrameGetLTM_fn || !g_scene) logger->Error("Mode sudut tidak tersedia, memakai mode waktu");
    RpAtomicGetGeometry_fn = (decltype(RpAtomicGetGeometry_fn))aml->GetSym(h, "_Z18RpAtomicGetGeometryP8RpAtomic");

    if (!hook_target || !RpClumpForAllAtomics || !RpGeometryForAllMaterials) {
        logger->Error("Simbol tidak ditemukan (hook=%p clump=%p mat=%p). Cek dengan: nm -D libGTASA.so",
                      hook_target, RpClumpForAllAtomics, RpGeometryForAllMaterials);
        return;
    }
    g_tpl.name = "default";
    LoadConfig();
    aml->Hook(hook_target, (void*)SetEditableMaterials_hook, (void**)&SetEditableMaterials_orig);
    g_ready = true;
    if (!g_templates.count("custom")) { Template c; c.name = "custom"; c.a = {40, 120, 255}; c.b = {255, 60, 200}; c.c = {60, 220, 160}; c.hasC = true; g_templates["custom"] = c; }
    for (auto& kv : g_templates) if (g_palette.size() < 32) g_palette.push_back(kv.second);
    if (g_shaderPatchEnabled) {
        bool ok = InstallGotHook(hook_target, 0x845650, 0, (void*)glUseProgram_hook, (void**)&glUseProgram_orig, "glUseProgram")
               && InstallGotHook(hook_target, 0x8496e0, 0, (void*)glLinkProgram_hook, (void**)&glLinkProgram_orig, "glLinkProgram")
               && InstallGotHook(hook_target, 0x8497f8, 0, (void*)glShaderSource_hook, (void**)&glShaderSource_orig, "glShaderSource");
        g_shaderOK = ok;
        if (!ok) logger->Error("Mode pixel tidak aktif, memakai mode sudut");
    }
    if (g_menuEnabled) {
        GetCoords_orig = (decltype(GetCoords_orig))aml->GetSym(h, "_Z25LIB_PointerGetCoordinatesiPiS_Pf");
        FindPlayerVehicle_fn = (decltype(FindPlayerVehicle_fn))aml->GetSym(h, "_Z17FindPlayerVehicleib");
        void* any = hook_target;
        bool ok = GetCoords_orig
               && InstallGotHook(any, 0x842fd8, 0x32703c, (void*)GetButton_hook, (void**)&GetButton_orig, "LIB_PointerGetButton")
               && InstallGotHook(any, 0x83f028, 0x31e2a4, (void*)SwapBuffers_hook, (void**)&SwapBuffers_orig, "OS_ScreenSwapBuffers");
        if (!ok) { g_menuEnabled = false; logger->Error("Menu in-game tidak aktif (hook gagal)"); }
    }
    logger->Info("Pearlescent aktif. Config: %s", g_iniPath.c_str());
}
