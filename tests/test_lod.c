/*******************************************************************************************
*
*   test_lod - CPU-side tests for the raylib instance LOD batch API
*
*   Build/run:
*       make -C tests
*       ./tests/test_lod
*
*   Leak check:
*       make -C tests asan
*       ./tests/test_lod_asan
*
*******************************************************************************************/

#include "raylib.h"
#include "raymath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846
#define TEST_BUDGET 1.0f
#define TEST_HEIGHT 1080
#define TEST_FOVY 45.0f

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static Camera MakeCamera(Vector3 position, int projection)
{
    Camera camera = { 0 };

    camera.position = position;
    camera.target = (Vector3){ position.x, position.y, position.z + 1.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = TEST_FOVY;
    camera.projection = projection;

    return camera;
}

static LodInstance MakeInstance(float z, int seamGroup)
{
    LodInstance instance = { 0 };

    instance.transform = MatrixTranslate(0.0f, 0.0f, z);
    instance.boundsCenter = (Vector3){ 0.0f, 0.0f, 0.0f };
    instance.boundsRadius = 0.0f;
    instance.seamGroup = seamGroup;

    return instance;
}

static void TestValidation(void)
{
    LodLevel levels[RL_LOD_MAX_LEVELS] = { 0 };

    for (int i = 1; i < RL_LOD_MAX_LEVELS; i++) levels[i].geometricError = (float)i*0.01f;

    CHECK(CreateLodBatch(NULL, 1) == NULL);
    CHECK(CreateLodBatch(levels, 0) == NULL);
    CHECK(CreateLodBatch(levels, RL_LOD_MAX_LEVELS + 1) == NULL);

    levels[2].geometricError = -0.1f;
    CHECK(CreateLodBatch(levels, 3) == NULL);

    levels[2].geometricError = 0.02f;
    levels[1].geometricError = 0.03f;
    CHECK(CreateLodBatch(levels, 3) == NULL);

    levels[1].geometricError = 0.01f;
    LodBatch *batch = CreateLodBatch(levels, 3);
    CHECK(batch != NULL);
    CHECK(IsLodBatchValid(batch));
    CHECK(!IsLodBatchValid(NULL));

    LodInstance valid = MakeInstance(4.0f, 0);
    LodInstance invalid = valid;

    CHECK(!SubmitLodInstances(NULL, &valid, 1));
    CHECK(!SubmitLodInstances(batch, NULL, 1));
    CHECK(!SubmitLodInstances(batch, &valid, 0));
    CHECK(!SubmitLodInstances(batch, &valid, -1));

    invalid = valid;
    invalid.seamGroup = -1;
    CHECK(!SubmitLodInstances(batch, &invalid, 1));

    invalid = valid;
    invalid.boundsRadius = -1.0f;
    CHECK(!SubmitLodInstances(batch, &invalid, 1));

    invalid = valid;
    invalid.transform.m12 = NAN;
    CHECK(!SubmitLodInstances(batch, &invalid, 1));

    Camera camera = MakeCamera((Vector3){ 0.0f, 0.0f, -10.0f }, CAMERA_PERSPECTIVE);

    CHECK(!UpdateLodBatch(NULL, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));
    CHECK(!UpdateLodBatch(batch, camera, 0, TEST_BUDGET, 0.1f));
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, 0.0f, 0.1f));
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, -0.1f));
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 1.0f));

    camera.fovy = 0.0f;
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));

    camera = MakeCamera((Vector3){ 0.0f, 0.0f, -10.0f }, CAMERA_PERSPECTIVE);
    camera.position.x = NAN;
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));

    camera = MakeCamera((Vector3){ 0.0f, 0.0f, -10.0f }, CAMERA_PERSPECTIVE);
    camera.target = camera.position;
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));

    camera = MakeCamera((Vector3){ 0.0f, 0.0f, -10.0f }, 999);
    CHECK(!UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));

    CHECK(SubmitLodInstances(batch, &valid, 1));
    CHECK(GetLodBatchStats(batch).instanceCount == 1);

    CHECK(!SetLodInstanceTransform(batch, -1, valid.transform));
    CHECK(!SetLodInstanceTransform(batch, 1, valid.transform));

    Matrix invalidTransform = valid.transform;
    invalidTransform.m0 = NAN;
    CHECK(!SetLodInstanceTransform(batch, 0, invalidTransform));
    CHECK(SetLodInstanceTransform(batch, 0, MatrixTranslate(0.0f, 0.0f, 12.0f)));

    CHECK(GetLodInstanceLevel(batch, -1) == -1);
    CHECK(GetLodInstanceLevel(batch, 1) == -1);
    CHECK(GetLodInstanceLevel(batch, 0) == -1);
    CHECK(GetLodInstanceScreenError(batch, 0) < 0.0f);
    CHECK(GetLodInstanceScreenError(batch, -1) < 0.0f);
    CHECK(GetLodBatchDraws(NULL, NULL, 0) == 0);

    ClearLodInstances(batch);
    CHECK(GetLodBatchStats(batch).instanceCount == 0);

    camera = MakeCamera((Vector3){ 0.0f, 0.0f, -10.0f }, CAMERA_PERSPECTIVE);
    CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));

    UnloadLodBatch(batch);
    UnloadLodBatch(NULL);
}

