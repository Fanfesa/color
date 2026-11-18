// Pearlescent Colors - AML mod untuk GTA SA Android v2.10 (arm64-v8a)
// Warna body utama (primary) mobil bergeser halus antara warna A dan B.
#include <mod/amlmod.h>
#include <android/log.h>
#include <cstdarg>

struct SimpleLogger {
    void SetTag(const char*) {}
    void Info(const char* f, ...) { va_list a; va_start(a, f); __android_log_vprint(ANDROID_LOG_INFO, "Pearlescent", f, a); va_end(a); }
    void Error(const char* f, ...) { va_list a; va_start(a, f); __android_log_vprint(ANDROID_LOG_ERROR, "Pearlescent", f, a); va_end(a); }
};
static SimpleLogger g_logger;
#define logger (&g_logger)
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <dlfcn.h>

MYMOD(net.pearl.gtasa, Pearlescent Colors, 1.0, Pearl)

struct RGB { int r, g, b; };
struct Template { std::string name; RGB a{255,255,255}, b{255,255,255}; float intensity = 1.0f; };
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
static std::vector<void*> g_primary;

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
    if (!f) return;
    std::map<std::string, Template> t;
    std::string active = "Purple-Gold", sec;
    float speed = 0.6f, inten = 1.0f;
    Template* cur = nullptr;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            size_t e = s.find(']');
            sec = s.substr(1, e == std::string::npos ? std::string::npos : e - 1);
            cur = nullptr;
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
        } else if (cur) {
            if (k == "a") ParseRGB(v, cur->a);
            else if (k == "b") ParseRGB(v, cur->b);
            else if (k == "intensity") cur->intensity = (float)atof(v.c_str());
        }
    }
    fclose(f);
    if (t.empty()) return;
    g_templates = t;
    auto it = g_templates.find(active);
    g_tpl = (it != g_templates.end()) ? it->second : g_templates.begin()->second;
    g_speed = speed; g_intensity = inten;
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

static RGB Pearl(void* clump) {
    float phase = (float)(((uintptr_t)clump >> 4) % 1000) / 1000.0f * 6.2831853f;
    float t = 0.5f + 0.5f * sinf((float)(NowSec() * g_speed * 6.2831853) + phase);
    float k = t * g_tpl.intensity * g_intensity;
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return { Clamp255((int)(g_tpl.a.r + (g_tpl.b.r - g_tpl.a.r) * k)),
             Clamp255((int)(g_tpl.a.g + (g_tpl.b.g - g_tpl.a.g) * k)),
             Clamp255((int)(g_tpl.a.b + (g_tpl.b.b - g_tpl.a.b) * k)) };
}

// Material primary pada DFF mobil bertanda warna (60,255,0)
static void* OnMaterial(void* mat, void*) {
    RwRGBA* c = (RwRGBA*)((char*)mat + sizeof(void*)); // RpMaterial: texture* lalu color
    if (c->r == 60 && c->g == 255 && c->b == 0) g_primary.push_back(mat);
    return mat;
}
static void* OnAtomic(void* atomic, void*) {
    void* geom = RpAtomicGetGeometry_fn ? RpAtomicGetGeometry_fn(atomic)
               : *(void**)((char*)atomic + (sizeof(void*) == 8 ? 40 : 20));
    if (geom) RpGeometryForAllMaterials(geom, OnMaterial, nullptr);
    return atomic;
}

static void SetEditableMaterials_hook(void* clump) {
    g_primary.clear();
    if (g_ready && clump) { MaybeReload(); RpClumpForAllAtomics(clump, OnAtomic, nullptr); }
    SetEditableMaterials_orig(clump);
    if (!g_primary.empty()) {
        RGB p = Pearl(clump);
        for (void* m : g_primary) {
            RwRGBA* c = (RwRGBA*)((char*)m + sizeof(void*));
            c->r = p.r; c->g = p.g; c->b = p.b;
        }
    }
}

ON_MOD_PRELOAD() {
    logger->SetTag("Pearlescent");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { logger->Error("libGTASA.so tidak ditemukan"); return; }

    std::string cfg = aml->GetConfigPath();
    if (!cfg.empty() && cfg.back() != '/') cfg += '/';
    g_iniPath = cfg + "pearlescent.ini";

    void* hook_target = (void*)aml->GetSym(h, "_ZN17CVehicleModelInfo20SetEditableMaterialsEP7RpClump");
    RpClumpForAllAtomics = (decltype(RpClumpForAllAtomics))aml->GetSym(h, "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_");
    RpGeometryForAllMaterials = (decltype(RpGeometryForAllMaterials))aml->GetSym(h, "_Z25RpGeometryForAllMaterialsP10RpGeometryPFP10RpMaterialS2_PvES3_");
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
    logger->Info("Pearlescent aktif. Config: %s", g_iniPath.c_str());
}
