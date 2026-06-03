/**
 * Undeleter Bot for Discord
 * =========================
 * A lightweight C++ bot that reposts deleted messages via webhook.
 * 
 * Uses D++ library: https://github.com/brainboxdotcc/DPP
 * 
 * Features:
 * - Caches messages in memory (configurable limit)
 * - Detects message deletions
 * - Reposts via webhook with original author name and trash emoji
 * - Optional message persistence to file
 * - Minimal RAM usage (C++)
 * - Minimal required permissions
 * 
 * Required Discord Intents:
 * - GUILD_MESSAGES
 * - MESSAGE_CONTENT
 */

#include <dpp/dpp.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <ctime>
#include <algorithm>
#include <csignal>
#include <functional>
#include <cctype>

using namespace dpp;

// Configuration structure
struct Config {
    std::string bot_token;
    std::string webhook_url;  // Default webhook URL
    std::string trash_emoji = "🗑️";
    int max_messages_per_channel = 500;
    bool persist_to_file = false;
    std::string storage_file = "messages.dat";
    std::string activity_type = "watching";
    std::string activity_name = "for deleted messages";
    std::vector<snowflake> guild_whitelist;
    std::vector<snowflake> guild_blacklist;
    std::vector<snowflake> channel_blacklist;
    std::vector<snowflake> channel_whitelist;
    // Per-channel webhooks: channel_id -> webhook_url
    std::unordered_map<snowflake, std::string> channel_webhooks;
    // Skip undeleting messages from users with these role IDs
    std::vector<snowflake> skip_role_ids;
    // Skip undeleting if user has any of these permissions
    // Default: skip users with manage_messages or administrator
    permission skip_permissions = p_manage_messages;
    // Auto-create webhooks for channels that don't have one
    bool auto_create_webhooks = false;
};

// Cached message structure
struct CachedMessage {
    snowflake id;
    snowflake channel_id;
    snowflake guild_id;
    snowflake author_id;
    std::string author_name;
    std::string content;
    time_t timestamp;
    bool skip_undelete = false;  // True if this message should not be undeleted (e.g., from admin)
};

// Global configuration
Config config;

// Message cache: channel_id -> vector of messages
std::unordered_map<snowflake, std::vector<CachedMessage>> message_cache;
std::mutex cache_mutex;

// Get the cluster for webhook execution
cluster* bot_cluster = nullptr;

// Undeleter toggle state (not persisted, in-memory only)
bool undelete_enabled = true;
std::mutex undelete_mutex;

/**
 * Split a string by delimiter
 */
