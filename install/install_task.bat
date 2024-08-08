@echo off

:: Check for admin privileges
net session >nul 2>&1
if %errorLevel% == 0 (
    goto :admin
) else (
    echo Requesting administrative privileges...
    goto :elevate
)

:elevate
:: Self-elevate the script if not already running as admin
powershell -Command "Start-Process cmd -Verb RunAs -ArgumentList '/c %~dpnx0 %*'"
exit /b

:admin
echo Running with administrative privileges.

cd %~dp0

mkdir "%LOCALAPPDATA%\ScreamSender" > NUL
copy ScreamSender.exe "%LOCALAPPDATA%\ScreamSender\ScreamSender.exe" > NUL

:: Get user input for IP, port, and multicast option
set /p TARGET_IP=Enter the target IP address: 
set TARGET_PORT=16401
set /p TARGET_PORT=Enter the target port number (default: 16401): 
if not defined TARGET_PORT set TARGET_PORT=16401

set USE_MULTICAST=n
set /p USE_MULTICAST=Use multicast? (y/n, default: n): 
if not defined USE_MULTICAST set USE_MULTICAST=n

:: Prepare the command with parameters
set COMMAND=%%LOCALAPPDATA%%\ScreamSender\ScreamSender.exe %TARGET_IP% %TARGET_PORT%

:: Add multicast option if selected
if /i "%USE_MULTICAST%"=="y" set COMMAND=%COMMAND% -m
:: Create the scheduled task with parameters
schtasks /create /tn "RunScreamSender" /tr "%COMMAND%" /sc onlogon /rl highest /f

if %errorlevel% equ 0 (
    echo Scheduled task created successfully.
    schtasks /run /tn "RunScreamSender"
    echo Scheduled task created successfully with the following parameters:
    echo IP: %TARGET_IP%
    echo Port: %TARGET_PORT%
    echo Multicast: %USE_MULTICAST%
    if %errorlevel% equ 0 (    

        echo Scheduled task created successfully.
        echo Scheduled task started successfully.
    ) else (
        echo Failed to start the scheduled task.
    )
) else (
    echo Failed to create the scheduled task.
)

pause
