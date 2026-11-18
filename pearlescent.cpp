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
            else if (k == "geom_offset") geomOff = atoi(v.c_str());
            else if (k == "shift_mode") shiftMode = (v == "time") ? 1 : (v == "both" ? 2 : 0);
            else if (k == "angle_gain") angleGain = (float)atof(v.c_str());
            else if (k == "elev_gain") elevGain = (float)atof(v.c_str());
            else if (k == "part_spread") partSpread = (float)atof(v.c_str());
        } else if (sec == "cars") {
            cars[k] = v;
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
    bool ok = (g_shiftMode != 1) && ViewAngles(clump, nullptr, az0, el0);
    if (ok) {
        float az1 = az0, el1 = el0;
        if (partFrame) ViewAngles(clump, partFrame, az1, el1);
        float dAz = az1 - az0;
        if (dAz > 3.14159265f) dAz -= 6.2831853f;
        if (dAz < -3.14159265f) dAz += 6.2831853f;
        float az = az0 + dAz * g_partSpread, el = el0 + (el1 - el0) * g_partSpread;
        arg += az * g_angleGain + el * g_elevGain;
    }
    if (g_shiftMode != 0 || !ok) {
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
        void* lastFrame = (void*)1; RGB p{0, 0, 0};
        for (const PrimMat& pm : g_primary) {
            if (pm.frame != lastFrame) { p = Pearl(clump, pm.frame, *tp); lastFrame = pm.frame; }
            RwRGBA* c = (RwRGBA*)((char*)pm.mat + sizeof(void*));
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
    logger->Info("Pearlescent aktif. Config: %s", g_iniPath.c_str());
}
