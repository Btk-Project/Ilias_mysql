#include <gtest/gtest.h>

#include <ilias/platform.hpp>
#include "ilias/mysql/detail/mysql.hpp"
#include "ilias/mysql/sqlquery.hpp"
#include "ilias/mysql/sqlresult.hpp"

ILIAS_SQL_USE_NAMESPACE;

struct Person {
    int                    id;
    std::string            name;
    int                    age;
    std::string            email;
    SqlDate                born;
    std::vector<std::byte> promise;
};

ILIAS_NAMESPACE::Task<void> test() {
    SqlDatabase db;
    db.setHost("127.0.0.1");
    db.setUserName("root");
    db.setPassword("123456");
    db.setPort(3306);
    auto ret = co_await db.open();
    EXPECT_TRUE(ret.has_value());
    if (!ret.has_value()) {
        co_return;
    }
    SqlQuery query(db);

    // create datebase test.
    ret = co_await query.execute("CREATE DATABASE IF NOT EXISTS test");
    EXPECT_TRUE(ret.has_value());
    if (!ret.has_value()) {
        co_return;
    }
    // use database test.
    ret = co_await db.selectDb("test");
    EXPECT_TRUE(ret.has_value());
    if (!ret.has_value()) {
        co_return;
    }
    // delete test_table if exists
    ret = co_await query.execute("DROP TABLE IF EXISTS test_table");
    EXPECT_TRUE(ret.has_value());
    if (!ret.has_value()) {
        co_return;
    }
    // create table test. / primary key id | NOT NULL name varchar(255) | age int | born date | UNIQUE email
    // varchar(255)
    ret = co_await query.execute(
        "CREATE TABLE IF NOT EXISTS test_table (id INT NOT NULL PRIMARY KEY, name VARCHAR(255) NOT "
        "NULL, age INT, born DATE, email VARCHAR(255) UNIQUE, promise BLOB(1000))");
    EXPECT_TRUE(ret.has_value());
    if (!ret.has_value()) {
        co_return;
    }
    // insert 0001, test, 18, 2000-01-01, test@test.com
    // insert 0002, 小明, 19, 2001-01-01, xiaoming@test.com
    std::vector<Person> persons = {
        {1, "a test user", 18, "test@test.com", SqlDate(2025, 1, 20), {std::byte {1}, std::byte {2}, std::byte {0}}},
        {2, "王小明", 19, "xiaoming@test.com", SqlDate(2025, 1, 21), {std::byte {1}, std::byte {0}, std::byte {3}}},
        {3, "Alice", 18, "Alice@test.com", SqlDate(2025, 1, 22), {std::byte {1}, std::byte {2}, std::byte {5}}},
        {4, "Bob", 18, "Bob@test.com", SqlDate(2025, 1, 20), {std::byte {3}, std::byte {2}, std::byte {3}}},
    };
    for (auto &person : persons) {
        ret = co_await query.prepare("INSERT INTO test_table (id, name, age, born, email, promise) VALUES (:id, :name, "
                                     ":age, :born, :email, :promise)");
        EXPECT_TRUE(ret.has_value());
        if (!ret.has_value()) {
            co_return;
        }
        query.bind("id", person.id);
        query.bind("name", person.name);
        query.bind("age", person.age);
        query.bind("email", person.email);
        query.bind("born", person.born);
        query.bindView("promise", person.promise);
        ret = co_await query.execute();
        EXPECT_TRUE(ret.has_value());
        if (!ret.has_value()) {
            co_return;
        }
    }

    // co_await mysql.autoCommit(false);
    co_return;
}

TEST(SQL, test) {
    ilias_wait test();
}

int main(int argc, char **argv) {
    ILIAS_LOG_SET_LEVEL(ILIAS_TRACE_LEVEL);
    ilias::PlatformContext ioContext;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}