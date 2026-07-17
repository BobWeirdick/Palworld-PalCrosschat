#pragma once

#include "Config.h"
#include "Queues.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace PalCrosschat
{
    // Owns the MySQL connection exclusively on a background jthread.
    // Game thread may only touch the queues (push outbound / pop inbound).
    class DbWorker
    {
    public:
        DbWorker(Config config, OutboundQueue& outbound, InboundQueue& inbound);
        ~DbWorker();

        DbWorker(const DbWorker&) = delete;
        DbWorker& operator=(const DbWorker&) = delete;

        void Start();
        void Stop(); // signals stop_token and joins

        uint64_t OutboundRelayCount() const { return m_outbound_relay_count.load(); }
        uint64_t InboundRelayCount() const { return m_inbound_relay_count.load(); }
        int64_t CursorId() const { return m_cursor_id.load(); }
        bool IsConnected() const { return m_connected.load(); }

    private:
        void ThreadMain(std::stop_token stop);

        Config m_config;
        OutboundQueue& m_outbound;
        InboundQueue& m_inbound;

        std::jthread m_thread;
        std::atomic<bool> m_started{false};

        std::atomic<uint64_t> m_outbound_relay_count{0};
        std::atomic<uint64_t> m_inbound_relay_count{0};
        std::atomic<int64_t> m_cursor_id{-1};
        std::atomic<bool> m_connected{false};
    };
}
