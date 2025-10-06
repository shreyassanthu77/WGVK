const std = @import("std");

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const use_vma = b.option(bool, "vma", "Use GPUOpen's VMA allocator (Requires C++)") orelse false;
    const support_drm = b.option(bool, "drm", "Support Direct Rendering Infrastructure Surfaces (Linux)") orelse false;
    const enable_x11 = b.option(bool, "x11", "Enable X11 support") orelse true;
    const enable_wayland = b.option(bool, "wayland", "Enable Wayland support") orelse true;
    const wgvk_options = WgvkOptions{
        .target = target,
        .optimize = optimize,
        .use_vma = use_vma,
        .support_drm = support_drm,
        .enable_x11 = enable_x11,
        .enable_wayland = enable_wayland,
    };

    const wgvk_lib = try buildLib(b, wgvk_options);
    b.installArtifact(wgvk_lib);

    const examples_step = b.step("examples", "Build examples");
    const examples: []const []const u8 = &.{
        "asynchronous_loading",
        "basic_compute",
        "glfw_surface",
        "multi_submit",
        "rgfw_surface",
    };
    for (examples) |example| {
        const example_output = try buildExample(b, wgvk_options, wgvk_lib, example);
        examples_step.dependOn(&example_output.step);
    }

    const build_all_step = b.step("all", "Build all targets");
    const build_targets: []const std.Target.Query = &.{
        .{ .cpu_arch = .aarch64, .os_tag = .windows },
        .{ .cpu_arch = .aarch64, .os_tag = .macos },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .gnu },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .x86_64, .os_tag = .windows },
        .{ .cpu_arch = .x86_64, .os_tag = .macos },
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .musl },
    };
    for (build_targets) |t| {
        const resolved_target = b.resolveTargetQuery(t);
        const lib = try buildLib(b, .{
            .target = resolved_target,
            .optimize = optimize,
            .use_vma = use_vma,
            .support_drm = support_drm,
            .enable_x11 = enable_x11,
            .enable_wayland = enable_wayland,
        });
        const target_output = b.addInstallArtifact(lib, .{
            .dest_dir = .{
                .override = .{
                    .custom = try t.zigTriple(b.allocator),
                },
            },
        });
        build_all_step.dependOn(&target_output.step);
    }
}

const WgvkOptions = struct {
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    use_vma: bool,
    support_drm: bool = false,
    enable_x11: bool,
    enable_wayland: bool,
};

fn buildLib(b: *std.Build, options: WgvkOptions) !*std.Build.Step.Compile {
    const wgvk_mod = b.createModule(.{
        .target = options.target,
        .optimize = options.optimize,
        .link_libc = true,
    });

    wgvk_mod.addCMacro("_POSIX_C_SOURCE", "200809L");
    wgvk_mod.addIncludePath(b.path("./include/"));
    wgvk_mod.addCSourceFiles(.{
        .files = &.{
            "src/wgvk.c",
        },
        .flags = &.{
            "-std=c11",
        },
    });
    if (options.use_vma) {
        wgvk_mod.addCMacro("USE_VMA_ALLOCATOR", "1");
        wgvk_mod.addCSourceFiles(.{
            .files = &.{"src/vma_impl.cpp"},
            .flags = &.{"-std=c++17"},
        });
        wgvk_mod.link_libcpp = true;
    }

    switch (options.target.result.os.tag) {
        .windows => {
            wgvk_mod.addCMacro("SUPPORT_WIN32_SURFACE", "1");
        },
        .macos => {
            if (b.lazyDependency("xcode_frameworks", .{})) |frameworks| {
                wgvk_mod.addSystemFrameworkPath(frameworks.path("Frameworks"));
                wgvk_mod.addSystemIncludePath(frameworks.path("include"));
                wgvk_mod.addLibraryPath(frameworks.path("lib"));
            }
            wgvk_mod.addCMacro("SUPPORT_METAL_SURFACE", "1");
            wgvk_mod.linkFramework("Metal", .{});
            wgvk_mod.linkFramework("QuartzCore", .{});
        },
        else => {
            const is_android = options.target.result.abi.isAndroid();
            if (!is_android and options.support_drm) {
                wgvk_mod.addCMacro("SUPPORT_DRM_SURFACE", "1");
            }

            if (!is_android and options.enable_x11) if (b.lazyDependency("x11_headers", .{})) |x11| {
                wgvk_mod.addCMacro("SUPPORT_XLIB_SURFACE", "1");
                wgvk_mod.linkLibrary(x11.artifact("x11-headers"));
            };
            if (options.enable_wayland) if (b.lazyDependency("wayland_headers", .{})) |wayland| {
                wgvk_mod.addCMacro("SUPPORT_WAYLAND_SURFACE", "1");
                wgvk_mod.addIncludePath(wayland.path("wayland"));
            };
        },
    }

    const wgvk_lib = b.addLibrary(.{
        .name = "wgvk",
        .root_module = wgvk_mod,
    });
    wgvk_lib.installHeadersDirectory(b.path("./include/webgpu"), "webgpu", .{});
    wgvk_lib.installHeader(b.path("./include/wgvk.h"), "wgvk.h");
    wgvk_lib.installHeader(b.path("./include/wgvk_config.h"), "wgvk_config.h");

    return wgvk_lib;
}

fn buildExample(
    b: *std.Build,
    options: WgvkOptions,
    wgvk_lib: *std.Build.Step.Compile,
    example: []const u8,
) !*std.Build.Step.InstallArtifact {
    const example_exe = b.addExecutable(.{
        .name = example,
        .root_module = b.createModule(.{
            .target = options.target,
            .optimize = options.optimize,
        }),
    });
    example_exe.root_module.addCMacro("_POSIX_C_SOURCE", "200809L");
    example_exe.addIncludePath(b.path("include"));
    example_exe.addCSourceFile(.{
        .file = b.path(b.fmt("examples/{s}.c", .{example})),
    });
    example_exe.linkLibrary(wgvk_lib);

    if (b.lazyDependency("glfw", .{
        .target = options.target,
        .optimize = options.optimize,
    })) |glfw| {
        example_exe.linkLibrary(glfw.artifact("glfw"));
    }

    switch (options.target.result.os.tag) {
        .windows => {
            example_exe.root_module.addCMacro("SUPPORT_WIN32_SURFACE", "1");
        },
        .macos => {
            example_exe.root_module.addCMacro("SUPPORT_METAL_SURFACE", "1");
        },
        .linux => {
            const is_android = options.target.result.abi.isAndroid();
            if (!is_android and options.enable_x11) if (b.lazyDependency("x11_headers", .{})) |x11| {
                example_exe.root_module.addCMacro("SUPPORT_XLIB_SURFACE", "1");
                example_exe.linkLibrary(x11.artifact("x11-headers"));
                example_exe.linkSystemLibrary("X11");
                example_exe.linkSystemLibrary("Xrandr");
            };
            if (!is_android and options.enable_wayland) if (b.lazyDependency("wayland_headers", .{})) |wayland| {
                example_exe.root_module.addCMacro("SUPPORT_WAYLAND_SURFACE", "1");
                example_exe.addIncludePath(wayland.path("wayland"));
                example_exe.linkSystemLibrary("wayland-client");
            };
        },
        else => {
            return error.UnsupportedPlatform;
        },
    }

    const example_output = b.addInstallArtifact(example_exe, .{
        .dest_dir = .{
            .override = .{
                .custom = "examples",
            },
        },
    });

    return example_output;
}
