@echo off
REM fake-gh.bat —— 让 CreateProcessW 能"直接执行"的 gh 桩入口。
REM GitRT 用 CreateProcessW 起进程，它不能执行 .ps1；.bat 由系统 shell 关联执行。
REM 参数一路透传给 fake-gh.ps1（-Argv 用 RemainingArgs 收集）。
pwsh -NoProfile -File "%~dp0fake-gh.ps1" %*
