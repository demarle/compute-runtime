/*
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/command_container/command_encoder.h"
#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/device/device.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/blit_commands_helper.h"
#include "shared/source/helpers/heap_helper.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/helpers/register_offsets.h"
#include "shared/source/helpers/string.h"
#include "shared/source/helpers/surface_format_info.h"
#include "shared/source/indirect_heap/indirect_heap.h"
#include "shared/source/memory_manager/allocation_properties.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/source/memory_manager/memory_manager.h"

#include "level_zero/core/source/cmdlist/cmdlist_hw.h"
#include "level_zero/core/source/device/device_imp.h"
#include "level_zero/core/source/event/event.h"
#include "level_zero/core/source/image/image.h"
#include "level_zero/core/source/module/module.h"

#include "pipe_control_args.h"

#include <algorithm>

namespace L0 {

template <GFXCORE_FAMILY gfxCoreFamily>
struct EncodeStateBaseAddress;

inline ze_result_t parseErrorCode(NEO::ErrorCode returnValue) {
    switch (returnValue) {
    case NEO::ErrorCode::OUT_OF_DEVICE_MEMORY:
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    default:
        return ZE_RESULT_SUCCESS;
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::programThreadArbitrationPolicy(Device *device) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    auto &hwHelper = NEO::HwHelper::get(device->getNEODevice()->getHardwareInfo().platform.eRenderCoreFamily);
    uint32_t threadArbitrationPolicy = hwHelper.getDefaultThreadArbitrationPolicy();
    if (NEO::DebugManager.flags.OverrideThreadArbitrationPolicy.get() != -1) {
        threadArbitrationPolicy = static_cast<uint32_t>(NEO::DebugManager.flags.OverrideThreadArbitrationPolicy.get());
    }
    NEO::PreambleHelper<GfxFamily>::programThreadArbitration(commandContainer.getCommandStream(), threadArbitrationPolicy);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::reset() {
    printfFunctionContainer.clear();
    removeDeallocationContainerData();
    removeHostPtrAllocations();
    commandContainer.reset();
    containsStatelessUncachedResource = false;
    indirectAllocationsAllowed = false;
    unifiedMemoryControls.indirectHostAllocationsAllowed = false;
    unifiedMemoryControls.indirectSharedAllocationsAllowed = false;
    commandListPreemptionMode = device->getDevicePreemptionMode();
    commandListPerThreadScratchSize = 0u;

    if (!isCopyOnly()) {
        if (!NEO::ApiSpecificConfig::getBindlessConfiguration()) {
            programStateBaseAddress(commandContainer, false);
        }
        commandContainer.setDirtyStateForAllHeaps(false);
        programThreadArbitrationPolicy(device);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::initialize(Device *device, NEO::EngineGroupType engineGroupType) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    this->device = device;
    this->commandListPreemptionMode = device->getDevicePreemptionMode();
    this->engineGroupType = engineGroupType;

    commandContainer.setReservedSshSize(getReserveSshSize());
    auto returnValue = commandContainer.initialize(static_cast<DeviceImp *>(device)->neoDevice);
    ze_result_t returnType = parseErrorCode(returnValue);
    if (returnType == ZE_RESULT_SUCCESS) {
        if (!isCopyOnly()) {
            if (!NEO::ApiSpecificConfig::getBindlessConfiguration()) {
                programStateBaseAddress(commandContainer, false);
            }
            commandContainer.setDirtyStateForAllHeaps(false);
            programThreadArbitrationPolicy(device);
        }
    }

    return returnType;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::executeCommandListImmediate(bool performMigration) {
    this->close();
    ze_command_list_handle_t immediateHandle = this->toHandle();
    this->cmdQImmediate->executeCommandLists(1, &immediateHandle, nullptr, performMigration);
    this->cmdQImmediate->synchronize(std::numeric_limits<uint64_t>::max());
    this->reset();

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::close() {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;

    commandContainer.removeDuplicatesFromResidencyContainer();
    NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferEnd(commandContainer);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::programL3(bool isSLMused) {}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(ze_kernel_handle_t hKernel,
                                                                     const ze_group_count_t *pThreadGroupDimensions,
                                                                     ze_event_handle_t hEvent,
                                                                     uint32_t numWaitEvents,
                                                                     ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (ret) {
        return ret;
    }

    return appendLaunchKernelWithParams(hKernel, pThreadGroupDimensions,
                                        hEvent, false, false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchCooperativeKernel(ze_kernel_handle_t hKernel,
                                                                                const ze_group_count_t *pLaunchFuncArgs,
                                                                                ze_event_handle_t hSignalEvent,
                                                                                uint32_t numWaitEvents,
                                                                                ze_event_handle_t *phWaitEvents) {

    return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelIndirect(ze_kernel_handle_t hKernel,
                                                                             const ze_group_count_t *pDispatchArgumentsBuffer,
                                                                             ze_event_handle_t hEvent,
                                                                             uint32_t numWaitEvents,
                                                                             ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (ret) {
        return ret;
    }
    appendEventForProfiling(hEvent, true);
    ret = appendLaunchKernelWithParams(hKernel, pDispatchArgumentsBuffer,
                                       nullptr, true, false);
    appendSignalEventPostWalker(hEvent);

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchMultipleKernelsIndirect(uint32_t numKernels,
                                                                                      const ze_kernel_handle_t *phKernels,
                                                                                      const uint32_t *pNumLaunchArguments,
                                                                                      const ze_group_count_t *pLaunchArgumentsBuffer,
                                                                                      ze_event_handle_t hEvent,
                                                                                      uint32_t numWaitEvents,
                                                                                      ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (ret) {
        return ret;
    }
    appendEventForProfiling(hEvent, true);
    const bool haveLaunchArguments = pLaunchArgumentsBuffer != nullptr;
    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(pNumLaunchArguments);
    auto alloc = allocData->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
    commandContainer.addToResidencyContainer(alloc);

    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    for (uint32_t i = 0; i < numKernels; i++) {
        NEO::EncodeMathMMIO<GfxFamily>::encodeGreaterThanPredicate(commandContainer, alloc->getGpuAddress(), i);

        ret = appendLaunchKernelWithParams(phKernels[i],
                                           haveLaunchArguments ? &pLaunchArgumentsBuffer[i] : nullptr,
                                           nullptr, true, true);
        if (ret) {
            return ret;
        }
    }

    appendSignalEventPostWalker(hEvent);

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendEventReset(ze_event_handle_t hEvent) {
    using POST_SYNC_OPERATION = typename GfxFamily::PIPE_CONTROL::POST_SYNC_OPERATION;
    auto event = Event::fromHandle(hEvent);

    uint64_t baseAddr = event->getGpuAddress();
    size_t eventOffset = 0;
    if (event->isTimestampEvent) {
        eventOffset = offsetof(TimestampPacketStorage::Packet, contextEnd);
        event->resetPackets();
    }
    commandContainer.addToResidencyContainer(&event->getAllocation());
    if (isCopyOnly()) {
        NEO::EncodeMiFlushDW<GfxFamily>::programMiFlushDw(*commandContainer.getCommandStream(), event->getGpuAddress(), Event::STATE_CLEARED, false, true);
    } else {
        NEO::PipeControlArgs args;
        args.dcFlushEnable = (!event->signalScope) ? false : true;
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControlAndProgramPostSyncOperation(
            *commandContainer.getCommandStream(),
            POST_SYNC_OPERATION::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA,
            ptrOffset(baseAddr, eventOffset),
            Event::STATE_CLEARED,
            commandContainer.getDevice()->getHardwareInfo(),
            args);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendBarrier(ze_event_handle_t hSignalEvent,
                                                                uint32_t numWaitEvents,
                                                                ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (ret) {
        return ret;
    }
    appendEventForProfiling(hSignalEvent, true);

    if (!hSignalEvent) {
        if (isCopyOnly()) {
            NEO::EncodeMiFlushDW<GfxFamily>::programMiFlushDw(*commandContainer.getCommandStream(), 0, 0, false, false);
        } else {
            NEO::PipeControlArgs args;
            NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
        }
    } else {
        appendSignalEventPostWalker(hSignalEvent);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryRangesBarrier(uint32_t numRanges,
                                                                            const size_t *pRangeSizes,
                                                                            const void **pRanges,
                                                                            ze_event_handle_t hSignalEvent,
                                                                            uint32_t numWaitEvents,
                                                                            ze_event_handle_t *phWaitEvents) {

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (ret) {
        return ret;
    }

    appendEventForProfiling(hSignalEvent, true);
    applyMemoryRangesBarrier(numRanges, pRangeSizes, pRanges);
    appendSignalEventPostWalker(hSignalEvent);

    if (this->cmdListType == CommandListType::TYPE_IMMEDIATE) {
        executeCommandListImmediate(true);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopyFromMemory(ze_image_handle_t hDstImage,
                                                                            const void *srcPtr,
                                                                            const ze_image_region_t *pDstRegion,
                                                                            ze_event_handle_t hEvent,
                                                                            uint32_t numWaitEvents,
                                                                            ze_event_handle_t *phWaitEvents) {

    auto image = Image::fromHandle(hDstImage);
    auto bytesPerPixel = static_cast<uint32_t>(image->getImageInfo().surfaceFormat->ImageElementSizeInBytes);

    Vec3<uint32_t> imgSize = {static_cast<uint32_t>(image->getImageDesc().width),
                              static_cast<uint32_t>(image->getImageDesc().height),
                              static_cast<uint32_t>(image->getImageDesc().depth)};

    ze_image_region_t tmpRegion;
    if (pDstRegion == nullptr) {
        // If this is a 1D or 2D image, then the height or depth is ignored and must be set to 1.
        // Internally, all dimensions must be >= 1.
        if (image->getImageDesc().type == ZE_IMAGE_TYPE_1D || image->getImageDesc().type == ZE_IMAGE_TYPE_1DARRAY) {
            imgSize.y = 1;
        }
        if (image->getImageDesc().type != ZE_IMAGE_TYPE_3D) {
            imgSize.z = 1;
        }
        tmpRegion = {0,
                     0,
                     0,
                     imgSize.x,
                     imgSize.y,
                     imgSize.z};
        pDstRegion = &tmpRegion;
    }

    uint64_t bufferSize = getInputBufferSize(image->getImageInfo().imgDesc.imageType, bytesPerPixel, pDstRegion);

    auto allocationStruct = getAlignedAllocation(this->device, srcPtr, bufferSize);

    auto rowPitch = pDstRegion->width * bytesPerPixel;
    auto slicePitch =
        image->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : pDstRegion->height * rowPitch;

    if (isCopyOnly()) {
        return appendCopyImageBlit(allocationStruct.alloc, image->getAllocation(),
                                   {0, 0, 0}, {pDstRegion->originX, pDstRegion->originY, pDstRegion->originZ}, rowPitch, slicePitch,
                                   rowPitch, slicePitch, bytesPerPixel, {pDstRegion->width, pDstRegion->height, pDstRegion->depth}, {pDstRegion->width, pDstRegion->height, pDstRegion->depth}, imgSize, hEvent);
    }

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    Kernel *builtinKernel = nullptr;

    switch (bytesPerPixel) {
    default:
        UNRECOVERABLE_IF(true);
    case 1u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3dBytes);
        break;
    case 2u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d2Bytes);
        break;
    case 4u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d4Bytes);
        break;
    case 8u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d8Bytes);
        break;
    case 16u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyBufferToImage3d16Bytes);
        break;
    }

    builtinKernel->setArgBufferWithAlloc(0u, allocationStruct.alignedAllocationPtr,
                                         allocationStruct.alloc);
    builtinKernel->setArgRedescribedImage(1u, hDstImage);
    builtinKernel->setArgumentValue(2u, sizeof(size_t), &allocationStruct.offset);

    uint32_t origin[] = {
        static_cast<uint32_t>(pDstRegion->originX),
        static_cast<uint32_t>(pDstRegion->originY),
        static_cast<uint32_t>(pDstRegion->originZ),
        0};
    builtinKernel->setArgumentValue(3u, sizeof(origin), &origin);

    uint32_t pitch[] = {
        rowPitch,
        slicePitch};
    builtinKernel->setArgumentValue(4u, sizeof(pitch), &pitch);

    uint32_t groupSizeX = pDstRegion->width;
    uint32_t groupSizeY = pDstRegion->height;
    uint32_t groupSizeZ = pDstRegion->depth;

    if (builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ,
                                        &groupSizeX, &groupSizeY, &groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (pDstRegion->width % groupSizeX || pDstRegion->height % groupSizeY || pDstRegion->depth % groupSizeZ) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t functionArgs{pDstRegion->width / groupSizeX, pDstRegion->height / groupSizeY,
                                  pDstRegion->depth / groupSizeZ};

    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinKernel->toHandle(), &functionArgs,
                                                                    hEvent, numWaitEvents, phWaitEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopyToMemory(void *dstPtr,
                                                                          ze_image_handle_t hSrcImage,
                                                                          const ze_image_region_t *pSrcRegion,
                                                                          ze_event_handle_t hEvent,
                                                                          uint32_t numWaitEvents,
                                                                          ze_event_handle_t *phWaitEvents) {

    auto image = Image::fromHandle(hSrcImage);
    auto bytesPerPixel = static_cast<uint32_t>(image->getImageInfo().surfaceFormat->ImageElementSizeInBytes);

    Vec3<uint32_t> imgSize = {static_cast<uint32_t>(image->getImageDesc().width),
                              static_cast<uint32_t>(image->getImageDesc().height),
                              static_cast<uint32_t>(image->getImageDesc().depth)};

    ze_image_region_t tmpRegion;
    if (pSrcRegion == nullptr) {
        // If this is a 1D or 2D image, then the height or depth is ignored and must be set to 1.
        // Internally, all dimensions must be >= 1.
        if (image->getImageDesc().type == ZE_IMAGE_TYPE_1D || image->getImageDesc().type == ZE_IMAGE_TYPE_1DARRAY) {
            imgSize.y = 1;
        }
        if (image->getImageDesc().type != ZE_IMAGE_TYPE_3D) {
            imgSize.z = 1;
        }
        tmpRegion = {0,
                     0,
                     0,
                     imgSize.x,
                     imgSize.y,
                     imgSize.z};
        pSrcRegion = &tmpRegion;
    }

    uint64_t bufferSize = getInputBufferSize(image->getImageInfo().imgDesc.imageType, bytesPerPixel, pSrcRegion);

    auto allocationStruct = getAlignedAllocation(this->device, dstPtr, bufferSize);

    auto rowPitch = pSrcRegion->width * bytesPerPixel;
    auto slicePitch =
        (image->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : pSrcRegion->height) * rowPitch;

    if (isCopyOnly()) {
        return appendCopyImageBlit(image->getAllocation(), allocationStruct.alloc,
                                   {pSrcRegion->originX, pSrcRegion->originY, pSrcRegion->originZ}, {0, 0, 0}, rowPitch, slicePitch,
                                   rowPitch, slicePitch, bytesPerPixel, {pSrcRegion->width, pSrcRegion->height, pSrcRegion->depth}, imgSize, {pSrcRegion->width, pSrcRegion->height, pSrcRegion->depth}, hEvent);
    }

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    Kernel *builtinKernel = nullptr;

    switch (bytesPerPixel) {
    default:
        UNRECOVERABLE_IF(true);
    case 1u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBufferBytes);
        break;
    case 2u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer2Bytes);
        break;
    case 4u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer4Bytes);
        break;
    case 8u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer8Bytes);
        break;
    case 16u:
        builtinKernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImage3dToBuffer16Bytes);
        break;
    }

    builtinKernel->setArgRedescribedImage(0u, hSrcImage);
    builtinKernel->setArgBufferWithAlloc(1u, allocationStruct.alignedAllocationPtr,
                                         allocationStruct.alloc);

    uint32_t origin[] = {
        static_cast<uint32_t>(pSrcRegion->originX),
        static_cast<uint32_t>(pSrcRegion->originY),
        static_cast<uint32_t>(pSrcRegion->originZ),
        0};
    builtinKernel->setArgumentValue(2u, sizeof(origin), &origin);

    builtinKernel->setArgumentValue(3u, sizeof(size_t), &allocationStruct.offset);

    uint32_t pitch[] = {
        rowPitch,
        slicePitch};
    builtinKernel->setArgumentValue(4u, sizeof(pitch), &pitch);

    uint32_t groupSizeX = pSrcRegion->width;
    uint32_t groupSizeY = pSrcRegion->height;
    uint32_t groupSizeZ = pSrcRegion->depth;

    if (builtinKernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ,
                                        &groupSizeX, &groupSizeY, &groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (builtinKernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (pSrcRegion->width % groupSizeX || pSrcRegion->height % groupSizeY || pSrcRegion->depth % groupSizeZ) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t functionArgs{pSrcRegion->width / groupSizeX, pSrcRegion->height / groupSizeY,
                                  pSrcRegion->depth / groupSizeZ};

    auto ret = CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinKernel->toHandle(), &functionArgs,
                                                                        hEvent, numWaitEvents, phWaitEvents);

    auto event = Event::fromHandle(hEvent);
    if (event) {
        allocationStruct.needsFlush &= !event->signalScope;
    }

    if (allocationStruct.needsFlush) {
        NEO::PipeControlArgs args(true);
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
    }

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopyRegion(ze_image_handle_t hDstImage,
                                                                        ze_image_handle_t hSrcImage,
                                                                        const ze_image_region_t *pDstRegion,
                                                                        const ze_image_region_t *pSrcRegion,
                                                                        ze_event_handle_t hEvent,
                                                                        uint32_t numWaitEvents,
                                                                        ze_event_handle_t *phWaitEvents) {
    auto dstImage = L0::Image::fromHandle(hDstImage);
    auto srcImage = L0::Image::fromHandle(hSrcImage);
    cl_int4 srcOffset, dstOffset;

    ze_image_region_t srcRegion, dstRegion;

    if (pSrcRegion != nullptr) {
        srcRegion = *pSrcRegion;
    } else {
        ze_image_desc_t srcDesc = srcImage->getImageDesc();
        srcRegion = {0, 0, 0, static_cast<uint32_t>(srcDesc.width), srcDesc.height, srcDesc.depth};
    }

    srcOffset.x = static_cast<cl_int>(srcRegion.originX);
    srcOffset.y = static_cast<cl_int>(srcRegion.originY);
    srcOffset.z = static_cast<cl_int>(srcRegion.originZ);
    srcOffset.w = 0;

    if (pDstRegion != nullptr) {
        dstRegion = *pDstRegion;
    } else {
        ze_image_desc_t dstDesc = dstImage->getImageDesc();
        dstRegion = {0, 0, 0, static_cast<uint32_t>(dstDesc.width), dstDesc.height, dstDesc.depth};
    }

    dstOffset.x = static_cast<cl_int>(dstRegion.originX);
    dstOffset.y = static_cast<cl_int>(dstRegion.originY);
    dstOffset.z = static_cast<cl_int>(dstRegion.originZ);
    dstOffset.w = 0;

    if (srcRegion.width != dstRegion.width ||
        srcRegion.height != dstRegion.height ||
        srcRegion.depth != dstRegion.depth) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    uint32_t groupSizeX = srcRegion.width;
    uint32_t groupSizeY = srcRegion.height;
    uint32_t groupSizeZ = srcRegion.depth;

    if (isCopyOnly()) {
        auto bytesPerPixel = static_cast<uint32_t>(srcImage->getImageInfo().surfaceFormat->ImageElementSizeInBytes);

        Vec3<uint32_t> srcImgSize = {static_cast<uint32_t>(srcImage->getImageInfo().imgDesc.imageWidth),
                                     static_cast<uint32_t>(srcImage->getImageInfo().imgDesc.imageHeight),
                                     static_cast<uint32_t>(srcImage->getImageInfo().imgDesc.imageDepth)};

        Vec3<uint32_t> dstImgSize = {static_cast<uint32_t>(dstImage->getImageInfo().imgDesc.imageWidth),
                                     static_cast<uint32_t>(dstImage->getImageInfo().imgDesc.imageHeight),
                                     static_cast<uint32_t>(dstImage->getImageInfo().imgDesc.imageDepth)};

        auto srcRowPitch = srcRegion.width * bytesPerPixel;
        auto srcSlicePitch =
            (srcImage->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : srcRegion.height) * srcRowPitch;

        auto dstRowPitch = dstRegion.width * bytesPerPixel;
        auto dstSlicePitch =
            (dstImage->getImageInfo().imgDesc.imageType == NEO::ImageType::Image1DArray ? 1 : dstRegion.height) * dstRowPitch;

        return appendCopyImageBlit(srcImage->getAllocation(), dstImage->getAllocation(),
                                   {srcRegion.originX, srcRegion.originY, srcRegion.originZ}, {dstRegion.originX, dstRegion.originY, dstRegion.originZ}, srcRowPitch, srcSlicePitch,
                                   dstRowPitch, dstSlicePitch, bytesPerPixel, {srcRegion.width, srcRegion.height, srcRegion.depth}, srcImgSize, dstImgSize, hEvent);
    }

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    auto kernel = device->getBuiltinFunctionsLib()->getImageFunction(ImageBuiltin::CopyImageRegion);

    if (kernel->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX,
                                 &groupSizeY, &groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (kernel->setGroupSize(groupSizeX, groupSizeY, groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (srcRegion.width % groupSizeX || srcRegion.height % groupSizeY || srcRegion.depth % groupSizeZ) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t functionArgs{srcRegion.width / groupSizeX, srcRegion.height / groupSizeY,
                                  srcRegion.depth / groupSizeZ};

    kernel->setArgRedescribedImage(0, hSrcImage);
    kernel->setArgRedescribedImage(1, hDstImage);
    kernel->setArgumentValue(2, sizeof(srcOffset), &srcOffset);
    kernel->setArgumentValue(3, sizeof(dstOffset), &dstOffset);

    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(kernel->toHandle(), &functionArgs,
                                                                    hEvent, numWaitEvents, phWaitEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendImageCopy(ze_image_handle_t hDstImage,
                                                                  ze_image_handle_t hSrcImage,
                                                                  ze_event_handle_t hEvent,
                                                                  uint32_t numWaitEvents,
                                                                  ze_event_handle_t *phWaitEvents) {

    return this->appendImageCopyRegion(hDstImage, hSrcImage, nullptr, nullptr, hEvent,
                                       numWaitEvents, phWaitEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemAdvise(ze_device_handle_t hDevice,
                                                                  const void *ptr, size_t size,
                                                                  ze_memory_advice_t advice) {

    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(ptr);
    if (allocData) {
        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_UNKNOWN;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelSplit(ze_kernel_handle_t hKernel,
                                                                          const ze_group_count_t *pThreadGroupDimensions,
                                                                          ze_event_handle_t hEvent) {
    return appendLaunchKernelWithParams(hKernel, pThreadGroupDimensions, nullptr, false, false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendEventForProfilingAllWalkers(ze_event_handle_t hEvent, bool beforeWalker) {
    if (beforeWalker) {
        appendEventForProfiling(hEvent, true);
    } else {
        appendSignalEventPostWalker(hEvent);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyKernelWithGA(void *dstPtr,
                                                                               NEO::GraphicsAllocation *dstPtrAlloc,
                                                                               uint64_t dstOffset,
                                                                               void *srcPtr,
                                                                               NEO::GraphicsAllocation *srcPtrAlloc,
                                                                               uint64_t srcOffset,
                                                                               uint32_t size,
                                                                               uint32_t elementSize,
                                                                               Builtin builtin,
                                                                               ze_event_handle_t hSignalEvent) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    auto builtinFunction = device->getBuiltinFunctionsLib()->getFunction(builtin);

    uint32_t groupSizeX = builtinFunction->getImmutableData()
                              ->getDescriptor()
                              .kernelAttributes.simdSize;
    uint32_t groupSizeY = 1u;
    uint32_t groupSizeZ = 1u;

    if (builtinFunction->setGroupSize(groupSizeX, groupSizeY, groupSizeZ)) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    builtinFunction->setArgBufferWithAlloc(0u, *reinterpret_cast<uintptr_t *>(dstPtr), dstPtrAlloc);
    builtinFunction->setArgBufferWithAlloc(1u, *reinterpret_cast<uintptr_t *>(srcPtr), srcPtrAlloc);

    uint32_t elems = size / elementSize;
    builtinFunction->setArgumentValue(2, sizeof(elems), &elems);
    builtinFunction->setArgumentValue(3, sizeof(dstOffset), &dstOffset);
    builtinFunction->setArgumentValue(4, sizeof(srcOffset), &srcOffset);

    uint32_t groups = (size + ((groupSizeX * elementSize) - 1)) / (groupSizeX * elementSize);
    ze_group_count_t dispatchFuncArgs{groups, 1u, 1u};

    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernelSplit(builtinFunction->toHandle(), &dispatchFuncArgs, hSignalEvent);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyBlit(uintptr_t dstPtr,
                                                                       NEO::GraphicsAllocation *dstPtrAlloc,
                                                                       uint64_t dstOffset, uintptr_t srcPtr,
                                                                       NEO::GraphicsAllocation *srcPtrAlloc,
                                                                       uint64_t srcOffset,
                                                                       uint32_t size) {
    dstOffset += ptrDiff<uintptr_t>(dstPtr, dstPtrAlloc->getGpuAddress());
    srcOffset += ptrDiff<uintptr_t>(srcPtr, srcPtrAlloc->getGpuAddress());

    auto clearColorAllocation = device->getNEODevice()->getDefaultEngine().commandStreamReceiver->getClearColorAllocation();

    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    auto blitProperties = NEO::BlitProperties::constructPropertiesForCopyBuffer(dstPtrAlloc, srcPtrAlloc, {dstOffset, 0, 0}, {srcOffset, 0, 0}, {size, 0, 0}, 0, 0, 0, 0, clearColorAllocation);
    commandContainer.addToResidencyContainer(dstPtrAlloc);
    commandContainer.addToResidencyContainer(srcPtrAlloc);
    commandContainer.addToResidencyContainer(clearColorAllocation);

    NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBufferPerRow(blitProperties, *commandContainer.getCommandStream(), *device->getNEODevice()->getExecutionEnvironment()->rootDeviceEnvironments[device->getRootDeviceIndex()]);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyBlitRegion(NEO::GraphicsAllocation *srcAlloc,
                                                                             NEO::GraphicsAllocation *dstAlloc,
                                                                             size_t srcOffset,
                                                                             size_t dstOffset,
                                                                             ze_copy_region_t srcRegion,
                                                                             ze_copy_region_t dstRegion, Vec3<size_t> copySize,
                                                                             size_t srcRowPitch, size_t srcSlicePitch,
                                                                             size_t dstRowPitch, size_t dstSlicePitch,
                                                                             Vec3<uint32_t> srcSize, Vec3<uint32_t> dstSize, ze_event_handle_t hSignalEvent,
                                                                             uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;

    dstRegion.originX += static_cast<uint32_t>(dstOffset);
    srcRegion.originX += static_cast<uint32_t>(srcOffset);
    uint32_t bytesPerPixel = NEO::BlitCommandsHelper<GfxFamily>::getAvailableBytesPerPixel(copySize.x, srcRegion.originX, dstRegion.originX, srcSize.x, dstSize.x);
    bool copyOneCommand = NEO::BlitCommandsHelper<GfxFamily>::useOneBlitCopyCommand(copySize, bytesPerPixel);
    Vec3<size_t> srcPtrOffset = {(copyOneCommand ? (srcRegion.originX / bytesPerPixel) : srcRegion.originX), srcRegion.originY, srcRegion.originZ};
    Vec3<size_t> dstPtrOffset = {(copyOneCommand ? (dstRegion.originX / bytesPerPixel) : dstRegion.originX), dstRegion.originY, dstRegion.originZ};
    copySize.x = copyOneCommand ? copySize.x / bytesPerPixel : copySize.x;

    auto clearColorAllocation = device->getNEODevice()->getDefaultEngine().commandStreamReceiver->getClearColorAllocation();

    auto blitProperties = NEO::BlitProperties::constructPropertiesForCopyBuffer(dstAlloc, srcAlloc,
                                                                                dstPtrOffset, srcPtrOffset, copySize, srcRowPitch, srcSlicePitch,
                                                                                dstRowPitch, dstSlicePitch, clearColorAllocation);
    commandContainer.addToResidencyContainer(dstAlloc);
    commandContainer.addToResidencyContainer(srcAlloc);
    commandContainer.addToResidencyContainer(clearColorAllocation);
    blitProperties.bytesPerPixel = bytesPerPixel;
    blitProperties.srcSize = srcSize;
    blitProperties.dstSize = dstSize;

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (ret) {
        return ret;
    }

    appendEventForProfiling(hSignalEvent, true);
    if (copyOneCommand) {
        NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsRegion(blitProperties, *commandContainer.getCommandStream(), *device->getNEODevice()->getExecutionEnvironment()->rootDeviceEnvironments[device->getRootDeviceIndex()]);
    } else {
        NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBufferPerRow(blitProperties, *commandContainer.getCommandStream(), *device->getNEODevice()->getExecutionEnvironment()->rootDeviceEnvironments[device->getRootDeviceIndex()]);
    }
    appendSignalEventPostWalker(hSignalEvent);
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendCopyImageBlit(NEO::GraphicsAllocation *src,
                                                                      NEO::GraphicsAllocation *dst,
                                                                      Vec3<size_t> srcOffsets, Vec3<size_t> dstOffsets,
                                                                      size_t srcRowPitch, size_t srcSlicePitch,
                                                                      size_t dstRowPitch, size_t dstSlicePitch,
                                                                      size_t bytesPerPixel, Vec3<size_t> copySize,
                                                                      Vec3<uint32_t> srcSize, Vec3<uint32_t> dstSize, ze_event_handle_t hSignalEvent) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;

    auto clearColorAllocation = device->getNEODevice()->getDefaultEngine().commandStreamReceiver->getClearColorAllocation();

    auto blitProperties = NEO::BlitProperties::constructPropertiesForCopyBuffer(dst, src,
                                                                                dstOffsets, srcOffsets, copySize, srcRowPitch, srcSlicePitch,
                                                                                dstRowPitch, dstSlicePitch, clearColorAllocation);
    blitProperties.bytesPerPixel = bytesPerPixel;
    blitProperties.srcSize = srcSize;
    blitProperties.dstSize = dstSize;
    commandContainer.addToResidencyContainer(dst);
    commandContainer.addToResidencyContainer(src);
    commandContainer.addToResidencyContainer(clearColorAllocation);
    appendEventForProfiling(hSignalEvent, true);
    NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsRegion(blitProperties, *commandContainer.getCommandStream(), *device->getNEODevice()->getExecutionEnvironment()->rootDeviceEnvironments[device->getRootDeviceIndex()]);
    appendSignalEventPostWalker(hSignalEvent);
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendPageFaultCopy(NEO::GraphicsAllocation *dstptr,
                                                                      NEO::GraphicsAllocation *srcptr,
                                                                      size_t size, bool flushHost) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    auto builtinFunction = device->getBuiltinFunctionsLib()->getPageFaultFunction();

    uint32_t groupSizeX = builtinFunction->getImmutableData()
                              ->getDescriptor()
                              .kernelAttributes.simdSize;
    uint32_t groupSizeY = 1u;
    uint32_t groupSizeZ = 1u;

    if (builtinFunction->setGroupSize(groupSizeX, groupSizeY, groupSizeZ)) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    auto dstValPtr = static_cast<uintptr_t>(dstptr->getGpuAddress());
    auto srcValPtr = static_cast<uintptr_t>(srcptr->getGpuAddress());

    builtinFunction->setArgBufferWithAlloc(0, dstValPtr, dstptr);
    builtinFunction->setArgBufferWithAlloc(1, srcValPtr, srcptr);
    builtinFunction->setArgumentValue(2, sizeof(size), &size);

    uint32_t groups = (static_cast<uint32_t>(size) + ((groupSizeX)-1)) / (groupSizeX);
    ze_group_count_t dispatchFuncArgs{groups, 1u, 1u};

    ze_result_t ret = appendLaunchKernelWithParams(builtinFunction->toHandle(), &dispatchFuncArgs,
                                                   nullptr, false, false);
    if (ret != ZE_RESULT_SUCCESS) {
        return ret;
    }

    if (flushHost) {
        NEO::PipeControlArgs args(true);
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
    }

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopy(void *dstptr,
                                                                   const void *srcptr,
                                                                   size_t size,
                                                                   ze_event_handle_t hSignalEvent,
                                                                   uint32_t numWaitEvents,
                                                                   ze_event_handle_t *phWaitEvents) {

    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    uintptr_t start = reinterpret_cast<uintptr_t>(dstptr);

    size_t middleAlignment = MemoryConstants::cacheLineSize;
    size_t middleElSize = sizeof(uint32_t) * 4;

    uintptr_t leftSize = start % middleAlignment;
    leftSize = (leftSize > 0) ? (middleAlignment - leftSize) : 0;
    leftSize = std::min(leftSize, size);

    uintptr_t rightSize = (start + size) % middleAlignment;
    rightSize = std::min(rightSize, size - leftSize);

    uintptr_t middleSizeBytes = size - leftSize - rightSize;

    if (!isAligned<4>(reinterpret_cast<uintptr_t>(srcptr) + leftSize)) {
        leftSize += middleSizeBytes;
        middleSizeBytes = 0;
    }

    DEBUG_BREAK_IF(size != leftSize + middleSizeBytes + rightSize);

    auto dstAllocationStruct = getAlignedAllocation(this->device, dstptr, size);
    auto srcAllocationStruct = getAlignedAllocation(this->device, srcptr, size);

    ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);

    if (ret) {
        return ret;
    }

    appendEventForProfilingAllWalkers(hSignalEvent, true);

    if (ret == ZE_RESULT_SUCCESS && leftSize) {
        ret = isCopyOnly() ? appendMemoryCopyBlit(dstAllocationStruct.alignedAllocationPtr,
                                                  dstAllocationStruct.alloc, dstAllocationStruct.offset,
                                                  srcAllocationStruct.alignedAllocationPtr,
                                                  srcAllocationStruct.alloc, srcAllocationStruct.offset, static_cast<uint32_t>(leftSize))
                           : appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAllocationStruct.alignedAllocationPtr),
                                                          dstAllocationStruct.alloc, dstAllocationStruct.offset,
                                                          reinterpret_cast<void *>(&srcAllocationStruct.alignedAllocationPtr),
                                                          srcAllocationStruct.alloc, srcAllocationStruct.offset,
                                                          static_cast<uint32_t>(leftSize), 1,
                                                          Builtin::CopyBufferToBufferSide,
                                                          hSignalEvent);
    }

    if (ret == ZE_RESULT_SUCCESS && middleSizeBytes) {
        ret = isCopyOnly() ? appendMemoryCopyBlit(dstAllocationStruct.alignedAllocationPtr,
                                                  dstAllocationStruct.alloc, leftSize + dstAllocationStruct.offset,
                                                  srcAllocationStruct.alignedAllocationPtr,
                                                  srcAllocationStruct.alloc, leftSize + srcAllocationStruct.offset, static_cast<uint32_t>(middleSizeBytes))
                           : appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAllocationStruct.alignedAllocationPtr),
                                                          dstAllocationStruct.alloc, leftSize + dstAllocationStruct.offset,
                                                          reinterpret_cast<void *>(&srcAllocationStruct.alignedAllocationPtr),
                                                          srcAllocationStruct.alloc, leftSize + srcAllocationStruct.offset,
                                                          static_cast<uint32_t>(middleSizeBytes),
                                                          static_cast<uint32_t>(middleElSize),
                                                          Builtin::CopyBufferToBufferMiddle,
                                                          hSignalEvent);
    }

    if (ret == ZE_RESULT_SUCCESS && rightSize) {
        ret = isCopyOnly() ? appendMemoryCopyBlit(dstAllocationStruct.alignedAllocationPtr,
                                                  dstAllocationStruct.alloc, leftSize + middleSizeBytes + dstAllocationStruct.offset,
                                                  srcAllocationStruct.alignedAllocationPtr,
                                                  srcAllocationStruct.alloc, leftSize + middleSizeBytes + srcAllocationStruct.offset, static_cast<uint32_t>(rightSize))
                           : appendMemoryCopyKernelWithGA(reinterpret_cast<void *>(&dstAllocationStruct.alignedAllocationPtr),
                                                          dstAllocationStruct.alloc, leftSize + middleSizeBytes + dstAllocationStruct.offset,
                                                          reinterpret_cast<void *>(&srcAllocationStruct.alignedAllocationPtr),
                                                          srcAllocationStruct.alloc, leftSize + middleSizeBytes + srcAllocationStruct.offset,
                                                          static_cast<uint32_t>(rightSize), 1u,
                                                          Builtin::CopyBufferToBufferSide,
                                                          hSignalEvent);
    }

    appendEventForProfilingAllWalkers(hSignalEvent, false);

    auto event = Event::fromHandle(hSignalEvent);
    if (event) {
        dstAllocationStruct.needsFlush &= !event->signalScope;
    }

    if (dstAllocationStruct.needsFlush && !isCopyOnly()) {
        NEO::PipeControlArgs args(true);
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
    }

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyRegion(void *dstPtr,
                                                                         const ze_copy_region_t *dstRegion,
                                                                         uint32_t dstPitch,
                                                                         uint32_t dstSlicePitch,
                                                                         const void *srcPtr,
                                                                         const ze_copy_region_t *srcRegion,
                                                                         uint32_t srcPitch,
                                                                         uint32_t srcSlicePitch,
                                                                         ze_event_handle_t hSignalEvent,
                                                                         uint32_t numWaitEvents,
                                                                         ze_event_handle_t *phWaitEvents) {

    size_t dstSize = 0;
    size_t srcSize = 0;

    if (srcRegion->depth > 1) {
        uint32_t hostPtrDstOffset = dstRegion->originX + ((dstRegion->originY) * dstPitch) + ((dstRegion->originZ) * dstSlicePitch);
        uint32_t hostPtrSrcOffset = srcRegion->originX + ((srcRegion->originY) * srcPitch) + ((srcRegion->originZ) * srcSlicePitch);
        dstSize = (dstRegion->width * dstRegion->height * dstRegion->depth) + hostPtrDstOffset;
        srcSize = (srcRegion->width * srcRegion->height * srcRegion->depth) + hostPtrSrcOffset;
    } else {
        uint32_t hostPtrDstOffset = dstRegion->originX + ((dstRegion->originY) * dstPitch);
        uint32_t hostPtrSrcOffset = srcRegion->originX + ((srcRegion->originY) * srcPitch);
        dstSize = (dstRegion->width * dstRegion->height) + hostPtrDstOffset;
        srcSize = (srcRegion->width * srcRegion->height) + hostPtrSrcOffset;
    }

    auto dstAllocationStruct = getAlignedAllocation(this->device, dstPtr, dstSize);
    auto srcAllocationStruct = getAlignedAllocation(this->device, srcPtr, srcSize);

    dstSize += dstAllocationStruct.offset;
    srcSize += srcAllocationStruct.offset;

    Vec3<uint32_t> srcSize3 = {srcPitch ? srcPitch : srcRegion->width + srcRegion->originX,
                               srcSlicePitch ? srcSlicePitch / srcPitch : srcRegion->height + srcRegion->originY,
                               srcRegion->depth + srcRegion->originZ};
    Vec3<uint32_t> dstSize3 = {dstPitch ? dstPitch : dstRegion->width + dstRegion->originX,
                               dstSlicePitch ? dstSlicePitch / dstPitch : dstRegion->height + dstRegion->originY,
                               dstRegion->depth + dstRegion->originZ};

    ze_result_t result = ZE_RESULT_SUCCESS;
    if (srcRegion->depth > 1) {
        result = isCopyOnly() ? appendMemoryCopyBlitRegion(srcAllocationStruct.alloc, dstAllocationStruct.alloc, srcAllocationStruct.offset, dstAllocationStruct.offset, *srcRegion, *dstRegion, {srcRegion->width, srcRegion->height, srcRegion->depth},
                                                           srcPitch, srcSlicePitch, dstPitch, dstSlicePitch, srcSize3, dstSize3, hSignalEvent, numWaitEvents, phWaitEvents)
                              : this->appendMemoryCopyKernel3d(&dstAllocationStruct, &srcAllocationStruct,
                                                               Builtin::CopyBufferRectBytes3d, dstRegion, dstPitch, dstSlicePitch, dstAllocationStruct.offset,
                                                               srcRegion, srcPitch, srcSlicePitch, srcAllocationStruct.offset, hSignalEvent, numWaitEvents, phWaitEvents);
    } else {
        result = isCopyOnly() ? appendMemoryCopyBlitRegion(srcAllocationStruct.alloc, dstAllocationStruct.alloc, srcAllocationStruct.offset, dstAllocationStruct.offset, *srcRegion, *dstRegion, {srcRegion->width, srcRegion->height, srcRegion->depth},
                                                           srcPitch, srcSlicePitch, dstPitch, dstSlicePitch, srcSize3, dstSize3, hSignalEvent, numWaitEvents, phWaitEvents)
                              : this->appendMemoryCopyKernel2d(&dstAllocationStruct, &srcAllocationStruct,
                                                               Builtin::CopyBufferRectBytes2d, dstRegion, dstPitch, dstAllocationStruct.offset,
                                                               srcRegion, srcPitch, srcAllocationStruct.offset, hSignalEvent, numWaitEvents, phWaitEvents);
    }

    if (result) {
        return result;
    }

    auto event = Event::fromHandle(hSignalEvent);
    if (event) {
        dstAllocationStruct.needsFlush &= !event->signalScope;
    }

    if (dstAllocationStruct.needsFlush && !isCopyOnly()) {
        NEO::PipeControlArgs args(true);
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyKernel3d(AlignedAllocationData *dstAlignedAllocation,
                                                                           AlignedAllocationData *srcAlignedAllocation,
                                                                           Builtin builtin,
                                                                           const ze_copy_region_t *dstRegion,
                                                                           uint32_t dstPitch,
                                                                           uint32_t dstSlicePitch,
                                                                           size_t dstOffset,
                                                                           const ze_copy_region_t *srcRegion,
                                                                           uint32_t srcPitch,
                                                                           uint32_t srcSlicePitch,
                                                                           size_t srcOffset,
                                                                           ze_event_handle_t hSignalEvent,
                                                                           uint32_t numWaitEvents,
                                                                           ze_event_handle_t *phWaitEvents) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    auto builtinFunction = device->getBuiltinFunctionsLib()->getFunction(builtin);

    uint32_t groupSizeX = srcRegion->width;
    uint32_t groupSizeY = srcRegion->height;
    uint32_t groupSizeZ = srcRegion->depth;

    if (builtinFunction->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ,
                                          &groupSizeX, &groupSizeY, &groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (builtinFunction->setGroupSize(groupSizeX, groupSizeY, groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (srcRegion->width % groupSizeX || srcRegion->height % groupSizeY || srcRegion->depth % groupSizeZ) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t dispatchFuncArgs{srcRegion->width / groupSizeX, srcRegion->height / groupSizeY,
                                      srcRegion->depth / groupSizeZ};

    uint32_t srcOrigin[3] = {(srcRegion->originX + static_cast<uint32_t>(srcOffset)), (srcRegion->originY), (srcRegion->originZ)};
    uint32_t dstOrigin[3] = {(dstRegion->originX + static_cast<uint32_t>(dstOffset)), (dstRegion->originY), (dstRegion->originZ)};
    uint32_t srcPitches[2] = {(srcPitch), (srcSlicePitch)};
    uint32_t dstPitches[2] = {(dstPitch), (dstSlicePitch)};

    builtinFunction->setArgBufferWithAlloc(0, srcAlignedAllocation->alignedAllocationPtr, srcAlignedAllocation->alloc);
    builtinFunction->setArgBufferWithAlloc(1, dstAlignedAllocation->alignedAllocationPtr, dstAlignedAllocation->alloc);
    builtinFunction->setArgumentValue(2, sizeof(srcOrigin), &srcOrigin);
    builtinFunction->setArgumentValue(3, sizeof(dstOrigin), &dstOrigin);
    builtinFunction->setArgumentValue(4, sizeof(srcPitches), &srcPitches);
    builtinFunction->setArgumentValue(5, sizeof(dstPitches), &dstPitches);

    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinFunction->toHandle(), &dispatchFuncArgs, hSignalEvent, numWaitEvents,
                                                                    phWaitEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyKernel2d(AlignedAllocationData *dstAlignedAllocation,
                                                                           AlignedAllocationData *srcAlignedAllocation,
                                                                           Builtin builtin,
                                                                           const ze_copy_region_t *dstRegion,
                                                                           uint32_t dstPitch,
                                                                           size_t dstOffset,
                                                                           const ze_copy_region_t *srcRegion,
                                                                           uint32_t srcPitch,
                                                                           size_t srcOffset,
                                                                           ze_event_handle_t hSignalEvent,
                                                                           uint32_t numWaitEvents,
                                                                           ze_event_handle_t *phWaitEvents) {

    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    auto builtinFunction = device->getBuiltinFunctionsLib()->getFunction(builtin);

    uint32_t groupSizeX = srcRegion->width;
    uint32_t groupSizeY = srcRegion->height;
    uint32_t groupSizeZ = 1u;

    if (builtinFunction->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX,
                                          &groupSizeY, &groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (builtinFunction->setGroupSize(groupSizeX, groupSizeY, groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (srcRegion->width % groupSizeX || srcRegion->height % groupSizeY) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t dispatchFuncArgs{srcRegion->width / groupSizeX, srcRegion->height / groupSizeY, 1u};

    uint32_t srcOrigin[2] = {(srcRegion->originX + static_cast<uint32_t>(srcOffset)), (srcRegion->originY)};
    uint32_t dstOrigin[2] = {(dstRegion->originX + static_cast<uint32_t>(dstOffset)), (dstRegion->originY)};

    builtinFunction->setArgBufferWithAlloc(0, srcAlignedAllocation->alignedAllocationPtr, srcAlignedAllocation->alloc);
    builtinFunction->setArgBufferWithAlloc(1, dstAlignedAllocation->alignedAllocationPtr, dstAlignedAllocation->alloc);
    builtinFunction->setArgumentValue(2, sizeof(srcOrigin), &srcOrigin);
    builtinFunction->setArgumentValue(3, sizeof(dstOrigin), &dstOrigin);
    builtinFunction->setArgumentValue(4, sizeof(srcPitch), &srcPitch);
    builtinFunction->setArgumentValue(5, sizeof(dstPitch), &dstPitch);

    return CommandListCoreFamily<gfxCoreFamily>::appendLaunchKernel(builtinFunction->toHandle(),
                                                                    &dispatchFuncArgs, hSignalEvent,
                                                                    numWaitEvents,
                                                                    phWaitEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryPrefetch(const void *ptr,
                                                                       size_t count) {
    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(ptr);
    if (allocData) {
        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_UNKNOWN;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryFill(void *ptr,
                                                                   const void *pattern,
                                                                   size_t patternSize,
                                                                   size_t size,
                                                                   ze_event_handle_t hSignalEvent,
                                                                   uint32_t numWaitEvents,
                                                                   ze_event_handle_t *phWaitEvents) {

    if (isCopyOnly()) {
        return appendBlitFill(ptr, pattern, patternSize, size, hSignalEvent, numWaitEvents, phWaitEvents);
    }

    ze_result_t res = addEventsToCmdList(numWaitEvents, phWaitEvents);
    if (res) {
        return res;
    }

    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    bool hostPointerNeedsFlush = false;

    NEO::SvmAllocationData *allocData = nullptr;
    bool dstAllocFound = device->getDriverHandle()->findAllocationDataForRange(ptr, size, &allocData);
    if (dstAllocFound) {
        if (allocData->memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY ||
            allocData->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY) {
            hostPointerNeedsFlush = true;
        }
    } else {
        if (device->getDriverHandle()->getHostPointerBaseAddress(ptr, nullptr) != ZE_RESULT_SUCCESS) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        } else {
            hostPointerNeedsFlush = true;
        }
    }

    auto dstAllocation = this->getAlignedAllocation(this->device, ptr, size);
    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    if (patternSize == 1) {
        auto builtinFunction = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferImmediate);

        uint32_t groupSizeX = builtinFunction->getImmutableData()->getDescriptor().kernelAttributes.simdSize;
        if (groupSizeX > static_cast<uint32_t>(size)) {
            groupSizeX = static_cast<uint32_t>(size);
        }
        if (builtinFunction->setGroupSize(groupSizeX, 1u, 1u)) {
            DEBUG_BREAK_IF(true);
            return ZE_RESULT_ERROR_UNKNOWN;
        }

        uint32_t value = *(reinterpret_cast<uint32_t *>(const_cast<void *>(pattern)));
        builtinFunction->setArgBufferWithAlloc(0, dstAllocation.alignedAllocationPtr, dstAllocation.alloc);
        builtinFunction->setArgumentValue(1, sizeof(dstAllocation.offset), &dstAllocation.offset);
        builtinFunction->setArgumentValue(2, sizeof(value), &value);

        appendEventForProfilingAllWalkers(hSignalEvent, true);

        uint32_t groups = static_cast<uint32_t>(size) / groupSizeX;
        ze_group_count_t dispatchFuncArgs{groups, 1u, 1u};
        res = appendLaunchKernelSplit(builtinFunction->toHandle(), &dispatchFuncArgs, hSignalEvent);
        if (res) {
            return res;
        }

        uint32_t groupRemainderSizeX = static_cast<uint32_t>(size) % groupSizeX;
        if (groupRemainderSizeX) {
            builtinFunction->setGroupSize(groupRemainderSizeX, 1u, 1u);
            ze_group_count_t dispatchFuncRemainderArgs{1u, 1u, 1u};

            size_t dstOffset = dstAllocation.offset + (size - groupRemainderSizeX);
            builtinFunction->setArgBufferWithAlloc(0, dstAllocation.alignedAllocationPtr, dstAllocation.alloc);
            builtinFunction->setArgumentValue(1, sizeof(dstOffset), &dstOffset);

            res = appendLaunchKernelSplit(builtinFunction->toHandle(), &dispatchFuncRemainderArgs, hSignalEvent);
            if (res) {
                return res;
            }
        }
    } else {
        auto builtinFunction = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferMiddle);

        size_t middleElSize = sizeof(uint32_t);
        size_t adjustedSize = size / middleElSize;
        uint32_t groupSizeX = static_cast<uint32_t>(adjustedSize);
        uint32_t groupSizeY = 1, groupSizeZ = 1;
        builtinFunction->suggestGroupSize(groupSizeX, groupSizeY, groupSizeZ, &groupSizeX, &groupSizeY, &groupSizeZ);
        builtinFunction->setGroupSize(groupSizeX, groupSizeY, groupSizeZ);

        uint32_t groups = static_cast<uint32_t>(adjustedSize) / groupSizeX;
        uint32_t groupRemainderSizeX = static_cast<uint32_t>(size) % groupSizeX;

        size_t patternAllocationSize = alignUp(patternSize, MemoryConstants::cacheLineSize);
        uint32_t patternSizeInEls = static_cast<uint32_t>(patternAllocationSize / middleElSize);

        auto patternGfxAlloc = getAllocationFromHostPtrMap(pattern, patternAllocationSize);
        if (patternGfxAlloc == nullptr) {
            patternGfxAlloc = device->getDriverHandle()->getMemoryManager()->allocateGraphicsMemoryWithProperties({device->getNEODevice()->getRootDeviceIndex(),
                                                                                                                   patternAllocationSize,
                                                                                                                   NEO::GraphicsAllocation::AllocationType::FILL_PATTERN,
                                                                                                                   device->getNEODevice()->getDeviceBitfield()});
            hostPtrMap.insert(std::make_pair(pattern, patternGfxAlloc));
        }
        void *patternGfxAllocPtr = patternGfxAlloc->getUnderlyingBuffer();

        uint64_t patternAllocPtr = reinterpret_cast<uintptr_t>(patternGfxAllocPtr);
        uint64_t patternAllocOffset = 0;
        uint64_t patternSizeToCopy = patternSize;
        do {
            memcpy_s(reinterpret_cast<void *>(patternAllocPtr + patternAllocOffset),
                     patternSizeToCopy, pattern, patternSizeToCopy);

            if ((patternAllocOffset + patternSizeToCopy) > patternAllocationSize) {
                patternSizeToCopy = patternAllocationSize - patternAllocOffset;
            }

            patternAllocOffset += patternSizeToCopy;
        } while (patternAllocOffset < patternAllocationSize);

        builtinFunction->setArgBufferWithAlloc(0, dstAllocation.alignedAllocationPtr, dstAllocation.alloc);
        builtinFunction->setArgumentValue(1, sizeof(dstAllocation.offset), &dstAllocation.offset);
        builtinFunction->setArgBufferWithAlloc(2, reinterpret_cast<uintptr_t>(patternGfxAllocPtr), patternGfxAlloc);
        builtinFunction->setArgumentValue(3, sizeof(patternSizeInEls), &patternSizeInEls);

        appendEventForProfilingAllWalkers(hSignalEvent, true);

        ze_group_count_t dispatchFuncArgs{groups, 1u, 1u};
        res = appendLaunchKernelSplit(builtinFunction->toHandle(), &dispatchFuncArgs, hSignalEvent);
        if (res) {
            return res;
        }

        if (groupRemainderSizeX) {
            uint32_t dstOffsetRemainder = groups * groupSizeX * static_cast<uint32_t>(middleElSize);
            uint64_t patternOffsetRemainder = (groupSizeX * groups & (patternSizeInEls - 1)) * middleElSize;

            auto builtinFunctionRemainder = device->getBuiltinFunctionsLib()->getFunction(Builtin::FillBufferRightLeftover);
            builtinFunctionRemainder->setGroupSize(groupRemainderSizeX, 1u, 1u);
            ze_group_count_t dispatchFuncArgs{1u, 1u, 1u};

            builtinFunctionRemainder->setArgBufferWithAlloc(0,
                                                            dstAllocation.alignedAllocationPtr,
                                                            dstAllocation.alloc);
            builtinFunctionRemainder->setArgumentValue(1,
                                                       sizeof(dstOffsetRemainder),
                                                       &dstOffsetRemainder);
            builtinFunctionRemainder->setArgBufferWithAlloc(2,
                                                            reinterpret_cast<uintptr_t>(patternGfxAllocPtr) + patternOffsetRemainder,
                                                            patternGfxAlloc);
            builtinFunctionRemainder->setArgumentValue(3, sizeof(patternSizeInEls), &patternSizeInEls);
            res = appendLaunchKernelSplit(builtinFunctionRemainder->toHandle(), &dispatchFuncArgs, hSignalEvent);
            if (res) {
                return res;
            }
        }
    }

    appendEventForProfilingAllWalkers(hSignalEvent, false);

    auto event = Event::fromHandle(hSignalEvent);
    if (event) {
        hostPointerNeedsFlush &= !event->signalScope;
    }

    if (hostPointerNeedsFlush) {
        NEO::PipeControlArgs args(true);
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
    }

    return res;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendBlitFill(void *ptr,
                                                                 const void *pattern,
                                                                 size_t patternSize,
                                                                 size_t size,
                                                                 ze_event_handle_t hSignalEvent,
                                                                 uint32_t numWaitEvents,
                                                                 ze_event_handle_t *phWaitEvents) {
    auto neoDevice = device->getNEODevice();
    if (NEO::HwHelper::get(device->getHwInfo().platform.eRenderCoreFamily).getMaxFillPaternSizeForCopyEngine() < patternSize) {
        return ZE_RESULT_ERROR_INVALID_SIZE;
    } else {
        ze_result_t ret = addEventsToCmdList(numWaitEvents, phWaitEvents);
        if (ret) {
            return ret;
        }
        appendEventForProfiling(hSignalEvent, true);
        NEO::GraphicsAllocation *gpuAllocation = device->getDriverHandle()->getDriverSystemMemoryAllocation(ptr,
                                                                                                            size,
                                                                                                            neoDevice->getRootDeviceIndex(),
                                                                                                            nullptr);
        if (gpuAllocation == nullptr) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
        commandContainer.addToResidencyContainer(gpuAllocation);
        uint32_t patternToCommand[4] = {};
        memcpy_s(&patternToCommand, sizeof(patternToCommand), pattern, patternSize);
        NEO::BlitCommandsHelper<GfxFamily>::dispatchBlitMemoryColorFill(gpuAllocation, patternToCommand, patternSize,
                                                                        *commandContainer.getCommandStream(),
                                                                        size,
                                                                        *neoDevice->getExecutionEnvironment()->rootDeviceEnvironments[device->getRootDeviceIndex()]);
        appendSignalEventPostWalker(hSignalEvent);
    }
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendSignalEventPostWalker(ze_event_handle_t hEvent) {
    if (hEvent == nullptr) {
        return;
    }
    auto event = Event::fromHandle(hEvent);
    if (event->isTimestampEvent) {
        appendEventForProfiling(hEvent, false);
    } else {
        CommandListCoreFamily<gfxCoreFamily>::appendSignalEvent(hEvent);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendEventForProfilingCopyCommand(ze_event_handle_t hEvent, bool beforeWalker) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    auto event = Event::fromHandle(hEvent);

    if (!event->isTimestampEvent) {
        return;
    }
    commandContainer.addToResidencyContainer(&event->getAllocation());

    if (!beforeWalker) {
        NEO::EncodeMiFlushDW<GfxFamily>::programMiFlushDw(*commandContainer.getCommandStream(), 0, 0, false, false);
    }
    appendWriteKernelTimestamp(hEvent, beforeWalker, false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline uint64_t CommandListCoreFamily<gfxCoreFamily>::getInputBufferSize(NEO::ImageType imageType,
                                                                         uint64_t bytesPerPixel,
                                                                         const ze_image_region_t *region) {

    switch (imageType) {
    default:
        UNRECOVERABLE_IF(true);
    case NEO::ImageType::Image1D:
    case NEO::ImageType::Image1DArray:
        return bytesPerPixel * region->width;
    case NEO::ImageType::Image2D:
    case NEO::ImageType::Image2DArray:
        return bytesPerPixel * region->width * region->height;
    case NEO::ImageType::Image3D:
        return bytesPerPixel * region->width * region->height * region->depth;
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline AlignedAllocationData CommandListCoreFamily<gfxCoreFamily>::getAlignedAllocation(Device *device,
                                                                                        const void *buffer,
                                                                                        uint64_t bufferSize) {

    NEO::SvmAllocationData *allocData = nullptr;
    void *ptr = const_cast<void *>(buffer);
    bool srcAllocFound = device->getDriverHandle()->findAllocationDataForRange(ptr,
                                                                               bufferSize, &allocData);
    NEO::GraphicsAllocation *alloc = nullptr;

    uintptr_t sourcePtr = reinterpret_cast<uintptr_t>(ptr);
    size_t offset = 0;
    NEO::EncodeSurfaceState<GfxFamily>::getSshAlignedPointer(sourcePtr, offset);
    uintptr_t alignedPtr = 0u;
    bool hostPointerNeedsFlush = false;

    if (srcAllocFound == false) {
        alloc = device->getDriverHandle()->findHostPointerAllocation(ptr, static_cast<size_t>(bufferSize), device->getRootDeviceIndex());
        if (alloc == nullptr) {
            alloc = getHostPtrAlloc(buffer, bufferSize);
        }
        alignedPtr = static_cast<uintptr_t>(alignDown(alloc->getGpuAddress(), NEO::EncodeSurfaceState<GfxFamily>::getSurfaceBaseAddressAlignment()));

        hostPointerNeedsFlush = true;
    } else {
        alloc = allocData->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
        alignedPtr = sourcePtr;

        if (allocData->memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY ||
            allocData->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY) {
            hostPointerNeedsFlush = true;
        }
    }

    return {alignedPtr, offset, alloc, hostPointerNeedsFlush};
}

template <GFXCORE_FAMILY gfxCoreFamily>
inline ze_result_t CommandListCoreFamily<gfxCoreFamily>::addEventsToCmdList(uint32_t numWaitEvents,
                                                                            ze_event_handle_t *phWaitEvents) {

    if (numWaitEvents > 0) {
        if (phWaitEvents) {
            CommandListCoreFamily<gfxCoreFamily>::appendWaitOnEvents(numWaitEvents, phWaitEvents);
        } else {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendSignalEvent(ze_event_handle_t hEvent) {
    using POST_SYNC_OPERATION = typename GfxFamily::PIPE_CONTROL::POST_SYNC_OPERATION;
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    auto event = Event::fromHandle(hEvent);

    commandContainer.addToResidencyContainer(&event->getAllocation());
    uint64_t baseAddr = event->getGpuAddress();
    size_t eventSignalOffset = 0;
    if (event->isTimestampEvent) {
        eventSignalOffset = offsetof(TimestampPacketStorage::Packet, contextEnd);
    }

    if (isCopyOnly()) {
        NEO::EncodeMiFlushDW<GfxFamily>::programMiFlushDw(*commandContainer.getCommandStream(), ptrOffset(baseAddr, eventSignalOffset), Event::STATE_SIGNALED, false, true);
    } else {
        NEO::PipeControlArgs args;
        args.dcFlushEnable = (!event->signalScope) ? false : true;
        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControlAndProgramPostSyncOperation(
            *commandContainer.getCommandStream(), POST_SYNC_OPERATION::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA,
            ptrOffset(baseAddr, eventSignalOffset), Event::STATE_SIGNALED,
            commandContainer.getDevice()->getHardwareInfo(),
            args);
    }
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendWaitOnEvents(uint32_t numEvents,
                                                                     ze_event_handle_t *phEvent) {

    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using MI_SEMAPHORE_WAIT = typename GfxFamily::MI_SEMAPHORE_WAIT;
    using COMPARE_OPERATION = typename GfxFamily::MI_SEMAPHORE_WAIT::COMPARE_OPERATION;

    uint64_t gpuAddr = 0;
    constexpr uint32_t eventStateClear = static_cast<uint32_t>(-1);
    bool dcFlushRequired = false;

    for (uint32_t i = 0; i < numEvents; i++) {
        auto event = Event::fromHandle(phEvent[i]);
        dcFlushRequired |= (!event->waitScope) ? false : true;
    }

    if (dcFlushRequired) {
        if (isCopyOnly()) {
            NEO::EncodeMiFlushDW<GfxFamily>::programMiFlushDw(*commandContainer.getCommandStream(), 0, 0, false, false);
        } else {
            NEO::PipeControlArgs args(true);
            NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
        }
    }

    for (uint32_t i = 0; i < numEvents; i++) {
        auto event = Event::fromHandle(phEvent[i]);
        commandContainer.addToResidencyContainer(&event->getAllocation());

        gpuAddr = event->getGpuAddress();
        if (event->isTimestampEvent) {
            gpuAddr += offsetof(TimestampPacketStorage::Packet, contextEnd);
        }
        NEO::EncodeSempahore<GfxFamily>::addMiSemaphoreWaitCommand(*commandContainer.getCommandStream(),
                                                                   gpuAddr,
                                                                   eventStateClear,
                                                                   COMPARE_OPERATION::COMPARE_OPERATION_SAD_NOT_EQUAL_SDD);
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendWriteKernelTimestamp(ze_event_handle_t hEvent, bool beforeWalker, bool maskLsb) {
    constexpr uint32_t mask = 0xfffffffe;
    auto event = Event::fromHandle(hEvent);

    auto baseAddr = event->getGpuAddress();
    auto contextOffset = beforeWalker ? offsetof(TimestampPacketStorage::Packet, contextStart) : offsetof(TimestampPacketStorage::Packet, contextEnd);
    auto globalOffset = beforeWalker ? offsetof(TimestampPacketStorage::Packet, globalStart) : offsetof(TimestampPacketStorage::Packet, globalEnd);

    if (maskLsb) {
        NEO::EncodeMathMMIO<GfxFamily>::encodeBitwiseAndVal(commandContainer, REG_GLOBAL_TIMESTAMP_LDW, mask, ptrOffset(baseAddr, globalOffset));
        NEO::EncodeMathMMIO<GfxFamily>::encodeBitwiseAndVal(commandContainer, GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, mask, ptrOffset(baseAddr, contextOffset));
    } else {
        NEO::EncodeStoreMMIO<GfxFamily>::encode(*commandContainer.getCommandStream(), REG_GLOBAL_TIMESTAMP_LDW, ptrOffset(baseAddr, globalOffset));
        NEO::EncodeStoreMMIO<GfxFamily>::encode(*commandContainer.getCommandStream(), GP_THREAD_TIME_REG_ADDRESS_OFFSET_LOW, ptrOffset(baseAddr, contextOffset));
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::appendEventForProfiling(ze_event_handle_t hEvent, bool beforeWalker) {
    if (!hEvent) {
        return;
    }
    if (isCopyOnly()) {
        appendEventForProfilingCopyCommand(hEvent, beforeWalker);
    } else {
        auto event = Event::fromHandle(hEvent);

        if (!event->isTimestampEvent) {
            return;
        }

        commandContainer.addToResidencyContainer(&event->getAllocation());

        if (beforeWalker) {
            appendWriteKernelTimestamp(hEvent, beforeWalker, true);
        } else {

            NEO::PipeControlArgs args = {};
            args.dcFlushEnable = (!event->signalScope) ? false : true;

            NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);
            appendWriteKernelTimestamp(hEvent, beforeWalker, true);
        }
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendWriteGlobalTimestamp(
    uint64_t *dstptr, ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {

    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    using POST_SYNC_OPERATION = typename GfxFamily::PIPE_CONTROL::POST_SYNC_OPERATION;

    if (numWaitEvents > 0) {
        if (phWaitEvents) {
            CommandListCoreFamily<gfxCoreFamily>::appendWaitOnEvents(numWaitEvents, phWaitEvents);
        } else {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
    }

    if (isCopyOnly()) {
        NEO::EncodeMiFlushDW<GfxFamily>::programMiFlushDw(*commandContainer.getCommandStream(),
                                                          reinterpret_cast<uint64_t>(dstptr),
                                                          0,
                                                          true,
                                                          true);
    } else {
        NEO::PipeControlArgs args(false);

        NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControlWithPostSync(
            *commandContainer.getCommandStream(),
            POST_SYNC_OPERATION::POST_SYNC_OPERATION_WRITE_TIMESTAMP,
            reinterpret_cast<uint64_t>(dstptr),
            0,
            args);
    }

    if (hSignalEvent) {
        CommandListCoreFamily<gfxCoreFamily>::appendSignalEvent(hSignalEvent);
    }

    auto allocationStruct = getAlignedAllocation(this->device, dstptr, sizeof(uint64_t));
    commandContainer.addToResidencyContainer(allocationStruct.alloc);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopyFromContext(
    void *dstptr, ze_context_handle_t hContextSrc, const void *srcptr,
    size_t size, ze_event_handle_t hSignalEvent, uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {

    return CommandListCoreFamily<gfxCoreFamily>::appendMemoryCopy(dstptr, srcptr, size, hSignalEvent, numWaitEvents, phWaitEvents);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::appendQueryKernelTimestamps(
    uint32_t numEvents, ze_event_handle_t *phEvents, void *dstptr,
    const size_t *pOffsets, ze_event_handle_t hSignalEvent,
    uint32_t numWaitEvents, ze_event_handle_t *phWaitEvents) {

    auto dstptrAllocationStruct = getAlignedAllocation(this->device, dstptr, sizeof(ze_kernel_timestamp_result_t) * numEvents);
    commandContainer.addToResidencyContainer(dstptrAllocationStruct.alloc);

    std::unique_ptr<EventData[]> timestampsData = std::make_unique<EventData[]>(numEvents);

    for (uint32_t i = 0u; i < numEvents; ++i) {
        auto event = Event::fromHandle(phEvents[i]);
        commandContainer.addToResidencyContainer(&event->getAllocation());
        timestampsData[i].address = event->getGpuAddress();
        timestampsData[i].packetsInUse = event->getPacketsInUse();
    }

    size_t alignedSize = alignUp<size_t>(sizeof(EventData) * numEvents, MemoryConstants::pageSize64k);
    NEO::GraphicsAllocation::AllocationType allocationType = NEO::GraphicsAllocation::AllocationType::GPU_TIMESTAMP_DEVICE_BUFFER;
    auto devices = device->getNEODevice()->getDeviceBitfield();
    NEO::AllocationProperties allocationProperties{device->getRootDeviceIndex(),
                                                   true,
                                                   alignedSize,
                                                   allocationType,
                                                   devices.count() > 1,
                                                   false,
                                                   devices};

    NEO::GraphicsAllocation *timestampsGPUData = device->getDriverHandle()->getMemoryManager()->allocateGraphicsMemoryWithProperties(allocationProperties);

    UNRECOVERABLE_IF(timestampsGPUData == nullptr);

    commandContainer.addToResidencyContainer(timestampsGPUData);
    commandContainer.getDeallocationContainer().push_back(timestampsGPUData);

    bool result = device->getDriverHandle()->getMemoryManager()->copyMemoryToAllocation(timestampsGPUData, 0, timestampsData.get(), sizeof(EventData) * numEvents);

    UNRECOVERABLE_IF(!result);

    Kernel *builtinFunction = nullptr;
    auto useOnlyGlobalTimestamps = NEO::HwHelper::get(device->getHwInfo().platform.eRenderCoreFamily).useOnlyGlobalTimestamps() ? 1u : 0u;
    auto lock = device->getBuiltinFunctionsLib()->obtainUniqueOwnership();

    if (pOffsets == nullptr) {
        builtinFunction = device->getBuiltinFunctionsLib()->getFunction(Builtin::QueryKernelTimestamps);
        builtinFunction->setArgumentValue(2u, sizeof(uint32_t), &useOnlyGlobalTimestamps);
    } else {
        auto pOffsetAllocationStruct = getAlignedAllocation(this->device, pOffsets, sizeof(size_t) * numEvents);
        auto offsetValPtr = static_cast<uintptr_t>(pOffsetAllocationStruct.alloc->getGpuAddress());
        commandContainer.addToResidencyContainer(pOffsetAllocationStruct.alloc);
        builtinFunction = device->getBuiltinFunctionsLib()->getFunction(Builtin::QueryKernelTimestampsWithOffsets);
        builtinFunction->setArgBufferWithAlloc(2, offsetValPtr, pOffsetAllocationStruct.alloc);
        builtinFunction->setArgumentValue(3u, sizeof(uint32_t), &useOnlyGlobalTimestamps);
        offsetValPtr += sizeof(size_t);
    }

    uint32_t groupSizeX = 1u;
    uint32_t groupSizeY = 1u;
    uint32_t groupSizeZ = 1u;

    if (builtinFunction->suggestGroupSize(numEvents, 1u, 1u,
                                          &groupSizeX, &groupSizeY, &groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    if (builtinFunction->setGroupSize(groupSizeX, groupSizeY, groupSizeZ) != ZE_RESULT_SUCCESS) {
        DEBUG_BREAK_IF(true);
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    ze_group_count_t dispatchFuncArgs{numEvents / groupSizeX, 1u, 1u};

    auto dstValPtr = static_cast<uintptr_t>(dstptrAllocationStruct.alloc->getGpuAddress());

    builtinFunction->setArgBufferWithAlloc(0u, static_cast<uintptr_t>(timestampsGPUData->getGpuAddress()), timestampsGPUData);
    builtinFunction->setArgBufferWithAlloc(1, dstValPtr, dstptrAllocationStruct.alloc);

    auto appendResult = appendLaunchKernel(builtinFunction->toHandle(), &dispatchFuncArgs, hSignalEvent, numWaitEvents,
                                           phWaitEvents);
    if (appendResult != ZE_RESULT_SUCCESS) {
        return appendResult;
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::reserveSpace(size_t size, void **ptr) {
    auto availableSpace = commandContainer.getCommandStream()->getAvailableSpace();
    if (availableSpace < size) {
        *ptr = nullptr;
    } else {
        *ptr = commandContainer.getCommandStream()->getSpace(size);
    }
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::prepareIndirectParams(const ze_group_count_t *pThreadGroupDimensions) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;
    auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(pThreadGroupDimensions);
    if (allocData) {
        auto alloc = allocData->gpuAllocations.getGraphicsAllocation(device->getRootDeviceIndex());
        commandContainer.addToResidencyContainer(alloc);

        size_t groupCountOffset = 0;
        if (allocData->cpuAllocation != nullptr) {
            commandContainer.addToResidencyContainer(allocData->cpuAllocation);
            groupCountOffset = ptrDiff(pThreadGroupDimensions, allocData->cpuAllocation->getUnderlyingBuffer());
        } else {
            groupCountOffset = ptrDiff(pThreadGroupDimensions, alloc->getGpuAddress());
        }

        auto groupCount = ptrOffset(alloc->getGpuAddress(), groupCountOffset);

        NEO::EncodeSetMMIO<GfxFamily>::encodeMEM(commandContainer, GPUGPU_DISPATCHDIMX,
                                                 ptrOffset(groupCount, offsetof(ze_group_count_t, groupCountX)));
        NEO::EncodeSetMMIO<GfxFamily>::encodeMEM(commandContainer, GPUGPU_DISPATCHDIMY,
                                                 ptrOffset(groupCount, offsetof(ze_group_count_t, groupCountY)));
        NEO::EncodeSetMMIO<GfxFamily>::encodeMEM(commandContainer, GPUGPU_DISPATCHDIMZ,
                                                 ptrOffset(groupCount, offsetof(ze_group_count_t, groupCountZ)));
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandListCoreFamily<gfxCoreFamily>::setGlobalWorkSizeIndirect(NEO::CrossThreadDataOffset offsets[3], void *crossThreadAddress, uint32_t lws[3]) {
    using GfxFamily = typename NEO::GfxFamilyMapper<gfxCoreFamily>::GfxFamily;

    NEO::EncodeIndirectParams<GfxFamily>::setGlobalWorkSizeIndirect(commandContainer, offsets, crossThreadAddress, lws);

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandListCoreFamily<gfxCoreFamily>::programStateBaseAddress(NEO::CommandContainer &container, bool genericMediaStateClearRequired) {
    NEO::PipeControlArgs args(true);
    args.hdcPipelineFlush = true;
    args.textureCacheInvalidationEnable = true;
    NEO::MemorySynchronizationCommands<GfxFamily>::addPipeControl(*commandContainer.getCommandStream(), args);

    STATE_BASE_ADDRESS sba;
    NEO::EncodeStateBaseAddress<GfxFamily>::encode(commandContainer, sba);
    if (NEO::Debugger::isDebugEnabled(this->internalUsage) && device->getL0Debugger()) {
        NEO::Debugger::SbaAddresses sbaAddresses = {};
        sbaAddresses.BindlessSurfaceStateBaseAddress = sba.getBindlessSurfaceStateBaseAddress();
        sbaAddresses.DynamicStateBaseAddress = sba.getDynamicStateBaseAddress();
        sbaAddresses.GeneralStateBaseAddress = sba.getGeneralStateBaseAddress();
        sbaAddresses.IndirectObjectBaseAddress = sba.getIndirectObjectBaseAddress();
        sbaAddresses.InstructionBaseAddress = sba.getInstructionBaseAddress();
        sbaAddresses.SurfaceStateBaseAddress = sba.getSurfaceStateBaseAddress();

        device->getL0Debugger()->captureStateBaseAddress(commandContainer, sbaAddresses);
    }
}

} // namespace L0
