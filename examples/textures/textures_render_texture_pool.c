/*******************************************************************************************
*
*   raylib [textures] example - render texture pool
*
*   Example complexity rating: [★★★☆] 3/4
*
*   Example demonstrates how to reuse offscreen render targets by (width, height, format),
*   delay reclamation for frames still in flight, and enforce a VRAM budget with LRU
*   eviction. Run with "--test" or "--benchmark" for non-interactive checks.
*
*   Example licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software
*
********************************************************************************************/

#include "raylib.h"
#include "rlgl.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#define RTPOOL_ESTIMATED_OVERHEAD   (4*1024)    // FBO/driver bookkeeping estimate per target
#define RTPOOL_MAX_FORMATS          16          // Groups retained by this example

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef enum {
    RTPOOL_TARGET_EMPTY = 0,
    RTPOOL_TARGET_RETIRING,
    RTPOOL_TARGET_FREE,
    RTPOOL_TARGET_LIVE
} RTPoolTargetState;

typedef struct RTPoolStats {
    unsigned long long acquires;
    unsigned long long reuses;
    unsigned long long allocations;
    unsigned long long releases;
    unsigned long long evictions;
    unsigned long long rejects;
    unsigned long long overshoots;
} RTPoolStats;

typedef struct RTPoolTarget {
    RenderTexture2D target;
    size_t bytes;
    int group;
    int nextFree;
    unsigned long long lastUseFrame;
    unsigned long long retireFrame;
    unsigned int generation;
    RTPoolTargetState state;
} RTPoolTarget;

typedef struct RTPoolGroup {
    int width;
    int height;
    int format;
    int firstFree;
    int targetCount;
} RTPoolGroup;

// NOTE: Pool operations are not thread-safe. Tick once per rendered frame on the
// thread that owns the graphics context.
typedef struct RTPool {
    RTPoolTarget *targets;
    int targetCount;
    int targetCapacity;
    RTPoolGroup *groups;
    int groupCount;
    int groupCapacity;
    int maxTargets;

    size_t budgetBytes;         // Eviction target, 0 means unlimited
    size_t hardLimitBytes;      // Allocation ceiling, 0 means unlimited
    size_t residentBytes;
    size_t peakBytes;

    // Frame delay keeps the target out of the free list while submitted GPU work may
    // still reference it. Production engines should pair this with a GL fence and
    // only publish the slot after the fence has signaled.
    int reclaimDelayFrames;
    bool clearOnRelease;
    unsigned long long frame;
    int activeTarget;

    RTPoolStats stats;
} RTPool;

typedef struct RTPoolLease {
    RenderTexture2D target;
    int index;
    unsigned int generation;
} RTPoolLease;

typedef struct ResolutionCase {
    const char *name;
    int width;
    int height;
} ResolutionCase;

//----------------------------------------------------------------------------------
// Module Functions Declaration
//----------------------------------------------------------------------------------
static bool RTPoolInit(RTPool *pool, int maxTargets, size_t budgetBytes, size_t hardLimitBytes, int reclaimDelayFrames);
static void RTPoolShutdown(RTPool *pool);
static void RTPoolTick(RTPool *pool);
static bool RTPoolAcquire(RTPool *pool, int width, int height, int format, RTPoolLease *lease);
static bool RTPoolBegin(RTPool *pool, RTPoolLease lease, Color clearColor);
static void RTPoolEnd(RTPool *pool);
static bool RTPoolRelease(RTPool *pool, RTPoolLease *lease);
static size_t RTPoolEstimateBytes(int width, int height, int format);

static int RunPoolTests(void);
static void RunBenchmark(void);
static void DrawPoolScene(RenderTexture2D target, float time);
static void DrawPoolStats(RTPool *pool, int x, int y);
static bool ColorMatches(Color actual, Color expected);

