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
    }

    std::pair<std::string, std::string> ApplyChatFormat(std::string_view format,
                                                        std::string_view prefix,
                                                        std::string_view guild,
                                                        std::string_view player,
                                                        std::string_view message)
    {
        std::string msg_part(message);

        if (format.empty())
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
            return {std::move(sender), std::move(msg_part)};
        }

        // Split on the {message} placeholder BEFORE substitution. Peeling the filled
        // message text with EndsWith was brittle: trailing junk after {message} (e.g. ']'),
        // or CleanupEmptyGuildBrackets mutating the body, left the full line in Sender and
        // doubled the chat under Palworld's native [Sender]:Message wrap.
        constexpr std::string_view kMessagePh = "{message}";
        const size_t msg_ph = format.find(kMessagePh);

        std::string sender_part;
        if (msg_ph != std::string::npos)
        {
            sender_part.assign(format.substr(0, msg_ph));
            // Anything after {message} is ignored for both fields (misplaced ']').

            // Move ": " / ":" into Message spacing so the UI still shows "]: hi".
            if (sender_part.size() >= 2 &&
                sender_part.compare(sender_part.size() - 2, 2, ": ") == 0)
            {
                sender_part.resize(sender_part.size() - 2);
                if (!msg_part.empty() && msg_part.front() != ' ')
                {
                    msg_part.insert(msg_part.begin(), ' ');
                }
            }
            else if (!sender_part.empty() && sender_part.back() == ':')
            {
                sender_part.pop_back();
            }
        }
        else
        {
            sender_part.assign(format);
        }

        ReplaceAll(sender_part, "{prefix}", prefix);
        ReplaceAll(sender_part, "{guild}", guild);
        ReplaceAll(sender_part, "{player}", player);
        CleanupEmptyGuildBrackets(sender_part);

        // Game prepends '['; drop one leading '[' from templates like "[{prefix}] ...".
        if (!sender_part.empty() && sender_part.front() == '[')
        {
            sender_part.erase(sender_part.begin());
        }
        if (sender_part.empty())
        {
            sender_part = player.empty() ? "Unknown" : std::string(player);
        }
        // Message MUST be non-empty or the line is dropped / invisible (ShowLocalServerTag
        // clears the original first — empty Message made typing appear to do nothing).
        if (msg_part.empty())
        {
            msg_part = std::string(message);
        }

        return {std::move(sender_part), std::move(msg_part)};
    }

    std::pair<std::string, std::string> ApplyChatFormatAttributed(
        std::string_view /*format*/,
        std::string_view /*prefix*/,
        std::string_view /*guild*/,
        std::string_view player,
        std::string_view message)
    {
        // Xbox / unknown-platform path: plain body + real SenderPlayerUId so the client
        // can attribute the line. Do not inject ChatFormat tags (those need nil uid and
        // get masked on Xbox). Renders as [PlayerName]:message.
        const std::string sender = player.empty() ? "Unknown" : std::string(player);
        return {sender, std::string(message)};
    }
}
