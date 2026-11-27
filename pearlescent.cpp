// Pearlescent Colors - AML mod untuk GTA SA Android v2.10 (arm64-v8a)
// Warna body utama (primary) mobil bergeser halus antara warna A dan B.
#include <mod/amlmod.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
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
#include <cstdlib>
#include <cstdint>
#include <set>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <ctime>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <strings.h>
#include <sys/mman.h>

MYMOD(net.pearl.gtasa, Pearlescent Colors, 1.0, Pearl)

// ---- tulis file secara asinkron ----
// Storage emulasi (FUSE) lambat: satu fopen/fclose bisa memakan puluhan ms. Semua penulisan file
// (log, simpan config, dump shader) dikerjakan thread terpisah agar rendering tidak tersendat.
struct IoJob { std::string path, data; bool append; bool markIni; };
static std::mutex g_ioMtx;
static std::condition_variable g_ioCv;
static std::deque<IoJob> g_ioQ;
static bool g_ioStarted = false;
static std::atomic<bool> g_reloadPending(false);   // file ini berubah dari luar
static std::atomic<long long> g_mtimeA(0);         // mtime ini terakhir yang diketahui
static std::string g_iniWatch;

static void IoWorker() {
    for (;;) {
        std::deque<IoJob> jobs;
        {
            std::unique_lock<std::mutex> lk(g_ioMtx);
            g_ioCv.wait_for(lk, std::chrono::seconds(2), [] { return !g_ioQ.empty(); });
            jobs.swap(g_ioQ);
        }
        for (const IoJob& j : jobs) {
            FILE* fp = fopen(j.path.c_str(), j.append ? "a" : "w");
            if (fp) { fwrite(j.data.data(), 1, j.data.size(), fp); fclose(fp); }
            if (j.markIni) { struct stat st; if (stat(j.path.c_str(), &st) == 0) g_mtimeA.store((long long)st.st_mtime); }
        }
        if (!g_iniWatch.empty()) {   // pantau perubahan ini dari luar (edit manual)
            struct stat st;
            if (stat(g_iniWatch.c_str(), &st) == 0) {
                long long m = (long long)st.st_mtime, known = g_mtimeA.load();
                if (known == 0) g_mtimeA.store(m);
                else if (m != known) { g_mtimeA.store(m); g_reloadPending.store(true); }
            }
        }
    }
}
static void IoStart() { if (g_ioStarted) return; g_ioStarted = true; std::thread(IoWorker).detach(); }
static void IoPost(const std::string& path, const std::string& data, bool append, bool markIni) {
    {
        std::lock_guard<std::mutex> lk(g_ioMtx);
        if (g_ioQ.size() > 400) return;
        g_ioQ.push_back({path, data, append, markIni});
    }
    g_ioCv.notify_one();
}

