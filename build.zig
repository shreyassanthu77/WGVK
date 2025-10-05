const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const use_vma = b.option(bool, "vma", "Use GPUOpen's VMA allocator (Requires C++)") orelse false;
    const support_drm = b.option(bool, "drm", "Support Direct Rendering Infrastructure Surfaces (Linux)") orelse false;
    const enable_x11 = b.option(bool, "x11", "Enable X11 support") orelse true;
    const enable_wayland = b.option(bool, "wayland", "Enable Wayland support") orelse true;

    const wgvk_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
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
    if (use_vma) {
        wgvk_mod.addCMacro("USE_VMA_ALLOCATOR", "1");
        wgvk_mod.addCSourceFiles(.{
            .files = &.{"src/vma_impl.cpp"},
            .flags = &.{"-std=c++17"},
        });
        wgvk_mod.link_libcpp = true;
    }

    switch (target.result.os.tag) {
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
            const is_android = target.result.abi.isAndroid();
            if (!is_android and support_drm) {
                wgvk_mod.addCMacro("SUPPORT_DRM_SURFACE", "1");
            }

            const is_linux = target.result.os.tag == .linux;
            if (!is_android and is_linux) {
                if (enable_x11) if (b.lazyDependency("x11_headers", .{})) |x11| {
                    wgvk_mod.addCMacro("SUPPORT_XLIB_SURFACE", "1");
                    wgvk_mod.linkLibrary(x11.artifact("x11-headers"));
                };
                if (enable_wayland) if (b.lazyDependency("wayland_headers", .{})) |wayland| {
                    wgvk_mod.addCMacro("SUPPORT_WAYLAND_SURFACE", "1");
                    wgvk_mod.addIncludePath(wayland.path("wayland"));
                };
            }
        },
    }

    const wgvk_lib = b.addLibrary(.{
        .name = "wgvk",
        .root_module = wgvk_mod,
    });
    b.installArtifact(wgvk_lib);
    wgvk_lib.installHeadersDirectory(b.path("./include/webgpu"), "webgpu", .{});
    wgvk_lib.installHeader(b.path("./include/wgvk.h"), "wgvk.h");
    wgvk_lib.installHeader(b.path("./include/wgvk_config.h"), "wgvk_config.h");
}
