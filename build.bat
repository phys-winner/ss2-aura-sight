@echo off

:: Check if cl.exe is in PATH
where cl.exe >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [!] Error: cl.exe not found. 
    echo Please run this script from a 'Developer Command Prompt for VS'.
    exit /b 1
)

if not exist "build" mkdir "build"

echo Building System Shock 2 Internal DLL...

cl /nologo /O2 /MD /EHsc /LD ^
    /I "deps/imgui" ^
    /I "deps/imgui/backends" ^
    /I "deps/minhook/include" ^
    /I "src" ^
    /D "IMGUI_IMPL_WIN32_DISABLE_GAMEPAD" ^
    src/d3d9_hook.cpp ^
    src/dllmain.cpp ^
    deps/imgui/imgui.cpp ^
    deps/imgui/imgui_draw.cpp ^
    deps/imgui/imgui_widgets.cpp ^
    deps/imgui/imgui_tables.cpp ^
    deps/imgui/imgui_demo.cpp ^
    deps/imgui/backends/imgui_impl_dx9.cpp ^
    deps/imgui/backends/imgui_impl_win32.cpp ^
    deps/minhook/src/buffer.c ^
    deps/minhook/src/hook.c ^
    deps/minhook/src/trampoline.c ^
    deps/minhook/src/hde/hde64.c ^
    deps/minhook/src/hde/hde32.c ^
    /Fo:build/ ^
    /Fe:build/hook.dll ^
    d3d9.lib dxgi.lib user32.lib gdi32.lib dwmapi.lib

if %ERRORLEVEL% equ 0 (
    echo Success: hook.dll created.
) else (
    echo Failure: Build failed with error %ERRORLEVEL%.
)