static std::string g_logPath;
struct SimpleLogger {
    void SetTag(const char*) {}
    void Write(int prio, const char* tag, const char* f, va_list a) {
        char buf[512];
        vsnprintf(buf, sizeof buf, f, a);
        __android_log_print(prio, "Pearlescent", "%s", buf);
        if (!g_logPath.empty()) IoPost(g_logPath, std::string("[") + tag + "] " + buf + "\n", true, false);
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

// ---- slot custom: 8 slot, tiap slot punya 3 warna sendiri (bisa dipakai mobil berbeda) ----
static std::string g_curCustom = "custom";   // slot custom yang sedang diedit
static bool IsCustomName(const std::string& n, int* slot = nullptr) {
    if (n == "custom") { if (slot) *slot = 0; return true; }
    if (n.size() == 7 && n.compare(0, 6, "custom") == 0 && n[6] >= '2' && n[6] <= '8') { if (slot) *slot = n[6] - '1'; return true; }
    return false;
}
static std::string CustomName(int slot) { return slot <= 0 ? std::string("custom") : "custom" + std::to_string(slot + 1); }
static const RGB kCustomDef[8][3] = {
    {{40, 120, 255}, {255, 60, 200}, {60, 220, 160}},
    {{255, 120, 40}, {200, 40, 160}, {60, 60, 220}},
    {{30, 200, 120}, {30, 120, 220}, {150, 60, 220}},
    {{220, 60, 60}, {240, 180, 40}, {60, 200, 200}},
    {{120, 40, 200}, {40, 200, 220}, {240, 100, 160}},
    {{20, 20, 60}, {80, 40, 160}, {200, 60, 200}},
    {{200, 200, 60}, {60, 180, 80}, {40, 100, 200}},
    {{240, 240, 240}, {160, 140, 230}, {100, 200, 220}},
};
static void EnsureCustomSlots() {
    for (int i = 0; i < 8; i++) {
        std::string n = CustomName(i);
        auto it = g_templates.find(n);
        if (it == g_templates.end()) {
            Template t; t.name = n; t.a = kCustomDef[i][0]; t.b = kCustomDef[i][1]; t.c = kCustomDef[i][2]; t.hasC = true;
            g_templates[n] = t;
        } else {
            it->second.name = n;
            if (!it->second.hasC) { it->second.c = it->second.b; it->second.hasC = true; }
        }
    }
}
static Template& CustomT() {
    Template& t = g_templates[g_curCustom];
    t.name = g_curCustom;
    if (!t.hasC) { t.c = t.b; t.hasC = true; }
    return t;
}
static std::map<std::string, int> g_keyToId;   // kunci nama di [cars] -> id model (diisi ResolveCars)
// ---- nama kendaraan bawaan game (ID 400-611). Untuk model tambahan ditampilkan sebagai "Kendaraan kustom". ----
static const char* const kVehNames[212] = {
    "Landstalker", "Bravura", "Buffalo", "Linerunner", "Perennial", "Sentinel", "Dumper", "Firetruck",
    "Trashmaster", "Stretch", "Manana", "Infernus", "Voodoo", "Pony", "Mule", "Cheetah", "Ambulance",
    "Leviathan", "Moonbeam", "Esperanto", "Taxi", "Washington", "Bobcat", "Mr. Whoopee", "BF Injection",
    "Hunter", "Premier", "Enforcer", "Securicar", "Banshee", "Predator", "Bus", "Rhino", "Barracks",
    "Hotknife", "Artic Trailer 1", "Previon", "Coach", "Cabbie", "Stallion", "Rumpo", "RC Bandit", "Romero",
    "Packer", "Monster", "Admiral", "Squalo", "Seasparrow", "Pizzaboy", "Tram", "Artic Trailer 2", "Turismo",
    "Speeder", "Reefer", "Tropic", "Flatbed", "Yankee", "Caddy", "Solair", "Berkley's RC Van", "Skimmer",
    "PCJ-600", "Faggio", "Freeway", "RC Baron", "RC Raider", "Glendale", "Oceanic", "Sanchez", "Sparrow",
    "Patriot", "Quad", "Coastguard", "Dinghy", "Hermes", "Sabre", "Rustler", "ZR-350", "Walton", "Regina",
    "Comet", "BMX", "Burrito", "Camper", "Marquis", "Baggage", "Dozer", "Maverick", "News Chopper", "Rancher",
    "FBI Rancher", "Virgo", "Greenwood", "Jetmax", "Hotring Racer", "Sandking", "Blista Compact",
    "Police Maverick", "Boxville", "Benson", "Mesa", "RC Goblin", "Hotring Racer A", "Hotring Racer B",
    "Bloodring Banger", "Rancher Lure", "Super GT", "Elegant", "Journey", "Bike", "Mountain Bike", "Beagle",
    "Cropduster", "Stuntplane", "Tanker", "Roadtrain", "Nebula", "Majestic", "Buccaneer", "Shamal", "Hydra",
    "FCR-900", "NRG-500", "HPV1000", "Cement Truck", "Tow Truck", "Fortune", "Cadrona", "FBI Truck",
    "Willard", "Forklift", "Tractor", "Combine Harvester", "Feltzer", "Remington", "Slamvan", "Blade",
    "Freight", "Brownstreak", "Vortex", "Vincent", "Bullet", "Clover", "Sadler", "Firetruck LA", "Hustler",
    "Intruder", "Primo", "Cargobob", "Tampa", "Sunrise", "Merit", "Utility Van", "Nevada", "Yosemite",
    "Windsor", "Monster A", "Monster B", "Uranus", "Jester", "Sultan", "Stratum", "Elegy", "Raindance",
    "RC Tiger", "Flash", "Tahoma", "Savanna", "Bandito", "Freight Flat Trailer", "Streak Trailer", "Kart",
    "Mower", "Dune", "Sweeper", "Broadway", "Tornado", "AT-400", "DFT-30", "Huntley", "Stafford", "BF-400",
    "Newsvan", "Tug", "Petrol Trailer", "Emperor", "Wayfarer", "Euros", "Hotdog", "Club",
    "Freight Box Trailer", "Artic Trailer 3", "Andromada", "Dodo", "RC Cam", "Launch", "Police LS",
    "Police SF", "Police LV", "Police Ranger", "Picador", "S.W.A.T.", "Alpha", "Phoenix", "Glendale Damaged",
    "Sadler Damaged", "Baggage Trailer A", "Baggage Trailer B", "Tug Stairs Trailer", "Boxville Mission",
    "Farm Trailer", "Utility Trailer",
};
static const char* VehicleName(int id) { return (id >= 400 && id <= 611) ? kVehNames[id - 400] : nullptr; }
static std::string VehicleLabel(int id) {
    const char* n = VehicleName(id);
    return n ? std::string(n) + " (ID " + std::to_string(id) + ")" : "Kendaraan kustom (ID " + std::to_string(id) + ")";
}
static std::string KeyLabel(const std::string& key) {   // kunci di [cars]: ID angka atau nama model
    bool numeric = !key.empty() && isdigit((unsigned char)key[0]);
    int id = -1;
    if (numeric) id = atoi(key.c_str());
    else { auto it = g_keyToId.find(key); if (it != g_keyToId.end()) id = it->second; }
    if (id < 0) return key;
    return numeric ? VehicleLabel(id) : key + " -> " + VehicleLabel(id);
}
static std::string DisplayName(const std::string& n) {
    int sl = 0;
    if (IsCustomName(n, &sl)) return "Custom " + std::to_string(sl + 1);
    std::string r = n;
    for (char& ch : r) if (ch == '-') ch = ' ';
    return r;
}
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
static bool  g_tutorialDone = false;       // panduan awal sudah ditutup
static bool  g_debugLog = false;           // ini: debug_log (log rinci per kendaraan/shader)
static bool  g_crashGuard = true;          // ini: crash_guard (lewati panggilan RpClumpForAllAtomics dengan clump NULL)
static bool  g_aa = false;                 // anti-aliasing (FXAA) aktif
static int   g_aaQuality = 1;              // 0 rendah, 1 sedang, 2 tinggi
static float g_aaStrength = 1.0f;          // 0.1 - 1.0
static bool  g_aaFailed = false;           // AA tidak didukung perangkat
static int   g_randomMode = 0;             // warna acak: 0 mati, 1 hanya mobil NPC, 2 semua mobil
static int   g_randPool = 0;               // sumber acak: 0 semua, 1 gelap, 2 sedang, 3 terang
static bool  g_randCustom = false;         // sertakan slot custom pada sumber acak
static bool  g_lockOwned = true;           // mobil yang pernah dikendarai pemain tidak ikut diacak
static bool  g_keepNpcColor = true;        // mobil NPC yang dinaiki pemain mempertahankan warna acaknya
static bool  g_spawnOwn = true;            // mobil hasil spawn skrip (CLEO) langsung memakai warna pilihan pemain
static bool  g_randDirty = true;           // daftar sumber acak perlu dibangun ulang
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
    bool shaderPatch = g_shaderPatchEnabled, menuOn = g_menuEnabled, enabledV = g_enabled, autoSaveV = g_autoSave, tutorialV = g_tutorialDone, debugV = g_debugLog, aaV = g_aa, guardV = g_crashGuard;
    int aaQ = g_aaQuality; float aaS = g_aaStrength;
    int randMode = g_randomMode, randPool = g_randPool; bool randCustom = g_randCustom, randLock = g_lockOwned, randKeep = g_keepNpcColor, randSpawn = g_spawnOwn;
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
            else if (IsCustomName(sec)) name = sec;
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
            else if (k == "tutorial_done") tutorialV = (atoi(v.c_str()) != 0);
            else if (k == "debug_log") debugV = (atoi(v.c_str()) != 0);
            else if (k == "crash_guard") guardV = (atoi(v.c_str()) != 0);
            else if (k == "aa") aaV = (atoi(v.c_str()) != 0);
            else if (k == "aa_quality") aaQ = atoi(v.c_str());
            else if (k == "aa_strength") aaS = (float)atof(v.c_str());
            else if (k == "random_mode") randMode = atoi(v.c_str());
            else if (k == "random_pool") randPool = atoi(v.c_str());
            else if (k == "random_custom") randCustom = (atoi(v.c_str()) != 0);
            else if (k == "random_lock") randLock = (atoi(v.c_str()) != 0);
            else if (k == "random_keep") randKeep = (atoi(v.c_str()) != 0);
            else if (k == "random_spawn") randSpawn = (atoi(v.c_str()) != 0);
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
    EnsureCustomSlots();
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
    g_enabled = enabledV; g_autoSave = autoSaveV; g_tutorialDone = tutorialV; g_debugLog = debugV;
    g_crashGuard = guardV; g_aa = aaV;
    g_aaQuality = aaQ < 0 ? 0 : (aaQ > 2 ? 2 : aaQ);
    g_aaStrength = aaS < 0.1f ? 0.1f : (aaS > 1.0f ? 1.0f : aaS);
    g_randomMode = randMode < 0 ? 0 : (randMode > 2 ? 2 : randMode);
    g_randPool = randPool < 0 ? 0 : (randPool > 3 ? 3 : randPool);
    g_randCustom = randCustom; g_lockOwned = randLock; g_keepNpcColor = randKeep; g_spawnOwn = randSpawn; g_randDirty = true;
    for (int i = 0; i < 6; i++) g_presets[i] = pre[i];
    g_hashInit = false;
    g_uniVersion++;
    g_whitelist = whitelist; g_carsCfg = cars; g_carById.clear(); g_needResolve = true;
    { struct stat st; if (stat(g_iniPath.c_str(), &st) == 0) g_mtimeA.store((long long)st.st_mtime); }
    logger->Info("Config dimuat. Template aktif: %s", g_tpl.name.c_str());
}

static void MaybeReload() {
    if (g_reloadPending.exchange(false)) LoadConfig();   // dipicu thread I/O bila file diubah dari luar
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
    if (g_debugLog && logged < 8) { logged++; logger->Info("pearl: angle_ok=%d az0=%.2f el0=%.2f arg=%.2f t=%.2f bagian=%p", (int)ok, az0, el0, arg, t, partFrame); }
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
    g_keyToId.clear();
    for (auto& kv : g_carsCfg) {
        const std::string& k = kv.first;
        int id = -1;
        if (!k.empty() && isdigit((unsigned char)k[0])) id = atoi(k.c_str());
        else if (GetModelInfoByName) { int out = -1; if (GetModelInfoByName(k.c_str(), &out) && out >= 0) id = out; }
        if (id >= 0) { g_carById[id] = kv.second; g_keyToId[k] = id; }
        else logger->Error("Mobil tidak ditemukan: %s", k.c_str());
    }
    g_needResolve = false;
}

static int LumClass(const Template& t) {   // 1 = gelap, 2 = sedang, 3 = terang
    RGB c3 = t.hasC ? t.c : t.b;
    float l = ((t.a.r + t.b.r + c3.r) * 0.299f + (t.a.g + t.b.g + c3.g) * 0.587f + (t.a.b + t.b.b + c3.b) * 0.114f) / (3.0f * 255.0f);
    return l < 0.30f ? 1 : (l > 0.52f ? 3 : 2);
}

// ---------- warna acak otomatis per kendaraan ----------
static int g_clumpOff = -1;                       // offset pointer clump di objek CVehicle (dideteksi otomatis)
static std::unordered_set<void*> g_seenClumps;
struct RandAssign { std::string name; int model; bool forced; };
static std::unordered_map<void*, RandAssign> g_randAssign;
static std::vector<std::string> g_randNames;
struct OwnedEntry { void* clump; int model; };
static std::vector<OwnedEntry> g_owned;           // mobil yang pernah dikendarai pemain (maks 8 terakhir)
static void MarkOwned(void* clump, int model) {
    for (OwnedEntry& e : g_owned) if (e.clump == clump) { e.model = model; return; }
    if (g_owned.size() >= 8) g_owned.erase(g_owned.begin());
    g_owned.push_back({clump, model});
}
static bool IsOwned(void* clump, int model) {
    for (const OwnedEntry& e : g_owned) if (e.clump == clump && e.model == model) return true;
    return false;
}

static bool InPalette(const std::string& n) { for (const Template& t : g_palette) if (t.name == n) return true; return false; }

static void RebuildRandPool() {
    g_randNames.clear();
    for (auto& kv : g_templates) {
        bool cust = IsCustomName(kv.first);
        if (cust) { if (!g_randCustom) continue; }
        else {
            if (g_randPool != 0 && LumClass(kv.second) != g_randPool) continue;
            if (!InPalette(kv.first)) continue;   // hanya template yang ada di palet shader
        }
        g_randNames.push_back(kv.first);
    }
    g_randDirty = false;
}

static const Template* RandomTemplateFor(void* clump, int model, bool forced = false) {
    auto it = g_randAssign.find(clump);
    if (it != g_randAssign.end() && it->second.model == model) {
        auto t = g_templates.find(it->second.name);
        if (t != g_templates.end()) return &t->second;
    }
    if (g_randDirty) RebuildRandPool();
    if (g_randNames.empty()) return nullptr;
    if (g_randAssign.size() > 400) g_randAssign.clear();
    static int last = -1;
    int n = (int)g_randNames.size(), idx = 0;
    if (n > 1) { do { idx = rand() % n; } while (idx == last); }
    last = idx;
    g_randAssign[clump] = { g_randNames[idx], model, forced };
    auto t = g_templates.find(g_randNames[idx]);
    return t != g_templates.end() ? &t->second : nullptr;
}

// cari offset pointer clump di CVehicle dengan mencocokkan dengan clump yang pernah dirender
static void CalibrateClumpOffset(void* clump) {
    if (g_clumpOff >= 0 || !FindPlayerVehicle_fn) return;
    if (g_seenClumps.size() > 256) g_seenClumps.clear();
    g_seenClumps.insert(clump);
    void* v = FindPlayerVehicle_fn(-1, false);
    if (!v) return;
    for (int off = 8; off <= 0x78; off += 8) {
        void* pp = *(void**)((char*)v + off);
        if (pp && g_seenClumps.count(pp)) {
            g_clumpOff = off;
            g_seenClumps.clear();
            logger->Info("Offset clump kendaraan terdeteksi: 0x%x", off);
            return;
        }
    }
}

struct V3f { float x, y, z; };                       // CVector (HFA: dikirim lewat register float seperti aslinya)
static std::vector<void*> g_pendingVeh;               // CVehicle* hasil CreateCarForScript yang belum dicocokkan dengan clump
static std::unordered_map<void*, int> g_scriptClumps; // clump -> model, untuk mobil hasil spawn skrip (CLEO/misi)
static std::unordered_set<void*> g_npcSeen;           // clump yang sudah dipastikan bukan hasil spawn skrip
static void* (*CreateCarForScript_orig)(int, V3f, unsigned char) = nullptr;
static void* CreateCarForScript_hook(int model, V3f pos, unsigned char cleanup) {
    void* veh = CreateCarForScript_orig(model, pos, cleanup);
    if (veh) {
        if (g_pendingVeh.size() >= 16) g_pendingVeh.erase(g_pendingVeh.begin());
        g_pendingVeh.push_back(veh);
    }
    return veh;
}
static bool IsScriptCar(void* clump, int model) {
    auto it = g_scriptClumps.find(clump);
    if (it != g_scriptClumps.end()) {
        if (it->second == model || it->second < 0 || model < 0) return true;
        g_scriptClumps.erase(it);                      // alamat dipakai ulang oleh model lain
    }
    if (g_npcSeen.count(clump)) return false;
    for (size_t i = 0; i < g_pendingVeh.size(); i++) {
        const char* v = (const char*)g_pendingVeh[i];
        for (int off = 8; off <= 0x78; off += 8) {
            if (*(void* const*)(v + off) == clump) {
                if (g_clumpOff < 0) { g_clumpOff = off; logger->Info("Offset clump kendaraan terdeteksi (spawn): 0x%x", off); }
                if (g_scriptClumps.size() > 256) g_scriptClumps.clear();
                g_scriptClumps[clump] = model;
                g_pendingVeh.erase(g_pendingVeh.begin() + (long)i);
                if (g_debugLog) logger->Info("Mobil hasil spawn skrip terdeteksi (model %d)", model);
                return true;
            }
        }
    }
    if (g_npcSeen.size() > 1024) g_npcSeen.clear();
    g_npcSeen.insert(clump);
    return false;
}

static bool HasRandAssign(void* clump, int model) {
    auto it = g_randAssign.find(clump);
    return it != g_randAssign.end() && it->second.model == model;
}

// 1 = mobil yang sedang dikendarai pemain, 0 = bukan, -1 = belum diketahui
static int PlayerClumpState(void* clump) {
    if (!FindPlayerVehicle_fn) return -1;
    void* v = FindPlayerVehicle_fn(-1, false);
    if (!v) return 0;
    if (g_clumpOff < 0) return -1;
    return (*(void**)((char*)v + g_clumpOff) == clump) ? 1 : 0;
}

static void* CurrentVehicleClump() {
    if (!FindPlayerVehicle_fn || g_clumpOff < 0) return nullptr;
    void* v = FindPlayerVehicle_fn(-1, false);
    return v ? *(void**)((char*)v + g_clumpOff) : nullptr;
}

static void SetEditableMaterials_hook(void* clump) {
    if (!clump) return;   // cegah crash: game tidak boleh memproses clump NULL
    if (g_ready && g_clumpOff < 0) CalibrateClumpOffset(clump);
    g_primary.clear();
    g_matSeen = 0;
    bool active = g_ready && g_enabled && clump;
    const Template* tp = &g_tpl;
    int mid = -1;
    bool scriptCar = false;
    if (g_ready && g_spawnOwn) { mid = ModelIdOf(clump); scriptCar = IsScriptCar(clump, mid); }   // klasifikasi pada penampakan pertama
    if (active) {
        MaybeReload();
        if (g_needResolve) ResolveCars();
        if (mid < 0) mid = ModelIdOf(clump);
        auto car = g_carById.find(mid);
        if (car != g_carById.end()) {
            auto tt = g_templates.find(car->second);
            if (tt != g_templates.end()) tp = &tt->second;
        } else if (g_whitelist) {
            active = false;   // mobil lain: tidak disentuh sama sekali (hemat CPU, tanpa jalan-jalan di material)
        }
        if (active && g_randomMode != 0) {   // warna acak: tetap untuk tiap mobil selama mobil itu ada
            int ps = PlayerClumpState(clump);
            if (ps == 1) MarkOwned(clump, mid);   // ingat mobil yang pernah dikendarai pemain
            bool doRandom;
            bool assigned = HasRandAssign(clump, mid);
            bool forced = assigned && g_randAssign[clump].forced;                          // diacak manual lewat menu
            if (g_randomMode == 2) doRandom = true;                                        // semua mobil diacak
            else if (forced) doRandom = true;
            else if (scriptCar) doRandom = false;                                          // mobil hasil spawn skrip (CLEO): langsung warna pilihanmu
            else if (g_keepNpcColor && assigned) doRandom = true;                          // mobil NPC yang sudah punya warna acak: tetap, juga saat dinaiki
            else doRandom = (ps == 0) && !(g_lockOwned && IsOwned(clump, mid));            // NPC baru: beri warna acak; mobil pemain: warna pilihan
            if (doRandom) { const Template* rt = RandomTemplateFor(clump, mid); if (rt) tp = rt; }
        }
    }
    if (active) RpClumpForAllAtomics(clump, OnAtomic, nullptr);
    SetEditableMaterials_orig(clump);
    if (g_debugLog && g_ready && clump && g_loggedIds.size() < 40) {
        int mid = ModelIdOf(clump);
        if (g_loggedIds.insert(mid).second)
            logger->Info("kendaraan model id=%d, material dilihat=%d, primary ditemukan=%d, whitelist=%d, geom_offset=%d", mid, g_matSeen, (int)g_primary.size(), (int)g_whitelist, g_geomOff);
    }
    if (!active || g_primary.empty()) return;
    {
        if (g_shiftMode == 3 && g_shaderOK) {
            // tandai material primary; shader menghitung warna per piksel
            int cslot = 0;
            bool isCustom = IsCustomName(tp->name, &cslot);
            int idx = isCustom ? cslot : PaletteIndex(tp->name);
            for (const PrimMat& pm : g_primary) {
                RwRGBA* c = (RwRGBA*)((char*)pm.mat + sizeof(void*));
                c->r = isCustom ? 5 : 1; c->g = 253; c->b = (unsigned char)(idx * 8);
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
        if (tp.hasC) s += "mix(mix(" + V3(tp.a) + "," + V3(tp.b) + ",u1)," + V3(tp.c) + ",u2);\n";
        else         s += "mix(" + V3(tp.a) + "," + V3(tp.b) + ",us);\n";
    }
    for (int k = 0; k < 8; k++) {
        char b[160];
        snprintf(b, sizeof b, " if (idx < %d.5) return mix(mix(ChamA%d,ChamB%d,u1),ChamC%d,u2);\n", 100 + k, k, k, k);
        s += b;
    }
    s += " return vec3(1.0);\n}\n";
    return s;
}

static void ReplaceAll(std::string& s, const std::string& a, const std::string& b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}

// Penanda material primary di vertex shader: 0 = bukan penanda, 1..32 = template, 101..108 = slot custom
static const char* kMarkerExpr =
    "(abs(MaterialDiffuse.y * 255.0 - 253.0) < 1.5) ? ((abs(MaterialDiffuse.x * 255.0 - 1.0) < 1.5) ? floor(MaterialDiffuse.z * 255.0 / 8.0 + 0.5) + 1.0 : "
    "((abs(MaterialDiffuse.x * 255.0 - 5.0) < 1.5) ? floor(MaterialDiffuse.z * 255.0 / 8.0 + 0.5) + 101.0 : 0.0)) : 0.0";

// Semua shader berpencahayaan (yang memakai Out_Color) dipatch sepasang: vertex shader mengirim penanda + normal + arah
// pandang, fragment shader menghitung warna chameleon PER PIKSEL. Dengan begitu model LOD kendaraan jarak jauh
// (shader tanpa pantulan) juga memakai efek per piksel, bukan per vertex dan bukan hijau polos.
static bool PatchShader(std::string& src) {
    const size_t np = std::string::npos;
    const std::string varyings = "varying mediump float Out_Cham;\nvarying mediump vec3 Out_WN;\nvarying mediump vec3 Out_VV;\n";
    std::string uniforms = "uniform mediump vec4 ChamP0;\nuniform mediump vec4 ChamP1;\n";
    for (int k = 0; k < 8; k++) {
        char b[160];
        snprintf(b, sizeof b, "uniform mediump vec3 ChamA%d;\nuniform mediump vec3 ChamB%d;\nuniform mediump vec3 ChamC%d;\n", k, k, k);
        uniforms += b;
    }
    size_t m = src.find("void main()");
    if (m == np) return false;

    if (src.find("gl_Position") != np) {                       // vertex shader
        if (src.find("Out_Color =") == np) return false;        // hanya shader yang menulis Out_Color
        size_t endMain = src.rfind('}');
        if (endMain == np || endMain < m) return false;
        bool hasMD = src.find("uniform lowp vec4 MaterialDiffuse") != np;
        bool hasN = src.find("vec3 WorldNormal") != np && src.find("vec3 WorldNormal = vec3(0.0, 0.0, 0.0)") == np;
        bool hasV = src.find("vec4 WorldPos") != np && src.find("uniform vec3 CameraPosition") != np;
        std::string blk = std::string("Out_Cham = ") + (hasMD ? std::string("(") + kMarkerExpr + ")" : std::string("0.0")) + ";\n";
        blk += hasN ? "Out_WN = WorldNormal;\n" : "Out_WN = vec3(0.0, 0.0, 1.0);\n";
        blk += hasV ? "Out_VV = normalize(WorldPos.xyz - CameraPosition.xyz);\n" : "Out_VV = vec3(0.0, 1.0, 0.0);\n";
        src.insert(endMain, blk);
        src.insert(m, varyings);
        return true;
    }
    if (src.find("gl_FragColor") != np) {                      // fragment shader
        if (src.find("Out_Color") == np) return false;
        bool hasEnv = src.find("EnvMapCoefficient") != np;     // varian dengan pantulan lingkungan
        size_t br = src.find('{', m);
        if (br == np) return false;
        std::string head = src.substr(0, m), mid = src.substr(m, br + 1 - m), body = src.substr(br + 1);
        ReplaceAll(body, "Out_Color", "CCol");
        if (hasEnv) ReplaceAll(body, "EnvMapCoefficient", "CEnv");
        std::string block =
            "lowp vec4 CCol = Out_Color;\n";
        if (hasEnv) block += "lowp float CEnv = EnvMapCoefficient;\n";
        block +=
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
            " CCol.xyz = ChamPal(Out_Cham - 1.0, cT) * (cL * cS * ChamP1.w) * (1.0 + (cG - 0.5) * ChamP1.z);\n";
        if (hasEnv) block += " CEnv = clamp(max(EnvMapCoefficient, ChamP1.x) + cF * cF * ChamP1.y, 0.0, 0.7);\n";
        block += "}\n";
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
                IoPost(dump, "===== shader #" + std::to_string(g_shaderPatched) + " =====\n" + src + "\n", g_shaderPatched != 1, false);
            }
            if (g_debugLog) logger->Info("Shader dipatch (#%d)", g_shaderPatched);
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

// ---------- pelindung crash: clump / geometry NULL ----------
static void* (*RpClumpForAllAtomics_orig)(void*, void*, void*) = nullptr;
static void* (*RpGeometryForAllMaterials_orig)(void*, void*, void*) = nullptr;
static int g_nullGuardHits = 0;
static void LogNullCaller(const char* fn, void* ra) {
    if (g_nullGuardHits >= 6) return;
    g_nullGuardHits++;
    Dl_info di; const char* lib = "?"; long off = 0;
    if (dladdr(ra, &di) && di.dli_fname) { lib = di.dli_fname; off = (long)((char*)ra - (char*)di.dli_fbase); }
    logger->Error("%s(NULL) dilewati untuk mencegah crash. Pemanggil: %s + 0x%lx", fn, lib, off);
}
static void* RpClumpForAllAtomics_guard(void* clump, void* cb, void* data) {
    if (!clump) { LogNullCaller("RpClumpForAllAtomics", __builtin_return_address(0)); return nullptr; }
    return RpClumpForAllAtomics_orig(clump, cb, data);
}
static void* RpGeometryForAllMaterials_guard(void* geom, void* cb, void* data) {
    if (!geom) { LogNullCaller("RpGeometryForAllMaterials", __builtin_return_address(0)); return nullptr; }
    return RpGeometryForAllMaterials_orig(geom, cb, data);
}

// ---------- uniform live untuk shader chameleon ----------
static void (*glUseProgram_orig)(unsigned) = nullptr;
static void (*glLinkProgram_orig)(unsigned) = nullptr;
struct ProgLocs { int p0 = -1, p1 = -1, ca[8], cb[8], cc[8]; unsigned ver = 0; bool known = false; ProgLocs() { for (int i = 0; i < 8; i++) ca[i] = cb[i] = cc[i] = -1; } };
static std::map<unsigned, ProgLocs> g_progs;

static void SetChamUniforms(const ProgLocs& L) {
    glUniform4f(L.p0, g_chamGain, g_chamFlow, g_chamBias, g_chamDepth);
    if (L.p1 >= 0) glUniform4f(L.p1, g_chamReflect, g_chamFresnel, g_chamFlake, g_chamBright);
    for (int i = 0; i < 8; i++) {
        RGB a{255, 255, 255}, b = a, c = a;
        auto it = g_templates.find(CustomName(i));
        if (it != g_templates.end()) { a = it->second.a; b = it->second.b; c = it->second.hasC ? it->second.c : it->second.b; }
        if (L.ca[i] >= 0) glUniform3f(L.ca[i], a.r / 255.0f, a.g / 255.0f, a.b / 255.0f);
        if (L.cb[i] >= 0) glUniform3f(L.cb[i], b.r / 255.0f, b.g / 255.0f, b.b / 255.0f);
        if (L.cc[i] >= 0) glUniform3f(L.cc[i], c.r / 255.0f, c.g / 255.0f, c.b / 255.0f);
    }
}

static void glUseProgram_hook(unsigned prog) {
    glUseProgram_orig(prog);
    if (!prog || g_shaderPatched == 0) return;
    ProgLocs& L = g_progs[prog];
    if (!L.known) {
        L.p0 = glGetUniformLocation(prog, "ChamP0");
        if (L.p0 >= 0) {   // hanya program chameleon yang punya uniform ini
            L.p1 = glGetUniformLocation(prog, "ChamP1");
            for (int i = 0; i < 8; i++) {
                char nm[16];
                snprintf(nm, sizeof nm, "ChamA%d", i); L.ca[i] = glGetUniformLocation(prog, nm);
                snprintf(nm, sizeof nm, "ChamB%d", i); L.cb[i] = glGetUniformLocation(prog, nm);
                snprintf(nm, sizeof nm, "ChamC%d", i); L.cc[i] = glGetUniformLocation(prog, nm);
            }
        }
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

static void Pf(std::string& out, const char* fmt, ...) {
    char b[768];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    out += b;
}

static void WriteTpl(std::string& out, const Template& t) {
    if (IsCustomName(t.name)) Pf(out, "\n[%s]\n", t.name.c_str()); else Pf(out, "\n[template:%s]\n", t.name.c_str());
    Pf(out, "a=%d,%d,%d\nb=%d,%d,%d\n", t.a.r, t.a.g, t.a.b, t.b.r, t.b.g, t.b.b);
    if (t.hasC) Pf(out, "c=%d,%d,%d\n", t.c.r, t.c.g, t.c.b);
    if (t.intensity != 1.0f) Pf(out, "intensity=%.2f\n", t.intensity);
}

static void SaveConfig() {
    std::string out;
    Pf(out, "; Pearlescent Colors - disimpan dari menu in-game\n[settings]\n");
    Pf(out, "enabled=%d\nautosave=%d\ntutorial_done=%d\n", g_enabled ? 1 : 0, g_autoSave ? 1 : 0, g_tutorialDone ? 1 : 0);
    Pf(out, "mode=%s\nactive=%s\nshift_mode=%s\n", g_whitelist ? "whitelist" : "all", g_tpl.name.c_str(), ShiftName(g_shiftMode));
    Pf(out, "angle_gain=%.2f\nelev_gain=%.2f\npart_spread=%.2f\nspeed=%.2f\nintensity=%.2f\n", g_angleGain, g_elevGain, g_partSpread, g_speed, g_intensity);
    Pf(out, "shader_patch=%d\ngeom_offset=%d\n", g_shaderPatchEnabled ? 1 : 0, g_geomOff);
    Pf(out, "cham_gain=%.2f\ncham_flow=%.2f\ncham_bias=%.2f\ncham_reflect=%.2f\ncham_fresnel=%.2f\ncham_depth=%.2f\ncham_flake=%.2f\ncham_brightness=%.2f\n",
            g_chamGain, g_chamFlow, g_chamBias, g_chamReflect, g_chamFresnel, g_chamDepth, g_chamFlake, g_chamBright);
    Pf(out, "debug_log=%d\ncrash_guard=%d\naa=%d\naa_quality=%d\naa_strength=%.2f\n", g_debugLog ? 1 : 0, g_crashGuard ? 1 : 0, g_aa ? 1 : 0, g_aaQuality, g_aaStrength);
    Pf(out, "random_mode=%d\nrandom_pool=%d\nrandom_custom=%d\nrandom_lock=%d\nrandom_keep=%d\nrandom_spawn=%d\n", g_randomMode, g_randPool, g_randCustom ? 1 : 0, g_lockOwned ? 1 : 0, g_keepNpcColor ? 1 : 0, g_spawnOwn ? 1 : 0);
    Pf(out, "menu=%d\nmenu_scale=%.2f\nmenu_button_x=%.2f\nmenu_button_y=%.2f\nmenu_button_alpha=%.2f\nmenu_panel_alpha=%.2f\n", g_menuEnabled ? 1 : 0, g_menuScale, g_btnX, g_btnY, g_btnAlpha, g_panelAlpha);
    Pf(out, "\n[cars]\n");
    for (auto& kv : g_carsCfg) Pf(out, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
    for (int i = 0; i < 8; i++) { auto cu = g_templates.find(CustomName(i)); if (cu != g_templates.end()) WriteTpl(out, cu->second); }
    for (auto& kv : g_templates) if (!IsCustomName(kv.first)) WriteTpl(out, kv.second);
    for (int i = 0; i < 6; i++) {
        if (!g_presets[i].used) continue;
        const PresetSlot& ps = g_presets[i];
        Pf(out, "\n[preset:%d]\na=%d,%d,%d\nb=%d,%d,%d\nc=%d,%d,%d\n", i + 1, ps.a.r, ps.a.g, ps.a.b, ps.b.r, ps.b.g, ps.b.b, ps.c.r, ps.c.g, ps.c.b);
    }
    IoPost(g_iniPath, out, false, true);   // ditulis thread I/O; mtime diperbarui setelah selesai
    if (g_debugLog) logger->Info("Config disimpan");
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
    f.v(g_enabled); f.v(g_autoSave); f.v(g_tutorialDone); f.v(g_whitelist); f.v(g_shiftMode); f.s(g_tpl.name);
    f.v(g_angleGain); f.v(g_elevGain); f.v(g_partSpread); f.v(g_speed); f.v(g_intensity);
    f.v(g_chamGain); f.v(g_chamFlow); f.v(g_chamBias); f.v(g_chamReflect); f.v(g_chamFresnel); f.v(g_chamDepth); f.v(g_chamFlake); f.v(g_chamBright);
    f.v(g_btnX); f.v(g_btnY); f.v(g_btnAlpha); f.v(g_panelAlpha);
    f.v(g_aa); f.v(g_aaQuality); f.v(g_aaStrength);
    f.v(g_randomMode); f.v(g_randPool); f.v(g_randCustom); f.v(g_lockOwned); f.v(g_keepNpcColor); f.v(g_spawnOwn);
    for (auto& kv : g_carsCfg) { f.s(kv.first); f.s(kv.second); }
    f.s(g_curCustom);
    for (int i = 0; i < 8; i++) {
        auto cu = g_templates.find(CustomName(i));
        if (cu != g_templates.end()) { HashRGB(f, cu->second.a); HashRGB(f, cu->second.b); HashRGB(f, cu->second.c); f.v(cu->second.hasC); }
    }
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
static float Gutter() { return 46.0f * g_sc; }   // area kosong di kanan untuk menggeser (scroll)
static float g_rightPad = 0.0f;                  // padding kanan saat berada di dalam kartu
static float AvailW() { return ImGui::GetContentRegionAvail().x - g_rightPad; }
static int   g_curCar = -1;                      // model mobil yang sedang dikendarai (-1 = tidak ada)
static bool  g_scopeCar = false;                 // true: pilihan warna hanya untuk mobil yang dikendarai
static bool  g_advanced = false;
static int   g_filter = 0;
static bool SlotInUse(const std::string& n) {
    if (g_tpl.name == n) return true;
    for (auto& kv : g_carsCfg) if (kv.second == n) return true;
    return false;
}
static void ChooseColor(const Template& t);
static void ShowVehicleInfo();
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
    for (int i = 0; i < 8; i++) {
        std::string n = CustomName(i);
        Template& cu = g_templates[n];
        cu.name = n; cu.a = kCustomDef[i][0]; cu.b = kCustomDef[i][1]; cu.c = kCustomDef[i][2]; cu.hasC = true; cu.intensity = 1.0f;
    }
    g_curCustom = "custom";
    for (int i = 0; i < 6; i++) g_presets[i] = PresetSlot();
    auto it = g_templates.find("Purple-Blue-Green");
    g_tpl = (it != g_templates.end()) ? it->second : g_templates["custom"];
    g_editSlot = -1; g_pickSlot = -1;
    g_scopeCar = false; g_advanced = false; g_tutorialDone = false;
    g_aa = false; g_aaQuality = 1; g_aaStrength = 1.0f;
    g_randomMode = 0; g_randPool = 0; g_randCustom = false; g_lockOwned = true; g_keepNpcColor = true; g_spawnOwn = true; g_randAssign.clear(); g_owned.clear(); g_randDirty = true;
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
    if (t.hasC || IsCustomName(t.name)) {
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
    ImGui::SameLine(ImGui::GetContentRegionMax().x - tw - g_rightPad);
    ImGui::PushID(label);
    bool r = Toggle("##t", v);
    ImGui::PopID();
    return r;
}

static bool Segmented(const char* id, const char* const* items, int n, int* sel) {
    bool changed = false;
    float avail = AvailW(), sp = ImGui::GetStyle().ItemSpacing.x;
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
    ImGui::SetNextItemWidth(AvailW() - Gutter());
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
    Template& cu = CustomT();
    if (!cu.hasC) { cu.c = cu.b; cu.hasC = true; }
    if (slot == 0) cu.a = c; else if (slot == 1) cu.b = c; else cu.c = c;
    if (g_tpl.name == cu.name) g_tpl = cu;          // salinan aktif ikut diperbarui
    else if (!SlotInUse(cu.name)) ChooseColor(cu);   // slot belum dipakai: langsung terapkan agar hasilnya terlihat
    g_uniVersion++;
}

static void ColorRow(int slot, const char* label) {
    Template& cu = CustomT();
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
    float fw = AvailW() - (bw * 2 + sp * 2);
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
        float kw = (AvailW() - sp * 7) / 8.0f, kh = ImGui::GetFrameHeight() * 1.35f;
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
        float kw4 = (AvailW() - sp * 3) / 4.0f;
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
        float availW = AvailW();
        float pickW = availW * 0.62f;
        if (pickW > availW - Gutter()) pickW = availW - Gutter();
        ImVec2 pk0 = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(pickW);
        if (ImGui::ColorPicker3("##pick", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_PickerHueBar))
            ApplyCustom(slot, ToRGB(col));
        ImVec2 pk1 = ImGui::GetCursorScreenPos();
        {   // scroll hanya dimatikan bila jari menyentuh area picker itu sendiri
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (ImGui::IsAnyItemActive() && mp.x >= pk0.x && mp.x <= pk0.x + availW - Gutter() && mp.y >= pk0.y && mp.y <= pk1.y) g_noScroll = true;
        }
        RGB c1 = slot == 0 ? cu.a : (slot == 1 ? cu.b : (cu.hasC ? cu.c : cu.b));
        int rgb[3] = { c1.r, c1.g, c1.b };
        static const char* fm[3] = { "R   %d", "G   %d", "B   %d" };
        bool chg = false;
        for (int i = 0; i < 3; i++) {
            ImGui::PushID(100 + i);
            ImGui::SetNextItemWidth(AvailW() - Gutter());
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
    void* clump = (g_clumpOff >= 0) ? *(void**)((char*)v + g_clumpOff) : nullptr;   // offset terdeteksi otomatis
    if (clump) id = ModelIdOf(clump);
    if (id < 0) id = (int)*(uint16_t*)((char*)v + 0x2E);    // CEntity::m_nModelIndex (64-bit)
    return (id >= 0 && id < 20000) ? id : -1;
}

// ================= antarmuka sederhana (v2) =================
struct EffPreset { const char* name; const char* desc; float gain, flow, bias, depth, reflect, fresnel, flake, bright; };
static const EffPreset kEffPresets[4] = {
    {"Lembut",   "Perubahan warna halus dan natural.",            1.2f, 0.15f, 0.0f, 0.15f, 0.12f, 0.30f, 0.04f, 0.95f},
    {"Standar",  "Seimbang, cocok untuk kebanyakan mobil.",       1.6f, 0.25f, 0.0f, 0.25f, 0.22f, 0.50f, 0.08f, 0.90f},
    {"Kilap",    "Cat mengkilap seperti dipoles (clearcoat).",    1.8f, 0.30f, 0.0f, 0.30f, 0.38f, 0.65f, 0.10f, 0.90f},
    {"Kontras",  "Warna lebih dalam dan berani.",                 2.4f, 0.35f, 0.0f, 0.45f, 0.30f, 0.60f, 0.12f, 0.85f},
};
static bool NearF(float a, float b) { float d = a - b; return (d < 0 ? -d : d) < 0.011f; }
static int ActivePreset() {
    for (int i = 0; i < 4; i++) {
        const EffPreset& e = kEffPresets[i];
        if (NearF(g_chamGain, e.gain) && NearF(g_chamFlow, e.flow) && NearF(g_chamBias, e.bias) && NearF(g_chamDepth, e.depth) &&
            NearF(g_chamReflect, e.reflect) && NearF(g_chamFresnel, e.fresnel) && NearF(g_chamFlake, e.flake) && NearF(g_chamBright, e.bright)) return i;
    }
    return -1;
}
static void ApplyEffPreset(int i) {
    const EffPreset& e = kEffPresets[i];
    g_chamGain = e.gain; g_chamFlow = e.flow; g_chamBias = e.bias; g_chamDepth = e.depth;
    g_chamReflect = e.reflect; g_chamFresnel = e.fresnel; g_chamFlake = e.flake; g_chamBright = e.bright;
    g_uniVersion++;
}
static const Template* TemplateByName(const std::string& n) { auto it = g_templates.find(n); return it == g_templates.end() ? nullptr : &it->second; }

static void ShowVehicleInfo() {
    if (g_curCar < 0) return;
    const char* nm = VehicleName(g_curCar);
    ImGui::Text("Nama: %s", nm ? nm : "Kendaraan kustom");
    ImGui::Text("ID: %d", g_curCar);
}

static void ChooseColor(const Template& t) {
    std::string name = t.name;
    if (IsCustomName(name)) g_curCustom = name;
    if (g_scopeCar) {
        if (g_curCar < 0) { Toast("Naiki mobil dulu untuk memberi warna khusus"); return; }
        g_carsCfg[std::to_string(g_curCar)] = name;
        g_needResolve = true;
        Toast(VehicleLabel(g_curCar) + " memakai " + DisplayName(name));
    } else {
        auto it = g_templates.find(name);
        g_tpl = (it != g_templates.end()) ? it->second : t;
        Toast("Semua mobil memakai " + DisplayName(name));
    }
    g_uniVersion++;
}

static void RandomColor() {
    std::vector<const Template*> list;
    for (auto& kv : g_templates) if (!IsCustomName(kv.first)) list.push_back(&kv.second);
    if (list.empty()) return;
    ChooseColor(*list[(size_t)rand() % list.size()]);
}

static void Hint(const std::string& t) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.74f, 1.0f));
    ImGui::TextWrapped("%s", t.c_str());
    ImGui::PopStyleColor();
}

static float g_cardX = 0, g_cardY = 0, g_cardW = 0;
static void CardBegin() {
    float pad = 12.0f * g_sc;
    ImVec2 p = ImGui::GetCursorScreenPos();
    g_cardX = p.x; g_cardY = p.y; g_cardW = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(1.0f, pad * 0.5f));
    ImGui::Indent(pad);
    g_rightPad = pad;
}
static void CardEnd() {
    float pad = 12.0f * g_sc;
    g_rightPad = 0.0f;
    ImGui::Dummy(ImVec2(1.0f, pad * 0.4f));
    ImGui::Unindent(pad);
    ImGui::EndGroup();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(ImVec2(g_cardX, g_cardY), ImVec2(g_cardX + g_cardW, mx.y), IM_COL32(27, 27, 41, 255), 14.0f * g_sc);
    dl->AddRect(ImVec2(g_cardX, g_cardY), ImVec2(g_cardX + g_cardW, mx.y), IM_COL32(255, 255, 255, 20), 14.0f * g_sc);
    dl->ChannelsMerge();
    ImGui::Dummy(ImVec2(1.0f, 6.0f * g_sc));
}

static void DrawTplBar(const Template& t, float h, const std::string& label) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = AvailW();
    ImGui::Dummy(ImVec2(w, h));
    DrawRamp(t, p, ImVec2(p.x + w, p.y + h), 12.0f * g_sc);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.35f), ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 175), IM_COL32(0, 0, 0, 175));
    dl->AddText(ImVec2(p.x + 12.0f * g_sc, p.y + h - ImGui::GetTextLineHeight() - 7.0f * g_sc), IM_COL32(255, 255, 255, 255), label.c_str());
}

static bool SliderHelp(const char* label, const char* help, float* v, float lo, float hi) {
    ImGui::TextUnformatted(label);
    Hint(help);
    ImGui::SetNextItemWidth(AvailW() - Gutter());
    ImGui::PushID(label);
    bool ch = ImGui::SliderFloat("##s", v, lo, hi, "%.2f");
    if (ImGui::IsItemActive()) g_noScroll = true;
    ImGui::PopID();
    ImGui::Spacing();
    return ch;
}

static const Template& EffectiveForScope() {
    if (g_scopeCar && g_curCar >= 0) {
        auto ex = g_carsCfg.find(std::to_string(g_curCar));
        if (ex != g_carsCfg.end()) { const Template* t = TemplateByName(ex->second); if (t) return *t; }
    }
    return g_tpl;
}

// ---------------- TAB UTAMA ----------------
static void TabUtama() {
    float bh = ImGui::GetFrameHeight() * 1.3f, sp = ImGui::GetStyle().ItemSpacing.x;
    if (!g_tutorialDone) {
        CardBegin();
        ImGui::TextColored(kAccent2, "Selamat datang! Cara pakai:");
        ImGui::TextWrapped("1. Buka tab Warna, lalu pilih warna yang kamu suka.");
        ImGui::TextWrapped("2. Mau beda tiap mobil? Naiki mobilnya, buka tab Warna, lalu pilih \"Mobil ini saja\".");
        ImGui::TextWrapped("3. Semua pengaturan tersimpan otomatis.");
        ImGui::Spacing();
        if (Btn("Mengerti, tutup panduan", ImVec2(AvailW(), bh))) g_tutorialDone = true;
        CardEnd();
    }

    CardBegin();
    ToggleRow("Efek chameleon", &g_enabled);
    Hint(g_enabled ? "Aktif. Matikan untuk melihat warna asli mobil." : "Mati. Mobil tampil dengan warna aslinya.");
    ImGui::Spacing();
    ToggleRow("Anti-aliasing (FXAA)", &g_aa);
    Hint(g_aaFailed ? "Anti-aliasing tidak didukung di perangkat ini." : "Menghaluskan tepi bergerigi. Atur kualitasnya di tab Lainnya.");
    ImGui::Spacing();
    {
        bool rnd = (g_randomMode != 0);
        if (ToggleRow("Warna acak untuk mobil NPC", &rnd)) g_randomMode = rnd ? 1 : 0;
        Hint(rnd ? "Mobil NPC mendapat warna chameleon acak dan tetap memakainya saat kamu naiki. Mobilmu sendiri memakai warna pilihanmu. Atur di tab Mobil."
                 : "Mati. Semua mobil sejenis memakai warna yang sama.");
    }
    CardEnd();

    CardBegin();
    ImGui::TextColored(kAccent2, "Warna untuk semua mobil");
    DrawTplBar(g_tpl, ImGui::GetFrameHeight() * 1.8f, DisplayName(g_tpl.name));
    float bw = (AvailW() - sp) / 2.0f;
    if (Btn("Ganti warna", ImVec2(bw, bh))) { g_scopeCar = false; g_tab = 1; }
    ImGui::SameLine();
    if (Btn("Acak", ImVec2(bw, bh))) { g_scopeCar = false; RandomColor(); }
    CardEnd();

    CardBegin();
    ImGui::TextColored(kAccent2, "Mobil yang sedang dikendarai");
    if (g_curCar >= 0) {
        ShowVehicleInfo();
        auto ex = g_carsCfg.find(std::to_string(g_curCar));
        if (ex != g_carsCfg.end()) {
            const Template* t = TemplateByName(ex->second);
            if (t) DrawTplBar(*t, ImGui::GetFrameHeight() * 1.5f, "Warna khusus: " + DisplayName(ex->second));
            float b2 = (AvailW() - sp) / 2.0f;
            if (Btn("Ganti warna", ImVec2(b2, bh))) { g_scopeCar = true; g_tab = 1; }
            ImGui::SameLine();
            if (Btn("Hapus warna khusus", ImVec2(b2, bh))) { g_carsCfg.erase(ex); g_needResolve = true; Toast("Warna khusus mobil ini dihapus"); }
        } else {
            Hint(g_whitelist ? "Mobil ini belum diberi warna, jadi tampil normal." : "Mobil ini memakai warna umum.");
            if (Btn("Beri warna khusus", ImVec2(AvailW(), bh))) { g_scopeCar = true; g_tab = 1; }
        }
    } else {
        Hint("Naiki sebuah mobil untuk memberinya warna khusus.");
    }
    CardEnd();

    CardBegin();
    ImGui::TextColored(kAccent2, "Gaya efek");
    {
        float w = (AvailW() - sp * 3) / 4.0f;
        int act = ActivePreset();
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            bool on = (act == i);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kPanel);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccent : kPanelHi);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent2);
            if (Btn(kEffPresets[i].name, ImVec2(w, bh))) { ApplyEffPreset(i); Toast(std::string("Gaya ") + kEffPresets[i].name + " diterapkan"); }
            ImGui::PopStyleColor(3);
        }
        Hint(act >= 0 ? kEffPresets[act].desc : "Pengaturan kustom. Atur detailnya di tab Efek.");
    }
    CardEnd();
}

