@echo off

:: Run setup if venv is missing or broken
if not exist .venv (
    call setup.bat
) else (
    .venv\Scripts\python.exe -c "import sys; exit(0)" 2>nul
    if errorlevel 1 call setup.bat
)

:: Start the app
.venv\Scripts\python.exe vfoKnob.py
