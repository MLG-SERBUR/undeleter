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
#include <optional>
#include <ctime>
#include <algorithm>
#include <csignal>
#include <functional>
#include <cctype>

// Configuration structure
struct Config {
    std::string bot_token;
    std::string webhook_url;  // Default webhook URL
    std::string trash_emoji = "🗑️";
    int max_messages_per_channel = 100;
    bool persist_to_file = true;
    std::string storage_file = "messages.dat";
    std::vector<dpp::snowflake> guild_whitelist;
    std::vector<dpp::snowflake> guild_blacklist;
    std::vector<dpp::snowflake> channel_blacklist;
    std::vector<dpp::snowflake> channel_whitelist;
    // Per-channel webhooks: channel_id -> webhook_url
    std::unordered_map<dpp::snowflake, std::string> channel_webhooks;
    // Skip undeleting messages from users with these role IDs
    std::vector<dpp::snowflake> skip_role_ids;
    // Skip undeleting if user has any of these permissions
    // Default: skip users with manage_messages or administrator
    uint64_t skip_permissions = dpp::p_manage_messages;
    // Auto-create webhooks for channels that don't have one
    bool auto_create_webhooks = true;
};

// Cached message structure
struct CachedMessage {
    dpp::snowflake id;
    dpp::snowflake channel_id;
    dpp::snowflake guild_id;
    dpp::snowflake author_id;
    std::string author_name;
    std::string content;
    time_t timestamp;
    bool skip_undelete = false;  // True if this message should not be undeleted (e.g., from admin)
};

// Global configuration
Config config;

// Message cache: channel_id -> vector of messages
std::unordered_map<dpp::snowflake, std::vector<CachedMessage>> message_cache;
std::mutex cache_mutex;

// Cached webhook objects for channels, including auto-created webhooks
std::unordered_map<dpp::snowflake, dpp::webhook> channel_webhook_objects;
std::mutex webhook_mutex;

// Get the cluster for webhook execution
dpp::cluster* bot_cluster = nullptr;

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
        if(!token.empty()) {
            token.erase(token.find_last_not_of(" \t") + 1);
        }
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

/**
 * Trim whitespace from a string
 */
