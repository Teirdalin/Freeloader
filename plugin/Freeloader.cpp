#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/SharedKing.h>
#include <kenshi/ZoneManager.h>
#include <kenshi/util/iVector2.h>
#include <ogre/OgreVector3.h>
#include "../src/StreamingPolicy.h"

namespace {
typedef void (*UpdateFn)(ZoneManager*, const Ogre::Vector3&);
typedef void (*WorldFn)(GameWorld*);

UpdateFn originalUpdate = 0;
WorldFn originalReset = 0;
WorldFn originalClear = 0;
bool attempted = false, active = false;
volatile LONG resetting = 0, epoch = 0;
LONG observedEpoch = -1;
DWORD updateThread = 0;
SharedKing* const* editorSingleton = 0;
freeloader::Predictor predictor;
freeloader::Sector leases[freeloader::MaxLeases];
unsigned int nextSample = 0, lastRequest = 0, lastReport = 0;
unsigned int requests = 0, frames = 0, loadingFrames = 0;
double worstUpdateMs = 0;
LARGE_INTEGER frequency;
int leadDistance = 1800, maxActiveZones = 32, intervalMs = 500;
bool diagnostics = true;

void log(const char* message) { DebugLog(message); }

void resetState() {
    predictor.reset();
    for (int i = 0; i < freeloader::MaxLeases; ++i) leases[i] = freeloader::Sector();
    nextSample = lastRequest = GetTickCount();
    lastReport = nextSample;
    requests = frames = loadingFrames = 0;
    worstUpdateMs = 0;
}

void resetHook(GameWorld* world) {
    InterlockedIncrement(&resetting);
    InterlockedIncrement(&epoch);
    originalReset(world);
    InterlockedDecrement(&resetting);
}
void clearHook(GameWorld* world) {
    InterlockedIncrement(&resetting);
    InterlockedIncrement(&epoch);
    originalClear(world);
    InterlockedDecrement(&resetting);
}

int setting(const wchar_t* path, const wchar_t* keyName, int fallback, int lo, int hi) {
    int value = static_cast<int>(GetPrivateProfileIntW(L"Freeloader", keyName, fallback, path));
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

bool configure() {
    HMODULE module = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&configure), &module)) return false;
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || static_cast<size_t>(slash - path) + 16 >= MAX_PATH) return false;
    wcscpy(slash + 1, L"Freeloader.ini");
    if (!setting(path, L"Enabled", 1, 0, 1)) return false;
    diagnostics = setting(path, L"Diagnostics", 1, 0, 1) != 0;
    leadDistance = setting(path, L"LookAheadDistance", 1800, 500, 3500);
    maxActiveZones = setting(path, L"MaxActiveZones", 32, 12, 64);
    intervalMs = setting(path, L"RequestIntervalMs", 500, 250, 5000);
    return true;
}

bool localAreaReady(ZoneManager* manager, const Ogre::Vector3& camera) {
    for (int x = -1; x <= 1; x += 2) for (int z = -1; z <= 1; z += 2) {
        const freeloader::Sector sector = freeloader::sectorAt(camera.x + x * 600.0,
            camera.z + z * 600.0);
        if (!sector.valid()) return false;
        ZoneMap* zone = manager->getZoneMap(sector.x, sector.z);
        if (!zone || !zone->isLoadedMT() || !zone->isTerrainCollisionLoaded()) return false;
    }
    return true;
}

void prefetch(ZoneManager* manager, const Ogre::Vector3& camera, unsigned int now) {
    const int loading = manager->isLoading();
    if (!manager->getCentralZone() || loading) {
        predictor.reset();
        return;
    }
    const int activeCount = manager->getNumActiveZones();
    if (now - nextSample < 250) return;
    nextSample = now;
    const freeloader::Candidates candidates = predictor.update(camera.x, camera.z, now, leadDistance);
    if (!candidates.count || !localAreaReady(manager, camera)) return;

    int freeSlot = -1;
    for (int i = 0; i < freeloader::MaxLeases; ++i) {
        if (leases[i].valid()) {
            ZoneMap* zone = manager->getZoneMap(leases[i].x, leases[i].z);
            if (!zone || (!zone->isActive() && !zone->isBeingLoadedMT()))
                leases[i] = freeloader::Sector();
        }
        if (!leases[i].valid() && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0 || now - lastRequest < static_cast<unsigned int>(intervalMs) ||
        activeCount >= maxActiveZones) return;
    MEMORYSTATUSEX memory = { sizeof(MEMORYSTATUSEX) };
    if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPhys < (2ull * 1024 * 1024 * 1024)) return;

    for (int i = 0; i < candidates.count; ++i) {
        const freeloader::Sector sector = candidates.sectors[i];
        ZoneMap* zone = manager->getZoneMap(sector.x, sector.z);
        if (!zone || zone->isLoadedMT() || zone->isBeingLoadedMT() || zone->isActive()) continue;
        lastRequest = now;
        if (manager->activateZoneMap(zone, iVector2(sector.x, sector.z), 0, ACTIVATION_CAMERA, 60.0f)) {
            leases[freeSlot] = sector;
            ++requests;
            if (diagnostics) {
                char message[160];
                sprintf_s(message, sizeof(message), "Freeloader: prefetch sector=(%d,%d) active=%d phase=%d",
                    sector.x, sector.z, activeCount, loading);
                log(message);
            }
        }
        break;
    }
}

