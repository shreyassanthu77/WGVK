#include "common.h"
#include <wgvk.h>
#include <wgvk_structs_impl.h>

extern const char raygenSource[];
extern const char rchitSource[];
extern const char rmissSource[];


#define vertexFloatCount 9
WGPUShaderModule compileGLSLModule(WGPUDevice device, const char* source, WGPUShaderStage stage){
    WGPUShaderSourceGLSL glslSource = {
    .chain.sType = WGPUSType_ShaderSourceGLSL,
        .code = {
            source,
            WGPU_STRLEN
        },
        .stage = stage
    };
    WGPUShaderModuleDescriptor vertexMD = {
        .nextInChain = &glslSource.chain
    };
    return wgpuDeviceCreateShaderModule(device, &vertexMD);
}

int main(){
    wgpu_base base = wgpu_init();
    printf("Initialized device: %p\n", base.device);
    WGPUBufferDescriptor vbDesc = {
        .usage = WGPUBufferUsage_Raytracing,
        .size = sizeof(float) * vertexFloatCount,
    };
    WGPUBuffer vertexBuffer = wgpuDeviceCreateBuffer(base.device, &vbDesc);
    float vertices[vertexFloatCount] = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0
    };

    wgpuQueueWriteBuffer(base.queue, vertexBuffer, 0, vertices, vertexFloatCount * sizeof(float));
    WGPURayTracingAccelerationGeometryDescriptor geometry = {
        .type = WGPURayTracingAccelerationGeometryType_Triangles,
        .index.format = WGPUIndexFormat_Undefined,
        .vertex = {
            .format = WGPUVertexFormat_Float32x3,
            .count = 3,
            .stride = sizeof(float) * vertexFloatCount,
            .offset = 0,
            .buffer = vertexBuffer
        }
    };
    

    WGPURayTracingAccelerationContainerDescriptor blasDesc = {
        .level = WGPURayTracingAccelerationContainerLevel_Bottom,
        .geometryCount = 1,
        .geometries = &geometry
    };



    WGPURayTracingAccelerationContainer blas = wgpuDeviceCreateRayTracingAccelerationContainer(base.device, &blasDesc);
    
    WGPURayTracingAccelerationInstanceTransformDescriptor identity = {0};
    identity.scale.x = 1;
    identity.scale.y = 1;
    identity.scale.z = 1;
    WGPURayTracingAccelerationInstanceDescriptor instance = {
        .instanceId = 0,
        .instanceOffset = 0,
        .transform = identity,
        .geometryContainer = blas,
    };
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(base.device, NULL);
        wgpuCommandEncoderBuildRayTracingAccelerationContainer(enc, blas);
        WGPUCommandBuffer cbuf = wgpuCommandEncoderFinish(enc, NULL);
        wgpuQueueSubmit(base.queue, 1, &cbuf);
    }

    WGPURayTracingAccelerationContainerDescriptor tlasDesc = {
        .level = WGPURayTracingAccelerationContainerLevel_Top,
        .instanceCount = 1,
        .instances = &instance
    };
    
    WGPURayTracingAccelerationContainer tlas = wgpuDeviceCreateRayTracingAccelerationContainer(base.device, &tlasDesc);
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(base.device, NULL);
        wgpuCommandEncoderBuildRayTracingAccelerationContainer(enc, tlas);
        WGPUCommandBuffer cbuf = wgpuCommandEncoderFinish(enc, NULL);
        wgpuQueueSubmit(base.queue, 1, &cbuf);
    }
    WGPUShaderModule raygenModule = compileGLSLModule(base.device, raygenSource, WGPUShaderStage_RayGen);
    WGPUShaderModule rchitModule  = compileGLSLModule(base.device, rchitSource,  WGPUShaderStage_ClosestHit);
    WGPUShaderModule rmissModule  = compileGLSLModule(base.device, rmissSource,  WGPUShaderStage_Miss);


    const VkRayTracingShaderGroupCreateInfoKHR sGroup = {
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
        .anyHitShader = VK_SHADER_UNUSED_KHR,
        .closestHitShader = 0,
    };
    
    const VkRayTracingPipelineCreateInfoKHR vkrtdesc = {
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .groupCount = 1,
        .pGroups = &sGroup,
    };

    VkPipeline vkrtP = VK_NULL_HANDLE;
    //VkResult result = base.device->functions.vkCreateRayTracingPipelinesKHR(base.device->device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &vkrtdesc, NULL, &vkrtP);
    //if(result != VK_SUCCESS){
    //    abort();
    //}
    //result = base.device->functions.vkCmdTraceRaysKHR()

    WGPURayTracingShaderBindingTableStageDescriptor stages[3] = {
        {
            .stage = WGPUShaderStage_RayGen,
            .module = raygenModule,
        },
        {
            .stage = WGPUShaderStage_ClosestHit,
            .module = rchitModule,
        },
        {
            .stage = WGPUShaderStage_Miss,
            .module = rmissModule,
        }
    };
    WGPURayTracingShaderBindingTableGroupDescriptor groups[3] = {
        {
            .type = WGPURayTracingShaderBindingTableGroupType_General, 
            .anyHitIndex = 0,
            .closestHitIndex = 0,
            .generalIndex = 0,
            .intersectionIndex = 0,
        },
        {
            .type = WGPURayTracingShaderBindingTableGroupType_TrianglesHitGroup, 
            .anyHitIndex = 0,
            .closestHitIndex = 1,
            .generalIndex = 0,
            .intersectionIndex = 0,
        },
        {
            .type = WGPURayTracingShaderBindingTableGroupType_TrianglesHitGroup, 
            .anyHitIndex = 0,
            .closestHitIndex = 1,
            .generalIndex = 0,
            .intersectionIndex = 0,
        },
    };

    WGPURayTracingShaderBindingTableDescriptor sbtableDesc = {
        .stageCount = 3,
        .stages = stages,
        .groupCount = 3,
        .groups = groups,
    };
    WGPURayTracingShaderBindingTable sbt = wgpuDeviceCreateRayTracingShaderBindingTable(base.device, &sbtableDesc);

    WGPURayTracingStateDescriptor rtState = {
        .shaderBindingTable = sbt,
        .maxPayloadSize = 64,
        .maxRecursionDepth = 8
    };
    WGPUBindGroupLayoutEntry bglEntries[3] = {
        [0] = {
            .binding = 0,
            .visibility = WGPUShaderStage_RayGen,
            .accelerationStructure = 1
        },
        [1] = {
            .binding = 1,
            .visibility = WGPUShaderStage_RayGen,
            .storageTexture = {
                .viewDimension = WGPUTextureViewDimension_2D,
                .access = WGPUStorageTextureAccess_WriteOnly,
                .format = WGPUTextureFormat_RGBA8Unorm
            }
        },
        [2] = {
            .binding = 2,
            .visibility = WGPUShaderStage_RayGen,
            .buffer = {
                .type = WGPUBufferBindingType_Uniform,
                .minBindingSize = sizeof(float) * 16,
                .hasDynamicOffset = 0
            }
        }
    };
    WGPUBindGroupLayoutDescriptor bglDesc = {
        .entries = bglEntries,
        .entryCount = 3,
    };
    WGPUBindGroupLayout bgLayout = wgpuDeviceCreateBindGroupLayout(base.device, &bglDesc);
    WGPUPipelineLayoutDescriptor pllDesc = {
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &bgLayout,
    };
    WGPUPipelineLayout plLayout = wgpuDeviceCreatePipelineLayout(base.device, &pllDesc);

    WGPURayTracingPipelineDescriptor rtpDescriptor = {
        .rayTracingState = rtState,
        .layout = plLayout
    };
    WGPURaytracingPipeline rtPipeline = wgpuDeviceCreateRayTracingPipeline(base.device, &rtpDescriptor);

    const WGPUTextureFormat storageTextureFormat = WGPUTextureFormat_RGBA32Float;
    WGPUTextureDescriptor storageTextureDescriptor = {
        .usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc,
        .dimension = WGPUTextureDimension_2D,
        .size = {
            .width = 1024,
            .height = 1024,
            .depthOrArrayLayers = 1
        },
        .format = storageTextureFormat,
        .mipLevelCount = 1,
        .sampleCount = 1,
        .viewFormatCount = 1,
        .viewFormats = &storageTextureFormat,
    };
    WGPUTexture storageTexture = wgpuDeviceCreateTexture(base.device, &storageTextureDescriptor);
    WGPUTextureViewDescriptor storageViewDescriptor = {
        .format = storageTextureFormat,
        .dimension = WGPUTextureViewDimension_2D,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = 1,
        .aspect = WGPUTextureAspect_All,
        .usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc,
    };

    WGPUTextureView storageTextureView = wgpuTextureCreateView(storageTexture,  &storageViewDescriptor);
    WGPUBuffer camerabuffer = wgpuDeviceCreateBuffer(base.device, &(const WGPUBufferDescriptor){
        .size = 64,
        .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst
    });
    struct Vector4{float x,y,z,w;} cameraData[4] = {
        {0, 0, -4, 0},
        {0, 0, 0, 0},
        {0, 1, 0, 0},
        {1.1, 0, 0, 0},
    };
    wgpuQueueWriteBuffer(base.queue, camerabuffer, 0, cameraData, 64);

    WGPUBindGroupEntry bindGroupEntries[3] = {
        {
            .binding = 0,
            .accelerationStructure = tlas
        },
        {
            .binding = 1,
            .textureView = storageTextureView
        },
        {
            .binding = 2,
            .buffer = camerabuffer,
            .size = WGPU_WHOLE_SIZE,
        },
    };

    WGPUBindGroupDescriptor bgDesc = {
        .layout = bgLayout,
        .entries = bindGroupEntries,
        .entryCount = 3,
    };
    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(base.device, &bgDesc);
    WGPUCommandEncoder cenc = wgpuDeviceCreateCommandEncoder(base.device, NULL);
    WGPURayTracingPassDescriptor rtDesc = {
        .maxRecursionDepth = 4,
        .maxPayloadSize = 64,
        .shaderBindingTable = sbt,
    };
    WGPURaytracingPassEncoder rtenc = wgpuCommandEncoderBeginRaytracingPass(cenc, &rtDesc);
    wgpuRaytracingPassEncoderSetPipeline(rtenc, rtPipeline);
    wgpuRaytracingPassEncoderSetBindGroup(rtenc, 0, bindGroup, 0, NULL);
    wgpuRaytracingPassEncoderTraceRays(rtenc, 0, 1, 2, 1024, 1024, 1);
    wgpuRaytracingPassEncoderEnd(rtenc);
    WGPUCommandBuffer cbuffer =  wgpuCommandEncoderFinish(cenc, NULL);
    wgpuQueueSubmit(base.queue, 1, &cbuffer);
    WGPUBuffer textureDump = wgpuDeviceCreateBuffer(base.device, &(const WGPUBufferDescriptor){
        .size = 1024 * 1024 * 16,
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst
    });
    {
        WGPUTexelCopyTextureInfo source = {
            .texture = storageTexture,
            .mipLevel = 0,
            .origin = {0},
            .aspect = WGPUTextureAspect_All,
        };
        WGPUTexelCopyBufferInfo dest = {
            .buffer = textureDump,
            .layout = {
                .bytesPerRow = 0,
                .rowsPerImage = 0,
                .offset = 0,
            }
        };
        WGPUCommandEncoder dumpEnc = wgpuDeviceCreateCommandEncoder(base.device, NULL);
        WGPUExtent3D copySize = {
            1024, 1024, 1
        };
        wgpuCommandEncoderCopyTextureToBuffer(dumpEnc, &source, &dest, &copySize);
        WGPUCommandBuffer buffer = wgpuCommandEncoderFinish(dumpEnc, NULL);
        wgpuQueueSubmit(base.queue, 1, &buffer);
        wgpuCommandEncoderRelease(dumpEnc);
        wgpuCommandBufferRelease(buffer);
        struct Vector4{float x,y,z,w;}* mapPointer = NULL;
        wgpuBufferMap(textureDump, WGPUMapMode_Read, 0, WGPU_WHOLE_MAP_SIZE, (void**)&mapPointer);
        for(size_t i = 0;i < 1024 * 1024;i++){
            printf("%f, ", mapPointer[i].x);
        }
    }
}



