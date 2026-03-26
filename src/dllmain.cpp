#include "MinHook.h"
#include "d3d9_hook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

struct Vec3 {
  float x, y, z;
};

// Typedefs
typedef long(__stdcall *EndSceneFn)(IDirect3DDevice9 *device);
EndSceneFn oEndScene = nullptr;

typedef long(__stdcall *DrawPrimitiveUPFn)(IDirect3DDevice9 *device,
                                           D3DPRIMITIVETYPE PrimitiveType,
                                           UINT PrimitiveCount,
                                           const void *pVertexStreamZeroData,
                                           UINT VertexStreamZeroStride);
DrawPrimitiveUPFn oDrawPrimitiveUP = nullptr;

typedef long(__stdcall *SetTextureFn)(IDirect3DDevice9 *device, DWORD Stage,
                                      IDirect3DBaseTexture9 *pTexture);
SetTextureFn oSetTexture = nullptr;

typedef BOOL(WINAPI *SetCursorPosFn)(int x, int y);
SetCursorPosFn oSetCursorPos = nullptr;

// Global State
bool g_Initialized = false;
bool g_ShowMenu = true;
bool g_BlockInput = true;

enum EntityCategory {
  CAT_UNKNOWN,
  CAT_WEAPON,
  CAT_AMMO,
  CAT_CONSUMABLE,
  CAT_RESOURCES,
  CAT_MISC,
  CAT_INTERACTIVE,
  CAT_ENEMY,
  CAT_COUNT
};

struct CategoryInfo {
  const char *name;
  ImU32 color;
  bool enabled;
};

CategoryInfo g_Categories[CAT_COUNT] = {
    {"Unknown", IM_COL32(200, 200, 200, 255), true},
    {"Weapons", IM_COL32(255, 50, 50, 255), true},
    {"Ammo", IM_COL32(255, 165, 0, 255), true},
    {"Consumables", IM_COL32(50, 255, 50, 255), true},
    {"Resources", IM_COL32(0, 255, 255, 255), true},
    {"Misc Items", IM_COL32(255, 255, 0, 255), true},
    {"Interactive", IM_COL32(200, 0, 255, 255), true},
    {"Enemies", IM_COL32(255, 0, 0, 255), true}};

// Hack Settings
bool g_Wallhack = false;
bool g_Wireframe = false;
bool g_Fullbright = false;
bool g_ItemESP = false;
bool g_3DESP = false;
bool g_3DESPShowNonItems = false;
float g_Transparency = 0.5f;
float g_ESPDistance = 50.0f;
char g_SearchFilter[64] = "";
std::string g_SearchFilterLower = "";

// Cache
struct CachedEntity {
  std::string name;
  std::string lowerName;
  EntityCategory cat;
};
std::unordered_map<uintptr_t, CachedEntity> g_EntityCache;

// Object Data
struct GameObject {
  float x, y, z;
  uintptr_t ptr;
  int id;
};
#define MAX_OBJECTS 8000
GameObject g_Objects[MAX_OBJECTS];
int g_ObjectCount = 0;

struct ViewData {
  float sinYaw, cosYaw;
  float sinPitch, cosPitch;
  float sinRoll, cosRoll;
  float fovScale;
};
ViewData g_ViewData;

int g_TrackedEntityID = -1;
bool g_HideEmptyNames = true;

// Player Data
struct PlayerData {
  float x, y, z;
  float camX, camY, camZ;
  short camRoll, camUpDn, camLftRt;
  short playerUpDn, playerLftRt;
} g_Player;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

WNDPROC oWndProc;
HWND window = NULL;

struct cStr {
  char *m_pchData;
  int m_nLen;
  int m_nAlloc;

  cStr() : m_pchData(nullptr), m_nLen(0), m_nAlloc(0) {}
};

typedef int(__stdcall *QueryInterface_t)(void *, void *, void **);

// GetPropertyStr: (This, OutBuffer*, ObjID, PropName, Context) -> cStr*
typedef int(__stdcall *GetPropertyStr_t)(void *, cStr *, int, const char *);

constexpr uintptr_t ADDR_ROOT_PTR = 0x5FF87C;    // dword_9FF87C
constexpr uintptr_t ADDR_IID_PROPMAN = 0x343050; // unk_743050 (GUID)

uintptr_t g_SS2Base = 0;

