@echo off
setlocal

rem Run from an x86 Native Tools Command Prompt.
rem UNICODE must not be defined.
rem Use VS2005 or change the subsystem to 5.1 or above.

rc /nologo /fo cake32.res cake32.rc
if errorlevel 1 exit /b 1

cl /nologo /TC /c /O1 /W3 /GS- /Zl cake32.c /Fo:cake32.obj
if errorlevel 1 exit /b 1

link /nologo /NODEFAULTLIB /MACHINE:X86 /SUBSYSTEM:WINDOWS,3.10 /ENTRY:CakeEntry /BASE:0x00400000 /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO /STACK:32768,4096 cake32.obj 
cake32.res kernel32.lib user32.lib gdi32.lib /OUT:CAKE32.exe
if errorlevel 1 exit /b 1

echo Built CAKE32.exe
endlocal