// ---------------- TAB WARNA ----------------
static void TabWarna() {
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float bh = ImGui::GetFrameHeight() * 1.3f;
    SectionTitle("ATUR WARNA UNTUK");
    static const char* kScope[] = {"Semua mobil", "Mobil ini saja"};
    int sc = g_scopeCar ? 1 : 0;
    if (Segmented("scope", kScope, 2, &sc)) g_scopeCar = (sc == 1);
    if (g_scopeCar) Hint(g_curCar >= 0 ? "Pilihan di bawah hanya berlaku untuk mobil yang sedang kamu kendarai: " + VehicleLabel(g_curCar) + "."
                                       : "Naiki sebuah mobil dulu agar bisa memilih warna khusus untuk mobil itu.");
    else Hint("Pilihan di bawah berlaku untuk semua mobil yang tidak punya warna khusus.");
    ImGui::Spacing();
    const Template& eff = EffectiveForScope();
    DrawTplBar(eff, ImGui::GetFrameHeight() * 2.0f, "Dipakai: " + DisplayName(eff.name));

    SectionTitle("WARNA SIAP PAKAI");
    static const char* kF[] = {"Semua", "Gelap", "Sedang", "Terang"};
    Segmented("flt", kF, 4, &g_filter);
    if (Btn("Acak warna", ImVec2(AvailW(), bh))) RandomColor();
    ImGui::Spacing();
    {
        float cw = (AvailW() - sp) / 2.0f, ch = ImGui::GetFrameHeight() * 2.1f;
        int idx = 0;
        for (auto& kv : g_templates) {
            if (IsCustomName(kv.first)) continue;
            if (g_filter != 0 && LumClass(kv.second) != g_filter) continue;
            if (idx % 2 == 1) ImGui::SameLine();
            if (TemplateCard(kv.first, kv.second, kv.first == eff.name, cw, ch)) ChooseColor(kv.second);
            idx++;
        }
        if (idx == 0) Hint("Tidak ada warna pada kategori ini.");
    }

    SectionTitle("WARNA BUATAN SAYA");
    Hint("Ada 8 tempat penyimpanan. Tiap mobil bisa memakai tempat yang berbeda. Ketuk salah satu untuk dipakai dan diedit.");
    {
        float cw = (AvailW() - sp * 3) / 4.0f, ch = ImGui::GetFrameHeight() * 1.7f;
        for (int i = 0; i < 8; i++) {
            if (i % 4) ImGui::SameLine();
            std::string n = CustomName(i);
            const Template* t = TemplateByName(n);
            if (!t) continue;
            if (TemplateCard("Custom " + std::to_string(i + 1), *t, n == eff.name, cw, ch)) ChooseColor(*t);
        }
    }
    ImGui::Spacing();
    {
        const Template& cu = CustomT();
        std::string used;
        if (g_tpl.name == cu.name) used = "semua mobil";
        for (auto& kv : g_carsCfg) if (kv.second == cu.name) used += (used.empty() ? "" : ", ") + std::string("mobil ") + kv.first;
        SectionTitle(("EDIT " + DisplayName(g_curCustom)).c_str());
        Hint(used.empty() ? "Belum dipakai. Ketuk kartunya di atas, atau ubah warnanya di sini agar langsung diterapkan."
                          : "Sedang dipakai: " + used + ".");
    }
    ImGui::Spacing();
    ColorRow(0, "Warna 1 - saat menghadap kamera");
    ColorRow(1, "Warna 2 - peralihan");
    ColorRow(2, "Warna 3 - saat miring / di tepi");
    Hint("Ketuk kotak warna untuk picker, atau ketuk kolom kode untuk mengetik hex. Tempel/Salin memakai clipboard (#RRGGBB, RGB, atau 120,40,200).");
    ImGui::Spacing();
    {
        float bw = (AvailW() - sp * 2) / 3.0f;
        Template& cu = CustomT();
        if (Btn("Salin 3 warna", ImVec2(bw, bh))) {
            RGB c3 = cu.hasC ? cu.c : cu.b;
            Toast(ClipboardSet("#" + HexStr(cu.a) + " #" + HexStr(cu.b) + " #" + HexStr(c3)) ? "3 warna disalin" : "Clipboard tidak tersedia");
        }
        ImGui::SameLine();
        if (Btn("Tempel 3 warna", ImVec2(bw, bh))) {
            RGB pal[3];
            int n = ParsePalette(ClipboardGet(), pal);
            if (n >= 2) { for (int i = 0; i < n; i++) ApplyCustom(i, pal[i]); Toast("3 warna ditempel"); }
            else Toast("Format: #RRGGBB #RRGGBB #RRGGBB");
        }
        ImGui::SameLine();
        if (Btn("Mulai dari warna terpilih", ImVec2(bw, bh))) {
            Template src = eff;
            if (src.name != cu.name) { ApplyCustom(0, src.a); ApplyCustom(1, src.b); ApplyCustom(2, src.hasC ? src.c : src.b); Toast("Warna terpilih disalin ke " + DisplayName(g_curCustom)); }
        }
    }
}