EntityCategory GetEntityCategory(const std::string &lowerName) {
  if (lowerName.empty())
    return CAT_UNKNOWN;

  // Enemies
  if (lowerName.find("hybrid") != std::string::npos ||
      lowerName.find("monkey") != std::string::npos ||
      lowerName.find("swinekeeper") != std::string::npos)
    return CAT_ENEMY;

  // Weapons
  if (lowerName.find("pistol") != std::string::npos ||
      lowerName.find("shotgun") != std::string::npos ||
      lowerName.find("wrench") != std::string::npos ||
      lowerName.find("launcher") != std::string::npos ||
      lowerName.find("psi amp") != std::string::npos)
    return CAT_WEAPON;

  // Ammo
  if (lowerName.find("bullets") != std::string::npos ||
      lowerName.find("shells") != std::string::npos ||
      lowerName.find("slugs") != std::string::npos)
    return CAT_AMMO;

  // Consumables
  if (lowerName.find("hypo") != std::string::npos ||
      lowerName.find("booster") != std::string::npos ||
      lowerName.find("bottle") != std::string::npos ||
      lowerName.find("can") != std::string::npos ||
      lowerName.find("chips") != std::string::npos ||
      lowerName.find("juice") != std::string::npos ||
      lowerName.find("vodka") != std::string::npos ||
      lowerName.find("liquor") != std::string::npos)
    return CAT_CONSUMABLE;

  // Resources / Important Items
  if (lowerName.find("nanites") != std::string::npos ||
      lowerName.find("cyber modules") != std::string::npos ||
      lowerName.find("software") != std::string::npos ||
      lowerName.find("implant") != std::string::npos ||
      lowerName.find("log") != std::string::npos ||
      lowerName.find("tool") != std::string::npos)
    return CAT_RESOURCES;

  // Periodic Table Elements (Resources)
  static const std::vector<std::string> elements = {
      "fermium", "gallium", "antimony",  "yttrium",    "californium",
      "osmium",  "iridium", "tellurium", "technetium", "barium"};
  for (const auto &el : elements) {
    if (lowerName.find(el) != std::string::npos)
      return CAT_RESOURCES;
  }

  // Interactive
  if (lowerName.find("computer") != std::string::npos ||
      lowerName.find("console") != std::string::npos ||
      lowerName.find("crate") != std::string::npos ||
      lowerName.find("replicator") != std::string::npos ||
      lowerName.find("port") != std::string::npos ||
      lowerName.find("keypad") != std::string::npos ||
      lowerName.find("locker") != std::string::npos)
    return CAT_INTERACTIVE;

  // Misc
  if (lowerName.find("desk") != std::string::npos ||
      lowerName.find("mug") != std::string::npos ||
      lowerName.find("plant") != std::string::npos ||
      lowerName.find("gurney") != std::string::npos ||
      lowerName.find("bag") != std::string::npos ||
      lowerName.find("barrel") != std::string::npos ||
      lowerName.find("basketball") != std::string::npos ||
      lowerName.find("cigarettes") != std::string::npos ||
      lowerName.find("player") != std::string::npos)
    return CAT_MISC;

  return CAT_UNKNOWN;
}

void InitSS2Base() {
  if (!g_SS2Base) {
    g_SS2Base = (uintptr_t)GetModuleHandleA("ss2.exe");
  }
}

bool GetStringProperty(int itemID, const char *propName, char *outBuffer,
                       int outSize) {
  if (!g_SS2Base)
    InitSS2Base();
  if (!outBuffer || outSize <= 0)
    return false;
  outBuffer[0] = '\0';

  void **pRoot = *(void ***)(ADDR_ROOT_PTR + g_SS2Base);
  if (!pRoot || !*pRoot) {
    return false;
  }

  void *pGUID = (void *)(ADDR_IID_PROPMAN + g_SS2Base);
  void *pPropManager = nullptr;

  QueryInterface_t QueryInterface = (*(QueryInterface_t **)pRoot)[0];

  if (QueryInterface(pRoot, pGUID, &pPropManager) != 0) {
    return false;
  }

  if (pPropManager == nullptr) {
    return false;
  }
  cStr tempStr;
  GetPropertyStr_t GetProp =
      (*(GetPropertyStr_t **)pPropManager)[5]; // 20 / 4 = 5

  GetProp(pPropManager, &tempStr, itemID + 1, propName);

  bool success = false;
  if (tempStr.m_pchData && tempStr.m_nLen > 0) {
    strncpy_s(outBuffer, outSize, tempStr.m_pchData, _TRUNCATE);
    success = true;
  }

  return success;
}

