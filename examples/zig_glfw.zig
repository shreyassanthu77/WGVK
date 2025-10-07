const std = @import("std");
const log = std.log.scoped(.zig_glfw);
const builtin = @import("builtin");
// const wk = @import("wgvk_c");
const wk = @cImport({
    @cInclude("wgvk.h");
});
const c = @cImport({
    @cDefine("GLFW_INCLUDE_NONE", "1");
    @cInclude("GLFW/glfw3.h");
    switch (builtin.os.tag) {
        .windows => @cDefine("GLFW_EXPOSE_NATIVE_WIN32", "1"),
        .macos => @cDefine("GLFW_EXPOSE_NATIVE_COCOA", "1"),
        .linux => {
            @cDefine("GLFW_EXPOSE_NATIVE_X11", "1");
            @cDefine("GLFW_EXPOSE_NATIVE_WAYLAND", "1");
        },
        else => @compileError("unsupported OS"),
    }
    @cInclude("GLFW/glfw3native.h");
});

pub fn main() !void {
    const layernames: []const [*]const u8 = &.{"VK_LAYER_KHRONOS_validation"};
    var layersel = wk.WGPUInstanceLayerSelection{
        .chain = .{
            .next = null,
            .sType = wk.WGPUSType_InstanceLayerSelection,
        },
        .instanceLayers = layernames.ptr,
        .instanceLayerCount = layernames.len,
    };
    const instance_features: []const wk.WGPUInstanceFeatureName = &.{
        wk.WGPUInstanceFeatureName_TimedWaitAny,
        wk.WGPUInstanceFeatureName_ShaderSourceSPIRV,
    };
    const instance = wk.wgpuCreateInstance(&wk.WGPUInstanceDescriptor{
        .nextInChain = if (builtin.mode == .Debug) &layersel.chain else null,
        .requiredFeatures = instance_features.ptr,
        .requiredFeatureCount = instance_features.len,
    });
    if (instance == null) return error.InstanceCreationFailed;
    defer wk.wgpuInstanceRelease(instance);
    log.debug("created wgpu instance: {}", .{instance.?});

    var adapter_ptr: wk.WGPUAdapter = null;
    awaitFuture(instance, wk.wgpuInstanceRequestAdapter(instance, &wk.WGPURequestAdapterOptions{
        .featureLevel = wk.WGPUFeatureLevel_Core,
    }, .{
        .callback = adapterCallback,
        .userdata1 = @ptrCast(@alignCast(&adapter_ptr)),
    }));
    if (adapter_ptr == null) return error.NoAdapterFound;
    const adapter = adapter_ptr.?;
    defer wk.wgpuAdapterRelease(adapter);
    log.debug("acquired adapter: {}", .{adapter});

    var device_ptr: wk.WGPUDevice = null;
    awaitFuture(instance, wk.wgpuAdapterRequestDevice(adapter, &wk.WGPUDeviceDescriptor{
        .nextInChain = null,
        .label = sv("WGPU Device"),
        .requiredFeatures = null,
        .requiredFeatureCount = 0,
        .requiredLimits = null,
    }, .{
        .callback = deviceCallback,
        .userdata1 = @ptrCast(@alignCast(&device_ptr)),
        .mode = wk.WGPUCallbackMode_WaitAnyOnly,
    }));
    if (device_ptr == null) return error.NoDeviceFound;
    const device = device_ptr.?;
    defer wk.wgpuDeviceRelease(device);
    log.debug("acquired device: {}", .{device});

    const queue = wk.wgpuDeviceGetQueue(device) orelse return error.NoQueueFound;
    defer wk.wgpuQueueRelease(queue);
    log.debug("queue: {}", .{queue});

    if (c.glfwInit() != 1) return error.GLFWInitFailed;
    defer c.glfwTerminate();

    c.glfwWindowHint(c.GLFW_CLIENT_API, c.GLFW_NO_API);
    const window = c.glfwCreateWindow(1280, 720, "GLFW Window", null, null) orelse return error.WindowCreationFailed;
    defer c.glfwDestroyWindow(window);
    _ = c.glfwSetKeyCallback(window, keyCallback);

    var width: c_int, var height: c_int = .{ 0, 0 };
    c.glfwGetWindowSize(window, &width, &height);
    log.debug("window size: {}x{}", .{ width, height });

    const surface = glfwCreateWindowWGPUSurface(instance, window);
    // var capabilities: wk.WGPUSurfaceCapabilities = .{};
    // _ = wk.wgpuSurfaceGetCapabilities(surface, adapter, &capabilities);
    wk.wgpuSurfaceConfigure(surface, &wk.WGPUSurfaceConfiguration{
        .alphaMode = wk.WGPUCompositeAlphaMode_Opaque,
        .presentMode = wk.WGPUPresentMode_Immediate,
        .device = device,
        .format = wk.WGPUTextureFormat_BGRA8Unorm,
        .width = @intCast(width),
        .height = @intCast(height),
        .usage = wk.WGPUTextureUsage_RenderAttachment,
    });

    while (c.glfwWindowShouldClose(window) == 0) {
        c.glfwPollEvents();
        c.glfwSwapBuffers(window);
    }
}

