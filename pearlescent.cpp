// Pearlescent Colors - AML mod untuk GTA SA Android v2.10 (arm64-v8a)
// Warna body utama (primary) mobil bergeser halus antara warna A dan B.
#include <mod/amlmod.h>
#include <android/log.h>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cctype>
#include <set>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <dlfcn.h>

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

static const char* kDefaultIni = R"(; Pearlescent Colors (dibuat otomatis)
[settings]
mode=all
active=Purple-Gold
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
        } else if (sec == "cars") {
            cars[k] = v;
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

static RGB Pearl(void* clump, const Template& tp) {
    float phase = (float)(((uintptr_t)clump >> 4) % 1000) / 1000.0f * 6.2831853f;
    float t = 0.5f + 0.5f * sinf((float)(NowSec() * g_speed * 6.2831853) + phase);
    float k = t * tp.intensity * g_intensity;
    k = k < 0 ? 0 : (k > 1 ? 1 : k);
    return { Clamp255((int)(tp.a.r + (tp.b.r - tp.a.r) * k)),
             Clamp255((int)(tp.a.g + (tp.b.g - tp.a.g) * k)),
             Clamp255((int)(tp.a.b + (tp.b.b - tp.a.b) * k)) };
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
    if (g_ready && clump) { MaybeReload(); RpClumpForAllAtomics(clump, OnAtomic, nullptr); }
    SetEditableMaterials_orig(clump);
    if (g_ready && clump && g_loggedIds.size() < 40) {
        int mid = ModelIdOf(clump);
        if (g_loggedIds.insert(mid).second)
            logger->Info("kendaraan model id=%d, material primary ditemukan=%d, whitelist=%d", mid, (int)g_primary.size(), (int)g_whitelist);
    }
    if (!g_primary.empty()) {
        if (g_needResolve) ResolveCars();
        const Template* tp = &g_tpl;
        auto car = g_carById.find(ModelIdOf(clump));
        if (car != g_carById.end()) {
            auto tt = g_templates.find(car->second);
            if (tt != g_templates.end()) tp = &tt->second;
        } else if (g_whitelist) {
            return;  // mobil lain tidak diubah
        }
        RGB p = Pearl(clump, *tp);
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
    g_logPath = cfg + "pearlescent_log.txt";
    { FILE* c = fopen(g_logPath.c_str(), "w"); if (c) fclose(c); }

    void* hook_target = (void*)aml->GetSym(h, "_ZN17CVehicleModelInfo20SetEditableMaterialsEP7RpClump");
    RpClumpForAllAtomics = (decltype(RpClumpForAllAtomics))aml->GetSym(h, "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_");
    RpGeometryForAllMaterials = (decltype(RpGeometryForAllMaterials))aml->GetSym(h, "_Z25RpGeometryForAllMaterialsP10RpGeometryPFP10RpMaterialS2_PvES3_");
    GetClumpModelInfo = (decltype(GetClumpModelInfo))aml->GetSym(h, "_ZN18CVisibilityPlugins17GetClumpModelInfoEP7RpClump");
    GetModelInfoByName = (decltype(GetModelInfoByName))aml->GetSym(h, "_ZN10CModelInfo12GetModelInfoEPKcPi");
    g_modelInfoPtrs = (void**)aml->GetSym(h, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    if (!GetClumpModelInfo || !g_modelInfoPtrs) logger->Error("Fitur per-mobil tidak tersedia (simbol tidak ada)");
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