std::vector<std::string> split_string(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream token_stream(str);
    while (std::getline(token_stream, token, delimiter)) {
        // Trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

/**
 * Trim whitespace from a string
 */
std::string trim(const std::string& str) {
    std::string result = str;
    result.erase(0, result.find_first_not_of(" \t"));
    result.erase(result.find_last_not_of(" \t") + 1);
    return result;
}

/**
 * Remove surrounding quotes from a string
 */
std::string unquote(const std::string& str) {
    std::string result = str;
    if (result.size() >= 2 && result.front() == '"' && result.back() == '"') {
        result = result.substr(1, result.size() - 2);
    }
    if (result.size() >= 2 && result.front() == '\'' && result.back() == '\'') {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

/**
 * Parse a list of snowflake IDs from a string
 */
std::vector<snowflake> parse_snowflake_list(const std::string& value) {
    std::vector<snowflake> result;
    // Handle both comma-separated and YAML list format
    std::string trimmed = trim(unquote(value));
    
    // Check if it's a YAML list (starts with -)
    if (trimmed.find('-') != std::string::npos) {
        // Parse YAML list format
        std::istringstream iss(trimmed);
        std::string line;
        while (std::getline(iss, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '-') {
                std::string id = trim(line.substr(1));
                try {
                    result.push_back(std::stoull(id));
                } catch (...) {
                    std::cerr << "Invalid ID in list: " << id << std::endl;
                }
            }
        }
    } else {
        // Parse comma-separated format
        auto ids = split_string(trimmed, ',');
        for (auto& id : ids) {
            try {
                result.push_back(std::stoull(id));
            } catch (...) {
                std::cerr << "Invalid ID in list: " << id << std::endl;
            }
        }
    }
    return result;
}

/**
 * Load configuration from YAML file
 */
bool load_config(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open config file: " << filename << std::endl;
        return false;
    }

    std::string line;
    std::string current_section;
    std::string current_key;
    
    while (std::getline(file, line)) {
        line = trim(line);
        
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        // Check for section (e.g., [webhook] or guilds:)
        if (line[0] == '[') {
            size_t end_bracket = line.find(']');
            if (end_bracket != std::string::npos) {
                current_section = line.substr(1, end_bracket - 1);
                current_key.clear();
                continue;
            }
        }
        
        // Check for YAML-style section (e.g., "webhook:" or "cache:")
        if (line.back() == ':') {
            std::string section = trim(line.substr(0, line.size() - 1));
            // If we're in a section and encounter a sub-section, build the full key path
            if (!current_section.empty()) {
                current_key = current_section + "." + section;
            } else {
                current_section = section;
            }
            continue;
        }
        
        // Handle channel_webhooks map (special case)
        if (current_section == "channel_webhooks" && !line.empty() && line[0] != '#') {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string channel_id_str = trim(line.substr(0, colon_pos));
                std::string webhook_url = trim(unquote(line.substr(colon_pos + 1)));
                try {
                    snowflake channel_id = std::stoull(channel_id_str);
                    config.channel_webhooks[channel_id] = webhook_url;
                } catch (...) {
                    std::cerr << "Invalid channel ID in channel_webhooks: " << channel_id_str << std::endl;
                }
            }
            continue;
        }
        
        // Parse key-value pairs
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            // Maybe a list item (starts with -)
            if (!line.empty() && line[0] == '-') {
                std::string id = trim(line.substr(1));
                if (current_section == "guild_blacklist") {
                    try { config.guild_blacklist.push_back(std::stoull(id)); } catch (...) {}
                } else if (current_section == "guild_whitelist") {
                    try { config.guild_whitelist.push_back(std::stoull(id)); } catch (...) {}
                } else if (current_section == "channel_blacklist") {
                    try { config.channel_blacklist.push_back(std::stoull(id)); } catch (...) {}
                } else if (current_section == "channel_whitelist") {
                    try { config.channel_whitelist.push_back(std::stoull(id)); } catch (...) {}
                }
            }
            continue;
        }
        
        std::string key = trim(line.substr(0, colon_pos));
        std::string value = trim(line.substr(colon_pos + 1));
        value = unquote(value);
        
        // Build full key path
        std::string full_key;
        if (!current_key.empty()) {
            full_key = current_key + "." + key;
        } else if (!current_section.empty()) {
            full_key = current_section + "." + key;
        } else {
            full_key = key;
        }
        
        // Process the key-value pair
        if (full_key == "bot_token" || full_key == "token") {
            config.bot_token = value;
        } else if (full_key == "webhook_url") {
            config.webhook_url = value;
        } else if (full_key == "trash_emoji") {
            config.trash_emoji = value;
        } else if (full_key == "cache_max_messages" || full_key == "cache.max_messages_per_channel") {
            try { config.max_messages_per_channel = std::stoi(value); }
            catch (...) { std::cerr << "Invalid cache_max_messages: " << value << std::endl; }
        } else if (full_key == "cache_persist" || full_key == "cache.persist_to_file") {
            config.persist_to_file = (value == "true" || value == "1" || value == "yes");
        } else if (full_key == "cache_file" || full_key == "cache.storage_file") {
            config.storage_file = value;
        } else if (full_key == "activity_type") {
            config.activity_type = value;
        } else if (full_key == "activity_name") {
            config.activity_name = value;
        } else if (key == "channel_webhooks") {
            // Parse channel_id: webhook_url pairs
            // Format: "channel_id: webhook_url" on each line
            // Skip for now, would need YAML map parsing
        } else if (key == "auto_create_webhooks") {
            config.auto_create_webhooks = (value == "true" || value == "1" || value == "yes");
        } else if (key == "skip_role_ids") {
            auto ids = parse_snowflake_list(value);
            config.skip_role_ids.insert(config.skip_role_ids.end(), ids.begin(), ids.end());
        } else if (key == "skip_permissions") {
            // Parse permission flags - supports multiple comma-separated values
            // Common: administrator, manage_messages, kick_members, ban_members, manage_guild
            std::string lower_value = value;
            std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
            
            permission perms = p_none;
            
            if (lower_value.find("administrator") != std::string::npos || 
                lower_value.find("admin") != std::string::npos) {
                perms = perms | p_administrator;
            }
            if (lower_value.find("manage_messages") != std::string::npos ||
                lower_value.find("manage msg") != std::string::npos) {
                perms = perms | p_manage_messages;
            }
            if (lower_value.find("kick_members") != std::string::npos ||
                lower_value.find("kick") != std::string::npos) {
                perms = perms | p_kick_members;
            }
            if (lower_value.find("ban_members") != std::string::npos ||
                lower_value.find("ban") != std::string::npos) {
                perms = perms | p_ban_members;
            }
            if (lower_value.find("manage_guild") != std::string::npos ||
                lower_value.find("manage server") != std::string::npos) {
                perms = perms | p_manage_guild;
            }
            
            if (perms != p_none) {
                config.skip_permissions = perms;
            }
        } else if (full_key == "guild_blacklist" || key == "guild_blacklist") {
            auto ids = parse_snowflake_list(value);
            config.guild_blacklist.insert(config.guild_blacklist.end(), ids.begin(), ids.end());
        } else if (full_key == "guild_whitelist" || key == "guild_whitelist") {
            auto ids = parse_snowflake_list(value);
            config.guild_whitelist.insert(config.guild_whitelist.end(), ids.begin(), ids.end());
        } else if (full_key == "channel_blacklist" || key == "channel_blacklist") {
            auto ids = parse_snowflake_list(value);
            config.channel_blacklist.insert(config.channel_blacklist.end(), ids.begin(), ids.end());
        } else if (full_key == "channel_whitelist" || key == "channel_whitelist") {
            auto ids = parse_snowflake_list(value);
            config.channel_whitelist.insert(config.channel_whitelist.end(), ids.begin(), ids.end());
        } else if (key == "guild_blacklist" || key == "guild_whitelist" || 
                   key == "channel_blacklist" || key == "channel_whitelist") {
            // These are handled as sections, but we might get them as keys with empty values
        } else {
            // Unknown key, skip
            // std::cerr << "Unknown config key: " << full_key << std::endl;
        }
    }
    
    // Validate required configuration
    if (config.bot_token.empty()) {
        std::cerr << "Error: bot_token is required in config file" << std::endl;
        return false;
    }
    
    return true;
}

/**
 * Load cached messages from file
 */
void load_messages_from_file() {
    if (!config.persist_to_file) return;
    
    std::ifstream file(config.storage_file, std::ios::binary);
    if (!file.is_open()) {
        std::cout << "No cached messages file found, starting fresh." << std::endl;
        return;
    }
    
    try {
        // Check file version (first byte)
        uint8_t version = 0;
        if (!file.read(reinterpret_cast<char*>(&version), sizeof(version))) {
            std::cerr << "Error reading message cache: invalid format" << std::endl;
            return;
        }
        
        // For now, version 0 is the old format, version 1 is new format with skip_undelete
        // We'll just load what we can
        file.seekg(0, std::ios::beg); // Rewind
        
        size_t channel_count;
        if (!file.read(reinterpret_cast<char*>(&channel_count), sizeof(channel_count))) {
            std::cerr << "Error reading message cache: invalid format" << std::endl;
            return;
        }
        
        for (size_t i = 0; i < channel_count; i++) {
            snowflake channel_id;
            if (!file.read(reinterpret_cast<char*>(&channel_id), sizeof(channel_id))) {
                std::cerr << "Error reading message cache: truncated file" << std::endl;
                return;
            }
            
            size_t message_count;
            if (!file.read(reinterpret_cast<char*>(&message_count), sizeof(message_count))) {
                std::cerr << "Error reading message cache: truncated file" << std::endl;
                return;
            }
            
            std::vector<CachedMessage> messages;
            for (size_t j = 0; j < message_count; j++) {
                CachedMessage msg;
                
                if (!file.read(reinterpret_cast<char*>(&msg.id), sizeof(msg.id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.channel_id), sizeof(msg.channel_id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.guild_id), sizeof(msg.guild_id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.author_id), sizeof(msg.author_id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.timestamp), sizeof(msg.timestamp))) break;
                
                // Try to read skip_undelete flag (new format)
                if (!file.read(reinterpret_cast<char*>(&msg.skip_undelete), sizeof(msg.skip_undelete))) {
                    msg.skip_undelete = false; // Default to false for old files
                    file.clear(); // Clear error flag
                    // Rewind a bit and try to continue
                    file.seekg(-static_cast<std::streamoff>(sizeof(msg.timestamp)), std::ios::cur);
                }
                
                uint32_t author_name_len;
                if (!file.read(reinterpret_cast<char*>(&author_name_len), sizeof(author_name_len))) break;
                msg.author_name.resize(author_name_len);
                if (!file.read(&msg.author_name[0], author_name_len)) break;
                
                uint32_t content_len;
                if (!file.read(reinterpret_cast<char*>(&content_len), sizeof(content_len))) break;
                msg.content.resize(content_len);
                if (!file.read(&msg.content[0], content_len)) break;
                
                messages.push_back(msg);
            }
            
            if (!messages.empty()) {
                message_cache[channel_id] = messages;
                std::cout << "Loaded " << messages.size() << " cached messages for channel " << channel_id << std::endl;
            }
        }
        std::cout << "Loaded message cache with " << message_cache.size() << " channels." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading messages from file: " << e.what() << std::endl;
    }
}

/**
 * Save cached messages to file
 */
void save_messages_to_file() {
    if (!config.persist_to_file) return;
    
    std::ofstream file(config.storage_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open storage file for writing: " << config.storage_file << std::endl;
        return;
    }
    
    try {
        // Write version byte (1 = current format with skip_undelete)
        uint8_t version = 1;
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        
        size_t channel_count = message_cache.size();
        file.write(reinterpret_cast<const char*>(&channel_count), sizeof(channel_count));
        
        size_t total_messages = 0;
        for (auto& [channel_id, messages] : message_cache) {
            file.write(reinterpret_cast<const char*>(&channel_id), sizeof(channel_id));
            
            size_t message_count = messages.size();
            file.write(reinterpret_cast<const char*>(&message_count), sizeof(message_count));
            total_messages += message_count;
            
            for (auto& msg : messages) {
                file.write(reinterpret_cast<const char*>(&msg.id), sizeof(msg.id));
                file.write(reinterpret_cast<const char*>(&msg.channel_id), sizeof(msg.channel_id));
                file.write(reinterpret_cast<const char*>(&msg.guild_id), sizeof(msg.guild_id));
                file.write(reinterpret_cast<const char*>(&msg.author_id), sizeof(msg.author_id));
                file.write(reinterpret_cast<const char*>(&msg.timestamp), sizeof(msg.timestamp));
                file.write(reinterpret_cast<const char*>(&msg.skip_undelete), sizeof(msg.skip_undelete));
                
                uint32_t author_name_len = static_cast<uint32_t>(msg.author_name.size());
                file.write(reinterpret_cast<const char*>(&author_name_len), sizeof(author_name_len));
                file.write(msg.author_name.c_str(), author_name_len);
                
                uint32_t content_len = static_cast<uint32_t>(msg.content.size());
                file.write(reinterpret_cast<const char*>(&content_len), sizeof(content_len));
                file.write(msg.content.c_str(), content_len);
            }
        }
        std::cout << "Saved " << total_messages << " messages to cache file." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error saving messages to file: " << e.what() << std::endl;
    }
}

/**
 * Check if a channel is allowed (not blacklisted, or in whitelist)
 */
bool is_channel_allowed(snowflake channel_id, snowflake guild_id) {
    // Check guild blacklist
    if (std::find(config.guild_blacklist.begin(), config.guild_blacklist.end(), guild_id) 
        != config.guild_blacklist.end()) {
        return false;
    }
    
    // Check guild whitelist (if non-empty)
    if (!config.guild_whitelist.empty()) {
        if (std::find(config.guild_whitelist.begin(), config.guild_whitelist.end(), guild_id)
            == config.guild_whitelist.end()) {
            return false;
        }
    }
    
    // Check channel blacklist
    if (std::find(config.channel_blacklist.begin(), config.channel_blacklist.end(), channel_id)
        != config.channel_blacklist.end()) {
        return false;
    }
    
    // Check channel whitelist (if non-empty)
    if (!config.channel_whitelist.empty()) {
        if (std::find(config.channel_whitelist.begin(), config.channel_whitelist.end(), channel_id)
            == config.channel_whitelist.end()) {
            return false;
        }
    }
    
    return true;
}

/**
 * Check if a guild member has manage_messages permission
 */
bool has_manage_messages(const message_create_t& event) {
    // Can't check permissions in DMs
    if (event.msg.guild_id == 0) {
        return false;
    }
    
    // Need member info
    if (!event.msg.member) {
        return false;
    }
    
    // Check for manage_messages permission
    if ((event.msg.member.permissions & p_manage_messages) != p_none) {
        return true;
    }
    
    // Check for administrator (implies all permissions)
    if ((event.msg.member.permissions & p_administrator) != p_none) {
        return true;
    }
    
    return false;
}

/**
 * Check if a user should be skipped (e.g., admin, moderator)
 */
bool should_skip_user(const message_create_t& event) {
    // Always skip bots
    if (event.msg.author.is_bot()) {
        return true;
    }
    
    // If it's a DM, don't skip
    if (event.msg.guild_id == 0) {
        return false;
    }
    
    // If no member info (shouldn't happen in guild), don't skip
    if (!event.msg.member) {
        return false;
    }
    
    // Check if user has any skip roles
    if (!config.skip_role_ids.empty()) {
        for (auto role_id : config.skip_role_ids) {
            if (std::find(event.msg.member.roles.begin(), event.msg.member.roles.end(), role_id)
                != event.msg.member.roles.end()) {
                return true;
            }
        }
    }
    
    // Check if user has skip permissions
    // Note: p_administrator implies all other permissions, so we check it specifically
    if (config.skip_permissions != p_none) {
        if ((event.msg.member.permissions & config.skip_permissions) != p_none) {
            return true;
        }
        // Also check for administrator if specific permission is requested
        if (config.skip_permissions == p_administrator && 
            (event.msg.member.permissions & p_administrator) != p_none) {
            return true;
        }
    }
    
    return false;
}

/**
 * Cache a message
 */
void cache_message(const message_create_t& event) {
    // Ignore empty messages
    if (event.msg.content.empty()) {
        return;
    }
    
    // Check if channel is allowed
    if (!is_channel_allowed(event.msg.channel_id, event.msg.guild_id)) {
        return;
    }
    
    // Check if we should skip this user
    bool skip = should_skip_user(event);
    
    CachedMessage msg;
    msg.id = event.msg.id;
    msg.channel_id = event.msg.channel_id;
    msg.guild_id = event.msg.guild_id;
    msg.author_id = event.msg.author.id;
    msg.author_name = event.msg.author.username;
    msg.content = event.msg.content;
    msg.timestamp = time(nullptr);
    msg.skip_undelete = skip;
    
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    // Check if we already have this message
    auto& channel_messages = message_cache[event.msg.channel_id];
    for (auto& existing : channel_messages) {
        if (existing.id == msg.id) {
            return; // Already cached
        }
    }
    
    // Add to cache
    channel_messages.push_back(msg);
    
    // Trim cache if too large
    if (channel_messages.size() > static_cast<size_t>(config.max_messages_per_channel)) {
        channel_messages.erase(channel_messages.begin());
    }
}

/**
 * Find a cached message by ID in a specific channel
 */
CachedMessage* find_cached_message(snowflake message_id, snowflake channel_id) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    auto it = message_cache.find(channel_id);
    if (it == message_cache.end()) {
        return nullptr;
    }
    
    for (auto& msg : it->second) {
        if (msg.id == message_id) {
            return &msg;
        }
    }
    
    return nullptr;
}

