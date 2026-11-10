#!/bin/bash
set -e
sudo apt update
sudo apt install -y build-essential git libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libssl-dev
make
