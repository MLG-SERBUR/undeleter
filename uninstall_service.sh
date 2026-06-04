#!/bin/bash

# Undeleter Uninstallation Script
# This script removes the user systemd service.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SERVICE_NAME="undeleter"
SERVICE_FILE="$HOME/.config/systemd/user/$SERVICE_NAME.service"

echo -e "${BLUE}=== Undeleter Uninstaller ===${NC}"

# 1. Stop and disable the service (FIXED: Check file existence directly)
if [ -f "$SERVICE_FILE" ]; then
    echo -e "${BLUE}Stopping and disabling service: $SERVICE_NAME...${NC}"
    # FIXED: Added || true to disable so set -e doesn't break if already disabled
    systemctl --user stop "$SERVICE_NAME" || true
    systemctl --user disable "$SERVICE_NAME" || true
    
    # 2. Remove the service file
    echo -e "${BLUE}Removing service file...${NC}"
    rm "$SERVICE_FILE"
    
    # 3. Reload systemd
    systemctl --user daemon-reload
    systemctl --user reset-failed
    
    echo -e "${GREEN}=== Uninstallation Complete! ===${NC}"
    echo "The service has been removed from your user session."
else
    echo -e "${RED}User service $SERVICE_NAME not found.${NC}"
fi