// ---------------- TAB EFEK ----------------
static void TabEfek() {
    float bh = ImGui::GetFrameHeight() * 1.3f, sp = ImGui::GetStyle().ItemSpacing.x;
    SectionTitle("GAYA CEPAT");
    {
        float w = (AvailW() - sp * 3) / 4.0f;
        int act = ActivePreset();
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            bool on = (act == i);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kPanel);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kAccent : kPanelHi);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent2);
            if (Btn(kEffPresets[i].name, ImVec2(w, bh))) { ApplyEffPreset(i); Toast(std::string("Gaya ") + kEffPresets[i].name + " diterapkan"); }
            ImGui::PopStyleColor(3);
        }
        Hint(act >= 0 ? kEffPresets[act].desc : "Pengaturan kustom (sudah kamu ubah manual).");
    }
    SectionTitle("ATUR SENDIRI");
    bool ch = false;
    ch |= SliderHelp("Kecepatan ganti warna", "Makin besar, warna makin cepat berubah saat sudut pandang berubah.", &g_chamGain, 0.2f, 4.0f);
    ch |= SliderHelp("Kilap dasar", "Seberapa mengkilap catnya (pantulan lingkungan).", &g_chamReflect, 0.0f, 0.6f);
    ch |= SliderHelp("Kilap di tepi", "Tambahan kilap pada sisi bodi yang melengkung.", &g_chamFresnel, 0.0f, 1.0f);
    ch |= SliderHelp("Kedalaman warna", "Makin besar, warna dasar makin gelap dan dalam.", &g_chamDepth, 0.0f, 0.8f);
    ch |= SliderHelp("Butiran metalik", "Butiran halus berkilau pada cat. 0 untuk mematikan.", &g_chamFlake, 0.0f, 0.5f);
    ch |= SliderHelp("Kecerahan", "Kecerahan keseluruhan warna. Turunkan jika terlalu pucat.", &g_chamBright, 0.3f, 1.5f);
    if (ch) g_uniVersion++;

    SectionTitle("LANJUTAN");
    if (Btn(g_advanced ? "Sembunyikan pengaturan lanjutan" : "Tampilkan pengaturan lanjutan", ImVec2(AvailW(), bh))) g_advanced = !g_advanced;
    if (g_advanced) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Teknik efek");
        static const char* kModes[] = {"Pixel", "Sudut", "Waktu", "Gabungan"};
        static const int kModeVal[] = {3, 0, 1, 2};
        static const char* kModeHelp[] = {
            "Pixel: warna dihitung per piksel, paling realistis (disarankan).",
            "Sudut: warna tiap bagian bodi mengikuti sudut kamera.",
            "Waktu: warna berubah pelan seiring waktu.",
            "Gabungan: sudut kamera ditambah perubahan waktu." };
        int mi = 0;
        for (int i = 0; i < 4; i++) if (kModeVal[i] == g_shiftMode) mi = i;
        if (Segmented("mode", kModes, 4, &mi)) g_shiftMode = kModeVal[mi];
        Hint(kModeHelp[mi]);
        if (g_shiftMode == 3 && !g_shaderOK) Hint("Pixel belum aktif (shader_patch=0 atau hook gagal), sementara dipakai mode Sudut.");
        ImGui::Spacing();
        bool c2 = false;
        c2 |= SliderHelp("Pengaruh pantulan atas/bawah", "Menggeser warna menurut arah pantulan langit dan tanah.", &g_chamFlow, -1.0f, 1.0f);
        c2 |= SliderHelp("Geser awal gradasi", "Memajukan atau memundurkan titik awal perubahan warna.", &g_chamBias, -0.6f, 0.6f);
        if (c2) g_uniVersion++;
        SliderHelp("Mode sudut: gain keliling", "Jumlah pergantian warna saat mengelilingi mobil.", &g_angleGain, 0.5f, 6.0f);
        SliderHelp("Mode sudut: gain atas/bawah", "Pengaruh sudut kamera dari atas atau bawah.", &g_elevGain, 0.0f, 4.0f);
        SliderHelp("Mode sudut: beda antar bagian", "Perbedaan warna antara kap, pintu, bumper, dan lainnya.", &g_partSpread, 0.0f, 6.0f);
        SliderHelp("Mode waktu: kecepatan", "Kecepatan perubahan warna terhadap waktu.", &g_speed, 0.0f, 3.0f);
        SliderHelp("Intensitas keseluruhan", "Kekuatan perpindahan antar warna.", &g_intensity, 0.0f, 1.0f);
    }
    SectionTitle("KEMBALIKAN");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.26f, 0.38f, 1.0f));
    if (Btn("Kembalikan efek ke default", ImVec2(AvailW(), bh))) { ResetEffects(); Toast("Efek dikembalikan ke default"); }
    ImGui::PopStyleColor(2);
}

