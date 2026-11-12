add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

-- The preloader SDK supplies the mod lifecycle macros, the hook engine, the
-- signature scanner and the launcher Mod Menu bridge.
package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("nlohmann_json v3.11.3")
add_requires("fmt")

target("BlockEntitySelector")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_files("src/**.cpp")
    add_includedirs("include", {public = true})
    add_includedirs("src")
    add_packages("preloader", "nlohmann_json", "fmt")

    if is_plat("android") then
        add_cxflags("-fPIC", "-O2", "-ffunction-sections", "-fdata-sections", "-fexceptions", "-w")
        -- RTTI stays enabled: the mod identifies actors by reading the game's
        -- own typeinfo names, and __cxa_demangle needs libstdc++'s support.
        add_cxflags("-frtti")
        add_shflags("-Wl,--gc-sections", "-Wl,--no-undefined", "-Wl,--hash-style=gnu",
                    "-Wl,-z,max-page-size=16384")
        add_ldflags("-Wl,--no-undefined")
        add_links("android", "log", "dl", "EGL", "GLESv3", "GLESv2")
    end

    after_build(function (target)
        if not target:is_plat("android") then return end
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        if not python then
            python = find_tool("py")
        end
        assert(python, "Python 3 is required to package BlockEntitySelector.levipack")
        local args = {}
        table.insert(args, path.join(os.projectdir(), "scripts", "package_levipack.py"))
        table.insert(args, "--library")
        table.insert(args, target:targetfile())
        -- The launcher loads the manifest's entry name verbatim; pin it so the
        -- package is valid even if xmake renames the build output.
        table.insert(args, "--entry-name")
        table.insert(args, "libBlockEntitySelector.so")
        table.insert(args, "--icon")
        table.insert(args, path.join(os.projectdir(), "assets", "icon.png"))
        table.insert(args, "--profiles")
        table.insert(args, path.join(os.projectdir(), "resources", "signatures.json"))
        table.insert(args, "--version-header")
        table.insert(args, path.join(os.projectdir(), "include", "bes", "Version.hpp"))
        table.insert(args, "--output")
        table.insert(args, path.join(target:targetdir(), "BlockEntitySelector.levipack"))
        os.vrunv(python.program, args)
    end)

-- Host-side unit tests for the pieces that do not need the game:
-- RTTI name handling, AABB/ray maths, profile parsing, selection matching.
target("tests")
    set_kind("binary")
    set_languages("c++20")
    set_default(false)
    add_files("tests/*.cpp", "src/core/Profile.cpp", "src/core/Rtti.cpp", "src/core/Logging.cpp",
              "src/selection/ScannerState.cpp")
    add_includedirs("include", "src")
    -- Host builds have no preloader SDK, so fmt and the logger come from stubs
    -- in tests/stubs. Point BES_TEST_STUBS at another SDK checkout to use the real
    -- headers instead.
    add_packages("nlohmann_json")
    add_includedirs("tests/stubs")
