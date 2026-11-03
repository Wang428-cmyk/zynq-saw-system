@echo off
setlocal
cd /d "%~dp0..\src\upper_computer"
python -c "import sys; assert sys.version_info.major == 3" >nul 2>nul
if not errorlevel 1 goto with_python
py -3 -c "print('PYTHON_OK')" | findstr /C:"PYTHON_OK" >nul 2>nul
if not errorlevel 1 goto with_py
 echo Python 3 is required.
 exit /b 1
:with_python
python server.py --open
exit /b %errorlevel%
:with_py
py -3 server.py --open
exit /b %errorlevel%
