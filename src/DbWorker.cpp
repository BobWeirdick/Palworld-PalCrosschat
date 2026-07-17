#include "DbWorker.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>

#include <mysql.h>

namespace PalCrosschat
{
    namespace
    {
        class MysqlConnection
        {
        public:
            MysqlConnection() = default;
            ~MysqlConnection() { Close(); }

            MysqlConnection(const MysqlConnection&) = delete;
            MysqlConnection& operator=(const MysqlConnection&) = delete;

            bool Connect(const Config& cfg)
            {
                Close();
                m_mysql = mysql_init(nullptr);
                if (!m_mysql)
                {
                    return false;
                }

                mysql_options(m_mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");
                unsigned int timeout = 5;
                mysql_options(m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
                mysql_options(m_mysql, MYSQL_OPT_READ_TIMEOUT, &timeout);
                mysql_options(m_mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

                MYSQL* result = mysql_real_connect(
                    m_mysql,
                    cfg.mysql_host.c_str(),
                    cfg.mysql_user.c_str(),
                    cfg.mysql_password.c_str(),
                    cfg.mysql_database.c_str(),
                    static_cast<unsigned int>(cfg.mysql_port),
                    nullptr,
                    CLIENT_REMEMBER_OPTIONS);

                if (!result)
                {
                    return false;
                }
                return true;
            }

            void Close()
            {
                FreeStmts();
                if (m_mysql)
                {
                    mysql_close(m_mysql);
                    m_mysql = nullptr;
                }
            }

            MYSQL* Get() const { return m_mysql; }

            const char* Error() const
            {
                return m_mysql ? mysql_error(m_mysql) : "no handle";
            }

            unsigned int Errno() const
            {
                return m_mysql ? mysql_errno(m_mysql) : 0;
            }

            bool IsSchemaMissing() const
            {
                // ER_NO_SUCH_TABLE = 1146
                return Errno() == 1146;
            }

            MYSQL_STMT* StmtInsert() const { return m_stmt_insert; }
            MYSQL_STMT* StmtSelect() const { return m_stmt_select; }
            MYSQL_STMT* StmtCursorGet() const { return m_stmt_cursor_get; }
            MYSQL_STMT* StmtCursorUpsert() const { return m_stmt_cursor_upsert; }
            MYSQL_STMT* StmtMaxId() const { return m_stmt_max_id; }

            bool PrepareStatements()
            {
                FreeStmts();

                m_stmt_insert = mysql_stmt_init(m_mysql);
                m_stmt_select = mysql_stmt_init(m_mysql);
                m_stmt_cursor_get = mysql_stmt_init(m_mysql);
                m_stmt_cursor_upsert = mysql_stmt_init(m_mysql);
                m_stmt_max_id = mysql_stmt_init(m_mysql);

                if (!m_stmt_insert || !m_stmt_select || !m_stmt_cursor_get || !m_stmt_cursor_upsert ||
                    !m_stmt_max_id)
                {
                    FreeStmts();
                    return false;
                }

                const char* insert_sql =
                    "INSERT INTO crosschat_messages (origin, sender_name, sender_id, message) "
                    "VALUES (?, ?, ?, ?)";
                const char* select_sql =
                    "SELECT id, origin, sender_name, message FROM crosschat_messages "
                    "WHERE id > ? AND origin != ? ORDER BY id ASC LIMIT ?";
                const char* cursor_get_sql =
                    "SELECT last_id FROM crosschat_cursors WHERE consumer = ? LIMIT 1";
                const char* cursor_upsert_sql =
                    "INSERT INTO crosschat_cursors (consumer, last_id) VALUES (?, ?) "
                    "ON DUPLICATE KEY UPDATE last_id = VALUES(last_id)";
                const char* max_id_sql = "SELECT COALESCE(MAX(id), 0) FROM crosschat_messages";

                if (mysql_stmt_prepare(m_stmt_insert, insert_sql, static_cast<unsigned long>(std::strlen(insert_sql))) != 0 ||
                    mysql_stmt_prepare(m_stmt_select, select_sql, static_cast<unsigned long>(std::strlen(select_sql))) != 0 ||
                    mysql_stmt_prepare(m_stmt_cursor_get, cursor_get_sql, static_cast<unsigned long>(std::strlen(cursor_get_sql))) != 0 ||
                    mysql_stmt_prepare(m_stmt_cursor_upsert, cursor_upsert_sql, static_cast<unsigned long>(std::strlen(cursor_upsert_sql))) != 0 ||
                    mysql_stmt_prepare(m_stmt_max_id, max_id_sql, static_cast<unsigned long>(std::strlen(max_id_sql))) != 0)
                {
                    return false;
                }
                return true;
            }

            void FreeStmts()
            {
                auto free_one = [](MYSQL_STMT*& s) {
                    if (s)
                    {
                        mysql_stmt_close(s);
                        s = nullptr;
                    }
                };
                free_one(m_stmt_insert);
                free_one(m_stmt_select);
                free_one(m_stmt_cursor_get);
                free_one(m_stmt_cursor_upsert);
                free_one(m_stmt_max_id);
            }

        private:
            MYSQL* m_mysql = nullptr;
            MYSQL_STMT* m_stmt_insert = nullptr;
            MYSQL_STMT* m_stmt_select = nullptr;
            MYSQL_STMT* m_stmt_cursor_get = nullptr;
            MYSQL_STMT* m_stmt_cursor_upsert = nullptr;
            MYSQL_STMT* m_stmt_max_id = nullptr;
        };

        void BindString(MYSQL_BIND& bind, const std::string& s, unsigned long& length)
        {
            std::memset(&bind, 0, sizeof(bind));
            length = static_cast<unsigned long>(s.size());
            bind.buffer_type = MYSQL_TYPE_STRING;
            bind.buffer = const_cast<char*>(s.data());
            bind.buffer_length = length;
            bind.length = &length;
        }

        void BindI64(MYSQL_BIND& bind, int64_t& value)
        {
            std::memset(&bind, 0, sizeof(bind));
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = &value;
            bind.is_unsigned = 0;
        }

        void BindI32(MYSQL_BIND& bind, int32_t& value)
        {
            std::memset(&bind, 0, sizeof(bind));
            bind.buffer_type = MYSQL_TYPE_LONG;
            bind.buffer = &value;
            bind.is_unsigned = 0;
        }

        bool SleepInterruptible(std::stop_token stop, std::chrono::milliseconds total)
        {
            constexpr auto slice = std::chrono::milliseconds(50);
            auto remaining = total;
            while (remaining.count() > 0)
            {
                if (stop.stop_requested())
                {
                    return false;
                }
                const auto step = remaining < slice ? remaining : slice;
                std::this_thread::sleep_for(step);
                remaining -= step;
            }
            return !stop.stop_requested();
        }
    }

    DbWorker::DbWorker(Config config, OutboundQueue& outbound, InboundQueue& inbound)
        : m_config(std::move(config)), m_outbound(outbound), m_inbound(inbound)
    {
    }

    DbWorker::~DbWorker()
    {
        Stop();
    }

    void DbWorker::Start()
    {
        if (m_started.exchange(true))
        {
            return;
        }
        m_thread = std::jthread([this](std::stop_token stop) { ThreadMain(stop); });
    }

    void DbWorker::Stop()
    {
        if (!m_started.exchange(false))
        {
            // Still join if a thread object exists from a prior start that raced.
        }
        if (m_thread.joinable())
        {
            m_thread.request_stop();
            m_thread.join();
        }
    }

    void DbWorker::ThreadMain(std::stop_token stop)
    {
        MysqlConnection conn;
        int backoff_sec = 1;
        const int backoff_cap = m_config.reconnect_backoff_max_sec;
        bool was_connected = false;
        auto last_poll = std::chrono::steady_clock::now() - std::chrono::hours(1);
        bool cursor_ready = false;

        auto log_schema_missing = [&]() {
            RC::Output::send<RC::LogLevel::Error>(
                STR("[PalCrosschat] Schema not found (missing crosschat tables). "
                    "Run schema.sql from the bot project. Will retry.\n"));
        };

        auto ensure_connected = [&]() -> bool {
            if (conn.Get() && m_connected.load())
            {
                return true;
            }

            if (!conn.Connect(m_config))
            {
                if (was_connected)
                {
                    RC::Output::send<RC::LogLevel::Warning>(
                        STR("[PalCrosschat] MySQL disconnected: {}\n"),
                        RC::ensure_str(conn.Error()));
                    was_connected = false;
                }
                else
                {
                    RC::Output::send<RC::LogLevel::Warning>(
                        STR("[PalCrosschat] MySQL connect failed: {} (backoff {}s)\n"),
                        RC::ensure_str(conn.Error()),
                        backoff_sec);
                }
                m_connected = false;
                cursor_ready = false;
                return false;
            }

            if (!conn.PrepareStatements())
            {
                if (conn.IsSchemaMissing())
                {
                    log_schema_missing();
                }
                else
                {
                    RC::Output::send<RC::LogLevel::Error>(
                        STR("[PalCrosschat] Failed to prepare SQL statements: {}\n"),
                        RC::ensure_str(conn.Error()));
                }
                conn.Close();
                m_connected = false;
                cursor_ready = false;
                return false;
            }

            if (!was_connected)
            {
                RC::Output::send<RC::LogLevel::Normal>(
                    STR("[PalCrosschat] MySQL connected to {}:{}/{}\n"),
                    RC::ensure_str(m_config.mysql_host),
                    m_config.mysql_port,
                    RC::ensure_str(m_config.mysql_database));
            }
            was_connected = true;
            m_connected = true;
            backoff_sec = 1;
            return true;
        };

        auto mark_failure = [&]() {
            m_connected = false;
            cursor_ready = false;
            conn.Close();
        };

        auto init_cursor = [&]() -> bool {
            MYSQL_STMT* stmt = conn.StmtCursorGet();
            MYSQL_BIND bind_param{};
            unsigned long consumer_len = 0;
            BindString(bind_param, m_config.server_origin, consumer_len);

            if (mysql_stmt_bind_param(stmt, &bind_param) != 0 || mysql_stmt_execute(stmt) != 0)
            {
                if (conn.IsSchemaMissing() || mysql_stmt_errno(stmt) == 1146)
                {
                    log_schema_missing();
                }
                else
                {
                    RC::Output::send<RC::LogLevel::Error>(
                        STR("[PalCrosschat] Cursor SELECT failed: {}\n"),
                        RC::ensure_str(mysql_stmt_error(stmt)));
                }
                return false;
            }

            int64_t last_id = 0;
            MYSQL_BIND bind_result{};
            BindI64(bind_result, last_id);
            bool is_null = false;
            bind_result.is_null = reinterpret_cast<char*>(&is_null);

            if (mysql_stmt_bind_result(stmt, &bind_result) != 0)
            {
                return false;
            }

            const int fetch_rc = mysql_stmt_fetch(stmt);
            mysql_stmt_free_result(stmt);

            if (fetch_rc == 0)
            {
                m_cursor_id = last_id;
                cursor_ready = true;
                RC::Output::send<RC::LogLevel::Normal>(
                    STR("[PalCrosschat] Cursor loaded: consumer={} last_id={}\n"),
                    RC::ensure_str(m_config.server_origin),
                    last_id);
                return true;
            }

            // No cursor row: initialize to MAX(id) so we never replay history.
            MYSQL_STMT* max_stmt = conn.StmtMaxId();
            if (mysql_stmt_execute(max_stmt) != 0)
            {
                if (mysql_stmt_errno(max_stmt) == 1146)
                {
                    log_schema_missing();
                }
                return false;
            }

            int64_t max_id = 0;
            MYSQL_BIND max_bind{};
            BindI64(max_bind, max_id);
            if (mysql_stmt_bind_result(max_stmt, &max_bind) != 0 || mysql_stmt_fetch(max_stmt) != 0)
            {
                mysql_stmt_free_result(max_stmt);
                return false;
            }
            mysql_stmt_free_result(max_stmt);

            MYSQL_STMT* upsert = conn.StmtCursorUpsert();
            MYSQL_BIND upsert_binds[2]{};
            unsigned long c_len = 0;
            BindString(upsert_binds[0], m_config.server_origin, c_len);
            BindI64(upsert_binds[1], max_id);
            if (mysql_stmt_bind_param(upsert, upsert_binds) != 0 || mysql_stmt_execute(upsert) != 0)
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("[PalCrosschat] Cursor init INSERT failed: {}\n"),
                    RC::ensure_str(mysql_stmt_error(upsert)));
                return false;
            }

            m_cursor_id = max_id;
            cursor_ready = true;
            RC::Output::send<RC::LogLevel::Normal>(
                STR("[PalCrosschat] Cursor initialized to MAX(id)={} (no history replay)\n"),
                max_id);
            return true;
        };

