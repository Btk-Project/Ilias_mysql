/**
 * @file mysql.hpp
 * @author llhsdmd (llhsdmd@gmail.com)
 * @brief mysql I/O
 * @version 0.1
 * @date 2024-12-31
 *
 * @copyright Copyright (c) 2024
 *
 */
#pragma once

#include <ilias/io/context.hpp>
#include <ilias/io/fd_utils.hpp>
#include <ilias/io/method.hpp>
#include <ilias/io/system_error.hpp>
#include <ilias/net/poller.hpp>
#include <ilias/net/sockfd.hpp>
#include <ilias/task/when_any.hpp>
#include <ilias/task/decorator.hpp>
#include <mariadb/mysql.h>

#include "../sqlerror.hpp"
#include "global.hpp"
#include "sqlopt.hpp"

ILIAS_SQL_NS_BEGIN
namespace detail {

class MySql final {
public:
    enum ShutdownType {
        SHUTDOWN_DEFAULT = ::mysql_enum_shutdown_level::SHUTDOWN_DEFAULT,
        KILL_QUERY       = ::mysql_enum_shutdown_level::KILL_QUERY,
        KILL_CONNECTION  = ::mysql_enum_shutdown_level::KILL_CONNECTION
    };

    enum ServerOption {
        MULTI_STATEMENTS_ON  = ::enum_mysql_set_option::MYSQL_OPTION_MULTI_STATEMENTS_ON,
        MULTI_STATEMENTS_OFF = ::enum_mysql_set_option::MYSQL_OPTION_MULTI_STATEMENTS_OFF,
    };

public:
    MySql();
    MySql(const MySql &) = delete;
    ~MySql();

    // connect
    [[nodiscard("Don't forget to use co_await")]]
    auto connect(std::string_view host, std::string_view user, std::string_view passwd, std::string_view db,
                 int port = 3306, std::string_view unix_socket = "", unsigned long client_flag = 0) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto resetConnection() -> IoTask<int>;
    [[nodiscard("Don't forget to use co_await")]]
    auto disconnect() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto changeUser(std::string_view user, std::string_view passwd, std::string_view db) -> IoTask<bool>;
    [[nodiscard("Don't forget to use co_await")]]
    auto dumpDebugInfo() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto setServerOption(ServerOption option) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto setCharacterSet(std::string_view csname) -> IoTask<void>;

    // mysql database
    [[nodiscard("Don't forget to use co_await")]]
    auto selectDb(std::string_view db) -> IoTask<void>;

    [[nodiscard("Don't forget to use co_await")]]
    auto query(std::string_view sql) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto commit() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto autoCommit(bool autoMode) -> IoTask<void>;

    [[nodiscard("Don't forget to use co_await")]]
    auto rollback() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto listFields(MYSQL_RES **ret, std::string_view table, std::string_view wildcard = "*") -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto sendQuery(std::string_view sql) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto refresh(uint32_t refreshOptions) -> IoTask<void>; // FIXME: where has defines abort options?
    [[nodiscard("Don't forget to use co_await")]]
    auto kill(uint64_t pid) -> IoTask<void>; // FIXME: is kill self?
    [[nodiscard("Don't forget to use co_await")]]
    auto ping() -> IoTask<int>;
    [[nodiscard("Don't forget to use co_await")]]
    auto stat() -> IoTask<const char *>;
    [[nodiscard("Don't forget to use co_await")]]
    auto readQueryResult() -> IoTask<void>;

    // useResult -> fetchRow -> freeResult
    [[nodiscard("Don't forget to use co_await")]]
    auto useResult() -> IoTask<MYSQL_RES *>;
    [[nodiscard("Don't forget to use co_await")]]
    auto nextResult() -> IoTask<void>;
    // store result might use too much memory in retrieving a large result set all at once.
    [[nodiscard("Don't forget to use co_await")]]
    auto storeResult(MYSQL_RES **result) -> IoTask<void>;
    auto fieldCount() -> std::size_t;

    // shutdown
    [[nodiscard("Don't forget to use co_await")]]
    auto shutdown(ShutdownType shutdownType) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto pollStatus(int &status, uint32_t pollEvents = 0) -> IoTask<void>;

