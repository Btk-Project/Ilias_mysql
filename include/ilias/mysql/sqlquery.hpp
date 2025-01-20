/**
 * @file sqlquery.hpp
 * @author llhsdmd(llhsdmd@gmail.com)
 * @brief
 * @version 0.1
 * @date 2025-01-09
 *
 * @copyright Copyright (c) 2025
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
#include <mariadb/mysql.h>
#include <mariadb/mysqld_error.h>

#include "detail/global.hpp"
#include "detail/mysql.hpp"
#include "detail/typehlep.hpp"
#include "sqldatabase.hpp"

ILIAS_SQL_NS_BEGIN

struct SqlDate : public MYSQL_TIME {
    inline SqlDate(int year = 0, int month = 0, int day = 0, int hour = 0, int minute = 0, int second = 0) {
        setTime(year, month, day, hour, minute, second);
    }
    inline SqlDate(struct tm *timeinfo) { setTime(timeinfo); }
    inline SqlDate(std::chrono::system_clock::time_point tp) { setTime(tp); }
    inline SqlDate(std::chrono::milliseconds timestamp) { setTime(timestamp); }

    auto setTime(std::chrono::system_clock::time_point tp) -> void;
    auto setTime(std::chrono::milliseconds timestamp) -> void;
    auto setTime(int year, int month, int day, int hour, int minute, int second) -> void;
    auto setTime(struct tm *timeinfo) -> void;
};

class SqlQuery {
public:
    SqlQuery(SqlDatabase &mysql);
    ~SqlQuery();

    SqlQuery(SqlQuery &&) noexcept;
    SqlQuery &operator=(SqlQuery &&) noexcept;

    SqlQuery(const SqlQuery &)            = delete;
    SqlQuery &operator=(const SqlQuery &) = delete;

    [[nodiscard("Don't forget to use co_await")]]
    auto execute(std::string_view query) -> IoTask<void>;

    [[nodiscard("Don't forget to use co_await")]]
    auto prepare(std::string_view query) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto execute() -> IoTask<void>;
    ///> bind TINYINT
    auto bind(int index, signed char value) -> SqlError;
    ///> bind SMALLINT
    auto bind(int index, short int value) -> SqlError;
    ///> bind INT
    auto bind(int index, int value) -> SqlError;
    ///> bind BIGINT / LONGLONG
    auto bind(int index, long long int value) -> SqlError;
    ///> bind FLOAT
    auto bind(int index, float value) -> SqlError;
    ///> bind DOUBLE
    auto bind(int index, double value) -> SqlError;
    ///> bind TEXT, CHAR, VARCHAR
    auto bind(int index, const std::string &value) -> SqlError;
    ///> bind TIME, DATE, DATETIME, TIMESTAMP
    auto bind(int index, SqlDate value) -> SqlError;
    ///> bind TEXT, CHAR, VARCHAR
    auto bind(int index, const std::u8string &value) -> SqlError;
    ///> bind value
    template <detail::BindAble<SqlQuery> T>
    auto bind(const std::string &name, const T &value) -> SqlError;

    ///> bind TEXT, CHAR, VARCHAR, this api will not copy, Please ensure that the data is valid during execute.
    auto bindView(int index, std::string_view value) -> SqlError;
    ///> bind TEXT, CHAR, VARCHAR, this api will not copy, Please ensure that the data is valid during execute.
    auto bindView(int index, const std::u8string_view &value) -> SqlError;
    ///> bind BLOB, BINARY, VARBINARY, this api will not copy, Please ensure that the data is voalid during execute.
    auto bindView(int index, std::span<const std::byte> value) -> SqlError;
    ///> bind valueViwe, this api will not copy, Please ensure that the data is voalid during execute.
    template <detail::BindViewAble<SqlQuery> T>
    auto bindView(const std::string &name, T value) -> SqlError;

    [[nodiscard("Don't forget to use co_await")]]
    auto close() -> IoTask<void>;
    auto clearBinds() -> void;

private:
    auto pareser(std::string_view query) -> std::string;

private:
    std::shared_ptr<detail::MySql> mMysql;
    MYSQL_STMT                    *mMysqlStmt = nullptr;
    std::vector<
        std::variant<signed char, short int, int, long long int, float, double, std::string, std::u8string, SqlDate>>
                                         mBindBuffer; // save var to continue it is life.
    std::vector<MYSQL_BIND>              mBinds;
    std::unordered_map<std::string, int> mIndexs;
};

inline SqlQuery::SqlQuery(SqlDatabase &db) : mMysql(db.mysql()) {
}

inline SqlQuery::~SqlQuery() {
    if (mMysqlStmt) {
        mysql_stmt_close(mMysqlStmt);
    }
    if (mMysql.use_count() == 1) {
        mMysql->close();
    }
}

inline SqlQuery::SqlQuery(SqlQuery &&other) noexcept : mMysql(other.mMysql) {
    mMysqlStmt       = other.mMysqlStmt;
    other.mMysqlStmt = nullptr;
}

inline SqlQuery &SqlQuery::operator=(SqlQuery &&other) noexcept {
    ILIAS_ASSERT(mMysql == other.mMysql);
    mMysqlStmt       = other.mMysqlStmt;
    other.mMysqlStmt = nullptr;
    return *this;
}

inline auto SqlQuery::execute(std::string_view query) -> IoTask<void> {
    ILIAS_ASSERT(mMysql != nullptr);
    co_return co_await (mMysql->query(query) | ignoreCancellation);
}

// this query should like "SELECT * FROM table WHERE name=:name,age=:age;"
// return query like "SELECT * FROM table WHERE name=?,age=?;"
inline auto SqlQuery::pareser(std::string_view query) -> std::string {
    mIndexs.clear();
    mBindBuffer.clear();
    mBinds.clear();
    std::string ret;
    auto        start = query.find_first_of(':');
    if (start != std::string::npos) {
        ret = query.substr(0, start);
    }
    auto end = start;
    while (start != std::string::npos) {
        end = start;
        while (end < query.size() && query[end] != ' ' && query[end] != ',' && query[end] != '\t' &&
               query[end] != '\n' && query[end] != '\r' && query[end] != ')' && query[end] != '(' &&
               query[end] != '"') {
            end++;
        }
        auto name = query.substr(start + 1, end - start - 1);
        mIndexs.emplace(name, mIndexs.size());
        ret += '?';
        start = query.find_first_of(':', end);
        ret += query.substr(end, start - end);
    }
    mBinds.resize(mIndexs.size());
    memset(mBinds.data(), 0, sizeof(MYSQL_BIND) * mBinds.size());
    for (int i = 0; i < (int)mBinds.size(); ++i) {
        mBinds[i].buffer_type = MYSQL_TYPE_NULL;
    }
    mBindBuffer.resize(mIndexs.size());
    return ret;
}

inline auto SqlQuery::prepare(std::string_view query) -> IoTask<void> {
    if (mMysqlStmt == nullptr) {
        mMysqlStmt = mMysql->stmtInit();
    }
    int  ret;
    auto queryp = pareser(query);
    auto status = mysql_stmt_prepare_start(&ret, mMysqlStmt, queryp.data(), (unsigned long)queryp.size());
    while (status) {
        ILIAS_TRACE("sql", "stmt prepare waiting for status {}", status);
        auto pret = co_await (mMysql->pollStatus(status) | ignoreCancellation);
        if (!pret) {
            co_return Unexpected<Error>(pret.error());
        }
        status = mysql_stmt_prepare_cont(&ret, mMysqlStmt, status);
    }
    if (ret != 0) {
        ILIAS_ERROR("sql", "stmt failed, error: {}", ret);
        co_return Unexpected<Error>((SqlError::Code)ret);
    }
    co_return {};
}

template <detail::BindAble<SqlQuery> T>
inline auto SqlQuery::bind(const std::string &name, const T &value) -> SqlError {
    auto index = mIndexs.find(name);
    if (index == mIndexs.end()) {
        return SqlError::INVALID_INDEX;
    }
    return bind(index->second, value);
}

template <detail::BindViewAble<SqlQuery> T>
inline auto SqlQuery::bindView(const std::string &name, T value) -> SqlError {
    auto index = mIndexs.find(name);
    if (index == mIndexs.end()) {
        return SqlError::INVALID_INDEX;
    }
    return bindView(index->second, value);
}

inline auto SqlQuery::bindView(int index, std::string_view value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type   = MYSQL_TYPE_STRING;
    bind.buffer        = const_cast<char *>(value.data());
    bind.buffer_length = (unsigned long)value.size();
    mBinds[index]      = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, const std::string &value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_STRING;
    ILIAS_ASSERT(std::get_if<std::string>(&mBindBuffer[index]));
    bind.buffer        = std::get<std::string>(mBindBuffer[index]).data();
    bind.buffer_length = (unsigned long)std::get<std::string>(mBindBuffer[index]).size();
    mBinds[index]      = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, const std::u8string &value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_STRING;
    ILIAS_ASSERT(std::get_if<std::string>(&mBindBuffer[index]));
    bind.buffer        = std::get<std::u8string>(mBindBuffer[index]).data();
    bind.buffer_length = (unsigned long)std::get<std::u8string>(mBindBuffer[index]).size();
    mBinds[index]      = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, signed char value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_TINY; // what's c++ type it will be reinterpreted?
    ILIAS_ASSERT(std::get_if<signed char>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<signed char>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, short int value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_SHORT; // what's c++ type it will be reinterpreted?
    ILIAS_ASSERT(std::get_if<short int>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<short int>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, int value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_LONG; // what's c++ type it will be reinterpreted?
    ILIAS_ASSERT(std::get_if<int>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<int>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, long long int value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_LONGLONG; // what's c++ type it will be reinterpreted?
    ILIAS_ASSERT(std::get_if<long long int>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<long long int>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, float value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_FLOAT;
    ILIAS_ASSERT(std::get_if<float>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<float>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, double value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_DOUBLE;
    ILIAS_ASSERT(std::get_if<double>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<double>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bind(int index, SqlDate value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    mBindBuffer[index] = value;
    bind.buffer_type   = MYSQL_TYPE_DATE;
    ILIAS_ASSERT(std::get_if<SqlDate>(&mBindBuffer[index]));
    bind.buffer   = std::get_if<SqlDate>(&mBindBuffer[index]);
    mBinds[index] = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bindView(int index, const std::u8string_view &value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type   = MYSQL_TYPE_STRING;
    bind.buffer        = const_cast<char8_t *>(value.data());
    bind.buffer_length = (unsigned long)value.size();
    mBinds[index]      = bind;
    return SqlError::OK;
}

inline auto SqlQuery::bindView(int index, std::span<const std::byte> value) -> SqlError {
    if (mMysqlStmt == nullptr) {
        return SqlError::NOT_PREPARED;
    }
    if (index < 0 || index >= (int)mBinds.size()) {
        return SqlError::INVALID_INDEX;
    }
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type   = MYSQL_TYPE_BLOB;
    bind.buffer        = const_cast<std::byte *>(value.data());
    bind.buffer_length = (unsigned long)value.size();
    mBinds[index]      = bind;
    return SqlError::OK;
}

inline auto SqlQuery::close() -> IoTask<void> {
    my_bool ret;
    auto    status = mysql_stmt_close_start(&ret, mMysqlStmt);
    while (status) {
        ILIAS_TRACE("sql", "stmt close waiting for status {}", status);
        auto pret = co_await (mMysql->pollStatus(status) | ignoreCancellation);
        if (!pret) {
            mMysqlStmt = nullptr;
            ILIAS_TRACE("sql", "stmt close waiting for status {}", status);
            co_return Unexpected<Error>(pret.error());
        }
        status = mysql_stmt_close_cont(&ret, mMysqlStmt, status);
    }
    mMysqlStmt = nullptr;
    if (!ret) {
        ILIAS_ERROR("sql", "stmt close failed. (error {}:{})", ret, mMysql->lastErrorMessage());
        co_return Unexpected<Error>(Error::Unknown);
    }
    co_return {};
}

inline auto SqlQuery::execute() -> IoTask<void> {
    if (mMysqlStmt == nullptr) {
        co_return Unexpected<Error>(SqlError::Code::NOT_PREPARED);
    }
    int ret     = 0;
    ret         = mysql_stmt_bind_param(mMysqlStmt, mBinds.data());
    auto status = mysql_stmt_execute_start(&ret, mMysqlStmt);
    while (status) {
        ILIAS_TRACE("sql", "stmt execute waiting for status {}", status);
        auto pret = co_await (mMysql->pollStatus(status) | ignoreCancellation);
        if (!pret) {
            ILIAS_ERROR("sql", "stmt execute failed. (error: {})", pret.error().message());
            co_return Unexpected<Error>(pret.error());
        }
        status = mysql_stmt_execute_cont(&ret, mMysqlStmt, status);
    }
    if (ret != 0) {
        ILIAS_ERROR("sql", "stmt execute failed. (error {}:{})", ret, mMysql->lastErrorMessage());
        co_return Unexpected<Error>((SqlError::Code)ret);
    }
    co_return {};
}

inline auto SqlQuery::clearBinds() -> void {
    memset(mBinds.data(), 0, sizeof(MYSQL_BIND) * mBinds.size());
    for (int i = 0; i < (int)mBinds.size(); ++i) {
        mBinds[i].buffer_type = MYSQL_TYPE_NULL;
    }
}

inline auto SqlDate::setTime(std::chrono::system_clock::time_point tp) -> void {
    auto     tt  = std::chrono::system_clock::to_time_t(tp);
    std::tm *now = gmtime(&tt);
    setTime(now);
}

inline auto SqlDate::setTime(std::chrono::milliseconds timestamp) -> void {
    auto     milliseconds = std::chrono::milliseconds(timestamp);
    auto     tp           = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>(milliseconds);
    auto     tt           = std::chrono::system_clock::to_time_t(tp);
    std::tm *now          = gmtime(&tt);
    setTime(now);
}

inline auto SqlDate::setTime(int year_, int month_, int day_, int hour_, int minute_, int second_) -> void {
    year        = year_;
    month       = month_;
    day         = day_;
    hour        = hour_;
    minute      = minute_;
    second      = second_;
    time_type   = MYSQL_TIMESTAMP_DATE;
    second_part = 0;
    neg         = 0;
}

inline auto SqlDate::setTime(struct tm *timeinfo) -> void {
    year        = timeinfo->tm_year;
    month       = timeinfo->tm_mon;
    day         = timeinfo->tm_mday;
    hour        = timeinfo->tm_hour;
    minute      = timeinfo->tm_min;
    second      = timeinfo->tm_sec;
    time_type   = MYSQL_TIMESTAMP_DATE;
    second_part = 0;
    neg         = 0;
}

ILIAS_SQL_NS_END