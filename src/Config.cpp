#include "Config.h"
#include "PalChatApi.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

#include <Windows.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace PalCrosschat
{
    namespace
    {
        std::wstring GetThisDllDirectory()
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&GetThisDllDirectory),
                    &module))
            {
                return {};
            }

            wchar_t path[MAX_PATH]{};
            const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
            {
                return {};
            }

            return fs::path(path).parent_path().wstring();
        }

        bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (towlower(a[i]) != towlower(b[i]))
                {
                    return false;
                }
            }
            return true;
        }

        std::wstring GetModRootDirectory()
        {
            const std::wstring dllDir = GetThisDllDirectory();
            if (dllDir.empty())
            {
                return {};
            }

            const fs::path dir(dllDir);
            if (EqualsIgnoreCase(dir.filename().wstring(), L"dlls"))
            {
                return dir.parent_path().wstring();
            }
            return dllDir;
        }

        template <typename T>
        T GetOr(const json& obj, const char* key, T fallback)
        {
            if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
            {
                return fallback;
            }
            try
            {
                return obj.at(key).get<T>();
            }
            catch (...)
            {
                return fallback;
            }
        }

        void ApplyInjectCategory(Config& cfg, const std::string& value)
        {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "discord")
            {
                cfg.inject_category = CHAT_CATEGORY_DISCORD;
                cfg.inject_category_name = "discord";
            }
            else
            {
                cfg.inject_category = CHAT_CATEGORY_GLOBAL;
                cfg.inject_category_name = "global";
            }
        }
    }

    Config LoadConfig()
    {
        Config cfg{};

        const std::wstring root = GetModRootDirectory();
        if (root.empty())
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] Could not resolve mod directory; mod disabled\n"));
            return cfg;
        }

        const fs::path configPath = fs::path(root) / L"config.json";
        if (!fs::exists(configPath))
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] Missing config.json at {}; copy config.json.example and fill MySQL settings. Mod disabled.\n"),
                RC::ensure_str(configPath.wstring()));
            return cfg;
        }

        std::ifstream in(configPath);
        if (!in)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] Failed to open config.json; mod disabled\n"));
            return cfg;
        }

        json root_json;
        try
        {
            in >> root_json;
        }
        catch (const std::exception& ex)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] Failed to parse config.json: {}; mod disabled\n"),
                RC::ensure_str(ex.what()));
            return cfg;
        }

        const json general = root_json.value("General", json::object());
        const json mysql = root_json.value("MySQL", json::object());
        const json format = root_json.value("Format", json::object());
        // Prefer ChatFilter; fall back to legacy WordBlacklist for older configs.
        const json chat_filter = root_json.contains("ChatFilter")
                                     ? root_json.value("ChatFilter", json::object())
                                     : root_json.value("WordBlacklist", json::object());

        cfg.server_origin = GetOr<std::string>(general, "ServerOrigin", "");
        cfg.poll_interval_ms = GetOr<int>(general, "PollIntervalMs", cfg.poll_interval_ms);
        cfg.max_batch = GetOr<int>(general, "MaxBatch", cfg.max_batch);
        cfg.max_broadcasts_per_tick =
            GetOr<int>(general, "MaxBroadcastsPerTick", cfg.max_broadcasts_per_tick);
        cfg.debug_verbose = GetOr<bool>(general, "DebugVerbose", cfg.debug_verbose);

        cfg.mysql_host = GetOr<std::string>(mysql, "Host", "");
        cfg.mysql_port = GetOr<int>(mysql, "Port", cfg.mysql_port);
        cfg.mysql_user = GetOr<std::string>(mysql, "User", "");
        cfg.mysql_password = GetOr<std::string>(mysql, "Password", "");
        cfg.mysql_database = GetOr<std::string>(mysql, "Database", "");
        cfg.reconnect_backoff_max_sec =
            GetOr<int>(mysql, "ReconnectBackoffMaxSec", cfg.reconnect_backoff_max_sec);

        cfg.prefix_na = GetOr<std::string>(format, "PrefixNA", cfg.prefix_na);
        cfg.prefix_eu = GetOr<std::string>(format, "PrefixEU", cfg.prefix_eu);
        cfg.prefix_discord = GetOr<std::string>(format, "PrefixDiscord", cfg.prefix_discord);
        cfg.chat_format = GetOr<std::string>(format, "ChatFormat", cfg.chat_format);
        ApplyInjectCategory(cfg, GetOr<std::string>(format, "InjectCategory", "global"));
        cfg.show_local_server_tag =
            GetOr<bool>(format, "ShowLocalServerTag", cfg.show_local_server_tag);
        cfg.preserve_sender_uid =
            GetOr<bool>(format, "PreserveSenderUId", cfg.preserve_sender_uid);
        cfg.enable_audience_scan =
            GetOr<bool>(format, "EnableAudienceScan", cfg.enable_audience_scan);

        cfg.mute_log_webhook = GetOr<std::string>(
            chat_filter,
            "LogWebhookUrl",
            GetOr<std::string>(chat_filter, "MuteLogWebhook", ""));
        cfg.initial_mute_notification = GetOr<std::string>(
            chat_filter, "InitialMuteNotification", cfg.initial_mute_notification);
        cfg.active_mute_notification = GetOr<std::string>(
            chat_filter, "ActiveMuteNotification", cfg.active_mute_notification);

        if (chat_filter.contains("FilteredPatterns") && chat_filter["FilteredPatterns"].is_array())
        {
            for (const auto& entry : chat_filter["FilteredPatterns"])
            {
                if (!entry.is_object())
                {
                    continue;
                }
                FilterPatternConfig pattern_cfg;
                pattern_cfg.pattern = GetOr<std::string>(entry, "Pattern", "");
                pattern_cfg.mute_minutes = GetOr<int>(entry, "MuteMinutes", 5);
                pattern_cfg.mute_message = GetOr<std::string>(entry, "MuteMessage", "");
                if (pattern_cfg.mute_minutes < 0)
                {
                    pattern_cfg.mute_minutes = 0;
                }
                if (!pattern_cfg.pattern.empty())
                {
                    cfg.filter_patterns.push_back(std::move(pattern_cfg));
                }
            }
        }
        else if (chat_filter.contains("BlockedWords") && chat_filter["BlockedWords"].is_array())
        {
            // Legacy WordBlacklist.BlockedWords: string array + shared AutoMuteMinutes.
            const int legacy_minutes = GetOr<int>(chat_filter, "AutoMuteMinutes", 5);
            for (const auto& entry : chat_filter["BlockedWords"])
            {
                if (!entry.is_string())
                {
                    continue;
                }
                FilterPatternConfig pattern_cfg;
                pattern_cfg.pattern = entry.get<std::string>();
                pattern_cfg.mute_minutes = legacy_minutes < 0 ? 0 : legacy_minutes;
                pattern_cfg.mute_message = "blocked word or phrase (chat filter)";
                if (!pattern_cfg.pattern.empty())
                {
                    cfg.filter_patterns.push_back(std::move(pattern_cfg));
                }
            }
        }

        if (cfg.server_origin != "pal-na" && cfg.server_origin != "pal-eu")
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] ServerOrigin must be 'pal-na' or 'pal-eu' (got '{}'); mod disabled\n"),
                RC::ensure_str(cfg.server_origin));
            return Config{};
        }

        if (cfg.mysql_host.empty() || cfg.mysql_user.empty() || cfg.mysql_database.empty())
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] MySQL Host/User/Database must be set in config.json; mod disabled\n"));
            return Config{};
        }

        if (cfg.poll_interval_ms < 100)
        {
            cfg.poll_interval_ms = 100;
        }
        if (cfg.max_batch < 1)
        {
            cfg.max_batch = 1;
        }
        if (cfg.max_broadcasts_per_tick < 1)
        {
            cfg.max_broadcasts_per_tick = 1;
        }
        if (cfg.reconnect_backoff_max_sec < 1)
        {
            cfg.reconnect_backoff_max_sec = 1;
        }
        if (cfg.mysql_port <= 0 || cfg.mysql_port > 65535)
        {
            cfg.mysql_port = 3306;
        }
        cfg.enabled = true;
        return cfg;
    }

    void LogConfigSummary(const Config& cfg)
    {
        if (!cfg.enabled)
        {
            RC::Output::send<RC::LogLevel::Warning>(
                STR("[PalCrosschat] Config: disabled\n"));
            return;
        }

        RC::Output::send<RC::LogLevel::Normal>(
            STR("[PalCrosschat] Config: origin={} poll={}ms maxBatch={} maxBroadcasts/tick={} verbose={} "
                "mysql={}:{} user={} db={} backoffMax={}s injectCategory={} showLocalTag={} "
                "audienceScan={} prefixes=[{}|{}|{}] chatFormat={} chatFilterPatterns={} muteWebhook={}\n"),
            RC::ensure_str(cfg.server_origin),
            cfg.poll_interval_ms,
            cfg.max_batch,
            cfg.max_broadcasts_per_tick,
            cfg.debug_verbose ? STR("true") : STR("false"),
            RC::ensure_str(cfg.mysql_host),
            cfg.mysql_port,
            RC::ensure_str(cfg.mysql_user),
            RC::ensure_str(cfg.mysql_database),
            cfg.reconnect_backoff_max_sec,
            RC::ensure_str(cfg.inject_category_name),
            cfg.show_local_server_tag ? STR("true") : STR("false"),
            cfg.enable_audience_scan ? STR("true") : STR("false"),
            RC::ensure_str(cfg.prefix_na),
            RC::ensure_str(cfg.prefix_eu),
            RC::ensure_str(cfg.prefix_discord),
            RC::ensure_str(cfg.chat_format),
            cfg.filter_patterns.size(),
            cfg.mute_log_webhook.empty() ? STR("off") : STR("on"));
    }
}
