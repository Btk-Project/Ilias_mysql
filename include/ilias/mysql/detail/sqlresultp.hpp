#pragma once

#include <unordered_map>
#include <variant>
#include <iomanip>
#include <charconv>
#include <chrono>
#include <mariadb/mysql.h>
#include <mariadb/mysqld_error.h>

#include "global.hpp"
#include "mysql.hpp"

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

namespace detail {

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
    virtual auto next() -> IoTask<void>                              = 0;
    virtual auto countRows() -> size_t                               = 0;
    virtual auto get(size_t index) -> Result<SqlResultType>          = 0;
    virtual auto get(std::string_view name) -> Result<SqlResultType> = 0;
};

class SqlQueryResult final : public SqlResultBase {
public:
    SqlQueryResult(std::shared_ptr<detail::MySql> sql);
    SqlQueryResult(SqlQueryResult &&);
    SqlQueryResult &operator=(SqlQueryResult &&);
    ~SqlQueryResult();

    [[nodiscard("Don't forget to use co_await")]]
    auto next() -> IoTask<void> override;
    auto get(size_t index) -> Result<SqlResultType> override;
    auto get(std::string_view name) -> Result<SqlResultType> override;
    auto countRows() -> size_t override;

protected:
    [[nodiscard("Don't forget to use co_await")]]
    auto getResult() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto fetchRow() -> IoTask<MYSQL_ROW>;
    auto freeResult() -> void;

private:
    std::shared_ptr<detail::MySql> mMysql;
    MYSQL_RES                     *mResult     = nullptr;
    MYSQL_ROW                      mCurrentRow = nullptr;
    std::vector<MYSQL_FIELD *>     mFieldMetas = {};

    friend class ::ILIAS_SQL_COMPLETE_NAMESPACE::SqlQuery;
};

class SqlStmtResult final : public SqlResultBase {
public:
    SqlStmtResult(std::shared_ptr<detail::MySql> sql, MYSQL_STMT *stmt);
    SqlStmtResult(SqlStmtResult &&);
    SqlStmtResult &operator=(SqlStmtResult &&);
    ~SqlStmtResult();

    [[nodiscard("Don't forget to use co_await")]]
    auto next() -> IoTask<void> override;
    auto get(size_t index) -> Result<SqlResultType> override;
    auto get(std::string_view name) -> Result<SqlResultType> override;
    auto countRows() -> size_t override;

protected:
    [[nodiscard("Don't forget to use co_await")]]
    auto getResult() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto fetchRow() -> IoTask<void>;
    auto freeResult() -> void;
    [[nodiscard("Don't forget to use co_await")]]
    auto storeResult(MYSQL_RES **res) -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto nextResult() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto close() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto reset() -> IoTask<void>;

private:
    std::shared_ptr<detail::MySql>                              mMysql;
    MYSQL_STMT                                                 *mStmt       = nullptr;
    MYSQL_RES                                                  *mResult     = nullptr;
    std::vector<MYSQL_FIELD *>                                  mFieldMetas = {};
    std::unordered_map<std::string, std::unique_ptr<uint8_t[]>> mFields;
    std::unique_ptr<MYSQL_BIND[]>                               mBinds;
    std::unique_ptr<unsigned long[]>                            mLengths;

    friend class ::ILIAS_SQL_COMPLETE_NAMESPACE::SqlQuery;
};

inline SqlQueryResult::SqlQueryResult(SqlQueryResult &&other) {
    mMysql            = std::move(other.mMysql);
    mResult           = other.mResult;
    mCurrentRow       = other.mCurrentRow;
    mFieldMetas       = std::move(other.mFieldMetas);
    other.mResult     = nullptr;
    other.mCurrentRow = nullptr;
    other.mFieldMetas.clear();
}

inline SqlQueryResult &SqlQueryResult::operator=(SqlQueryResult &&other) {
    if (mResult) {
        freeResult();
    }
    if (this != &other) {
        mMysql            = std::move(other.mMysql);
        mResult           = other.mResult;
        mCurrentRow       = other.mCurrentRow;
        mFieldMetas       = std::move(other.mFieldMetas);
        other.mResult     = nullptr;
        other.mCurrentRow = nullptr;
        other.mFieldMetas.clear();
    }
    return *this;
}

inline SqlQueryResult::SqlQueryResult(std::shared_ptr<detail::MySql> sql) : mMysql(sql) {
}

