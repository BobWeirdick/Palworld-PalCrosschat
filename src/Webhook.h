#pragma once

#include "Queues.h"

#include <atomic>
#include <string>
#include <thread>

namespace PalCrosschat
{
    class WebhookWorker
    {
    public:
        WebhookWorker();
        ~WebhookWorker();

        void Start();
        void Stop();

        void Enqueue(std::string webhook_url, std::string content);

    private:
        void ThreadMain(std::stop_token stop);

        WebhookQueue m_queue{kQueueMax, "webhook"};
        std::jthread m_thread;
        std::atomic<bool> m_started{false};
    };

    bool PostDiscordWebhookSync(const std::string& webhook_url, const std::string& content);
}
