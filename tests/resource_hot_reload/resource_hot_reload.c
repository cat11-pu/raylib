#if defined(__linux__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "raylib.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#ifndef ASSET_ROOT
#define ASSET_ROOT "."
#endif

#define SLOT_COUNT 8
#define ROUND_COUNT 200
#define JOURNAL_CAP 24
#define TRACE_CAP 96
#define TEXT_CAP 192
#define HANDLE_MAGIC 0xC5u
#define HANDLE_INVALID UINT64_C(0)

typedef enum ResourceKind {
    RESOURCE_NONE = 0,
    RESOURCE_TEXTURE = 1,
    RESOURCE_MODEL = 2,
    RESOURCE_SOUND = 3
} ResourceKind;

typedef uint64_t ResourceHandle;

typedef struct ResourceData {
    Texture2D texture;
    Model model;
    Sound sound;
} ResourceData;

typedef struct ResourceSlot {
    bool live;
    ResourceKind kind;
    uint32_t generation;
    unsigned int refs;
    uint32_t successful_loads;
    char path[TEXT_CAP];
    ResourceData data;
    char journal[JOURNAL_CAP][TEXT_CAP];
    unsigned int journalStart;
    unsigned int journalCount;
} ResourceSlot;

typedef struct TestStats {
    uint64_t operations;
    uint64_t creates;
    uint64_t acquires;
    uint64_t releases;
    uint64_t uses;
    uint64_t hot_reloads;
    uint64_t hot_reload_ok;
    uint64_t hot_reload_injected_failure;
    uint64_t hot_reload_actual_failure;
    uint64_t invalid_operations;
    uint64_t rejected_bad_magic;
    uint64_t rejected_out_of_range;
    uint64_t rejected_wrong_kind;
    uint64_t rejected_stale;
    uint64_t rejected_dead;
    uint64_t cleaned_partial_loads;
    unsigned int peak_refs;
    unsigned int peak_live;
} TestStats;

typedef struct TraceEntry {
    uint64_t sequence;
    char text[TEXT_CAP];
} TraceEntry;

static ResourceSlot slots[SLOT_COUNT];
static TestStats stats;
static unsigned int liveCount;
static unsigned int totalRefs;
static ResourceHandle currentHandle;
static unsigned int currentRound;

static TraceEntry traceEntries[TRACE_CAP];
static unsigned int traceCount;
static uint64_t sequenceNumber;

static const char *texturePaths[] = {
    ASSET_ROOT "/examples/textures/resources/raylib_logo.png",
    ASSET_ROOT "/examples/textures/resources/raybunny.png"
};

static const char *modelPaths[] = {
    ASSET_ROOT "/examples/models/resources/models/obj/cube.obj",
    ASSET_ROOT "/examples/models/resources/models/obj/plane.obj"
};

static const char *soundPaths[] = {
    ASSET_ROOT "/examples/audio/resources/coin.wav",
    ASSET_ROOT "/examples/audio/resources/sound.wav"
};

static const char *missingPaths[] = {
    ASSET_ROOT "/examples/textures/resources/__missing__.png",
    ASSET_ROOT "/examples/models/resources/models/obj/__missing__.obj",
    ASSET_ROOT "/examples/audio/resources/__missing__.wav"
};

static const char *kindName(ResourceKind kind)
{
    switch (kind) {
        case RESOURCE_TEXTURE: return "texture";
        case RESOURCE_MODEL: return "model";
        case RESOURCE_SOUND: return "sound";
        default: return "none";
    }
}

static const char *kindPaths(ResourceKind kind, unsigned int variant)
{
    if (kind == RESOURCE_TEXTURE) return texturePaths[variant & 1u];
    if (kind == RESOURCE_MODEL) return modelPaths[variant & 1u];
    if (kind == RESOURCE_SOUND) return soundPaths[variant & 1u];
    return NULL;
}

