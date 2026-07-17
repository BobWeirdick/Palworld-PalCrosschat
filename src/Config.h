#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace PalCrosschat
{
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
        std::string prefix_na = "[NA]";
        std::string prefix_eu = "[EU]";
        std::string prefix_discord = "[Discord]";
        // InjectCategory: "global" -> 1, "discord" -> 4
        uint8_t inject_category = 1;
        std::string inject_category_name = "global";

        // WordBlacklist — each BlockedWords entry is a regex (ECMAScript).
        std::vector<std::string> blocked_words;
        int auto_mute_minutes = 5;
        std::string mute_log_webhook;
    };

    // Loads Mods/PalCrosschat/config.json (mod root next to dlls/).
    // On failure: logs error, returns config with enabled=false.
    Config LoadConfig();

    // Log summary without password.
    void LogConfigSummary(const Config& cfg);
}
