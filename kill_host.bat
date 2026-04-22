@echo off
taskkill /F /IM vbox_host_server.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo Host server stopped.
) else (
    echo Host server not running.
)
