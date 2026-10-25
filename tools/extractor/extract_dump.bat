@echo off
rem Automated PKG and ISO extractor for Bloodborne PC (Windows)
setlocal enabledelayedexpansion

set "SOURCE_FILE=%~1"
set "DEST_DIR=%~2"

if "%SOURCE_FILE%"=="" (
    echo Usage: extract_dump.bat ^<game_dump.pkg^|game_dump.iso^> [destination_directory]
    exit /b 1
)

if not exist "%SOURCE_FILE%" (
    echo Error: Source dump file does not exist: %SOURCE_FILE%
    exit /b 1
)

if "%DEST_DIR%"=="" (
    set "DEST_DIR=%~dp0..\..\games\CUSA03173"
)
if not exist "%DEST_DIR%" mkdir "%DEST_DIR%"

echo ================================================================================
echo [EXTRACT] Initializing dump extraction (Windows)
echo   Source:      %SOURCE_FILE%
echo   Destination: %DEST_DIR%
echo ================================================================================

set "EXT=%~x1"
if /i "%EXT%"==".iso" (
    echo [EXTRACT] Processing Disc Image (ISO)...
    tar -xf "%SOURCE_FILE%" -C "%DEST_DIR%"
    goto check_result
)

if /i "%EXT%"==".pkg" (
    echo [EXTRACT] Processing PlayStation 4 Package (PKG)...
    set "EXTRACTOR=%~dp0pkg_extractor.exe"
    if not exist "!EXTRACTOR!" (
        echo [EXTRACT] Standalone pkg_extractor.exe not found. Downloading ShadPs4Plus binary...
        powershell -Command "Invoke-WebRequest -Uri 'https://github.com/AzaharPlus/shadPS4Plus/releases/download/PKG_EXTRACTOR_1_1/ShadPs4Plus-PkgExtractor-1.1-windows.zip' -OutFile '%TEMP%\pkg_ext_win.zip'; Expand-Archive -Path '%TEMP%\pkg_ext_win.zip' -DestinationPath '%TEMP%\pkg_ext_win' -Force; Copy-Item '%TEMP%\pkg_ext_win\ShadPs4Plus-PkgExtractor-1.1-windows\*' '%~dp0' -Force"
    )
    if exist "!EXTRACTOR!" (
        "!EXTRACTOR!" "%SOURCE_FILE%" "%DEST_DIR%"
    ) else (
        echo Error: Could not obtain pkg_extractor.exe
        exit /b 1
    )
    goto check_result
)

:check_result
echo ================================================================================
for %%T in (CUSA03173 CUSA00900 CUSA00207 CUSA00208 CUSA00299 CUSA01363 CUSA03179 CUSA03014 CUSA03023) do (
    if exist "%DEST_DIR%\%%T\eboot.bin" if exist "%DEST_DIR%\eboot.bin" (
        echo [EXTRACT] Merging nested update files ^(%%T^) into game root...
        xcopy /y /e /q "%DEST_DIR%\%%T\*" "%DEST_DIR%\"
        rd /s /q "%DEST_DIR%\%%T"
    )
)
if exist "%DEST_DIR%\eboot.bin" (
    echo [EXTRACT] SUCCESS: Found eboot.bin in %DEST_DIR%
) else if exist "%DEST_DIR%\CUSA03173\eboot.bin" (
    echo [EXTRACT] SUCCESS: Found eboot.bin in %DEST_DIR%\CUSA03173
) else (
    echo [EXTRACT] Warning: Extraction finished. Check %DEST_DIR% for game files.
)
echo ================================================================================
exit /b 0