static const char *missingPath(ResourceKind kind)
{
    if (kind == RESOURCE_TEXTURE) return missingPaths[0];
    if (kind == RESOURCE_MODEL) return missingPaths[1];
    if (kind == RESOURCE_SOUND) return missingPaths[2];
    return NULL;
}

static ResourceHandle encodeHandle(unsigned int index, uint32_t generation, ResourceKind kind)
{
    return ((uint64_t)HANDLE_MAGIC << 56) |
           ((uint64_t)(kind & 0xFFu) << 48) |
           ((uint64_t)generation << 16) |
           (uint64_t)(index & 0xFFFFu);
}

static bool decodeHandle(
    ResourceHandle handle,
    unsigned int *index,
    uint32_t *generation,
    ResourceKind *kind
)
{
    unsigned int decodedIndex = (unsigned int)(handle & 0xFFFFu);
    uint32_t decodedGeneration = (uint32_t)((handle >> 16) & UINT32_C(0xFFFFFFFF));
    ResourceKind decodedKind = (ResourceKind)((handle >> 48) & 0xFFu);
    unsigned int magic = (unsigned int)((handle >> 56) & 0xFFu);

    if ((handle == HANDLE_INVALID) || (magic != HANDLE_MAGIC) ||
        (decodedKind < RESOURCE_TEXTURE) || (decodedKind > RESOURCE_SOUND) ||
        (decodedGeneration == 0)) return false;

    if (index != NULL) *index = decodedIndex;
    if (generation != NULL) *generation = decodedGeneration;
    if (kind != NULL) *kind = decodedKind;
    return true;
}

static void recordTrace(const char *format, ...)
{
    va_list args;

    stats.operations++;
    sequenceNumber++;

    if (traceCount >= TRACE_CAP) return;

    traceEntries[traceCount].sequence = sequenceNumber;
    va_start(args, format);
    vsnprintf(traceEntries[traceCount].text, TEXT_CAP, format, args);
    va_end(args);
    traceCount++;
}

static void resetTrace(void)
{
    traceCount = 0;
}

static void recordSlot(ResourceSlot *slot, const char *format, ...)
{
    va_list args;
    unsigned int writeIndex;

    if (slot == NULL) return;

    if (slot->journalCount == JOURNAL_CAP) {
        slot->journalStart = (slot->journalStart + 1u) % JOURNAL_CAP;
        slot->journalCount--;
    }

    writeIndex = (slot->journalStart + slot->journalCount) % JOURNAL_CAP;
    snprintf(slot->journal[writeIndex], TEXT_CAP, "#%llu ",
             (unsigned long long)sequenceNumber);

    va_start(args, format);
    vsnprintf(slot->journal[writeIndex] + strlen(slot->journal[writeIndex]),
              TEXT_CAP - strlen(slot->journal[writeIndex]), format, args);
    va_end(args);
    slot->journalCount++;
}

static bool isResourceDataValid(ResourceKind kind, const ResourceData *data)
{
    if (kind == RESOURCE_TEXTURE) return IsTextureValid(data->texture);
    if (kind == RESOURCE_MODEL) return IsModelValid(data->model);
    if (kind == RESOURCE_SOUND) return IsSoundValid(data->sound);
    return false;
}

static void unloadResource(ResourceKind kind, ResourceData *data)
{
    if (kind == RESOURCE_TEXTURE) UnloadTexture(data->texture);
    else if (kind == RESOURCE_MODEL) UnloadModel(data->model);
    else if (kind == RESOURCE_SOUND) UnloadSound(data->sound);

    memset(data, 0, sizeof(*data));
}

static bool loadResource(
    ResourceKind kind,
    const char *path,
    bool injectFailureAfterLoad,
    ResourceData *data
)
{
    bool valid = false;

    memset(data, 0, sizeof(*data));

    if (kind == RESOURCE_TEXTURE) data->texture = LoadTexture(path);
    else if (kind == RESOURCE_MODEL) data->model = LoadModel(path);
    else if (kind == RESOURCE_SOUND) data->sound = LoadSound(path);
    else return false;

    valid = isResourceDataValid(kind, data);

    if (injectFailureAfterLoad && valid) {
        unloadResource(kind, data);
        stats.cleaned_partial_loads++;
        return false;
    }

    if (!valid) {
        if (kind == RESOURCE_MODEL) {
            UnloadModel(data->model);
        } else {
            unloadResource(kind, data);
        }
        stats.cleaned_partial_loads++;
        return false;
    }

    return true;
}

