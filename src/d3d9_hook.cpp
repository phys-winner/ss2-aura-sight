#include "d3d9_hook.h"

// Helper: Create a dummy window for device creation
HWND GetProcessWindow() {
  WNDCLASSEX wc = {sizeof(WNDCLASSEX)};
  wc.lpfnWndProc = DefWindowProc;
  wc.lpszClassName = TEXT("DummyWindow");
  RegisterClassEx(&wc);
  return CreateWindow(wc.lpszClassName, TEXT("Dummy Window"), 0, 0, 0, 0, 0, 0,
                      0, wc.hInstance, 0);
}

void *GetD3D9VTableFunction(int index) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return nullptr;

  D3DPRESENT_PARAMETERS d3dpp = {};
  d3dpp.Windowed = TRUE;
  d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  d3dpp.hDeviceWindow = GetProcessWindow();

  IDirect3DDevice9 *device = nullptr;
  HRESULT hr =
      d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3dpp.hDeviceWindow,
                        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &device);

  if (FAILED(hr) || !device) {
    d3d->Release();
    return nullptr;
  }

  void **vtable = *reinterpret_cast<void ***>(device);
  void *result = vtable[index];

  device->Release();
  d3d->Release();

  return result;
}
