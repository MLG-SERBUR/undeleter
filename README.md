# Undeleter Bot

A lightweight Discord bot written in C++ that reposts deleted messages via webhook, preserving the message content.

## Features

- **C++ Implementation**: Minimal RAM usage, efficient performance
- **Message Caching**: Caches messages in memory (configurable limit)
- **Webhook Reposting**: Uses Discord webhooks to repost deleted messages
- **File Persistence**: Optional message persistence to file for survival across restarts
- **Minimal Permissions**: Only requires View Channel and Send Messages permissions
- **Channel Filtering**: Blacklist/whitelist channels and guilds
- **Customizable**: Trash emoji, bot status, and more

## Requirements

### Discord API
- Discord Bot Token (from https://discord.com/developers/applications)
- Discord Webhook URL (or permission to create one)

### Required Permissions for Bot
- **View Channel**: To see messages in channels
- **Send Messages**: To send messages via webhook
- **Message Content Intent**: To read message content (must be enabled in Discord Developer Portal)

### Optional Permissions (if bot creates its own webhook)
- **Manage Webhooks**: Only needed if you want the bot to create webhooks automatically

### Software Requirements
- C++17 compatible compiler
- CMake 3.15+
- D++ library (https://github.com/brainboxdotcc/DPP)
- OpenSSL
- zlib

## Installation

### 1. Install Dependencies

#### On Ubuntu/Debian:
```bash
sudo apt update
sudo apt install -y cmake g++ libssl-dev zlib1g-dev git
```

#### On CentOS/RHEL:
```bash
yum install -y cmake gcc-c++ openssl-devel zlib-devel git
```

### 2. Install D++ Library

```bash
# Clone D++
git clone https://github.com/brainboxdotcc/DPP.git
cd DPP
mkdir build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON
make -j$(nproc)
sudo make install
```

Or on some systems, you can install from package manager:
```bash
# Ubuntu (if available)
sudo apt install libdpp-dev
```

### 3. Build the Bot

```bash
# Clone this repository
git clone https://github.com/your-repo/undeleter-bot.git
cd undeleter-bot

# Create build directory
mkdir build && cd build

# Build
cmake ..
make -j$(nproc)
```

### 4. Configure the Bot

Copy the example config and edit it:
```bash
cp ../config.example.yml config.yml
nano config.yml  # or use your preferred editor
```

Edit `config.yml` with your settings:
- `bot_token`: Your Discord bot token
- `webhook.url`: Your Discord webhook URL
- Other options as desired

### 5. Run the Bot

```bash
./undeleter-bot ../config.yml
```

Or with default config (config.yml in current directory):
```bash
./undeleter-bot
```

## Configuration

See `config.example.yml` for all available options.

### Creating a Webhook

1. Go to your Discord server
2. Go to the channel where you want undeleted messages to appear
3. Click the channel name at the top
4. Click "Integrations" -> "Webhooks" -> "New Webhook"
5. Copy the Webhook URL and paste it into your config

### Discord Developer Portal Setup

1. Go to https://discord.com/developers/applications
2. Create a new application
3. Go to "Bot" tab and click "Add Bot"
4. Copy the bot token
5. Go to "OAuth2" -> "URL Generator"
6. Select "bot" and "applications.commands" scopes
7. Under "Bot Permissions", select:
   - View Channels
   - Send Messages
   - (Optional) Manage Webhooks - only if bot should create webhooks
8. Generate the URL and invite the bot to your server
9. Enable "Message Content Intent" in the Bot settings

## Usage

Once running, the bot will:
1. Listen for new messages and cache them
2. When a message is deleted, it will repost it via webhook with:
   - The webhook username set to the trash emoji + original author's username (e.g., "🗑️Username")
   - The original message content as-is

Example reposted message:
- **Webhook Username:** `🗑️Username`
- **Message Content:** `This was a deleted message`

## File Structure

```
undeleter-bot/
├── CMakeLists.txt          # Build configuration
├── config.example.yml      # Example configuration
├── README.md               # This file
└── src/
    └── main.cpp            # Main bot code
```

## Customization

### Changing the Trash Emoji
Edit `config.yml`:
```yaml
trash_emoji: "🗓️"  # or any other emoji
```

### Changing Cache Size
To reduce RAM usage, lower the cache size:
```yaml
cache:
  max_messages_per_channel: 100  # Lower for less RAM usage
```

### Persisting to File
Enable file persistence to survive restarts:
```yaml
cache:
  persist_to_file: true
  storage_file: "messages.dat"
```

### Channel Filtering
Only monitor specific channels:
```yaml
channels:
  whitelist:
    - 123456789012345678
    - 876543210987654321
```

Exclude specific channels:
```yaml
channels:
  blacklist:
    - 111111111111111111
    - 222222222222222222
```

## Troubleshooting

### Bot doesn't see messages
- Make sure "Message Content Intent" is enabled in Discord Developer Portal
- Make sure the bot has "View Channel" permission in the channel

### Webhook messages don't appear
- Verify the webhook URL is correct
- Make sure the bot has "Send Messages" permission in the webhook's channel
- Check that the webhook hasn't been deleted

### Bot doesn't start
- Check that all dependencies are installed
- Verify the bot token is correct
- Check for errors in the console output

### Messages aren't being undeleted
- The bot can only undelete messages that were sent after it started (or loaded from cache)
- Check that the message cache isn't full (increase max_messages_per_channel)

## Building with Docker

A Dockerfile is provided for containerized deployment:

```bash
# Build the image
docker build -t undeleter-bot .

# Run the bot
docker run -d --name undeleter-bot -v $(pwd)/config.yml:/app/config.yml undeleter-bot
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## License

MIT License

## Credits

- [D++ Library](https://github.com/brainboxdotcc/DPP) - Discord API library for C++
