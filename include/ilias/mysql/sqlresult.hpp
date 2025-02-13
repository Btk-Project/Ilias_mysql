/**
 * @file sqlresultBase.hpp
 * @author llhsdmd (llhsdmd@gmail.com)
 * @brief
 * @version 0.1
 * @date 2025-01-12
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
#include <iomanip>

#include "detail/global.hpp"
#include "detail/mysql.hpp"

ILIAS_SQL_NS_BEGIN
class SqlQuery;

struct SqlDate {
    inline SqlDate(int year = 0, int month = 0, int day = 0, int hour = 0, int minute = 0, int second = 0) {
        setTime(year, month, day, hour, minute, second);
    }
    inline SqlDate(struct tm *timeinfo) { setTime(timeinfo); }
    inline SqlDate(std::chrono::system_clock::time_point tp) { setTime(tp); }
    inline SqlDate(std::chrono::milliseconds timestamp) { setTime(timestamp); }
    inline SqlDate(std::string_view str, std::string_view fmt = "%Y-%m-%d %H:%M:%S") { setTime(str, fmt); }
    inline SqlDate(const MYSQL_TIME &time) { this->time = time; };

    auto setTime(std::chrono::system_clock::time_point tp) -> void;
    auto setTime(std::chrono::milliseconds timestamp) -> void;
    auto setTime(int year, int month, int day, int hour, int minute, int second) -> void;
    auto setDate(int year, int month, int day) -> void;
    auto setTime(int hour, int minute, int second) -> void;
    auto setTime(struct tm *timeinfo) -> void;
    auto setTime(std::string_view str, std::string_view fmt = "%Y-%m-%d %H:%M:%S") -> void;

    auto toString() const -> std::string;
    auto toTimestamp() const -> uint64_t;

    SqlDate operator=(const MYSQL_TIME &time) {
        this->time = time;
        return *this;
    }

    operator MYSQL_TIME() const { return time; }

    MYSQL_TIME time = {};
};

using SqlArrayBuffer = std::vector<std::byte>;
using SqlResultType =
    std::variant<nullptr_t, char, int32_t, int64_t, double, float, std::string, SqlDate, SqlArrayBuffer>;

class SqlResultBase {
public:
    SqlResultBase()                            = default;
    SqlResultBase(SqlResultBase &&)            = default;
    SqlResultBase &operator=(SqlResultBase &&) = default;
    ~SqlResultBase()                           = default;

    SqlResultBase(const SqlResultBase &)            = delete;
    SqlResultBase &operator=(const SqlResultBase &) = delete;

    [[nodiscard("Don't forget to use co_await")]]
    virtual auto next() -> IoTask<void> = 0;
    virtual auto countRows() -> size_t  = 0;
    template <typename T>
    auto get(size_t index) -> Result<T>;
    template <typename T>
    auto get(std::string_view name) -> Result<T>;

protected:
    virtual auto get(size_t index) -> Result<SqlResultType>          = 0;
    virtual auto get(std::string_view name) -> Result<SqlResultType> = 0;
};

using SqlResult = std::unique_ptr<SqlResultBase>;

template <typename T>
auto SqlResultBase::get(size_t index) -> Result<T> {
    auto val = get(index);
    if (!val) {
        ILIAS_TRACE("sql", "no column value: {}", index);
        return Unexpected<Error>(val.error());
    }
    auto res = std::get_if<T>(&val.value());
    if (res == nullptr) {
        ILIAS_TRACE("sql", "Wrong type of column value: {}", index);
        return Unexpected<Error>(SqlError::WRONG_TYPE_COLUMN_VALUE_ERROR);
    }
    return *res;
}

template <typename T>
auto SqlResultBase::get(std::string_view name) -> Result<T> {
    auto val = get(name);
    if (!val) {
        ILIAS_TRACE("sql", "no column value: {}", name);
        return Unexpected<Error>(val.error());
    }
    auto res = std::get_if<T>(&val.value());
    if (res == nullptr) {
        ILIAS_TRACE("sql", "Wrong type of column value: {}", name);
        return Unexpected<Error>(SqlError::WRONG_TYPE_COLUMN_VALUE_ERROR);
    }
    return *res;
}

inline auto SqlDate::toString() const -> std::string {
    if (time.time_type == MYSQL_TIMESTAMP_DATETIME) {
        return std::to_string(time.year) + "-" + std::to_string(time.month) + "-" + std::to_string(time.day) + " " +
               std::to_string(time.hour) + ":" + std::to_string(time.minute) + ":" + std::to_string(time.second);
    }
    else if (time.time_type == MYSQL_TIMESTAMP_TIME) {
        return std::to_string(time.hour) + ":" + std::to_string(time.minute) + ":" + std::to_string(time.second);
    }
    else if (time.time_type == MYSQL_TIMESTAMP_DATE) {
        return std::to_string(time.year) + "-" + std::to_string(time.month) + "-" + std::to_string(time.day);
    }
    else {
        return "error time";
    }
}

inline auto SqlDate::toTimestamp() const -> uint64_t {
    return time.year * 31536000 + time.month * 2592000 + time.day * 86400 + time.hour * 3600 + time.minute * 60 +
           time.second;
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
    if (year_ < 0 || month_ < 0 || month_ > 12 || day_ < 0 || day_ > 31 || hour_ < 0 || hour_ > 23 || minute_ < 0 ||
        minute_ > 59 || second_ < 0 || second_ > 59) {
        time.time_type = MYSQL_TIMESTAMP_ERROR;
        ILIAS_ERROR("sql", "error date time set {}-{}-{} {}:{}:{}", year_, month_, day_, hour_, minute_, second_);
        return;
    }
    time.year        = year_;
    time.month       = month_;
    time.day         = day_;
    time.hour        = hour_;
    time.minute      = minute_;
    time.second      = second_;
    time.time_type   = MYSQL_TIMESTAMP_DATETIME;
    time.second_part = 0;
    time.neg         = 0;
}

inline auto SqlDate::setDate(int year, int month, int day) -> void {
    if (year < 0 || month < 0 || month > 12 || day < 0 || day > 31) {
        time.time_type = MYSQL_TIMESTAMP_ERROR;
        ILIAS_ERROR("sql", "error date set {}-{}-{}", year, month, day);
        return;
    }
    memset(&time, 0, sizeof(time));
    time.year      = year;
    time.month     = month;
    time.day       = day;
    time.time_type = MYSQL_TIMESTAMP_DATE;
}

inline auto SqlDate::setTime(int hour, int minute, int second) -> void {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        time.time_type = MYSQL_TIMESTAMP_ERROR;
        ILIAS_ERROR("sql", "error time set {}:{}:{}", hour, minute, second);
        return;
    }
    memset(&time, 0, sizeof(time));
    time.hour      = hour;
    time.minute    = minute;
    time.second    = second;
    time.time_type = MYSQL_TIMESTAMP_TIME;
}

inline auto SqlDate::setTime(struct tm *timeinfo) -> void {
    time.year        = timeinfo->tm_year;
    time.month       = timeinfo->tm_mon;
    time.day         = timeinfo->tm_mday;
    time.hour        = timeinfo->tm_hour;
    time.minute      = timeinfo->tm_min;
    time.second      = timeinfo->tm_sec;
    time.time_type   = MYSQL_TIMESTAMP_DATETIME;
    time.second_part = 0;
    time.neg         = 0;
}

inline auto SqlDate::setTime(std::string_view str, std::string_view fmt) -> void {
    if (fmt == "") {
        fmt = "%Y-%m-%d %H:%M:%S";
    }
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(struct tm));
    std::istringstream istr((std::string(str)));
    istr >> std::get_time(&timeinfo, std::string(fmt).c_str());
    timeinfo.tm_year += 1900;
    timeinfo.tm_mon += 1;
    setTime(&timeinfo);
}
ILIAS_SQL_NS_END