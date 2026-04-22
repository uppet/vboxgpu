@echo off
REM ============================================================
REM  VBox GPU Bridge - Run Guest Only (host must be running)
REM  Usage: man_run_only.bat [test_name]
REM    test_name: triangle | depth | blend | rtt | sortcourt | ultrakill
REM ============================================================

setlocal enabledelayedexpansion

set TEST=%~1
if "%TEST%"=="" set TEST=triangle

set ROOT=S:\bld\vboxgpu
set ICD_DLL=%ROOT%\build\guest_vk_icd\Debug\vbox_vulkan.dll
set TEST_ARGS=

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
goto :selected

:sel_depth
set TEST_DIR=%ROOT%\tests\dx11_depth_test\test_env
set TEST_EXE=dx11_depth_test.exe
goto :selected

:sel_blend
set TEST_DIR=%ROOT%\tests\dx11_multi_blend\test_env
set TEST_EXE=dx11_multi_blend.exe
goto :selected

:sel_rtt
set TEST_DIR=%ROOT%\tests\dx11_rtt\test_env
set TEST_EXE=dx11_rtt.exe
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

copy /Y "%ICD_DLL%" "%TEST_DIR%\vbox_vulkan.dll" >nul

set VK_ICD_FILENAMES=%TEST_DIR%\vbox_icd.json
set VK_LOADER_LAYERS_DISABLE=*

echo Running %TEST_EXE% %TEST_ARGS%
cd /d "%TEST_DIR%"
"%TEST_EXE%" %TEST_ARGS%

echo Guest exited.
endlocal