/**
 * Remove a message from cache
 */
void remove_cached_message(snowflake message_id, snowflake channel_id) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    
    auto it = message_cache.find(channel_id);
    if (it == message_cache.end()) {
        return;
    }
    
    auto& messages = it->second;
    messages.erase(
        std::remove_if(messages.begin(), messages.end(),
            [message_id](const CachedMessage& msg) { return msg.id == message_id; }
        ),
        messages.end()
    );
    
    // Clean up empty channels
    if (messages.empty()) {
        message_cache.erase(it);
    }
}

/**
 * Extract webhook ID and token from URL
 */
std::pair<std::string, std::string> parse_webhook_url(const std::string& url) {
    size_t api_pos = url.find("/api/webhooks/");
    if (api_pos == std::string::npos) {
        return {"", ""};
    }
    
    std::string rest = url.substr(api_pos + 15); // Length of "/api/webhooks/"
    size_t slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) {
        return {"", ""};
    }
    
    return {rest.substr(0, slash_pos), rest.substr(slash_pos + 1)};
}

/**
 * Execute a webhook message
 */
void execute_webhook(const std::string& webhook_url, const webhook_message& wm) {
    auto [webhook_id_str, token] = parse_webhook_url(webhook_url);
    
    if (webhook_id_str.empty() || token.empty()) {
        std::cerr << "Invalid webhook URL format: " << webhook_url << std::endl;
        return;
    }
    
    try {
        snowflake webhook_id = std::stoull(webhook_id_str);
        
        bot_cluster->execute_webhook(webhook_id, token, true, wm,
            [](const http_request_completion_t& completion) {
                if (completion.status != http_status_code::h_204 && 
                    completion.status != http_status_code::h_200) {
                    std::cerr << "Webhook error: HTTP " 
                              << static_cast<int>(completion.status) 
                              << " - " << http_status_code_text(completion.status)
                              << std::endl;
                } else {
                    std::cout << "Successfully reposted deleted message via webhook" << std::endl;
                }
            }
        );
    } catch (const std::exception& e) {
        std::cerr << "Error executing webhook: " << e.what() << std::endl;
    }
}

