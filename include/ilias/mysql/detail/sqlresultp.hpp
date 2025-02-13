#pragma once

#include <unordered_map>

#include "global.hpp"
#include "../sqlresult.hpp"

ILIAS_SQL_NS_BEGIN

class SqlQuery;

namespace detail {

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
    try {
        SqlResultType result;
        auto          lengths = mysql_fetch_lengths(mResult);
        switch (mFieldMetas[index]->type) {
            case MYSQL_TYPE_TINY: // char
                result.emplace<char>(std::stoi(std::string(mCurrentRow[index]), nullptr, 10));
                break;
            case MYSQL_TYPE_SHORT: // short
            case MYSQL_TYPE_LONG:  // int
            case MYSQL_TYPE_INT24:
                result.emplace<int32_t>(std::stoi(std::string(mCurrentRow[index]), nullptr, 10));
                break;
            case MYSQL_TYPE_FLOAT:
                result.emplace<float>(std::stof(std::string(mCurrentRow[index]), nullptr));
                break;
            case MYSQL_TYPE_DOUBLE:
                result.emplace<double>(std::stod(std::string(mCurrentRow[index]), nullptr));
                break;
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
    } catch (std::exception e) {
        ILIAS_TRACE("sql", "exception {}", e.what());
        return Unexpected<Error>(SqlError::Code::UNKNOWN_ERROR);
    }
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
    try {
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
    } catch (std::exception e) {
        ILIAS_TRACE("sql", "exception {}", e.what());
        return Unexpected<Error>(SqlError::Code::UNKNOWN_ERROR);
    }
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

ILIAS_SQL_NS_END