const char raygenSource[] = R"(#version 460
#extension GL_EXT_ray_tracing : require

// Binding for acceleration structure
layout(binding = 0) uniform accelerationStructureEXT topLevelAS;
// Output image
layout(binding = 1, rgba32f) uniform image2D image;
// Camera uniform buffer


layout(binding = 2) uniform CameraProperties {
    vec4 eye;
    vec4 target;
    vec4 up;
    vec4 fovY;
} camera;

// Ray payload - will be passed to closest hit or miss shader
layout(location = 0) rayPayloadEXT vec4 payload;

void main() {
    // Get the current pixel coordinate
    const vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);
    const vec2 inUV = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
    vec2 d = inUV * 2.0 - 1.0;

    // Calculate ray origin and direction using camera matrices
    vec3 origin = camera.eye.xyz;
    vec3 target = camera.target.xyz;
    vec3 direction = normalize(target - origin);
    vec3 left = cross(normalize(camera.up.xyz), direction);
    vec3 realup = normalize(cross(direction, left));
    float factor = tan(camera.fovY.x * 0.5f);
    vec3 raydirection = normalize(direction + factor * d.x * left + factor * d.y * realup);

    payload = vec4(raydirection.yx, 0.3f, 1);
    // Initialize payload

    // Trace ray
    traceRayEXT(
        topLevelAS,           // Acceleration structure
        gl_RayFlagsOpaqueEXT, // Ray flags
        0xFF,                 // Cull mask
        0,                    // sbtRecordOffset
        0,                    // sbtRecordStride
        0,                    // missIndex
        origin.xyz,           // Ray origin
        0.001,                // Min ray distance
        raydirection.xyz,     // Ray direction
        100.0,                // Max ray distance
        0                     // Payload location
    );
    
    // Write result to output image
    imageStore(image, ivec2(gl_LaunchIDEXT.xy), vec4(payload.xyz, 1.0f));
    //imageStore(image, ivec2(gl_LaunchIDEXT.xy), vec4(vec2(gl_LaunchIDEXT.xy * 0.01f),1,1));
}
)";
const char rchitSource[] = R"(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