static bool useResource(ResourceKind kind, const ResourceData *data)
{
    if (kind == RESOURCE_TEXTURE) {
        if (!IsTextureValid(data->texture)) return false;
        SetTextureFilter(data->texture, TEXTURE_FILTER_BILINEAR);
        return true;
    }

    if (kind == RESOURCE_MODEL) {
        if (!IsModelValid(data->model)) return false;
        (void)GetModelBoundingBox(data->model);
        return true;
    }

    if (kind == RESOURCE_SOUND) {
        if (!IsSoundValid(data->sound)) return false;
        SetSoundVolume(data->sound, 0.25f);
        return true;
    }

    return false;
}

static ResourceSlot *slotFromHandle(
    ResourceHandle handle,
    const char **rejection,
    ResourceKind expectedKind
)
{
    unsigned int index;
    uint32_t generation;
    ResourceKind kind;

    if (!decodeHandle(handle, &index, &generation, &kind)) {
        if (rejection != NULL) *rejection = "bad-magic-or-kind";
        stats.rejected_bad_magic++;
        return NULL;
    }

    if (index >= SLOT_COUNT) {
        if (rejection != NULL) *rejection = "index-out-of-range";
        stats.rejected_out_of_range++;
        return NULL;
    }

    if ((expectedKind != RESOURCE_NONE) && (kind != expectedKind)) {
        if (rejection != NULL) *rejection = "wrong-kind";
        stats.rejected_wrong_kind++;
        return NULL;
    }

    if (!slots[index].live) {
        if (rejection != NULL) *rejection = "stale-or-dead";
        stats.rejected_stale++;
        return NULL;
    }

    if (slots[index].generation != generation) {
        if (rejection != NULL) *rejection = "stale-generation";
        stats.rejected_stale++;
        return NULL;
    }

    if (slots[index].kind != kind) {
        if (rejection != NULL) *rejection = "kind-mismatch";
        stats.rejected_wrong_kind++;
        return NULL;
    }

    if (slots[index].refs == 0) {
        if (rejection != NULL) *rejection = "zero-refcount";
        stats.rejected_dead++;
        return NULL;
    }

    return &slots[index];
}

static void printSequence(ResourceHandle handle)
{
    unsigned int index = 0;
    uint32_t generation = 0;
    ResourceKind kind = RESOURCE_NONE;
    bool decoded = decodeHandle(handle, &index, &generation, &kind);

    printf("minimal operation sequence:\n");

    if (decoded && (index < SLOT_COUNT) && (slots[index].journalCount > 0)) {
        ResourceSlot *slot = &slots[index];
        unsigned int i;

        printf("  handle=%s[%u] generation=%u refs=%u\n",
               kindName(slot->kind), index, slot->generation, slot->refs);
        for (i = 0; i < slot->journalCount; i++) {
            unsigned int at = (slot->journalStart + i) % JOURNAL_CAP;
            printf("  %s\n", slot->journal[at]);
        }
        return;
    }

    if (traceCount > 0) {
        unsigned int i;
        for (i = 0; i < traceCount; i++) {
            printf("  %s\n", traceEntries[i].text);
        }
        return;
    }

    printf("  <no recorded operations>\n");
}

static void failTest(
    const char *file,
    int line,
    ResourceHandle handle,
    const char *format,
    ...
)
{
    va_list args;

    printf("\nFAIL: %s:%d (round %u)\n", file, line, currentRound);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    printSequence(handle);
    printf("state: live=%u totalRefs=%u\n", liveCount, totalRefs);
    fflush(stdout);
    exit(EXIT_FAILURE);
}

