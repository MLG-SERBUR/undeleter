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

echo -e "${BLUE}=== Undeleter Uninstaller ===${NC}"

# 1. Stop and disable the service
if systemctl --user list-unit-files | grep -q "$SERVICE_NAME.service"; then
    echo -e "${BLUE}Stopping and disabling service: $SERVICE_NAME...${NC}"
    systemctl --user stop "$SERVICE_NAME" || true
    systemctl --user disable "$SERVICE_NAME"
    
    # 2. Remove the service file
    echo -e "${BLUE}Removing service file...${NC}"
    rm "$HOME/.config/systemd/user/$SERVICE_NAME.service"
    
    # 3. Reload systemd
    systemctl --user daemon-reload
    systemctl --user reset-failed
    
    echo -e "${GREEN}=== Uninstallation Complete! ===${NC}"
    echo "The service has been removed from your user session."
else
    echo -e "${RED}User service $SERVICE_NAME not found.${NC}"
fi
