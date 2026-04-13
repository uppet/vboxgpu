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
cmake --build "%BUILD64%" --config Debug --target vbox_host_server
if %ERRORLEVEL% neq 0 ( echo FAILED: host server build & exit /b 1 )

echo.
echo ============================================================
echo  [2/4] Building ICD + tests (64-bit)...
echo ============================================================
cmake --build "%BUILD64%" --config Debug --target vbox_vk_icd
if %ERRORLEVEL% neq 0 ( echo FAILED: 64-bit ICD build & exit /b 1 )
cmake --build "%BUILD64%" --config Debug --target dx11_triangle
if %ERRORLEVEL% neq 0 ( echo FAILED: dx11_triangle build & exit /b 1 )
cmake --build "%BUILD64%" --config Debug --target dx11_depth_test
if %ERRORLEVEL% neq 0 ( echo FAILED: dx11_depth_test build & exit /b 1 )
cmake --build "%BUILD64%" --config Debug --target dx11_multi_blend
if %ERRORLEVEL% neq 0 ( echo FAILED: dx11_multi_blend build & exit /b 1 )
cmake --build "%BUILD64%" --config Debug --target dx11_rtt
if %ERRORLEVEL% neq 0 ( echo FAILED: dx11_rtt build & exit /b 1 )

echo.
echo ============================================================
echo  [3/4] Building ICD (32-bit for SortTheCourt)...
echo ============================================================
cmake --build "%BUILD32%" --config Debug --target vbox_vk_icd
if %ERRORLEVEL% neq 0 ( echo FAILED: 32-bit ICD build & exit /b 1 )

echo.
echo ============================================================
echo  [4/4] Deploying to test directories...
echo ============================================================

REM --- 64-bit test programs ---
copy /Y "%BUILD64%\tests\dx11_triangle\Debug\dx11_triangle.exe"  "%ROOT%\tests\dx11_triangle\test_env\"   >nul
copy /Y "%BUILD64%\tests\dx11_depth_test\Debug\dx11_depth_test.exe" "%ROOT%\tests\dx11_depth_test\test_env\" >nul
copy /Y "%BUILD64%\tests\dx11_multi_blend\Debug\dx11_multi_blend.exe" "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%BUILD64%\tests\dx11_rtt\Debug\dx11_rtt.exe"             "%ROOT%\tests\dx11_rtt\test_env\"        >nul

REM --- 64-bit ICD ---
copy /Y "%BUILD64%\guest_vk_icd\Debug\vbox_vulkan.dll" "%ROOT%\tests\dx11_triangle\test_env\"  >nul
copy /Y "%BUILD64%\guest_vk_icd\Debug\vbox_vulkan.dll" "%ROOT%\tests\dx11_depth_test\test_env\" >nul
copy /Y "%BUILD64%\guest_vk_icd\Debug\vbox_vulkan.dll" "%ROOT%\tests\dx11_multi_blend\test_env\" >nul
copy /Y "%BUILD64%\guest_vk_icd\Debug\vbox_vulkan.dll" "%ROOT%\tests\dx11_rtt\test_env\"        >nul

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
copy /Y "%BUILD64%\guest_vk_icd\Debug\vbox_vulkan.dll" "%ROOT%\tests\UltraKill\"                 >nul

REM --- 32-bit ICD for SortTheCourt ---
copy /Y "%BUILD32%\guest_vk_icd\Debug\vbox_vulkan.dll" "%ROOT%\tests\SortTheCourt\"              >nul

echo.
echo ============================================================
echo  All built and deployed successfully.
echo  Run: man_run.bat triangle ^| depth ^| blend ^| rtt ^| sortcourt ^| ultrakill
echo ============================================================

endlocal
