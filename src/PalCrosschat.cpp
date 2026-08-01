#include "Audience.h"
#include "ChatCapture.h"
#include "ChatInject.h"
#include "Config.h"
#include "DbWorker.h"
#include "Queues.h"
#include "Webhook.h"
#include "WordFilter.h"

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
        ModVersion = STR("1.87");
        ModDescription = STR("Relays Palworld chat to MySQL, injects cross-server messages, Discord !setdiscord link");
        ModAuthors = STR("ARKADE");

        m_config = LoadConfig();
        Output::send<LogLevel::Normal>(
            STR("[PalCrosschat] Version {}\n"), ModVersion);
        LogConfigSummary(m_config);

        if (!m_config.enabled)
        {
            Output::send<LogLevel::Warning>(
                STR("[PalCrosschat] Mod disabled (config invalid or incomplete). Server continues normally.\n"));
            return;
        }

        m_outbound = std::make_unique<OutboundQueue>(kQueueMax, "outbound");
        m_inbound = std::make_unique<InboundQueue>(kQueueMax, "inbound");
        m_link_jobs = std::make_unique<LinkQueue>(kQueueMax, "link_jobs");
        m_link_results = std::make_unique<LinkResultQueue>(kQueueMax, "link_results");
        m_webhook = std::make_unique<WebhookWorker>();
        m_filter = std::make_unique<WordFilter>(m_config);
        m_audience = std::make_unique<AudienceTracker>();
        m_db = std::make_unique<DbWorker>(
            m_config, *m_outbound, *m_inbound, *m_link_jobs, *m_link_results);
        m_inject = std::make_unique<ChatInject>(m_config, m_filter.get(), m_audience.get());
        m_capture = std::make_unique<ChatCapture>(m_config,
                                                  *m_outbound,
                                                  *m_link_jobs,
                                                  m_webhook.get(),
                                                  m_filter.get(),
                                                  m_inject.get(),
                                                  m_audience.get());

        if (!m_config.mute_log_webhook.empty() && !m_config.filter_patterns.empty())
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
        if (m_config.show_local_server_tag)
        {
            Output::send<LogLevel::Normal>(
                STR("[PalCrosschat] ShowLocalServerTag=true; local Global chat rebroadcast with prefix\n"));
        }
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

        // Game thread only: finish !setdiscord identity resolve (unsafe inside chat hook).
        if (m_capture)
        {
            m_capture->FlushDeferred();
        }

        // Slow audience refresh (FindAllOf + AccountName props only). Never per chat line.
        if (m_audience)
        {
            m_audience->TickRefresh();
        }

        // Game thread only: private link feedback (screen log), then chat inject.
        if (m_link_results && m_capture)
        {
            while (auto result = m_link_results->TryPop())
            {
                m_capture->DeliverLinkResult(*result);
            }
        }

        // Game thread only: flush deferred hook work, then inbound BroadcastChatMessage.
        m_inject->Drain(*m_inbound, m_config.max_broadcasts_per_tick);
    }

private:
    Config m_config{};
    std::unique_ptr<OutboundQueue> m_outbound;
    std::unique_ptr<InboundQueue> m_inbound;
    std::unique_ptr<LinkQueue> m_link_jobs;
    std::unique_ptr<LinkResultQueue> m_link_results;
    std::unique_ptr<WebhookWorker> m_webhook;
    std::unique_ptr<WordFilter> m_filter;
    std::unique_ptr<AudienceTracker> m_audience;
    std::unique_ptr<DbWorker> m_db;
    std::unique_ptr<ChatInject> m_inject;
    std::unique_ptr<ChatCapture> m_capture;
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
