#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace PalCrosschat
{
    // Strip ASCII control chars (0x00-0x1F, 0x7F), trim whitespace.
    // Returns empty if nothing left.
    std::string StripControlAndTrim(std::string_view input);

    // Truncate to max_bytes on a UTF-8 code-point boundary (never split a multibyte seq).
    std::string TruncateUtf8(std::string_view input, size_t max_bytes);

    // Full outbound/inbound sanitize: control strip, trim, empty check, 512-byte truncate.
    // Returns empty string if the message should be dropped.
    std::string SanitizeMessage(std::string_view input, size_t max_bytes = 512);

    // Like SanitizeMessage but keeps '\\n' (for multi-line server notices).
    std::string SanitizeNotice(std::string_view input, size_t max_bytes = 512);

    // True if message starts with any of the given prefixes (second-layer loop protection).
    bool StartsWithAnyPrefix(std::string_view message, const std::vector<std::string>& prefixes);
}