// ---------------- pilih mobil dari daftar nama ----------------
static void VehiclePicker() {
    static bool open = false;
    static int range = 0;
    static std::vector<int> order;
    float bh = ImGui::GetFrameHeight() * 1.3f, sp = ImGui::GetStyle().ItemSpacing.x;
    SectionTitle("TAMBAH MOBIL DARI DAFTAR NAMA");
    Hint("Pilih mobil lewat namanya tanpa harus menaikinya. Mobil memakai warna umum yang aktif (ganti lewat tab Warna). Ketuk lagi untuk menghapus.");
    if (Btn(open ? "Sembunyikan daftar nama" : "Tampilkan daftar nama", ImVec2(AvailW(), bh))) open = !open;
    if (!open) return;
    if (order.empty()) {
        for (int id = 400; id <= 611; id++) order.push_back(id);
        std::sort(order.begin(), order.end(), [](int a, int b) { return strcasecmp(kVehNames[a - 400], kVehNames[b - 400]) < 0; });
    }
    static const char* kR[] = {"A-D", "E-L", "M-R", "S-Z"};
    Segmented("vr", kR, 4, &range);
    ImGui::Spacing();
    float cw = (AvailW() - sp) / 2.0f;
    int n = 0;
    for (int id : order) {
        const char* nm = kVehNames[id - 400];
        char c = (char)toupper((unsigned char)nm[0]);
        bool inRange = range == 0 ? (c >= 'A' && c <= 'D') : (range == 1 ? (c >= 'E' && c <= 'L') : (range == 2 ? (c >= 'M' && c <= 'R') : (c >= 'S' && c <= 'Z')));
        if (!inRange) continue;
        if (n % 2 == 1) ImGui::SameLine();
        std::string key = std::to_string(id);
        bool reg = g_carsCfg.count(key) > 0;
        std::string lb = std::string(nm) + " (" + key + ")";
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, reg ? kAccent : kPanel);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, reg ? kAccent : kPanelHi);
        if (Btn(lb.c_str(), ImVec2(cw, bh))) {
            if (reg) { g_carsCfg.erase(key); Toast(VehicleLabel(id) + " dihapus dari daftar"); }
            else { g_carsCfg[key] = g_tpl.name; Toast(VehicleLabel(id) + " ditambahkan"); }
            g_needResolve = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        n++;
    }
}