// Bind scene descriptors
//layout(binding = 3) buffer SceneDesc { vec4 data[]; } sceneDesc;
//layout(binding = 4) uniform sampler2D textures[];

// Ray payload
layout(location = 0) rayPayloadInEXT vec4 payload;

// Hit attributes from intersection
hitAttributeEXT vec2 attribs;

// Shader record buffer index
//layout(binding = 4) uniform _ShaderRecordBuffer {
//    int materialID;
//} shaderRecordBuffer;

void main(){
    // Basic surface color (replace with your material system)
    vec3 hitColor = vec3(0.7, 0.7, 0.7);
    
    // Get hit triangle vertices
    int primitiveID = gl_PrimitiveID;
    int materialID = 0;//shaderRecordBuffer.materialID;
    
    // Simple diffuse shading based on normal
    vec3 barycentrics = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    
    // Calculate surface normal using barycentric coordinates
    // (In a real implementation, you would use vertex data)
    vec3 normal = normalize(vec3(0, 1, 0)); // Simplified normal
    
    // Direction to light (hardcoded for simplicity)
    vec3 lightDir = normalize(vec3(1, 1, 1));
    
    // Simple diffuse lighting
    float diffuse = max(dot(normal, lightDir), 0.2);
    
    // Set final color
    //payload = vec4(hitColor * diffuse, 1.0);
    payload = vec4(1.0,float(gl_InstanceID),0.0,1.0);
}
)";
const char rmissSource[] = R"(#version 460
#extension GL_EXT_ray_tracing : require

// Ray payload
layout(location = 0) rayPayloadInEXT vec4 payload;

void main(){
    // Sky color based on ray direction
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    
    // Simple gradient for sky
    float t = 0.5 * (dir.y + 1.0);
    vec3 skyColor = mix(vec3(1.0, 1.0, 1.0), vec3(0.5, 0.7, 1.0), t);
    
    // Write sky color to payload
    payload = vec4(skyColor, 1.0f);
})";