/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/source/command_queue/command_queue.h"

#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/helpers/aligned_memory.h"
#include "shared/source/helpers/array_count.h"
#include "shared/source/helpers/engine_node_helper.h"
#include "shared/source/helpers/get_info.h"
#include "shared/source/helpers/ptr_math.h"
#include "shared/source/helpers/string.h"
#include "shared/source/helpers/timestamp_packet.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/utilities/api_intercept.h"
#include "shared/source/utilities/tag_allocator.h"

#include "opencl/source/built_ins/builtins_dispatch_builder.h"
#include "opencl/source/cl_device/cl_device.h"
#include "opencl/source/context/context.h"
#include "opencl/source/device_queue/device_queue.h"
#include "opencl/source/event/event_builder.h"
#include "opencl/source/event/user_event.h"
#include "opencl/source/gtpin/gtpin_notify.h"
#include "opencl/source/helpers/cl_hw_helper.h"
#include "opencl/source/helpers/convert_color.h"
#include "opencl/source/helpers/hardware_commands_helper.h"
#include "opencl/source/helpers/mipmap.h"
#include "opencl/source/helpers/queue_helpers.h"
#include "opencl/source/mem_obj/buffer.h"
#include "opencl/source/mem_obj/image.h"
#include "opencl/source/program/printf_handler.h"

#include "CL/cl_ext.h"

#include <limits>
#include <map>