#define REQUIRE(condition, handle, ...) \
    do { \
        if (!(condition)) failTest(__FILE__, __LINE__, (handle), __VA_ARGS__); \
    } while (0)

static unsigned int slotIndex(ResourceSlot *slot)
{
    return (unsigned int)(slot - &slots[0]);
}

static ResourceHandle createResource(
    ResourceKind kind,
    const char *path,
    bool injectFailureAfterLoad
)
{
    unsigned int i;
    ResourceSlot *slot = NULL;
    ResourceData data;
    uint32_t generation;

    for (i = 0; i < SLOT_COUNT; i++) {
        if (!slots[i].live) {
            slot = &slots[i];
            break;
        }
    }

    REQUIRE(slot != NULL, HANDLE_INVALID, "slot table exhausted\n");
    REQUIRE(loadResource(kind, path, injectFailureAfterLoad, &data), HANDLE_INVALID,
            "resource creation failed: %s %s\n", kindName(kind), path);

    generation = slot->generation + 1u;
    if (generation == 0) generation = 1;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    slot->live = true;
    slot->kind = kind;
    slot->refs = 1;
    slot->successful_loads = 1;
    slot->data = data;
    snprintf(slot->path, sizeof(slot->path), "%s", path);

    liveCount++;
    totalRefs++;
    stats.creates++;
    if (totalRefs > stats.peak_refs) stats.peak_refs = totalRefs;
    if (liveCount > stats.peak_live) stats.peak_live = liveCount;

    currentHandle = encodeHandle(i, generation, kind);
    recordTrace("CREATE %s[%u] %s", kindName(kind), i, path);
    recordSlot(slot, "CREATE %s[%u] %s refs=1 generation=%u",
               kindName(kind), i, path, generation);
    return currentHandle;
}

static bool acquireResource(ResourceHandle handle)
{
    ResourceSlot *slot = slotFromHandle(handle, NULL, RESOURCE_NONE);

    if (slot == NULL) {
        stats.invalid_operations++;
        recordTrace("ACQUIRE rejected handle=0x%llx",
                    (unsigned long long)handle);
        return false;
    }

    slot->refs++;
    totalRefs++;
    stats.acquires++;
    if (totalRefs > stats.peak_refs) stats.peak_refs = totalRefs;
    recordTrace("ACQUIRE %s[%u] refs=%u", kindName(slot->kind),
                slotIndex(slot), slot->refs);
    recordSlot(slot, "ACQUIRE refs=%u", slot->refs);
    return true;
}

static bool releaseResource(ResourceHandle handle)
{
    ResourceSlot *slot = slotFromHandle(handle, NULL, RESOURCE_NONE);

    if (slot == NULL) {
        stats.invalid_operations++;
        recordTrace("RELEASE rejected handle=0x%llx",
                    (unsigned long long)handle);
        return false;
    }

    if (slot->refs > 1) {
        slot->refs--;
        totalRefs--;
        stats.releases++;
        recordTrace("RELEASE %s[%u] refs=%u", kindName(slot->kind),
                    slotIndex(slot), slot->refs);
        recordSlot(slot, "RELEASE refs=%u", slot->refs);
        return true;
    }

    REQUIRE(slot->refs == 1, handle, "refcount underflow for %s[%u]\n",
            kindName(slot->kind), slotIndex(slot));

    recordTrace("RELEASE %s[%u] final refs=0", kindName(slot->kind),
                slotIndex(slot));
    recordSlot(slot, "RELEASE final -> unload resource");

    unloadResource(slot->kind, &slot->data);
    slot->live = false;
    slot->refs = 0;
    slot->generation++;
    if (slot->generation == 0) slot->generation = 1;

    liveCount--;
    totalRefs--;
    stats.releases++;
    return true;
}

