@echo off
REM SIREN Web Client -- install and start (Windows)
REM Usage: install.bat [--port 8760] [--host 127.0.0.1] [--https]

setlocal EnableDelayedExpansion

set SCRIPT_DIR=%~dp0
set HOST=127.0.0.1
set PORT=8760
set USE_HTTPS=0

REM Parse optional args
:parse_args
if "%~1"=="" goto args_done
if "%~1"=="--port" (
    set PORT=%~2
    shift & shift
    goto parse_args
)
if "%~1"=="--host" (
    set HOST=%~2
    shift & shift
    goto parse_args
)
if "%~1"=="--https" (
    set USE_HTTPS=1
    shift
    goto parse_args
)
echo Unknown option: %~1
exit /b 1
:args_done

echo === SIREN Web Client installer ===

REM ---- Python virtualenv + deps ----
set VENV_DIR=%SCRIPT_DIR%.venv

if not exist "%VENV_DIR%\" (
    echo [1/4] Creating Python virtual environment...
    python -m venv "%VENV_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to create venv. Is Python 3.11+ installed?
        exit /b 1
    )
)

echo [2/4] Installing Python dependencies...
"%VENV_DIR%\Scripts\pip.exe" install --quiet --upgrade pip
"%VENV_DIR%\Scripts\pip.exe" install --quiet -r "%SCRIPT_DIR%server\requirements.txt"
if errorlevel 1 (
    echo ERROR: pip install failed.
    exit /b 1
)

REM ---- Node / frontend ----
set FRONTEND_DIR=%SCRIPT_DIR%frontend
set STATIC_DIR=%SCRIPT_DIR%server\static

if exist "%FRONTEND_DIR%\package.json" (
    echo [3/4] Building React frontend...
    pushd "%FRONTEND_DIR%"
    call npm ci --silent
    call npm run build --silent
    if not exist "%STATIC_DIR%\" mkdir "%STATIC_DIR%"
    xcopy /E /Y /Q dist\* "%STATIC_DIR%\" >nul
    popd
) else (
    echo [3/4] No frontend found -- skipping build.
    if not exist "%STATIC_DIR%\" mkdir "%STATIC_DIR%"
    if not exist "%STATIC_DIR%\index.html" (
        (
            echo ^<!DOCTYPE html^>
            echo ^<html^>^<head^>^<meta charset="utf-8"^>^<title^>SIREN Web Client^</title^>^</head^>
            echo ^<body^>^<h1^>SIREN Web Client^</h1^>
            echo ^<p^>Frontend not built yet. REST API and WebSocket bridge are running.^</p^>
            echo ^<p^>API: ^<a href="/api/state"^>/api/state^</a^>^</p^>
            echo ^</body^>^</html^>
        ) > "%STATIC_DIR%\index.html"
    )
)

REM ---- SSL certificates (if --https) ----
set SSL_ARGS=
set SCHEME=http
if "%USE_HTTPS%"=="1" (
    where openssl >nul 2>&1
    if errorlevel 1 (
        echo ERROR: openssl not found.
        echo openssl ships with Git for Windows. Add its usr\bin directory to PATH, e.g.:
        echo   set PATH=C:\Program Files\Git\usr\bin;%%PATH%%
        echo Alternatively, use mkcert to generate trusted certificates.
        exit /b 1
    )
    set CERT_DIR=%SCRIPT_DIR%.certs
    if not exist "%CERT_DIR%\" mkdir "%CERT_DIR%"
    if not exist "%CERT_DIR%\cert.pem" if not exist "%CERT_DIR%\key.pem" (
        echo [SSL] Generating self-signed certificate...
        openssl req -x509 -newkey rsa:2048 -nodes ^
            -keyout "%CERT_DIR%\key.pem" ^
            -out "%CERT_DIR%\cert.pem" ^
            -days 365 ^
            -subj "/CN=SIREN" ^
            -addext "subjectAltName=IP:127.0.0.1,DNS:localhost"
        if errorlevel 1 (
            echo ERROR: Failed to generate SSL certificate.
            exit /b 1
        )
    ) else (
        echo [SSL] Using existing certificates in .certs\
    )
    set SSL_ARGS=--ssl-cert "%CERT_DIR%\cert.pem" --ssl-key "%CERT_DIR%\key.pem"
    set SCHEME=https
)

REM ---- Start server ----
echo [4/4] Starting SIREN bridge on %SCHEME%://%HOST%:%PORT% ...
echo.
echo   Open %SCHEME%://%HOST%:%PORT% in your browser
if "%USE_HTTPS%"=="1" (
    echo   Browser will show a certificate warning - click Advanced -^> Proceed. This is normal for self-signed certs.
)
echo   Press Ctrl+C to stop.
echo.

"%VENV_DIR%\Scripts\python.exe" "%SCRIPT_DIR%server\app.py" --host "%HOST%" --port "%PORT%" %SSL_ARGS%