// ---------------- TAB MOBIL ----------------
static void TabMobil() {
    float bh = ImGui::GetFrameHeight() * 1.3f, sp = ImGui::GetStyle().ItemSpacing.x;
    SectionTitle("MOBIL LAIN");
    bool others = !g_whitelist;
    if (ToggleRow("Mobil lain ikut berwarna", &others)) g_whitelist = !others;
    Hint(others ? "Semua mobil berwarna chameleon. Mobil di daftar bawah memakai warna khususnya."
                : "Hanya mobil di daftar bawah yang berwarna chameleon. Mobil lain tampil normal.");

    SectionTitle("WARNA ACAK OTOMATIS");
    {
        static const char* kRm[] = {"Mati", "NPC saja", "Semua mobil"};
        if (Segmented("rm", kRm, 3, &g_randomMode)) { g_randAssign.clear(); }
        Hint(g_randomMode == 0 ? "Mati. Mobil sejenis memakai warna yang sama."
             : (g_randomMode == 1 ? "Mobil NPC mendapat warna chameleon acak yang berbeda-beda. Mobil yang sedang kamu kendarai memakai warna pilihanmu."
                                  : "Semua mobil, termasuk milikmu, mendapat warna acak masing-masing."));
        if (g_randomMode != 0) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Sumber warna acak");
            static const char* kRp[] = {"Semua", "Gelap", "Sedang", "Terang"};
            if (Segmented("rp", kRp, 4, &g_randPool)) g_randDirty = true;
            ImGui::Spacing();
            if (ToggleRow("Sertakan warna buatanku", &g_randCustom)) g_randDirty = true;
            if (g_randomMode == 1) {
                ImGui::Spacing();
                ToggleRow("Pertahankan warna saat naik mobil NPC", &g_keepNpcColor);
                Hint("Mobil NPC yang kamu naiki tetap memakai warna acaknya. Matikan agar mobil itu berganti ke warna pilihanmu saat dinaiki.");
                ImGui::Spacing();
                ToggleRow("Mobil hasil spawn (CLEO) pakai warna pilihanku", &g_spawnOwn);
                Hint("Mobil yang kamu spawn lewat skrip seperti CLEO langsung memakai warna pilihanmu, tidak dianggap NPC. Mobil dari misi cerita game juga ikut.");
                ImGui::Spacing();
                ToggleRow("Kunci mobil yang pernah kukendarai", &g_lockOwned);
                Hint("Mobil yang pernah kamu naiki tetap memakai warna pilihanmu walau kamu sudah keluar. Yang diingat 8 mobil terakhir.");
                if (g_lockOwned && Btn("Lepas semua kunci", ImVec2(AvailW(), bh))) { g_owned.clear(); Toast("Kunci mobil dilepas"); }
            }
            Hint("Warna acak tetap selama mobil itu ada. Berlaku untuk mobil yang ikut berwarna (lihat saklar 'Mobil lain ikut berwarna').");
            if (Btn("Acak ulang semua mobil", ImVec2(AvailW(), bh))) { g_randAssign.clear(); Toast("Warna acak diacak ulang"); }
        }
    }

    SectionTitle("MOBIL YANG DIKENDARAI");
    if (g_curCar >= 0) {
        ShowVehicleInfo();
        if (g_randomMode == 1) {
            void* cc = CurrentVehicleClump();
            if (cc) {
                int cm = ModelIdOf(cc);
                bool assigned = HasRandAssign(cc, cm);
                std::string nm = "pilihanmu";
                if (assigned) { auto ra = g_randAssign.find(cc); if (ra != g_randAssign.end()) nm = "acak - " + DisplayName(ra->second.name); }
                Hint("Warna mobil ini: " + nm);
                float b2s = (AvailW() - sp) / 2.0f;
                if (Btn("Pakai warna pilihanku", ImVec2(b2s, bh))) { g_randAssign.erase(cc); MarkOwned(cc, cm); Toast("Mobil ini memakai warna pilihanmu"); }
                ImGui::SameLine();
                if (Btn("Acak warna mobil ini", ImVec2(b2s, bh))) { g_randAssign.erase(cc); RandomTemplateFor(cc, cm, true); Toast("Warna mobil ini diacak"); }
                ImGui::Spacing();
            }
        }
        auto ex = g_carsCfg.find(std::to_string(g_curCar));
        float b2 = (AvailW() - sp) / 2.0f;
        if (ex == g_carsCfg.end()) {
            if (Btn("Beri warna khusus", ImVec2(AvailW(), bh))) { g_scopeCar = true; g_tab = 1; }
        } else {
            if (Btn("Ganti warna", ImVec2(b2, bh))) { g_scopeCar = true; g_tab = 1; }
            ImGui::SameLine();
            if (Btn("Hapus dari daftar", ImVec2(b2, bh))) { g_carsCfg.erase(ex); g_needResolve = true; Toast("Mobil dihapus dari daftar"); }
        }
    } else {
        Hint("Naiki sebuah mobil untuk mengatur warna khususnya.");
    }

    SectionTitle("DAFTAR MOBIL BERWARNA KHUSUS");
    if (g_carsCfg.empty()) Hint("Belum ada. Naiki mobil, buka tab Warna, lalu pilih \"Mobil ini saja\".");
    std::string del;
    for (auto& kv : g_carsCfg) {
        ImGui::PushID(kv.first.c_str());
        CardBegin();
        ImGui::TextWrapped("%s%s", KeyLabel(kv.first).c_str(), (g_curCar >= 0 && std::to_string(g_curCar) == kv.first) ? "  (sedang dikendarai)" : "");
        const Template* t = TemplateByName(kv.second);
        if (t) DrawTplBar(*t, ImGui::GetFrameHeight() * 1.4f, DisplayName(kv.second));
        else Hint("Warna tidak ditemukan: " + kv.second);
        float b3 = (AvailW() - sp * 2) / 3.0f;
        if (IsCustomName(kv.second)) {
            if (Btn("Edit warna", ImVec2(b3, bh))) { g_curCustom = kv.second; g_scopeCar = false; g_tab = 1; g_uniVersion++; }
        } else {
            ImGui::BeginDisabled();
            Btn("Edit warna", ImVec2(b3, bh));
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (Btn("Pakai warna umum", ImVec2(b3, bh))) { kv.second = g_tpl.name; g_needResolve = true; Toast("Mobil memakai warna umum yang aktif"); }
        ImGui::SameLine();
        if (Btn("Hapus", ImVec2(b3, bh))) del = kv.first;
        CardEnd();
        ImGui::PopID();
    }
    if (!del.empty()) { g_carsCfg.erase(del); g_needResolve = true; Toast("Mobil dihapus dari daftar"); }
    VehiclePicker();
}

