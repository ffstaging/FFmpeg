@echo off
setlocal EnableDelayedExpansion
REM
REM ffmpeg-sg - FFmpeg Show-Graph Wrapper (aka killer feature)
REM             Show the FFmpeg execution graph in default browser 
REM
REM Copyright (c) 2025 softworkz
REM
REM This file is part of FFmpeg.
REM
REM FFmpeg is free software; you can redistribute it and/or
REM modify it under the terms of the GNU Lesser General Public
REM License as published by the Free Software Foundation; either
REM version 2.1 of the License, or (at your option) any later version.
REM
REM FFmpeg is distributed in the hope that it will be useful,
REM but WITHOUT ANY WARRANTY; without even the implied warranty of
REM MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
REM Lesser General Public License for more details.
REM
REM You should have received a copy of the GNU Lesser General Public
REM License along with FFmpeg; if not, write to the Free Software
REM Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
REM

REM Check for ffmpeg.exe in folder
if not exist "ffmpeg.exe" (
    echo Error: ffmpeg.exe not found in current directory.
    exit /b 1
)

REM Check params
set "conflict_found="
for %%i in (%*) do (
    if /i "%%i"=="-print_graphs_file" set "conflict_found=1"
    if /i "%%i"=="-print_graphs_format" set "conflict_found=1"
)

if defined conflict_found (
    echo Error: -print_graphs_file or -print_graphs_format parameter already provided.
    echo This script manages graph file generation automatically.
    exit /b 1
)

REM Validate temp dir
if not exist "%TEMP%" (
    echo Error: Temp directory not accessible
    exit /b 1
)

REM Generate HTML filename
set "date_part=%date:~-4,4%-%date:~-10,2%-%date:~-7,2%"
set "time_part=%time:~0,2%-%time:~3,2%-%time:~6,2%"
set "date_part=%date_part:/=-%"
set "time_part=%time_part: =0%"

set "html_file=%TEMP%\ffmpeg_graph_%date_part%_%time_part%_%RANDOM%.html"

REM Execute ffmpeg
REM Use start /wait /b to avoid "Terminate batch job" prompt on Ctrl-C
start /wait /b ffmpeg.exe -print_graphs_file "%html_file%" -print_graphs_format mermaidhtml %*
set "ffmpeg_exit_code=%ERRORLEVEL%"

REM Open browser if HTML file was created
if exist "%html_file%" (
    echo "Execution graph opened in browser: %html_file%
    start "FFmpeg Graph" "%html_file%"
) else (
    echo Warning: FFmpeg completed but no graph file was generated.
)

REM Exit with ffmpeg exit code
exit /b %ffmpeg_exit_code%
