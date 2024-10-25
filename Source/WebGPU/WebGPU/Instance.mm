/*
 * Copyright (c) 2021-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"
#import "Instance.h"

#import "APIConversions.h"
#import "Adapter.h"
#import "HardwareCapabilities.h"
#import "PresentationContext.h"
#import <cstring>
#import <dlfcn.h>
#import <wtf/BlockPtr.h>
#import <wtf/MachSendRight.h>
#import <wtf/StdLibExtras.h>
#import <wtf/TZoneMallocInlines.h>

namespace WebGPU {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Instance);

Ref<Instance> Instance::create(const WGPUInstanceDescriptor& descriptor)
{
    if (!descriptor.nextInChain)
        return Instance::createInvalid();

    if (descriptor.nextInChain->sType != static_cast<WGPUSType>(WGPUSTypeExtended_InstanceCocoaDescriptor))
        return Instance::createInvalid();

    const WGPUInstanceCocoaDescriptor& cocoaDescriptor = reinterpret_cast<const WGPUInstanceCocoaDescriptor&>(*descriptor.nextInChain);

    if (cocoaDescriptor.chain.next)
        return Instance::createInvalid();

    return adoptRef(*new Instance(cocoaDescriptor.scheduleWorkBlock, reinterpret_cast<const WTF::MachSendRight*>(cocoaDescriptor.webProcessResourceOwner)));
}

Instance::Instance(WGPUScheduleWorkBlock scheduleWorkBlock, const MachSendRight* webProcessResourceOwner)
    : m_webProcessID(webProcessResourceOwner ? std::optional<MachSendRight>(*webProcessResourceOwner) : std::nullopt)
    , m_scheduleWorkBlock(scheduleWorkBlock ? WTFMove(scheduleWorkBlock) : ^(WGPUWorkItem workItem) { defaultScheduleWork(WTFMove(workItem)); })
{
}

Instance::Instance()
    : m_scheduleWorkBlock(^(WGPUWorkItem workItem) { defaultScheduleWork(WTFMove(workItem)); })
    , m_isValid(false)
{
}

Instance::~Instance() = default;

Ref<PresentationContext> Instance::createSurface(const WGPUSurfaceDescriptor& descriptor)
{
    return PresentationContext::create(descriptor, *this);
}

void Instance::scheduleWork(WorkItem&& workItem)
{
    m_scheduleWorkBlock(makeBlockPtr(WTFMove(workItem)).get());
}

const std::optional<const MachSendRight>& Instance::webProcessID() const
{
    return m_webProcessID;
}

void Instance::defaultScheduleWork(WGPUWorkItem&& workItem)
{
    Locker locker(m_lock);
    m_pendingWork.append(WTFMove(workItem));
}

void Instance::processEvents()
{
    while (true) {
        Deque<WGPUWorkItem> localWork;
        {
            Locker locker(m_lock);
            std::swap(m_pendingWork, localWork);
        }
        if (localWork.isEmpty())
            return;
        for (auto& workItem : localWork)
            workItem();
    }
}

static NSArray<id<MTLDevice>> *sortedDevices(NSArray<id<MTLDevice>> *devices, WGPUPowerPreference powerPreference)
{
    switch (powerPreference) {
    case WGPUPowerPreference_Undefined:
        return devices;
    case WGPUPowerPreference_LowPower:
#if PLATFORM(MAC) || PLATFORM(MACCATALYST)
        return [devices sortedArrayWithOptions:NSSortStable usingComparator:^NSComparisonResult (id<MTLDevice> obj1, id<MTLDevice> obj2)
        {
            if (obj1.lowPower == obj2.lowPower)
                return NSOrderedSame;
            if (obj1.lowPower)
                return NSOrderedAscending;
            return NSOrderedDescending;
        }];
#else
        return devices;
#endif
    case WGPUPowerPreference_HighPerformance:
#if PLATFORM(MAC) || PLATFORM(MACCATALYST)
        return [devices sortedArrayWithOptions:NSSortStable usingComparator:^NSComparisonResult (id<MTLDevice> obj1, id<MTLDevice> obj2)
        {
            if (obj1.lowPower == obj2.lowPower)
                return NSOrderedSame;
            if (obj1.lowPower)
                return NSOrderedDescending;
            return NSOrderedAscending;
        }];
#else
        return devices;
#endif
    case WGPUPowerPreference_Force32:
        ASSERT_NOT_REACHED();
        return nil;
    }
}

void Instance::loseAllDevices()
{
    for (WeakPtr ptr : m_weakAdapters) {
        if (RefPtr refPtr = ptr.get())
            refPtr->makeInvalid();
    }

    m_weakAdapters.clear();
}

void Instance::requestAdapter(const WGPURequestAdapterOptions& options, CompletionHandler<void(WGPURequestAdapterStatus, Ref<Adapter>&&, String&&)>&& callback)
{
#if PLATFORM(MAC) || PLATFORM(MACCATALYST)
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
#else
    NSMutableArray<id<MTLDevice>> *devices = [NSMutableArray array];
    if (id<MTLDevice> device = MTLCreateSystemDefaultDevice())
        [devices addObject:device];
#endif

    // FIXME: Deal with options.compatibleSurface.

    auto sortedDevices = WebGPU::sortedDevices(devices, options.powerPreference);

    if (options.nextInChain) {
        callback(WGPURequestAdapterStatus_Error, Adapter::createInvalid(*this), "Unknown descriptor type"_s);
        return;
    }

    if (options.forceFallbackAdapter) {
        callback(WGPURequestAdapterStatus_Unavailable, Adapter::createInvalid(*this), "No adapters present"_s);
        return;
    }

    if (!sortedDevices) {
        callback(WGPURequestAdapterStatus_Error, Adapter::createInvalid(*this), "Unknown power preference"_s);
        return;
    }

    if (!sortedDevices.count) {
        callback(WGPURequestAdapterStatus_Unavailable, Adapter::createInvalid(*this), "No adapters present"_s);
        return;
    }

    if (!sortedDevices[0]) {
        callback(WGPURequestAdapterStatus_Error, Adapter::createInvalid(*this), "Adapter is internally null"_s);
        return;
    }

    auto device = sortedDevices[0];

    auto deviceCapabilities = hardwareCapabilities(device);

    if (!deviceCapabilities) {
        callback(WGPURequestAdapterStatus_Error, Adapter::createInvalid(*this), "Device does not support WebGPU"_s);
        return;
    }

    auto adapter = Adapter::create(sortedDevices[0], *this, options.xrCompatible, WTFMove(*deviceCapabilities));
    m_weakAdapters.append(adapter.ptr());
    callback(WGPURequestAdapterStatus_Success, WTFMove(adapter), { });
}

} // namespace WebGPU

#pragma mark WGPU Stubs

void wgpuInstanceReference(WGPUInstance instance)
{
    WebGPU::fromAPI(instance).ref();
}

void wgpuInstanceRelease(WGPUInstance instance)
{
    WebGPU::fromAPI(instance).deref();
}

WGPUInstance wgpuCreateInstance(const WGPUInstanceDescriptor* descriptor)
{
    return WebGPU::releaseToAPI(WebGPU::Instance::create(*descriptor));
}

WGPUProc wgpuGetProcAddress(WGPUDevice, const char*)
{
    return nullptr;
}

WGPUSurface wgpuInstanceCreateSurface(WGPUInstance instance, const WGPUSurfaceDescriptor* descriptor)
{
    return WebGPU::releaseToAPI(WebGPU::fromAPI(instance).createSurface(*descriptor));
}

void wgpuInstanceProcessEvents(WGPUInstance instance)
{
    WebGPU::fromAPI(instance).processEvents();
}

void wgpuInstanceLoseAllDevices(WGPUInstance instance)
{
    WebGPU::fromAPI(instance).loseAllDevices();
}

void wgpuInstanceRequestAdapter(WGPUInstance instance, const WGPURequestAdapterOptions* options, WGPURequestAdapterCallback callback, void* userdata)
{
    WebGPU::fromAPI(instance).requestAdapter(*options, [callback, userdata](WGPURequestAdapterStatus status, Ref<WebGPU::Adapter>&& adapter, String&& message) {
        if (status != WGPURequestAdapterStatus_Success) {
            callback(status, nullptr, message.utf8().data(), userdata);
            return;
        }

        callback(status, WebGPU::releaseToAPI(WTFMove(adapter)), message.utf8().data(), userdata);
    });
}

void wgpuInstanceRequestAdapterWithBlock(WGPUInstance instance, WGPURequestAdapterOptions const * options, WGPURequestAdapterBlockCallback callback)
{
    WebGPU::fromAPI(instance).requestAdapter(*options, [callback = WebGPU::fromAPI(WTFMove(callback))](WGPURequestAdapterStatus status, Ref<WebGPU::Adapter>&& adapter, String&& message) {
        if (status != WGPURequestAdapterStatus_Success) {
            callback(status, nullptr, message.utf8().data());
            return;
        }

        callback(status, WebGPU::releaseToAPI(WTFMove(adapter)), message.utf8().data());
    });
}

// Fuzzer things
WGPUBool wgpuBufferIsValid(WGPUBuffer buffer)
{
    return WebGPU::fromAPI(buffer).isValid();
}

WGPUBool wgpuAdapterIsValid(WGPUAdapter adapter)
{
    return WebGPU::fromAPI(adapter).isValid();
}

WGPUBool wgpuBindGroupIsValid(WGPUBindGroup bindGroup)
{
    return WebGPU::fromAPI(bindGroup).isValid();
}

WGPUBool wgpuBindGroupLayoutIsValid(WGPUBindGroupLayout bindGroupLayout)
{
    return WebGPU::fromAPI(bindGroupLayout).isValid();
}

WGPUBool wgpuCommandBufferIsValid(WGPUCommandBuffer commandBuffer)
{
    return WebGPU::fromAPI(commandBuffer).isValid();
}

WGPUBool wgpuCommandEncoderIsValid(WGPUCommandEncoder commandEncoder)
{
    return WebGPU::fromAPI(commandEncoder).isValid();
}

WGPUBool wgpuComputePassEncoderIsValid(WGPUComputePassEncoder computePassEncoder)
{
    return WebGPU::fromAPI(computePassEncoder).isValid();
}

WGPUBool wgpuComputePipelineIsValid(WGPUComputePipeline computePipeline)
{
    return WebGPU::fromAPI(computePipeline).isValid();
}

WGPUBool wgpuDeviceIsValid(WGPUDevice device)
{
    return WebGPU::fromAPI(device).isValid();
}

WGPUBool wgpuExternalTextureIsValid(WGPUExternalTexture externalTexture)
{
    return WebGPU::fromAPI(externalTexture).isValid();
}

WGPUBool wgpuPipelineLayoutIsValid(WGPUPipelineLayout pipelineLayout)
{
    return WebGPU::fromAPI(pipelineLayout).isValid();
}

WGPUBool wgpuPresentationContextIsValid(WGPUSurface presentationContext)
{
    return WebGPU::fromAPI(presentationContext).isValid();
}

WGPUBool wgpuQuerySetIsValid(WGPUQuerySet querySet)
{
    return WebGPU::fromAPI(querySet).isValid();
}

WGPUBool wgpuQueueIsValid(WGPUQueue queue)
{
    return WebGPU::fromAPI(queue).isValid();
}

WGPUBool wgpuRenderBundleEncoderIsValid(WGPURenderBundleEncoder renderBundleEncoder)
{
    return WebGPU::fromAPI(renderBundleEncoder).isValid();
}

WGPUBool wgpuRenderBundleIsValid(WGPURenderBundle renderBundle)
{
    return WebGPU::fromAPI(renderBundle).isValid();
}

WGPUBool wgpuRenderPassEncoderIsValid(WGPURenderPassEncoder renderPassEncoder)
{
    return WebGPU::fromAPI(renderPassEncoder).isValid();
}

WGPUBool wgpuRenderPipelineIsValid(WGPURenderPipeline renderPipeline)
{
    return WebGPU::fromAPI(renderPipeline).isValid();
}

WGPUBool wgpuSamplerIsValid(WGPUSampler sampler)
{
    return WebGPU::fromAPI(sampler).isValid();
}

WGPUBool wgpuShaderModuleIsValid(WGPUShaderModule shaderModule)
{
    return WebGPU::fromAPI(shaderModule).isValid();
}

WGPUBool wgpuTextureIsValid(WGPUTexture texture)
{
    return WebGPU::fromAPI(texture).isValid();
}

WGPUBool wgpuTextureViewIsValid(WGPUTextureView textureView)
{
    return WebGPU::fromAPI(textureView).isValid();
}

WGPUBool wgpuXRBindingIsValid(WGPUXRBinding binding)
{
    return WebGPU::fromAPI(binding).isValid();
}

WGPUBool wgpuXRSubImageIsValid(WGPUXRSubImage subImage)
{
    return WebGPU::fromAPI(subImage).isValid();
}

WGPUBool wgpuXRProjectionLayerIsValid(WGPUXRProjectionLayer layer)
{
    return WebGPU::fromAPI(layer).isValid();
}

WGPUBool wgpuXRViewIsValid(WGPUXRView view)
{
    return WebGPU::fromAPI(view).isValid();
}