/**
 * Get the webhook URL for a specific channel
 * Returns the channel-specific webhook if configured, otherwise the default
 */
std::string get_webhook_url(snowflake channel_id) {
    // Check for channel-specific webhook
    auto it = config.channel_webhooks.find(channel_id);
    if (it != config.channel_webhooks.end()) {
        return it->second;
    }
    
    // Return default webhook
    return config.webhook_url;
}

/**
 * Create a webhook for a channel
 * Requires Manage Webhooks permission
 */
void create_webhook_for_channel(snowflake channel_id, snowflake guild_id, const std::function<void(const webhook)&) &callback) {
    if (guild_id == 0) {
        // Can't create webhooks in DMs
        return;
    }
    
    // Create webhook with a name
    bot_cluster->current_user_create_webhook(guild_id, channel_id, "Undeleter", "",
        [channel_id, callback](const confirmation_callback_t& response) {
            if (response.is_error()) {
                std::cerr << "Failed to create webhook for channel " << channel_id 
                          << ": " << response.get_error().message << std::endl;
                return;
            }
            
            // The response contains the webhook object
            webhook wh = response.get<webhook>();
            
            // Store the webhook URL
            std::string webhook_url = "https://discord.com/api/webhooks/" + 
                                      std::to_string(wh.id) + "/" + wh.token;
            
            // Store in config
            config.channel_webhooks[channel_id] = webhook_url;
            
            std::cout << "Created webhook for channel " << channel_id 
                      << ": " << wh.id << std::endl;
            
            if (callback) {
                callback(wh);
            }
        }
    );
}

