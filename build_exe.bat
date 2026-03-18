@echo off
REM Build vfoKnob.exe with all required dependencies

REM Activate virtual environment (optional, if not already active)
call .venv\Scripts\activate.bat

REM Run PyInstaller build
.venv\Scripts\pyinstaller --onefile --noconsole --name smartknob --add-data "src;src" --add-binary "c:\Users\user\AppData\Local\Python\pythoncore-3.14-64\DLLs\tcl86t.dll;." --add-binary "c:\Users\user\AppData\Local\Python\pythoncore-3.14-64\DLLs\tk86t.dll;." --add-data "c:\Users\user\AppData\Local\Python\pythoncore-3.14-64\tcl\tcl8.6;tcl8.6" --add-data "c:\Users\user\AppData\Local\Python\pythoncore-3.14-64\tcl\tk8.6;tk8.6" --hidden-import=tkinter --hidden-import=tkinter.font --hidden-import=tkinter.ttk --hidden-import=tkinter.filedialog --hidden-import=tkinter.messagebox --hidden-import=tkinter.colorchooser --hidden-import=configparser --hidden-import=serial --hidden-import=serial.tools.list_ports --hidden-import=queue --hidden-import=threading --hidden-import=time --hidden-import=os --hidden-import=sys --hidden-import=json --hidden-import=omnipyrig --hidden-import=win32com.client vfoKnob.py

REM Copy all required files to dist
copy /Y app_state.ini dist\
copy /Y radio_profiles.ini dist\
if exist calibration_profiles (
	xcopy /E /I /Y calibration_profiles dist\calibration_profiles\
)
if exist src\img (
	xcopy /E /I /Y src\img dist\img\
)
for %%f in (*.ini) do copy /Y "%%f" dist\

REM The new exe and all needed files will be in the dist folder
pause
