#include <gtest/gtest.h>

#include <ilias/platform.hpp>
#include <sqlite3.h>

TEST(sqlite, version) {
    const char *version = sqlite3_libversion();
    ILIAS_TRACE("sqlite test", "sqlite version: {}", version);
    ASSERT_TRUE(version != nullptr);
    ASSERT_TRUE(strlen(version) > 0);
}

TEST(sqlite, compile_options) {
    const char *options = sqlite3_compileoption_get(0);
    ILIAS_TRACE("sqlite test", "sqlite compile options: {}", options);
    ASSERT_TRUE(options != nullptr);
    ASSERT_TRUE(strlen(options) > 0);
}

int main(int argc, char **argv) {
    ILIAS_LOG_SET_LEVEL(ILIAS_TRACE_LEVEL);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}