namespace NEO {

// Global table of create functions
CommandQueueCreateFunc commandQueueFactory[IGFX_MAX_CORE] = {};

CommandQueue *CommandQueue::create(Context *context,
                                   ClDevice *device,
                                   const cl_queue_properties *properties,
                                   bool internalUsage,
                                   cl_int &retVal) {
    retVal = CL_SUCCESS;

    auto funcCreate = commandQueueFactory[device->getRenderCoreFamily()];
    DEBUG_BREAK_IF(nullptr == funcCreate);

    return funcCreate(context, device, properties, internalUsage);
}

CommandQueue::CommandQueue(Context *context, ClDevice *device, const cl_queue_properties *properties)
    : context(context), device(device) {
    if (context) {
        context->incRefInternal();
    }

    commandQueueProperties = getCmdQueueProperties<cl_command_queue_properties>(properties);
    flushStamp.reset(new FlushStampTracker(true));

    if (device) {
        auto hwInfo = device->getHardwareInfo();
        auto &hwHelper = HwHelper::get(hwInfo.platform.eRenderCoreFamily);

        gpgpuEngine = &device->getDefaultEngine();
        UNRECOVERABLE_IF(gpgpuEngine->getEngineType() >= aub_stream::EngineType::NUM_ENGINES);

        bool bcsAllowed = hwInfo.capabilityTable.blitterOperationsSupported &&
                          hwHelper.isSubDeviceEngineSupported(hwInfo, device->getDeviceBitfield(), aub_stream::EngineType::ENGINE_BCS);

        if (bcsAllowed || gpgpuEngine->commandStreamReceiver->peekTimestampPacketWriteEnabled()) {
            timestampPacketContainer = std::make_unique<TimestampPacketContainer>();
        }
        if (bcsAllowed) {
            auto &selectorCopyEngine = device->getDeviceById(0)->getSelectorCopyEngine();
            bcsEngine = &device->getDeviceById(0)->getEngine(EngineHelpers::getBcsEngineType(hwInfo, selectorCopyEngine), EngineUsage::Regular);
        }
    }

    storeProperties(properties);
    processProperties(properties);
}

CommandQueue::~CommandQueue() {
    if (virtualEvent) {
        UNRECOVERABLE_IF(this->virtualEvent->getCommandQueue() != this && this->virtualEvent->getCommandQueue() != nullptr);
        virtualEvent->decRefInternal();
    }

    if (device) {
        auto storageForAllocation = gpgpuEngine->commandStreamReceiver->getInternalAllocationStorage();

        if (commandStream) {
            storageForAllocation->storeAllocation(std::unique_ptr<GraphicsAllocation>(commandStream->getGraphicsAllocation()), REUSABLE_ALLOCATION);
        }
        delete commandStream;

        if (this->perfCountersEnabled) {
            device->getPerformanceCounters()->shutdown();
        }
    }

    timestampPacketContainer.reset();
    //for normal queue, decrement ref count on context
    //special queue is owned by context so ref count doesn't have to be decremented
    if (context && !isSpecialCommandQueue) {
        context->decRefInternal();
    }
}

CommandStreamReceiver &CommandQueue::getGpgpuCommandStreamReceiver() const {
    return *gpgpuEngine->commandStreamReceiver;
}

CommandStreamReceiver *CommandQueue::getBcsCommandStreamReceiver() const {
    if (bcsEngine) {
        return bcsEngine->commandStreamReceiver;
    }
    return nullptr;
}

CommandStreamReceiver &CommandQueue::getCommandStreamReceiver(bool blitAllowed) const {
    if (blitAllowed) {
        auto csr = getBcsCommandStreamReceiver();
        UNRECOVERABLE_IF(!csr);
        return *csr;
    }
    return getGpgpuCommandStreamReceiver();
}

Device &CommandQueue::getDevice() const noexcept {
    return device->getDevice();
}

uint32_t CommandQueue::getHwTag() const {
    uint32_t tag = *getHwTagAddress();
    return tag;
}

volatile uint32_t *CommandQueue::getHwTagAddress() const {
    return getGpgpuCommandStreamReceiver().getTagAddress();
}

bool CommandQueue::isCompleted(uint32_t gpgpuTaskCount, uint32_t bcsTaskCount) const {
    uint32_t gpgpuHwTag = getHwTag();
    DEBUG_BREAK_IF(gpgpuHwTag == CompletionStamp::notReady);

    if (gpgpuHwTag >= gpgpuTaskCount) {
        if (auto bcsCsr = getBcsCommandStreamReceiver()) {
            return (*bcsCsr->getTagAddress()) >= bcsTaskCount;
        }

        return true;
    }

    return false;
}

void CommandQueue::waitUntilComplete(uint32_t gpgpuTaskCountToWait, uint32_t bcsTaskCountToWait, FlushStamp flushStampToWait, bool useQuickKmdSleep) {
    WAIT_ENTER()

    DBG_LOG(LogTaskCounts, __FUNCTION__, "Waiting for taskCount:", gpgpuTaskCountToWait);
    DBG_LOG(LogTaskCounts, __FUNCTION__, "Line: ", __LINE__, "Current taskCount:", getHwTag());

    bool forcePowerSavingMode = this->throttle == QueueThrottle::LOW;

    getGpgpuCommandStreamReceiver().waitForTaskCountWithKmdNotifyFallback(gpgpuTaskCountToWait, flushStampToWait,
                                                                          useQuickKmdSleep, forcePowerSavingMode);
    DEBUG_BREAK_IF(getHwTag() < gpgpuTaskCountToWait);

    if (gtpinIsGTPinInitialized()) {
        gtpinNotifyTaskCompletion(gpgpuTaskCountToWait);
    }

    if (auto bcsCsr = getBcsCommandStreamReceiver()) {
        bcsCsr->waitForTaskCountWithKmdNotifyFallback(bcsTaskCountToWait, 0, false, false);
        bcsCsr->waitForTaskCountAndCleanTemporaryAllocationList(bcsTaskCountToWait);
    }

    getGpgpuCommandStreamReceiver().waitForTaskCountAndCleanTemporaryAllocationList(gpgpuTaskCountToWait);

    WAIT_LEAVE()
}

bool CommandQueue::isQueueBlocked() {
    TakeOwnershipWrapper<CommandQueue> takeOwnershipWrapper(*this);
    //check if we have user event and if so, if it is in blocked state.
    if (this->virtualEvent) {
        auto executionStatus = this->virtualEvent->peekExecutionStatus();
        if (executionStatus <= CL_SUBMITTED) {
            UNRECOVERABLE_IF(this->virtualEvent == nullptr);

            if (this->virtualEvent->isStatusCompletedByTermination(executionStatus) == false) {
                taskCount = this->virtualEvent->peekTaskCount();
                flushStamp->setStamp(this->virtualEvent->flushStamp->peekStamp());
                taskLevel = this->virtualEvent->taskLevel;
                // If this isn't an OOQ, update the taskLevel for the queue
                if (!isOOQEnabled()) {
                    taskLevel++;
                }
            } else {
                //at this point we may reset queue TaskCount, since all command previous to this were aborted
                taskCount = 0;
                flushStamp->setStamp(0);
                taskLevel = getGpgpuCommandStreamReceiver().peekTaskLevel();
            }

            FileLoggerInstance().log(DebugManager.flags.EventsDebugEnable.get(), "isQueueBlocked taskLevel change from", taskLevel, "to new from virtualEvent", this->virtualEvent, "new tasklevel", this->virtualEvent->taskLevel.load());

            //close the access to virtual event, driver added only 1 ref count.
            this->virtualEvent->decRefInternal();
            this->virtualEvent = nullptr;
            return false;
        }
        return true;
    }
    return false;
}

cl_int CommandQueue::getCommandQueueInfo(cl_command_queue_info paramName,
                                         size_t paramValueSize,
                                         void *paramValue,
                                         size_t *paramValueSizeRet) {
    return getQueueInfo<CommandQueue>(this, paramName, paramValueSize, paramValue, paramValueSizeRet);
}

uint32_t CommandQueue::getTaskLevelFromWaitList(uint32_t taskLevel,
                                                cl_uint numEventsInWaitList,
                                                const cl_event *eventWaitList) {
    for (auto iEvent = 0u; iEvent < numEventsInWaitList; ++iEvent) {
        auto pEvent = (Event *)(eventWaitList[iEvent]);
        uint32_t eventTaskLevel = pEvent->taskLevel;
        taskLevel = std::max(taskLevel, eventTaskLevel);
    }
    return taskLevel;
}

LinearStream &CommandQueue::getCS(size_t minRequiredSize) {
    DEBUG_BREAK_IF(nullptr == device);

    if (!commandStream) {
        commandStream = new LinearStream(nullptr);
    }

    minRequiredSize += CSRequirements::minCommandQueueCommandStreamSize;
    constexpr static auto additionalAllocationSize = CSRequirements::minCommandQueueCommandStreamSize + CSRequirements::csOverfetchSize;
    getGpgpuCommandStreamReceiver().ensureCommandBufferAllocation(*commandStream, minRequiredSize, additionalAllocationSize);
    return *commandStream;
}

cl_int CommandQueue::enqueueAcquireSharedObjects(cl_uint numObjects, const cl_mem *memObjects, cl_uint numEventsInWaitList, const cl_event *eventWaitList, cl_event *oclEvent, cl_uint cmdType) {
    if ((memObjects == nullptr && numObjects != 0) || (memObjects != nullptr && numObjects == 0)) {
        return CL_INVALID_VALUE;
    }

    for (unsigned int object = 0; object < numObjects; object++) {
        auto memObject = castToObject<MemObj>(memObjects[object]);
        if (memObject == nullptr || memObject->peekSharingHandler() == nullptr) {
            return CL_INVALID_MEM_OBJECT;
        }

        int result = memObject->peekSharingHandler()->acquire(memObject, getDevice().getRootDeviceIndex());
        if (result != CL_SUCCESS) {
            return result;
        }
        memObject->acquireCount++;
    }
    auto status = enqueueMarkerWithWaitList(
        numEventsInWaitList,
        eventWaitList,
        oclEvent);

    if (oclEvent) {
        castToObjectOrAbort<Event>(*oclEvent)->setCmdType(cmdType);
    }

    return status;
}

cl_int CommandQueue::enqueueReleaseSharedObjects(cl_uint numObjects, const cl_mem *memObjects, cl_uint numEventsInWaitList, const cl_event *eventWaitList, cl_event *oclEvent, cl_uint cmdType) {
    if ((memObjects == nullptr && numObjects != 0) || (memObjects != nullptr && numObjects == 0)) {
        return CL_INVALID_VALUE;
    }

    for (unsigned int object = 0; object < numObjects; object++) {
        auto memObject = castToObject<MemObj>(memObjects[object]);
        if (memObject == nullptr || memObject->peekSharingHandler() == nullptr) {
            return CL_INVALID_MEM_OBJECT;
        }

        memObject->peekSharingHandler()->release(memObject, getDevice().getRootDeviceIndex());
        DEBUG_BREAK_IF(memObject->acquireCount <= 0);
        memObject->acquireCount--;
    }
    auto status = enqueueMarkerWithWaitList(
        numEventsInWaitList,
        eventWaitList,
        oclEvent);

    if (oclEvent) {
        castToObjectOrAbort<Event>(*oclEvent)->setCmdType(cmdType);
    }
    return status;
}

void CommandQueue::updateFromCompletionStamp(const CompletionStamp &completionStamp, Event *outEvent) {
    DEBUG_BREAK_IF(this->taskLevel > completionStamp.taskLevel);
    DEBUG_BREAK_IF(this->taskCount > completionStamp.taskCount);
    if (completionStamp.taskCount != CompletionStamp::notReady) {
        taskCount = completionStamp.taskCount;
    }
    flushStamp->setStamp(completionStamp.flushStamp);
    this->taskLevel = completionStamp.taskLevel;

    if (outEvent) {
        outEvent->updateCompletionStamp(completionStamp.taskCount, bcsTaskCount, completionStamp.taskLevel, completionStamp.flushStamp);
        FileLoggerInstance().log(DebugManager.flags.EventsDebugEnable.get(), "updateCompletionStamp Event", outEvent, "taskLevel", outEvent->taskLevel.load());
    }
}

bool CommandQueue::setPerfCountersEnabled() {
    DEBUG_BREAK_IF(device == nullptr);

    auto perfCounters = device->getPerformanceCounters();
    bool isCcsEngine = EngineHelpers::isCcs(getGpgpuEngine().osContext->getEngineType());

    perfCountersEnabled = perfCounters->enable(isCcsEngine);

    if (!perfCountersEnabled) {
        perfCounters->shutdown();
    }

    return perfCountersEnabled;
}

PerformanceCounters *CommandQueue::getPerfCounters() {
    return device->getPerformanceCounters();
}

cl_int CommandQueue::enqueueWriteMemObjForUnmap(MemObj *memObj, void *mappedPtr, EventsRequest &eventsRequest) {
    cl_int retVal = CL_SUCCESS;

    MapInfo unmapInfo;
    if (!memObj->findMappedPtr(mappedPtr, unmapInfo)) {
        return CL_INVALID_VALUE;
    }

    if (!unmapInfo.readOnly) {
        memObj->getMapAllocation(getDevice().getRootDeviceIndex())->setAubWritable(true, GraphicsAllocation::defaultBank);
        memObj->getMapAllocation(getDevice().getRootDeviceIndex())->setTbxWritable(true, GraphicsAllocation::defaultBank);

        if (memObj->peekClMemObjType() == CL_MEM_OBJECT_BUFFER) {
            auto buffer = castToObject<Buffer>(memObj);

            retVal = enqueueWriteBuffer(buffer, CL_FALSE, unmapInfo.offset[0], unmapInfo.size[0], mappedPtr, memObj->getMapAllocation(getDevice().getRootDeviceIndex()),
                                        eventsRequest.numEventsInWaitList, eventsRequest.eventWaitList, eventsRequest.outEvent);
        } else {
            auto image = castToObjectOrAbort<Image>(memObj);
            size_t writeOrigin[4] = {unmapInfo.offset[0], unmapInfo.offset[1], unmapInfo.offset[2], 0};
            auto mipIdx = getMipLevelOriginIdx(image->peekClMemObjType());
            UNRECOVERABLE_IF(mipIdx >= 4);
            writeOrigin[mipIdx] = unmapInfo.mipLevel;
            retVal = enqueueWriteImage(image, CL_FALSE, writeOrigin, &unmapInfo.size[0],
                                       image->getHostPtrRowPitch(), image->getHostPtrSlicePitch(), mappedPtr, memObj->getMapAllocation(getDevice().getRootDeviceIndex()),
                                       eventsRequest.numEventsInWaitList, eventsRequest.eventWaitList, eventsRequest.outEvent);
        }
    } else {
        retVal = enqueueMarkerWithWaitList(eventsRequest.numEventsInWaitList, eventsRequest.eventWaitList, eventsRequest.outEvent);
    }

    if (retVal == CL_SUCCESS) {
        memObj->removeMappedPtr(mappedPtr);
        if (eventsRequest.outEvent) {
            auto event = castToObject<Event>(*eventsRequest.outEvent);
            event->setCmdType(CL_COMMAND_UNMAP_MEM_OBJECT);
        }
    }
    return retVal;
}

void *CommandQueue::enqueueReadMemObjForMap(TransferProperties &transferProperties, EventsRequest &eventsRequest, cl_int &errcodeRet) {
    void *basePtr = transferProperties.memObj->getBasePtrForMap(getDevice().getRootDeviceIndex());
    size_t mapPtrOffset = transferProperties.memObj->calculateOffsetForMapping(transferProperties.offset) + transferProperties.mipPtrOffset;
    if (transferProperties.memObj->peekClMemObjType() == CL_MEM_OBJECT_BUFFER) {
        mapPtrOffset += transferProperties.memObj->getOffset();
    }
    void *returnPtr = ptrOffset(basePtr, mapPtrOffset);

    if (!transferProperties.memObj->addMappedPtr(returnPtr, transferProperties.memObj->calculateMappedPtrLength(transferProperties.size),
                                                 transferProperties.mapFlags, transferProperties.size, transferProperties.offset, transferProperties.mipLevel)) {
        errcodeRet = CL_INVALID_OPERATION;
        return nullptr;
    }

    if (transferProperties.memObj->peekClMemObjType() == CL_MEM_OBJECT_BUFFER) {
        auto buffer = castToObject<Buffer>(transferProperties.memObj);
        errcodeRet = enqueueReadBuffer(buffer, transferProperties.blocking, transferProperties.offset[0], transferProperties.size[0],
                                       returnPtr, transferProperties.memObj->getMapAllocation(getDevice().getRootDeviceIndex()), eventsRequest.numEventsInWaitList,
                                       eventsRequest.eventWaitList, eventsRequest.outEvent);
    } else {
        auto image = castToObjectOrAbort<Image>(transferProperties.memObj);
        size_t readOrigin[4] = {transferProperties.offset[0], transferProperties.offset[1], transferProperties.offset[2], 0};
        auto mipIdx = getMipLevelOriginIdx(image->peekClMemObjType());
        UNRECOVERABLE_IF(mipIdx >= 4);
        readOrigin[mipIdx] = transferProperties.mipLevel;
        errcodeRet = enqueueReadImage(image, transferProperties.blocking, readOrigin, &transferProperties.size[0],
                                      image->getHostPtrRowPitch(), image->getHostPtrSlicePitch(),
                                      returnPtr, transferProperties.memObj->getMapAllocation(getDevice().getRootDeviceIndex()), eventsRequest.numEventsInWaitList,
                                      eventsRequest.eventWaitList, eventsRequest.outEvent);
    }

    if (errcodeRet != CL_SUCCESS) {
        transferProperties.memObj->removeMappedPtr(returnPtr);
        return nullptr;
    }
    if (eventsRequest.outEvent) {
        auto event = castToObject<Event>(*eventsRequest.outEvent);
        event->setCmdType(transferProperties.cmdType);
    }
    return returnPtr;
}

void *CommandQueue::enqueueMapMemObject(TransferProperties &transferProperties, EventsRequest &eventsRequest, cl_int &errcodeRet) {
    if (transferProperties.memObj->mappingOnCpuAllowed()) {
        return cpuDataTransferHandler(transferProperties, eventsRequest, errcodeRet);
    } else {
        return enqueueReadMemObjForMap(transferProperties, eventsRequest, errcodeRet);
    }
}

cl_int CommandQueue::enqueueUnmapMemObject(TransferProperties &transferProperties, EventsRequest &eventsRequest) {
    cl_int retVal = CL_SUCCESS;
    if (transferProperties.memObj->mappingOnCpuAllowed()) {
        cpuDataTransferHandler(transferProperties, eventsRequest, retVal);
    } else {
        retVal = enqueueWriteMemObjForUnmap(transferProperties.memObj, transferProperties.ptr, eventsRequest);
    }
    return retVal;
}

void *CommandQueue::enqueueMapBuffer(Buffer *buffer, cl_bool blockingMap,
                                     cl_map_flags mapFlags, size_t offset,
                                     size_t size, cl_uint numEventsInWaitList,
                                     const cl_event *eventWaitList, cl_event *event,
                                     cl_int &errcodeRet) {
    TransferProperties transferProperties(buffer, CL_COMMAND_MAP_BUFFER, mapFlags, blockingMap != CL_FALSE, &offset, &size, nullptr, false, getDevice().getRootDeviceIndex());
    EventsRequest eventsRequest(numEventsInWaitList, eventWaitList, event);

    return enqueueMapMemObject(transferProperties, eventsRequest, errcodeRet);
}

void *CommandQueue::enqueueMapImage(Image *image, cl_bool blockingMap,
                                    cl_map_flags mapFlags, const size_t *origin,
                                    const size_t *region, size_t *imageRowPitch,
                                    size_t *imageSlicePitch,
                                    cl_uint numEventsInWaitList,
                                    const cl_event *eventWaitList, cl_event *event,
                                    cl_int &errcodeRet) {
    TransferProperties transferProperties(image, CL_COMMAND_MAP_IMAGE, mapFlags, blockingMap != CL_FALSE,
                                          const_cast<size_t *>(origin), const_cast<size_t *>(region), nullptr, false, getDevice().getRootDeviceIndex());
    EventsRequest eventsRequest(numEventsInWaitList, eventWaitList, event);

    if (image->isMemObjZeroCopy() && image->mappingOnCpuAllowed()) {
        GetInfoHelper::set(imageSlicePitch, image->getImageDesc().image_slice_pitch);
        if (image->getImageDesc().image_type == CL_MEM_OBJECT_IMAGE1D_ARRAY) {
            // There are differences in qPitch programming between Gen8 vs Gen9+ devices.
            // For Gen8 qPitch is distance in rows while Gen9+ it is in pixels.
            // Minimum value of qPitch is 4 and this causes slicePitch = 4*rowPitch on Gen8.
            // To allow zero-copy we have to tell what is correct value rowPitch which should equal to slicePitch.
            GetInfoHelper::set(imageRowPitch, image->getImageDesc().image_slice_pitch);
        } else {
            GetInfoHelper::set(imageRowPitch, image->getImageDesc().image_row_pitch);
        }
    } else {
        GetInfoHelper::set(imageSlicePitch, image->getHostPtrSlicePitch());
        GetInfoHelper::set(imageRowPitch, image->getHostPtrRowPitch());
    }
    if (Image::hasSlices(image->peekClMemObjType()) == false) {
        GetInfoHelper::set(imageSlicePitch, static_cast<size_t>(0));
    }
    return enqueueMapMemObject(transferProperties, eventsRequest, errcodeRet);
}

cl_int CommandQueue::enqueueUnmapMemObject(MemObj *memObj, void *mappedPtr, cl_uint numEventsInWaitList, const cl_event *eventWaitList, cl_event *event) {
    TransferProperties transferProperties(memObj, CL_COMMAND_UNMAP_MEM_OBJECT, 0, false, nullptr, nullptr, mappedPtr, false, getDevice().getRootDeviceIndex());
    EventsRequest eventsRequest(numEventsInWaitList, eventWaitList, event);

    return enqueueUnmapMemObject(transferProperties, eventsRequest);
}

void CommandQueue::enqueueBlockedMapUnmapOperation(const cl_event *eventWaitList,
                                                   size_t numEventsInWaitlist,
                                                   MapOperationType opType,
                                                   MemObj *memObj,
                                                   MemObjSizeArray &copySize,
                                                   MemObjOffsetArray &copyOffset,
                                                   bool readOnly,
                                                   EventBuilder &externalEventBuilder) {
    EventBuilder internalEventBuilder;
    EventBuilder *eventBuilder;
    // check if event will be exposed externally
    if (externalEventBuilder.getEvent()) {
        externalEventBuilder.getEvent()->incRefInternal();
        eventBuilder = &externalEventBuilder;
    } else {
        // it will be an internal event
        internalEventBuilder.create<VirtualEvent>(this, context);
        eventBuilder = &internalEventBuilder;
    }

    //store task data in event
    auto cmd = std::unique_ptr<Command>(new CommandMapUnmap(opType, *memObj, copySize, copyOffset, readOnly, *this));
    eventBuilder->getEvent()->setCommand(std::move(cmd));

    //bind output event with input events
    eventBuilder->addParentEvents(ArrayRef<const cl_event>(eventWaitList, numEventsInWaitlist));
    eventBuilder->addParentEvent(this->virtualEvent);
    eventBuilder->finalize();

    if (this->virtualEvent) {
        this->virtualEvent->decRefInternal();
    }
    this->virtualEvent = eventBuilder->getEvent();
}

bool CommandQueue::setupDebugSurface(Kernel *kernel) {
    auto debugSurface = getGpgpuCommandStreamReceiver().getDebugSurfaceAllocation();

    auto rootDeviceIndex = device->getRootDeviceIndex();
    DEBUG_BREAK_IF(!kernel->requiresSshForBuffers(rootDeviceIndex));
    auto surfaceState = ptrOffset(reinterpret_cast<uintptr_t *>(kernel->getSurfaceStateHeap(rootDeviceIndex)),
                                  kernel->getKernelInfo(rootDeviceIndex).kernelDescriptor.payloadMappings.implicitArgs.systemThreadSurfaceAddress.bindful);
    void *addressToPatch = reinterpret_cast<void *>(debugSurface->getGpuAddress());
    size_t sizeToPatch = debugSurface->getUnderlyingBufferSize();
    Buffer::setSurfaceState(&device->getDevice(), surfaceState, false, false, sizeToPatch,
                            addressToPatch, 0, debugSurface, 0, 0,
                            kernel->getDefaultKernelInfo().kernelDescriptor.kernelAttributes.flags.useGlobalAtomics,
                            kernel->getTotalNumDevicesInContext());
    return true;
}

bool CommandQueue::validateCapability(cl_command_queue_capabilities_intel capability) const {
    return this->queueCapabilities == CL_QUEUE_DEFAULT_CAPABILITIES_INTEL || isValueSet(this->queueCapabilities, capability);
}

bool CommandQueue::validateCapabilitiesForEventWaitList(cl_uint numEventsInWaitList, const cl_event *waitList) const {
    for (cl_uint eventIndex = 0u; eventIndex < numEventsInWaitList; eventIndex++) {
        const Event *event = castToObject<Event>(waitList[eventIndex]);
        if (event->isUserEvent()) {
            continue;
        }

        const CommandQueue *eventCommandQueue = event->getCommandQueue();
        const bool crossQueue = this != eventCommandQueue;
        const cl_command_queue_capabilities_intel createCap = crossQueue ? CL_QUEUE_CAPABILITY_CREATE_CROSS_QUEUE_EVENTS_INTEL
                                                                         : CL_QUEUE_CAPABILITY_CREATE_SINGLE_QUEUE_EVENTS_INTEL;
        const cl_command_queue_capabilities_intel waitCap = crossQueue ? CL_QUEUE_CAPABILITY_CROSS_QUEUE_EVENT_WAIT_LIST_INTEL
                                                                       : CL_QUEUE_CAPABILITY_SINGLE_QUEUE_EVENT_WAIT_LIST_INTEL;
        if (!validateCapability(waitCap) || !eventCommandQueue->validateCapability(createCap)) {
            return false;
        }
    }

    return true;
}

bool CommandQueue::validateCapabilityForOperation(cl_command_queue_capabilities_intel capability,
                                                  cl_uint numEventsInWaitList,
                                                  const cl_event *waitList,
                                                  const cl_event *outEvent) const {
    const bool operationValid = validateCapability(capability);
    const bool waitListValid = validateCapabilitiesForEventWaitList(numEventsInWaitList, waitList);
    const bool outEventValid = outEvent == nullptr ||
                               validateCapability(CL_QUEUE_CAPABILITY_CREATE_SINGLE_QUEUE_EVENTS_INTEL) ||
                               validateCapability(CL_QUEUE_CAPABILITY_CREATE_CROSS_QUEUE_EVENTS_INTEL);
    return operationValid && waitListValid && outEventValid;
}

void CommandQueue::waitForEventsFromDifferentRootDeviceIndex(cl_uint numEventsInWaitList, const cl_event *eventWaitList,
                                                             StackVec<cl_event, 8> &waitListCurrentRootDeviceIndex, bool &isEventWaitListFromPreviousRootDevice) {
    isEventWaitListFromPreviousRootDevice = false;

    for (auto &rootDeviceIndex : context->getRootDeviceIndices()) {
        CommandQueue *commandQueuePreviousRootDevice = nullptr;
        auto maxTaskCountPreviousRootDevice = 0u;

        if (this->getDevice().getRootDeviceIndex() != rootDeviceIndex) {
            for (auto eventId = 0u; eventId < numEventsInWaitList; eventId++) {
                auto event = castToObject<Event>(eventWaitList[eventId]);

                if (event->getCommandQueue() && event->getCommandQueue()->getDevice().getRootDeviceIndex() == rootDeviceIndex) {
                    maxTaskCountPreviousRootDevice = std::max(maxTaskCountPreviousRootDevice, event->peekTaskCount());
                    commandQueuePreviousRootDevice = event->getCommandQueue();
                    isEventWaitListFromPreviousRootDevice = true;
                }
            }

            if (maxTaskCountPreviousRootDevice) {
                commandQueuePreviousRootDevice->getCommandStreamReceiver(false).waitForCompletionWithTimeout(false, 0, maxTaskCountPreviousRootDevice);
            }
        }
    }

    if (isEventWaitListFromPreviousRootDevice) {
        for (auto eventId = 0u; eventId < numEventsInWaitList; eventId++) {
            auto event = castToObject<Event>(eventWaitList[eventId]);

            if (event->getCommandQueue()) {
                if (event->getCommandQueue()->getDevice().getRootDeviceIndex() == this->getDevice().getRootDeviceIndex()) {
                    waitListCurrentRootDeviceIndex.push_back(static_cast<cl_event>(eventWaitList[eventId]));
                }
            } else {
                waitListCurrentRootDeviceIndex.push_back(static_cast<cl_event>(eventWaitList[eventId]));
            }
        }
    }
}

cl_uint CommandQueue::getQueueFamilyIndex() const {
    if (isQueueFamilySelected()) {
        return queueFamilyIndex;
    } else {
        const auto &hwInfo = device->getHardwareInfo();
        const auto &hwHelper = HwHelper::get(hwInfo.platform.eRenderCoreFamily);
        const auto engineGroupType = hwHelper.getEngineGroupType(gpgpuEngine->getEngineType(), hwInfo);
        const auto familyIndex = device->getDevice().getIndexOfNonEmptyEngineGroup(engineGroupType);
        return static_cast<cl_uint>(familyIndex);
    }
}

IndirectHeap &CommandQueue::getIndirectHeap(IndirectHeap::Type heapType, size_t minRequiredSize) {
    return getGpgpuCommandStreamReceiver().getIndirectHeap(heapType, minRequiredSize);
}

void CommandQueue::allocateHeapMemory(IndirectHeap::Type heapType, size_t minRequiredSize, IndirectHeap *&indirectHeap) {
    getGpgpuCommandStreamReceiver().allocateHeapMemory(heapType, minRequiredSize, indirectHeap);
}

void CommandQueue::releaseIndirectHeap(IndirectHeap::Type heapType) {
    getGpgpuCommandStreamReceiver().releaseIndirectHeap(heapType);
}

void CommandQueue::obtainNewTimestampPacketNodes(size_t numberOfNodes, TimestampPacketContainer &previousNodes, bool clearAllDependencies, bool blitEnqueue) {
    auto allocator = blitEnqueue ? getBcsCommandStreamReceiver()->getTimestampPacketAllocator()
                                 : getGpgpuCommandStreamReceiver().getTimestampPacketAllocator();

    previousNodes.swapNodes(*timestampPacketContainer);

    if ((previousNodes.peekNodes().size() > 0) && (previousNodes.peekNodes()[0]->getAllocator() != allocator)) {
        clearAllDependencies = false;
    }

    previousNodes.resolveDependencies(clearAllDependencies);

    DEBUG_BREAK_IF(timestampPacketContainer->peekNodes().size() > 0);

    for (size_t i = 0; i < numberOfNodes; i++) {
        timestampPacketContainer->add(allocator->getTag());
    }
}

size_t CommandQueue::estimateTimestampPacketNodesCount(const MultiDispatchInfo &dispatchInfo) const {
    size_t nodesCount = dispatchInfo.size();
    auto mainKernel = dispatchInfo.peekMainKernel();
    if (obtainTimestampPacketForCacheFlush(mainKernel->requiresCacheFlushCommand(*this))) {
        nodesCount++;
    }
    return nodesCount;
}

bool CommandQueue::bufferCpuCopyAllowed(Buffer *buffer, cl_command_type commandType, cl_bool blocking, size_t size, void *ptr,
                                        cl_uint numEventsInWaitList, const cl_event *eventWaitList) {

    auto debugVariableSet = false;
    // Requested by debug variable or allowed by Buffer
    if (CL_COMMAND_READ_BUFFER == commandType && DebugManager.flags.DoCpuCopyOnReadBuffer.get() != -1) {
        if (DebugManager.flags.DoCpuCopyOnReadBuffer.get() == 0) {
            return false;
        }
        debugVariableSet = true;
    }
    if (CL_COMMAND_WRITE_BUFFER == commandType && DebugManager.flags.DoCpuCopyOnWriteBuffer.get() != -1) {
        if (DebugManager.flags.DoCpuCopyOnWriteBuffer.get() == 0) {
            return false;
        }
        debugVariableSet = true;
    }

    //if we are blocked by user events, we can't service the call on CPU
    if (Event::checkUserEventDependencies(numEventsInWaitList, eventWaitList)) {
        return false;
    }

    //check if buffer is compatible
    if (!buffer->isReadWriteOnCpuAllowed(device->getDevice())) {
        return false;
    }

    if (buffer->getMemoryManager() && buffer->getMemoryManager()->isCpuCopyRequired(ptr)) {
        return true;
    }

    if (debugVariableSet) {
        return true;
    }

    //non blocking transfers are not expected to be serviced by CPU
    //we do not want to artifically stall the pipeline to allow CPU access
    if (blocking == CL_FALSE) {
        return false;
    }

    //check if it is beneficial to do transfer on CPU
    if (!buffer->isReadWriteOnCpuPreferred(ptr, size, getDevice())) {
        return false;
    }

    //make sure that event wait list is empty
    if (numEventsInWaitList == 0) {
        return true;
    }

    return false;
}

bool CommandQueue::queueDependenciesClearRequired() const {
    return isOOQEnabled() || DebugManager.flags.OmitTimestampPacketDependencies.get();
}

bool CommandQueue::blitEnqueueAllowed(cl_command_type cmdType) const {
    auto blitterSupported = device->getHardwareInfo().capabilityTable.blitterOperationsSupported || this->isCopyOnly;

    bool blitEnqueueAllowed = getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled() || this->isCopyOnly;
    if (DebugManager.flags.EnableBlitterForEnqueueOperations.get() != -1) {
        blitEnqueueAllowed = DebugManager.flags.EnableBlitterForEnqueueOperations.get();
    }

    switch (cmdType) {
    case CL_COMMAND_READ_BUFFER:
    case CL_COMMAND_WRITE_BUFFER:
    case CL_COMMAND_COPY_BUFFER:
    case CL_COMMAND_READ_BUFFER_RECT:
    case CL_COMMAND_WRITE_BUFFER_RECT:
    case CL_COMMAND_COPY_BUFFER_RECT:
    case CL_COMMAND_SVM_MEMCPY:
    case CL_COMMAND_READ_IMAGE:
    case CL_COMMAND_WRITE_IMAGE:
        return blitterSupported && blitEnqueueAllowed;
    default:
        return false;
    }
}

bool CommandQueue::blitEnqueuePreferred(cl_command_type cmdType, const BuiltinOpParams &builtinOpParams) const {
    bool isLocalToLocal = false;

    if (cmdType == CL_COMMAND_COPY_BUFFER &&
        builtinOpParams.srcMemObj->getGraphicsAllocation(device->getRootDeviceIndex())->isAllocatedInLocalMemoryPool() &&
        builtinOpParams.dstMemObj->getGraphicsAllocation(device->getRootDeviceIndex())->isAllocatedInLocalMemoryPool()) {
        isLocalToLocal = true;
    }
    if (cmdType == CL_COMMAND_SVM_MEMCPY &&
        builtinOpParams.srcSvmAlloc->isAllocatedInLocalMemoryPool() &&
        builtinOpParams.dstSvmAlloc->isAllocatedInLocalMemoryPool()) {
        isLocalToLocal = true;
    }

    if (isLocalToLocal) {
        if (DebugManager.flags.PreferCopyEngineForCopyBufferToBuffer.get() != -1) {
            return static_cast<bool>(DebugManager.flags.PreferCopyEngineForCopyBufferToBuffer.get());
        }
        const auto &clHwHelper = ClHwHelper::get(device->getHardwareInfo().platform.eRenderCoreFamily);
        return clHwHelper.preferBlitterForLocalToLocalTransfers();
    }

    return true;
}

bool CommandQueue::blitEnqueueImageAllowed(const size_t *origin, const size_t *region) {
    auto blitEnqueuImageAllowed = false;

    if (DebugManager.flags.EnableBlitterForReadWriteImage.get() != -1) {
        blitEnqueuImageAllowed = DebugManager.flags.EnableBlitterForReadWriteImage.get();
        blitEnqueuImageAllowed &= (origin[0] + region[0] <= BlitterConstants::maxBlitWidth) && (origin[1] + region[1] <= BlitterConstants::maxBlitHeight);
    }

    return blitEnqueuImageAllowed;
}

bool CommandQueue::isBlockedCommandStreamRequired(uint32_t commandType, const EventsRequest &eventsRequest, bool blockedQueue) const {
    if (!blockedQueue) {
        return false;
    }

    if (isCacheFlushCommand(commandType) || !isCommandWithoutKernel(commandType)) {
        return true;
    }

    if ((CL_COMMAND_BARRIER == commandType || CL_COMMAND_MARKER == commandType) &&
        getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {

        for (size_t i = 0; i < eventsRequest.numEventsInWaitList; i++) {
            auto waitlistEvent = castToObjectOrAbort<Event>(eventsRequest.eventWaitList[i]);
            if (waitlistEvent->getTimestampPacketNodes()) {
                return true;
            }
        }
    }

    return false;
}

void CommandQueue::storeProperties(const cl_queue_properties *properties) {
    if (properties) {
        for (size_t i = 0; properties[i] != 0; i += 2) {
            propertiesVector.push_back(properties[i]);
            propertiesVector.push_back(properties[i + 1]);
        }
        propertiesVector.push_back(0);
    }
}

void CommandQueue::processProperties(const cl_queue_properties *properties) {
    if (properties != nullptr) {
        bool specificEngineSelected = false;
        cl_uint selectedQueueFamilyIndex = std::numeric_limits<uint32_t>::max();
        cl_uint selectedQueueIndex = std::numeric_limits<uint32_t>::max();

        for (auto currentProperties = properties; *currentProperties != 0; currentProperties += 2) {
            switch (*currentProperties) {
            case CL_QUEUE_FAMILY_INTEL:
                selectedQueueFamilyIndex = static_cast<cl_uint>(*(currentProperties + 1));
                specificEngineSelected = true;
                break;
            case CL_QUEUE_INDEX_INTEL:
                selectedQueueIndex = static_cast<cl_uint>(*(currentProperties + 1));
                specificEngineSelected = true;
                break;
            }
        }

        if (specificEngineSelected) {
            this->queueFamilySelected = true;
            if (getDevice().getNumAvailableDevices() == 1) {
                auto queueFamily = getDevice().getNonEmptyEngineGroup(selectedQueueFamilyIndex);
                auto engine = queueFamily->at(selectedQueueIndex);
                auto engineType = engine.getEngineType();
                this->overrideEngine(engineType);
                this->queueCapabilities = getClDevice().getDeviceInfo().queueFamilyProperties[selectedQueueFamilyIndex].capabilities;
                this->queueFamilyIndex = selectedQueueFamilyIndex;
                this->queueIndexWithinFamily = selectedQueueIndex;
            }
        }
    }
    processPropertiesExtra(properties);
}

void CommandQueue::overrideEngine(aub_stream::EngineType engineType) {
    const HardwareInfo &hwInfo = getDevice().getHardwareInfo();
    const HwHelper &hwHelper = HwHelper::get(hwInfo.platform.eRenderCoreFamily);
    const EngineGroupType engineGroupType = hwHelper.getEngineGroupType(engineType, hwInfo);
    const bool isEngineCopyOnly = hwHelper.isCopyOnlyEngineType(engineGroupType);

    if (isEngineCopyOnly) {
        bcsEngine = &device->getEngine(engineType, EngineUsage::Regular);
        timestampPacketContainer = std::make_unique<TimestampPacketContainer>();
        isCopyOnly = true;
    } else {
        gpgpuEngine = &device->getEngine(engineType, EngineUsage::Regular);
    }
}

void CommandQueue::aubCaptureHook(bool &blocking, bool &clearAllDependencies, const MultiDispatchInfo &multiDispatchInfo) {
    if (DebugManager.flags.AUBDumpSubCaptureMode.get()) {
        auto status = getGpgpuCommandStreamReceiver().checkAndActivateAubSubCapture(multiDispatchInfo);
        if (!status.isActive) {
            // make each enqueue blocking when subcapture is not active to split batch buffer
            blocking = true;
        } else if (!status.wasActiveInPreviousEnqueue) {
            // omit timestamp packet dependencies dependencies upon subcapture activation
            clearAllDependencies = true;
        }
    }

    if (getGpgpuCommandStreamReceiver().getType() > CommandStreamReceiverType::CSR_HW) {
        for (auto &dispatchInfo : multiDispatchInfo) {
            auto kernelName = dispatchInfo.getKernel()->getKernelInfo(device->getRootDeviceIndex()).kernelDescriptor.kernelMetadata.kernelName;
            getGpgpuCommandStreamReceiver().addAubComment(kernelName.c_str());
        }
    }
}

void CommandQueue::waitUntilComplete(bool blockedQueue, PrintfHandler *printfHandler) {
    if (blockedQueue) {
        while (isQueueBlocked()) {
        }
    }

    waitUntilComplete(taskCount, bcsTaskCount, flushStamp->peekStamp(), false);

    if (printfHandler) {
        printfHandler->printEnqueueOutput();
    }
}

} // namespace NEO
