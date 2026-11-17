# Instance LOD tests and data

`test_lod.c` exercises the CPU side of the `LodBatch` API added to `raylib.h`.
The renderer uses one `DrawMeshInstanced()` call per active level.

## Usage

```c
LodLevel levels[] = {
    { meshLod0, material, 0.00f },
    { meshLod1, material, 0.01f },
    { meshLod2, material, 0.04f }
};

LodBatch *batch = CreateLodBatch(levels, 3);

LodInstance instances[] = {
    { transform, { 0.0f, 0.0f, 0.0f }, 1.5f, 1001 },
    { transform2, { 0.0f, 0.0f, 0.0f }, 1.5f, 1001 }
};

SubmitLodInstances(batch, instances, 2);

// Per frame, before drawing
UpdateLodBatch(batch, camera, GetScreenHeight(), 1.0f, 0.1f);
DrawLodBatch(batch);

// At shutdown; batch does not own meshes or materials
UnloadLodBatch(batch);
```

`UpdateLodBatch()` does not allocate. It only updates the existing instance
level arrays, seam-group state, and per-level transform buckets.

## Error bound

`LodLevel.geometricError` is the absolute Hausdorff-style deviation from the
reference mesh in local units. For levels `a` and `b`, the contour difference
is bounded by:

```
worldError <= geometricError[a] + geometricError[b]
```

The implementation uses the conservative `2 * max(error[a], error[b])` bound.
At nearest view depth `z`, with viewport height `H` and vertical FOV `f`, the
projected bound is:

```
2 * geometricError * maxScale * H / (2 * z * tan(f / 2))
```

For an orthographic camera, `H / fovy` replaces the perspective pixel scale.
The instance bounding sphere is projected to its nearest view depth, so the
bound is conservative over the whole instance and for off-axis views.

A switch is allowed only when the projected bound is at most
`screenErrorBudget`. Hysteresis delays coarsening to
`screenErrorBudget * (1 - hysteresis)` but never relaxes the hard budget.

## Seam locking

Instances with the same non-zero `seamGroup` always receive one common level.
The group uses the finest level requested by any member, which keeps every
shared border on the same mesh level and prevents cracks. Group 0 is
independent. Seam groups are batch-local.

Keep a seam group local to one connected patch. A very large group inherits
the finest level needed by any member, which is correct but can increase draw
work.

Shared borders must still be authored with identical vertices in every LOD
level. This API locks the selected level and the switch frame; it cannot repair
different border geometry supplied by the caller.

## Boundary cases covered

- zero, negative, or more than `RL_LOD_MAX_LEVELS` levels
- negative, NaN, or decreasing geometric errors
- null submissions, zero/negative instance counts
- NaN transforms and centers, negative or NaN bounds radius
- negative seam groups and empty batches
- invalid viewport height, FOV, projection, budget, or hysteresis
- switch-distance boundaries, hysteresis dead band, and immediate refinement
- seam groups with members at different distances
- every instance in one seam group, producing one draw group
- perspective and orthographic pixel scales
- clear and reuse of a batch without freeing its capacity
- 10,000 consecutive updates with moving camera

## Measured contour data

The contour test uses regular polygons inscribed in a unit circle. A polygon
with `N` sides has geometric error `1 - cos(pi / N)`. Test settings:

- viewport height: 1080 px
- vertical FOV: 45 degrees
- pixel budget: 1.0 px
- camera at the instance origin
- 262,144 angular samples per switch

| Fine sides | Coarse sides | Switch distance | API bound | Sampled world gap | Projected gap |
|---:|---:|---:|---:|---:|---:|
| 64 | 32 | 12.56 | 1.000 px | 0.004815 | 0.500000 px |
| 32 | 16 | 50.10 | 1.000 px | 0.019215 | 0.500000 px |
| 16 | 8 | 198.47 | 1.000 px | 0.076120 | 0.500000 px |

The sampled gap is exactly the coarse-level geometric error, so the measured
contour jump is 50% of the conservative API bound.

## Stress and leak result

The stress case submits 512 instances across 4 levels and 16 seam groups, then
runs 10,000 camera updates with hysteresis 0.1. Observed on the test machine:

```
512 instances x 10000 updates
allocatedBytes: 95,848 before and after
level switches: 4,677
maxScreenError <= 1.0 px
```

`make -C tests asan` plus `ASAN_OPTIONS=detect_leaks=1` completes with no leaks
or sanitizer errors.

## Build

The test links against a memory-platform static raylib build so it does not
need a window or GPU:

```sh
make -C tests
./tests/test_lod

make -C tests asan
ASAN_OPTIONS=detect_leaks=1 ./tests/test_lod_asan
```
