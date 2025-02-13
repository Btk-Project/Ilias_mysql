add_requires("gtest")

option("sql")
    set_default(true)
    set_showmenu(true)
    set_category("module")
    set_description("add sql support")
option_end()

option("sqlite")
    set_default(true)
    set_showmenu(true)
    set_category("module")
    set_description("add sqlite support")
option_end()

if has_config("sql") then
    add_requires("mariadb-connector-c")
    add_packages("mariadb-connector-c")
end

if has_config("sqlite") then
    add_requires("sqlite3")
    add_packages("sqlite3")
end

-- Make all files in the unit directory into targets
for _, file in ipairs(os.files("unit/**.cpp")) do
    local name = path.basename(file)
    local dir = path.directory(file)
    local conf_path = dir .. "/" .. name .. ".lua"

    -- If this file require a specific configuration, load it, and skip the auto target creation
    if os.exists(conf_path) then 
        includes(conf_path)
        goto continue
    end

    -- Otherwise, create a target for this file, in most case, it should enough
    target("test_" .. name)
        set_kind("binary")
        set_default(false)

        add_files(file)
        add_tests(name, {run_timeout = 10000})
        add_packages("gtest")
        add_includedirs("$(projectdir)/include")
        add_defines("ILIAS_ENABLE_LOG")
    target_end()

    ::continue::
end