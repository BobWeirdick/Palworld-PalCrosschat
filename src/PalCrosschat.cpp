#include "ChatCapture.h"
#include "ChatInject.h"
#include "Config.h"
#include "DbWorker.h"
#include "Queues.h"
#include "Webhook.h"

#include <memory>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>

using namespace RC;
using namespace PalCrosschat;

class PalCrosschatMod : public CppUserModBase
{
public:
    PalCrosschatMod() : CppUserModBase()
    {
        ModName = STR("PalCrosschat");
        ModVersion = STR("1.1.0");
        ModDescription = STR("Relays Palworld chat to a shared MySQL database and injects cross-server messages");
        ModAuthors = STR("ARKADE");

        m_config = LoadConfig();
        LogConfigSummary(m_config);

        if (!m_config.enabled)
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Mod disabled (config invalid or incomplete). Server continues normally.\n"));
            return;
        }

        m_outbound = std::make_unique<OutboundQueue>(kQueueMax, "outbound");
        m_inbound = std::make_unique<InboundQueue>(kQueueMax, "inbound");
        m_webhook = std::make_unique<WebhookWorker>();
        m_db = std::make_unique<DbWorker>(m_config, *m_outbound, *m_inbound);
        m_capture = std::make_unique<ChatCapture>(m_config, *m_outbound, m_webhook.get());
        m_inject = std::make_unique<ChatInject>(m_config);

        if (!m_config.mute_log_webhook.empty() && !m_config.blocked_words.empty())
        {
            m_webhook->Start();
        }

        m_db->Start();
        Output::send<LogLevel::Normal>(STR("[PalCrosschat] Init complete; waiting for Unreal\n"));
    }

    ~PalCrosschatMod() override
    {
        if (m_capture)
        {
            m_capture->Unregister();
        }
        if (m_db)
        {
            m_db->Stop();
        }
        if (m_webhook)
        {
            m_webhook->Stop();
        }
        Output::send<LogLevel::Normal>(STR("[PalCrosschat] Unloaded\n"));
    }

    auto on_unreal_init() -> void override
    {
        if (!m_config.enabled || !m_capture)
        {
            return;
        }

        m_capture->Register();
        Output::send<LogLevel::Normal>(
            STR("[PalCrosschat] Unreal ready; cursor={} connected={}\n"),
            m_db ? m_db->CursorId() : static_cast<int64_t>(-1),
            (m_db && m_db->IsConnected()) ? STR("yes") : STR("pending"));
    }

    auto on_update() -> void override
    {
        if (!m_config.enabled || !m_inject || !m_inbound)
        {
            return;
        }

        // Game thread only: drain inbound queue and call BroadcastChatMessage.
        m_inject->Drain(*m_inbound, m_config.max_broadcasts_per_tick);
    }

private:
    Config m_config{};
    std::unique_ptr<OutboundQueue> m_outbound;
    std::unique_ptr<InboundQueue> m_inbound;
    std::unique_ptr<WebhookWorker> m_webhook;
    std::unique_ptr<DbWorker> m_db;
    std::unique_ptr<ChatCapture> m_capture;
    std::unique_ptr<ChatInject> m_inject;
};

#define PAL_CROSSCHAT_API __declspec(dllexport)
extern "C"
{
    PAL_CROSSCHAT_API CppUserModBase* start_mod()
    {
        return new PalCrosschatMod();
    }

    PAL_CROSSCHAT_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
