@echo off
setlocal
rem ============================================================================
rem  HSMono - build the Windows DLL and deploy it to the Hailstone runtime:
rem    build-msvc\hailstone\plugins\HSMono\HSMono.dll
rem
rem  Double-click to run. Safe to re-run: it reuses the existing build-native
rem  tree, and MSBuild rebuilds just what changed. Only HSMono.dll is copied:
rem  sentinel.json, hailstone-addon.txt, managed\ and runtime\ in the deploy
rem  folder are never touched. Close Hailstone first - a loaded DLL is locked.
rem ============================================================================
title HSMono - build and deploy

rem This .bat lives in:  plugins\HSMono\hailstone\
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\..\build-native"

rem Prefer the headless tree's SDK (the inventory-era ABI the deployed client
rem has), as the existing build-native tree is configured. The root SDK lacks
rem the calls HSMono is built against.
for %%I in ("%SRC%\..\..\..\build-wt\headless\sdk") do set "SDK=%%~fI"
if not exist "%SDK%\hailstone_plugin.h" (
  for %%I in ("%SRC%\..\..\..\sdk") do set "SDK=%%~fI"
)
rem The client's own fetched ImGui source, when its build tree has it (the SDK's
rem imgui_context contract needs the exact 1.93.0 the client builds); CMake
rem fetches it when the folder is not there.
set "IMGUI="
for %%I in ("%SRC%\..\..\..\build-wt\headless\build-headless\_deps\imgui-src") do if exist "%%~fI\imgui.h" set "IMGUI=%%~fI"
rem Deploy folder:  <repo>\build-msvc\hailstone\plugins\HSMono
for %%I in ("%SRC%\..\..\..\build-msvc\hailstone\plugins\HSMono") do set "DEST=%%~fI"

if not exist "%SDK%\hailstone_plugin.h" (
  echo ERROR: Hailstone SDK not found at:
  echo   %SDK%
  echo This .bat must stay in plugins\HSMono\hailstone\
  goto :fail
)

rem --- Find CMake (PATH first, then the copies Visual Studio 2022 ships) ---
set "CMAKE="
where cmake >nul 2>nul && set "CMAKE=cmake"
if not defined CMAKE for %%E in (Community Professional Enterprise BuildTools) do ^
  if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
    set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE (
  echo ERROR: cmake.exe not found. Install CMake or add it to PATH.
  goto :fail
)

echo.
echo === Configuring (Visual Studio 17 2022, x64) ===
echo     Source : %SRC%
echo     SDK    : %SDK%
echo     Build  : %BUILD%
if defined IMGUI (
  "%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 -DHAILSTONE_SDK="%SDK%" -DHAILSTONE_IMGUI_DIR="%IMGUI%"
) else (
  "%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 -DHAILSTONE_SDK="%SDK%"
)
if errorlevel 1 (
  echo.
  echo ERROR: CMake configure failed.
  goto :fail
)

echo.
echo === Building (Release) ===
"%CMAKE%" --build "%BUILD%" --config Release
if errorlevel 1 (
  echo.
  echo ERROR: build failed - see the compiler errors above. Nothing was copied.
  goto :fail
)

echo.
echo === Building and running tests ===
"%CMAKE%" --build "%BUILD%" --config Release --target ui_bridge_test
if errorlevel 1 (
  echo.
  echo ERROR: test build failed. Nothing was copied.
  goto :fail
)
"%CMAKE%" --build "%BUILD%" --config Release --target embedded_warrior_test
if errorlevel 1 (
  echo.
  echo ERROR: test build failed. Nothing was copied.
  goto :fail
)
rem The two Windows tests run the real Mono runtime against the prepared
rem plugin layout in the deploy folder (runtime\ and managed\sentinel.dll).
rem Without that layout there is nothing to run them against: say so and go on.
if exist "%DEST%\runtime\mono-2.0-sgen.dll" if exist "%DEST%\managed\sentinel.dll" (
  "%BUILD%\Release\ui_bridge_test.exe" "%DEST%\runtime" "%BUILD%\UiSmoke.dll"
  if errorlevel 1 (
    echo.
    echo ERROR: ui_bridge_test failed. Nothing was copied.
    goto :fail
  )
  "%BUILD%\Release\embedded_warrior_test.exe" "%DEST%"
  if errorlevel 1 (
    echo.
    echo ERROR: embedded_warrior_test failed. Nothing was copied.
    goto :fail
  )
) else (
  echo Skipped: no prepared runtime\ + managed\sentinel.dll layout in %DEST%
)
echo (The PowerShell tests in hailstone\tests\ are not run by this .bat;
echo  run them by hand when the managed host changes.)

echo.
echo === Deploying to %DEST% ===
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%BUILD%\Release\HSMono.dll" "%DEST%\" >nul
if errorlevel 1 (
  echo ERROR: could not copy HSMono.dll - is Hailstone still running?
  echo Close hailstone.exe / hailstone_headless.exe and run this again.
  goto :fail
)

echo.
echo === Done ===
echo HSMono.dll copied to:
echo   %DEST%
echo (sentinel.json, managed\ and runtime\ in the deploy folder were left as-is.)
echo Reload it per client: /plugin HSMono unload, then /plugin HSMono load
echo (or restart the client).
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1
