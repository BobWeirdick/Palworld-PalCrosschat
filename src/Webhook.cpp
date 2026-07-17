#include "Webhook.h"

#include <chrono>
#include <string>
#include <thread>

#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace PalCrosschat
{
    namespace
    {
        struct ParsedUrl
        {
            bool https = true;
            std::wstring host;
            INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
            std::wstring path;
            bool valid = false;
        };

        ParsedUrl ParseUrl(const std::string& url)
        {
            ParsedUrl result{};
            const int wideLen =
                MultiByteToWideChar(CP_UTF8, 0, url.c_str(), static_cast<int>(url.size()), nullptr, 0);
            if (wideLen <= 0)
            {
                return result;
            }

            std::wstring wide(static_cast<size_t>(wideLen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, url.c_str(), static_cast<int>(url.size()), wide.data(), wideLen);

            URL_COMPONENTSW comps{};
            comps.dwStructSize = sizeof(comps);
            comps.dwSchemeLength = static_cast<DWORD>(-1);
            comps.dwHostNameLength = static_cast<DWORD>(-1);
            comps.dwUrlPathLength = static_cast<DWORD>(-1);
            comps.dwExtraInfoLength = static_cast<DWORD>(-1);

            if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &comps))
            {
                return result;
            }

            result.https = (comps.nScheme == INTERNET_SCHEME_HTTPS);
            result.port = comps.nPort;
            result.host.assign(comps.lpszHostName, comps.dwHostNameLength);

            std::wstring path;
            if (comps.lpszUrlPath && comps.dwUrlPathLength > 0)
            {
                path.assign(comps.lpszUrlPath, comps.dwUrlPathLength);
            }
            if (comps.lpszExtraInfo && comps.dwExtraInfoLength > 0)
            {
                path.append(comps.lpszExtraInfo, comps.dwExtraInfoLength);
            }
            if (path.empty())
            {
                path = L"/";
            }
            result.path = std::move(path);
            result.valid = !result.host.empty();
            return result;
        }

        std::string EscapeJsonString(const std::string& input)
        {
            std::string out;
            out.reserve(input.size() + 16);
            for (const char ch : input)
            {
                switch (ch)
                {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += ch;
                    break;
                }
            }
            return out;
        }
    }

    bool PostDiscordWebhookSync(const std::string& webhook_url, const std::string& content)
    {
        if (webhook_url.empty() || content.empty())
        {
            return false;
        }

        const ParsedUrl url = ParseUrl(webhook_url);
        if (!url.valid)
        {
            return false;
        }

        const std::string body = std::string("{\"content\":\"") + EscapeJsonString(content) + "\"}";

        HINTERNET session = WinHttpOpen(
            L"PalCrosschat/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
        {
            return false;
        }

        WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);

        HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        const DWORD flags = url.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(
            connect,
            L"POST",
            url.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags);
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        const wchar_t* headers = L"Content-Type: application/json\r\n";
        const BOOL sent = WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1),
            (LPVOID)body.data(),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0);

        bool ok = false;
        if (sent && WinHttpReceiveResponse(request, nullptr))
        {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            if (WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status,
                    &statusSize,
                    WINHTTP_NO_HEADER_INDEX))
            {
                ok = (status >= 200 && status < 300);
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    WebhookWorker::WebhookWorker() = default;

    WebhookWorker::~WebhookWorker()
    {
        Stop();
    }

    void WebhookWorker::Start()
    {
        if (m_started.exchange(true))
        {
            return;
        }
        m_thread = std::jthread([this](std::stop_token stop) { ThreadMain(stop); });
    }

    void WebhookWorker::Stop()
    {
        if (!m_started.exchange(false))
        {
            return;
        }
        if (m_thread.joinable())
        {
            m_thread.request_stop();
            m_thread.join();
        }
    }

    void WebhookWorker::Enqueue(std::string webhook_url, std::string content)
    {
        if (webhook_url.empty() || content.empty())
        {
            return;
        }
        m_queue.Push(WebhookJob{std::move(webhook_url), std::move(content)});
    }

    void WebhookWorker::ThreadMain(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            auto job = m_queue.TryPop();
            if (!job)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            PostDiscordWebhookSync(job->webhook_url, job->content);
        }
    }
}