    // stmt
    auto stmtInit() -> MYSQL_STMT *;
    auto setOpt(const sqlopt::OptionBase &opt) -> int;
    auto close() -> void;
    auto lastError() -> SqlError;
    auto lastErrorMessage() -> const char *;

    bool operator==(MySql &other);

private:
    IoContext *mCtxt = nullptr;
    MYSQL      mMysql;
    Poller     mPoller;
};

inline MySql::MySql() {
    mCtxt = IoContext::currentThread();
    if (mCtxt == nullptr) {
        ILIAS_ERROR("sql", "no io context in current thread");
        return;
    }
    if (mysql_init(&mMysql) == nullptr) {
        ILIAS_ERROR("sql", "mysql init failed");
    }
    auto ret = mysql_options(&mMysql, MYSQL_OPT_NONBLOCK, 0);
    if (ret != 0) {
        ILIAS_ERROR("sql", "mysql set option failed, {}", SqlError(static_cast<SqlError::Code>(ret)).message());
    }
}

inline bool MySql::operator==(MySql &other) {
    return &mMysql == &other.mMysql;
}

inline MySql::~MySql() {
    close();
}

inline auto MySql::pollStatus(int &status, uint32_t pollEvents) -> IoTask<void> {
    if (!mPoller) {
        co_return Unexpected<Error>(Error::Unknown);
    }
    uint64_t timeOut = 0;
    if (status & MYSQL_WAIT_TIMEOUT) {
        timeOut = mysql_get_timeout_value_ms(&mMysql);
    }
    std::string events;
    if (pollEvents == 0) {
        if (status & MYSQL_WAIT_READ) {
            pollEvents |= POLLIN;
            events += "POLLIN ";
        }
        if (status & MYSQL_WAIT_WRITE) {
            pollEvents |= POLLOUT;
            events += "POLLOUT ";
        }
        if (status & MYSQL_WAIT_EXCEPT) {
            pollEvents |= POLLPRI;
            events += "POLLPRI";
        }
    }
    ILIAS_TRACE("sql", "poll events: {}", events);
    Result<unsigned int> ret;
    if (timeOut == 0) {
        auto ret = co_await mPoller.poll(pollEvents);
        if (!ret) {
            ILIAS_ERROR("sql", "poll failed, {}", ret.error().message());
            co_return Unexpected<Error>(ret.error());
        }
    }
    else {
        auto ret = co_await (mPoller.poll(pollEvents) | setTimeout(std::chrono::milliseconds(timeOut)));
        if (!ret) {
            ILIAS_ERROR("sql", "poll failed, no result in poll.");
            co_return Unexpected<Error>(ret.error());
        }
    }
    if (status & MYSQL_WAIT_TIMEOUT) {
        status = MYSQL_WAIT_TIMEOUT;
    }
    if (ret.value_or(0) & POLLIN) {
        status |= MYSQL_WAIT_READ;
    }
    if (ret.value_or(0) | POLLOUT) {
        status |= MYSQL_WAIT_WRITE;
    }
    if (ret.value_or(0) | POLLPRI) {
        status |= MYSQL_WAIT_EXCEPT;
    }
    co_return {};
}

// please make poller when xxx_start() return not 0.
#define SQL_PRIVATE_MAKE_POLLER                                                                                        \
    {                                                                                                                  \
        if (mCtxt == nullptr) {                                                                                        \
            co_return Unexpected<Error>(Error::Unknown);                                                               \
        }                                                                                                              \
        auto fd = mysql_get_socket(&mMysql);                                                                           \
        if (fd == (decltype(fd))MARIADB_INVALID_SOCKET) {                                                              \
            ILIAS_ERROR("sql", "get socket failed");                                                                   \
            co_return Unexpected<Error>(Error::Unknown);                                                               \
        }                                                                                                              \
        if (!mPoller || (mPoller.fd() != (fd_t)fd)) {                                                                  \
            mPoller = Poller(*mCtxt, (fd_t)fd, IoDescriptor::Socket);                                                  \
            if (!mPoller) {                                                                                            \
                ILIAS_ERROR("sql", "add fd({}) to IoContext failed.", fd);                                             \
                co_return Unexpected<Error>(Error::Unknown);                                                           \
            }                                                                                                          \
        }                                                                                                              \
    }

#define SQL_PRIVATE_SYNC_CODE(OutP, MysqlFunc, ...)                                                                    \
    auto status = MysqlFunc##_start(&OutP, &mMysql, ##__VA_ARGS__);                                                    \
    if (status) {                                                                                                      \
        SQL_PRIVATE_MAKE_POLLER                                                                                        \
        while (status) {                                                                                               \
            ILIAS_TRACE("sql", "{} waiting for status {}", #MysqlFunc, status);                                        \
            auto pret = co_await pollStatus(status);                                                                   \
            status    = MysqlFunc##_cont(&OutP, &mMysql, status);                                                      \
            if (!pret) {                                                                                               \
                co_return Unexpected<Error>(pret.error());                                                             \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    auto _check = [](auto p) {                                                                                         \
        if constexpr (std::is_pointer_v<decltype(p)>) {                                                                \
            if (p != nullptr)                                                                                          \
                return true;                                                                                           \
        }                                                                                                              \
        else if constexpr (std::is_same_v<decltype(p), my_bool>) {                                                     \
            if (p)                                                                                                     \
                return true;                                                                                           \
        }                                                                                                              \
        else if constexpr (std::is_integral_v<decltype(p)>) {                                                          \
            if (p == 0)                                                                                                \
                return true;                                                                                           \
        }                                                                                                              \
        else {                                                                                                         \
            ILIAS_ERROR("sql", "unknow type?");                                                                        \
            return false;                                                                                              \
        }                                                                                                              \
        return false;                                                                                                  \
    };                                                                                                                 \
    if (!_check(OutP)) {                                                                                               \
        auto error   = mysql_error(&mMysql);                                                                           \
        auto errCode = mysql_errno(&mMysql);                                                                           \
        if (errCode != 0)                                                                                              \
            ILIAS_ERROR("sql", "{} failed, error({}): {}", #MysqlFunc, errCode, error);                                \
        co_return Unexpected<Error>(SqlError::Code(errCode));                                                          \
    }

inline auto MySql::connect(std::string_view host, std::string_view user, std::string_view passwd, std::string_view db,
                           int port, std::string_view unix_socket, unsigned long client_flag) -> IoTask<void> {
    MYSQL      *ret;
    std::string targetHost(host);
    std::string targetUser(user);
    std::string targetPasswd(passwd);
    std::string targetDb(db);

    SQL_PRIVATE_SYNC_CODE(ret, mysql_real_connect, host == "" ? nullptr : targetHost.c_str(), targetUser.c_str(),
                          targetPasswd.c_str(), targetDb.c_str(), port,
                          unix_socket == "" ? nullptr : unix_socket.data(), client_flag)
    co_return {};
}

inline auto MySql::resetConnection() -> IoTask<int> {

    // this ret is what.
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_reset_connection);
    co_return {};
}

inline auto MySql::changeUser(std::string_view user, std::string_view passwd, std::string_view db) -> IoTask<bool> {

    // this ret is what.
    my_bool     ret;
    std::string localUser(user);
    std::string localPasswd(passwd);
    std::string localDb(db);
    SQL_PRIVATE_SYNC_CODE(ret, mysql_change_user, localUser.c_str(), localPasswd.c_str(), localDb.c_str())
    co_return {};
}

inline auto MySql::close() -> void {
    ILIAS_TRACE("sql", "close mysql connection");
    mPoller.close();
    mysql_close(&mMysql);
}

inline auto MySql::dumpDebugInfo() -> IoTask<void> {
    // this ret is what.
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_dump_debug_info)
    co_return {};
}

inline auto MySql::setServerOption(ServerOption option) -> IoTask<void> {

    // this ret is what.
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_set_server_option, static_cast<enum_mysql_set_option>(option))
    co_return {};
}

inline auto MySql::setCharacterSet(std::string_view csname) -> IoTask<void> {
    // this ret is what.
    int         ret;
    std::string localCsname(csname);
    SQL_PRIVATE_SYNC_CODE(ret, mysql_set_character_set, localCsname.c_str())
    co_return {};
}

inline auto MySql::selectDb(std::string_view db) -> IoTask<void> {

    // this ret is what.
    int         ret;
    std::string localDb(db);
    SQL_PRIVATE_SYNC_CODE(ret, mysql_select_db, localDb.c_str())
    co_return {};
}

inline auto MySql::query(std::string_view sql) -> IoTask<void> {
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_real_query, sql.data(), (uint32_t)sql.size())
    // can use mysql_num_fields() to determine if a statement returned a result set.
    co_return {};
}

inline auto MySql::commit() -> IoTask<void> {

    my_bool ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_commit)
    co_return {};
}

inline auto MySql::disconnect() -> IoTask<void> {
    mPoller.close();
    auto status = mysql_close_start(&mMysql);
    if (status) {
        SQL_PRIVATE_MAKE_POLLER
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await pollStatus(status);
            status    = mysql_close_cont(&mMysql, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    ILIAS_TRACE("sql", "close mysql connection");
    co_return {};
}

inline auto MySql::autoCommit(bool autoMode) -> IoTask<void> {
    // TODO: need query "select @@autocommit;" and get the result.
    my_bool ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_autocommit, autoMode)
    co_return {};
}

inline auto MySql::nextResult() -> IoTask<void> {
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_next_result)
    co_return {};
}

inline auto MySql::fieldCount() -> std::size_t {
    return mysql_field_count(&mMysql);
}

inline auto MySql::useResult() -> IoTask<MYSQL_RES *> {
    co_return mysql_use_result(&mMysql);
}

inline auto MySql::storeResult(MYSQL_RES **result) -> IoTask<void> {
    SQL_PRIVATE_SYNC_CODE(*result, mysql_store_result)
    co_return {};
}

inline auto MySql::rollback() -> IoTask<void> {
    my_bool ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_rollback)
    co_return {};
}

