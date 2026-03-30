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
bool g_RadarForwardUp = false;
float g_RadarZoom = 2.0f;
bool g_RadarShowRings = true;
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
        if (itemPtr > 0x10000) {
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
            if (itemPtr > 0x10000) { // Basic validity check
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
  // Optimization: Pre-calculate FOV scale factor to avoid redundant tanf calls
  static const float fovScaleFactor =
      1.0f / tanf(73.74f * (3.14159265f / 180.0f) * 0.5f);
  g_ViewData.fovScale = cy * fovScaleFactor;
}

void DrawRadar() {
  if (!g_ItemESP)
    return;

  ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
  ImGui::Begin("Item Radar", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 size = ImGui::GetWindowSize();
  ImVec2 center = ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          IM_COL32(30, 30, 30, 150));
  drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    IM_COL32(255, 255, 255, 255));

  float angle = -(float)g_Player.camLftRt * (2.0f * 3.14159265f / 65536.0f);

  if (g_RadarShowRings) {
    for (float rDist = 10.0f; rDist <= 100.0f; rDist += 10.0f) {
      float radius = rDist * g_RadarZoom;
      if (radius > size.x * 0.5f && radius > size.y * 0.5f)
        break;
      drawList->AddCircle(center, radius, IM_COL32(100, 100, 100, 80), 64);
    }
  }

  if (!g_RadarForwardUp) {
    drawList->AddText(ImVec2(center.x - 5, pos.y + 5),
                      IM_COL32(255, 255, 255, 200), "N");
    float forwardX = sinf(angle);
    float forwardY = -cosf(angle);
    ImVec2 p1 =
        ImVec2(center.x + forwardX * 12.0f, center.y + forwardY * 12.0f);
    ImVec2 p2 = ImVec2(center.x + sinf(angle + 2.5f) * 7.0f,
                       center.y - cosf(angle + 2.5f) * 7.0f);
    ImVec2 p3 = ImVec2(center.x + sinf(angle - 2.5f) * 7.0f,
                       center.y - cosf(angle - 2.5f) * 7.0f);
    drawList->AddTriangleFilled(p1, p2, p3, IM_COL32(0, 255, 255, 255));
  } else {
    drawList->AddText(ImVec2(center.x - 5, pos.y + 5),
                      IM_COL32(0, 255, 255, 200), "^");
    ImVec2 p1 = ImVec2(center.x, center.y - 12.0f);
    ImVec2 p2 = ImVec2(center.x - 5.0f, center.y - 2.0f);
    ImVec2 p3 = ImVec2(center.x + 5.0f, center.y - 2.0f);
    drawList->AddTriangleFilled(p1, p2, p3, IM_COL32(0, 255, 255, 255));
  }

  ImVec2 mousePos = ImGui::GetIO().MousePos;
  bool isMouseClicked = ImGui::IsMouseClicked(0);
  bool trackedDrawn = false;
  bool trackedFound = false;
  ImVec2 trackedRelPos = {0, 0};

  for (int i = 0; i < g_ObjectCount; i++) {
    bool isTracked = (g_Objects[i].id == g_TrackedEntityID);
    float dx = g_Objects[i].x - g_Player.camX;
    float dy = g_Objects[i].y - g_Player.camY;
    float dz = fabsf(g_Objects[i].z - g_Player.camZ);

    if (dz > 5.0f && !isTracked)
      continue;

    float relX = -dy;
    float relY = -dx;
    if (g_RadarForwardUp) {
      float cosA = cosf(angle);
      float sinA = sinf(angle);
      float nx = relX * cosA + relY * sinA;
      float ny = -relX * sinA + relY * cosA;
      relX = nx;
      relY = ny;
    }

    if (isTracked) {
      trackedRelPos = ImVec2(relX, relY);
      trackedFound = true;
    }

    float screenX = center.x + relX * g_RadarZoom;
    float screenY = center.y + relY * g_RadarZoom;

    if (screenX <= pos.x || screenX >= pos.x + size.x || screenY <= pos.y ||
        screenY >= pos.y + size.y)
      continue;

    if (isTracked)
      trackedDrawn = true;

    const CachedEntity *pEnt = nullptr;
    auto it = g_EntityCache.find(g_Objects[i].ptr);
    if (it != g_EntityCache.end()) {
      pEnt = &it->second;
    } else {
      char nameBuf[256];
      std::string name;
      if (GetStringProperty(g_Objects[i].id, "ObjShort", nameBuf, 256) &&
          nameBuf[0] != '\0') {
        name = nameBuf;
      }
      std::string lowerName = name;
      std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                     ::tolower);
      g_EntityCache[g_Objects[i].ptr] = {name, lowerName,
                                         GetEntityCategory(lowerName)};
      pEnt = &g_EntityCache[g_Objects[i].ptr];
    }

    if (!isTracked) {
      if (!g_3DESPShowNonItems && pEnt->name.empty())
        continue;
      if (!g_Categories[pEnt->cat].enabled)
        continue;
      if (!g_SearchFilterLower.empty() &&
          pEnt->lowerName.find(g_SearchFilterLower) == std::string::npos)
        continue;
    }

    if (g_ShowMenu && isMouseClicked) {
      float dSq = (mousePos.x - screenX) * (mousePos.x - screenX) +
                  (mousePos.y - screenY) * (mousePos.y - screenY);
      if (dSq < 64.0f) {
        g_TrackedEntityID = g_Objects[i].id;
      }
    }

    int alpha = (int)(255.0f * (1.0f - (dz / 5.0f)));
    if (alpha < 50)
      alpha = 50;
    if (isTracked)
      alpha = 255;

    ImU32 color = (g_Categories[pEnt->cat].color & 0x00FFFFFF) | (alpha << 24);
    if (isTracked)
      color = IM_COL32(255, 255, 255, 255);

    if (pEnt->cat == CAT_ENEMY) {
      drawList->AddTriangleFilled(ImVec2(screenX, screenY - 4),
                                  ImVec2(screenX - 4, screenY + 3),
                                  ImVec2(screenX + 4, screenY + 3), color);
    } else if (pEnt->cat == CAT_WEAPON || pEnt->cat == CAT_AMMO) {
      drawList->AddRectFilled(ImVec2(screenX - 3, screenY - 3),
                              ImVec2(screenX + 3, screenY + 3), color);
    } else {
      drawList->AddCircleFilled(ImVec2(screenX, screenY), 2.5f, color);
    }

    if (isTracked) {
      drawList->AddCircle(ImVec2(screenX, screenY), 6.0f,
                          IM_COL32(255, 255, 255, 255), 16, 1.0f);
    }
  }

  if (g_TrackedEntityID != -1 && trackedFound && !trackedDrawn) {
    float len = sqrtf(trackedRelPos.x * trackedRelPos.x +
                      trackedRelPos.y * trackedRelPos.y);
    if (len > 0.1f) {
      float dirX = trackedRelPos.x / len;
      float dirY = trackedRelPos.y / len;
      float f = 1000.0f;
      if (fabsf(dirX) > 0.001f)
        f = fminf(f, fabsf((size.x * 0.5f - 10.0f) / dirX));
      if (fabsf(dirY) > 0.001f)
        f = fminf(f, fabsf((size.y * 0.5f - 10.0f) / dirY));

      ImVec2 arrowPos = ImVec2(center.x + dirX * f, center.y + dirY * f);
      drawList->AddCircleFilled(arrowPos, 3.0f, IM_COL32(255, 255, 255, 255));

      ImVec2 p1 = ImVec2(arrowPos.x + dirX * 8, arrowPos.y + dirY * 8);
      ImVec2 p2 = ImVec2(arrowPos.x - dirY * 5, arrowPos.y + dirX * 5);
      ImVec2 p3 = ImVec2(arrowPos.x + dirY * 5, arrowPos.y - dirX * 5);
      drawList->AddTriangleFilled(p1, p2, p3, IM_COL32(255, 255, 255, 255));
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
    const CachedEntity *pEnt = nullptr;

    auto it = g_EntityCache.find(g_Objects[i].ptr);
    if (it != g_EntityCache.end()) {
      pEnt = &it->second;
    } else {
      char nameBuf[256];
      std::string name;
      if (GetStringProperty(g_Objects[i].id, "ObjShort", nameBuf, 256) &&
          nameBuf[0] != '\0') {
        name = nameBuf;
      }
      std::string lowerName = name;
      std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                     ::tolower);
      g_EntityCache[g_Objects[i].ptr] = {name, lowerName,
                                         GetEntityCategory(lowerName)};
      pEnt = &g_EntityCache[g_Objects[i].ptr];
    }

    if (!isTracked && !g_3DESPShowNonItems && pEnt->name.empty())
      continue;

    if (!isTracked) {
      if (!g_Categories[pEnt->cat].enabled)
        continue;

      if (!g_SearchFilterLower.empty()) {
        if (pEnt->lowerName.find(g_SearchFilterLower) == std::string::npos)
          continue;
      }
    }

    if (!pEnt->name.empty()) {
      _snprintf_s(label, sizeof(label), _TRUNCATE, "%s [%.0fm]",
                  pEnt->name.c_str(), dist);
    } else if (g_3DESPShowNonItems) {
      _snprintf_s(label, sizeof(label), _TRUNCATE, "%.0fm", dist);
    }

    ImU32 color = isTracked ? IM_COL32(255, 255, 255, 255)
                            : g_Categories[pEnt->cat].color;
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
          if (g_ItemESP) {
            ImGui::Indent();
            ImGui::Checkbox("Forward-Up Orientation", &g_RadarForwardUp);
            ImGui::SetItemTooltip(
                "Rotate the radar so the top is always where you look.");
            ImGui::SliderFloat("Radar Zoom", &g_RadarZoom, 1.0f, 10.0f,
                               "%.1fx");
            ImGui::SetItemTooltip("Adjust the scale of the radar map.");
            ImGui::Checkbox("Show Range Rings", &g_RadarShowRings);
            ImGui::SetItemTooltip(
                "Draw concentric circles indicating distance.");
            ImGui::Unindent();
          }

          ImGui::Checkbox("Item ESP", &g_3DESP);
          ImGui::SetItemTooltip(
              "Display item names and distances in 3D world space.");
          if (g_3DESP) {
            ImGui::SliderFloat("ESP Distance", &g_ESPDistance, 10.0f, 100.0f,
                               "%.0fm");
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
            ImGui::SameLine();
            if (ImGui::Button("X##Search")) {
              g_SearchFilter[0] = '\0';
              g_SearchFilterLower.clear();
            }
            ImGui::SetItemTooltip("Clear search filter");

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
          ImGui::InputText("Filter##Explorer", explorerFilter, 64);
          ImGui::SetItemTooltip("Filter entities by name.");
          ImGui::SameLine();
          if (ImGui::Button("X##Explorer")) {
            explorerFilter[0] = '\0';
          }
          ImGui::SetItemTooltip("Clear filter");

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

          // Prepare list for display (filtered and pre-calculated for
          // sorting)
          struct TableEntry {
            int index;
            const CachedEntity *pEnt;
            float distSq;
          };
          static std::vector<TableEntry> entries;
          entries.clear();
          entries.reserve(g_ObjectCount);

          for (int i = 0; i < g_ObjectCount; i++) {
            auto it = g_EntityCache.find(g_Objects[i].ptr);
            const CachedEntity *pCached = nullptr;
            if (it != g_EntityCache.end()) {
              pCached = &it->second;
            } else {
              char nameBuf[256];
              std::string name;
              if (GetStringProperty(g_Objects[i].id, "ObjShort", nameBuf,
                                    256) &&
                  nameBuf[0] != '\0') {
                name = nameBuf;
              }
              std::string lowerName = name;
              std::transform(lowerName.begin(), lowerName.end(),
                             lowerName.begin(), ::tolower);
              g_EntityCache[g_Objects[i].ptr] = {name, lowerName,
                                                 GetEntityCategory(lowerName)};
              pCached = &g_EntityCache[g_Objects[i].ptr];
            }

            if (g_HideEmptyNames && pCached->name.empty())
              continue;

            if (!lowerFilter.empty() &&
                pCached->lowerName.find(lowerFilter) == std::string::npos)
              continue;

            float dx = g_Objects[i].x - g_Player.camX;
            float dy = g_Objects[i].y - g_Player.camY;
            float dz = g_Objects[i].z - g_Player.camZ;
            entries.push_back({i, pCached, dx * dx + dy * dy + dz * dz});
          }

          ImGui::Text("Showing %d / %d entities", (int)entries.size(),
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

            // Always sort if requested
            if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs()) {
              const auto &spec = sortSpecs->Specs[0];
              std::sort(entries.begin(), entries.end(),
                        [&](const TableEntry &a, const TableEntry &b) {
                          int res = 0;
                          if (spec.ColumnIndex == 0) { // Name
                            res = a.pEnt->name.compare(b.pEnt->name);
                          } else if (spec.ColumnIndex == 1) { // Cat
                            res = strcmp(g_Categories[a.pEnt->cat].name,
                                         g_Categories[b.pEnt->cat].name);
                          } else if (spec.ColumnIndex == 2) { // Dist
                            if (a.distSq < b.distSq)
                              res = -1;
                            else if (a.distSq > b.distSq)
                              res = 1;
                          }

                          if (spec.SortDirection ==
                              ImGuiSortDirection_Descending)
                            return res > 0;
                          return res < 0;
                        });
            }

            if (entries.empty()) {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextDisabled("No entities matching filter");
            }

            for (const auto &entry : entries) {
              int i = entry.index;
              const CachedEntity *pEnt = entry.pEnt;
              float dist = sqrtf(entry.distSq);

              ImGui::TableNextRow();
              if (dist > g_ESPDistance) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(80, 20, 20, 100));
              }
              if (g_Objects[i].id == g_TrackedEntityID) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(20, 80, 20, 100));
              }

              ImGui::TableSetColumnIndex(0);
              ImGui::Text("%s",
                          pEnt->name.empty() ? "(empty)" : pEnt->name.c_str());

              ImGui::TableSetColumnIndex(1);
              ImGui::Text("%s", g_Categories[pEnt->cat].name);

              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%.0f", dist);

              ImGui::TableSetColumnIndex(3);
              ImGui::PushID(i + 10000);
              bool isTracked = (g_Objects[i].id == g_TrackedEntityID);
              if (ImGui::Checkbox("##track", &isTracked)) {
                g_TrackedEntityID = isTracked ? g_Objects[i].id : -1;
              }
              ImGui::SetItemTooltip(
                  "Highlight this entity in the 3D world and on the radar.");
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
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
