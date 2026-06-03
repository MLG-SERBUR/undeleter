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

using namespace dpp;

// Configuration structure
struct Config {
    std::string bot_token;
    std::string webhook_url;
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
};

// Global configuration
Config config;

// Message cache: channel_id -> vector of messages
std::unordered_map<snowflake, std::vector<CachedMessage>> message_cache;
std::mutex cache_mutex;

// Get the cluster for webhook execution
cluster* bot_cluster = nullptr;

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
 * Cache a message
 */
void cache_message(const message_create_t& event) {
    // Ignore messages from bots
    if (event.msg.author.is_bot()) {
        return;
    }
    
    // Ignore empty messages
    if (event.msg.content.empty()) {
        return;
    }
    
    // Check if channel is allowed
    if (!is_channel_allowed(event.msg.channel_id, event.msg.guild_id)) {
        return;
    }
    
    CachedMessage msg;
    msg.id = event.msg.id;
    msg.channel_id = event.msg.channel_id;
    msg.guild_id = event.msg.guild_id;
    msg.author_id = event.msg.author.id;
    msg.author_name = event.msg.author.username;
    msg.content = event.msg.content;
    msg.timestamp = time(nullptr);
    
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
 * Repost a deleted message via webhook
 */
void repost_message(const CachedMessage& msg) {
    if (config.webhook_url.empty()) {
        std::cerr << "No webhook URL configured, cannot repost message" << std::endl;
        return;
    }
    
    // Prepare the message content - just the original message
    std::string content = msg.content;
    
    // Create webhook message with trash emoji + username as the webhook username
    webhook_message wm;
    wm.content = content;
    wm.username = config.trash_emoji + msg.author_name;
    
    // Extract webhook ID and token from URL
    // URL format: https://discord.com/api/webhooks/ID/TOKEN
    std::string url = config.webhook_url;
    size_t api_pos = url.find("/api/webhooks/");
    if (api_pos == std::string::npos) {
        std::cerr << "Invalid webhook URL format: " << url << std::endl;
        return;
    }
    
    std::string rest = url.substr(api_pos + 15); // Length of "/api/webhooks/"
    size_t slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) {
        std::cerr << "Invalid webhook URL format: " << url << std::endl;
        return;
    }
    
    std::string webhook_id_str = rest.substr(0, slash_pos);
    std::string token = rest.substr(slash_pos + 1);
    
    try {
        snowflake webhook_id = std::stoull(webhook_id_str);
        
        // Send the webhook message
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
 * Handle message creation
 */
void on_message_create(const message_create_t& event) {
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
    
    // Set bot activity
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