/**
 * Get or create a webhook for a channel
 * If auto_create_webhooks is enabled and no webhook exists, creates one
 */
void get_or_create_webhook(snowflake channel_id, snowflake guild_id, 
                          const std::function<void(const std::string&)>& callback) {
    // Check if we already have a webhook for this channel
    std::string existing_url = get_webhook_url(channel_id);
    if (!existing_url.empty()) {
        callback(existing_url);
        return;
    }
    
    // If auto-create is enabled, create a webhook
    if (config.auto_create_webhooks && guild_id != 0) {
        create_webhook_for_channel(channel_id, guild_id, 
            [callback, channel_id](const webhook& wh) {
                std::string webhook_url = "https://discord.com/api/webhooks/" + 
                                          std::to_string(wh.id) + "/" + wh.token;
                callback(webhook_url);
            }
        );
        return;
    }
    
    // No webhook available
    callback("");
}

/**
 * Repost a deleted message via webhook
 */
void repost_message(const CachedMessage& msg) {
    // Check if undeleter is enabled
    {
        std::lock_guard<std::mutex> lock(undelete_mutex);
        if (!undelete_enabled) {
            std::cout << "Undeleter is disabled, skipping repost for message " 
                      << msg.id << " from " << msg.author_name << std::endl;
            return;
        }
    }
    
    // Skip if this message should not be undeleted (e.g., from admin/mod)
    if (msg.skip_undelete) {
        std::cout << "Skipping undelete for privileged user message from " 
                  << msg.author_name << " (has manage messages permission)" << std::endl;
        return;
    }
    
    // Get or create webhook for this channel
    get_or_create_webhook(msg.channel_id, msg.guild_id, 
        [msg](const std::string& webhook_url) {
            if (webhook_url.empty()) {
                std::cerr << "No webhook URL configured/available for channel " 
                          << msg.channel_id << ", cannot repost message" << std::endl;
                return;
            }
            
            // Prepare the message content - just the original message
            std::string content = msg.content;
            
            // Create webhook message with trash emoji + username as the webhook username
            webhook_message wm;
            wm.content = content;
            wm.username = config.trash_emoji + msg.author_name;
            
            // Execute the webhook
            execute_webhook(webhook_url, wm);
        }
    );
}

