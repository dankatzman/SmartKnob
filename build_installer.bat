@echo off
echo ============================================
echo  Smart Knob - Build Installer
echo ============================================
echo.

echo [1/3] Compiling Python to EXE with Nuitka...
set PYTHONPATH=%~dp0src
set OUTPUT_DIR=%~dp0build

"%~dp0.venv\Scripts\python.exe" -m nuitka ^
    --standalone ^
    --onefile ^
    --windows-console-mode=disable ^
    --output-filename=smartknob.exe ^
    --output-dir="%OUTPUT_DIR%" ^
    --assume-yes-for-downloads ^
    --enable-plugin=tk-inter ^
    --include-module=app ^
    --include-module=gui_monitor ^
    --include-module=protocol ^
    --include-module=rig_adapter ^
    --include-module=serial_transport ^
    --include-module=radio_poller ^
    --include-module=version ^
    --include-data-files="%~dp0radio_profiles.ini=radio_profiles.ini" ^
    --include-data-files="%~dp0legalHFfreq.txt=legalHFfreq.txt" ^
    "%~dp0vfoKnob.py"

if errorlevel 1 (
    echo.
    echo ERROR: Nuitka compilation failed!
    pause
    exit /b 1
)

echo.
echo [2/3] Copying EXE and data files to dist folder...
copy /y "%OUTPUT_DIR%\smartknob.exe" "%~dp0dist\smartknob.exe"
copy /y "%~dp0legalHFfreq.txt" "%~dp0dist\legalHFfreq.txt"
copy /y "%~dp0radio_profiles.ini" "%~dp0dist\radio_profiles.ini"

echo.
echo [3/3] Building installer with Inno Setup...
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "%~dp0SmartKnobSetup.iss"

if errorlevel 1 (
    echo.
    echo ERROR: Inno Setup compilation failed!
    pause
    exit /b 1
)

echo.
echo ============================================
echo  Done! Installer is in the Output folder.
echo ============================================
pause
