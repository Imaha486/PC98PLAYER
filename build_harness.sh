#!/bin/sh
set -e
g++ -std=c++17 -O2 -g -DGMPV3_WITH_YMFM -I../ymfm/src core/*.cpp ../ymfm/src/ymfm_opn.cpp ../ymfm/src/ymfm_adpcm.cpp ../ymfm/src/ymfm_ssg.cpp tools/harness.cpp -o tools/harness 2>&1 | grep -E "error" -A3 | head -40
