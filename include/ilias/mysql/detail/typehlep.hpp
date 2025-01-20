#pragma once

#include <span>

#include "global.hpp"

ILIAS_SQL_NS_BEGIN
class SqlError;

namespace detail {

template <typename U, typename T>
concept BindAble = requires(T t, U u, int i) {
    { t.bind(i, u) } -> std::same_as<SqlError>;
};

template <typename U, typename T>
concept BindViewAble = requires(T t, U u, int i) {
    { t.bindView(i, u) } -> std::same_as<SqlError>;
};

} // namespace detail
ILIAS_SQL_NS_END