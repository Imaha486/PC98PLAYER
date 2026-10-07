#!/bin/sh
# mingw-w64 でのクロスビルド（Visual Studio 用のプロジェクトは別に用意してある）
set -e
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-windres -O coff win32/pc98player_mingw.rc -o out/pc98player_res.o
x86_64-w64-mingw32-g++ -std=c++17 -O2 -DGMPV3_WITH_YMFM -DUNICODE -D_UNICODE -I../ymfm/src \
  core/*.cpp ../ymfm/src/ymfm_opn.cpp ../ymfm/src/ymfm_adpcm.cpp ../ymfm/src/ymfm_ssg.cpp win32/main.cpp out/pc98player_res.o \
  -municode -mwindows -static -o out/PC98PLAYER.EXE -lgdi32 -lwinmm -luser32 -lshell32 -lcomdlg32 -lsetupapi -ladvapi32 -lole32 -luuid
ls -la out/PC98PLAYER.EXE