        auto update_cursor = [&](int64_t new_id) -> bool {
            MYSQL_STMT* upsert = conn.StmtCursorUpsert();
            MYSQL_BIND binds[2]{};
            unsigned long c_len = 0;
            BindString(binds[0], m_config.server_origin, c_len);
            BindI64(binds[1], new_id);
            if (mysql_stmt_bind_param(upsert, binds) != 0 || mysql_stmt_execute(upsert) != 0)
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("[PalCrosschat] Cursor UPDATE failed: {}\n"),
                    RC::ensure_str(mysql_stmt_error(upsert)));
                return false;
            }
            m_cursor_id = new_id;
            return true;
        };

        auto insert_outbound = [&](const OutboundMessage& msg) -> bool {
            MYSQL_STMT* stmt = conn.StmtInsert();
            MYSQL_BIND binds[4]{};
            unsigned long lens[4]{};
            BindString(binds[0], m_config.server_origin, lens[0]);
            BindString(binds[1], msg.sender_name, lens[1]);
            BindString(binds[2], msg.sender_id, lens[2]);
            BindString(binds[3], msg.message, lens[3]);

            if (mysql_stmt_bind_param(stmt, binds) != 0 || mysql_stmt_execute(stmt) != 0)
            {
                if (mysql_stmt_errno(stmt) == 1146)
                {
                    log_schema_missing();
                }
                else
                {
                    RC::Output::send<RC::LogLevel::Error>(
                        STR("[PalCrosschat] INSERT failed: {}\n"),
                        RC::ensure_str(mysql_stmt_error(stmt)));
                }
                return false;
            }

            const uint64_t n = ++m_outbound_relay_count;
            if (n % 100 == 0)
            {
                RC::Output::send<RC::LogLevel::Normal>(
                    STR("[PalCrosschat] Outbound relayed {} messages\n"), n);
            }
            if (m_config.debug_verbose)
            {
                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("[PalCrosschat] OUT origin={} sender={} msg={}\n"),
                    RC::ensure_str(m_config.server_origin),
                    RC::ensure_str(msg.sender_name),
                    RC::ensure_str(msg.message));
            }
            return true;
        };

        auto poll_inbound = [&]() -> bool {
            int64_t cursor = m_cursor_id.load();
            int32_t limit = m_config.max_batch;
            MYSQL_STMT* stmt = conn.StmtSelect();

            MYSQL_BIND params[3]{};
            unsigned long origin_len = 0;
            BindI64(params[0], cursor);
            BindString(params[1], m_config.server_origin, origin_len);
            BindI32(params[2], limit);

            if (mysql_stmt_bind_param(stmt, params) != 0 || mysql_stmt_execute(stmt) != 0)
            {
                if (mysql_stmt_errno(stmt) == 1146)
                {
                    log_schema_missing();
                }
                else
                {
                    RC::Output::send<RC::LogLevel::Error>(
                        STR("[PalCrosschat] Poll SELECT failed: {}\n"),
                        RC::ensure_str(mysql_stmt_error(stmt)));
                }
                return false;
            }

            int64_t id = 0;
            char origin_buf[64]{};
            char sender_buf[64]{};
            char message_buf[512]{};
            unsigned long origin_rlen = 0, sender_rlen = 0, message_rlen = 0;
            bool origin_null = false, sender_null = false, message_null = false;

            MYSQL_BIND results[4]{};
            BindI64(results[0], id);

            std::memset(&results[1], 0, sizeof(MYSQL_BIND));
            results[1].buffer_type = MYSQL_TYPE_STRING;
            results[1].buffer = origin_buf;
            results[1].buffer_length = sizeof(origin_buf) - 1;
            results[1].length = &origin_rlen;
            results[1].is_null = reinterpret_cast<char*>(&origin_null);

            std::memset(&results[2], 0, sizeof(MYSQL_BIND));
            results[2].buffer_type = MYSQL_TYPE_STRING;
            results[2].buffer = sender_buf;
            results[2].buffer_length = sizeof(sender_buf) - 1;
            results[2].length = &sender_rlen;
            results[2].is_null = reinterpret_cast<char*>(&sender_null);

            std::memset(&results[3], 0, sizeof(MYSQL_BIND));
            results[3].buffer_type = MYSQL_TYPE_STRING;
            results[3].buffer = message_buf;
            results[3].buffer_length = sizeof(message_buf) - 1;
            results[3].length = &message_rlen;
            results[3].is_null = reinterpret_cast<char*>(&message_null);

            if (mysql_stmt_bind_result(stmt, results) != 0)
            {
                mysql_stmt_free_result(stmt);
                return false;
            }

            // Store results so we can update cursor per row after queue push.
            if (mysql_stmt_store_result(stmt) != 0)
            {
                mysql_stmt_free_result(stmt);
                return false;
            }

            while (true)
            {
                const int fetch_rc = mysql_stmt_fetch(stmt);
                if (fetch_rc == MYSQL_NO_DATA)
                {
                    break;
                }
                if (fetch_rc != 0 && fetch_rc != MYSQL_DATA_TRUNCATED)
                {
                    RC::Output::send<RC::LogLevel::Error>(
                        STR("[PalCrosschat] Poll fetch failed: {}\n"),
                        RC::ensure_str(mysql_stmt_error(stmt)));
                    mysql_stmt_free_result(stmt);
                    return false;
                }

                InboundMessage inbound;
                inbound.id = id;
                inbound.origin.assign(origin_buf, origin_null ? 0 : origin_rlen);
                inbound.sender_name.assign(sender_buf, sender_null ? 0 : sender_rlen);
                inbound.message.assign(message_buf, message_null ? 0 : message_rlen);

                // THREAD BOUNDARY: DB thread -> inbound queue (plain structs only).
                m_inbound.Push(std::move(inbound));

                if (!update_cursor(id))
                {
                    mysql_stmt_free_result(stmt);
                    return false;
                }

                const uint64_t n = ++m_inbound_relay_count;
                if (n % 100 == 0)
                {
                    RC::Output::send<RC::LogLevel::Normal>(
                        STR("[PalCrosschat] Inbound queued {} messages\n"), n);
                }
                if (m_config.debug_verbose)
                {
                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR("[PalCrosschat] IN id={} origin={} sender={}\n"),
                        id,
                        RC::ensure_str(std::string(origin_buf, origin_null ? 0 : origin_rlen)),
                        RC::ensure_str(std::string(sender_buf, sender_null ? 0 : sender_rlen)));
                }
            }

            mysql_stmt_free_result(stmt);
            return true;
        };

        RC::Output::send<RC::LogLevel::Normal>(STR("[PalCrosschat] DB worker started\n"));

        while (!stop.stop_requested())
        {
            if (!ensure_connected())
            {
                if (!SleepInterruptible(stop, std::chrono::seconds(backoff_sec)))
                {
                    break;
                }
                backoff_sec = (std::min)(backoff_sec * 2, backoff_cap);
                continue;
            }

            if (!cursor_ready)
            {
                if (!init_cursor())
                {
                    mark_failure();
                    if (!SleepInterruptible(stop, std::chrono::seconds(backoff_sec)))
                    {
                        break;
                    }
                    backoff_sec = (std::min)(backoff_sec * 2, backoff_cap);
                    continue;
                }
            }

            // Drain outbound queue into INSERTs.
            bool insert_ok = true;
            while (!stop.stop_requested())
            {
                auto item = m_outbound.TryPop();
                if (!item)
                {
                    break;
                }
                // THREAD BOUNDARY: outbound queue -> MySQL (DB thread only).
                if (!insert_outbound(*item))
                {
                    // Re-queue failed message at front is hard with deque API; push back.
                    m_outbound.Push(std::move(*item));
                    insert_ok = false;
                    break;
                }
            }

            if (!insert_ok)
            {
                mark_failure();
                if (!SleepInterruptible(stop, std::chrono::seconds(backoff_sec)))
                {
                    break;
                }
                backoff_sec = (std::min)(backoff_sec * 2, backoff_cap);
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto poll_due =
                now - last_poll >= std::chrono::milliseconds(m_config.poll_interval_ms);
            if (poll_due)
            {
                if (!poll_inbound())
                {
                    mark_failure();
                    if (!SleepInterruptible(stop, std::chrono::seconds(backoff_sec)))
                    {
                        break;
                    }
                    backoff_sec = (std::min)(backoff_sec * 2, backoff_cap);
                    continue;
                }
                last_poll = now;
            }

            // Short idle sleep so outbound latency stays under ~1s without busy-spinning.
            if (!SleepInterruptible(stop, std::chrono::milliseconds(50)))
            {
                break;
            }
        }

        conn.Close();
        m_connected = false;
        RC::Output::send<RC::LogLevel::Normal>(STR("[PalCrosschat] DB worker stopped\n"));
    }
}