static bool useHandle(ResourceHandle handle)
{
    ResourceSlot *slot = slotFromHandle(handle, NULL, RESOURCE_NONE);
    bool used;

    if (slot == NULL) {
        stats.invalid_operations++;
        recordTrace("USE rejected handle=0x%llx",
                    (unsigned long long)handle);
        return false;
    }

    used = useResource(slot->kind, &slot->data);
    REQUIRE(used, handle, "resource failed validation before use: %s[%u]\n",
            kindName(slot->kind), slotIndex(slot));

    stats.uses++;
    recordTrace("USE %s[%u]", kindName(slot->kind), slotIndex(slot));
    recordSlot(slot, "USE ok refs=%u", slot->refs);
    return true;
}

static bool hotReloadResource(
    ResourceHandle handle,
    const char *newPath,
    bool injectFailureAfterLoad,
    bool *injected
)
{
    ResourceSlot *slot = slotFromHandle(handle, NULL, RESOURCE_NONE);
    ResourceData replacement;
    unsigned int index;
    uint32_t generation;
    ResourceKind kind;

    *injected = false;

    if (slot == NULL) {
        stats.invalid_operations++;
        recordTrace("RELOAD rejected handle=0x%llx",
                    (unsigned long long)handle);
        return false;
    }

    decodeHandle(handle, &index, &generation, &kind);
    stats.hot_reloads++;
    recordTrace("RELOAD %s[%u] old=%s new=%s",
                kindName(kind), index, slot->path, newPath);
    recordSlot(slot, "RELOAD begin old=%s new=%s refs=%u",
               slot->path, newPath, slot->refs);

    if (!loadResource(kind, newPath, injectFailureAfterLoad, &replacement)) {
        *injected = injectFailureAfterLoad;
        if (injectFailureAfterLoad) stats.hot_reload_injected_failure++;
        else stats.hot_reload_actual_failure++;

        REQUIRE(slot->live && (slot->refs > 0) &&
                isResourceDataValid(slot->kind, &slot->data),
                handle, "failed reload damaged the old resource\n");
        recordTrace("RELOAD rejected; old resource retained");
        recordSlot(slot, "RELOAD failed -> old resource retained refs=%u",
                   slot->refs);
        return false;
    }

    unloadResource(kind, &slot->data);
    slot->data = replacement;
    slot->successful_loads++;
    snprintf(slot->path, sizeof(slot->path), "%s", newPath);
    stats.hot_reload_ok++;

    REQUIRE(isResourceDataValid(slot->kind, &slot->data), handle,
            "reloaded resource is invalid\n");
    recordTrace("RELOAD ok");
    recordSlot(slot, "RELOAD ok loads=%u refs=%u",
               slot->successful_loads, slot->refs);
    return true;
}

static void checkCleanState(const char *where)
{
    unsigned int i;

    for (i = 0; i < SLOT_COUNT; i++) {
        REQUIRE(!slots[i].live, encodeHandle(i, slots[i].generation, slots[i].kind),
                "live slot leaked at %s: %s[%u] refs=%u path=%s\n",
                where, kindName(slots[i].kind), i, slots[i].refs, slots[i].path);
        REQUIRE(slots[i].refs == 0, encodeHandle(i, slots[i].generation,
                slots[i].kind),
                "nonzero refcount at %s: %s[%u] refs=%u\n",
                where, kindName(slots[i].kind), i, slots[i].refs);
    }

    REQUIRE(liveCount == 0, HANDLE_INVALID,
            "live resource count is %u at %s\n", liveCount, where);
    REQUIRE(totalRefs == 0, HANDLE_INVALID,
            "total reference count is %u at %s\n", totalRefs, where);
}