//----------------------------------------------------------------------------------
// Program main entry point
//----------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    bool runTests = false;
    bool runBenchmark = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--test") == 0) runTests = true;
        else if (strcmp(argv[i], "--benchmark") == 0) runBenchmark = true;
        else if (strcmp(argv[i], "--all") == 0) runTests = runBenchmark = true;
    }

    SetTraceLogLevel(LOG_WARNING);
    if (runTests || runBenchmark) SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(960, 540, "raylib [textures] example - render texture pool");

    int failures = 0;
    if (runTests) failures = RunPoolTests();
    if (runBenchmark) RunBenchmark();

    if (runTests || runBenchmark)
    {
        CloseWindow();
        return (failures == 0)? 0 : 1;
    }

    static const ResolutionCase cases[] = {
        { "640x360", 640, 360 },
        { "1280x720", 1280, 720 },
        { "1920x1080", 1920, 1080 },
        { "960x540", 960, 540 },
        { "1600x900", 1600, 900 }
    };
    const int caseCount = (int)(sizeof(cases)/sizeof(cases[0]));
    int currentCase = 0;

    RTPool pool = { 0 };
    RTPoolInit(&pool, 12, 64*1024*1024, 128*1024*1024, 2);

    RTPoolLease lease = { 0 };
    bool leaseHeld = false;

    while (!WindowShouldClose())
    {
        RTPoolTick(&pool);

        for (int i = 0; i < caseCount; i++)
        {
            if ((IsKeyPressed(KEY_ONE + i)) && (i != currentCase))
            {
                if (leaseHeld) RTPoolRelease(&pool, &lease);
                currentCase = i;
                leaseHeld = false;
            }
        }

        if (!leaseHeld)
        {
            leaseHeld = RTPoolAcquire(&pool, cases[currentCase].width, cases[currentCase].height,
                                      PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease);
        }

        if (leaseHeld)
        {
            RTPoolBegin(&pool, lease, BLACK);
                DrawPoolScene(lease.target, (float)GetTime());
            RTPoolEnd(&pool);
        }

        BeginDrawing();
            ClearBackground(RAYWHITE);

            if (leaseHeld)
            {
                Rectangle source = {
                    0.0f, 0.0f,
                    (float)lease.target.texture.width,
                    -(float)lease.target.texture.height
                };
                Rectangle dest = { 0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight() };
                DrawTexturePro(lease.target.texture, source, dest, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
            }

            DrawRectangle(0, 0, GetScreenWidth(), 112, Fade(BLACK, 0.72f));
            DrawText("1-5: switch resolution, targets are retained for later reuse", 10, 8, 16, WHITE);
            DrawText(TextFormat("Current: %s | releases are delayed by %i frames",
                     cases[currentCase].name, pool.reclaimDelayFrames), 10, 28, 16, LIGHTGRAY);
            DrawPoolStats(&pool, 10, 48);

        EndDrawing();
    }

    if (leaseHeld) RTPoolRelease(&pool, &lease);
    RTPoolShutdown(&pool);
    CloseWindow();

    return 0;
}

//----------------------------------------------------------------------------------
// Render target pool implementation
//----------------------------------------------------------------------------------
static bool RTPoolGrowTargets(RTPool *pool, int required)
{
    if (required <= pool->targetCapacity) return true;

    int capacity = (pool->targetCapacity > 0)? pool->targetCapacity : 8;
    while (capacity < required) capacity *= 2;

    RTPoolTarget *targets = (RTPoolTarget *)realloc(pool->targets, (size_t)capacity*sizeof(RTPoolTarget));
    if (targets == NULL) return false;

    for (int i = pool->targetCapacity; i < capacity; i++)
    {
        memset(&targets[i], 0, sizeof(RTPoolTarget));
        targets[i].group = -1;
        targets[i].nextFree = -1;
        targets[i].state = RTPOOL_TARGET_EMPTY;
    }

    pool->targets = targets;
    pool->targetCapacity = capacity;

    return true;
}

static bool RTPoolGrowGroups(RTPool *pool, int required)
{
    if (required <= pool->groupCapacity) return true;

    int capacity = (pool->groupCapacity > 0)? pool->groupCapacity : 8;
    while (capacity < required) capacity *= 2;

    RTPoolGroup *groups = (RTPoolGroup *)realloc(pool->groups, (size_t)capacity*sizeof(RTPoolGroup));
    if (groups == NULL) return false;

    for (int i = pool->groupCapacity; i < capacity; i++)
    {
        memset(&groups[i], 0, sizeof(RTPoolGroup));
        groups[i].firstFree = -1;
    }

    pool->groups = groups;
    pool->groupCapacity = capacity;

    return true;
}

static bool RTPoolInit(RTPool *pool, int maxTargets, size_t budgetBytes, size_t hardLimitBytes, int reclaimDelayFrames)
{
    memset(pool, 0, sizeof(RTPool));

    pool->maxTargets = maxTargets;
    pool->budgetBytes = budgetBytes;
    pool->hardLimitBytes = hardLimitBytes;
    pool->reclaimDelayFrames = reclaimDelayFrames;
    pool->activeTarget = -1;

    if (!RTPoolGrowTargets(pool, (maxTargets < 8)? maxTargets : 8)) return false;
    if (!RTPoolGrowGroups(pool, 8)) return false;

    return true;
}

static void RTPoolShutdown(RTPool *pool)
{
    if (pool->activeTarget >= 0) RTPoolEnd(pool);

    for (int i = 0; i < pool->targetCount; i++)
    {
        if (pool->targets[i].state != RTPOOL_TARGET_EMPTY) UnloadRenderTexture(pool->targets[i].target);
    }

    free(pool->targets);
    free(pool->groups);
    memset(pool, 0, sizeof(RTPool));
    pool->activeTarget = -1;
}

