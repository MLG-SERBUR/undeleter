#!/bin/bash

# Undeleter Installation Script (user systemd service)
# This script builds the project and sets it up as a user systemd service.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Undeleter Installer ===${NC}"

# 1. Check requirements
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Error: CMake is not installed.${NC}"
    exit 1
fi

if ! command -v make &> /dev/null; then
    echo -e "${RED}Error: make is not installed.${NC}"
    exit 1
fi

if ! command -v g++ &> /dev/null; then
    echo -e "${RED}Error: g++ is not installed.${NC}"
    exit 1
fi

# 2. Check for config.yml
if [ ! -f "config.yml" ]; then
    echo -e "${RED}Error: config.yml not found in the current directory.${NC}"
    echo "Please create config.yml from config.example.yml before running this installer."
    exit 1
fi

# 3. Prepare Service variables
SERVICE_NAME="undeleter"
USER_NAME=$(whoami)
WORK_DIR=$(pwd)
EXEC_FILE="build/undeleter"

# 4. Stop existing service before building
# (Prevents "Text file busy" errors when deleting/overwriting the active binary)
if systemctl --user is-active --quiet "$SERVICE_NAME"; then
    echo -e "${BLUE}Stopping existing user service for update...${NC}"
    systemctl --user stop "$SERVICE_NAME"
fi

# 5. Build the project
echo -e "${BLUE}Cleaning previous build...${NC}"
rm -rf build/ # <--- Fully removes the old build folder so CMake builds completely from scratch

echo -e "${BLUE}Building project with CMake...${NC}"
mkdir -p build
cd build
cmake ..
make
cd ..

# 6. Verify successful build
if [ ! -f "$EXEC_FILE" ]; then
    echo -e "${RED}Error: Executable not found at $EXEC_FILE after build.${NC}"
    exit 1
fi

echo -e "${BLUE}Configuring systemd service: $SERVICE_NAME...${NC}"

# 7. Create the service file content
SERVICE_CONTENT="[Unit]
Description=Undeleter Service
After=network.target

[Service]
WorkingDirectory=$WORK_DIR
ExecStart=$WORK_DIR/$EXEC_FILE
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target"

# 8. Write to user systemd directory
USER_SYSTEMD_DIR="$HOME/.config/systemd/user"
mkdir -p "$USER_SYSTEMD_DIR"

echo "$SERVICE_CONTENT" > "$USER_SYSTEMD_DIR/$SERVICE_NAME.service"

echo -e "${BLUE}Applying systemd configuration...${NC}"
systemctl --user daemon-reload
systemctl --user enable "$SERVICE_NAME"
systemctl --user start "$SERVICE_NAME"

echo -e "${GREEN}=== Installation/Update Complete! ===${NC}"
echo -e "You can view the logs with: ${BLUE}journalctl --user -u $SERVICE_NAME -f${NC}"
echo -e "The service will now start automatically when you log in."
echo -e "${BLUE}Note: To keep the service running after logout, run: sudo loginctl enable-linger $USER_NAME${NC}"
echo -e "${RED}Important: Do not delete or move this folder, as the service runs directly from it.${NC}"