fn adapterCallback(status: wk.WGPURequestAdapterStatus, adapter: wk.WGPUAdapter, message_view: wk.WGPUStringView, userdata1: ?*anyopaque, userdata2: ?*anyopaque) callconv(.c) void {
    _ = userdata2; // autofix

    if (status != wk.WGPURequestAdapterStatus_Success) {
        const message = message_view.data[0..message_view.length];
        std.debug.print("adapterCallback: status={} message={s}\n", .{ status, message });
        return;
    }
    const adapter_ptr: *wk.WGPUAdapter = @ptrCast(@alignCast(userdata1 orelse return));
    adapter_ptr.* = adapter.?;
}

fn deviceCallback(status: wk.WGPURequestDeviceStatus, device: wk.WGPUDevice, message_view: wk.WGPUStringView, userdata1: ?*anyopaque, userdata2: ?*anyopaque) callconv(.c) void {
    _ = userdata2; // autofix

    if (status != wk.WGPURequestDeviceStatus_Success) {
        const message = message_view.data[0..message_view.length];
        std.debug.print("deviceCallback: status={} message={s}\n", .{ status, message });
        return;
    }
    const device_ptr: *wk.WGPUDevice = @ptrCast(@alignCast(userdata1 orelse return));
    device_ptr.* = device.?;
}

fn keyCallback(window: ?*c.GLFWwindow, key: c_int, scancode: c_int, action: c_int, mods: c_int) callconv(.c) void {
    _ = scancode; // autofix
    _ = mods; // autofix

    if (key == c.GLFW_KEY_ESCAPE and action == c.GLFW_PRESS) {
        c.glfwSetWindowShouldClose(window, 1);
    }
}

fn glfwCreateWindowWGPUSurface(instance: wk.WGPUInstance, window: *c.GLFWwindow) *wk.WGPUSurfaceImpl {
    const surface_handle = switch (builtin.os.tag) {
        .windows => blk: {
            const hwnd = c.glfwGetWin32Window(window);
            const hinstance = c.GetModuleHandleA(null);
            var fromWindowsHWND = wk.WGPUSurfaceSourceWindowsHWND{
                .hwnd = hwnd,
                .hinstance = hinstance,
                .chain = .{
                    .sType = wk.WGPUSType_SurfaceSourceWindowsHWND,
                    .next = null,
                },
            };
            break :blk &fromWindowsHWND.chain;
        },
        .macos => {
            @panic("TODO");
        },
        .linux => switch (c.glfwGetPlatform()) {
            c.GLFW_PLATFORM_X11 => blk: {
                const display = c.glfwGetX11Display();
                const x_window = c.glfwGetX11Window(window);
                var fromX11 = wk.WGPUSurfaceSourceXlibWindow{
                    .display = display,
                    .window = x_window,
                    .chain = .{
                        .sType = wk.WGPUSType_SurfaceSourceXlibWindow,
                        .next = null,
                    },
                };
                break :blk &fromX11.chain;
            },
            c.GLFW_PLATFORM_WAYLAND => blk: {
                const display = c.glfwGetWaylandDisplay();
                const surface = c.glfwGetWaylandWindow(window);
                var fromWayland = wk.WGPUSurfaceSourceWaylandSurface{
                    .display = display,
                    .surface = surface,
                    .chain = .{
                        .sType = wk.WGPUSType_SurfaceSourceWaylandSurface,
                        .next = null,
                    },
                };
                break :blk &fromWayland.chain;
            },
            else => @panic("unsupported platform"),
        },
        else => comptime unreachable,
    };
    return wk.wgpuInstanceCreateSurface(instance, &.{
        .label = sv("GLFW Surface"),
        .nextInChain = surface_handle,
    }) orelse {
        log.err("unable to create surface", .{});
        std.process.exit(1);
    };
}

fn awaitFuture(instance: wk.WGPUInstance, future: wk.WGPUFuture) void {
    var future_wait_info = wk.WGPUFutureWaitInfo{
        .future = future,
    };
    const start_time = std.time.milliTimestamp();
    while (future_wait_info.completed == 0) {
        _ = wk.wgpuInstanceWaitAny(instance, 1, &future_wait_info, std.time.ns_per_ms);
    }
    const elapsed_time = std.time.milliTimestamp() - start_time;
    log.debug("future completed in {d}ms", .{elapsed_time});
}

inline fn sv(s: []const u8) wk.WGPUStringView {
    return .{ .data = s.ptr, .length = s.len };
}
