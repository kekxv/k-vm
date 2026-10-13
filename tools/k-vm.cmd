@echo off
setlocal
if exist "%~dp0k-vm.exe" goto bundled
if exist "%~dp0..\out\win32\k-vm.exe" goto direct
if exist "%~dp0..\bazel-bin\k-vm.exe" goto bazel
echo k-vm.exe not found. Build it with: py -3 "%~dp0build_win32.py" 1>&2
exit /b 1
:bundled
"%~dp0k-vm.exe" %*
exit /b %errorlevel%
:direct
"%~dp0..\out\win32\k-vm.exe" %*
exit /b %errorlevel%
:bazel
"%~dp0..\bazel-bin\k-vm.exe" %*
exit /b %errorlevel%
