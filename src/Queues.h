#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include <DynamicOutput/DynamicOutput.hpp>
#include <String/StringType.hpp>

namespace PalCrosschat
{
    // Plain structs only. No UE types. Thread-boundary safe.

    struct OutboundMessage
    {
        std::string sender_name;
        std::string sender_id;
        std::string guild_name;
        std::string message;
        // crosschat_messages.category (0=global, 1=guild, 2=say).
        uint8_t category = 0;
    };

    struct InboundMessage
    {
        int64_t id = 0;
        std::string origin;
        std::string sender_name;
        // Sender's PlayerUId on the origin server; empty for Discord rows.
        std::string sender_id;
        std::string guild_name;
        std::string message;
    };

    struct WebhookJob
    {
        std::string webhook_url;
        std::string content;
    };

    // Discord link: game thread pushes LinkJob; DB thread pushes LinkResult notice.
    struct LinkJob
    {
        std::string connect_code;
        std::string platform;          // steam / gdk / ps5
        std::string user_id;           // bare id
        std::string platform_user_id;  // steam_… / gdk_… / ps5_…
        std::string player_name;
    };

    struct LinkResult
    {
        std::string notice;
        std::string platform_user_id; // delivers private screen-log to the requester
    };

    template <typename T>
    class BoundedQueue
    {
    public:
        explicit BoundedQueue(size_t max_size, const char* name)
            : m_max(max_size), m_name(name)
        {
        }

        // Returns true if pushed. Drops oldest if full.
        bool Push(T item)
        {
            std::lock_guard lock(m_mutex);
            if (m_deque.size() >= m_max)
            {
                m_deque.pop_front();
                ++m_drops;
                if (m_drops == 1 || m_drops % 50 == 0)
                {
                    RC::Output::send<RC::LogLevel::Warning>(
                        STR("[PalCrosschat] Queue full; dropped oldest (total drops={})\n"),
                        m_drops);
                }
            }
            m_deque.push_back(std::move(item));
            return true;
        }

        std::optional<T> TryPop()
        {
            std::lock_guard lock(m_mutex);
            if (m_deque.empty())
            {
                return std::nullopt;
            }
            T item = std::move(m_deque.front());
            m_deque.pop_front();
            return item;
        }

        size_t Size() const
        {
            std::lock_guard lock(m_mutex);
            return m_deque.size();
        }

        uint64_t DropCount() const
        {
            std::lock_guard lock(m_mutex);
            return m_drops;
        }

    private:
        mutable std::mutex m_mutex;
        std::deque<T> m_deque;
        size_t m_max;
        const char* m_name;
        uint64_t m_drops = 0;
    };

    constexpr size_t kQueueMax = 500;

    using OutboundQueue = BoundedQueue<OutboundMessage>;
    using InboundQueue = BoundedQueue<InboundMessage>;
    using WebhookQueue = BoundedQueue<WebhookJob>;
    using LinkQueue = BoundedQueue<LinkJob>;
    using LinkResultQueue = BoundedQueue<LinkResult>;
}
