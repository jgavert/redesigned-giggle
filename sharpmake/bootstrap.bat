@echo off

:: set batch file directory as current
pushd "%~dp0\..\external\Sharpmake\"

set SHARPMAKE_EXECUTABLE=Sharpmake.Application\bin\Debug\net6.0\win-x64\Sharpmake.Application.exe

::call CompileSharpmake.bat Sharpmake.Application/Sharpmake.Application.csproj Debug AnyCPU
dotnet build Sharpmake.sln -c Release
if %errorlevel% NEQ 0 goto error
set SHARPMAKE_MAIN="Sharpmake.Main.sharpmake.cs"
if not "%~1" == "" (
    set SHARPMAKE_MAIN="%~1"
)

set SM_CMD=%SHARPMAKE_EXECUTABLE% /sources('%SHARPMAKE_MAIN%') /generateDebugSolution /verbose
echo %SM_CMD%
%SM_CMD%
if %errorlevel% NEQ 0 goto error

goto success

@REM -----------------------------------------------------------------------
:success
COLOR 2F
echo Bootstrap succeeded^!
set ERROR_CODE=0
goto end

@REM -----------------------------------------------------------------------
:error
COLOR 4F
echo Bootstrap failed^!
set ERROR_CODE=1
goto end

@REM -----------------------------------------------------------------------
:end
:: restore caller current directory
popd
exit /b %ERROR_CODE%
