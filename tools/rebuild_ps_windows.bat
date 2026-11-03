@echo off
setlocal
python -c "import sys; assert sys.version_info >= (3,10)" >nul 2>nul
if not errorlevel 1 (
    python "%~dp0build_ps.py" %*
    exit /b
)
py -3 -c "import sys; assert sys.version_info >= (3,10)" >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0build_ps.py" %*
    exit /b
)
echo Python 3.10 or later is required. Run build_ps.py with your Python executable.
exit /b 1
