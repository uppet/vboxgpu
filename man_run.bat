@echo off
REM ============================================================
REM  VBox GPU Bridge - Manual Run Script
REM  Usage: man_run.bat [test_name] [nohook]
REM    test_name: triangle | depth | blend | rtt | sortcourt
REM    nohook:    run directly without DXVK/ICD (native D3D11)
REM ============================================================

setlocal enabledelayedexpansion

set TEST=%~1
if "%TEST%"=="" set TEST=triangle
set MODE=%~2

set ROOT=S:\bld\vboxgpu
set HOST_EXE=%ROOT%\build\host\Debug\vbox_host_server.exe
set ICD_DLL=%ROOT%\build\guest_vk_icd\Debug\vbox_vulkan.dll
set TEST_ARGS=
set BUILD_EXE=

REM --- Select test via goto ---
if "%TEST%"=="triangle"  goto :sel_triangle
if "%TEST%"=="depth"     goto :sel_depth
if "%TEST%"=="blend"     goto :sel_blend
if "%TEST%"=="rtt"       goto :sel_rtt
if "%TEST%"=="sortcourt" goto :sel_sortcourt
if "%TEST%"=="ultrakill" goto :sel_ultrakill
echo Unknown test: %TEST%
echo Available: triangle, depth, blend, rtt, sortcourt, ultrakill
exit /b 1

:sel_triangle
set TEST_DIR=%ROOT%\tests\dx11_triangle\test_env
set TEST_EXE=dx11_triangle.exe
set BUILD_EXE=%ROOT%\build\tests\dx11_triangle\Debug\dx11_triangle.exe
goto :selected

:sel_depth
set TEST_DIR=%ROOT%\tests\dx11_depth_test\test_env
set TEST_EXE=dx11_depth_test.exe
set BUILD_EXE=%ROOT%\build\tests\dx11_depth_test\Debug\dx11_depth_test.exe
goto :selected

:sel_blend
set TEST_DIR=%ROOT%\tests\dx11_multi_blend\test_env
set TEST_EXE=dx11_multi_blend.exe
set BUILD_EXE=%ROOT%\build\tests\dx11_multi_blend\Debug\dx11_multi_blend.exe
goto :selected

:sel_rtt
set TEST_DIR=%ROOT%\tests\dx11_rtt\test_env
set TEST_EXE=dx11_rtt.exe
set BUILD_EXE=%ROOT%\build\tests\dx11_rtt\Debug\dx11_rtt.exe
goto :selected

:sel_sortcourt
set TEST_DIR=%ROOT%\tests\SortTheCourt
set TEST_EXE=SortTheCourt.exe
set TEST_ARGS=-screen-width 800 -screen-height 600 -screen-fullscreen 0
set ICD_DLL=%ROOT%\build32\guest_vk_icd\Debug\vbox_vulkan.dll
goto :selected

:sel_ultrakill
set TEST_DIR=%ROOT%\tests\UltraKill
set TEST_EXE=ULTRAKILL.exe
set TEST_ARGS=-screen-width 800 -screen-height 600 -screen-fullscreen 0
goto :selected

:selected

REM === NOHOOK mode ===
if /I "%MODE%"=="nohook" (
    if "%BUILD_EXE%"=="" (
        echo nohook not supported for %TEST%
        exit /b 1
    )
    echo [nohook] Running %TEST% directly...
    "%BUILD_EXE%" %TEST_ARGS%
    echo Done.
    exit /b 0
)

REM === Normal mode: DXVK + ICD + Host ===
taskkill /F /IM vbox_host_server.exe >nul 2>&1
taskkill /F /IM %TEST_EXE% >nul 2>&1
REM Kill any Unity child processes (UnityCrashHandler, etc.)
taskkill /F /IM UnityCrashHandler64.exe >nul 2>&1
REM Wait for TCP TIME_WAIT cleanup + process tree teardown
timeout /t 3 /nobreak >nul

echo [1/3] Copying latest ICD DLL...
copy /Y "%ICD_DLL%" "%TEST_DIR%\vbox_vulkan.dll" >nul

echo [2/3] Starting host server...
start "HostServer" /D "%ROOT%" cmd /c "%HOST_EXE% 2>%ROOT%\host_err.txt"
timeout /t 2 /nobreak >nul

set VK_ICD_FILENAMES=%TEST_DIR%\vbox_icd.json
set VK_LOADER_LAYERS_DISABLE=*

echo [3/3] Starting guest: %TEST_EXE% %TEST_ARGS%
cd /d "%TEST_DIR%"
"%TEST_EXE%" %TEST_ARGS%

echo.
echo Guest exited. Stopping host server...
taskkill /F /IM vbox_host_server.exe >nul 2>&1
echo Done.
endlocal
