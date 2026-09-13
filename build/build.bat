@echo off
setlocal enabledelayedexpansion

REM ==============================================================================
REM rootkit/build/build.bat
REM------------------------------------------------------------------------------
REM Build script for the rootkit WDM driver.
REM
REM STEPS:
REM   1. Run xorstr.py to encrypt XORSTR("...") literals in-place
REM   2. Build the WDK project via msbuild
REM   3. (Optional) Test-sign the driver with signtool
REM
REM REQUIREMENTS:
REM   - Windows 10/11 SDK + WDK 10.0.22621
REM   - Visual Studio 2022 / MSBuild
REM   - Python 3.x (for xorstr.py)
REM
REM USAGE:
REM   build.bat
REM
REM OUTPUT:
REM   build\x64\Release\rootkit.sys
REM==============================================================================

set ROOT=%~dp0..
set PY=python

echo [*] ================================================================
echo [*] rootkit build script
echo [*] ================================================================

echo.
echo [*] Step 1/3: Preprocessing string literals (xorstr.py)
%PY% "%~dp0xorstr.py" "%ROOT%\src" || goto :err

echo.
echo [*] Step 2/3: Building driver (msbuild)
msbuild "%ROOT%\build\rootkit.vcxproj" /p:Configuration=Release /p:Platform=x64 /m || goto :err

echo.
echo [*] Step 3/3: Build complete
echo [*] Output: %ROOT%\build\x64\Release\rootkit.sys

REM ---------------------------------------------------------------------------
REM OPTIONAL: Test-sign the driver (requires test signing mode enabled)
REM   bcdedit /set testsigning on
REM   reboot
REM Uncomment the following line to enable:
REM signtool sign /a /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 "%ROOT%\build\x64\Release\rootkit.sys" || goto :err
REM ---------------------------------------------------------------------------

echo.
echo [*] ================================================================
echo [*] Build successful!
echo [*] ================================================================
exit /b 0

:err
echo.
echo [!] ================================================================
echo [!] Build failed
echo [!] ================================================================
exit /b 1