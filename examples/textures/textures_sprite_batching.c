/*******************************************************************************************
*
*   raylib [textures] example - sprite batching
*
*   Example complexity rating: [★★★★] 4/4
*
*   Example demonstrates state-sorted sprite rendering with persistent CPU buffers.
*   Sprites are sorted by blend mode and texture, while an overlap-aware layer pass
*   preserves painter's order for translucent sprites. Run with "--test" for a
*   frame-by-frame pixel comparison or "--benchmark" for 1K/5K/10K sprite timings.
*
*   Example licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software.
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
#define SPRITE_TEXTURE_COUNT        6
#define SPRITE_BLEND_COUNT          3
#define SPRITE_PIXEL_WIDTH          960
#define SPRITE_PIXEL_HEIGHT         540
#define SPRITE_GRID_CELL_SIZE       32
#define SPRITE_GRID_BUCKETS         2048
#define SPRITE_COMPARE_FRAMES       8
#define SPRITE_BENCH_WARMUP         5
#define SPRITE_BENCH_FRAMES         20
#define SPRITE_BENCH_SAMPLES        3

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef enum SpriteRenderPath {
    SPRITE_RENDER_LEGACY = 0,
    SPRITE_RENDER_BATCHED
} SpriteRenderPath;

typedef struct SpriteItem {
    Texture2D texture;
    BlendMode blend;
    Rectangle source;
    Rectangle destination;
    Vector2 origin;
    float rotation;
    Color tint;
    Rectangle bounds;
    int layer;
} SpriteItem;

typedef struct SpriteSortEntry {
    int layer;
    int blend;
    unsigned int textureId;
    int sourceIndex;
} SpriteSortEntry;

typedef struct SpriteGridNode {
    int itemIndex;
    int nextNode;
} SpriteGridNode;

typedef struct SpriteBatch {
    SpriteItem *items;
    int itemCount;
    int itemCapacity;

    SpriteSortEntry *sorted;

    SpriteGridNode *gridNodes;
    int gridNodeCount;
    int gridNodeCapacity;
    int gridBucketCount;
    int *gridHeads;
    int *layerStamp;
    int layerStampCounter;
    int layerCount;
} SpriteBatch;

typedef struct SpriteDrawStats {
    int stateChanges;
    int blendChanges;
    int textureBinds;
    int quadCount;
} SpriteDrawStats;

typedef struct ImageDiff {
    unsigned long long legacyHash;
    unsigned long long batchedHash;
    int mismatchedPixels;
    int maxChannelDelta;
} ImageDiff;

//----------------------------------------------------------------------------------
// Module Functions Declaration
//----------------------------------------------------------------------------------
static unsigned int SpriteHash(unsigned int value);
static Image CreateSpriteImage(int index);
static void GenerateSpriteTextures(Texture2D textures[SPRITE_TEXTURE_COUNT]);
static void UnloadSpriteTextures(Texture2D textures[SPRITE_TEXTURE_COUNT]);

static bool SpriteBatchInit(SpriteBatch *batch, int capacity, int bucketCount);
static void SpriteBatchUnload(SpriteBatch *batch);
static void SpriteBatchReset(SpriteBatch *batch);
static void SpriteBatchAdd(SpriteBatch *batch, const SpriteItem *item);
static void SpriteBatchPrepare(SpriteBatch *batch);
static void SpriteBatchSetCount(SpriteBatch *batch, int count, Texture2D textures[SPRITE_TEXTURE_COUNT]);

static Rectangle SpriteGetBounds(const SpriteItem *item);
static bool SpriteRectsOverlap(Rectangle a, Rectangle b);
static int SpriteGridHash(int cellX, int cellY, int bucketCount);
static int SpriteBatchQueryLayer(SpriteBatch *batch, const SpriteItem *item, int stamp);
static void SpriteBatchInsertGrid(SpriteBatch *batch, int itemIndex);

static int CompareSpriteSortEntries(const void *a, const void *b);
static void DrawSpriteQuad(const SpriteItem *item);
static void RenderLegacy(SpriteBatch *batch, SpriteDrawStats *stats);
static void RenderBatched(SpriteBatch *batch, SpriteDrawStats *stats);
static void RenderSpriteScene(SpriteBatch *batch, SpriteRenderPath path, SpriteDrawStats *stats);

static void RenderToTexture(SpriteBatch *batch, SpriteRenderPath path, RenderTexture2D target);
static unsigned long long HashImage(const Image *image);
static ImageDiff CompareImages(const Image *legacy, const Image *batched);
static Image CreateDiffImage(const Image *legacy, const Image *batched);
static int RunPixelTests(const char *artifactDir);
static int RunOverlapTest(const char *artifactDir);
static double BenchmarkPath(SpriteBatch *batch, SpriteRenderPath path, int frames);
static void RunBenchmark(void);

//----------------------------------------------------------------------------------
// Program main entry point
//----------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    bool runTests = false;
    bool runBenchmark = false;
    const char *artifactDir = "artifacts";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--test") == 0) runTests = true;
        else if (strcmp(argv[i], "--benchmark") == 0) runBenchmark = true;
        else if (strcmp(argv[i], "--all") == 0) runTests = runBenchmark = true;
        else if ((strcmp(argv[i], "--artifacts") == 0) && ((i + 1) < argc)) artifactDir = argv[++i];
    }

    SetTraceLogLevel(LOG_WARNING);
    if (runTests || runBenchmark) SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(SPRITE_PIXEL_WIDTH, SPRITE_PIXEL_HEIGHT, "raylib [textures] example - sprite batching");

    bool gpuReady = IsWindowReady();
    int failures = 0;

    if (gpuReady && runTests) failures += RunPixelTests(artifactDir);
    if (gpuReady && runBenchmark) RunBenchmark();

    if (runTests || runBenchmark)
    {
        CloseWindow();
        return ((gpuReady && (failures == 0))? 0 : 1);
    }

    Texture2D textures[SPRITE_TEXTURE_COUNT] = { 0 };
    GenerateSpriteTextures(textures);

    SpriteBatch batch = { 0 };
    SpriteBatchInit(&batch, 10000, SPRITE_GRID_BUCKETS);

    int spriteCounts[] = { 1000, 5000, 10000 };
    int countIndex = 2;
    int frame = 0;
    bool batched = true;

    SpriteBatchSetCount(&batch, spriteCounts[countIndex], textures);

    while (!WindowShouldClose())
    {
        if (IsKeyPressed(KEY_ONE)) { countIndex = 0; SpriteBatchSetCount(&batch, spriteCounts[countIndex], textures); }
        else if (IsKeyPressed(KEY_TWO)) { countIndex = 1; SpriteBatchSetCount(&batch, spriteCounts[countIndex], textures); }
        else if (IsKeyPressed(KEY_THREE)) { countIndex = 2; SpriteBatchSetCount(&batch, spriteCounts[countIndex], textures); }
        else if (IsKeyPressed(KEY_B)) batched = !batched;

        frame++;
        for (int i = 0; i < batch.itemCount; i++)
        {
            batch.items[i].tint.a = (unsigned char)(180 + ((frame + i*13)%76));
        }

        BeginDrawing();
            ClearBackground((Color){ 12, 14, 18, 255 });

            SpriteDrawStats stats = { 0 };
            RenderSpriteScene(&batch, batched? SPRITE_RENDER_BATCHED : SPRITE_RENDER_LEGACY, &stats);

            DrawRectangle(0, 0, GetScreenWidth(), 82, Fade(BLACK, 0.76f));
            DrawText("SPRITE BATCHING", 12, 8, 20, RAYWHITE);
            DrawText(TextFormat("%i sprites | path: %s | layers: %i",
                     batch.itemCount, batched? "sorted batch" : "legacy", batch.layerCount), 12, 32, 16,
                     batched? LIME : ORANGE);
            DrawText(TextFormat("state changes: %i | blend changes: %i | texture binds: %i",
                     stats.stateChanges, stats.blendChanges, stats.textureBinds), 12, 52, 16, LIGHTGRAY);
            DrawText("1/2/3: sprite count | B: toggle path", 12, GetScreenHeight() - 28, 16, GRAY);
        EndDrawing();
    }

    SpriteBatchUnload(&batch);
    UnloadSpriteTextures(textures);
    CloseWindow();

    return 0;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Scene setup
//----------------------------------------------------------------------------------
static unsigned int SpriteHash(unsigned int value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static Image CreateSpriteImage(int index)
{
    const int size = 16;
    Image image = GenImageColor(size, size, BLANK);

    Color colors[SPRITE_TEXTURE_COUNT] = {
        (Color){ 255, 110, 90, 255 },
        (Color){ 90, 210, 255, 255 },
        (Color){ 255, 220, 80, 255 },
        (Color){ 150, 120, 255, 255 },
        (Color){ 90, 235, 150, 255 },
        (Color){ 255, 150, 220, 255 }
    };
    Color base = colors[index%SPRITE_TEXTURE_COUNT];

    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float dx = (float)x - 7.5f;
            float dy = (float)y - 7.5f;
            float radius = sqrtf(dx*dx + dy*dy);
            bool inside = false;

            switch (index%SPRITE_TEXTURE_COUNT)
            {
                case 0: inside = radius <= 7.0f; break;
                case 1: inside = (abs(x - 7) + abs(y - 7)) <= 8; break;
                case 2: inside = ((x >= 3) && (x < 13) && (y >= 3) && (y < 13)); break;
                case 3: inside = ((x + y) >= 8) && ((x + y) <= 22); break;
                case 4: inside = (radius <= 7.5f) && ((x + y)%4 != 0); break;
                default: inside = ((x >= 4) && (x < 12)) || ((y >= 4) && (y < 12)); break;
            }

            if (inside)
            {
                unsigned char shade = (unsigned char)(210 + ((x*3 + y*5)%46));
                Color pixel = {
                    (unsigned char)((base.r*shade)/255),
                    (unsigned char)((base.g*shade)/255),
                    (unsigned char)((base.b*shade)/255),
                    (unsigned char)(235 + ((x + y)%21))
                };
                ImageDrawPixel(&image, x, y, pixel);
            }
        }
    }

    return image;
}

static void GenerateSpriteTextures(Texture2D textures[SPRITE_TEXTURE_COUNT])
{
    for (int i = 0; i < SPRITE_TEXTURE_COUNT; i++)
    {
        Image image = CreateSpriteImage(i);
        textures[i] = LoadTextureFromImage(image);
        SetTextureFilter(textures[i], TEXTURE_FILTER_POINT);
        UnloadImage(image);
    }
}

static void UnloadSpriteTextures(Texture2D textures[SPRITE_TEXTURE_COUNT])
{
    for (int i = 0; i < SPRITE_TEXTURE_COUNT; i++) UnloadTexture(textures[i]);
}

static void SpriteBatchSetCount(SpriteBatch *batch, int count, Texture2D textures[SPRITE_TEXTURE_COUNT])
{
    SpriteBatchReset(batch);

    int columns = (int)ceilf(sqrtf((float)count*(float)SPRITE_PIXEL_WIDTH/(float)SPRITE_PIXEL_HEIGHT));
    if (columns < 1) columns = 1;
    int rows = (count + columns - 1)/columns;
    if (rows < 1) rows = 1;

    float cellWidth = (float)SPRITE_PIXEL_WIDTH/(float)columns;
    float cellHeight = (float)SPRITE_PIXEL_HEIGHT/(float)rows;
    int spriteWidth = (int)fminf(7.0f, fmaxf(3.0f, cellWidth - 2.0f));
    int spriteHeight = (int)fminf(7.0f, fmaxf(3.0f, cellHeight - 2.0f));

    for (int i = 0; i < count; i++)
    {
        int column = i%columns;
        int row = i/columns;
        unsigned int hash = SpriteHash((unsigned int)i + 0x9e3779b9u);

        SpriteItem item = { 0 };
        item.texture = textures[hash%SPRITE_TEXTURE_COUNT];
        item.blend = (BlendMode)((hash >> 8)%(unsigned int)SPRITE_BLEND_COUNT);
        item.source = (Rectangle){ 0.0f, 0.0f, (float)item.texture.width, (float)item.texture.height };
        item.destination = (Rectangle){
            floorf((float)column*cellWidth) + 1.0f,
            floorf((float)row*cellHeight) + 1.0f,
            (float)spriteWidth,
            (float)spriteHeight
        };
        item.origin = (Vector2){ 0.0f, 0.0f };
        item.rotation = 0.0f;
        item.tint = (Color){
            (unsigned char)(180 + (hash%76)),
            (unsigned char)(180 + ((hash >> 8)%76)),
            (unsigned char)(180 + ((hash >> 16)%76)),
            (unsigned char)(180 + ((hash >> 24)%76))
        };
        SpriteBatchAdd(batch, &item);
    }
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Sprite batch
//----------------------------------------------------------------------------------
static bool SpriteBatchInit(SpriteBatch *batch, int capacity, int bucketCount)
{
    memset(batch, 0, sizeof(SpriteBatch));
    batch->itemCapacity = capacity;
    batch->items = (SpriteItem *)RL_CALLOC((size_t)capacity, sizeof(SpriteItem));
    batch->sorted = (SpriteSortEntry *)RL_CALLOC((size_t)capacity, sizeof(SpriteSortEntry));
    batch->gridNodeCapacity = capacity*8;
    batch->gridNodes = (SpriteGridNode *)RL_MALLOC((size_t)batch->gridNodeCapacity*sizeof(SpriteGridNode));
    batch->gridHeads = (int *)RL_MALLOC((size_t)bucketCount*sizeof(int));
    batch->layerStamp = (int *)RL_CALLOC((size_t)capacity, sizeof(int));
    batch->gridBucketCount = bucketCount;
    batch->layerStampCounter = 0;

    if ((batch->items == NULL) || (batch->sorted == NULL) || (batch->gridNodes == NULL) ||
        (batch->gridHeads == NULL) || (batch->layerStamp == NULL))
    {
        SpriteBatchUnload(batch);
        return false;
    }

    return true;
}

static void SpriteBatchUnload(SpriteBatch *batch)
{
    RL_FREE(batch->items);
    RL_FREE(batch->sorted);
    RL_FREE(batch->gridNodes);
    RL_FREE(batch->gridHeads);
    RL_FREE(batch->layerStamp);
    memset(batch, 0, sizeof(SpriteBatch));
}

static void SpriteBatchReset(SpriteBatch *batch)
{
    batch->itemCount = 0;
    batch->layerCount = 0;
}

static void SpriteBatchAdd(SpriteBatch *batch, const SpriteItem *item)
{
    if (batch->itemCount >= batch->itemCapacity)
    {
        int newCapacity = batch->itemCapacity*2;
        batch->items = (SpriteItem *)RL_REALLOC(batch->items, (size_t)newCapacity*sizeof(SpriteItem));
        batch->sorted = (SpriteSortEntry *)RL_REALLOC(batch->sorted, (size_t)newCapacity*sizeof(SpriteSortEntry));
        batch->layerStamp = (int *)RL_REALLOC(batch->layerStamp, (size_t)newCapacity*sizeof(int));

        if ((batch->items == NULL) || (batch->sorted == NULL) || (batch->layerStamp == NULL))
        {
            batch->itemCapacity = 0;
            batch->itemCount = 0;
            return;
        }

        memset(batch->layerStamp + batch->itemCapacity, 0,
               (size_t)(newCapacity - batch->itemCapacity)*sizeof(int));
        batch->itemCapacity = newCapacity;
    }

    batch->items[batch->itemCount] = *item;
    batch->itemCount++;
}

static Rectangle SpriteGetBounds(const SpriteItem *item)
{
    float x = item->destination.x - item->origin.x;
    float y = item->destination.y - item->origin.y;
    float width = item->destination.width;
    float height = item->destination.height;

    if (item->rotation != 0.0f)
    {
        float radians = item->rotation*DEG2RAD;
        float sine = sinf(radians);
        float cosine = cosf(radians);
        float dx = -item->origin.x;
        float dy = -item->origin.y;
        Vector2 points[4] = {
            { item->destination.x + dx*cosine - dy*sine,
              item->destination.y + dx*sine + dy*cosine },
            { item->destination.x + (dx + width)*cosine - dy*sine,
              item->destination.y + (dx + width)*sine + dy*cosine },
            { item->destination.x + dx*cosine - (dy + height)*sine,
              item->destination.y + dx*sine + (dy + height)*cosine },
            { item->destination.x + (dx + width)*cosine - (dy + height)*sine,
              item->destination.y + (dx + width)*sine + (dy + height)*cosine }
        };

        x = points[0].x;
        y = points[0].y;
        float maxX = points[0].x;
        float maxY = points[0].y;

        for (int i = 1; i < 4; i++)
        {
            if (points[i].x < x) x = points[i].x;
            if (points[i].y < y) y = points[i].y;
            if (points[i].x > maxX) maxX = points[i].x;
            if (points[i].y > maxY) maxY = points[i].y;
        }

        width = maxX - x;
        height = maxY - y;
    }

    return (Rectangle){ floorf(x), floorf(y), ceilf(width) + 1.0f, ceilf(height) + 1.0f };
}

static bool SpriteRectsOverlap(Rectangle a, Rectangle b)
{
    if ((a.width <= 0.0f) || (a.height <= 0.0f) || (b.width <= 0.0f) || (b.height <= 0.0f)) return false;
    return (a.x < (b.x + b.width)) && ((a.x + a.width) > b.x) &&
           (a.y < (b.y + b.height)) && ((a.y + a.height) > b.y);
}

static int SpriteGridHash(int cellX, int cellY, int bucketCount)
{
    unsigned int hash = ((unsigned int)cellX*0x8da6b343u) ^ ((unsigned int)cellY*0xd8163841u);
    return (int)(hash%((unsigned int)bucketCount));
}

static int SpriteBatchQueryLayer(SpriteBatch *batch, const SpriteItem *item, int stamp)
{
    Rectangle bounds = SpriteGetBounds(item);
    int minCellX = (int)floorf(bounds.x/(float)SPRITE_GRID_CELL_SIZE);
    int minCellY = (int)floorf(bounds.y/(float)SPRITE_GRID_CELL_SIZE);
    int maxCellX = (int)floorf((bounds.x + bounds.width)/(float)SPRITE_GRID_CELL_SIZE);
    int maxCellY = (int)floorf((bounds.y + bounds.height)/(float)SPRITE_GRID_CELL_SIZE);
    int maxOverlapLayer = -1;

    for (int cellY = minCellY; cellY <= maxCellY; cellY++)
    {
        for (int cellX = minCellX; cellX <= maxCellX; cellX++)
        {
            int bucket = SpriteGridHash(cellX, cellY, batch->gridBucketCount);

            for (int node = batch->gridHeads[bucket]; node >= 0; node = batch->gridNodes[node].nextNode)
            {
                int itemIndex = batch->gridNodes[node].itemIndex;
                if (!SpriteRectsOverlap(bounds, batch->items[itemIndex].bounds)) continue;
                if (batch->items[itemIndex].layer > maxOverlapLayer) maxOverlapLayer = batch->items[itemIndex].layer;
                batch->layerStamp[batch->items[itemIndex].layer] = stamp;
            }
        }
    }

    // A sprite must be drawn after every earlier sprite it overlaps. Keeping
    // non-overlapping sprites in the same layer lets the state sort reorder them
    // freely without changing a single output pixel.
    int layer = maxOverlapLayer + 1;
    while ((layer < batch->itemCapacity) && (batch->layerStamp[layer] == stamp)) layer++;

    return layer;
}

static void SpriteBatchInsertGrid(SpriteBatch *batch, int itemIndex)
{
    Rectangle bounds = batch->items[itemIndex].bounds;
    int minCellX = (int)floorf(bounds.x/(float)SPRITE_GRID_CELL_SIZE);
    int minCellY = (int)floorf(bounds.y/(float)SPRITE_GRID_CELL_SIZE);
    int maxCellX = (int)floorf((bounds.x + bounds.width)/(float)SPRITE_GRID_CELL_SIZE);
    int maxCellY = (int)floorf((bounds.y + bounds.height)/(float)SPRITE_GRID_CELL_SIZE);

    for (int cellY = minCellY; cellY <= maxCellY; cellY++)
    {
        for (int cellX = minCellX; cellX <= maxCellX; cellX++)
        {
            if (batch->gridNodeCount >= batch->gridNodeCapacity)
            {
                int newCapacity = batch->gridNodeCapacity*2;
                batch->gridNodes = (SpriteGridNode *)RL_REALLOC(batch->gridNodes,
                                                               (size_t)newCapacity*sizeof(SpriteGridNode));
                if (batch->gridNodes == NULL)
                {
                    batch->gridNodeCapacity = 0;
                    return;
                }
                batch->gridNodeCapacity = newCapacity;
            }

            int bucket = SpriteGridHash(cellX, cellY, batch->gridBucketCount);
            batch->gridNodes[batch->gridNodeCount].itemIndex = itemIndex;
            batch->gridNodes[batch->gridNodeCount].nextNode = batch->gridHeads[bucket];
            batch->gridHeads[bucket] = batch->gridNodeCount;
            batch->gridNodeCount++;
        }
    }
}

static int CompareSpriteSortEntries(const void *a, const void *b)
{
    const SpriteSortEntry *left = (const SpriteSortEntry *)a;
    const SpriteSortEntry *right = (const SpriteSortEntry *)b;

    if (left->layer != right->layer) return (left->layer < right->layer)? -1 : 1;
    if (left->blend != right->blend) return (left->blend < right->blend)? -1 : 1;
    if (left->textureId != right->textureId) return (left->textureId < right->textureId)? -1 : 1;
    if (left->sourceIndex != right->sourceIndex) return (left->sourceIndex < right->sourceIndex)? -1 : 1;

    return 0;
}

static void SpriteBatchPrepare(SpriteBatch *batch)
{
    for (int i = 0; i < batch->gridBucketCount; i++) batch->gridHeads[i] = -1;
    batch->gridNodeCount = 0;

    batch->layerCount = 0;
    for (int i = 0; i < batch->itemCount; i++)
    {
        batch->layerStampCounter++;
        if (batch->layerStampCounter == INT32_MAX) batch->layerStampCounter = 1;

        batch->items[i].bounds = SpriteGetBounds(&batch->items[i]);
        int layer = SpriteBatchQueryLayer(batch, &batch->items[i], batch->layerStampCounter);
        batch->items[i].layer = layer;
        if ((layer + 1) > batch->layerCount) batch->layerCount = layer + 1;

        SpriteBatchInsertGrid(batch, i);

        batch->sorted[i].layer = layer;
        batch->sorted[i].blend = (int)batch->items[i].blend;
        batch->sorted[i].textureId = batch->items[i].texture.id;
        batch->sorted[i].sourceIndex = i;
    }

    // Layer is the primary key for painter's order. Inside one layer there is no
    // overlap, so blend and texture are safe secondary keys.
    qsort(batch->sorted, (size_t)batch->itemCount, sizeof(SpriteSortEntry), CompareSpriteSortEntries);
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Rendering
//----------------------------------------------------------------------------------
static void DrawSpriteQuad(const SpriteItem *item)
{
    Rectangle source = item->source;
    Rectangle destination = item->destination;
    bool flipX = false;

    if (source.width < 0.0f) { flipX = true; source.width *= -1.0f; }
    if (source.height < 0.0f) { source.y -= source.height; source.height *= -1.0f; }
    if (destination.width < 0.0f) destination.width *= -1.0f;
    if (destination.height < 0.0f) destination.height *= -1.0f;

    float width = (float)item->texture.width;
    float height = (float)item->texture.height;
    Vector2 topLeft = { 0 };
    Vector2 topRight = { 0 };
    Vector2 bottomLeft = { 0 };
    Vector2 bottomRight = { 0 };

    if (item->rotation == 0.0f)
    {
        float x = destination.x - item->origin.x;
        float y = destination.y - item->origin.y;
        topLeft = (Vector2){ x, y };
        topRight = (Vector2){ x + destination.width, y };
        bottomLeft = (Vector2){ x, y + destination.height };
        bottomRight = (Vector2){ x + destination.width, y + destination.height };
    }
    else
    {
        float sine = sinf(item->rotation*DEG2RAD);
        float cosine = cosf(item->rotation*DEG2RAD);
        float x = destination.x;
        float y = destination.y;
        float dx = -item->origin.x;
        float dy = -item->origin.y;

        topLeft.x = x + dx*cosine - dy*sine;
        topLeft.y = y + dx*sine + dy*cosine;
        topRight.x = x + (dx + destination.width)*cosine - dy*sine;
        topRight.y = y + (dx + destination.width)*sine + dy*cosine;
        bottomLeft.x = x + dx*cosine - (dy + destination.height)*sine;
        bottomLeft.y = y + dx*sine + (dy + destination.height)*cosine;
        bottomRight.x = x + (dx + destination.width)*cosine - (dy + destination.height)*sine;
        bottomRight.y = y + (dx + destination.width)*sine + (dy + destination.height)*cosine;
    }

    rlColor4ub(item->tint.r, item->tint.g, item->tint.b, item->tint.a);
    rlNormal3f(0.0f, 0.0f, 1.0f);

    if (flipX) rlTexCoord2f((source.x + source.width)/width, source.y/height);
    else rlTexCoord2f(source.x/width, source.y/height);
    rlVertex2f(topLeft.x, topLeft.y);

    if (flipX) rlTexCoord2f((source.x + source.width)/width, (source.y + source.height)/height);
    else rlTexCoord2f(source.x/width, (source.y + source.height)/height);
    rlVertex2f(bottomLeft.x, bottomLeft.y);

    if (flipX) rlTexCoord2f(source.x/width, (source.y + source.height)/height);
    else rlTexCoord2f((source.x + source.width)/width, (source.y + source.height)/height);
    rlVertex2f(bottomRight.x, bottomRight.y);

    if (flipX) rlTexCoord2f(source.x/width, source.y/height);
    else rlTexCoord2f((source.x + source.width)/width, source.y/height);
    rlVertex2f(topRight.x, topRight.y);
}

static void RenderLegacy(SpriteBatch *batch, SpriteDrawStats *stats)
{
    int currentBlend = -1;
    unsigned int currentTexture = 0;

    for (int i = 0; i < batch->itemCount; i++)
    {
        SpriteItem *item = &batch->items[i];
        if (currentBlend != (int)item->blend)
        {
            BeginBlendMode((int)item->blend);
            currentBlend = (int)item->blend;
            stats->blendChanges++;
            stats->stateChanges++;
        }

        if (currentTexture != item->texture.id)
        {
            currentTexture = item->texture.id;
            stats->textureBinds++;
            stats->stateChanges++;
        }

        DrawTexturePro(item->texture, item->source, item->destination, item->origin, item->rotation, item->tint);
        stats->quadCount++;
    }

    if (currentBlend >= 0) EndBlendMode();
}

static void RenderBatched(SpriteBatch *batch, SpriteDrawStats *stats)
{
    SpriteBatchPrepare(batch);

    int currentBlend = -1;
    unsigned int currentTexture = 0;
    bool quadOpen = false;

    for (int i = 0; i < batch->itemCount; i++)
    {
        SpriteItem *item = &batch->items[batch->sorted[i].sourceIndex];

        if (currentBlend != (int)item->blend)
        {
            if (quadOpen) { rlEnd(); quadOpen = false; }
            BeginBlendMode((int)item->blend);
            currentBlend = (int)item->blend;
            stats->blendChanges++;
            stats->stateChanges++;
        }

        // Keep one rlBegin/rlEnd range per contiguous texture run. raylib then
        // reuses the same VBO and emits one indexed draw for that run.
        if (!quadOpen || (currentTexture != item->texture.id))
        {
            if (quadOpen) rlEnd();
            rlSetTexture(item->texture.id);
            currentTexture = item->texture.id;
            rlBegin(RL_QUADS);
            quadOpen = true;
            stats->textureBinds++;
            stats->stateChanges++;
        }

        DrawSpriteQuad(item);
        stats->quadCount++;
    }

    if (quadOpen) rlEnd();
    rlSetTexture(0);
    if (currentBlend >= 0) EndBlendMode();
}

static void RenderSpriteScene(SpriteBatch *batch, SpriteRenderPath path, SpriteDrawStats *stats)
{
    memset(stats, 0, sizeof(SpriteDrawStats));

    if (path == SPRITE_RENDER_BATCHED) RenderBatched(batch, stats);
    else RenderLegacy(batch, stats);
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Pixel tests
//----------------------------------------------------------------------------------
static void RenderToTexture(SpriteBatch *batch, SpriteRenderPath path, RenderTexture2D target)
{
    BeginTextureMode(target);
        ClearBackground((Color){ 12, 14, 18, 255 });
        SpriteDrawStats stats = { 0 };
        RenderSpriteScene(batch, path, &stats);
    EndTextureMode();
}

static unsigned long long HashImage(const Image *image)
{
    unsigned long long hash = 1469598103934665603ULL;
    int size = GetPixelDataSize(image->width, image->height, image->format);
    const unsigned char *bytes = (const unsigned char *)image->data;

    for (int i = 0; i < size; i++)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

static ImageDiff CompareImages(const Image *legacy, const Image *batched)
{
    ImageDiff diff = { 0 };
    diff.legacyHash = HashImage(legacy);
    diff.batchedHash = HashImage(batched);

    int bytesPerPixel = GetPixelDataSize(1, 1, legacy->format);
    int pixels = legacy->width*legacy->height;
    const unsigned char *legacyPixels = (const unsigned char *)legacy->data;
    const unsigned char *batchedPixels = (const unsigned char *)batched->data;

    for (int i = 0; i < pixels; i++)
    {
        bool mismatch = false;
        for (int channel = 0; channel < bytesPerPixel; channel++)
        {
            int delta = abs((int)legacyPixels[i*bytesPerPixel + channel] -
                            (int)batchedPixels[i*bytesPerPixel + channel]);
            if (delta > diff.maxChannelDelta) diff.maxChannelDelta = delta;
            if (delta != 0) mismatch = true;
        }
        if (mismatch) diff.mismatchedPixels++;
    }

    return diff;
}

static Image CreateDiffImage(const Image *legacy, const Image *batched)
{
    Image diff = GenImageColor(legacy->width, legacy->height, BLACK);
    int bytesPerPixel = GetPixelDataSize(1, 1, legacy->format);
    int pixels = legacy->width*legacy->height;
    const unsigned char *legacyPixels = (const unsigned char *)legacy->data;
    const unsigned char *batchedPixels = (const unsigned char *)batched->data;

    for (int i = 0; i < pixels; i++)
    {
        bool mismatch = false;
        for (int channel = 0; channel < bytesPerPixel; channel++)
        {
            if (legacyPixels[i*bytesPerPixel + channel] != batchedPixels[i*bytesPerPixel + channel])
            {
                mismatch = true;
                break;
            }
        }

        if (mismatch) ImageDrawPixel(&diff, i%legacy->width, i/legacy->width, RED);
    }

    return diff;
}

static void ExportComparison(const char *artifactDir, const char *prefix, int frame,
                             const Image *legacy, const Image *batched)
{
    char path[512] = { 0 };
    Image diff = CreateDiffImage(legacy, batched);

    snprintf(path, sizeof(path), "%s/%s_frame%02i_legacy.png", artifactDir, prefix, frame);
    ExportImage(*legacy, path);
    snprintf(path, sizeof(path), "%s/%s_frame%02i_batched.png", artifactDir, prefix, frame);
    ExportImage(*batched, path);
    snprintf(path, sizeof(path), "%s/%s_frame%02i_diff.png", artifactDir, prefix, frame);
    ExportImage(diff, path);

    UnloadImage(diff);
}

static int RunPixelTests(const char *artifactDir)
{
    Texture2D textures[SPRITE_TEXTURE_COUNT] = { 0 };
    GenerateSpriteTextures(textures);

    SpriteBatch batch = { 0 };
    if (!SpriteBatchInit(&batch, 10000, SPRITE_GRID_BUCKETS))
    {
        UnloadSpriteTextures(textures);
        printf("FAIL: could not allocate sprite batch\n");
        return 1;
    }

    int counts[] = { 1000, 5000, 10000 };
    int failures = 0;
    RenderTexture2D target = LoadRenderTexture(SPRITE_PIXEL_WIDTH, SPRITE_PIXEL_HEIGHT);

    for (int countIndex = 0; countIndex < 3; countIndex++)
    {
        int count = counts[countIndex];
        printf("\n[%i sprites] frame-by-frame pixel comparison\n", count);
        printf("frame  legacy_hash         batched_hash        mismatched  max_delta  result\n");

        SpriteBatchSetCount(&batch, count, textures);
        for (int frame = 0; frame < SPRITE_COMPARE_FRAMES; frame++)
        {
            for (int i = 0; i < batch.itemCount; i++)
            {
                batch.items[i].tint.a = (unsigned char)(150 + ((frame*11 + i*13)%106));
            }

            RenderToTexture(&batch, SPRITE_RENDER_LEGACY, target);
            Image legacy = LoadImageFromTexture(target.texture);

            RenderToTexture(&batch, SPRITE_RENDER_BATCHED, target);
            Image batched = LoadImageFromTexture(target.texture);

            ImageDiff diff = CompareImages(&legacy, &batched);
            bool pass = (diff.mismatchedPixels == 0) && (diff.maxChannelDelta == 0) &&
                        (diff.legacyHash == diff.batchedHash);

            printf("%5i  0x%016llx  0x%016llx  %10i  %9i  %s\n", frame, diff.legacyHash,
                   diff.batchedHash, diff.mismatchedPixels, diff.maxChannelDelta, pass? "PASS" : "FAIL");

            if (!pass) failures++;
            if (frame == 0)
            {
                char prefix[64] = { 0 };
                snprintf(prefix, sizeof(prefix), "sprite_batching_%i", count);
                ExportComparison(artifactDir, prefix, frame, &legacy, &batched);
            }

            UnloadImage(legacy);
            UnloadImage(batched);
        }
    }

    failures += RunOverlapTest(artifactDir);

    UnloadRenderTexture(target);
    SpriteBatchUnload(&batch);
    UnloadSpriteTextures(textures);

    printf("\nPixel tests: %s (%i failures)\n", (failures == 0)? "PASS" : "FAIL", failures);
    return failures;
}

static int RunOverlapTest(const char *artifactDir)
{
    Texture2D textures[SPRITE_TEXTURE_COUNT] = { 0 };
    GenerateSpriteTextures(textures);

    SpriteBatch batch = { 0 };
    if (!SpriteBatchInit(&batch, 256, SPRITE_GRID_BUCKETS))
    {
        UnloadSpriteTextures(textures);
        printf("FAIL: could not allocate overlap test batch\n");
        return 1;
    }
    int failures = 0;

    for (int i = 0; i < 192; i++)
    {
        unsigned int hash = SpriteHash((unsigned int)i + 0x51ed270bu);
        SpriteItem item = { 0 };
        item.texture = textures[hash%SPRITE_TEXTURE_COUNT];
        item.blend = (BlendMode)(hash%(unsigned int)SPRITE_BLEND_COUNT);
        item.source = (Rectangle){ 0.0f, 0.0f, (float)item.texture.width, (float)item.texture.height };
        item.destination = (Rectangle){
            50.0f + (float)((i*37)%760),
            40.0f + (float)((i*53)%430),
            34.0f + (float)(hash%30),
            34.0f + (float)((hash >> 8)%30)
        };
        item.origin = (Vector2){ item.destination.width*0.5f, item.destination.height*0.5f };
        item.rotation = (float)((int)(hash >> 16)%360);
        item.tint = (Color){
            (unsigned char)(180 + (hash%76)),
            (unsigned char)(180 + ((hash >> 8)%76)),
            (unsigned char)(180 + ((hash >> 16)%76)),
            150
        };
        SpriteBatchAdd(&batch, &item);
    }

    RenderTexture2D target = LoadRenderTexture(SPRITE_PIXEL_WIDTH, SPRITE_PIXEL_HEIGHT);
    printf("\n[overlap/rotation test] frame-by-frame pixel comparison\n");
    printf("frame  layers  legacy_hash         batched_hash        mismatched  max_delta  result\n");

    for (int frame = 0; frame < SPRITE_COMPARE_FRAMES; frame++)
    {
        for (int i = 0; i < batch.itemCount; i++)
        {
            batch.items[i].tint.a = (unsigned char)(125 + ((frame*17 + i*7)%131));
        }

        RenderToTexture(&batch, SPRITE_RENDER_LEGACY, target);
        Image legacy = LoadImageFromTexture(target.texture);
        RenderToTexture(&batch, SPRITE_RENDER_BATCHED, target);
        Image batched = LoadImageFromTexture(target.texture);

        ImageDiff diff = CompareImages(&legacy, &batched);
        bool pass = (diff.mismatchedPixels == 0) && (diff.maxChannelDelta == 0);
        printf("%5i  %6i  0x%016llx  0x%016llx  %10i  %9i  %s\n", frame, batch.layerCount,
               diff.legacyHash, diff.batchedHash, diff.mismatchedPixels, diff.maxChannelDelta,
               pass? "PASS" : "FAIL");

        if (!pass) failures++;
        if (frame == 0) ExportComparison(artifactDir, "sprite_batching_overlap", frame, &legacy, &batched);

        UnloadImage(legacy);
        UnloadImage(batched);
    }

    UnloadRenderTexture(target);
    SpriteBatchUnload(&batch);
    UnloadSpriteTextures(textures);

    return failures;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Benchmark
//----------------------------------------------------------------------------------
static double BenchmarkPath(SpriteBatch *batch, SpriteRenderPath path, int frames)
{
    RenderTexture2D target = LoadRenderTexture(SPRITE_PIXEL_WIDTH, SPRITE_PIXEL_HEIGHT);

    for (int i = 0; i < SPRITE_BENCH_WARMUP; i++) RenderToTexture(batch, path, target);

    rlDrawRenderBatchActive();
    Image sync = LoadImageFromTexture(target.texture);
    UnloadImage(sync);

    double start = GetTime();
    for (int i = 0; i < frames; i++) RenderToTexture(batch, path, target);
    rlDrawRenderBatchActive();

    Image finalImage = LoadImageFromTexture(target.texture);
    UnloadImage(finalImage);
    double elapsed = (GetTime() - start)*1000.0/(double)frames;

    UnloadRenderTexture(target);
    return elapsed;
}

static int CompareDoubles(const void *a, const void *b)
{
    double left = *(const double *)a;
    double right = *(const double *)b;
    return (left < right)? -1 : ((left > right)? 1 : 0);
}

static void RunBenchmark(void)
{
    Texture2D textures[SPRITE_TEXTURE_COUNT] = { 0 };
    GenerateSpriteTextures(textures);

    SpriteBatch batch = { 0 };
    if (!SpriteBatchInit(&batch, 10000, SPRITE_GRID_BUCKETS))
    {
        UnloadSpriteTextures(textures);
        printf("FAIL: could not allocate sprite batch\n");
        return;
    }

    int counts[] = { 1000, 5000, 10000 };
    double legacyTimes[SPRITE_BENCH_SAMPLES] = { 0 };
    double batchedTimes[SPRITE_BENCH_SAMPLES] = { 0 };
    SpriteDrawStats legacyStats = { 0 };
    SpriteDrawStats batchedStats = { 0 };

    printf("Sprite batching benchmark (%ix%i, %i frames/sample, median of %i)\n",
           SPRITE_PIXEL_WIDTH, SPRITE_PIXEL_HEIGHT, SPRITE_BENCH_FRAMES, SPRITE_BENCH_SAMPLES);
    printf("count  legacy_ms  batched_ms  reduction  speedup  legacy_state_ops  batched_state_ops\n");

    for (int countIndex = 0; countIndex < 3; countIndex++)
    {
        SpriteBatchSetCount(&batch, counts[countIndex], textures);

        for (int sample = 0; sample < SPRITE_BENCH_SAMPLES; sample++)
        {
            if (sample%2 == 0)
            {
                legacyTimes[sample] = BenchmarkPath(&batch, SPRITE_RENDER_LEGACY, SPRITE_BENCH_FRAMES);
                batchedTimes[sample] = BenchmarkPath(&batch, SPRITE_RENDER_BATCHED, SPRITE_BENCH_FRAMES);
            }
            else
            {
                batchedTimes[sample] = BenchmarkPath(&batch, SPRITE_RENDER_BATCHED, SPRITE_BENCH_FRAMES);
                legacyTimes[sample] = BenchmarkPath(&batch, SPRITE_RENDER_LEGACY, SPRITE_BENCH_FRAMES);
            }
        }

        RenderSpriteScene(&batch, SPRITE_RENDER_LEGACY, &legacyStats);
        RenderSpriteScene(&batch, SPRITE_RENDER_BATCHED, &batchedStats);

        qsort(legacyTimes, SPRITE_BENCH_SAMPLES, sizeof(double), CompareDoubles);
        qsort(batchedTimes, SPRITE_BENCH_SAMPLES, sizeof(double), CompareDoubles);
        double legacy = legacyTimes[SPRITE_BENCH_SAMPLES/2];
        double batched = batchedTimes[SPRITE_BENCH_SAMPLES/2];
        double reduction = 100.0*(legacy - batched)/legacy;
        double speedup = legacy/batched;

        printf("%5i  %9.3f  %10.3f  %8.1f%%  %6.2fx  %14i  %15i\n", counts[countIndex],
               legacy, batched, reduction, speedup, legacyStats.stateChanges,
               batchedStats.stateChanges);
        printf("      state ops: legacy blend=%i texture=%i | batched blend=%i texture=%i\n",
               legacyStats.blendChanges, legacyStats.textureBinds,
               batchedStats.blendChanges, batchedStats.textureBinds);
    }

    printf("\nBatched path reuses item/sort/grid buffers and raylib's internal VBO;\n");
    printf("no heap allocation occurs per frame once the batch reaches capacity.\n");

    SpriteBatchUnload(&batch);
    UnloadSpriteTextures(textures);
}