/**
 * Check if a user has manage_messages permission from a message
 */
bool user_has_manage_messages(const message_create_t& event) {
    return has_manage_messages(event);
}

/**
 * Check if a user has manage_messages from an interaction
 */
bool user_has_manage_messages(const interaction_create_t& event) {
    // Check if this is a guild interaction
    if (event.command.guild_id == 0) {
        return false;
    }
    
    // Get the member
    if (event.command.member.permissions & p_manage_messages) {
        return true;
    }
    if (event.command.member.permissions & p_administrator) {
        return true;
    }
    
    return false;
}

/**
 * Handle slash command for toggling undeleter
 */
void on_interaction_create(const interaction_create_t& event) {
    if (event.command.name == "undelete") {
        // Check permissions
        if (!user_has_manage_messages(event)) {
            // Respond with error (ephemeral)
            interaction_response resp;
            resp.type = ir_channel_message_with_source;
            resp.content = "❌ You do not have permission to use this command. (Require Manage Messages permission)";
            resp.flags = m_ephemeral;
            event.from->interaction_response_create(event.command.id, event.command.token, resp);
            return;
        }
        
        // Handle subcommands - check options
        std::string subcommand = "status"; // default
        for (const auto& opt : event.command.options) {
            if (opt.name == "toggle" || opt.name == "off") {
                subcommand = "off";
                break;
            } else if (opt.name == "on") {
                subcommand = "on";
                break;
            } else if (opt.name == "status") {
                subcommand = "status";
                break;
            }
        }
        
        if (subcommand == "toggle" || subcommand == "off") {
            std::lock_guard<std::mutex> lock(undelete_mutex);
            undelete_enabled = false;
            
            interaction_response resp;
            resp.type = ir_channel_message_with_source;
            resp.content = "✅ Undeleter is now **DISABLED**. Deleted messages will not be reposted.";
            resp.flags = m_ephemeral;
            event.from->interaction_response_create(event.command.id, event.command.token, resp);
        } else if (subcommand == "on") {
            std::lock_guard<std::mutex> lock(undelete_mutex);
            undelete_enabled = true;
            
            interaction_response resp;
            resp.type = ir_channel_message_with_source;
            resp.content = "✅ Undeleter is now **ENABLED**. Deleted messages will be reposted.";
            resp.flags = m_ephemeral;
            event.from->interaction_response_create(event.command.id, event.command.token, resp);
        } else {
            // status command
            interaction_response resp;
            resp.type = ir_channel_message_with_source;
            resp.content = "Undeleter is currently **" + std::string(undelete_enabled ? "ENABLED" : "DISABLED") + "**.\n"
                      "Use `/undelete toggle` or `/undelete off` to disable, `/undelete on` to enable.";
            resp.flags = m_ephemeral;
            event.from->interaction_response_create(event.command.id, event.command.token, resp);
        }
    }
}

