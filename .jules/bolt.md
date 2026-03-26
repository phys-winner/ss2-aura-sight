## 2025-05-14 - [SS2 Object List & Rendering Bottlenecks]
**Learning:** The application suffers from several performance bottlenecks related to game object tracking and rendering:
1. `UpdateObjectList` scans all 8000 object slots every frame, even if few are active.
2. `UpdatePlayerData` calls `UpdateObjectList` twice per frame redundantly.
3. `WorldToScreen` and `Draw3DESP` both calculate `sqrtf(dist)` independently, and `WorldToScreen` does it even for objects that are immediately discarded for being out of range.
4. `Draw3DESP` performs expensive `GetStringProperty` (including `QueryInterface`) lookups every frame for every object on screen, which is the most significant bottleneck.

**Action:** Compact the object list during scan, remove redundant calls, use squared distance checks to skip `sqrtf`, and implement a name cache for object properties.

## 2026-03-25 - [String Caching and Loop Reordering in D3D9 EndScene]
**Learning:** In high-frequency rendering loops (like DirectX EndScene), (N)$ string transformations (like `std::transform` for case-insensitivity) and redundant hash map lookups are significant bottlenecks. Reordering loops to perform spatial culling (height/distance/visibility) *before* metadata lookups can skip thousands of operations per frame.
**Action:** Always pre-calculate and cache expensive string properties (like lowercase names for filtering). Prioritize spatial checks (WorldToScreen, distance) to cull entities before accessing centralized caches or performing logic.

## 2026-03-26 - [DirectX Hook Rendering Hot-Path Optimization]
**Learning:** In high-frequency rendering loops (like DirectX EndScene), every operation is multiplied by the number of objects and the frame rate. Bottlenecks often include:
1. **Redundant Math:** Recalculating constants like FOV scaling or using double-precision math (`sin`) where floats (`sinf`) suffice.
2. **String Copies:** Copying `std::string` from cache for every visible object every frame.
3. **Cache Lookups in Sort:** Performing $O(N \log N)$ map lookups and math inside a `std::sort` comparator.
4. **Late Culling:** Performing metadata lookups for objects that are eventually culled by screen-space bounds.

**Action:** Pre-calculate FOV constants; use pointers/references to cached entities; move spatial culling (bounds checking) before cache lookups; pre-calculate sort keys (distSq) into a flat vector before sorting to optimize the comparison function.