static int RTPoolFindGroup(RTPool *pool, int width, int height, int format)
{
    for (int i = 0; i < pool->groupCount; i++)
    {
        RTPoolGroup *group = &pool->groups[i];
        if ((group->width == width) && (group->height == height) && (group->format == format)) return i;
    }

    return -1;
}

static int RTPoolGetGroup(RTPool *pool, int width, int height, int format)
{
    int index = RTPoolFindGroup(pool, width, height, format);
    if (index >= 0) return index;

    if ((pool->groupCount >= RTPOOL_MAX_FORMATS) || !RTPoolGrowGroups(pool, pool->groupCount + 1)) return -1;

    index = pool->groupCount;
    pool->groups[index].width = width;
    pool->groups[index].height = height;
    pool->groups[index].format = format;
    pool->groups[index].firstFree = -1;
    pool->groups[index].targetCount = 0;
    pool->groupCount++;

    return index;
}

static bool RTPoolValidateRequest(int width, int height, int format)
{
    if ((width <= 0) || (height <= 0)) return false;
    if ((format < PIXELFORMAT_UNCOMPRESSED_GRAYSCALE) || (format >= PIXELFORMAT_COMPRESSED_DXT1_RGB)) return false;
    if (RTPoolEstimateBytes(width, height, format) == 0) return false;

    return true;
}

static size_t RTPoolEstimateBytes(int width, int height, int format)
{
    int colorBytes = GetPixelDataSize(width, height, format);
    if (colorBytes <= 0) return 0;

    size_t pixels = (size_t)width*(size_t)height;
    if (pixels > (SIZE_MAX - RTPOOL_ESTIMATED_OVERHEAD - (size_t)colorBytes)/4u) return 0;

    size_t depthBytes = pixels*4u; // raylib depth renderbuffer is 24-bit, stored as 32-bit
    return (size_t)colorBytes + depthBytes + RTPOOL_ESTIMATED_OVERHEAD;
}

static bool RTPoolExceeds(size_t residentBytes, size_t requiredBytes, size_t limitBytes)
{
    if (limitBytes == 0) return false;
    if (requiredBytes > limitBytes) return true;

    return residentBytes > (limitBytes - requiredBytes);
}

static void RTPoolResetRenderState(void)
{
    // BeginTextureMode()/EndTextureMode() restore FBO, viewport and matrices only.
    // All state below can leak between passes if it is not reset explicitly.
    EndShaderMode();
    EndBlendMode();
    EndScissorMode();
    rlEnableColorBlend();
    rlDisableDepthTest();
    rlEnableDepthMask();
    rlDisableBackfaceCulling();
    rlColorMask(true, true, true, true);
    rlDisableWireMode();
    rlDisablePointMode();
    rlDisableSmoothLines();
    rlSetLineWidth(1.0f);
    rlSetPointSize(1.0f);
}

static void RTPoolClearTarget(RenderTexture2D target, Color clearColor)
{
    BeginTextureMode(target);
        RTPoolResetRenderState();
        ClearBackground(clearColor);
    EndTextureMode();
}

static int RTPoolFindLruFree(RTPool *pool)
{
    int best = -1;
    unsigned long long bestFrame = 0;

    for (int i = 0; i < pool->targetCount; i++)
    {
        if (pool->targets[i].state != RTPOOL_TARGET_FREE) continue;

        if ((best < 0) || (pool->targets[i].lastUseFrame < bestFrame))
        {
            best = i;
            bestFrame = pool->targets[i].lastUseFrame;
        }
    }

    return best;
}

static void RTPoolRemoveFree(RTPool *pool, int targetIndex)
{
    RTPoolTarget *target = &pool->targets[targetIndex];
    RTPoolGroup *group = &pool->groups[target->group];

    if (group->firstFree == targetIndex)
    {
        group->firstFree = target->nextFree;
    }
    else
    {
        int previous = group->firstFree;
        while ((previous >= 0) && (pool->targets[previous].nextFree != targetIndex))
        {
            previous = pool->targets[previous].nextFree;
        }

        if (previous >= 0) pool->targets[previous].nextFree = target->nextFree;
    }

    target->nextFree = -1;
}

static void RTPoolEvict(RTPool *pool, int targetIndex)
{
    RTPoolTarget *target = &pool->targets[targetIndex];
    RTPoolGroup *group = &pool->groups[target->group];

    if (target->state == RTPOOL_TARGET_FREE) RTPoolRemoveFree(pool, targetIndex);

    UnloadRenderTexture(target->target);
    pool->residentBytes -= target->bytes;
    group->targetCount--;

    target->bytes = 0;
    target->group = -1;
    target->state = RTPOOL_TARGET_EMPTY;
    target->nextFree = -1;
    pool->stats.evictions++;
}

