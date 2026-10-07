@echo off
rem ymfm (BSD 3-Clause, Aaron Giles) を third_party\ymfm へ取得します。
setlocal
set "DEST=%~dp0..\third_party\ymfm"
if exist "%DEST%\src\ymfm_opn.cpp" ( echo 取得済みです: %DEST% & goto :eof )
where git >nul 2>nul || ( echo git が見つかりません。https://github.com/aaronsgiles/ymfm を %DEST% に展開してください。 & pause & goto :eof )
git clone --depth 1 https://github.com/aaronsgiles/ymfm.git "%DEST%"
echo Visual Studio でソリューションを再読み込みしてビルドしてください。
pause