static void TestSelectionAndHysteresis(void)
{
    LodLevel levels[2] = { 0 };
    levels[1].geometricError = 0.01f;

    LodBatch *batch = CreateLodBatch(levels, 2);
    LodInstance instance = MakeInstance(9.5f, 0);
    Camera camera = MakeCamera((Vector3){ 0.0f, 0.0f, 0.0f }, CAMERA_PERSPECTIVE);

    CHECK(SubmitLodInstances(batch, &instance, 1));
    CHECK(UpdateLodBatch(batch, camera, 100, 0.1f, 0.2f));
    CHECK(GetLodInstanceLevel(batch, 0) == 0);

    // At 25 units the level-1 bound is 0.0966 px, inside budget but outside
    // the 0.08 px coarsening limit, so hysteresis keeps level 0
    CHECK(SetLodInstanceTransform(batch, 0, MatrixTranslate(0.0f, 0.0f, 25.0f)));
    CHECK(UpdateLodBatch(batch, camera, 100, 0.1f, 0.2f));
    CHECK(GetLodInstanceLevel(batch, 0) == 0);

    // At 31 units the bound is 0.0779 px, so coarsening is accepted
    CHECK(SetLodInstanceTransform(batch, 0, MatrixTranslate(0.0f, 0.0f, 31.0f)));
    CHECK(UpdateLodBatch(batch, camera, 100, 0.1f, 0.2f));
    CHECK(GetLodInstanceLevel(batch, 0) == 1);
    CHECK(GetLodInstanceScreenError(batch, 0) <= 0.1f);

    // Moving back inside the hard budget refines immediately
    CHECK(SetLodInstanceTransform(batch, 0, MatrixTranslate(0.0f, 0.0f, 23.0f)));
    CHECK(UpdateLodBatch(batch, camera, 100, 0.1f, 0.2f));
    CHECK(GetLodInstanceLevel(batch, 0) == 0);

    LodStats stats = GetLodBatchStats(batch);
    CHECK(stats.switchCount == 2);
    CHECK(stats.maxScreenError <= 0.1f);

    // Orthographic pixel scale uses fovy as the near-plane world height
    camera = MakeCamera((Vector3){ 0.0f, 0.0f, 0.0f }, CAMERA_ORTHOGRAPHIC);
    camera.fovy = 10.0f;
    CHECK(UpdateLodBatch(batch, camera, 100, 1.0f, 0.2f));
    CHECK(GetLodInstanceLevel(batch, 0) == 1);

    UnloadLodBatch(batch);
}