static void RTPoolEvictFreeTo(RTPool *pool, size_t desiredBytes)
{
    while (pool->residentBytes > desiredBytes)
    {
        int targetIndex = RTPoolFindLruFree(pool);
        if (targetIndex < 0) break; // Live/retiring targets cannot be evicted

        RTPoolEvict(pool, targetIndex);
    }
}

static int RTPoolFindEmptySlot(RTPool *pool)
{
    for (int i = 0; i < pool->targetCount; i++)
    {
        if (pool->targets[i].state == RTPOOL_TARGET_EMPTY) return i;
    }

    return -1;
}

static void RTPoolRetireTargets(RTPool *pool)
{
    for (int i = 0; i < pool->targetCount; i++)
    {
        RTPoolTarget *target = &pool->targets[i];
        if (target->state != RTPOOL_TARGET_RETIRING) continue;

        if ((pool->frame - target->retireFrame) >= (unsigned long long)pool->reclaimDelayFrames)
        {
            RTPoolGroup *group = &pool->groups[target->group];
            target->state = RTPOOL_TARGET_FREE;
            target->nextFree = group->firstFree;
            group->firstFree = i;
        }
    }
}

static void RTPoolTick(RTPool *pool)
{
    pool->frame++;
    RTPoolRetireTargets(pool);

    if (pool->budgetBytes > 0) RTPoolEvictFreeTo(pool, pool->budgetBytes);
}

static bool RTPoolAcquire(RTPool *pool, int width, int height, int format, RTPoolLease *lease)
{
    if ((lease == NULL) || (pool->activeTarget >= 0) || !RTPoolValidateRequest(width, height, format)) return false;

    int groupIndex = RTPoolGetGroup(pool, width, height, format);
    if (groupIndex < 0) return false;

    RTPoolGroup *group = &pool->groups[groupIndex];
    int targetIndex = group->firstFree;
    bool reused = false;

    if (targetIndex >= 0)
    {
        group->firstFree = pool->targets[targetIndex].nextFree;
        pool->targets[targetIndex].nextFree = -1;
        reused = true;
    }
    else
    {
        size_t bytes = RTPoolEstimateBytes(width, height, format);
        if (RTPoolExceeds(pool->residentBytes, bytes, pool->budgetBytes))
        {
            size_t desired = (bytes < pool->budgetBytes)? (pool->budgetBytes - bytes) : 0;
            RTPoolEvictFreeTo(pool, desired);
        }

        if (pool->hardLimitBytes > 0)
        {
            while (RTPoolExceeds(pool->residentBytes, bytes, pool->hardLimitBytes) && (RTPoolFindLruFree(pool) >= 0))
            {
                RTPoolEvict(pool, RTPoolFindLruFree(pool));
            }

            if (RTPoolExceeds(pool->residentBytes, bytes, pool->hardLimitBytes))
            {
                pool->stats.rejects++;
                return false;
            }
        }

        if (RTPoolExceeds(pool->residentBytes, bytes, pool->budgetBytes))
        {
            pool->stats.overshoots++;
        }

        targetIndex = RTPoolFindEmptySlot(pool);
        if (targetIndex < 0)
        {
            if ((pool->targetCount < pool->maxTargets) && RTPoolGrowTargets(pool, pool->targetCount + 1))
            {
                targetIndex = pool->targetCount;
                pool->targetCount++;
            }
            else
            {
                while ((targetIndex < 0) && (RTPoolFindLruFree(pool) >= 0))
                {
                    RTPoolEvict(pool, RTPoolFindLruFree(pool));
                    targetIndex = RTPoolFindEmptySlot(pool);
                }
            }

            if (targetIndex < 0)
            {
                pool->stats.rejects++;
                return false;
            }
        }

        RenderTexture2D target = LoadRenderTextureEx(width, height, format);
        if (!IsRenderTextureValid(target) || !rlFramebufferComplete(target.id))
        {
            UnloadRenderTexture(target);
            pool->stats.rejects++;
            return false;
        }

        RTPoolTarget *entry = &pool->targets[targetIndex];
        memset(entry, 0, sizeof(RTPoolTarget));
        entry->target = target;
        entry->bytes = bytes;
        entry->group = groupIndex;
        entry->nextFree = -1;
        entry->state = RTPOOL_TARGET_LIVE;
        group->targetCount++;

        pool->residentBytes += bytes;
        if (pool->residentBytes > pool->peakBytes) pool->peakBytes = pool->residentBytes;
        pool->stats.allocations++;
    }

    RTPoolTarget *entry = &pool->targets[targetIndex];
    entry->state = RTPOOL_TARGET_LIVE;
    entry->lastUseFrame = pool->frame;
    entry->generation++;

    lease->target = entry->target;
    lease->index = targetIndex;
    lease->generation = entry->generation;

    pool->stats.acquires++;
    if (reused) pool->stats.reuses++;

    return true;
}

