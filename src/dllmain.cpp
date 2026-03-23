#include "MinHook.h"
#include "d3d9_hook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Hack Settings
bool g_Wallhack = false;
bool g_Wireframe = false;
bool g_Fullbright = false;
bool g_ItemESP = false;
bool g_3DESP = false;
bool g_3DESPShowNonItems = false;
float g_Transparency = 0.5f;
float g_ESPDistance = 50.0f;

// Object Data
struct GameObject {
  float x, y, z;
  bool is_null;
};
#define MAX_OBJECTS 8000
GameObject g_Objects[MAX_OBJECTS];
int g_ObjectCount = 0;

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

  g_ObjectCount = 0;
  if (!g_ItemESP && !g_3DESP)
    return;

  uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
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
            bool is_null = true;
            if (itemPtr && itemPtr > 0x10000) { // Basic validity check
              float x = *(float *)(itemPtr + 0);
              float y = *(float *)(itemPtr + 4);
              float z = *(float *)(itemPtr + 8);
              if (x != 0.0f || y != 0.0f || z != 0.0f) {
                is_null = false;
                g_Objects[g_ObjectCount].x = x;
                g_Objects[g_ObjectCount].y = y;
                g_Objects[g_ObjectCount].z = z;
              }
            }
            g_Objects[g_ObjectCount].is_null = is_null;
            g_ObjectCount++;
            if (g_ObjectCount >= MAX_OBJECTS)
              break;
          }
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void CleanUpHooks() {}

void UpdatePlayerData() {
  uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
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
  UpdateObjectList();

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

    // Height filter
    if (dz > 5.0f)
      continue;

    // Movement Mapping:
    // Swapping and inverting signs to align with SS2 world space
    float screenX = center.x - dy * scale;
    float screenY = center.y - dx * scale;

    int alpha = (int)(150.0f * (1.0f - (dz / 5.0f)));
    if (alpha < 30)
      alpha = 30;

    if (screenX > pos.x && screenX < pos.x + size.x && screenY > pos.y &&
        screenY < pos.y + size.y) {
      ImU32 color = IM_COL32(255, 255, 0, alpha);
      // unsigned char cat = g_Objects[i].category;
      float radius = 2.0f;
      drawList->AddCircleFilled(ImVec2(screenX, screenY), radius, color);
    }
  }

  ImGui::End();
}

bool WorldToScreen(const Vec3 &world, ImVec2 &out) {
  // World delta
  float dx = world.x - g_Player.camX;
  float dy = world.y - g_Player.camY;
  float dz = world.z - g_Player.camZ;

  float dist = sqrtf(dx * dx + dy * dy + dz * dz);
  if (dist > g_ESPDistance)
    return false;

  // Angles
  float yaw =
      -(float)g_Player.camLftRt * (2.0f * 3.14159265f / 65536.0f) + 1.5707963f;
  float pitch = (float)g_Player.camUpDn * (3.14159265f / 32768.0f);
  float roll = (float)g_Player.camRoll * (3.14159265f / 32768.0f);

  float sinYaw = sinf(yaw);
  float cosYaw = cosf(yaw);
  float sinPitch = sinf(pitch);
  float cosPitch = cosf(pitch);
  float sinRoll = sinf(roll); // 🌀
  float cosRoll = cosf(roll); // 🌀

  // Rotate by yaw (Y axis)
  float x1 = dx * cosYaw - dy * sinYaw;
  float y1 = dx * sinYaw + dy * cosYaw;
  float z1 = dz;

  // Rotate by pitch (X axis)
  float x2 = x1;
  float y2 = y1 * cosPitch - z1 * sinPitch;
  float z2 = y1 * sinPitch + z1 * cosPitch;

  // Rotate by roll (Z axis / View Axis)
  float x3 = x2 * cosRoll - z2 * sinRoll;
  float z3 = x2 * sinRoll + z2 * cosRoll;

  // Behind camera
  if (y2 <= 0.1f)
    return false;

  ImGuiIO &io = ImGui::GetIO();
  float cx = io.DisplaySize.x * 0.5f;
  float cy = io.DisplaySize.y * 0.5f;

  float gameFov = 73.74f;
  float fovRad = gameFov * (3.14159265f / 180.0f);
  float scale = cy / tanf(fovRad * 0.5f);

  out.x = cx + (x3 / y2) * scale;
  out.y = cy - (z3 / y2) * scale;

  return true;
}

void Draw3DESP(IDirect3DDevice9 *device) {
  if (!g_3DESP || g_ObjectCount == 0)
    return;

  ImDrawList *draw = ImGui::GetForegroundDrawList();

  for (int i = 0; i < g_ObjectCount; i++) {

    if (g_Objects[i].is_null) {
      continue;
    }
    Vec3 obj = {g_Objects[i].x, g_Objects[i].y, g_Objects[i].z};

    ImVec2 screen;
    if (!WorldToScreen(obj, screen))
      continue;

    float dx = obj.x - g_Player.camX;
    float dy = obj.y - g_Player.camY;
    float dz = obj.z - g_Player.camZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    char label[300];
    char nameBuf[256];
    bool result = GetStringProperty(i, "ObjShort", nameBuf, 256);

    if (result && nameBuf[0] != '\0') {
      sprintf(label, "%s [%.0fm]", nameBuf, dist);
    } else if (g_3DESPShowNonItems) {
      sprintf(label, "%.0fm", dist);
    } else {
      continue;
    }

    ImU32 color = IM_COL32(255, 255, 0, 220);

    draw->AddCircleFilled(screen, 3.0f, color);

    draw->AddText(ImVec2(screen.x + 6, screen.y - 6),
                  IM_COL32(255, 255, 255, 200), label);
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
      ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_Always);

      ImGui::Begin("SS2 Hack Menu", &g_ShowMenu,
                   ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::Text("INSERT to hide menu");
      ImGui::Separator();

      ImGui::SeparatorText("GENERAL");
      ImGui::Checkbox("Block Game Input", &g_BlockInput);
      ImGui::SetItemTooltip("Stop keyboard and mouse events from reaching the game.");

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
      ImGui::SetItemTooltip("Disable lightmaps for consistent maximum brightness.");

      ImGui::SeparatorText("RADAR & ESP");
      ImGui::Checkbox("Item Radar", &g_ItemESP);
      ImGui::SetItemTooltip("Show nearby items on the 2D radar overlay.");
      ImGui::Checkbox("Item ESP", &g_3DESP);
      ImGui::SetItemTooltip("Display item names and distances in 3D world space.");
      if (g_3DESP) {
        ImGui::SliderFloat("ESP Distance", &g_ESPDistance, 10.0f, 100.0f);
        ImGui::SetItemTooltip("Maximum distance to display 3D labels.");
      }

      if (g_ItemESP || g_3DESP) {
        ImGui::Checkbox("Show non-items", &g_3DESPShowNonItems);
        ImGui::SetItemTooltip("Display all objects, not just lootable items.");
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