std::string ws_trim(const std::string& str) {
    std::string result = str;
    result.erase(0, result.find_first_not_of(" \t"));
    if(result.empty()) return result;
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
std::vector<dpp::snowflake> parse_snowflake_list(const std::string& value) {
    std::vector<dpp::snowflake> result;
    // Handle both comma-separated and YAML list format
    std::string trimmed = ws_trim(unquote(value));
    
    // Check if it's a YAML list (starts with -)
    if (trimmed.find('-') != std::string::npos) {
        // Parse YAML list format
        std::istringstream iss(trimmed);
        std::string line;
        while (std::getline(iss, line)) {
            line = ws_trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '-') {
                std::string id = ws_trim(line.substr(1));
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
    
    while (std::getline(file, line)) {
        line = ws_trim(line);
        
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        // Check for section (e.g., [webhook] or guilds:)
        if (line[0] == '[') {
            size_t end_bracket = line.find(']');
            if (end_bracket != std::string::npos) {
                current_section = line.substr(1, end_bracket - 1);
                continue;
            }
        }
        
        // Check for YAML-style section (e.g., "webhook:" or "cache:")
        if (line.back() == ':') {
            current_section = ws_trim(line.substr(0, line.size() - 1));
            continue;
        }
        
        // Handle channel_webhooks map (special case)
        if (current_section == "channel_webhooks" && !line.empty() && line[0] != '#') {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string channel_id_str = ws_trim(line.substr(0, colon_pos));
                std::string webhook_url = ws_trim(unquote(line.substr(colon_pos + 1)));
                try {
                    dpp::snowflake channel_id = std::stoull(channel_id_str);
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
                std::string id = ws_trim(line.substr(1));
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
        
        std::string key = ws_trim(line.substr(0, colon_pos));
        std::string value = ws_trim(line.substr(colon_pos + 1));
        value = unquote(value);
        std::string full_key = key;
        
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
        } else if (key == "channel_webhooks") {
            // Handled as section earlier
        } else if (key == "auto_create_webhooks") {
            config.auto_create_webhooks = (value == "true" || value == "1" || value == "yes");
        } else if (key == "skip_role_ids") {
            auto ids = parse_snowflake_list(value);
            config.skip_role_ids.insert(config.skip_role_ids.end(), ids.begin(), ids.end());
        } else if (key == "skip_permissions") {
            std::string lower_value = value;
            std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
            
            uint64_t perms = 0;
            
            if (lower_value.find("administrator") != std::string::npos || lower_value.find("admin") != std::string::npos) {
                perms = perms | dpp::p_administrator;
            }
            if (lower_value.find("manage_messages") != std::string::npos || lower_value.find("manage msg") != std::string::npos) {
                perms = perms | dpp::p_manage_messages;
            }
            if (lower_value.find("kick_members") != std::string::npos || lower_value.find("kick") != std::string::npos) {
                perms = perms | dpp::p_kick_members;
            }
            if (lower_value.find("ban_members") != std::string::npos || lower_value.find("ban") != std::string::npos) {
                perms = perms | dpp::p_ban_members;
            }
            if (lower_value.find("manage_guild") != std::string::npos || lower_value.find("manage server") != std::string::npos) {
                perms = perms | dpp::p_manage_guild;
            }
            
            if (perms != 0) {
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
        }
    }
    
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
        uint8_t version = 0;
        if (!file.read(reinterpret_cast<char*>(&version), sizeof(version))) {
            return;
        }
        
        file.seekg(0, std::ios::beg);
        size_t channel_count;
        if (!file.read(reinterpret_cast<char*>(&channel_count), sizeof(channel_count))) return;
        
        for (size_t i = 0; i < channel_count; i++) {
            dpp::snowflake channel_id;
            if (!file.read(reinterpret_cast<char*>(&channel_id), sizeof(channel_id))) return;
            
            size_t message_count;
            if (!file.read(reinterpret_cast<char*>(&message_count), sizeof(message_count))) return;
            
            std::vector<CachedMessage> messages;
            for (size_t j = 0; j < message_count; j++) {
                CachedMessage msg;
                if (!file.read(reinterpret_cast<char*>(&msg.id), sizeof(msg.id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.channel_id), sizeof(msg.channel_id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.guild_id), sizeof(msg.guild_id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.author_id), sizeof(msg.author_id))) break;
                if (!file.read(reinterpret_cast<char*>(&msg.timestamp), sizeof(msg.timestamp))) break;
                
                if (!file.read(reinterpret_cast<char*>(&msg.skip_undelete), sizeof(msg.skip_undelete))) {
                    msg.skip_undelete = false;
                    file.clear();
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
    if (!file.is_open()) return;
    
    try {
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
bool is_channel_allowed(dpp::snowflake channel_id, dpp::snowflake guild_id) {
    if (std::find(config.guild_blacklist.begin(), config.guild_blacklist.end(), guild_id) != config.guild_blacklist.end()) {
        return false;
    }
    
    if (!config.guild_whitelist.empty()) {
        if (std::find(config.guild_whitelist.begin(), config.guild_whitelist.end(), guild_id) == config.guild_whitelist.end()) {
            return false;
        }
    }
    
    if (std::find(config.channel_blacklist.begin(), config.channel_blacklist.end(), channel_id) != config.channel_blacklist.end()) {
        return false;
    }
    
    if (!config.channel_whitelist.empty()) {
        if (std::find(config.channel_whitelist.begin(), config.channel_whitelist.end(), channel_id) == config.channel_whitelist.end()) {
            return false;
        }
    }
    
    return true;
}

/**
 * Check if a user should be skipped (e.g., admin, moderator)
 */
bool should_skip_user(const dpp::message_create_t& event) {
    if (event.msg.author.is_bot()) {
        return true;
    }
    
    if (event.msg.guild_id == 0) {
        return false;
    }
    
    if (!config.skip_role_ids.empty()) {
        auto roles = event.msg.member.get_roles();
        for (auto role_id : config.skip_role_ids) {
            if (std::find(roles.begin(), roles.end(), role_id) != roles.end()) {
                return true;
            }
        }
    }
    
    if (config.skip_permissions != 0) {
        dpp::channel* c = dpp::find_channel(event.msg.channel_id);
        if (c) {
            dpp::permission perms = c->get_user_permissions(event.msg.member);
            if (perms.can(config.skip_permissions) || perms.can(dpp::p_administrator)) {
                return true;
            }
        }
    }
    
    return false;
}

/**
 * Cache a message
 */
void cache_message(const dpp::message_create_t& event) {
    if (event.msg.content.empty() || !is_channel_allowed(event.msg.channel_id, event.msg.guild_id)) {
        return;
    }
    
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
    
    auto& channel_messages = message_cache[event.msg.channel_id];
    for (auto& existing : channel_messages) {
        if (existing.id == msg.id) {
            return;
        }
    }
    
    channel_messages.push_back(msg);
    
    if (channel_messages.size() > static_cast<size_t>(config.max_messages_per_channel)) {
        channel_messages.erase(channel_messages.begin());
    }
}

/**
 * Find a cached message by ID in a specific channel
 */
CachedMessage* find_cached_message(dpp::snowflake message_id, dpp::snowflake channel_id) {
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
void remove_cached_message(dpp::snowflake message_id, dpp::snowflake channel_id) {
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
    
    if (messages.empty()) {
        message_cache.erase(it);
    }
}

/**
 * Parse a webhook URL into a webhook object.
 */
bool parse_webhook_url(const std::string& url, dpp::webhook& wh) {
    size_t api_pos = url.find("/api/webhooks/");
    if (api_pos == std::string::npos) {
        return false;
    }

    std::string rest = url.substr(api_pos + 14);
    size_t slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) {
        return false;
    }

    std::string webhook_id_str = rest.substr(0, slash_pos);
    std::string token = rest.substr(slash_pos + 1);
    if (webhook_id_str.empty() || token.empty()) {
        return false;
    }

    try {
        wh.id = std::stoull(webhook_id_str);
        wh.token = token;
        wh.url = url;
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Build a webhook URL from a webhook object.
 */
std::string make_webhook_url(const dpp::webhook& wh) {
    if (wh.id == 0 || wh.token.empty()) {
        return "";
    }
    return "https://discord.com/api/webhooks/" + std::to_string(wh.id) + "/" + wh.token;
}

/**
 * Cache webhook object for a channel.
 */
void cache_webhook_for_channel(dpp::snowflake channel_id, const dpp::webhook& wh) {
    std::lock_guard<std::mutex> lock(webhook_mutex);
    channel_webhook_objects[channel_id] = wh;
    std::string webhook_url = make_webhook_url(wh);
    if (!webhook_url.empty()) {
        config.channel_webhooks[channel_id] = webhook_url;
    }
}

/**
 * Remove stale webhook cache entries for a channel.
 */
void clear_cached_webhook(dpp::snowflake channel_id) {
    std::lock_guard<std::mutex> lock(webhook_mutex);
    channel_webhook_objects.erase(channel_id);
    config.channel_webhooks.erase(channel_id);
}

/**
 * Retrieve a cached webhook object for a channel.
 */
std::optional<dpp::webhook> get_stored_webhook(dpp::snowflake channel_id) {
    std::lock_guard<std::mutex> lock(webhook_mutex);
    auto it = channel_webhook_objects.find(channel_id);
    if (it != channel_webhook_objects.end()) {
        return it->second;
    }
    auto it2 = config.channel_webhooks.find(channel_id);
    if (it2 == config.channel_webhooks.end()) {
        return std::nullopt;
    }
    dpp::webhook wh;
    if (!parse_webhook_url(it2->second, wh)) {
        return std::nullopt;
    }
    channel_webhook_objects[channel_id] = wh;
    return wh;
}

/**
 * Execute a webhook message.
 */
void execute_webhook(const dpp::webhook& wh, const dpp::message& wm, dpp::snowflake channel_id, dpp::snowflake guild_id, bool retry_on_unknown = true);


/**
 * Execute a webhook message given a webhook URL.
 */
void execute_webhook(const std::string& webhook_url, const dpp::message& wm, dpp::snowflake channel_id = 0, dpp::snowflake guild_id = 0) {
    dpp::webhook wh;
    if (!parse_webhook_url(webhook_url, wh)) {
        std::cerr << "Invalid webhook URL format: " << webhook_url << std::endl;
        return;
    }
    if (channel_id != 0) {
        cache_webhook_for_channel(channel_id, wh);
    }
    execute_webhook(wh, wm, channel_id, guild_id);
}

/**
 * Get the webhook URL for a specific channel
 */
std::string get_webhook_url(dpp::snowflake channel_id) {
    auto it = config.channel_webhooks.find(channel_id);
    if (it != config.channel_webhooks.end()) {
        return it->second;
    }
    return config.webhook_url;
}

/**
 * Create a webhook for a channel
 */
void create_webhook_for_channel(dpp::snowflake channel_id, dpp::snowflake guild_id, const std::function<void(const dpp::webhook&)>& callback) {
    if (guild_id == 0) return;
    
    dpp::webhook wh;
    wh.channel_id = channel_id;
    wh.name = "🗑️";
    
    bot_cluster->create_webhook(wh, [channel_id, callback](const dpp::confirmation_callback_t& response) {
        if (response.is_error()) {
            std::cerr << "Failed to create webhook for channel " << channel_id 
                      << ": " << response.get_error().message << std::endl;
            return;
        }
        
        dpp::webhook created_wh = response.get<dpp::webhook>();
        cache_webhook_for_channel(channel_id, created_wh);
        std::cout << "Created webhook for channel " << channel_id << ": " << created_wh.id << std::endl;
        
        if (callback) {
            callback(created_wh);
        }
    });
}

/**
 * Execute a webhook message.
 */
void execute_webhook(const dpp::webhook& wh, const dpp::message& wm, dpp::snowflake channel_id, dpp::snowflake guild_id, bool retry_on_unknown) {
    dpp::json payload;
    payload["content"] = wm.content;
    if (!wh.name.empty()) {
        payload["username"] = wh.name;
    }
    if (!wh.avatar_url.empty()) {
        payload["avatar_url"] = wh.avatar_url;
    }

    bot_cluster->post_rest("/api/webhooks", std::to_string(wh.id), wh.token, dpp::m_post, payload.dump(),
        [channel_id, guild_id, wm, retry_on_unknown](dpp::json &j, const dpp::http_request_completion_t& http) {
            dpp::confirmation_callback_t completion(http);
            if (completion.is_error()) {
                std::string err = completion.get_error().message;
                std::cerr << "Webhook error: " << err << std::endl;
                if (err == "Unknown Webhook" && channel_id != 0) {
                    clear_cached_webhook(channel_id);
                    if (retry_on_unknown && config.auto_create_webhooks && guild_id != 0) {
                        std::cerr << "Unknown webhook for channel " << channel_id << ", recreating and retrying..." << std::endl;
                        create_webhook_for_channel(channel_id, guild_id, [wm, channel_id, guild_id](const dpp::webhook& new_wh) {
                            execute_webhook(new_wh, wm, channel_id, guild_id, false);
                        });
                    }
                }
            } else {
                std::cout << "Successfully reposted deleted message via webhook" << std::endl;
            }
        }
    );
}

/**
 * Get or create a webhook for a channel
 */
void get_or_create_webhook(dpp::snowflake channel_id, dpp::snowflake guild_id, 
                          const std::function<void(const std::string&)>& callback) {
    if (config.auto_create_webhooks && guild_id != 0) {
        if (auto cached = get_stored_webhook(channel_id); cached) {
            callback(make_webhook_url(*cached));
            return;
        }
        
        auto it = config.channel_webhooks.find(channel_id);
        if (it != config.channel_webhooks.end()) {
            callback(it->second);
            return;
        }
        
        create_webhook_for_channel(channel_id, guild_id, 
            [callback, channel_id](const dpp::webhook& wh) {
                std::string webhook_url = make_webhook_url(wh);
                callback(webhook_url);
            }
        );
        return;
    }
    
    std::string existing_url = get_webhook_url(channel_id);
    if (!existing_url.empty()) {
        callback(existing_url);
        return;
    }
    
    callback("");
}

/**
 * Repost a deleted message via webhook
 */
void repost_message(const CachedMessage& msg) {
    {
        std::lock_guard<std::mutex> lock(undelete_mutex);
        if (!undelete_enabled) {
            std::cout << "Undeleter is disabled, skipping repost for message " 
                      << msg.id << " from " << msg.author_name << std::endl;
            return;
        }
    }
    
    if (msg.skip_undelete) {
        std::cout << "Skipping undelete for privileged user message from " 
                  << msg.author_name << " (has manage messages permission)" << std::endl;
        return;
    }
    
    get_or_create_webhook(msg.channel_id, msg.guild_id, 
        [msg](const std::string& webhook_url) {
            if (webhook_url.empty()) {
                std::cerr << "No webhook URL configured/available for channel " 
                          << msg.channel_id << ", cannot repost message" << std::endl;
                return;
            }
            
            dpp::webhook wh;
            if (!parse_webhook_url(webhook_url, wh)) {
                std::cerr << "Invalid webhook URL format: " << webhook_url << std::endl;
                return;
            }
            wh.name = config.trash_emoji + msg.author_name;

            dpp::message wm;
            wm.content = msg.content;
            execute_webhook(wh, wm, msg.channel_id, msg.guild_id);
        }
    );
}

/**
 * Handle slash command for toggling undeleter
 */
void on_slashcommand(const dpp::slashcommand_t& event) {
    if (event.command.get_command_name() == "undelete") {
        dpp::permission perms = event.command.get_resolved_permission(event.command.usr.id);
        if (!perms.can(dpp::p_manage_messages) && !perms.can(dpp::p_administrator)) {
            event.reply(dpp::message("❌ You do not have permission to use this command. (Require Manage Messages permission)").set_flags(dpp::m_ephemeral));
            return;
        }
        
        std::string subcommand = "status"; 
        auto param = event.get_parameter("action");
        if (std::holds_alternative<std::string>(param)) {
            subcommand = std::get<std::string>(param);
        }
        
        if (subcommand == "toggle" || subcommand == "off") {
            std::lock_guard<std::mutex> lock(undelete_mutex);
            undelete_enabled = false;
            event.reply(dpp::message("✅ Undeleter is now **DISABLED**. Deleted messages will not be reposted.").set_flags(dpp::m_ephemeral));
        } else if (subcommand == "on") {
            std::lock_guard<std::mutex> lock(undelete_mutex);
            undelete_enabled = true;
            event.reply(dpp::message("✅ Undeleter is now **ENABLED**. Deleted messages will be reposted.").set_flags(dpp::m_ephemeral));
        } else {
            event.reply(dpp::message("Undeleter is currently **" + std::string(undelete_enabled ? "ENABLED" : "DISABLED") + "**.\n"
                      "Use `/undelete action:toggle` or `/undelete action:off` to disable, `/undelete action:on` to enable.").set_flags(dpp::m_ephemeral));
        }
    }
}

/**
 * Register slash commands
 */
void register_commands(dpp::cluster& bot) {
    dpp::slashcommand cmd;
    cmd.set_name("undelete");
    cmd.set_description("Manage the undeleter bot");
    cmd.set_application_id(bot.me.id);
    cmd.set_default_permissions(dpp::p_manage_messages);
    
    dpp::command_option action_opt(dpp::co_string, "action", "Action to perform", true);
    action_opt.add_choice(dpp::command_option_choice("toggle", "toggle"));
    action_opt.add_choice(dpp::command_option_choice("on", "on"));
    action_opt.add_choice(dpp::command_option_choice("off", "off"));
    action_opt.add_choice(dpp::command_option_choice("status", "status"));
    cmd.add_option(action_opt);
    
    bot.global_command_create(cmd, [](const dpp::confirmation_callback_t& response) {
        if (response.is_error()) {
            std::cerr << "Failed to register commands: " << response.get_error().message << std::endl;
        } else {
            std::cout << "Slash commands registered successfully" << std::endl;
        }
    });
}

/**
 * Handle message creation
 */
void on_message_create(const dpp::message_create_t& event) {
    cache_message(event);
}

/**
 * Handle message deletion
 */
void on_message_delete(const dpp::message_delete_t& event) {
    CachedMessage* cached = find_cached_message(event.id, event.channel_id);
    if (cached) {
        if (!is_channel_allowed(event.channel_id, cached->guild_id)) {
            return;
        }
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
void on_message_delete_bulk(const dpp::message_delete_bulk_t& event) {
    std::cout << "Bulk delete detected: " << event.deleted.size() << " messages." << std::endl;
    
    std::vector<CachedMessage> to_repost;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto msg_id : event.deleted) {
            for (auto& [channel_id, messages] : message_cache) {
                auto it = std::find_if(messages.begin(), messages.end(), [msg_id](const CachedMessage& msg) { return msg.id == msg_id; });
                if (it != messages.end()) {
                    to_repost.push_back(*it);
                    messages.erase(it);
                    break;
                }
            }
        }
    }
    
    for (const auto& msg : to_repost) {
        if (is_channel_allowed(msg.channel_id, msg.guild_id)) {
            repost_message(msg);
        }
    }
}

/**
 * Handle ready event
 */
void on_ready(const dpp::ready_t& event) {
    std::cout << "Bot logged in successfully!" << std::endl;
    register_commands(*bot_cluster);
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
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
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
    
    if (config.persist_to_file) {
        load_messages_from_file();
    }
    
    bot_cluster = new dpp::cluster(config.bot_token);
    bot_cluster->intents = dpp::i_message_content | dpp::i_guild_messages;
    
    bot_cluster->on_ready(on_ready);
    bot_cluster->on_message_create(on_message_create);
    bot_cluster->on_message_delete(on_message_delete);
    bot_cluster->on_message_delete_bulk(on_message_delete_bulk);
    bot_cluster->on_slashcommand(on_slashcommand);
    
    try {
        std::cout << "Starting bot..." << std::endl;
        bot_cluster->start(dpp::st_wait);
    } catch (const std::exception& e) {
        std::cerr << "Error starting bot: " << e.what() << std::endl;
        delete bot_cluster;
        return 1;
    }
    
    save_messages_to_file();
    delete bot_cluster;
    
    return 0;
}