@echo off
set APP_NAME=vitali.exe
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
cd /d "%~dp0"

set CL=/nologo /errorReport:none /Gm- /GF /GS- /MT /MP /W4 /wd4324 /wd4996
set LINK=/errorReport:none /INCREMENTAL:NO

set CL=%CL% /Ox
rem set CL=%CL% /Od /Zi
rem set LINK=%LINK% /DEBUG

cl.exe puff.c sqlite3.c zrif.c vitali.c /Fe%APP_NAME%
if %ERRORLEVEL% equ 0 echo =^> %APP_NAME%
pause
