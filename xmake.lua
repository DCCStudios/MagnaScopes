-- MagnaScope ships as one DLL. OG hooks are enabled today; NG and AE load
-- safely without resolving OG-only addresses.
includes("lib/commonlibf4")

set_project("MagnaScope")
set_version("0.11.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
set_allowedarchs("windows|x64")
set_defaultarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")
set_defaultmode("releasedbg")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- REL::ID initializer slots are always [OG, NG, AE].
add_defines("COMMONLIB_RUNTIMECOUNT=3")

add_requires("nlohmann_json")
add_requires("minhook")

local shader_profiles = {
    AutoSTS_PS = "ps_5_0",
    ScopeGeometryFill_GS = "gs_5_0",
    ScopeGeometryMagnify_PS = "ps_5_0",
    ScopeGeometryProbe_PS = "ps_5_0",
    ReticleLayer_PS = "ps_5_0",
    ScopeEffect_PS = "ps_5_0",
    ScopeEffect_PS_Legacy = "ps_5_0",
    ScopeEffect_PS_Output = "ps_5_0",
    ScopeEffect_PS_Output_Legacy = "ps_5_0",
    ScopeEffect_VS = "vs_5_0",
    ScopeEffect_VS_Legacy = "vs_5_0",
    ScopeEffect_VS_Output = "vs_5_0",
}

target("MagnaScope")
    add_rules("commonlibf4.plugin", {
        name = "MagnaScope",
        author = "DCC Studios",
        description = "Screen-space magnified scopes with automatic See Through Scopes compatibility.",
        plugin_template = path.join(os.projectdir(), "res/commonlibf4-plugin.cpp.in"),
    })

    add_packages("nlohmann_json", "minhook")
    add_files("src/**.cpp")
    remove_files("src/MathUtils.cpp")
    add_headerfiles("src/**.h")
    add_includedirs(
        "src",
        "include",
        "../F4SE-Menu-Framework-3/resources"
    )

    add_defines(
        "_UNICODE",
        "UNICODE",
        "NOMINMAX",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING"
    )

    add_syslinks(
        "d3d11",
        "d3d9",
        "d3dcompiler",
        "dwrite",
        "dxgi",
        "dxguid",
        "shlwapi"
    )

    add_cxxflags(
        "/sdl",
        "/Zi",
        "/permissive-",
        "/Zc:preprocessor",
        "/EHsc",
        "/wd4100",
        "/wd4189",
        "/wd4244",
        "/wd4267",
        "/wd4838",
        "/wd5105"
    )

    set_pcxxheader("src/PCH.h")
    set_runtimes("MD")
    set_symbols("debug")
    set_optimize("fastest")
    set_targetdir("Compile/F4SE/Plugins")

    after_build(function(target)
        import("core.base.option")

        local shader_dir = path.join(os.projectdir(), "Compile", "Shaders", "MagnaScope")
        os.mkdir(shader_dir)

        -- fxc is supplied by the Windows SDK but is not normally on PATH.
        local program_files_x86 = os.getenv("ProgramFiles(x86)")
        local candidates = {}
        if program_files_x86 then
            candidates = os.files(path.join(program_files_x86, "Windows Kits", "10", "bin", "*", "x64", "fxc.exe"))
        end
        table.sort(candidates)
        assert(#candidates > 0, "Windows SDK fxc.exe was not found")
        local fxc = candidates[#candidates]

        for shader, profile in pairs(shader_profiles) do
            local source = path.join(os.projectdir(), "src", "HLSL", shader .. ".hlsl")
            local output = path.join(shader_dir, shader .. ".cso")
            os.vrunv(fxc, {
                "/nologo",
                "/E", "main",
                "/T", profile,
                "/O3",
                "/Fo", output,
                source,
            })
        end

        os.cp(
            path.join(os.projectdir(), "MagnaScope.ini"),
            path.join(os.projectdir(), "Compile", "F4SE", "Plugins", "MagnaScope.ini")
        )
        os.cp(
            path.join(os.projectdir(), "MagnaScopeConfig.json"),
            path.join(os.projectdir(), "Compile", "F4SE", "Plugins", "MagnaScopeConfig.json")
        )

        local function stage_runtime(root)
            local plugin_dir = path.join(root, "F4SE", "Plugins")
            local staged_shader_dir = path.join(root, "Shaders", "MagnaScope")
            os.mkdir(plugin_dir)
            os.mkdir(staged_shader_dir)
            os.cp(
                target:targetfile(),
                path.join(plugin_dir, "MagnaScope.dll")
            )
            os.cp(
                path.join(os.projectdir(), "MagnaScope.ini"),
                path.join(plugin_dir, "MagnaScope.ini")
            )
            os.cp(
                path.join(os.projectdir(), "MagnaScopeConfig.json"),
                path.join(plugin_dir, "MagnaScopeConfig.json")
            )
            for shader, _ in pairs(shader_profiles) do
                os.cp(
                    path.join(shader_dir, shader .. ".cso"),
                    path.join(staged_shader_dir, shader .. ".cso")
                )
            end
        end

        -- Clean end-user tree: no PDB, import library, or build metadata.
        stage_runtime(
            path.join(os.projectdir(), "Package", "MagnaScope")
        )

        local mo2_mods_path = os.getenv("MAGNASCOPE_MO2_MODS_PATH")
        if mo2_mods_path then
            local deploy_dir = path.join(mo2_mods_path, "MagnaScope")
            stage_runtime(deploy_dir)
            cprint("${bright green}deployed MagnaScope to %s", deploy_dir)
        end
    end)

-- Executes the production automatic-STS shaders with the D3D11 WARP software
-- device. This proves the pixel shader changes only the lens region without
-- requiring Fallout 4 or a physical GPU; game-anchor verification remains a
-- separate in-game acceptance test.
target("AutoSTSShaderTest")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    add_files("tests/AutoSTSShaderHarness.cpp")
    add_defines("_UNICODE", "UNICODE", "NOMINMAX")
    add_syslinks("d3d11", "d3dcompiler", "dxgi")
    set_runtimes("MD")
    set_targetdir("build/tests")
    -- The harnesses resolve Compile/Shaders relative to the working
    -- directory. Without this, xmake run starts them in build/tests and
    -- they silently load whatever stale shader tree happens to sit there
    -- instead of the shaders just built, reporting failures that do not
    -- exist in the production output.
    set_rundir("$(projectdir)")

target("ScopeGeometryFillShaderTest")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    add_files("tests/ScopeGeometryFillShaderHarness.cpp")
    add_defines("_UNICODE", "UNICODE", "NOMINMAX")
    add_syslinks("d3d11", "d3dcompiler", "dxgi")
    set_runtimes("MD")
    set_targetdir("build/tests")
    -- The harnesses resolve Compile/Shaders relative to the working
    -- directory. Without this, xmake run starts them in build/tests and
    -- they silently load whatever stale shader tree happens to sit there
    -- instead of the shaders just built, reporting failures that do not
    -- exist in the production output.
    set_rundir("$(projectdir)")

target("DrawTimeEyeBoxTest")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    add_files("tests/DrawTimeEyeBoxHarness.cpp")
    add_includedirs("src")
    set_runtimes("MD")
    set_targetdir("build/tests")
    -- The harnesses resolve Compile/Shaders relative to the working
    -- directory. Without this, xmake run starts them in build/tests and
    -- they silently load whatever stale shader tree happens to sit there
    -- instead of the shaders just built, reporting failures that do not
    -- exist in the production output.
    set_rundir("$(projectdir)")

-- Executes the independent late reticle composite on D3D11 WARP. This
-- verifies local pivot scaling, optical-effect isolation, and physical lens
-- clipping without requiring Fallout 4 or a hardware GPU.
target("ReticleLayerShaderTest")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    add_files("tests/ReticleLayerShaderHarness.cpp")
    add_defines("_UNICODE", "UNICODE", "NOMINMAX")
    add_syslinks("d3d11", "d3dcompiler", "dxgi")
    set_runtimes("MD")
    set_targetdir("build/tests")
    -- The harnesses resolve Compile/Shaders relative to the working
    -- directory. Without this, xmake run starts them in build/tests and
    -- they silently load whatever stale shader tree happens to sit there
    -- instead of the shaders just built, reporting failures that do not
    -- exist in the production output.
    set_rundir("$(projectdir)")