// ---------------- TAB LAINNYA ----------------
static void TabSistem() {
    float bh = ImGui::GetFrameHeight() * 1.3f, sp = ImGui::GetStyle().ItemSpacing.x;
    SectionTitle("PENYIMPANAN");
    ToggleRow("Simpan otomatis", &g_autoSave);
    Hint("Semua pengaturan tersimpan otomatis ke pearlescent.ini, jadi tidak perlu diatur ulang saat main lagi.");
    ImGui::Spacing();
    float bw = (AvailW() - sp) / 2.0f;
    if (Btn("Simpan sekarang", ImVec2(bw, bh))) { SaveConfig(); g_hashInit = false; Toast("Pengaturan disimpan"); }
    ImGui::SameLine();
    if (Btn("Muat ulang dari file", ImVec2(bw, bh))) { LoadConfig(); g_uniVersion++; Toast("Pengaturan dimuat ulang"); }

    SectionTitle("GRAFIS");
    ToggleRow("Anti-aliasing (FXAA)", &g_aa);
    Hint(g_aaFailed ? "Anti-aliasing tidak didukung di perangkat ini (penyalinan framebuffer gagal)."
                    : "Menghaluskan tepi bergerigi di seluruh layar. Menambah sedikit beban GPU; matikan jika FPS turun.");
    if (g_aa) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Kualitas");
        static const char* kAaQ[] = {"Rendah", "Sedang", "Tinggi"};
        Segmented("aaq", kAaQ, 3, &g_aaQuality);
        ImGui::Spacing();
        SliderHelp("Kekuatan", "Makin besar tepi makin halus, tetapi gambar sedikit lebih lembut.", &g_aaStrength, 0.1f, 1.0f);
    }

    SectionTitle("TAMPILAN MENU");
    SliderHelp("Posisi tombol (kiri-kanan)", "Geser tombol PC agar tidak menghalangi layar.", &g_btnX, 0.0f, 0.95f);
    SliderHelp("Posisi tombol (atas-bawah)", "Geser tombol PC ke atas atau bawah.", &g_btnY, 0.0f, 0.9f);
    SliderHelp("Transparansi tombol", "Kiri = hampir transparan, kanan = solid.", &g_btnAlpha, 0.10f, 1.0f);
    SliderHelp("Transparansi panel menu", "Membuat panel menu menembus pandang.", &g_panelAlpha, 0.40f, 1.0f);

    SectionTitle("BANTUAN");
    if (Btn("Tampilkan panduan lagi", ImVec2(AvailW(), bh))) { g_tutorialDone = false; g_tab = 0; Toast("Panduan ditampilkan di tab Utama"); }
    Hint("Tips: geser jari di area kosong sebelah kanan untuk menggulir menu. Ketuk X di kanan atas untuk menutup menu.");

    SectionTitle("RESET TOTAL");
    Hint("Mengembalikan SEMUA pengaturan ke setelan awal: efek, warna buatan, daftar mobil, posisi dan transparansi tombol.");
    {
        static double confirmT = -10.0;
        bool armed = (NowSec() - confirmT) < 3.0;
        ImGui::PushStyleColor(ImGuiCol_Button, armed ? ImVec4(0.80f, 0.20f, 0.25f, 1.0f) : ImVec4(0.55f, 0.20f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.26f, 0.32f, 1.0f));
        if (Btn(armed ? "Ketuk lagi untuk KONFIRMASI" : "Kembalikan semua ke setelan awal", ImVec2(AvailW(), bh * 1.15f))) {
            if (armed) { ResetAll(); confirmT = -10.0; Toast("Semua pengaturan dikembalikan ke setelan awal"); }
            else confirmT = NowSec();
        }
        ImGui::PopStyleColor(2);
    }

    SectionTitle("STATUS");
    Hint(std::string("Shader per-piksel: ") + (g_shaderOK ? "aktif" : "tidak aktif"));
    Hint("Template: " + std::to_string((int)g_templates.size()) + " | Palet shader: " + std::to_string((int)g_palette.size()));
    Hint("File: " + g_iniPath);
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
            float w = AvailW();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilledMultiColor(p, ImVec2(p.x + w * 0.5f, p.y + 3.0f * g_sc), ImGui::GetColorU32(kAccent), ImGui::GetColorU32(kAccent2), ImGui::GetColorU32(kAccent2), ImGui::GetColorU32(kAccent));
            dl->AddRectFilledMultiColor(ImVec2(p.x + w * 0.5f, p.y), ImVec2(p.x + w, p.y + 3.0f * g_sc), ImGui::GetColorU32(kAccent2), IM_COL32(255, 90, 170, 255), IM_COL32(255, 90, 170, 255), ImGui::GetColorU32(kAccent2));
            ImGui::Dummy(ImVec2(w, 3.0f * g_sc));
        }
        ImGui::Spacing();
        g_curCar = CurrentVehicleModel();
        static const char* kTabs[] = {"Utama", "Warna", "Efek", "Mobil", "Lainnya"};
        Segmented("tabs", kTabs, 5, &g_tab);
        ImGui::Spacing();
        ImGui::BeginChild("##content", ImVec2(0, 0), 0, 0);
        switch (g_tab) {
            case 0: TabUtama(); break;
            case 1: TabWarna(); break;
            case 2: TabEfek(); break;
            case 3: TabMobil(); break;
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
    st.ItemInnerSpacing = ImVec2(8, 6); st.ScrollbarSize = 14.0f; st.GrabMinSize = 22.0f;
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

// ukuran layar sebenarnya dari surface EGL (viewport GL di akhir frame gameplay bisa kecil/berbeda)
static bool WindowSize(int& w, int& h) {
    EGLDisplay d = eglGetCurrentDisplay();
    EGLSurface sf = eglGetCurrentSurface(EGL_DRAW);
    EGLint ww = 0, hh = 0;
    if (d != EGL_NO_DISPLAY && sf != EGL_NO_SURFACE && eglQuerySurface(d, sf, EGL_WIDTH, &ww) && eglQuerySurface(d, sf, EGL_HEIGHT, &hh) && ww > 200 && hh > 200) {
        w = (int)ww; h = (int)hh;
        return true;
    }
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 200 && vp[3] > 200) { w = vp[2]; h = vp[3]; return true; }
    return false;
}