static void TestSeamLock(void)
{
    LodLevel levels[2] = { 0 };
    levels[1].geometricError = 0.01f;

    LodBatch *batch = CreateLodBatch(levels, 2);
    LodInstance instances[4] = {
        MakeInstance(4.0f, 7),
        MakeInstance(80.0f, 7),
        MakeInstance(4.0f, 8),
        MakeInstance(80.0f, 8)
    };
    Camera camera = MakeCamera((Vector3){ 0.0f, 0.0f, 0.0f }, CAMERA_PERSPECTIVE);

    CHECK(SubmitLodInstances(batch, instances, 4));
    CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));

    CHECK(GetLodInstanceLevel(batch, 0) == 0);
    CHECK(GetLodInstanceLevel(batch, 1) == 0);
    CHECK(GetLodInstanceLevel(batch, 2) == 0);
    CHECK(GetLodInstanceLevel(batch, 3) == 0);

    LodStats stats = GetLodBatchStats(batch);
    CHECK(stats.seamLockedCount == 2);
    CHECK(stats.drawCount == 1);
    CHECK(stats.levelCounts[0] == 4);
    CHECK(GetLodBatchDraws(batch, NULL, 0) == 1);

    LodDraw draw = { 0 };
    CHECK(GetLodBatchDraws(batch, &draw, 1) == 1);
    CHECK(draw.level == 0);
    CHECK(draw.instanceCount == 4);
    CHECK(draw.transforms != NULL);

    UnloadLodBatch(batch);
}

static void TestAppendSubmission(void)
{
    LodLevel levels[2] = { 0 };
    levels[1].geometricError = 0.01f;

    LodBatch *batch = CreateLodBatch(levels, 2);
    Camera camera = MakeCamera((Vector3){ 0.0f, 0.0f, 0.0f }, CAMERA_PERSPECTIVE);
    LodInstance nearInstance = MakeInstance(4.0f, 3);
    LodInstance farInstance = MakeInstance(80.0f, 3);
    LodInstance independent = MakeInstance(80.0f, 0);

    CHECK(SubmitLodInstances(batch, &nearInstance, 1));
    CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));
    CHECK(GetLodInstanceLevel(batch, 0) == 0);

    CHECK(SubmitLodInstances(batch, &farInstance, 1));
    CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));
    CHECK(GetLodBatchStats(batch).instanceCount == 2);
    CHECK(GetLodInstanceLevel(batch, 0) == 0);
    CHECK(GetLodInstanceLevel(batch, 1) == 0);
    CHECK(GetLodBatchStats(batch).seamLockedCount == 1);

    CHECK(SubmitLodInstances(batch, &independent, 1));
    CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));
    CHECK(GetLodInstanceLevel(batch, 2) == 1);
    CHECK(GetLodBatchStats(batch).drawCount == 2);

    ClearLodInstances(batch);
    CHECK(GetLodBatchStats(batch).instanceCount == 0);

    UnloadLodBatch(batch);
}

// Radial distance of an inscribed regular polygon at angle theta
static double GetPolygonRadius(double theta, int sides)
{
    double step = 2.0*TEST_PI/sides;
    double local = fmod(theta, step);
    if (local < 0.0) local += step;

    return cos(TEST_PI/sides)/cos(local - TEST_PI/sides);
}

// Measure the maximum radial gap between two inscribed polygon contours
static double MeasureContourGap(int fineSides, int coarseSides, int samples)
{
    double gap = 0.0;

    for (int i = 0; i < samples; i++)
    {
        double theta = 2.0*TEST_PI*(double)i/(double)samples;
        double fine = GetPolygonRadius(theta, fineSides);
        double coarse = GetPolygonRadius(theta, coarseSides);
        double delta = fabs(fine - coarse);

        if (delta > gap) gap = delta;
    }

    return gap;
}