inline SqlQueryResult::~SqlQueryResult() {
    if (mResult) {
        freeResult();
    }
}

inline auto SqlQueryResult::getResult() -> IoTask<void> {
    auto ret = co_await (mMysql->storeResult(&mResult) | ignoreCancellation);
    if (!ret && ret.error() != SqlError::Code::OK) {
        co_return Unexpected<Error>(ret.error());
    }
    co_return {};
}

inline auto SqlQueryResult::next() -> IoTask<void> {
    if (mResult == nullptr) {
        auto ret = co_await (mMysql->storeResult(&mResult) | ignoreCancellation);
        if (!ret) {
            co_return Unexpected<Error>(ret.error());
        }
    }
    auto retRow = co_await fetchRow();
    if (retRow) {
        mCurrentRow = retRow.value();
    }
    else {
        co_return Unexpected<Error>(retRow.error());
    }
    if (mCurrentRow) {
        co_return {};
    }
    else {
        auto ret = co_await (mMysql->nextResult() | ignoreCancellation);
        if (!ret) {
            co_return Unexpected<Error>(ret.error());
        }
        freeResult();
        co_return co_await next();
    }
}

inline auto SqlQueryResult::get(size_t index) -> Result<SqlResultType> {
    if (mCurrentRow == nullptr) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    if (mFieldMetas.empty()) {
        mFieldMetas.resize(mysql_num_fields(mResult));
        auto fieldMetas = mysql_fetch_fields(mResult);
        for (size_t i = 0; i < mFieldMetas.size(); ++i) {
            mFieldMetas[i] = &fieldMetas[i];
        }
    }
    if (index < 0 || index >= mFieldMetas.size()) {
        return Unexpected<Error>(SqlError::Code::INVALID_INDEX);
    }
    // ILIAS_TRACE("sql", "index({})({}) raw data {}", index, (int)mFieldMetas[index]->type, mCurrentRow[index]);
    SqlResultType result;
    auto          lengths = mysql_fetch_lengths(mResult);
    switch (mFieldMetas[index]->type) {
        case MYSQL_TYPE_TINY: { // char
            int res;
            auto [ptr, ec] = std::from_chars(mCurrentRow[index], mCurrentRow[index] + lengths[index], res);
            if (ec != std::errc() || ptr != mCurrentRow[index] + lengths[index]) {
                return Unexpected<Error>(SqlError::UNKNOWN_ERROR);
            }
            result.emplace<char>(res);
            break;
        }
        case MYSQL_TYPE_SHORT: // short
        case MYSQL_TYPE_LONG:  // int
        case MYSQL_TYPE_INT24: {
            int32_t res;
            auto [ptr, ec] = std::from_chars(mCurrentRow[index], mCurrentRow[index] + lengths[index], res);
            if (ec != std::errc() || ptr != mCurrentRow[index] + lengths[index]) {
                return Unexpected<Error>(SqlError::UNKNOWN_ERROR);
            }
            result.emplace<int32_t>(res);
            break;
        }
        case MYSQL_TYPE_FLOAT: {
            float res;
            auto [ptr, ec] = std::from_chars(mCurrentRow[index], mCurrentRow[index] + lengths[index], res);
            if (ec != std::errc() || ptr != mCurrentRow[index] + lengths[index]) {
                return Unexpected<Error>(SqlError::UNKNOWN_ERROR);
            }
            result.emplace<float>(res);
            break;
        }
        case MYSQL_TYPE_DOUBLE: {
            double res;
            auto [ptr, ec] = std::from_chars(mCurrentRow[index], mCurrentRow[index] + lengths[index], res);
            if (ec != std::errc() || ptr != mCurrentRow[index] + lengths[index]) {
                return Unexpected<Error>(SqlError::UNKNOWN_ERROR);
            }
            result.emplace<double>(res);
            break;
        }
        case MYSQL_TYPE_NULL:
            result.emplace<std::nullptr_t>(nullptr);
            break;
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_DATETIME:
            result.emplace<SqlDate>(std::string_view(mCurrentRow[index]));
            std::get<SqlDate>(result).time.time_type = MYSQL_TIMESTAMP_DATETIME;
            break;
        case MYSQL_TYPE_DATE:
            result.emplace<SqlDate>(std::string_view(mCurrentRow[index]));
            std::get<SqlDate>(result).time.time_type = MYSQL_TIMESTAMP_DATE;
            break;
        case MYSQL_TYPE_TIME:
            result.emplace<SqlDate>(std::string_view(mCurrentRow[index]), "%H:%M:%S");
            std::get<SqlDate>(result).time.time_type = MYSQL_TIMESTAMP_TIME;
            break;
        case MYSQL_TYPE_LONGLONG: // long long
            result.emplace<int64_t>(std::stoll(std::string(mCurrentRow[index]), nullptr, 10));
            break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
            result.emplace<std::string>(reinterpret_cast<char *>(mCurrentRow[index]), lengths[index]);
            break;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
            result.emplace<std::vector<std::byte>>(
                std::vector<std::byte> {reinterpret_cast<std::byte *>(mCurrentRow[index]),
                                        reinterpret_cast<std::byte *>(mCurrentRow[index]) + lengths[index]});
            break;
        default:
            return Unexpected<Error>(SqlError::Code::UNKNOWN_ERROR);
    }
    return result;
}

