// Pearlescent Colors - AML mod untuk GTA SA Android v2.10 (arm64-v8a)
// Warna body utama (primary) mobil bergeser halus antara warna A dan B.
#include <mod/amlmod.h>
#include <android/log.h>
#include <GLES2/gl2.h>
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
static UiRect g_uiRects[2] = { {0, 0, 0, 0, false}, {0, 0, 0, 0, false} };
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
    bool shaderPatch = g_shaderPatchEnabled, menuOn = g_menuEnabled;
    float menuScale = g_menuScale, btnX = g_btnX, btnY = g_btnY;
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
            else if (k == "menu_scale") menuScale = (float)atof(v.c_str());
            else if (k == "menu_button_x") btnX = (float)atof(v.c_str());
            else if (k == "menu_button_y") btnY = (float)atof(v.c_str());
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
    g_chamGain = chamGain; g_chamFlow = chamFlow; g_chamBias = chamBias;
    g_chamReflect = chamReflect; g_chamFresnel = chamFresnel; g_chamDepth = chamDepth; g_chamFlake = chamFlake; g_chamBright = chamBright; g_shaderPatchEnabled = shaderPatch;
    g_menuEnabled = menuOn; g_menuScale = menuScale; g_btnX = btnX; g_btnY = btnY;
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
    fprintf(f, "mode=%s\nactive=%s\nshift_mode=%s\n", g_whitelist ? "whitelist" : "all", g_tpl.name.c_str(), ShiftName(g_shiftMode));
    fprintf(f, "angle_gain=%.2f\nelev_gain=%.2f\npart_spread=%.2f\nspeed=%.2f\nintensity=%.2f\n", g_angleGain, g_elevGain, g_partSpread, g_speed, g_intensity);
    fprintf(f, "shader_patch=%d\ngeom_offset=%d\n", g_shaderPatchEnabled ? 1 : 0, g_geomOff);
    fprintf(f, "cham_gain=%.2f\ncham_flow=%.2f\ncham_bias=%.2f\ncham_reflect=%.2f\ncham_fresnel=%.2f\ncham_depth=%.2f\ncham_flake=%.2f\ncham_brightness=%.2f\n",
            g_chamGain, g_chamFlow, g_chamBias, g_chamReflect, g_chamFresnel, g_chamDepth, g_chamFlake, g_chamBright);
    fprintf(f, "menu=%d\nmenu_scale=%.2f\nmenu_button_x=%.2f\nmenu_button_y=%.2f\n", g_menuEnabled ? 1 : 0, g_menuScale, g_btnX, g_btnY);
    fprintf(f, "\n[cars]\n");
    for (auto& kv : g_carsCfg) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    auto cu = g_templates.find("custom");
    if (cu != g_templates.end()) WriteTpl(f, cu->second);
    for (auto& kv : g_templates) if (kv.first != "custom") WriteTpl(f, kv.second);
    fclose(f);
    struct stat st; if (stat(g_iniPath.c_str(), &st) == 0) g_mtime = st.st_mtime;
    logger->Info("Config disimpan dari menu");
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

static void SaveRect(int i) {
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    g_uiRects[i] = { p.x, p.y, p.x + sz.x, p.y + sz.y, true };
}

static void TouchScroll() {
    if (ImGui::IsWindowFocused() && ImGui::IsMouseDragging(0, 6.0f) && !ImGui::IsAnyItemActive())
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
}

