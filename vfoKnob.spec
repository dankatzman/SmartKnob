# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['vfoKnob.py'],
    pathex=[],
    binaries=[('c:\\Users\\user\\AppData\\Local\\Python\\pythoncore-3.14-64\\DLLs\\tcl86t.dll', '.'), ('c:\\Users\\user\\AppData\\Local\\Python\\pythoncore-3.14-64\\DLLs\\tk86t.dll', '.')],
    datas=[('src', 'src'), ('c:\\Users\\user\\AppData\\Local\\Python\\pythoncore-3.14-64\\tcl\\tcl8.6', 'tcl8.6'), ('c:\\Users\\user\\AppData\\Local\\Python\\pythoncore-3.14-64\\tcl\\tk8.6', 'tk8.6')],
    hiddenimports=['tkinter', 'tkinter.font', 'tkinter.ttk', 'tkinter.filedialog', 'tkinter.messagebox', 'tkinter.colorchooser', 'configparser', 'serial', 'serial.tools.list_ports', 'queue', 'threading', 'time', 'os', 'sys', 'json', 'omnipyrig', 'win32com.client'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='vfoKnob',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
