#include "Sanitize.h"

#include <cctype>

namespace PalCrosschat
{
    std::string StripControlAndTrim(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());
        for (unsigned char c : input)
        {
            if (c < 0x20 || c == 0x7F)
            {
                continue;
            }
            out.push_back(static_cast<char>(c));
        }

        size_t start = 0;
        while (start < out.size() && std::isspace(static_cast<unsigned char>(out[start])))
        {
            ++start;
        }
        size_t end = out.size();
        while (end > start && std::isspace(static_cast<unsigned char>(out[end - 1])))
        {
            --end;
        }
        return out.substr(start, end - start);
    }

    std::string TruncateUtf8(std::string_view input, size_t max_bytes)
    {
        if (input.size() <= max_bytes)
        {
            return std::string(input);
        }

        size_t cut = max_bytes;
        // Walk back over continuation bytes (10xxxxxx) so we do not split a code point.
        while (cut > 0 && (static_cast<unsigned char>(input[cut]) & 0xC0) == 0x80)
        {
            --cut;
        }
        return std::string(input.substr(0, cut));
    }

    std::string SanitizeMessage(std::string_view input, size_t max_bytes)
    {
        std::string cleaned = StripControlAndTrim(input);
        if (cleaned.empty())
        {
            return {};
        }
        return TruncateUtf8(cleaned, max_bytes);
    }

    bool StartsWithAnyPrefix(std::string_view message, const std::vector<std::string>& prefixes)
    {
        for (const auto& prefix : prefixes)
        {
            if (prefix.empty())
            {
                continue;
            }
            if (message.size() >= prefix.size() && message.compare(0, prefix.size(), prefix) == 0)
            {
                return true;
            }
        }
        return false;
    }
}