static bool InitMenu() {
    if (g_menuFailed) return false;
    int ww = 0, hh = 0;
    if (!WindowSize(ww, hh)) return false;
    float sc = g_menuScale > 0.0f ? g_menuScale : (float)hh / 480.0f;
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
    logger->Info("Menu ImGui siap (skala %.2f, layar %dx%d, font %s)", sc, ww, hh, fontOk ? "sistem" : "bawaan");
    return true;
}

// ================= anti-aliasing (FXAA) =================
static GLuint g_aaProg = 0, g_aaTex = 0;
static int g_aaW = 0, g_aaH = 0;
static GLint g_aaLocTex = -1, g_aaLocRes = -1, g_aaLocStr = -1, g_aaLocSpan = -1;

static GLuint AaCompile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[300] = {0};
        glGetShaderInfoLog(sh, sizeof log - 1, nullptr, log);
        logger->Error("Shader AA gagal dikompilasi: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool AaInit() {
    static const char* vs =
        "attribute vec2 aPos;\n"
        "varying vec2 vUV;\n"
        "void main() { vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char* fs =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n"
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "uniform sampler2D uTex;\n"
        "uniform vec2 uInvRes;\n"
        "uniform float uStrength;\n"
        "uniform float uSpan;\n"
        "varying vec2 vUV;\n"
        "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "void main() {\n"
        "  vec3 rgbM  = texture2D(uTex, vUV).rgb;\n"
        "  vec3 rgbNW = texture2D(uTex, vUV + vec2(-1.0, -1.0) * uInvRes).rgb;\n"
        "  vec3 rgbNE = texture2D(uTex, vUV + vec2( 1.0, -1.0) * uInvRes).rgb;\n"
        "  vec3 rgbSW = texture2D(uTex, vUV + vec2(-1.0,  1.0) * uInvRes).rgb;\n"
        "  vec3 rgbSE = texture2D(uTex, vUV + vec2( 1.0,  1.0) * uInvRes).rgb;\n"
        "  float lM = lum(rgbM), lNW = lum(rgbNW), lNE = lum(rgbNE), lSW = lum(rgbSW), lSE = lum(rgbSE);\n"
        "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
        "  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
        "  vec2 dir;\n"
        "  dir.x = -((lNW + lNE) - (lSW + lSE));\n"
        "  dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
        "  float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);\n"
        "  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
        "  dir = min(vec2(uSpan), max(vec2(-uSpan), dir * rcpDirMin)) * uInvRes;\n"
        "  vec3 rgbA = 0.5 * (texture2D(uTex, vUV + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(uTex, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
        "  vec3 rgbB = rgbA * 0.5 + 0.25 * (texture2D(uTex, vUV + dir * -0.5).rgb + texture2D(uTex, vUV + dir * 0.5).rgb);\n"
        "  float lB = lum(rgbB);\n"
        "  vec3 col = (lB < lMin || lB > lMax) ? rgbA : rgbB;\n"
        "  gl_FragColor = vec4(mix(rgbM, col, uStrength), 1.0);\n"
        "}\n";
    GLuint v = AaCompile(GL_VERTEX_SHADER, vs), f = AaCompile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    GLuint pr = glCreateProgram();
    glAttachShader(pr, v);
    glAttachShader(pr, f);
    glBindAttribLocation(pr, 0, "aPos");
    glLinkProgram(pr);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    glDeleteShader(v);
    glDeleteShader(f);
    if (!ok) { logger->Error("Program AA gagal di-link"); glDeleteProgram(pr); return false; }
    g_aaProg = pr;
    g_aaLocTex = glGetUniformLocation(pr, "uTex");
    g_aaLocRes = glGetUniformLocation(pr, "uInvRes");
    g_aaLocStr = glGetUniformLocation(pr, "uStrength");
    g_aaLocSpan = glGetUniformLocation(pr, "uSpan");
    logger->Info("Anti-aliasing (FXAA) siap");
    return true;
}

// salin framebuffer layar ke texture lalu gambar ulang dengan FXAA (sebelum UI menu digambar)
static void ApplyAA(int W, int H) {
    if (g_aaFailed) return;
    if (!g_aaProg && !AaInit()) { g_aaFailed = true; return; }

    GLint prevProg = 0, prevAct = 0, prevTex = 0, vpPrev[4] = {0, 0, 0, 0};
    GLboolean eBlend = glIsEnabled(GL_BLEND), eDepth = glIsEnabled(GL_DEPTH_TEST), eScissor = glIsEnabled(GL_SCISSOR_TEST),
              eCull = glIsEnabled(GL_CULL_FACE), eStencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevAct);
    glGetIntegerv(GL_VIEWPORT, vpPrev);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    while (glGetError() != GL_NO_ERROR) {}

    if (!g_aaTex) glGenTextures(1, &g_aaTex);
    glBindTexture(GL_TEXTURE_2D, g_aaTex);
    if (g_aaW != W || g_aaH != H) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, W, H, 0);
        g_aaW = W; g_aaH = H;
    } else {
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
    }
    if (glGetError() != GL_NO_ERROR) {   // perangkat tidak mendukung penyalinan framebuffer: matikan AA
        logger->Error("AA dimatikan: gagal menyalin framebuffer");
        g_aaFailed = true;
    } else {
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
        glViewport(0, 0, W, H);
        glUseProgram(g_aaProg);
        static const float span[3] = {4.0f, 8.0f, 12.0f};
        glUniform1i(g_aaLocTex, 0);
        glUniform2f(g_aaLocRes, 1.0f / (float)W, 1.0f / (float)H);
        glUniform1f(g_aaLocStr, g_aaStrength);
        glUniform1f(g_aaLocSpan, span[g_aaQuality < 0 ? 0 : (g_aaQuality > 2 ? 2 : g_aaQuality)]);
        static const float quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    // pulihkan state milik game
    glUseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevAct);
    if (eBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (eDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (eScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (eCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (eStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glViewport(vpPrev[0], vpPrev[1], vpPrev[2], vpPrev[3]);
}

struct AttribBackup { GLint en, size, type, norm, stride, buf; void* ptr; };

static void DrawMenuFrame() {
    int ww = 0, hh = 0;
    if (!WindowSize(ww, hh)) return;
    float W = (float)ww, H = (float)hh;

    // diagnostik: catat state GL pada beberapa frame pertama dan berkala
    static int frames = 0, logs = 0;
    if (g_debugLog && (frames < 4 || frames % 900 == 0) && logs < 14) {
        GLint vpd[4] = {0, 0, 0, 0}, fbo = 0; GLboolean cm[4] = {1, 1, 1, 1};
        glGetIntegerv(GL_VIEWPORT, vpd); glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo); glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        logger->Info("frame=%d layar=%dx%d viewport=%d,%d,%d,%d fbo=%d colormask=%d%d%d%d", frames, ww, hh, vpd[2 - 2], vpd[1], vpd[2], vpd[3], fbo, cm[0], cm[1], cm[2], cm[3]);
        logs++;
    }
    frames++;

    // normalkan state yang mungkin ditinggalkan game saat gameplay
    GLint prevFbo = 0; GLboolean prevMask[4] = {1, 1, 1, 1};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);
    if (prevFbo != 0) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

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

    if (g_aa) ApplyAA(ww, hh);

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
    glColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);
    if (prevFbo != 0) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
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
    g_iniWatch = g_iniPath;
    IoStart();

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
    if (g_spawnOwn) {
        InstallGotHook(hook_target, 0x83ece8, 0x3afa70, (void*)CreateCarForScript_hook, (void**)&CreateCarForScript_orig, "CCarCtrl::CreateCarForScript");
    }
    if (g_crashGuard) {
        InstallGotHook(hook_target, 0x83fd50, 0x2ba020, (void*)RpClumpForAllAtomics_guard, (void**)&RpClumpForAllAtomics_orig, "RpClumpForAllAtomics (pelindung NULL)");
        InstallGotHook(hook_target, 0x844ef8, 0x2bce78, (void*)RpGeometryForAllMaterials_guard, (void**)&RpGeometryForAllMaterials_orig, "RpGeometryForAllMaterials (pelindung NULL)");
    }
    EnsureCustomSlots();
    for (auto& kv : g_templates) if (!IsCustomName(kv.first) && g_palette.size() < 32) g_palette.push_back(kv.second);
    if (g_shaderPatchEnabled) {
        bool ok = InstallGotHook(hook_target, 0x845650, 0, (void*)glUseProgram_hook, (void**)&glUseProgram_orig, "glUseProgram")
               && InstallGotHook(hook_target, 0x8496e0, 0, (void*)glLinkProgram_hook, (void**)&glLinkProgram_orig, "glLinkProgram")
               && InstallGotHook(hook_target, 0x8497f8, 0, (void*)glShaderSource_hook, (void**)&glShaderSource_orig, "glShaderSource");
        g_shaderOK = ok;
        if (!ok) logger->Error("Mode pixel tidak aktif, memakai mode sudut");
    }
    FindPlayerVehicle_fn = (decltype(FindPlayerVehicle_fn))aml->GetSym(h, "_Z17FindPlayerVehicleib");
    srand((unsigned)time(nullptr));
    if (g_menuEnabled) {
        GetCoords_orig = (decltype(GetCoords_orig))aml->GetSym(h, "_Z25LIB_PointerGetCoordinatesiPiS_Pf");
        void* any = hook_target;
        bool ok = GetCoords_orig
               && InstallGotHook(any, 0x842fd8, 0x32703c, (void*)GetButton_hook, (void**)&GetButton_orig, "LIB_PointerGetButton")
               && InstallGotHook(any, 0x83f028, 0x31e2a4, (void*)SwapBuffers_hook, (void**)&SwapBuffers_orig, "OS_ScreenSwapBuffers");
        if (!ok) { g_menuEnabled = false; logger->Error("Menu in-game tidak aktif (hook gagal)"); }
    }
    logger->Info("Pearlescent aktif. Config: %s", g_iniPath.c_str());
}