/**
 * Register slash commands
 */
void register_commands(cluster& bot) {
    // Create slash command with subcommands
    command cmd;
    cmd.name = "undelete";
    cmd.description = "Manage the undeleter bot";
    cmd.type = ct_chat_input;
    
    // Add subcommands
    command_option toggle_opt;
    toggle_opt.name = "toggle";
    toggle_opt.description = "Toggle the undeleter on/off";
    toggle_opt.type = cot_sub_command;
    cmd.options.push_back(toggle_opt);
    
    command_option on_opt;
    on_opt.name = "on";
    on_opt.description = "Enable the undeleter";
    on_opt.type = cot_sub_command;
    cmd.options.push_back(on_opt);
    
    command_option off_opt;
    off_opt.name = "off";
    off_opt.description = "Disable the undeleter";
    off_opt.type = cot_sub_command;
    cmd.options.push_back(off_opt);
    
    command_option status_opt;
    status_opt.name = "status";
    status_opt.description = "Check the undeleter status";
    status_opt.type = cot_sub_command;
    cmd.options.push_back(status_opt);
    
    // Register the command globally
    bot.current_user_edit('', [&bot, cmd](const confirmation_callback_t& response) {
        if (response.is_error()) {
            std::cerr << "Failed to register commands: " << response.get_error().message << std::endl;
        } else {
            std::cout << "Slash commands registered successfully" << std::endl;
        }
    }, cmd);
}

/**
 * Handle message creation
 */
void on_message_create(const message_create_t& event) {
    // Cache the message
    cache_message(event);
}

/**
 * Handle message deletion
 */
