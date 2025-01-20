/**
 * @file sqlresult.hpp
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

#include "detail/global.hpp"
#include "detail/mysql.hpp"
#include "detail/typehlep.hpp"

ILIAS_SQL_NS_BEGIN

class SqlResult {
public:
    SqlResult(detail::MySql &sql);
    ~SqlResult();

    [[nodiscard("Don't forget to use co_await")]]
    auto next() -> IoTask<void>;
    template <typename T>
    [[nodiscard("Don't forget to use co_await")]]
    auto get(size_t index) -> IoTask<T>;
    template <typename T>
    [[nodiscard("Don't forget to use co_await")]]
    auto get(const std::string &name) -> IoTask<T>;
    auto countRows() -> size_t;

private:
    [[nodiscard("Don't forget to use co_await")]]
    auto fetchRow() -> IoTask<MYSQL_ROW>;
    auto freeResult() -> void;

private:
    detail::MySql &mSql;
    MYSQL_RES     *mResult     = nullptr;
    MYSQL_ROW      mCurrentRow = nullptr;
    MYSQL_FIELD   *mFieldMetas = nullptr;
};

inline SqlResult::SqlResult(detail::MySql &sql) : mSql(sql) {
}

inline SqlResult::~SqlResult() {
    if (mResult) {
        freeResult();
    }
}

inline auto SqlResult::next() -> IoTask<void> {
    if (mResult == nullptr) {
        co_return co_await (mSql.storeResult(&mResult) | ignoreCancellation);
    }
    else {
        mCurrentRow = mysql_fetch_row(mResult);
        if (mCurrentRow) {
            co_return {};
        }
        else {
            co_return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
        }
    }
}

template <typename T>
inline auto SqlResult::get(size_t index) -> IoTask<T> {
    if (mCurrentRow == nullptr) {
        co_return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    if (index < 0 || index >= mysql_num_rows(mResult)) {
        co_return Unexpected<Error>(SqlError::Code::INVALID_INDEX);
    }
    // TODO: try catch ?
    // co_return detail::fromeString<T>(mCurrentRow[index]);
}

// TODO: optimize
template <typename T>
inline auto SqlResult::get(const std::string &name) -> IoTask<T> {
    if (mResult == nullptr || mCurrentRow == nullptr) {
        co_return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    if (mFieldMetas == nullptr) {
        mFieldMetas = mysql_fetch_fields(mResult);
    }
    if (mFieldMetas == nullptr) {
        co_return Unexpected<Error>(SqlError::Code::NO_MORE_DATA);
    }
    auto index = -1;
    for (size_t i = 0; i < mysql_num_fields(mResult); ++i) {
        if (mFieldMetas[i].name == name) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        co_return Unexpected<Error>(SqlError::Code::INVALID_INDEX);
    }
    co_return co_await get<T>(index);
}

inline auto SqlResult::countRows() -> size_t {
    return mysql_num_rows(mResult);
}

inline auto SqlResult::fetchRow() -> IoTask<MYSQL_ROW> {
    ILIAS_ASSERT(mResult != nullptr);
    MYSQL_ROW row;
    auto      status = mysql_fetch_row_start(&row, mResult);
    if (status) {
        while (status) {
            ILIAS_TRACE("sql", "disconnect mysql waiting for status {}", status);
            auto pret = co_await mSql.pollStatus(status);
            status    = mysql_fetch_row_cont(&row, mResult, status);
            if (!pret) {
                co_return Unexpected<Error>(pret.error());
            }
        }
    }
    co_return {};
}

inline auto SqlResult::freeResult() -> void {
    if (mResult != nullptr) {
        mysql_free_result(mResult);
        mResult     = nullptr;
        mCurrentRow = nullptr;
        mFieldMetas = nullptr;
    }
}
ILIAS_SQL_NS_END