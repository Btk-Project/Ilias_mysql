add_rules("mode.debug", "mode.release", "mode.releasedbg", "mode.coverage")

set_languages("c++20")
add_includedirs("./include")
set_encodings("utf-8")

add_repositories("btk-repo https://github.com/Btk-Project/xmake-repo.git")

add_requires("ilias")

add_packages("ilias")
set_warnings("allextra")

includes("./include")
includes("./tests")