static bool RTPoolLeaseValid(RTPool *pool, RTPoolLease lease)
{
    if ((lease.index < 0) || (lease.index >= pool->targetCount)) return false;

    RTPoolTarget *entry = &pool->targets[lease.index];
    return (entry->state == RTPOOL_TARGET_LIVE) &&
           (entry->generation == lease.generation) &&
           (entry->target.id == lease.target.id);
}

static bool RTPoolBegin(RTPool *pool, RTPoolLease lease, Color clearColor)
{
    if (!RTPoolLeaseValid(pool, lease) || (pool->activeTarget >= 0)) return false;

    BeginTextureMode(lease.target);
        RTPoolResetRenderState();
        ClearBackground(clearColor);
    pool->activeTarget = lease.index;

    return true;
}

static void RTPoolEnd(RTPool *pool)
{
    if (pool->activeTarget < 0) return;

    EndTextureMode();
    RTPoolResetRenderState();
    pool->activeTarget = -1;
}

static bool RTPoolRelease(RTPool *pool, RTPoolLease *lease)
{
    if ((lease == NULL) || !RTPoolLeaseValid(pool, *lease)) return false;
    if (pool->activeTarget == lease->index) RTPoolEnd(pool);

    RTPoolTarget *entry = &pool->targets[lease->index];

    if (pool->clearOnRelease) RTPoolClearTarget(entry->target, BLACK);

    entry->state = RTPOOL_TARGET_RETIRING;
    entry->retireFrame = pool->frame;
    entry->lastUseFrame = pool->frame;
    entry->generation++;

    lease->index = -1;
    lease->generation = 0;
    memset(&lease->target, 0, sizeof(RenderTexture2D));
    pool->stats.releases++;

    return true;
}

//----------------------------------------------------------------------------------
// Tests and benchmark
//----------------------------------------------------------------------------------
static int testFailures = 0;

#define CHECK(condition, message) \
    do { \
        if (condition) printf("  [PASS] %s\n", message); \
        else { printf("  [FAIL] %s\n", message); testFailures++; } \
    } while (0)

