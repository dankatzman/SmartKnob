@echo off
echo Setting up vfoKnob environment...

:: Remove stale venv if it was created on a different machine
if exist .venv\Scripts\python.exe (
    .venv\Scripts\python.exe -c "import sys; exit(0)" 2>nul
    if errorlevel 1 (
        echo Removing stale virtual environment...
        rmdir /s /q .venv
    )
)

:: Create venv if it doesn't exist
if not exist .venv (
    echo Creating virtual environment...
    python -m venv .venv
)

:: Install dependencies
echo Installing dependencies...
.venv\Scripts\pip install -r requirements.txt

echo.
echo Setup complete!
pause
