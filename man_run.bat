@echo off
REM ============================================================
REM  VBox GPU Bridge - Manual Run Script
REM  Usage: man_run.bat [test_name]
REM    test_name: triangle | depth | blend | sortcourt (default: triangle)
REM ============================================================

setlocal

set TEST=%~1
if "%TEST%"=="" set TEST=triangle

REM --- Paths ---
set ROOT=S:\bld\vboxgpu
set HOST_EXE=%ROOT%\build\host\Debug\vbox_host_server.exe
set ICD_DLL=%ROOT%\build\guest_vk_icd\Debug\vbox_vulkan.dll

REM --- Select test ---
if "%TEST%"=="triangle" (
    set TEST_DIR=%ROOT%\tests\dx11_triangle\test_env
    set TEST_EXE=dx11_triangle.exe
) else if "%TEST%"=="depth" (
    set TEST_DIR=%ROOT%\tests\dx11_depth_test\test_env
    set TEST_EXE=dx11_depth_test.exe
) else if "%TEST%"=="blend" (
    set TEST_DIR=%ROOT%\tests\dx11_multi_blend\test_env
    set TEST_EXE=dx11_multi_blend.exe
) else if "%TEST%"=="sortcourt" (
    set TEST_DIR=%ROOT%\tests\SortTheCourt
    set TEST_EXE=SortTheCourt.exe
) else (
    echo Unknown test: %TEST%
    echo Available: triangle, depth, blend, sortcourt
    exit /b 1
)

REM --- Update ICD DLL ---
echo [1/3] Copying latest ICD DLL...
copy /Y "%ICD_DLL%" "%TEST_DIR%\vbox_vulkan.dll" >nul

REM --- Set environment ---
set VK_ICD_FILENAMES=%TEST_DIR%\vbox_icd.json
set VK_LOADER_LAYERS_DISABLE=*

REM --- Launch host server ---
echo [2/3] Starting host server...
start "VBox Host Server" /D "%ROOT%" "%HOST_EXE%"

REM Wait for host to initialize
timeout /t 2 /nobreak >nul

REM --- Launch guest ---
echo [3/3] Starting guest: %TEST_EXE%
cd /d "%TEST_DIR%"
"%TEST_EXE%"

REM --- Cleanup: kill host after guest exits ---
echo.
echo Guest exited. Stopping host server...
taskkill /F /IM vbox_host_server.exe >nul 2>&1

echo Done.
endlocal
