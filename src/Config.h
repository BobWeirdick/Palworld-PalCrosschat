#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace PalCrosschat
{
    struct FilterPatternConfig
    {
        std::string pattern;
        int mute_minutes = 5;
        std::string mute_message;
    };

    struct Config
    {
        bool enabled = false;

        // General
        std::string server_origin; // pal-na | pal-eu
        int poll_interval_ms = 750;
        int max_batch = 50;
        int max_broadcasts_per_tick = 5;
        bool debug_verbose = false;

        // MySQL
        std::string mysql_host;
        int mysql_port = 3306;
        std::string mysql_user;
        std::string mysql_password;
        std::string mysql_database;
        int reconnect_backoff_max_sec = 30;

        // Format
        // Used inside ChatFormat as {prefix}. Prefer "NA"/"EU"/"Discord" when ChatFormat
        // already wraps with [{prefix}].
        std::string prefix_na = "NA";
        std::string prefix_eu = "EU";
        std::string prefix_discord = "Discord";
        // Placeholders: {prefix}, {guild}, {player}, {message}
        std::string chat_format = "[{prefix}] [{guild}] {player}: {message}";
        // InjectCategory: "global" -> 1, "discord" -> 4
        uint8_t inject_category = 1;
        std::string inject_category_name = "global";
        // When true, local Global chat is rebroadcast using ChatFormat (same as cross-server).
        bool show_local_server_tag = false;

        // ChatFilter
        std::string mute_log_webhook;
        std::string initial_mute_notification = "You have been muted for {mutetime}!";
        std::string active_mute_notification = "You are muted! Time remaining: {remainingtime}";
        std::vector<FilterPatternConfig> filter_patterns;
    };

    // Loads Mods/PalCrosschat/config.json (mod root next to dlls/).
    // On failure: logs error, returns config with enabled=false.
    Config LoadConfig();

    // Log summary without password.
    void LogConfigSummary(const Config& cfg);
}