BOOL WINAPI Hooked_SetCursorPos(int x, int y) {
  if (g_ShowMenu && g_BlockInput)
    return TRUE;
  return oSetCursorPos(x, y);
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam,
                          LPARAM lParam) {
  if (uMsg == WM_KEYDOWN && wParam == VK_INSERT) {
    g_ShowMenu = !g_ShowMenu;
  }

  if (g_ShowMenu) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
      return true;

    if (g_BlockInput) {
      switch (uMsg) {
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_RBUTTONDBLCLK:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MBUTTONDBLCLK:
      case WM_MOUSEMOVE:
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL:
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP:
        return true;
      }
    }
  }

  return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

long __stdcall Hooked_DrawPrimitiveUP(IDirect3DDevice9 *device,
                                      D3DPRIMITIVETYPE PrimitiveType,
                                      UINT PrimitiveCount,
                                      const void *pVertexStreamZeroData,
                                      UINT VertexStreamZeroStride) {
  bool isWorld = false;
  IDirect3DBaseTexture9 *pTex1 = nullptr;
  if (SUCCEEDED(device->GetTexture(1, &pTex1)) && pTex1 != nullptr) {
    isWorld = true;
    pTex1->Release();
  }

  if (isWorld) {
    if (g_Wallhack) {
      device->SetRenderState(D3DRS_ZENABLE, FALSE);
      if (g_Transparency < 1.0f) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(
            D3DRS_TEXTUREFACTOR,
            D3DCOLOR_ARGB((int)(g_Transparency * 255), 255, 255, 255));
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
      }
    }
    if (g_Wireframe)
      device->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    if (g_Fullbright) {
      device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    }
  }

  HRESULT hr = oDrawPrimitiveUP(device, PrimitiveType, PrimitiveCount,
                                pVertexStreamZeroData, VertexStreamZeroStride);

  if (isWorld) {
    if (g_Wallhack) {
      device->SetRenderState(D3DRS_ZENABLE, TRUE);
      if (g_Transparency < 1.0f) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      }
    }
    if (g_Wireframe)
      device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    if (g_Fullbright) {
      device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
      device->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    }
  }
  return hr;
}

long __stdcall Hooked_SetTexture(IDirect3DDevice9 *device, DWORD Stage,
                                 IDirect3DBaseTexture9 *pTexture) {
  return oSetTexture(device, Stage, pTexture);
}

