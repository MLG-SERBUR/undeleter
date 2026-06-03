#!/bin/bash

# Exit on error
set -e

echo "Step 1: Installing system dependencies..."
if [ -f /etc/debian_version ]; then
    sudo apt update
    sudo apt install -y cmake g++ libssl-dev zlib1g-dev git
elif [ -f /etc/redhat-release ]; then
    sudo yum install -y cmake gcc-c++ openssl-devel zlib-devel git
else
    echo "Unsupported OS. Please install dependencies manually as per README.md."
    exit 1
fi

echo "Step 2: Installing D++ Library from source..."
# Create a temporary directory for DPP
mkdir -p build/external
cd build/external

if [ -d "DPP" ]; then
    echo "DPP directory already exists, updating..."
    cd DPP
    git pull
else
    git clone https://github.com/brainboxdotcc/DPP.git
    cd DPP
fi

mkdir -p build
cd build
cmake .. -DBUILD_SHARED_LIBS=ON
make -j$(nproc)
sudo make install

echo "Dependencies installed successfully."
