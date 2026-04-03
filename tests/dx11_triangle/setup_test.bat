@echo off
REM Setup test environment: copy DXVK dlls + our ICD into test directory
REM Usage: setup_test.bat [dxvk_x64_dir] [icd_dll_path] [test_exe_path]

set DXVK_DIR=%~1
set ICD_DLL=%~2
set TEST_DIR=%~dp0test_env

if not exist "%TEST_DIR%" mkdir "%TEST_DIR%"

echo Copying DXVK DLLs from %DXVK_DIR%...
copy /y "%DXVK_DIR%\d3d11.dll" "%TEST_DIR%\"
copy /y "%DXVK_DIR%\dxgi.dll" "%TEST_DIR%\"

echo Copying our Vulkan ICD as vulkan-1.dll...
copy /y "%ICD_DLL%" "%TEST_DIR%\vulkan-1.dll"

echo Copying test executable...
copy /y "%~3" "%TEST_DIR%\"

echo.
echo Test environment ready in %TEST_DIR%
echo To run: cd %TEST_DIR% ^& dx11_triangle.exe
