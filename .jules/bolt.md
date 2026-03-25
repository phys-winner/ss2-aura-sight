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