static void TestContourErrorData(void)
{
    const int sideCounts[4] = { 64, 32, 16, 8 };
    LodLevel levels[4] = { 0 };

    for (int i = 0; i < 4; i++)
    {
        levels[i].geometricError = (float)(1.0 - cos(TEST_PI/sideCounts[i]));
    }

    LodBatch *batch = CreateLodBatch(levels, 4);
    LodInstance instance = MakeInstance(0.0f, 0);
    Camera camera = MakeCamera((Vector3){ 0.0f, 0.0f, 0.0f }, CAMERA_PERSPECTIVE);

    CHECK(SubmitLodInstances(batch, &instance, 1));

    printf("\nContour switch data (vertical FOV %.0f deg, %d px, %.1f px budget)\n",
           TEST_FOVY, TEST_HEIGHT, TEST_BUDGET);
    printf("  fine  coarse  switch d  bound px  sampled gap  projected px\n");

    int previousLevel = 0;

    for (int i = 0; i < 4; i++)
    {
        double error = 1.0 - cos(TEST_PI/sideCounts[i]);
        double switchDistance = error*TEST_HEIGHT/(tan(TEST_FOVY*TEST_PI/180.0/2.0)*TEST_BUDGET);

        CHECK(SetLodInstanceTransform(batch, 0, MatrixTranslate(0.0f, 0.0f, (float)(switchDistance + 0.001))));
        CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.0f));

        int level = GetLodInstanceLevel(batch, 0);
        CHECK(level == i);
        CHECK(GetLodInstanceScreenError(batch, 0) <= TEST_BUDGET + 0.0001f);

        if (i > 0)
        {
            double gap = MeasureContourGap(sideCounts[previousLevel], sideCounts[i], 262144);
            double projected = gap*TEST_HEIGHT/(2.0*tan(TEST_FOVY*TEST_PI/180.0/2.0)*switchDistance);

            printf("  %4d  %6d  %8.2f  %7.3f  %11.6f  %12.6f\n",
                   sideCounts[previousLevel], sideCounts[i], switchDistance,
                   (double)GetLodInstanceScreenError(batch, 0), gap, projected);

            CHECK(projected <= TEST_BUDGET + 0.0001);
        }

        previousLevel = i;
    }

    UnloadLodBatch(batch);
}

static unsigned int NextRandom(unsigned int *state)
{
    *state = (*state*1664525u + 1013904223u);
    return *state;
}

static void TestStressUpdates(void)
{
    const int instanceCount = 512;
    const int updateCount = 10000;

    LodLevel levels[4] = { 0 };
    levels[1].geometricError = 0.01f;
    levels[2].geometricError = 0.04f;
    levels[3].geometricError = 0.16f;

    LodBatch *batch = CreateLodBatch(levels, 4);
    LodInstance *instances = (LodInstance *)calloc(instanceCount, sizeof(LodInstance));
    unsigned int randomState = 0x12345678u;

    CHECK(batch != NULL);
    CHECK(instances != NULL);

    for (int i = 0; i < instanceCount; i++)
    {
        float z = 2.0f + (float)(NextRandom(&randomState)%4000)/10.0f;
        instances[i] = MakeInstance(z, (i%16 == 0)? (i/16 + 1) : 0);
    }

    CHECK(SubmitLodInstances(batch, instances, instanceCount));

    unsigned int allocatedBefore = GetLodBatchStats(batch).allocatedBytes;
    Camera camera = MakeCamera((Vector3){ 0.0f, 0.0f, -30.0f }, CAMERA_PERSPECTIVE);

    for (int frame = 0; frame < updateCount; frame++)
    {
        camera.position.x = 35.0f*sinf((float)frame*0.013f);
        camera.position.y = 12.0f*cosf((float)frame*0.007f);
        camera.position.z = -30.0f + 120.0f*sinf((float)frame*0.0031f);
        camera.target.x = camera.position.x;
        camera.target.y = camera.position.y;
        camera.target.z = camera.position.z + 100.0f;

        CHECK(UpdateLodBatch(batch, camera, TEST_HEIGHT, TEST_BUDGET, 0.1f));
    }

    LodStats stats = GetLodBatchStats(batch);
    CHECK(stats.instanceCount == instanceCount);
    CHECK(stats.drawCount > 0);
    CHECK(stats.drawCount <= 4);
    CHECK(stats.maxScreenError <= TEST_BUDGET + 0.0001f);
    CHECK(stats.allocatedBytes == allocatedBefore);

    int active = 0;
    for (int i = 0; i < RL_LOD_MAX_LEVELS; i++) active += stats.levelCounts[i];
    CHECK(active == instanceCount);

    printf("\nStress: %d instances x %d updates, allocations stable at %u bytes, %d switches\n",
           instanceCount, updateCount, stats.allocatedBytes, stats.switchCount);

    free(instances);
    UnloadLodBatch(batch);
}

int main(void)
{
    TestValidation();
    TestSelectionAndHysteresis();
    TestSeamLock();
    TestAppendSubmission();
    TestContourErrorData();
    TestStressUpdates();

    if (failures == 0)
    {
        printf("\nAll LOD tests passed\n");
        return 0;
    }

    fprintf(stderr, "\n%d LOD test(s) failed\n", failures);

    return 1;
}