static void runInvalidHandleCounterexamples(ResourceHandle validHandle)
{
    unsigned int index;
    uint32_t generation;
    ResourceKind kind;
    ResourceHandle badMagic;
    ResourceHandle badIndex;
    ResourceHandle wrongKind;
    ResourceHandle zeroGeneration;

    REQUIRE(decodeHandle(validHandle, &index, &generation, &kind),
            validHandle, "test handle cannot be decoded\n");

    badMagic = validHandle ^ (UINT64_C(1) << 56);
    badIndex = encodeHandle(SLOT_COUNT + 3u, generation, kind);
    wrongKind = encodeHandle(index, generation,
                             kind == RESOURCE_TEXTURE ? RESOURCE_MODEL : RESOURCE_TEXTURE);
    zeroGeneration = encodeHandle(index, 0, kind);

    REQUIRE(!acquireResource(badMagic), badMagic,
            "bad-magic handle was accepted\n");
    REQUIRE(!releaseResource(badMagic), badMagic,
            "bad-magic handle was released\n");
    REQUIRE(!useHandle(badIndex), badIndex,
            "out-of-range handle was used\n");
    REQUIRE(!acquireResource(wrongKind), wrongKind,
            "wrong-kind handle was accepted\n");
    REQUIRE(!hotReloadResource(wrongKind, missingPath(kind), false, &(bool){false}),
            wrongKind, "wrong-kind handle was reloaded\n");
    REQUIRE(!acquireResource(zeroGeneration), zeroGeneration,
            "zero-generation handle was accepted\n");
    REQUIRE(!acquireResource(HANDLE_INVALID), HANDLE_INVALID,
            "zero handle was accepted\n");
}

static void runOneScenario(unsigned int round, ResourceKind kind, unsigned int scenario)
{
    ResourceHandle handle;
    ResourceSlot *slot;
    ResourceHandle staleHandle;
    const char *nextPath;
    bool expectFailure;
    bool expectInjected;
    bool reloadResult;
    bool injected = false;
    unsigned int expectedRefs = 1;

    resetTrace();
    recordTrace("SCENARIO round=%u kind=%s", round, kindName(kind));

    handle = createResource(kind, kindPaths(kind, round), false);
    currentHandle = handle;
    slot = slotFromHandle(handle, NULL, kind);
    REQUIRE(slot != NULL, handle, "freshly created handle is not valid\n");
    runInvalidHandleCounterexamples(handle);
    REQUIRE(slot->refs == 1, handle,
            "invalid handles changed live refcount to %u\n", slot->refs);

    REQUIRE(acquireResource(handle), handle, "first acquire failed\n");
    expectedRefs++;
    REQUIRE(acquireResource(handle), handle, "second acquire failed\n");
    expectedRefs++;
    REQUIRE(slot->refs == expectedRefs, handle,
            "refcount did not reach %u (actual %u)\n", expectedRefs, slot->refs);
    REQUIRE(totalRefs == expectedRefs, handle,
            "global refcount is inconsistent: %u != %u\n",
            totalRefs, expectedRefs);

    REQUIRE(useHandle(handle), handle, "valid use failed\n");

    expectInjected = ((scenario % 7u) == 3u);
    expectFailure = expectInjected || ((scenario % 5u) == 1u);
    nextPath = expectFailure && !expectInjected
        ? missingPath(kind)
        : kindPaths(kind, round + 1u);

    reloadResult = hotReloadResource(handle, nextPath, expectInjected, &injected);
    REQUIRE(reloadResult == !expectFailure, handle,
            "reload result mismatch: result=%d expectedFailure=%d injected=%d\n",
            reloadResult, expectFailure, injected);
    REQUIRE(injected == expectInjected, handle,
            "failure injection was not reported correctly\n");
    REQUIRE(slot->refs == expectedRefs, handle,
            "reload changed refcount: %u != %u\n", slot->refs, expectedRefs);
    REQUIRE(isResourceDataValid(kind, &slot->data), handle,
            "handle is not usable after reload path\n");

    REQUIRE(releaseResource(handle), handle, "release 1 failed\n");
    expectedRefs--;
    REQUIRE(releaseResource(handle), handle, "release 2 failed\n");
    expectedRefs--;
    REQUIRE(slot->refs == expectedRefs, handle,
            "refcount did not recover to %u (actual %u)\n",
            expectedRefs, slot->refs);

    staleHandle = handle;
    REQUIRE(releaseResource(handle), handle, "final release failed\n");
    REQUIRE(slot->live == false, staleHandle,
            "slot still live after final release\n");
    REQUIRE(slot->refs == 0, staleHandle,
            "refcount did not recover to zero\n");
    REQUIRE(totalRefs == 0, staleHandle,
            "global refcount did not recover to zero\n");
    REQUIRE(liveCount == 0, staleHandle,
            "live resource count did not recover to zero\n");

    REQUIRE(!releaseResource(staleHandle), staleHandle,
            "double release of stale handle succeeded\n");
    REQUIRE(!useHandle(staleHandle), staleHandle,
            "use of stale handle succeeded\n");
    REQUIRE(!hotReloadResource(staleHandle, kindPaths(kind, round),
            false, &injected), staleHandle, "reload of stale handle succeeded\n");
    REQUIRE(slot->refs == 0, staleHandle,
            "stale operations changed refcount to %u\n", slot->refs);

    recordTrace("SCENARIO clean");
}

