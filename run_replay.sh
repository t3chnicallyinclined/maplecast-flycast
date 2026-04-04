#!/bin/bash
export MAPLECAST=1
export MAPLECAST_STREAM=1
export MAPLECAST_JPEG=95
export MAPLECAST_REND_REPLAY=1
ROM="$HOME/roms/mvc2_us/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"
cd /home/tris/projects/maplecast-flycast
exec ./build/flycast "$ROM"
