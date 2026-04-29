@echo off
REM ============================================================
REM  VBox GPU Bridge - Build & Deploy All
REM  Builds host server, 64-bit ICD, 32-bit ICD, test programs,
REM  then copies everything to test_env directories.
REM ============================================================

setlocal

set ROOT=S:\bld\vboxgpu
set BUILD64=%ROOT%\build
set BUILD32=%ROOT%\build32

echo ============================================================
echo  [1/4] Building host server (64-bit)...
echo ============================================================
cmake --build "%BUILD64%" --config Release --target vbox_host_server
if %ERRORLEVEL% neq 0 ( echo FAILED: host server build & exit /b 1 )

echo.
echo ============================================================
echo  [2/4] Building ICD (64-bit) + tests (non-fatal)...
echo ============================================================
cmake --build "%BUILD64%" --config Release --target vbox_vk_icd
if %ERRORLEVEL% neq 0 ( echo FAILED: 64-bit ICD build & exit /b 1 )
cmake --build "%BUILD64%" --config Release --target dx11_triangle 2>nul
cmake --build "%BUILD64%" --config Release --target dx11_depth_test 2>nul
cmake --build "%BUILD64%" --config Release --target dx11_multi_blend 2>nul
cmake --build "%BUILD64%" --config Release --target dx11_rtt 2>nul

echo.
echo ============================================================
echo  [3/4] Building ICD (32-bit for SortTheCourt)...
echo ============================================================
cmake --build "%BUILD32%" --config Release --target vbox_vk_icd
if %ERRORLEVEL% neq 0 ( echo FAILED: 32-bit ICD build & exit /b 1 )

echo.
echo ============================================================
echo  [4/4] Deploying to test directories...
echo ============================================================

REM --- 64-bit test programs ---
copy /Y "%BUILD64%\tests\dx11_triangle\Release\dx11_triangle.exe"  "%ROOT%\tests\dx11_triangle\test_env\"   >nul
copy /Y "%BUILD64%\tests\dx11_depth_test\Release\dx11_depth_test.exe" "%ROOT%\tests\dx11_depth_test\test_env\" >nul
copy /Y "%BUILD64%\tests\dx11_multi_blend\Release\dx11_multi_blend.exe" "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%BUILD64%\tests\dx11_rtt\Release\dx11_rtt.exe"             "%ROOT%\tests\dx11_rtt\test_env\"        >nul

REM --- 64-bit ICD ---
copy /Y "%BUILD64%\guest_vk_icd\Release\vbox_vulkan.dll" "%ROOT%\tests\dx11_triangle\test_env\"  >nul
copy /Y "%BUILD64%\guest_vk_icd\Release\vbox_vulkan.dll" "%ROOT%\tests\dx11_depth_test\test_env\" >nul
copy /Y "%BUILD64%\guest_vk_icd\Release\vbox_vulkan.dll" "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%BUILD64%\guest_vk_icd\Release\vbox_vulkan.dll" "%ROOT%\tests\dx11_rtt\test_env\"        >nul

REM --- DXVK DLLs (d3d11.dll + dxgi.dll) for all 64-bit test_env ---
set DXVK_SRC=%ROOT%\tests\dx11_triangle\test_env
copy /Y "%DXVK_SRC%\d3d11.dll" "%ROOT%\tests\dx11_depth_test\test_env\"  >nul
copy /Y "%DXVK_SRC%\dxgi.dll"   "%ROOT%\tests\dx11_depth_test\test_env\"  >nul
copy /Y "%DXVK_SRC%\d3d11.dll" "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%DXVK_SRC%\dxgi.dll"   "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%DXVK_SRC%\d3d11.dll" "%ROOT%\tests\dx11_rtt\test_env\"        >nul
copy /Y "%DXVK_SRC%\dxgi.dll"   "%ROOT%\tests\dx11_rtt\test_env\"        >nul
copy /Y "%DXVK_SRC%\vbox_icd.json" "%ROOT%\tests\dx11_depth_test\test_env\"  >nul
copy /Y "%DXVK_SRC%\vbox_icd.json" "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%DXVK_SRC%\vbox_icd.json" "%ROOT%\tests\dx11_rtt\test_env\"        >nul

REM --- 64-bit ICD for UltraKill ---
copy /Y "%BUILD64%\guest_vk_icd\Release\vbox_vulkan.dll" "%ROOT%\tests\UltraKill\"                 >nul

REM --- 32-bit ICD for SortTheCourt ---
copy /Y "%BUILD32%\guest_vk_icd\Release\vbox_vulkan.dll" "%ROOT%\tests\SortTheCourt\"              >nul

echo.
echo ============================================================
echo  All built and deployed successfully.
echo  Run: man_run.bat triangle ^| depth ^| blend ^| rtt ^| sortcourt ^| ultrakill
echo ============================================================

endlocal
