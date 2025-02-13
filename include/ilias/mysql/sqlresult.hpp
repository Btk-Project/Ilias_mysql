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

#include "detail/global.hpp"
#include "detail/sqlresultp.hpp"

ILIAS_SQL_NS_BEGIN

class SqlQuery;

class SqlResult {
public:
    SqlResult(SqlResult &&)            = default;
    SqlResult &operator=(SqlResult &&) = default;
    ~SqlResult()                       = default;

    SqlResult(const SqlResult &)            = delete;
    SqlResult &operator=(const SqlResult &) = delete;

    [[nodiscard("Don't forget to use co_await")]]
    auto next() -> IoTask<void>;
    auto countRows() -> size_t;
    template <typename T>
    auto get(size_t index) -> Result<T>;
    template <typename T>
    auto get(std::string_view name) -> Result<T>;

protected:
    inline SqlResult(std::unique_ptr<detail::SqlResultBase> imp) : mImp(std::move(imp)) {}
    friend class SqlQuery;

private:
    std::unique_ptr<detail::SqlResultBase> mImp;
};

inline auto SqlResult::next() -> IoTask<void> {
    co_return co_await mImp->next();
}

inline auto SqlResult::countRows() -> size_t {
    return mImp->countRows();
}

template <typename T>
auto SqlResult::get(size_t index) -> Result<T> {
    auto val = mImp->get(index);
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
auto SqlResult::get(std::string_view name) -> Result<T> {
    auto val = mImp->get(name);
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

ILIAS_SQL_NS_END