static int RunPoolTests(void)
{
    printf("\n== render target pool tests ==\n");
    testFailures = 0;

    RTPool pool = { 0 };
    RTPoolLease lease = { 0 };

    CHECK(RTPoolInit(&pool, 8, 64*1024*1024, 96*1024*1024, 2), "pool initializes");
    CHECK(!RTPoolAcquire(&pool, 0, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "zero width rejected");
    CHECK(!RTPoolAcquire(&pool, 320, -1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "negative height rejected");
    CHECK(!RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_COMPRESSED_DXT1_RGB, &lease), "compressed format rejected");

    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "first target acquired");
    unsigned long long allocations = pool.stats.allocations;
    RTPoolLease staleLease = lease;
    CHECK(RTPoolRelease(&pool, &lease), "first target released");
    CHECK(!RTPoolRelease(&pool, &staleLease), "double release rejected");

    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "same key acquired while old target retires");
    CHECK(pool.stats.allocations == allocations + 1, "retiring target is not reused immediately");
    CHECK(RTPoolRelease(&pool, &lease), "second target released");

    RTPoolTick(&pool);
    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "acquire before reclamation delay");
    CHECK(pool.stats.allocations == allocations + 2, "delay still blocks reuse after one frame");
    CHECK(RTPoolRelease(&pool, &lease), "third target released");

    RTPoolTick(&pool);
    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "acquire after reclamation delay");
    CHECK(pool.stats.allocations == allocations + 2, "completed delay enables reuse");
    CHECK(RTPoolRelease(&pool, &lease), "reused target released");

    allocations = pool.stats.allocations;
    for (int i = 0; i < 3; i++) RTPoolTick(&pool);
    bool differentFormatAcquired = RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R5G6B5, &lease);
    CHECK(!differentFormatAcquired || (pool.stats.allocations == allocations + 1), "format is part of the reuse key");
    if (differentFormatAcquired) CHECK(RTPoolRelease(&pool, &lease), "different format released");
    else CHECK(pool.stats.rejects > 0, "unsupported render format rejected");

    RTPoolShutdown(&pool);

    // Dirty all render state that BeginTextureMode() does not restore.
    BeginScissorMode(0, 0, 2, 2);
    BeginBlendMode(BLEND_ADDITIVE);
    rlDisableDepthMask();
    rlColorMask(false, false, false, false);

    CHECK(RTPoolInit(&pool, 2, 4*1024*1024, 8*1024*1024, 2), "state test pool initializes");
    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "target acquired for state test");
    int cleanIndex = lease.index;
    CHECK(RTPoolRelease(&pool, &lease), "dirty target released");
    for (int i = 0; i < 3; i++) RTPoolTick(&pool);
    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "dirty target reacquired");
    CHECK(lease.index == cleanIndex, "same target was reused");

    CHECK(RTPoolBegin(&pool, lease, RED), "pool begin");
        rlDrawRenderBatchActive();
        unsigned char *pixels = rlReadScreenPixels(lease.target.texture.width, lease.target.texture.height);
        Color clearCorner = { pixels[0], pixels[1], pixels[2], pixels[3] };
        if (!ColorMatches(clearCorner, RED))
        {
            printf("  [INFO] cleared corner is (%u, %u, %u, %u)\n",
                   clearCorner.r, clearCorner.g, clearCorner.b, clearCorner.a);
        }
        CHECK(ColorMatches(clearCorner, RED), "begin clears stale target pixels");
        MemFree(pixels);

        DrawRectangle(0, 0, lease.target.texture.width, lease.target.texture.height, BLUE);
        rlDrawRenderBatchActive();
        pixels = rlReadScreenPixels(lease.target.texture.width, lease.target.texture.height);
    RTPoolEnd(&pool);

    Color corner = { pixels[0], pixels[1], pixels[2], pixels[3] };
    if (!ColorMatches(corner, BLUE))
    {
        printf("  [INFO] corner pixel is (%u, %u, %u, %u)\n", corner.r, corner.g, corner.b, corner.a);
    }
    CHECK(ColorMatches(corner, BLUE), "state reset removed scissor, color mask and blend leaks");
    MemFree(pixels);
    CHECK(RTPoolRelease(&pool, &lease), "state test target released");

    CHECK(RTPoolAcquire(&pool, 320, 180, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "release-while-active target acquired");
    CHECK(RTPoolBegin(&pool, lease, BLACK), "release-while-active begin");
    CHECK(RTPoolRelease(&pool, &lease), "release closes an active target");
    CHECK((pool.activeTarget == -1) && (rlGetActiveFramebuffer() == 0), "release restores the default framebuffer");

    RTPoolShutdown(&pool);

    RTPool lru = { 0 };
    RTPoolLease leaseA = { 0 };
    RTPoolLease leaseB = { 0 };
    CHECK(RTPoolInit(&lru, 4, 4*1024*1024, 8*1024*1024, 0), "LRU pool initializes");
    CHECK(RTPoolAcquire(&lru, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &leaseA), "LRU target A");
    int indexA = leaseA.index;
    CHECK(RTPoolAcquire(&lru, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &leaseB), "LRU target B");
    int indexB = leaseB.index;
    CHECK(RTPoolRelease(&lru, &leaseA), "LRU target A released");
    RTPoolTick(&lru);
    CHECK(RTPoolRelease(&lru, &leaseB), "LRU target B released");
    RTPoolTick(&lru);
    CHECK(RTPoolAcquire(&lru, 320, 240, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "LRU target C forces eviction");
    CHECK((lease.index == indexA) && (lru.targets[indexB].state == RTPOOL_TARGET_FREE), "LRU evicts the oldest free target");
    CHECK(RTPoolRelease(&lru, &lease), "LRU target C released");
    RTPoolShutdown(&lru);

    RTPool small = { 0 };
    CHECK(RTPoolInit(&small, 4, 4*1024*1024, 4*1024*1024, 0), "small pool initializes");
    CHECK(RTPoolAcquire(&small, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "small pool target A");
    CHECK(RTPoolAcquire(&small, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "small pool target B");
    CHECK(!RTPoolAcquire(&small, 640, 360, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease), "hard limit rejects beyond live set");
    CHECK(small.stats.rejects > 0, "reject is accounted");
    RTPoolShutdown(&small);

    printf("== %i test(s) failed ==\n", testFailures);
    return testFailures;
}

static bool ColorMatches(Color actual, Color expected)
{
    // The software backend returns BGRA from rlReadScreenPixels(); real GPU drivers return RGBA.
    bool same = (actual.r == expected.r) && (actual.g == expected.g) && (actual.b == expected.b) && (actual.a == expected.a);
    bool swapped = (actual.r == expected.b) && (actual.g == expected.g) && (actual.b == expected.r) && (actual.a == expected.a);
    return same || swapped;
}

typedef struct BenchmarkResult {
    double switchMs;
    double drawMs;
    double totalMs;
    double warmSwitchMs;
    double warmDrawMs;
    double warmTotalMs;
    size_t peakBytes;
    size_t residentBytes;
    unsigned long long allocations;
    unsigned long long reuses;
    unsigned long long evictions;
    int samples;
} BenchmarkResult;

static const ResolutionCase benchmarkCases[] = {
    { "640x360", 640, 360 },
    { "1280x720", 1280, 720 },
    { "1920x1080", 1920, 1080 },
    { "960x540", 960, 540 },
    { "800x450", 800, 450 },
    { "1600x900", 1600, 900 }
};

static void BenchmarkWorkloadContents(RenderTexture2D target)
{
    Color colors[] = { RED, ORANGE, GOLD, LIME, SKYBLUE, BLUE, VIOLET };

    for (int i = 0; i < 48; i++)
    {
        int x = (i*97)%target.texture.width;
        int y = (i*53)%target.texture.height;
        DrawRectangle(x, y, target.texture.width/12, target.texture.height/18, colors[i%7]);
    }
    DrawCircle(target.texture.width/2, target.texture.height/2, (float)((target.texture.width < target.texture.height)? target.texture.width : target.texture.height)/8.0f, Fade(WHITE, 0.5f));
}

static void BenchmarkWorkload(RenderTexture2D target)
{
    BeginTextureMode(target);
        ClearBackground((Color){ 18, 20, 28, 255 });
        BenchmarkWorkloadContents(target);
    EndTextureMode();
}

static BenchmarkResult BenchmarkNaive(int rounds)
{
    BenchmarkResult result = { 0 };
    const int caseCount = (int)(sizeof(benchmarkCases)/sizeof(benchmarkCases[0]));
    double switchSeconds = 0.0;
    double drawSeconds = 0.0;

    for (int round = 0; round < rounds; round++)
    {
        for (int i = 0; i < caseCount; i++)
        {
            double start = GetTime();
            RenderTexture2D target = LoadRenderTextureEx(benchmarkCases[i].width, benchmarkCases[i].height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            double allocated = GetTime();

            BenchmarkWorkload(target);
            double drawn = GetTime();

            UnloadRenderTexture(target);
            double released = GetTime();

            switchSeconds += (allocated - start) + (released - drawn);
            drawSeconds += drawn - allocated;

            size_t bytes = RTPoolEstimateBytes(benchmarkCases[i].width, benchmarkCases[i].height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            if (bytes > result.peakBytes) result.peakBytes = bytes;
            result.allocations++;
            result.samples++;
        }
    }

    result.switchMs = (switchSeconds*1000.0)/result.samples;
    result.drawMs = (drawSeconds*1000.0)/result.samples;
    result.totalMs = result.switchMs + result.drawMs;
    result.warmSwitchMs = result.switchMs;
    result.warmDrawMs = result.drawMs;
    result.warmTotalMs = result.totalMs;
    result.residentBytes = result.peakBytes;

    return result;
}

static BenchmarkResult BenchmarkPool(size_t budgetBytes, int rounds)
{
    BenchmarkResult result = { 0 };
    RTPool pool = { 0 };
    RTPoolLease lease = { 0 };
    const int caseCount = (int)(sizeof(benchmarkCases)/sizeof(benchmarkCases[0]));
    double coldSwitchSeconds = 0.0;
    double coldDrawSeconds = 0.0;
    int coldSamples = 0;
    double warmSwitchSeconds = 0.0;
    double warmDrawSeconds = 0.0;
    int warmSamples = 0;

    RTPoolInit(&pool, 16, budgetBytes, budgetBytes*2, 2);

    for (int round = 0; round < rounds; round++)
    {
        for (int i = 0; i < caseCount; i++)
        {
            RTPoolTick(&pool);

            double start = GetTime();
            if (!RTPoolAcquire(&pool, benchmarkCases[i].width, benchmarkCases[i].height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &lease))
            {
                printf("  [WARN] pool acquire failed for %s\n", benchmarkCases[i].name);
                continue;
            }
            double acquired = GetTime();

            RTPoolBegin(&pool, lease, BLACK);
                BenchmarkWorkloadContents(lease.target);
            RTPoolEnd(&pool);
            double drawn = GetTime();

            RTPoolRelease(&pool, &lease);
            double released = GetTime();

            double switchTime = (acquired - start) + (released - drawn);
            double drawTime = drawn - acquired;
            result.samples++;

            if (round == 0)
            {
                coldSwitchSeconds += switchTime;
                coldDrawSeconds += drawTime;
                coldSamples++;
            }
            else
            {
                warmSwitchSeconds += switchTime;
                warmDrawSeconds += drawTime;
                warmSamples++;
            }
        }
    }

    result.switchMs = (coldSamples > 0)? (coldSwitchSeconds*1000.0)/coldSamples : 0.0;
    result.drawMs = (coldSamples > 0)? (coldDrawSeconds*1000.0)/coldSamples : 0.0;
    result.totalMs = result.switchMs + result.drawMs;
    if (warmSamples > 0)
    {
        result.warmSwitchMs = (warmSwitchSeconds*1000.0)/warmSamples;
        result.warmDrawMs = (warmDrawSeconds*1000.0)/warmSamples;
        result.warmTotalMs = result.warmSwitchMs + result.warmDrawMs;
    }
    result.peakBytes = pool.peakBytes;
    result.residentBytes = pool.residentBytes;
    result.allocations = pool.stats.allocations;
    result.reuses = pool.stats.reuses;
    result.evictions = pool.stats.evictions;

    RTPoolShutdown(&pool);
    return result;
}

static void RunBenchmark(void)
{
    const int rounds = 6;
    const int caseCount = (int)(sizeof(benchmarkCases)/sizeof(benchmarkCases[0]));
    const size_t budget24 = 24*1024*1024;
    const size_t budget64 = 64*1024*1024;

    printf("\n== render target pool benchmark ==\n");
    printf("samples: %i switches per row, values are ms per switch\n", rounds*caseCount);

    // Warm up driver paths before timing.
    BenchmarkNaive(1);
    BenchmarkPool(budget64, 1);

    BenchmarkResult naive = BenchmarkNaive(rounds);
    BenchmarkResult pooled24 = BenchmarkPool(budget24, rounds);
    BenchmarkResult pooled64 = BenchmarkPool(budget64, rounds);

    printf("\n%-18s %9s %10s %9s %9s %9s %8s\n",
           "strategy", "cold ms", "warm us", "draw ms", "total ms", "peakMiB", "evict");
    printf("%-18s %9.3f %10.3f %9.3f %9.3f %9.1f %8llu\n",
           "naive create/free", naive.switchMs, naive.warmSwitchMs*1000.0, naive.warmDrawMs, naive.warmTotalMs,
           (double)naive.peakBytes/(1024.0*1024.0), 0ULL);
    printf("%-18s %9.3f %10.3f %9.3f %9.3f %9.1f %8llu\n",
           "pool 24 MiB", pooled24.switchMs, pooled24.warmSwitchMs*1000.0, pooled24.warmDrawMs, pooled24.warmTotalMs,
           (double)pooled24.peakBytes/(1024.0*1024.0), pooled24.evictions);
    printf("%-18s %9.3f %10.3f %9.3f %9.3f %9.1f %8llu\n",
           "pool 64 MiB", pooled64.switchMs, pooled64.warmSwitchMs*1000.0, pooled64.warmDrawMs, pooled64.warmTotalMs,
           (double)pooled64.peakBytes/(1024.0*1024.0), pooled64.evictions);

    printf("\nalloc/reuse detail:\n");
    printf("  naive: %llu allocations, %i samples\n", naive.allocations, naive.samples);
    printf("  pool 24 MiB: %llu allocations, %llu reuses, %llu evictions, %i samples\n",
           pooled24.allocations, pooled24.reuses, pooled24.evictions, pooled24.samples);
    printf("  pool 64 MiB: %llu allocations, %llu reuses, %llu evictions, %i samples\n",
           pooled64.allocations, pooled64.reuses, pooled64.evictions, pooled64.samples);
    printf("  estimated bytes include color + 32-bit depth + %i KiB FBO overhead\n", RTPOOL_ESTIMATED_OVERHEAD/1024);
}

//----------------------------------------------------------------------------------
// Interactive example helpers
//----------------------------------------------------------------------------------
static void DrawPoolScene(RenderTexture2D target, float time)
{
    int width = target.texture.width;
    int height = target.texture.height;

    ClearBackground((Color){ 20, 24, 34, 255 });

    for (int i = 0; i < 24; i++)
    {
        int x = (int)((float)(i*width)/24.0f);
        Color color = (i%2 == 0)? (Color){ 48, 56, 74, 255 } : (Color){ 38, 44, 60, 255 };
        DrawRectangle(x, 0, width/24 + 1, height, color);
    }

    float pulse = 0.5f + 0.5f*sinf(time*2.0f);
    DrawCircle(width/2, height/2, (float)((width < height)? width : height)*(0.14f + pulse*0.03f), Fade(GOLD, 0.9f));
    DrawCircleLines(width/2, height/2, (float)((width < height)? width : height)*(0.22f + pulse*0.04f), LIME);

    DrawRectangle(width/5, height/3, width/3, height/5, Fade(RED, 0.7f));
    DrawRectangle(width/2, height/2, width/3, height/5, Fade(SKYBLUE, 0.7f));

    DrawText("pooled RenderTexture2D", 20, 20, height/20, WHITE);
    DrawText(TextFormat("%ix%i", width, height), 20, 20 + height/20 + 8, height/28, LIGHTGRAY);
}

static void DrawPoolStats(RTPool *pool, int x, int y)
{
    DrawText(TextFormat("resident %.1f MiB / budget %.1f MiB | peak %.1f MiB",
             (double)pool->residentBytes/(1024.0*1024.0),
             (double)pool->budgetBytes/(1024.0*1024.0),
             (double)pool->peakBytes/(1024.0*1024.0)), x, y, 16, GOLD);
    DrawText(TextFormat("acquire %llu | reuse %llu | alloc %llu | evict %llu | reject %llu",
             pool->stats.acquires, pool->stats.reuses, pool->stats.allocations,
             pool->stats.evictions, pool->stats.rejects), x, y + 20, 16, LIGHTGRAY);
}