inline auto MySql::listFields(MYSQL_RES **ret, std::string_view table, std::string_view wildcard) -> IoTask<void> {
    std::string localTable(table);
    std::string localWildcard(wildcard);
    SQL_PRIVATE_SYNC_CODE(*ret, mysql_list_fields, localTable.c_str(), localWildcard.c_str());
    co_return {};
}

inline auto MySql::sendQuery(std::string_view sql) -> IoTask<void> {
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_real_query, sql.data(), (uint32_t)sql.size())
    co_return {};
}

inline auto MySql::refresh(uint32_t refreshOptions) -> IoTask<void> {
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_refresh, refreshOptions)
    co_return {};
}

inline auto MySql::kill(uint64_t pid) -> IoTask<void> {
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_kill, pid)
    co_return {};
}

inline auto MySql::ping() -> IoTask<int> {
    int ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_ping)
    co_return ret;
}

inline auto MySql::stat() -> IoTask<const char *> {
    const char *ret; // FIXME: this var's live who knows ?
    SQL_PRIVATE_SYNC_CODE(ret, mysql_stat)
    co_return ret;
}

inline auto MySql::readQueryResult() -> IoTask<void> {
    my_bool ret;
    SQL_PRIVATE_SYNC_CODE(ret, mysql_read_query_result)
    co_return {};
}

inline auto MySql::setOpt(const sqlopt::OptionBase &opt) -> int {
    return opt.setopt(mMysql);
}

inline auto MySql::stmtInit() -> MYSQL_STMT * {
    return mysql_stmt_init(&mMysql);
}

inline auto MySql::lastError() -> SqlError {
    return (SqlError::Code)mysql_errno(&mMysql);
}

inline auto MySql::lastErrorMessage() -> const char * {
    return mysql_error(&mMysql);
}

#undef SQL_PRIVATE_MAKE_POLLER
#undef SQL_PRIVATE_SYNC_CODE
#undef MYSQL_OPTION_TABLE
} // namespace detail
ILIAS_SQL_NS_END