// TODO: optimize
inline auto SqlQueryResult::get(std::string_view name) -> Result<SqlResultType> {
    if (mResult == nullptr || mCurrentRow == nullptr) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    if (mFieldMetas.empty()) {
        mFieldMetas.resize(mysql_num_fields(mResult));
        auto fieldMetas = mysql_fetch_fields(mResult);
        for (size_t i = 0; i < mFieldMetas.size(); ++i) {
            mFieldMetas[i] = &fieldMetas[i];
        }
    }
    if (mFieldMetas.empty()) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    std::size_t index = -1;
    for (size_t i = 0; i < mysql_num_fields(mResult); ++i) {
        if (mFieldMetas[i]->name == name) {
            index = i;
            break;
        }
    }
    if (index == (std::size_t)-1) {
        return Unexpected<Error>(SqlError::Code::INVALID_INDEX);
    }
    return get(index);
}

inline auto SqlQueryResult::countRows() -> size_t {
    return mysql_num_rows(mResult);
}

inline auto SqlQueryResult::fetchRow() -> IoTask<MYSQL_ROW> {
    ILIAS_ASSERT(mResult != nullptr);
    MYSQL_ROW row;
    auto      status = mysql_fetch_row_start(&row, mResult);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mMysql->pollStatus(status);
            status    = mysql_fetch_row_cont(&row, mResult, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    co_return row;
}

inline auto SqlQueryResult::freeResult() -> void {
    if (mResult != nullptr) {
        mysql_free_result(mResult);
        mResult     = nullptr;
        mCurrentRow = nullptr;
        mFieldMetas.clear();
    }
}

inline SqlStmtResult::SqlStmtResult(SqlStmtResult &&other) {
    mMysql      = std::move(other.mMysql);
    mStmt       = other.mStmt;
    other.mStmt = nullptr;
}

inline SqlStmtResult &SqlStmtResult::operator=(SqlStmtResult &&other) {
    if (mStmt) {
        ilias_wait close();
    }
    if (this != &other) {
        mMysql      = std::move(other.mMysql);
        mStmt       = other.mStmt;
        other.mStmt = nullptr;
    }
    return *this;
}

inline SqlStmtResult::SqlStmtResult(std::shared_ptr<detail::MySql> sql, MYSQL_STMT *stmt) : mMysql(sql), mStmt(stmt) {
}

inline SqlStmtResult::~SqlStmtResult() {
    ilias_wait close();
}

inline auto SqlStmtResult::getResult() -> IoTask<void> {
    freeResult();
    auto ret = co_await (storeResult(&mResult) | ignoreCancellation);
    if (!ret && ret.error() != SqlError::Code::OK) {
        co_return Unexpected<Error>(ret.error());
    }
    co_return {};
}

inline auto SqlStmtResult::next() -> IoTask<void> {
    if (mResult == nullptr) {
        auto ret = co_await (storeResult(&mResult) | ignoreCancellation);
        if (!ret) {
            co_return Unexpected<Error>(ret.error());
        }
    }
    auto ret = co_await fetchRow();
    if (!ret && ret.error() != SqlError::Code::OK) {
        co_return Unexpected<Error>(ret.error());
    }
    else if (ret) {
        co_return {};
    }
    else {
        auto ret = co_await (nextResult() | ignoreCancellation);
        if (!ret) {
            co_return Unexpected<Error>(ret.error());
        }
        freeResult();
        co_return co_await next();
    }
}

inline auto SqlStmtResult::get(size_t index) -> Result<SqlResultType> {
    if (mFields.empty() || mFieldMetas.empty()) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    if (index < 0 || index >= mFieldMetas.size()) {
        return Unexpected<Error>(SqlError::Code::INVALID_INDEX);
    }
    auto &currentRow = mFields[mFieldMetas[index]->name];
    // ILIAS_TRACE("sql", "{}({}) raw data {}: {}",
    //             std::string_view(mFieldMetas[index]->name, mFieldMetas[index]->name_length),
    //             (int)mFieldMetas[index]->type, mLengths[index], (char *)currentRow.get());
    SqlResultType result;
    switch (mFieldMetas[index]->type) {
        case MYSQL_TYPE_TINY: // char
            result.emplace<char>(*reinterpret_cast<int32_t *>(currentRow.get()));
            break;
        case MYSQL_TYPE_SHORT: // short
        case MYSQL_TYPE_LONG:  // int
        case MYSQL_TYPE_INT24:
            result.emplace<int32_t>(*reinterpret_cast<int32_t *>(currentRow.get()));
            break;
        case MYSQL_TYPE_FLOAT:
            result.emplace<float>(*reinterpret_cast<double *>(currentRow.get()));
            break;
        case MYSQL_TYPE_DOUBLE:
            result.emplace<double>(*reinterpret_cast<double *>(currentRow.get()));
            break;
        case MYSQL_TYPE_NULL:
            result.emplace<std::nullptr_t>(nullptr);
            break;
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_DATETIME:
            result.emplace<SqlDate>(std::string_view((char *)currentRow.get(), mLengths[index]));
            std::get<SqlDate>(result).time.time_type = MYSQL_TIMESTAMP_DATETIME;
            break;
        case MYSQL_TYPE_DATE:
            result.emplace<SqlDate>(std::string_view((char *)currentRow.get(), mLengths[index]));
            std::get<SqlDate>(result).time.time_type = MYSQL_TIMESTAMP_DATE;
            break;
        case MYSQL_TYPE_TIME:
            result.emplace<SqlDate>(std::string_view((char *)currentRow.get(), mLengths[index]), "%H:%M:%S");
            std::get<SqlDate>(result).time.time_type = MYSQL_TIMESTAMP_TIME;
            break;
        case MYSQL_TYPE_LONGLONG: // long long
            result.emplace<int64_t>(*reinterpret_cast<int64_t *>(currentRow.get()));
            break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
            result.emplace<std::string>(reinterpret_cast<char *>(currentRow.get()), mLengths[index]);
            break;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
            result.emplace<std::vector<std::byte>>(
                std::vector<std::byte> {reinterpret_cast<std::byte *>((char *)currentRow.get()),
                                        reinterpret_cast<std::byte *>((char *)currentRow.get()) + mLengths[index]});
            break;
        default:
            return Unexpected<Error>(SqlError::Code::UNKNOWN_ERROR);
    }
    return result;
}

// TODO: optimize
inline auto SqlStmtResult::get(std::string_view name) -> Result<SqlResultType> {
    if (mResult == nullptr || mFieldMetas.empty()) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    if (mFieldMetas.empty()) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    auto index = -1;
    for (size_t i = 0; i < mysql_num_fields(mResult); ++i) {
        if (mFieldMetas[i]->name == name) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        return Unexpected<Error>(SqlError::Code::INVALID_INDEX);
    }
    return get(index);
}

inline auto SqlStmtResult::countRows() -> size_t {
    return mysql_stmt_num_rows(mStmt);
}

inline auto SqlStmtResult::fetchRow() -> IoTask<void> {
    ILIAS_ASSERT(mStmt != nullptr);
    int  ret;
    auto status = mysql_stmt_fetch_start(&ret, mStmt);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mMysql->pollStatus(status);
            status    = mysql_stmt_fetch_cont(&ret, mStmt, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    if (ret != 0) {
        co_return Unexpected<Error>((SqlError::Code)ret);
    }
    co_return {};
}

inline auto SqlStmtResult::freeResult() -> void {
    if (mResult != nullptr) {
        mysql_free_result(mResult);
        mResult = nullptr;
    }
}

inline auto SqlStmtResult::storeResult(MYSQL_RES **res) -> IoTask<void> {
    ILIAS_ASSERT(mStmt != nullptr);
    int  ret;
    auto status = mysql_stmt_store_result_start(&ret, mStmt);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mMysql->pollStatus(status);
            status    = mysql_stmt_store_result_cont(&ret, mStmt, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    *res = mysql_stmt_result_metadata(mStmt);
    if (*res == nullptr) {
        auto error = mMysql->lastError();
        co_return Unexpected<Error>(error.error());
    }
    mFieldMetas.resize(mysql_num_fields(*res));
    auto fieldMetas = mysql_fetch_fields(*res);
    mBinds          = std::make_unique<MYSQL_BIND[]>(mFieldMetas.size());
    memset(mBinds.get(), 0, sizeof(MYSQL_BIND) * mFieldMetas.size());
    mLengths = std::make_unique<unsigned long[]>(mFieldMetas.size());
    memset(mLengths.get(), 0, sizeof(unsigned long) * mFieldMetas.size());
    for (size_t i = 0; i < mFieldMetas.size(); ++i) {
        mFieldMetas[i]        = &fieldMetas[i];
        mBinds[i].is_unsigned = (mFieldMetas[i]->flags & UNSIGNED_FLAG) ? 1 : 0;

        switch (mFieldMetas[i]->type) {
            case MYSQL_TYPE_NULL:
                mBinds[i].buffer_type = MYSQL_TYPE_NULL;
            case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_FLOAT:
                mBinds[i].buffer_type   = MYSQL_TYPE_DOUBLE;
                mBinds[i].buffer_length = sizeof(double);
                break;
            case MYSQL_TYPE_LONGLONG:
#if MYSQL_VERSION_ID > 50002
            case MYSQL_TYPE_BIT:
#endif
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_YEAR:
            case MYSQL_TYPE_TINY:
                mBinds[i].buffer_type   = MYSQL_TYPE_LONGLONG;
                mBinds[i].buffer_length = sizeof(int64_t);
                break;
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_NEWDATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_ENUM:
            case MYSQL_TYPE_SET:
            case MYSQL_TYPE_LONG_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_GEOMETRY:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                mBinds[i].buffer_type = MYSQL_TYPE_STRING;
                mBinds[i].buffer_length =
                    mFieldMetas[i]->max_length ? mFieldMetas[i]->max_length : mFieldMetas[i]->length;
                break;
            default:
                co_return Unexpected<Error>(SqlError::Code::UNKNOWN_ERROR);
        }
        mBinds[i].length = &mLengths[i];
        if (mBinds[i].buffer_length > 0) {
            mFields[mFieldMetas[i]->name] = std::make_unique<uint8_t[]>(mBinds[i].buffer_length);
            memset(mFields[mFieldMetas[i]->name].get(), 0, mBinds[i].buffer_length);
            mBinds[i].buffer = mFields[mFieldMetas[i]->name].get();
        }
    }
    auto bindRet = mysql_stmt_bind_result(mStmt, mBinds.get());
    if (!bindRet) {
        co_return Unexpected<Error>(mMysql->lastError().error());
    }
    co_return {};
}

inline auto SqlStmtResult::nextResult() -> IoTask<void> {
    ILIAS_ASSERT(mStmt != nullptr);
    int  ret;
    auto status = mysql_stmt_next_result_start(&ret, mStmt);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mMysql->pollStatus(status);
            status    = mysql_stmt_next_result_cont(&ret, mStmt, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    co_return {};
}

inline auto SqlStmtResult::close() -> IoTask<void> {
    ILIAS_ASSERT(mStmt != nullptr);
    my_bool ret;
    auto    status = mysql_stmt_close_start(&ret, mStmt);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mMysql->pollStatus(status);
            status    = mysql_stmt_close_cont(&ret, mStmt, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    freeResult();
    if (mStmt != nullptr) {
        mysql_stmt_free_result(mStmt);
        mStmt = nullptr;
    }
    co_return {};
}

inline auto SqlStmtResult::reset() -> IoTask<void> {
    ILIAS_ASSERT(mStmt != nullptr);
    my_bool ret;
    auto    status = mysql_stmt_reset_start(&ret, mStmt);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mMysql->pollStatus(status);
            status    = mysql_stmt_reset_cont(&ret, mStmt, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    co_return {};
}
} // namespace detail

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