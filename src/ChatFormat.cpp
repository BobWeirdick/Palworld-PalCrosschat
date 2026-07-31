#include "ChatFormat.h"

#include <cstddef>

namespace PalCrosschat
{
    namespace
    {
        void ReplaceAll(std::string& text, std::string_view from, std::string_view to)
        {
            if (from.empty())
            {
                return;
            }
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos)
            {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        void CollapseSpaces(std::string& text)
        {
            std::string out;
            out.reserve(text.size());
            bool prev_space = false;
            for (char c : text)
            {
                if (c == ' ' || c == '\t')
                {
                    if (!prev_space)
                    {
                        out.push_back(' ');
                        prev_space = true;
                    }
                    continue;
                }
                prev_space = false;
                out.push_back(c);
            }
            size_t start = 0;
            while (start < out.size() && out[start] == ' ')
            {
                ++start;
            }
            size_t end = out.size();
            while (end > start && out[end - 1] == ' ')
            {
                --end;
            }
            text = out.substr(start, end - start);
        }

        void CleanupEmptyGuildBrackets(std::string& text)
        {
            ReplaceAll(text, " []", " ");
            ReplaceAll(text, "[] ", " ");
            ReplaceAll(text, "[]", "");
            ReplaceAll(text, "[ ]", "");
            CollapseSpaces(text);
        }

        bool EndsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }

    std::pair<std::string, std::string> ApplyChatFormat(std::string_view format,
                                                        std::string_view prefix,
                                                        std::string_view guild,
                                                        std::string_view player,
                                                        std::string_view message)
    {
        std::string fmt(format);
        if (fmt.empty())
        {
            std::string sender;
            if (!prefix.empty())
            {
                sender = std::string(prefix);
                if (!player.empty())
                {
                    sender += " ";
                }
            }
            sender += player;
            if (sender.empty())
            {
                sender = "Unknown";
            }
            return {std::move(sender), std::string(message)};
        }

        const bool includes_message = fmt.find("{message}") != std::string::npos;
        const bool spaced_colon = fmt.find(": {message}") != std::string::npos;

        ReplaceAll(fmt, "{prefix}", prefix);
        ReplaceAll(fmt, "{guild}", guild);
        ReplaceAll(fmt, "{player}", player);
        ReplaceAll(fmt, "{message}", message);
        CleanupEmptyGuildBrackets(fmt);

        if (fmt.empty())
        {
            fmt = player.empty() ? "Unknown" : std::string(player);
        }

        // Palworld always renders: [Sender]:Message
        // Message MUST be non-empty or the line is dropped / invisible (and ShowLocalServerTag
        // clears the original chat first — which made typing appear to do nothing).
        //
        // Split ChatFormat into Sender + Message. The UI's closing ']' before ':' is native
        // (same as "[BOB]: hi"). Leading space on Message preserves ": hi" spacing as "]: hi".
        std::string sender_part = fmt;
        std::string msg_part(message);

        if (includes_message && !message.empty())
        {
            const std::string suffix_spaced = std::string(": ") + std::string(message);
            const std::string suffix_tight = std::string(":") + std::string(message);
            if (EndsWith(sender_part, suffix_spaced))
            {
                sender_part.resize(sender_part.size() - suffix_spaced.size());
                msg_part = std::string(" ") + std::string(message);
            }
            else if (EndsWith(sender_part, suffix_tight))
            {
                sender_part.resize(sender_part.size() - suffix_tight.size());
                msg_part = std::string(message);
            }
        }
        else if (spaced_colon && !msg_part.empty() && msg_part.front() != ' ')
        {
            msg_part.insert(msg_part.begin(), ' ');
        }

        CollapseSpaces(sender_part);
        // Game prepends '['; drop one leading '[' from templates like "[{prefix}] ...".
        if (!sender_part.empty() && sender_part.front() == '[')
        {
            sender_part.erase(sender_part.begin());
        }
        if (sender_part.empty())
        {
            sender_part = player.empty() ? "Unknown" : std::string(player);
        }
        if (msg_part.empty())
        {
            msg_part = std::string(message);
        }

        return {std::move(sender_part), std::move(msg_part)};
    }

    std::pair<std::string, std::string> ApplyChatFormatAttributed(
        std::string_view format,
        std::string_view prefix,
        std::string_view guild,
        std::string_view player,
        std::string_view message)
    {
        const std::string fallback_player = player.empty() ? "Unknown" : std::string(player);

        // Build the tag portion with an empty player so ChatFormat's {player} slot
        // collapses out; then put those tags in front of the real message body.
        auto [tag_part, msg_part] = ApplyChatFormat(format, prefix, guild, "", message);
        if (tag_part.empty() || tag_part == "Unknown")
        {
            // No prefix/guild left — just the plain message under the real name.
            return {fallback_player, std::string(message)};
        }

        std::string tagged = "[";
        tagged += tag_part;
        if (!msg_part.empty() && msg_part.front() == ' ')
        {
            tagged += msg_part;
        }
        else if (!msg_part.empty())
        {
            tagged += " ";
            tagged += msg_part;
        }
        else
        {
            tagged += " ";
            tagged += message;
        }
        CollapseSpaces(tagged);
        if (tagged.empty())
        {
            tagged = std::string(message);
        }
        return {fallback_player, std::move(tagged)};
    }
}
