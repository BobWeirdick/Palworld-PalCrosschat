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
    //
    // Split is done on the {message} placeholder before substitution so trailing
    // template junk (or cleanup of the filled body) cannot leave the chat text in Sender.
    std::pair<std::string, std::string> ApplyChatFormat(std::string_view format,
                                                        std::string_view prefix,
                                                        std::string_view guild,
                                                        std::string_view player,
                                                        std::string_view message);

    // Xbox path with a real SenderPlayerUId. Plain native chat only:
    // [PlayerName]: hello — no prefix/guild tags.
    std::pair<std::string, std::string> ApplyChatFormatAttributed(
        std::string_view format,
        std::string_view prefix,
        std::string_view guild,
        std::string_view player,
        std::string_view message);
}
