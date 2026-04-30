@echo off
REM ============================================================
REM  VBox GPU Bridge - Build ALL Release targets
REM  Host server + ICD 64-bit + ICD 32-bit
REM ============================================================
setlocal enabledelayedexpansion

set ROOT=S:\bld\vboxgpu

echo [1/3] Host server (64-bit Release)...
cmake --build %ROOT%\build --config Release --target vbox_host_server
if !ERRORLEVEL! neq 0 ( echo FAILED & exit /b 1 )

echo [2/3] ICD 64-bit (Release)...
cmake --build %ROOT%\build --config Release --target vbox_vk_icd
if !ERRORLEVEL! neq 0 ( echo FAILED & exit /b 1 )

echo [3/3] ICD 32-bit (Release)...
cmake --build %ROOT%\build32 --config Release --target vbox_vk_icd
if !ERRORLEVEL! neq 0 ( echo FAILED & exit /b 1 )

echo.
echo === ALL OK ===
echo   Host:     %ROOT%\build\host\Release\vbox_host_server.exe
echo   ICD x64:  %ROOT%\build\guest_vk_icd\Release\vbox_vulkan.dll
echo   ICD x86:  %ROOT%\build32\guest_vk_icd\Release\vbox_vulkan.dll
endlocal
