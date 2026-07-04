//! # Usage
//! The intended way to use this library is as follows:
//! 1. Add this library to your project as a dependency.
//!     ```bash
//!     zig fetch --save git+https://github.com/manuel5975p/WGVK#<commit-or-tag>
//!     ```
//! 2. Add the following to your build.zig file:
//!     ```zig
//!     const wgvk = @import("WGVK"); // the name of the dependency as defined in your build.zig.zon file
//!
//!     pub fn build(b: *std.Build) !void {
//!         // other build steps
//!         const wgvk_lib = wgvk.buildLib(b, .{
//!             .target = target,
//!             .optimize = optimize,
//!             // other optional features
//!             .use_vma = use_vma, // use GPUOpen's VMA allocator (Requires and links libc++)
//!             .support_drm = support_drm, // support Direct Rendering Infrastructure Surfaces (Linux)
//!             .enable_x11 = enable_x11, // enable X11 support (Requires system libX11-dev. Enable on Linux only.)
//!             .enable_wayland = enable_wayland, // enable Wayland support (Requires system libwayland-client. Enable on Linux only.)
//!         });
//!
//!         switch (target.result.os.tag) {
//!             .windows => {
//!                 exe.root_module.linkSystemLibrary("gdi32", .{});
//!             },
//!             .macos => {
//!                 // see below for more details on how to cross-compile to macos
//!                 exe.root_module.linkFramework("Foundation", .{});
//!                 exe.root_module.linkFramework("Metal", .{});
//!                 exe.root_module.linkFramework("Cocoa", .{});
//!             },
//!             .linux => {
//!                 const is_android = target.result.abi.isAndroid();
//!                 if (!is_android and enable_x11) {
//!                     exe.root_module.linkSystemLibrary("X11", .{});
//!                     exe.root_module.linkSystemLibrary("Xrandr", .{});
//!                 }
//!                 if (!is_android and enable_wayland) {
//!                     exe.root_module.linkSystemLibrary("wayland-client", .{});
//!                 }
//!             },
//!             else => return error.UnsupportedPlatform,
//!         }
//!
//!         exe.root_module.linkLibrary(wgvk_lib);
//!     }
//!     ```
//!
//! ## Cross-compiling to MacOS
//! If you are cross-compiling to MacOS, you will need xcode_frameworks to be
//! added to your module search path. This can be done by adding the following.
//!
//! > NOTE: you might need to tweak a few things depending on your setup.
//!
//! 1. Install xcode_frameworks
//!     ```bash
//!     zig fetch --save=xcode_frameworks https://code.hexops.org/hexops/xcode-frameworks
//!     ```
//! 2. Add the following to your build.zig file:
//!     ```zig
//!     fn build(b: *std.Build) !void {
//!         // your exe + wgvk_lib build steps
//!         if (target.result.os.tag == .macos) {
//!             // put this block next to the macos configuration in the previous step
//!             const xcode_frameworks = b.dependency("xcode_frameworks", .{});
//!
//!             b.addSystemFrameworkPath(xcode_frameworks.path("Frameworks"));
//!             b.addSystemIncludePath(xcode_frameworks.path("include"));
//!             b.addLibraryPath(xcode_frameworks.path("lib"));
//!         }
//!     }
//!     ```
const std = @import("std");

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const use_vma = b.option(bool, "vma", "Use GPUOpen's VMA allocator (Requires C++)") orelse false;
    const support_drm = b.option(bool, "drm", "Support Direct Rendering Infrastructure Surfaces (Linux)") orelse false;
    const enable_x11 = b.option(bool, "x11", "Enable X11 support") orelse true;
    const enable_wayland = b.option(bool, "wayland", "Enable Wayland support") orelse true;
    const skip_build = b.option(bool, "skip_build", "Skips everything in the build script. used internally") orelse false;
    if (skip_build) return;

    const wgvk_options = WgvkOptions{
        .target = target,
        .optimize = optimize,
        .use_vma = use_vma,
        .support_drm = support_drm,
        .enable_x11 = enable_x11,
        .enable_wayland = enable_wayland,
    };

    const wgvk_lib = try buildLib1(b, wgvk_options);
    b.installArtifact(wgvk_lib);

    const examples_step = b.step("examples", "Build examples");
    const examples: []const []const u8 = &.{
        "asynchronous_loading",
        "basic_compute",
        "glfw_surface",
        "multi_submit",
        "rgfw_surface",
    };
    for (examples) |src| {
        if (src.len == 0) continue;
        const example_output = buildExample(b, wgvk_options, wgvk_lib, src) catch continue;
        examples_step.dependOn(&example_output.step);
    }

    const build_all_step = b.step("all", "Build all targets");

    const ios_min_version: std.Target.Query.OsVersion = .{
        .semver = .{ .major = 13, .minor = 0, .patch = 0 },
    };

    const build_targets: []const std.Target.Query = &.{
        .{ .cpu_arch = .aarch64, .os_tag = .windows },
        .{ .cpu_arch = .aarch64, .os_tag = .macos },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .gnu },
        .{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .x86_64, .os_tag = .windows },
        .{ .cpu_arch = .x86_64, .os_tag = .macos },
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
        .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .musl },
        .{ .cpu_arch = .aarch64, .os_tag = .ios, .os_version_min = ios_min_version },
        .{ .cpu_arch = .aarch64, .os_tag = .ios, .abi = .simulator, .os_version_min = ios_min_version },
        .{ .cpu_arch = .x86_64, .os_tag = .ios, .abi = .simulator, .os_version_min = ios_min_version },
    };
    for (build_targets) |t| {
        const resolved_target = b.resolveTargetQuery(t);
        // skip iOS targets on non macOS hosts
        if (resolved_target.result.os.tag == .ios and b.graph.host.result.os.tag != .macos) continue;

        const lib = try buildLib1(b, .{
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

pub fn buildLib(b: *std.Build, options: WgvkOptions) !*std.Build.Step.Compile {
    const self = b.dependencyFromBuildZig(@This(), .{
        .target = options.target,
        .optimize = options.optimize,
        .skip_build = true,
    });
    return buildLib1(self.builder, options);
}

fn buildLib1(b: *std.Build, options: WgvkOptions) !*std.Build.Step.Compile {
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
            wgvk_mod.addCMacro("SUPPORT_METAL_SURFACE", "1");
        },
        .ios => {
            wgvk_mod.addCMacro("SUPPORT_METAL_SURFACE", "1");
            if (b.lazyDependency("xcode_frameworks", .{})) |frameworks| {
                wgvk_mod.addSystemFrameworkPath(frameworks.path("Frameworks"));
                wgvk_mod.addSystemIncludePath(frameworks.path("include"));
                wgvk_mod.addLibraryPath(frameworks.path("lib"));
            }
        },
        else => {
            const is_android = options.target.result.abi.isAndroid();
            if (!is_android and options.support_drm) {
                wgvk_mod.addCMacro("SUPPORT_DRM_SURFACE", "1");
            }

            if (!is_android and options.enable_x11) if (b.lazyDependency("x11_headers", .{})) |x11| {
                wgvk_mod.addCMacro("SUPPORT_XLIB_SURFACE", "1");
                wgvk_mod.addIncludePath(x11.path("."));
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
    example_exe.root_module.addIncludePath(b.path("include"));
    const file_path = b.path(b.fmt("examples/{s}.c", .{example}));
    const language: std.Build.Module.CSourceLanguage = switch (options.target.result.os.tag) {
        .macos => if (std.mem.eql(u8, example, "rgfw_surface")) .c else .objective_c,
        else => .c,
    };

    if (options.target.result.os.tag == .macos) {
        example_exe.root_module.addCMacro("_DARWIN_C_SOURCE", "1");
    }
    example_exe.root_module.addCSourceFile(.{
        .file = file_path,
        .language = language,
    });
    example_exe.root_module.linkLibrary(wgvk_lib);

    if (std.mem.eql(u8, example, "glfw_surface")) {
        if (b.lazyDependency("glfw", .{
            .target = options.target,
            .optimize = options.optimize,
            .x11 = options.enable_x11,
            .wayland = options.enable_wayland,
        })) |glfw| {
            example_exe.root_module.linkLibrary(glfw.artifact("glfw"));
        }
    }

    switch (options.target.result.os.tag) {
        .windows => {
            example_exe.root_module.linkSystemLibrary("gdi32", .{});
        },
        .macos => {
            if (b.lazyDependency("xcode_frameworks", .{})) |frameworks| {
                example_exe.root_module.addSystemFrameworkPath(frameworks.path("Frameworks"));
                example_exe.root_module.addSystemIncludePath(frameworks.path("include"));
                example_exe.root_module.addLibraryPath(frameworks.path("lib"));
            }
            example_exe.root_module.linkFramework("Foundation", .{});
            example_exe.root_module.linkFramework("Metal", .{});
            example_exe.root_module.linkFramework("Cocoa", .{});
        },
        .linux => {
            const is_android = options.target.result.abi.isAndroid();
            if (!is_android and options.enable_x11) {
                example_exe.root_module.linkSystemLibrary("X11", .{});
                example_exe.root_module.linkSystemLibrary("Xrandr", .{});
            }
            if (!is_android and options.enable_wayland) {
                example_exe.root_module.linkSystemLibrary("wayland-client", .{});
            }
        },
        .ios => {
            return error.UnsupportedPlatform;
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
