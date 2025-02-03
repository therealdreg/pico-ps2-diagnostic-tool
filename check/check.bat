@echo off
setlocal EnableDelayedExpansion

echo Debug: Starting file check...
set "EXPECTED_SIZE=983077"

if "%~1"=="" (
  echo Debug: No file provided.
  echo Usage: %~nx0 ^<file^>
  pause
  exit /b 1
)
if not exist "%~1" (
  echo Debug: File "%~1" not found.
  pause
  exit /b 1
)
set "FILE=%~1"

echo Debug: Extracting matching lines...
rem Extract lines starting with "unsigned char array_" to a temporary file.
findstr /b /c:"unsigned char array_" "%FILE%" > temp_match.txt
if errorlevel 1 (
  echo Debug: No matching lines found.
  pause
  exit /b 0
)

rem Get the total size (in bytes) of the temporary file.
for %%F in (temp_match.txt) do set "TOTAL_SIZE=%%~zF"

rem Count how many matching lines exist.
for /f %%N in ('find /c /v "" ^< temp_match.txt') do set "MATCH_COUNT=%%N"
echo Debug: Found %MATCH_COUNT% matching line(s) with a total size of %TOTAL_SIZE% bytes.

rem Compute the expected total size.
set /a EXPECTED_TOTAL=%EXPECTED_SIZE% * %MATCH_COUNT%
echo Debug: Expected total size is %EXPECTED_TOTAL% bytes.

if %TOTAL_SIZE% EQU %EXPECTED_TOTAL% (
  echo Debug: OK, all matching lines have the correct size.
) else (
  echo Error: Total matching line size is %TOTAL_SIZE% bytes, expected %EXPECTED_TOTAL% bytes.
)

del temp_match.txt >nul 2>&1
pause
endlocal
exit /b