static RGB ToRGB(const float* c) {
    return { Clamp255((int)(c[0] * 255.0f + 0.5f)), Clamp255((int)(c[1] * 255.0f + 0.5f)), Clamp255((int)(c[2] * 255.0f + 0.5f)) };
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

static void BuildUI(float W, float H) {
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(ImVec2(W * g_btnX, H * g_btnY), ImGuiCond_Always);
    ImGui::Begin("##pcbtn", nullptr, fl);
    if (ImGui::Button(g_menuOpen ? "PC  X" : "PC")) g_menuOpen = !g_menuOpen;
    SaveRect(0);
    ImGui::End();
    g_uiRects[1].valid = false;
    if (!g_menuOpen) return;

    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(W * 0.66f, H * 0.88f), ImGuiCond_Always);
    ImGui::Begin("Pearlescent Chameleon", &g_menuOpen,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    SaveRect(1);

    ImGui::Checkbox("Aktifkan efek", &g_enabled);
    ImGui::SameLine();
    if (ImGui::Button("Simpan")) SaveConfig();
    ImGui::SameLine();
    if (ImGui::Button("Muat ulang")) { LoadConfig(); g_uniVersion++; }
    ImGui::TextWrapped("Template aktif: %s | Shader per-piksel: %s", g_tpl.name.c_str(), g_shaderOK ? "aktif" : "tidak aktif");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Mode & target", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kModes[] = {"Pixel (shader)", "Sudut kamera", "Waktu", "Gabungan"};
        static const int kModeVal[] = {3, 0, 1, 2};
        int mi = 0;
        for (int i = 0; i < 4; i++) if (kModeVal[i] == g_shiftMode) mi = i;
        if (ImGui::Combo("Mode", &mi, kModes, 4)) g_shiftMode = kModeVal[mi];
        if (g_shiftMode == 3 && !g_shaderOK) ImGui::TextWrapped("Pixel belum aktif (shader_patch=0 atau hook gagal), dipakai mode sudut.");
        static const char* kTg[] = {"Semua mobil", "Hanya mobil terdaftar"};
        int tg = g_whitelist ? 1 : 0;
        if (ImGui::Combo("Target", &tg, kTg, 2)) g_whitelist = (tg == 1);
    }

    if (ImGui::CollapsingHeader("Template warna", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("tpllist", ImVec2(0, H * 0.30f), true);
        for (auto& kv : g_templates) {
            const Template& t = kv.second;
            ImGui::PushID(kv.first.c_str());
            float sz = ImGui::GetFrameHeight();
            ImGui::ColorButton("##a", ImVec4(t.a.r / 255.0f, t.a.g / 255.0f, t.a.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(sz, sz));
            ImGui::SameLine();
            ImGui::ColorButton("##b", ImVec4(t.b.r / 255.0f, t.b.g / 255.0f, t.b.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(sz, sz));
            ImGui::SameLine();
            const RGB& c3 = t.hasC ? t.c : t.b;
            ImGui::ColorButton("##c", ImVec4(c3.r / 255.0f, c3.g / 255.0f, c3.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(sz, sz));
            ImGui::SameLine();
            if (ImGui::Selectable(kv.first.c_str(), kv.first == g_tpl.name)) g_tpl = kv.second;
            ImGui::PopID();
        }
        TouchScroll();
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Mix manual (custom)")) {
        Template& cu = g_templates["custom"];
        cu.name = "custom";
        float ca[3] = { cu.a.r / 255.0f, cu.a.g / 255.0f, cu.a.b / 255.0f };
        float cb[3] = { cu.b.r / 255.0f, cu.b.g / 255.0f, cu.b.b / 255.0f };
        const RGB& c3 = cu.hasC ? cu.c : cu.b;
        float cc[3] = { c3.r / 255.0f, c3.g / 255.0f, c3.b / 255.0f };
        bool chg = false;
        chg |= ImGui::ColorEdit3("A (hadap kamera)", ca);
        chg |= ImGui::ColorEdit3("B (tengah)", cb);
        chg |= ImGui::ColorEdit3("C (tepi)", cc);
        if (chg) {
            cu.a = ToRGB(ca); cu.b = ToRGB(cb); cu.c = ToRGB(cc); cu.hasC = true;
            if (g_tpl.name == "custom") g_tpl = cu;
            g_uniVersion++;
        }
        if (ImGui::Button("Pakai custom")) g_tpl = cu;
        ImGui::SameLine();
        if (ImGui::Button("Salin template aktif")) {
            if (g_tpl.name != "custom") {
                cu.a = g_tpl.a; cu.b = g_tpl.b; cu.c = g_tpl.hasC ? g_tpl.c : g_tpl.b; cu.hasC = true;
                g_uniVersion++;
            }
        }
    }

    if (ImGui::CollapsingHeader("Efek per piksel")) {
        bool ch = false;
        ch |= ImGui::SliderFloat("Gain sudut", &g_chamGain, 0.2f, 4.0f, "%.2f");
        ch |= ImGui::SliderFloat("Aliran pantulan", &g_chamFlow, -1.0f, 1.0f, "%.2f");
        ch |= ImGui::SliderFloat("Geser awal", &g_chamBias, -0.6f, 0.6f, "%.2f");
        ch |= ImGui::SliderFloat("Kedalaman", &g_chamDepth, 0.0f, 0.8f, "%.2f");
        ch |= ImGui::SliderFloat("Kilap minimum", &g_chamReflect, 0.0f, 0.6f, "%.2f");
        ch |= ImGui::SliderFloat("Fresnel", &g_chamFresnel, 0.0f, 1.0f, "%.2f");
        ch |= ImGui::SliderFloat("Glitter", &g_chamFlake, 0.0f, 0.5f, "%.2f");
        ch |= ImGui::SliderFloat("Kecerahan", &g_chamBright, 0.3f, 1.5f, "%.2f");
        if (ch) g_uniVersion++;
    }

    if (ImGui::CollapsingHeader("Mode sudut / waktu")) {
        ImGui::SliderFloat("Gain sudut keliling", &g_angleGain, 0.5f, 6.0f, "%.2f");
        ImGui::SliderFloat("Gain elevasi", &g_elevGain, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Beda antar bagian", &g_partSpread, 0.0f, 6.0f, "%.2f");
        ImGui::SliderFloat("Kecepatan (waktu)", &g_speed, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Intensitas", &g_intensity, 0.0f, 1.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Pilih mobil")) {
        int cur = CurrentVehicleModel();
        if (cur >= 0) {
            ImGui::Text("Mobil dikendarai: model %d", cur);
            if (ImGui::Button("Daftarkan dgn template aktif")) { g_carsCfg[std::to_string(cur)] = g_tpl.name; g_needResolve = true; }
            ImGui::SameLine();
            if (ImGui::Button("Hapus mobil ini")) { g_carsCfg.erase(std::to_string(cur)); g_needResolve = true; }
        } else {
            ImGui::TextWrapped("Naiki sebuah mobil untuk mendaftarkannya.");
        }
        ImGui::Separator();
        ImGui::Text("Daftar mobil (mode 'Hanya mobil terdaftar'):");
        std::string del;
        for (auto& kv : g_carsCfg) {
            ImGui::PushID(kv.first.c_str());
            if (ImGui::Button("X")) del = kv.first;
            ImGui::SameLine();
            ImGui::Text("%s -> %s", kv.first.c_str(), kv.second.c_str());
            ImGui::PopID();
        }
        if (!del.empty()) { g_carsCfg.erase(del); g_needResolve = true; }
    }

    if (ImGui::CollapsingHeader("Tombol menu")) {
        ImGui::SliderFloat("Posisi X", &g_btnX, 0.0f, 0.95f, "%.2f");
        ImGui::SliderFloat("Posisi Y", &g_btnY, 0.0f, 0.9f, "%.2f");
    }

    TouchScroll();
    ImGui::End();
}

static bool InitMenu() {
    if (g_menuFailed) return false;
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] < 200 || vp[3] < 200) return false;
    float sc = g_menuScale > 0.0f ? g_menuScale : (float)vp[3] / 480.0f;
    if (sc < 1.0f) sc = 1.0f;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImFontConfig fc;
    fc.SizePixels = 13.0f * sc;
    io.Fonts->AddFontDefault(&fc);
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.ScaleAllSizes(sc);
    st.ScrollbarSize = 10.0f * sc;
    st.WindowRounding = 4.0f * sc;
    if (!ImGui_ImplOpenGL3_Init("#version 100")) { g_menuFailed = true; logger->Error("Init ImGui gagal"); return false; }
    g_menuReady = true;
    logger->Info("Menu ImGui siap (skala %.2f, layar %dx%d)", sc, vp[2], vp[3]);
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