static void tryInitAudio(void)
{
#if defined(__linux__)
    static char nullConfig[TEXT_CAP * 2];
#endif

    InitAudioDevice();
    if (IsAudioDeviceReady()) return;

#if defined(__linux__)
    snprintf(nullConfig, sizeof(nullConfig), "%s/tests/resource_hot_reload/alsa-null.conf",
             ASSET_ROOT);
    if (access(nullConfig, R_OK) == 0) {
        setenv("ALSA_CONFIG_PATH", nullConfig, 1);
        InitAudioDevice();
    }
#endif
}

static void printSummary(void)
{
    printf("\nRESULT: PASS\n");
    printf("rounds: %u\n", ROUND_COUNT);
    printf("scenarios: %llu hot_reloads=%llu ok=%llu injected_fail=%llu actual_fail=%llu\n",
           (unsigned long long)stats.creates,
           (unsigned long long)stats.hot_reloads,
           (unsigned long long)stats.hot_reload_ok,
           (unsigned long long)stats.hot_reload_injected_failure,
           (unsigned long long)stats.hot_reload_actual_failure);
    printf("acquires=%llu releases=%llu uses=%llu\n",
           (unsigned long long)stats.acquires,
           (unsigned long long)stats.releases,
           (unsigned long long)stats.uses);
    printf("invalid_ops=%llu stale=%llu wrong_kind=%llu oob=%llu bad_magic=%llu dead=%llu\n",
           (unsigned long long)stats.invalid_operations,
           (unsigned long long)stats.rejected_stale,
           (unsigned long long)stats.rejected_wrong_kind,
           (unsigned long long)stats.rejected_out_of_range,
           (unsigned long long)stats.rejected_bad_magic,
           (unsigned long long)stats.rejected_dead);
    printf("cleaned_partial_loads=%llu peak_refs=%u peak_live=%u\n",
           (unsigned long long)stats.cleaned_partial_loads,
           stats.peak_refs, stats.peak_live);
    printf("final_live=%u final_refs=%u\n", liveCount, totalRefs);
}

int main(void)
{
    unsigned int round;
    ResourceKind kind;
    unsigned int scenario = 0;

    SetTraceLogLevel(LOG_ERROR);
    InitWindow(64, 64, "raylib resource hot reload test");
    if (!IsWindowReady()) {
        printf("FAIL: window/OpenGL context could not be created\n");
        return EXIT_FAILURE;
    }

    tryInitAudio();
    if (!IsAudioDeviceReady()) {
        printf("FAIL: audio device could not be initialized\n");
        CloseWindow();
        return EXIT_FAILURE;
    }

    for (round = 0; round < ROUND_COUNT; round++) {
        currentRound = round;
        for (kind = RESOURCE_TEXTURE; kind <= RESOURCE_SOUND; kind++) {
            runOneScenario(round, kind, scenario++);
            checkCleanState("scenario boundary");
        }
    }

    checkCleanState("test end");
    printSummary();

    CloseAudioDevice();
    CloseWindow();
    return EXIT_SUCCESS;
}