void updateHook(ZoneManager* manager, const Ogre::Vector3& camera) {
    if (!active || !manager || InterlockedCompareExchange(&resetting, 0, 0)) {
        originalUpdate(manager, camera);
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (!updateThread) updateThread = thread;
    if (thread != updateThread) { originalUpdate(manager, camera); return; }
    const LONG currentEpoch = InterlockedCompareExchange(&epoch, 0, 0);
    if (observedEpoch != currentEpoch) { resetState(); observedEpoch = currentEpoch; }
    if (!ou || !*editorSingleton || ou->isPaused() || (*editorSingleton)->isLevelEditMode()) {
        predictor.reset();
        originalUpdate(manager, camera);
        return;
    }
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    const unsigned int now = GetTickCount();
    prefetch(manager, camera, now);
    originalUpdate(manager, camera);
    QueryPerformanceCounter(&end);
    const double duration = (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
    if (duration > worstUpdateMs) worstUpdateMs = duration;
    ++frames;
    if (manager->isLoading()) ++loadingFrames;
    if (diagnostics && now - lastReport >= 30000) {
        char message[224];
        sprintf_s(message, sizeof(message),
            "Freeloader: 30s sample requests=%u updates=%u loading_updates=%u worst_update_ms=%.2f",
            requests, frames, loadingFrames, worstUpdateMs);
        log(message);
        lastReport = now;
        requests = frames = loadingFrames = 0;
        worstUpdateMs = 0;
    }
}

// The installed KenshiLib does not export the update/reset methods or shou.
// Anchor to its resolved teardown method and validate the Steam 1.0.65 entries
// in the same mapped image before attaching hooks or accessing the singleton.
bool matchesCode(const unsigned char* address, const unsigned char* expected, size_t size) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        !(info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        (info.Protect & PAGE_GUARD)) return false;
    const size_t offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(info.BaseAddress);
    return offset <= info.RegionSize && size <= info.RegionSize - offset &&
        memcmp(address, expected, size) == 0;
}

template <typename Callback>
bool hook(const unsigned char* target, Callback* callback, Callback** original) {
    return KenshiLib::AddHook(reinterpret_cast<intptr_t>(target),
        reinterpret_cast<void*>(callback), original) == KenshiLib::SUCCESS && *original;
}

bool installHooks() {
    const intptr_t clearAddress = KenshiLib::GetRealAddress(&GameWorld::_clearAndDestroyGameWorldStuff);
    if (!clearAddress) return false;
    const unsigned char* clear = reinterpret_cast<const unsigned char*>(clearAddress);
    const uintptr_t base = reinterpret_cast<uintptr_t>(clear) - 0x36B940;
    const unsigned char* reset = reinterpret_cast<const unsigned char*>(base + 0x36CA40);
    const unsigned char* update = reinterpret_cast<const unsigned char*>(base + 0xA11B70);
    const unsigned char* editor = reinterpret_cast<const unsigned char*>(base + 0x791630);
    const unsigned char resetEntry[] = {0x48,0x8b,0xc4,0x55,0x48,0x8d,0x68,0x98,0x48,0x81,0xec,0x60,0x01,0x00,0x00};
    const unsigned char updateEntry[] = {0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x8d,0x68,0xa1};
    const unsigned char editorEntry[] = {0x48,0x8b,0x0d,0xf9,0x1f,0x9a,0x01,0x33,0xc0,0x48,0x39,0x81,0x98,0x02,0x00,0x00,0x0f,0x95,0xc0,0xc3};
    // The teardown entry can already be detoured by another RE_Kenshi mod.
    if (!matchesCode(reset, resetEntry, sizeof(resetEntry)) ||
        !matchesCode(update, updateEntry, sizeof(updateEntry)) ||
        !matchesCode(editor, editorEntry, sizeof(editorEntry)) ||
        reinterpret_cast<intptr_t>(editor) != KenshiLib::GetRealAddress(&SharedKing::isLevelEditMode))
        return false;
    editorSingleton = reinterpret_cast<SharedKing* const*>(base + 0x2133630);
    return hook(reset, resetHook, &originalReset) &&
        hook(clear, clearHook, &originalClear) &&
        hook(update, updateHook, &originalUpdate);
}
} // namespace

__declspec(dllexport) void startPlugin() {
    if (attempted) return;
    attempted = true;
    if (!configure()) {
        log("Freeloader: disabled or configuration path unavailable.");
        return;
    }
    if (!QueryPerformanceFrequency(&frequency) ||
        !installHooks()) {
        log("Freeloader: native hook setup failed; prefetch remains disabled.");
        return;
    }
    resetState();
    active = true;
    log("Freeloader: RE_Kenshi streaming hooks active (" __DATE__ " " __TIME__ ").");
}
