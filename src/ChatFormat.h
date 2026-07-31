#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace PalCrosschat
{
    // Apply ChatFormat placeholders: {prefix}, {guild}, {player}, {message}.
    // Empty guild removes dangling "[]". Returns {sender_field, message_field}.
    // Message is always non-empty when the input message is (empty Message makes chat
    // invisible). Palworld still wraps as [Sender]:Message, so lines look like
    // "[EU] [Guild] Bob]: hi" — the ']' before ':' is the game's name closer.
    std::pair<std::string, std::string> ApplyChatFormat(std::string_view format,
                                                        std::string_view prefix,
                                                        std::string_view guild,
                                                        std::string_view player,
                                                        std::string_view message);

    // Same placeholders, but for chat that carries a real SenderPlayerUId (console /
    // Xbox attribution). The game then ignores our Sender string and renders the
    // resolved player name, so prefix/guild tags are moved into the Message field:
    //   [PlayerName]: [EU] [Guild] hello
    // Returns {player_name, tagged_message}.
    std::pair<std::string, std::string> ApplyChatFormatAttributed(
        std::string_view format,
        std::string_view prefix,
        std::string_view guild,
        std::string_view player,
        std::string_view message);
}