void on_message_delete(const message_delete_t& event) {
    // Check if this channel is allowed
    if (!is_channel_allowed(event.channel_id, event.guild_id)) {
        return;
    }
    
    // Find and repost the deleted message
    CachedMessage* cached = find_cached_message(event.id, event.channel_id);
    if (cached) {
        std::cout << "Deleted message detected from " << cached->author_name 
                  << " in channel " << event.channel_id << ": " 
                  << cached->content << std::endl;
        repost_message(*cached);
        remove_cached_message(event.id, event.channel_id);
    } else {
        std::cout << "Message deleted but not in cache: " << event.id 
                  << " in channel " << event.channel_id << std::endl;
    }
}

/**
 * Handle bulk message deletion
 */
void on_message_delete_bulk(const message_delete_bulk_t& event) {
    // Check if this channel is allowed
    if (!is_channel_allowed(event.channel_id, event.guild_id)) {
        return;
    }
    
    std::cout << "Bulk delete detected: " << event.ids.size() << " messages in channel " 
              << event.channel_id << std::endl;
    
    // Find and repost each deleted message
    for (auto msg_id : event.ids) {
        CachedMessage* cached = find_cached_message(msg_id, event.channel_id);
        if (cached) {
            repost_message(*cached);
            remove_cached_message(msg_id, event.channel_id);
        }
    }
}

/**
 * Handle ready event
 */
void on_ready(const ready_t& event) {
    std::cout << "Bot logged in as " << event.my_user.username 
              << "#" << event.my_user.discriminator << " (ID: " 
              << event.my_user.id << ")" << std::endl;
    
    // Register slash commands
    register_commands(*event.from);
    
    // Set bot activity only if activity_type and activity_name are configured
    if (!config.activity_type.empty() && !config.activity_name.empty()) {
        presence_activity_type activity_type = presence_activity_type::pa_playing;
        if (config.activity_type == "watching") {
            activity_type = presence_activity_type::pa_watching;
        } else if (config.activity_type == "listening") {
            activity_type = presence_activity_type::pa_listening;
        } else if (config.activity_type == "streaming") {
            activity_type = presence_activity_type::pa_streaming;
        } else if (config.activity_type == "custom") {
            activity_type = presence_activity_type::pa_custom;
        }
        
        event.from->set_presence({
            {activity_type, config.activity_name},
            presence_status::ps_online
        });
    } else {
        // Just set online status without an activity
        event.from->set_presence({
            presence_status::ps_online
        });
    }
}

/**
 * Handle shutdown signal
 */
void signal_handler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    save_messages_to_file();
    if (bot_cluster) {
        bot_cluster->shutdown();
    }
    exit(0);
}

int main(int argc, char** argv) {
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Load configuration
    std::string config_file = "config.yml";
    if (argc > 1) {
        config_file = argv[1];
    }
    
    std::cout << "Loading configuration from " << config_file << "..." << std::endl;
    if (!load_config(config_file)) {
        std::cerr << "Failed to load configuration. Please check config.example.yml for reference." << std::endl;
        return 1;
    }
    
    std::cout << "Configuration loaded successfully." << std::endl;
    std::cout << "  Bot token: " << (config.bot_token.empty() ? "NOT SET" : "[hidden]") << std::endl;
    std::cout << "  Webhook URL: " << (config.webhook_url.empty() ? "NOT SET" : "[hidden]") << std::endl;
    std::cout << "  Trash emoji: " << config.trash_emoji << std::endl;
    std::cout << "  Max messages per channel: " << config.max_messages_per_channel << std::endl;
    std::cout << "  Cache persistence: " << (config.persist_to_file ? "enabled" : "disabled") << std::endl;
    
    // Load cached messages from file
    if (config.persist_to_file) {
        load_messages_from_file();
    }
    
    // Create bot cluster
    bot_cluster = new cluster(config.bot_token);
    
    // Set intents
    bot_cluster->intents = i_message_content | i_guild_messages;
    
    // Set up event handlers
    bot_cluster->on_ready = on_ready;
    bot_cluster->on_message_create = on_message_create;
    bot_cluster->on_message_delete = on_message_delete;
    bot_cluster->on_message_delete_bulk = on_message_delete_bulk;
    bot_cluster->on_interaction_create = on_interaction_create;
    
    // Log in
    try {
        std::cout << "Starting bot..." << std::endl;
        bot_cluster->start(false);
    } catch (const std::exception& e) {
        std::cerr << "Error starting bot: " << e.what() << std::endl;
        delete bot_cluster;
        return 1;
    }
    
    std::cout << "Bot is running. Press Ctrl+C to stop." << std::endl;
    
    // Wait for shutdown
    bot_cluster->wait();
    
    // Save cache before exiting
    save_messages_to_file();
    
    // Clean up
    delete bot_cluster;
    
    return 0;
}