void UpdateObjectList() {
  static unsigned int frameCount = 0;
  frameCount++;

  if (!g_ItemESP && !g_3DESP && !g_ShowMenu && g_TrackedEntityID == -1) {
    g_ObjectCount = 0;
    return;
  }

  // Update info about already cached items every frame for smooth movement
  if (g_ObjectCount > 0) {
    __try {
      for (int i = 0; i < g_ObjectCount; i++) {
        uintptr_t itemPtr = g_Objects[i].ptr;
        if (itemPtr && itemPtr > 0x10000) {
          g_Objects[i].x = *(float *)(itemPtr + 0);
          g_Objects[i].y = *(float *)(itemPtr + 4);
          g_Objects[i].z = *(float *)(itemPtr + 8);
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  // Only perform the full 8000-slot array scan every 30 frames to save CPU
  if (frameCount % 30 != 0 && g_ObjectCount > 0)
    return;

  g_ObjectCount = 0;

  uintptr_t base = g_SS2Base;
  uintptr_t *ptr1 = (uintptr_t *)(base + 0x63AC94);
  __try {
    if (ptr1 && *ptr1) {
      uintptr_t *ptr2 = (uintptr_t *)(*ptr1 + 0x18);
      if (ptr2 && *ptr2) {
        uintptr_t *listBase = (uintptr_t *)(*ptr2 + 0x4);
        if (listBase) {
          uintptr_t *items = listBase;
          // scan entire list
          for (int i = 0; i < MAX_OBJECTS; i++) {
            uintptr_t itemPtr = items[i];
            if (itemPtr && itemPtr > 0x10000) { // Basic validity check
              float x = *(float *)(itemPtr + 0);
              float y = *(float *)(itemPtr + 4);
              float z = *(float *)(itemPtr + 8);
              if (x != 0.0f || y != 0.0f || z != 0.0f) {
                g_Objects[g_ObjectCount].x = x;
                g_Objects[g_ObjectCount].y = y;
                g_Objects[g_ObjectCount].z = z;
                g_Objects[g_ObjectCount].ptr = itemPtr;
                g_Objects[g_ObjectCount].id = i;
                g_ObjectCount++;
                if (g_ObjectCount >= MAX_OBJECTS)
                  break;
              }
            }
          }
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void CleanUpHooks() {}

void UpdatePlayerData() {
  InitSS2Base();
  uintptr_t base = g_SS2Base;
  UpdateObjectList();
  uintptr_t *playerPtrAddr = (uintptr_t *)(base + 0x3320);
  if (playerPtrAddr && *playerPtrAddr) {
    uintptr_t playerPtr = *playerPtrAddr;
    __try {
      g_Player.x = *(float *)(playerPtr + 0x24);
      g_Player.y = *(float *)(playerPtr + 0x28);
      g_Player.z = *(float *)(playerPtr + 0x2c);
      g_Player.playerUpDn = *(short *)(playerPtr + 0x16);
      g_Player.playerLftRt = *(short *)(playerPtr + 0x38);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  uintptr_t *cameraPtrAddr = (uintptr_t *)(base + 0x1A6FF0);
  if (cameraPtrAddr && *cameraPtrAddr) {
    uintptr_t cameraPtr = *cameraPtrAddr;
    __try {
      g_Player.camX = *(float *)(cameraPtr);
      g_Player.camY = *(float *)(cameraPtr + 4);
      g_Player.camZ = *(float *)(cameraPtr + 8);
      g_Player.camRoll = *(short *)(cameraPtr + 0x14);
      g_Player.camUpDn = *(short *)(cameraPtr + 0x16);
      g_Player.camLftRt = *(short *)(cameraPtr + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  // Pre-calculate view constants
  float yaw =
      -(float)g_Player.camLftRt * (2.0f * 3.14159265f / 65536.0f) + 1.5707963f;
  float pitch = (float)g_Player.camUpDn * (3.14159265f / 32768.0f);
  float roll = (float)g_Player.camRoll * (3.14159265f / 32768.0f);

  g_ViewData.sinYaw = sinf(yaw);
  g_ViewData.cosYaw = cosf(yaw);
  g_ViewData.sinPitch = sinf(pitch);
  g_ViewData.cosPitch = cosf(pitch);
  g_ViewData.sinRoll = sinf(roll);
  g_ViewData.cosRoll = cosf(roll);

  float cy = ImGui::GetIO().DisplaySize.y * 0.5f;
  float fovRad = 73.74f * (3.14159265f / 180.0f);
  g_ViewData.fovScale = cy / tanf(fovRad * 0.5f);
}

void DrawRadar() {
  if (!g_ItemESP)
    return;

  ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
  ImGui::Begin("Item Radar", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoScrollbar);

  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 size = ImGui::GetWindowSize();
  ImVec2 center = ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          IM_COL32(30, 30, 30, 150));
  drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    IM_COL32(255, 255, 255, 255));

  // North Indicator
  drawList->AddText(ImVec2(center.x - 5, pos.y + 5),
                    IM_COL32(255, 255, 255, 200), "N");

  // Player Arrow (Cyan)
  // Angle negation to fix Left/Right inversion
  float angle = -(float)g_Player.camLftRt * (2.0f * 3.14159265f / 65536.0f);

  float forwardX = sin(angle);
  float forwardY = -cos(angle);

  ImVec2 p1 = ImVec2(center.x + forwardX * 12.0f, center.y + forwardY * 12.0f);
  ImVec2 p2 = ImVec2(center.x + sin(angle + 2.5f) * 7.0f,
                     center.y - cos(angle + 2.5f) * 7.0f);
  ImVec2 p3 = ImVec2(center.x + sin(angle - 2.5f) * 7.0f,
                     center.y - cos(angle - 2.5f) * 7.0f);
  drawList->AddTriangleFilled(p1, p2, p3, IM_COL32(0, 255, 255, 255));

  float scale = 2.0f;

  for (int i = 0; i < g_ObjectCount; i++) {
    float dx = g_Objects[i].x - g_Player.camX;
    float dy = g_Objects[i].y - g_Player.camY;
    float dz = fabsf(g_Objects[i].z - g_Player.camZ);

    // Height filter - skip expensive map lookups for entities on other floors
    if (dz > 5.0f)
      continue;

    std::string name;
    std::string lowerName;
    EntityCategory cat;

    auto it = g_EntityCache.find(g_Objects[i].ptr);
    if (it != g_EntityCache.end()) {
      name = it->second.name;
      lowerName = it->second.lowerName;
      cat = it->second.cat;
    } else {
      char nameBuf[256];
      if (GetStringProperty(g_Objects[i].id, "ObjShort", nameBuf, 256) &&
          nameBuf[0] != '\0') {
        name = nameBuf;
      } else {
        name = "";
      }
      lowerName = name;
      std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                     ::tolower);
      cat = GetEntityCategory(lowerName);
      g_EntityCache[g_Objects[i].ptr] = {name, lowerName, cat};
    }

    if (!g_3DESPShowNonItems && name.empty())
      continue;

    if (!g_Categories[cat].enabled)
      continue;

    if (!g_SearchFilterLower.empty()) {
      if (lowerName.find(g_SearchFilterLower) == std::string::npos)
        continue;
    }

    // Movement Mapping:
    // Swapping and inverting signs to align with SS2 world space
    float screenX = center.x - dy * scale;
    float screenY = center.y - dx * scale;

    int alpha = (int)(255.0f * (1.0f - (dz / 5.0f)));
    if (alpha < 50)
      alpha = 50;

    if (screenX > pos.x && screenX < pos.x + size.x && screenY > pos.y &&
        screenY < pos.y + size.y) {
      ImU32 color = (g_Categories[cat].color & 0x00FFFFFF) | (alpha << 24);
      float radius = 2.0f;
      drawList->AddCircleFilled(ImVec2(screenX, screenY), radius, color);
    }
  }

  ImGui::End();
}

bool WorldToScreen(const Vec3 &world, ImVec2 &out, float *outDist = nullptr,
                   bool ignoreDistance = false) {
  // World delta
  float dx = world.x - g_Player.camX;
  float dy = world.y - g_Player.camY;
  float dz = world.z - g_Player.camZ;

  float distSq = dx * dx + dy * dy + dz * dz;
  if (!ignoreDistance && distSq > g_ESPDistance * g_ESPDistance)
    return false;

  float dist = sqrtf(distSq);
  if (outDist)
    *outDist = dist;

  // Rotate by yaw (Y axis)
  float x1 = dx * g_ViewData.cosYaw - dy * g_ViewData.sinYaw;
  float y1 = dx * g_ViewData.sinYaw + dy * g_ViewData.cosYaw;
  float z1 = dz;

  // Rotate by pitch (X axis)
  float x2 = x1;
  float y2 = y1 * g_ViewData.cosPitch - z1 * g_ViewData.sinPitch;
  float z2 = y1 * g_ViewData.sinPitch + z1 * g_ViewData.cosPitch;

  // Rotate by roll (Z axis / View Axis)
  float x3 = x2 * g_ViewData.cosRoll - z2 * g_ViewData.sinRoll;
  float z3 = x2 * g_ViewData.sinRoll + z2 * g_ViewData.cosRoll;

  // Behind camera
  if (y2 <= 0.1f)
    return false;

  ImGuiIO &io = ImGui::GetIO();
  float cx = io.DisplaySize.x * 0.5f;
  float cy = io.DisplaySize.y * 0.5f;

  out.x = cx + (x3 / y2) * g_ViewData.fovScale;
  out.y = cy - (z3 / y2) * g_ViewData.fovScale;

  return true;
}

void Draw3DESP(IDirect3DDevice9 *device) {
  if (g_ObjectCount == 0)
    return;

  ImDrawList *draw = ImGui::GetForegroundDrawList();

  for (int i = 0; i < g_ObjectCount; i++) {
    bool isTracked = (g_Objects[i].id == g_TrackedEntityID);
    if (!g_3DESP && !isTracked)
      continue;

    Vec3 obj = {g_Objects[i].x, g_Objects[i].y, g_Objects[i].z};

    ImVec2 screen;
    float dist = 0.0f;
    // Skip expensive map lookups and filtering for entities that are off-screen
    // or out of distance range
    if (!WorldToScreen(obj, screen, &dist, isTracked))
      continue;

    char label[300];
    std::string name;
    std::string lowerName;
    EntityCategory cat;

    auto it = g_EntityCache.find(g_Objects[i].ptr);
    if (it != g_EntityCache.end()) {
      name = it->second.name;
      lowerName = it->second.lowerName;
      cat = it->second.cat;
    } else {
      char nameBuf[256];
      if (GetStringProperty(g_Objects[i].id, "ObjShort", nameBuf, 256) &&
          nameBuf[0] != '\0') {
        name = nameBuf;
      } else {
        name = "";
      }
      lowerName = name;
      std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                     ::tolower);
      cat = GetEntityCategory(lowerName);
      g_EntityCache[g_Objects[i].ptr] = {name, lowerName, cat};
    }

    if (!isTracked && !g_3DESPShowNonItems && name.empty())
      continue;

    if (!isTracked) {
      if (!g_Categories[cat].enabled)
        continue;

      if (!g_SearchFilterLower.empty()) {
        if (lowerName.find(g_SearchFilterLower) == std::string::npos)
          continue;
      }
    }

    if (!name.empty()) {
      _snprintf_s(label, sizeof(label), _TRUNCATE, "%s [%.0fm]", name.c_str(),
                  dist);
    } else if (g_3DESPShowNonItems) {
      _snprintf_s(label, sizeof(label), _TRUNCATE, "%.0fm", dist);
    }

    ImU32 color =
        isTracked ? IM_COL32(255, 255, 255, 255) : g_Categories[cat].color;
    if (isTracked) {
      draw->AddCircle(screen, 8.0f, color, 0, 2.0f);
    }
    draw->AddCircleFilled(screen, 3.0f, color);

    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos = ImVec2(screen.x + 6, screen.y - 6);
    draw->AddRectFilled(
        ImVec2(textPos.x - 2, textPos.y - 1),
        ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 1),
        IM_COL32(0, 0, 0, 120));

    draw->AddText(textPos, IM_COL32(255, 255, 255, 200), label);
  }
}

long __stdcall Hooked_EndScene(IDirect3DDevice9 *device) {
  if (!g_Initialized) {
    D3DDEVICE_CREATION_PARAMETERS parameters;
    device->GetCreationParameters(&parameters);
    window = parameters.hFocusWindow;
    if (window) {
      oWndProc =
          (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
      ImGui::CreateContext();
      ImGuiIO &io = ImGui::GetIO();
      io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
      ImGui_ImplWin32_Init(window);
      ImGui_ImplDX9_Init(device);
      g_Initialized = true;
    }
  }

  D3DVIEWPORT9 vp;
  device->GetViewport(&vp);
  RECT clientRect;
  if (window && GetClientRect(window, &clientRect)) {
    if (vp.Width != (UINT)(clientRect.right - clientRect.left) ||
        vp.Height != (UINT)(clientRect.bottom - clientRect.top))
      return oEndScene(device);
  }

  UpdatePlayerData();

  if (g_Initialized) {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = g_ShowMenu;

    if (g_ShowMenu) {
      ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
      ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);

      ImGui::Begin("SS2 Hack Menu", &g_ShowMenu);
      ImGui::Text("INSERT to hide menu");
      ImGui::Separator();

      if (ImGui::BeginTabBar("Tabs")) {
        if (ImGui::BeginTabItem("General")) {
          ImGui::SeparatorText("GENERAL");
          ImGui::Checkbox("Block Game Input", &g_BlockInput);
          ImGui::SetItemTooltip(
              "Stop keyboard and mouse events from reaching the game.");

          ImGui::SeparatorText("VISUALS");
          ImGui::Checkbox("Wallhack", &g_Wallhack);
          ImGui::SetItemTooltip("See through walls (Z-buffer disabled).");
          if (g_Wallhack) {
            ImGui::SliderFloat("Transparency", &g_Transparency, 0.1f, 1.0f);
            ImGui::SetItemTooltip("Adjust the opacity of world geometry.");
          }
          ImGui::Checkbox("Wireframe", &g_Wireframe);
          ImGui::SetItemTooltip("Render the world as a mesh grid.");
          ImGui::Checkbox("Fullbright", &g_Fullbright);
          ImGui::SetItemTooltip(
              "Disable lightmaps for consistent maximum brightness.");

          ImGui::SeparatorText("RADAR & ESP");
          ImGui::Checkbox("Item Radar", &g_ItemESP);
          ImGui::SetItemTooltip("Show nearby items on the 2D radar overlay.");
          ImGui::Checkbox("Item ESP", &g_3DESP);
          ImGui::SetItemTooltip(
              "Display item names and distances in 3D world space.");
          if (g_3DESP) {
            ImGui::SliderFloat("ESP Distance", &g_ESPDistance, 10.0f, 100.0f);
            ImGui::SetItemTooltip("Maximum distance to display 3D labels.");
          }

          if (g_ItemESP || g_3DESP) {
            ImGui::Checkbox("Show non-items", &g_3DESPShowNonItems);
            ImGui::SetItemTooltip(
                "Display all objects, not just lootable items.");

            ImGui::SeparatorText("FILTERS");
            if (ImGui::InputText("Search", g_SearchFilter,
                                 sizeof(g_SearchFilter))) {
              g_SearchFilterLower = g_SearchFilter;
              std::transform(g_SearchFilterLower.begin(),
                             g_SearchFilterLower.end(),
                             g_SearchFilterLower.begin(), ::tolower);
            }
            ImGui::SetItemTooltip("Filter ESP and Radar by entity name.");

            if (ImGui::TreeNode("Categories")) {
              for (int i = 0; i < CAT_COUNT; i++) {
                ImGui::PushID(i);
                ImVec4 color =
                    ImGui::ColorConvertU32ToFloat4(g_Categories[i].color);
                ImGui::ColorEdit4("##color", (float *)&color,
                                  ImGuiColorEditFlags_NoInputs |
                                      ImGuiColorEditFlags_NoLabel);
                g_Categories[i].color = ImGui::ColorConvertFloat4ToU32(color);
                ImGui::SameLine();
                ImGui::Checkbox(g_Categories[i].name, &g_Categories[i].enabled);
                ImGui::PopID();
              }
              ImGui::TreePop();
            }
          }

          ImGui::SeparatorText("PLAYER STATUS");
          ImGui::Text("X: %.2f Y: %.2f Z: %.2f", g_Player.x, g_Player.y,
                      g_Player.z);

          ImGui::Text("Pitch: %d", g_Player.playerUpDn);
          ImGui::Text("Yaw: %d", g_Player.playerLftRt);

          ImGui::SeparatorText("CAMERA STATUS");
          ImGui::Text("X: %.2f Y: %.2f Z: %.2f", g_Player.camX, g_Player.camY,
                      g_Player.camZ);

          ImGui::Text("Pitch: %d", g_Player.camUpDn);
          ImGui::Text("Yaw: %d", g_Player.camLftRt);
          ImGui::Text("Roll: %d", g_Player.camRoll);
          ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Explorer")) {
          ImGui::SeparatorText("ENTITY EXPLORER");
          ImGui::Checkbox("Hide unnamed", &g_HideEmptyNames);
          ImGui::SetItemTooltip(
              "Exclude entities without a name property from the list.");
          ImGui::SameLine();
          static char explorerFilter[64] = "";
          ImGui::SetNextItemWidth(120.0f);
          ImGui::InputText("Filter##Explorer", explorerFilter, 64);
          ImGui::SetItemTooltip("Filter entities by name.");

          if (explorerFilter[0] != '\0') {
            ImGui::SameLine();
            if (ImGui::Button("X##clearFilter")) {
              explorerFilter[0] = '\0';
            }
            ImGui::SetItemTooltip("Clear search filter.");
          }

          if (g_TrackedEntityID != -1) {
            ImGui::SameLine();
            if (ImGui::Button("Clear Tracking")) {
              g_TrackedEntityID = -1;
            }
            ImGui::SetItemTooltip(
                "Stop tracking the currently selected entity.");
          }

          std::string lowerFilter = explorerFilter;
          std::transform(lowerFilter.begin(), lowerFilter.end(),
                         lowerFilter.begin(), ::tolower);

          // Pre-filter and count entities
          std::vector<int> indices;
          for (int i = 0; i < g_ObjectCount; i++) {
            std::string name;
            auto it = g_EntityCache.find(g_Objects[i].ptr);
            CachedEntity *pCached = nullptr;
            if (it != g_EntityCache.end()) {
              pCached = &it->second;
            } else {
              char nameBuf[256];
              if (GetStringProperty(g_Objects[i].id, "ObjShort", nameBuf,
                                    256) &&
                  nameBuf[0] != '\0') {
                name = nameBuf;
              } else {
                name = "";
              }
              std::string lowerName = name;
              std::transform(lowerName.begin(), lowerName.end(),
                             lowerName.begin(), ::tolower);
              g_EntityCache[g_Objects[i].ptr] = {name, lowerName,
                                                 GetEntityCategory(lowerName)};
              pCached = &g_EntityCache[g_Objects[i].ptr];
            }
            name = pCached->name;

            if (g_HideEmptyNames && name.empty())
              continue;

            if (!lowerFilter.empty()) {
              const std::string &lowerName = pCached->lowerName;
              if (lowerName.find(lowerFilter) == std::string::npos)
                continue;
            }

            indices.push_back(i);
          }

          ImGui::TextDisabled("Showing %d / %d entities", (int)indices.size(),
                              g_ObjectCount);

          if (ImGui::BeginTable(
                  "Entities", 4,
                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                      ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable |
                      ImGuiTableFlags_Sortable)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Cat");
            ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed,
                                    40.0f);
            ImGui::TableSetupColumn("Track",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoSort,
                                    40.0f);
            ImGui::TableHeadersRow();

            if (indices.empty()) {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextDisabled("No entities matching filter.");
              ImGui::EndTable();
              goto end_explorer;
            }

            // Always sort if requested
            if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs()) {
              std::sort(indices.begin(), indices.end(), [&](int a, int b) {
                const auto &spec = sortSpecs->Specs[0];
                int res = 0;

                if (spec.ColumnIndex == 0) { // Name
                  res = g_EntityCache[g_Objects[a].ptr].name.compare(
                      g_EntityCache[g_Objects[b].ptr].name);
                } else if (spec.ColumnIndex == 1) { // Cat
                  res = strcmp(
                      g_Categories[g_EntityCache[g_Objects[a].ptr].cat].name,
                      g_Categories[g_EntityCache[g_Objects[b].ptr].cat].name);
                } else if (spec.ColumnIndex == 2) { // Dist
                  float dxA = g_Objects[a].x - g_Player.camX;
                  float dyA = g_Objects[a].y - g_Player.camY;
                  float dzA = g_Objects[a].z - g_Player.camZ;
                  float distA = dxA * dxA + dyA * dyA + dzA * dzA;

                  float dxB = g_Objects[b].x - g_Player.camX;
                  float dyB = g_Objects[b].y - g_Player.camY;
                  float dzB = g_Objects[b].z - g_Player.camZ;
                  float distB = dxB * dxB + dyB * dyB + dzB * dzB;

                  if (distA < distB)
                    res = -1;
                  else if (distA > distB)
                    res = 1;
                }

                if (spec.SortDirection == ImGuiSortDirection_Descending)
                  return res > 0;
                return res < 0;
              });
            }

            for (int i : indices) {
              std::string name = g_EntityCache[g_Objects[i].ptr].name;
              EntityCategory cat = g_EntityCache[g_Objects[i].ptr].cat;

              float dx = g_Objects[i].x - g_Player.camX;
              float dy = g_Objects[i].y - g_Player.camY;
              float dz = g_Objects[i].z - g_Player.camZ;
              float dist = sqrtf(dx * dx + dy * dy + dz * dz);

              ImGui::TableNextRow();
              bool isTracked = (g_Objects[i].id == g_TrackedEntityID);
              if (isTracked) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(20, 80, 20, 100));
              } else if (dist > g_ESPDistance) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(80, 20, 20, 100));
              }

              ImGui::TableSetColumnIndex(0);
              ImGui::Text("%s", name.empty() ? "(empty)" : name.c_str());

              ImGui::TableSetColumnIndex(1);
              ImGui::Text("%s", g_Categories[cat].name);

              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%.0f", dist);

              ImGui::TableSetColumnIndex(3);
              ImGui::PushID(i + 10000);
              if (ImGui::Checkbox("##track", &isTracked)) {
                g_TrackedEntityID = isTracked ? g_Objects[i].id : -1;
              }
              ImGui::SetItemTooltip(
                  "Highlight this entity in the 3D world and on the radar.");
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
        end_explorer:
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
      ImGui::End();
    }

    DrawRadar();
    Draw3DESP(device);

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
  }
  return oEndScene(device);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
  bool g_Running = true;

  void *pEndScene = GetD3D9VTableFunction(42);
  void *pDrawPrimitiveUP = GetD3D9VTableFunction(83);
  void *pSetTexture = GetD3D9VTableFunction(65);
  if (!pEndScene || !pDrawPrimitiveUP || !pSetTexture)
    return 1;

  MH_Initialize();
  MH_CreateHook(pEndScene, &Hooked_EndScene,
                reinterpret_cast<void **>(&oEndScene));
  MH_CreateHook(pDrawPrimitiveUP, &Hooked_DrawPrimitiveUP,
                reinterpret_cast<void **>(&oDrawPrimitiveUP));
  MH_CreateHook(pSetTexture, &Hooked_SetTexture,
                reinterpret_cast<void **>(&oSetTexture));
  MH_CreateHook(&SetCursorPos, (LPVOID)Hooked_SetCursorPos,
                reinterpret_cast<LPVOID *>(&oSetCursorPos));

  MH_EnableHook(MH_ALL_HOOKS);

  while (g_Running) {
    if (GetAsyncKeyState(VK_END) & 1) {
      g_Running = false;
    }
    Sleep(10);
  }

  // Cleanup
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();

  if (oWndProc) {
    SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
  }

  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  FreeLibraryAndExitThread((HMODULE)lpReserved, 0);

  return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
  }
  return TRUE;
}
