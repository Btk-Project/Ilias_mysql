#pragma once

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

private:
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

    friend class SqlQuery;
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

private:
    [[nodiscard("Don't forget to use co_await")]]
    auto getResult() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto fetchRow() -> IoTask<MYSQL_ROW>;
    auto freeResult() -> void;
    [[nodiscard("Don't forget to use co_await")]]
    auto storeResult() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto nextResult() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto close() -> IoTask<void>;
    [[nodiscard("Don't forget to use co_await")]]
    auto reset() -> IoTask<void>;

private:
    std::shared_ptr<detail::MySql> mMysql;
    MYSQL_STMT                    *mStmt       = nullptr;
    MYSQL_ROW                      mCurrentRow = nullptr;
    bool                           mIsInResult = false;

    friend class SqlQuery;
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
    mCurrentRow = mysql_fetch_row(mResult);
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
                result.emplace<char>(*static_cast<char *>(mCurrentRow[index]));
                break;
            case MYSQL_TYPE_SHORT: // short
                result.emplace<int32_t>(std::stoi(std::string(mCurrentRow[index]), nullptr, 10));
                break;
            case MYSQL_TYPE_LONG: // int
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
            case MYSQL_TYPE_DATE:
                result.emplace<SqlDate>(std::string_view(mCurrentRow[index]));
                break;
            case MYSQL_TYPE_LONGLONG: // long long
                result.emplace<int64_t>(std::stoll(std::string(mCurrentRow[index]), nullptr, 10));
                break;
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
                result.emplace<std::string>(reinterpret_cast<char *>(mCurrentRow[index]));
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
    co_return {};
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
    mMysql            = std::move(other.mMysql);
    mCurrentRow       = other.mCurrentRow;
    mStmt             = other.mStmt;
    other.mCurrentRow = nullptr;
    other.mStmt       = nullptr;
}

inline SqlStmtResult &SqlStmtResult::operator=(SqlStmtResult &&other) {
    if (mStmt) {
        ilias_wait close();
    }
    if (this != &other) {
        mMysql            = std::move(other.mMysql);
        mCurrentRow       = other.mCurrentRow;
        mStmt             = other.mStmt;
        other.mCurrentRow = nullptr;
        other.mStmt       = nullptr;
    }
    return *this;
}

inline SqlStmtResult::SqlStmtResult(std::shared_ptr<detail::MySql> sql, MYSQL_STMT *stmt) : mMysql(sql), mStmt(stmt) {
}

inline SqlStmtResult::~SqlStmtResult() {
    ilias_wait close();
}

inline auto SqlStmtResult::getResult() -> IoTask<void> {
    // freeResult();
    // auto ret = co_await (mMysql->readQueryResult() | ignoreCancellation);
    // if (!ret) {
    //     co_return Unexpected<Error>(ret.error());
    // }
    co_return {};
}

inline auto SqlStmtResult::next() -> IoTask<void> {
    if (!mIsInResult) {
        auto ret = co_await (storeResult() | ignoreCancellation);
        if (!ret) {
            co_return Unexpected<Error>(ret.error());
        }
        mIsInResult = true;
    }
    freeResult();
    auto ret = co_await fetchRow();
    if (!ret) {
        mIsInResult = false;
        co_return Unexpected<Error>(ret.error());
    }
    mCurrentRow = ret.value();
    if (mCurrentRow) {
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
    if (mCurrentRow == nullptr) {
        return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    return Unexpected<Error>(SqlError::Code::UNKNOWN_ERROR);
}

// TODO: optimize
inline auto SqlStmtResult::get(std::string_view name) -> Result<SqlResultType> {
    return get(-1);
}

inline auto SqlStmtResult::countRows() -> size_t {
    return mysql_stmt_num_rows(mStmt);
}

inline auto SqlStmtResult::fetchRow() -> IoTask<MYSQL_ROW> {
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
    co_return {};
}

inline auto SqlStmtResult::freeResult() -> void {
    if (mStmt != nullptr) {
        mysql_stmt_free_result(mStmt);
        mCurrentRow = nullptr;
        mStmt       = nullptr;
    }
}

inline auto SqlStmtResult::storeResult() -> IoTask<void> {
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
    mStmt       = nullptr;
    mCurrentRow = nullptr;
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