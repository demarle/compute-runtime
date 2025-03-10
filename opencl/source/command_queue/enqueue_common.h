/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/built_ins/built_ins.h"
#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/helpers/array_count.h"
#include "shared/source/helpers/engine_node_helper.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/surface.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/program/sync_buffer_handler.h"
#include "shared/source/utilities/range.h"
#include "shared/source/utilities/tag_allocator.h"

#include "opencl/source/built_ins/builtins_dispatch_builder.h"
#include "opencl/source/builtin_kernels_simulation/scheduler_simulation.h"
#include "opencl/source/command_queue/command_queue_hw.h"
#include "opencl/source/command_queue/gpgpu_walker.h"
#include "opencl/source/command_queue/hardware_interface.h"
#include "opencl/source/event/event_builder.h"
#include "opencl/source/event/user_event.h"
#include "opencl/source/gtpin/gtpin_notify.h"
#include "opencl/source/helpers/cl_blit_properties.h"
#include "opencl/source/helpers/cl_hw_helper.h"
#include "opencl/source/helpers/dispatch_info_builder.h"
#include "opencl/source/helpers/enqueue_properties.h"
#include "opencl/source/helpers/hardware_commands_helper.h"
#include "opencl/source/helpers/task_information.h"
#include "opencl/source/mem_obj/buffer.h"
#include "opencl/source/mem_obj/image.h"
#include "opencl/source/program/block_kernel_manager.h"
#include "opencl/source/program/printf_handler.h"

#include "pipe_control_args.h"

#include <algorithm>
#include <new>

namespace NEO {

template <typename GfxFamily>
template <uint32_t commandType, size_t surfaceCount>
void CommandQueueHw<GfxFamily>::enqueueHandler(Surface *(&surfaces)[surfaceCount],
                                               bool blocking,
                                               Kernel *kernel,
                                               cl_uint workDim,
                                               const size_t globalOffsets[3],
                                               const size_t workItems[3],
                                               const size_t *localWorkSizesIn,
                                               const size_t *enqueuedWorkSizes,
                                               cl_uint numEventsInWaitList,
                                               const cl_event *eventWaitList,
                                               cl_event *event) {
    BuiltInOwnershipWrapper builtInLock;
    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    MultiDispatchInfo multiDispatchInfo(kernel);

    auto auxTranslationMode = AuxTranslationMode::None;

    if (DebugManager.flags.ForceDispatchScheduler.get()) {
        forceDispatchScheduler(multiDispatchInfo);
    } else {
        auto rootDeviceIndex = device->getRootDeviceIndex();

        kernel->updateAuxTranslationRequired();
        if (kernel->isAuxTranslationRequired()) {
            kernel->fillWithKernelObjsForAuxTranslation(kernelObjsForAuxTranslation, rootDeviceIndex);
            multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);

            if (!kernelObjsForAuxTranslation.empty()) {
                auxTranslationMode = HwHelperHw<GfxFamily>::get().getAuxTranslationMode(device->getHardwareInfo());
            }
        }

        if (AuxTranslationMode::Builtin == auxTranslationMode) {
            auto &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::AuxTranslation, getClDevice());
            builtInLock.takeOwnership(builder);

            dispatchAuxTranslationBuiltin(multiDispatchInfo, AuxTranslationDirection::AuxToNonAux);
        }

        if (kernel->getKernelInfo(rootDeviceIndex).builtinDispatchBuilder == nullptr) {
            DispatchInfoBuilder<SplitDispatch::Dim::d3D, SplitDispatch::SplitMode::WalkerSplit> builder(getClDevice());
            builder.setDispatchGeometry(workDim, workItems, enqueuedWorkSizes, globalOffsets, Vec3<size_t>{0, 0, 0}, localWorkSizesIn);
            builder.setKernel(kernel);
            builder.bake(multiDispatchInfo);
        } else {
            auto builder = kernel->getKernelInfo(rootDeviceIndex).builtinDispatchBuilder;
            builder->buildDispatchInfos(multiDispatchInfo, kernel, workDim, workItems, enqueuedWorkSizes, globalOffsets);

            if (multiDispatchInfo.size() == 0) {
                return;
            }
        }

        if (AuxTranslationMode::Builtin == auxTranslationMode) {
            UNRECOVERABLE_IF(kernel->isParentKernel);
            dispatchAuxTranslationBuiltin(multiDispatchInfo, AuxTranslationDirection::NonAuxToAux);
        }
    }

    if (AuxTranslationMode::Blit == auxTranslationMode) {
        setupBlitAuxTranslation(multiDispatchInfo);
    }

    enqueueHandler<commandType>(surfaces, blocking, multiDispatchInfo, numEventsInWaitList, eventWaitList, event);
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::forceDispatchScheduler(NEO::MultiDispatchInfo &multiDispatchInfo) {
    SchedulerKernel &scheduler = getContext().getSchedulerKernel();

    auto devQueue = this->getContext().getDefaultDeviceQueue();
    DeviceQueueHw<GfxFamily> *devQueueHw = castToObjectOrAbort<DeviceQueueHw<GfxFamily>>(devQueue);

    DispatchInfo dispatchInfo(devQueue->getClDevice(), &scheduler, 1, Vec3<size_t>(scheduler.getGws(), 1, 1), Vec3<size_t>(scheduler.getLws(), 1, 1), Vec3<size_t>(0, 0, 0));
    Vec3<size_t> workGroupCount = generateWorkgroupsNumber(dispatchInfo.getGWS(), dispatchInfo.getEnqueuedWorkgroupSize());
    dispatchInfo.setTotalNumberOfWorkgroups(workGroupCount);
    dispatchInfo.setNumberOfWorkgroups(workGroupCount);

    scheduler.createReflectionSurface();
    GraphicsAllocation *reflectionSurface = scheduler.getKernelReflectionSurface();

    devQueueHw->resetDeviceQueue();

    scheduler.setArgs(devQueueHw->getQueueBuffer(),
                      devQueueHw->getStackBuffer(),
                      devQueueHw->getEventPoolBuffer(),
                      devQueueHw->getSlbBuffer(),
                      devQueueHw->getDshBuffer(),
                      reflectionSurface,
                      devQueueHw->getQueueStorageBuffer(),
                      this->getIndirectHeap(IndirectHeap::SURFACE_STATE, 0u).getGraphicsAllocation());

    multiDispatchInfo.push(dispatchInfo);
}

template <typename GfxFamily>
template <uint32_t commandType>
void CommandQueueHw<GfxFamily>::enqueueHandler(Surface **surfacesForResidency,
                                               size_t numSurfaceForResidency,
                                               bool blocking,
                                               const MultiDispatchInfo &multiDispatchInfo,
                                               cl_uint numEventsInWaitList,
                                               const cl_event *eventWaitList,
                                               cl_event *event) {
    if (multiDispatchInfo.empty() && !isCommandWithoutKernel(commandType)) {
        enqueueHandler<CL_COMMAND_MARKER>(surfacesForResidency, numSurfaceForResidency, blocking, multiDispatchInfo,
                                          numEventsInWaitList, eventWaitList, event);
        if (event) {
            castToObjectOrAbort<Event>(*event)->setCmdType(commandType);
        }
        return;
    }

    StackVec<cl_event, 8> waitListCurrentRootDeviceIndex;
    bool isEventWaitListFromPreviousRootDevice = false;

    if (context->getRootDeviceIndices().size() > 1u) {
        waitForEventsFromDifferentRootDeviceIndex(numEventsInWaitList, eventWaitList, waitListCurrentRootDeviceIndex, isEventWaitListFromPreviousRootDevice);
    }

    const cl_event *eventWaitListCurrentRootDevice = isEventWaitListFromPreviousRootDevice ? waitListCurrentRootDeviceIndex.data() : eventWaitList;
    cl_uint numEventsInWaitListCurrentRootDevice = isEventWaitListFromPreviousRootDevice ? static_cast<cl_uint>(waitListCurrentRootDeviceIndex.size()) : numEventsInWaitList;

    Kernel *parentKernel = multiDispatchInfo.peekParentKernel();
    auto devQueue = this->getContext().getDefaultDeviceQueue();
    DeviceQueueHw<GfxFamily> *devQueueHw = castToObject<DeviceQueueHw<GfxFamily>>(devQueue);
    auto clearAllDependencies = queueDependenciesClearRequired();

    TagNode<HwTimeStamps> *hwTimeStamps = nullptr;

    auto commandStreamRecieverOwnership = getGpgpuCommandStreamReceiver().obtainUniqueOwnership();

    EventBuilder eventBuilder;
    setupEvent(eventBuilder, event, commandType);

    std::unique_ptr<KernelOperation> blockedCommandsData;
    std::unique_ptr<PrintfHandler> printfHandler;
    TakeOwnershipWrapper<CommandQueueHw<GfxFamily>> queueOwnership(*this);

    auto blockQueue = false;
    auto taskLevel = 0u;
    obtainTaskLevelAndBlockedStatus(taskLevel, numEventsInWaitListCurrentRootDevice, eventWaitListCurrentRootDevice, blockQueue, commandType);

    if (parentKernel && !blockQueue) {
        while (!devQueueHw->isEMCriticalSectionFree())
            ;
    }

    enqueueHandlerHook(commandType, multiDispatchInfo);

    aubCaptureHook(blocking, clearAllDependencies, multiDispatchInfo);

    if (DebugManager.flags.MakeEachEnqueueBlocking.get()) {
        blocking = true;
    }

    TimestampPacketDependencies timestampPacketDependencies;
    EventsRequest eventsRequest(numEventsInWaitListCurrentRootDevice, eventWaitListCurrentRootDevice, event);
    CsrDependencies csrDeps;
    BlitPropertiesContainer blitPropertiesContainer;

    bool enqueueWithBlitAuxTranslation = isBlitAuxTranslationRequired(multiDispatchInfo);

    if (getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {
        eventsRequest.fillCsrDependencies(csrDeps, getGpgpuCommandStreamReceiver(), CsrDependencies::DependenciesType::OnCsr);
        auto allocator = getGpgpuCommandStreamReceiver().getTimestampPacketAllocator();

        size_t nodesCount = 0u;
        if (isCacheFlushCommand(commandType)) {
            nodesCount = 1;
        } else if (!multiDispatchInfo.empty()) {
            nodesCount = estimateTimestampPacketNodesCount(multiDispatchInfo);
        }

        if (isCacheFlushForBcsRequired() && enqueueWithBlitAuxTranslation) {
            // Cache flush for aux translation is always required (if supported)
            timestampPacketDependencies.cacheFlushNodes.add(allocator->getTag());
        }

        if (nodesCount > 0) {
            obtainNewTimestampPacketNodes(nodesCount, timestampPacketDependencies.previousEnqueueNodes, clearAllDependencies, false);
            csrDeps.push_back(&timestampPacketDependencies.previousEnqueueNodes);
        }
    }

    auto &commandStream = *obtainCommandStream<commandType>(csrDeps, false, blockQueue, multiDispatchInfo, eventsRequest,
                                                            blockedCommandsData, surfacesForResidency, numSurfaceForResidency);
    auto commandStreamStart = commandStream.getUsed();

    if (enqueueWithBlitAuxTranslation) {
        processDispatchForBlitAuxTranslation(multiDispatchInfo, blitPropertiesContainer, timestampPacketDependencies,
                                             eventsRequest, blockQueue);
    }

    if (eventBuilder.getEvent() && getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {
        eventBuilder.getEvent()->addTimestampPacketNodes(*timestampPacketContainer);
        eventBuilder.getEvent()->addTimestampPacketNodes(timestampPacketDependencies.nonAuxToAuxNodes);
        eventBuilder.getEvent()->addTimestampPacketNodes(timestampPacketDependencies.auxToNonAuxNodes);
    }

    bool flushDependenciesForNonKernelCommand = false;

    if (multiDispatchInfo.empty() == false) {
        processDispatchForKernels<commandType>(multiDispatchInfo, printfHandler, eventBuilder.getEvent(),
                                               hwTimeStamps, blockQueue, devQueueHw, csrDeps, blockedCommandsData.get(),
                                               timestampPacketDependencies);
    } else if (isCacheFlushCommand(commandType)) {
        processDispatchForCacheFlush(surfacesForResidency, numSurfaceForResidency, &commandStream, csrDeps);
    } else if (getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {
        if (CL_COMMAND_BARRIER == commandType) {
            getGpgpuCommandStreamReceiver().requestStallingPipeControlOnNextFlush();
        }

        for (size_t i = 0; i < eventsRequest.numEventsInWaitList; i++) {
            auto waitlistEvent = castToObjectOrAbort<Event>(eventsRequest.eventWaitList[i]);
            if (waitlistEvent->getTimestampPacketNodes()) {
                flushDependenciesForNonKernelCommand = true;
                if (eventBuilder.getEvent()) {
                    eventBuilder.getEvent()->addTimestampPacketNodes(*waitlistEvent->getTimestampPacketNodes());
                }
            }
        }
        if (flushDependenciesForNonKernelCommand) {
            TimestampPacketHelper::programCsrDependencies<GfxFamily>(commandStream, csrDeps, getGpgpuCommandStreamReceiver().getOsContext().getNumSupportedDevices());
        }
    }

    CompletionStamp completionStamp = {CompletionStamp::notReady, taskLevel, 0};

    const EnqueueProperties enqueueProperties(false, !multiDispatchInfo.empty(), isCacheFlushCommand(commandType),
                                              flushDependenciesForNonKernelCommand, &blitPropertiesContainer);

    if (!blockQueue) {
        csrDeps.makeResident(getGpgpuCommandStreamReceiver());

        if (parentKernel) {
            processDeviceEnqueue(devQueueHw, multiDispatchInfo, hwTimeStamps, blocking);
        }

        if (enqueueProperties.operation == EnqueueProperties::Operation::GpuKernel) {
            completionStamp = enqueueNonBlocked<commandType>(
                surfacesForResidency,
                numSurfaceForResidency,
                commandStream,
                commandStreamStart,
                blocking,
                multiDispatchInfo,
                enqueueProperties,
                timestampPacketDependencies,
                eventsRequest,
                eventBuilder,
                taskLevel,
                printfHandler.get());

            if (parentKernel) {
                getGpgpuCommandStreamReceiver().setMediaVFEStateDirty(true);

                if (devQueueHw->getSchedulerReturnInstance() > 0) {
                    waitUntilComplete(completionStamp.taskCount, bcsTaskCount, completionStamp.flushStamp, false);
                    this->runSchedulerSimulation(*devQueueHw, *parentKernel);
                }
            }
        } else if (enqueueProperties.isFlushWithoutKernelRequired()) {
            completionStamp = enqueueCommandWithoutKernel(
                surfacesForResidency,
                numSurfaceForResidency,
                &commandStream,
                commandStreamStart,
                blocking,
                enqueueProperties,
                timestampPacketDependencies,
                eventsRequest,
                eventBuilder,
                taskLevel);
        } else {
            UNRECOVERABLE_IF(enqueueProperties.operation != EnqueueProperties::Operation::EnqueueWithoutSubmission);

            auto maxTaskCountCurrentRootDevice = this->taskCount;

            for (auto eventId = 0u; eventId < numEventsInWaitListCurrentRootDevice; eventId++) {
                auto event = castToObject<Event>(eventWaitListCurrentRootDevice[eventId]);

                if (!event->isUserEvent() && !event->isExternallySynchronized()) {
                    maxTaskCountCurrentRootDevice = std::max(maxTaskCountCurrentRootDevice, event->peekTaskCount());
                }
            }

            //inherit data from event_wait_list and previous packets
            completionStamp.flushStamp = this->flushStamp->peekStamp();
            completionStamp.taskCount = maxTaskCountCurrentRootDevice;
            completionStamp.taskLevel = taskLevel;

            if (eventBuilder.getEvent() && isProfilingEnabled()) {
                TimeStampData submitTimeStamp;
                this->getDevice().getOSTime()->getCpuGpuTime(&submitTimeStamp);
                eventBuilder.getEvent()->setSubmitTimeStamp(&submitTimeStamp);
                eventBuilder.getEvent()->setSubmitTimeStamp();
                eventBuilder.getEvent()->setStartTimeStamp();
            }
        }
        if (eventBuilder.getEvent()) {
            eventBuilder.getEvent()->flushStamp->replaceStampObject(this->flushStamp->getStampReference());
        }

        this->latestSentEnqueueType = enqueueProperties.operation;
    }
    updateFromCompletionStamp(completionStamp, eventBuilder.getEvent());

    if (blockQueue) {
        if (parentKernel) {
            size_t minSizeSSHForEM = HardwareCommandsHelper<GfxFamily>::getSshSizeForExecutionModel(*parentKernel, device->getRootDeviceIndex());
            blockedCommandsData->surfaceStateHeapSizeEM = minSizeSSHForEM;
        }

        enqueueBlocked(commandType,
                       surfacesForResidency,
                       numSurfaceForResidency,
                       multiDispatchInfo,
                       timestampPacketDependencies,
                       blockedCommandsData,
                       enqueueProperties,
                       eventsRequest,
                       eventBuilder,
                       std::move(printfHandler));
    }

    queueOwnership.unlock();
    commandStreamRecieverOwnership.unlock();

    if (blocking) {
        waitUntilComplete(blockQueue, printfHandler.get());
    }
}

template <typename GfxFamily>
template <uint32_t commandType>
void CommandQueueHw<GfxFamily>::processDispatchForKernels(const MultiDispatchInfo &multiDispatchInfo,
                                                          std::unique_ptr<PrintfHandler> &printfHandler,
                                                          Event *event,
                                                          TagNode<HwTimeStamps> *&hwTimeStamps,
                                                          bool blockQueue,
                                                          DeviceQueueHw<GfxFamily> *devQueueHw,
                                                          CsrDependencies &csrDeps,
                                                          KernelOperation *blockedCommandsData,
                                                          TimestampPacketDependencies &timestampPacketDependencies) {
    TagNode<HwPerfCounter> *hwPerfCounter = nullptr;
    FileLoggerInstance().dumpKernelArgs(&multiDispatchInfo);

    printfHandler.reset(PrintfHandler::create(multiDispatchInfo, *device));
    if (printfHandler) {
        printfHandler->prepareDispatch(multiDispatchInfo);
    }

    if (multiDispatchInfo.peekMainKernel()->usesSyncBuffer(device->getRootDeviceIndex())) {
        auto &gws = multiDispatchInfo.begin()->getGWS();
        auto &lws = multiDispatchInfo.begin()->getLocalWorkgroupSize();
        size_t workGroupsCount = (gws.x * gws.y * gws.z) /
                                 (lws.x * lws.y * lws.z);
        device->syncBufferHandler->prepareForEnqueue(workGroupsCount, *multiDispatchInfo.peekMainKernel());
    }

    if (commandType == CL_COMMAND_NDRANGE_KERNEL) {
        if (multiDispatchInfo.peekMainKernel()->isKernelDebugEnabled()) {
            setupDebugSurface(multiDispatchInfo.peekMainKernel());
        }
    }

    if (event && this->isProfilingEnabled()) {
        // Get allocation for timestamps
        hwTimeStamps = event->getHwTimeStampNode();
    }

    if (auto parentKernel = multiDispatchInfo.peekParentKernel()) {
        parentKernel->createReflectionSurface();
        parentKernel->patchDefaultDeviceQueue(context->getDefaultDeviceQueue());
        parentKernel->patchEventPool(context->getDefaultDeviceQueue());
        parentKernel->patchReflectionSurface(context->getDefaultDeviceQueue(), printfHandler.get());
        if (!blockQueue) {
            devQueueHw->resetDeviceQueue();
            devQueueHw->acquireEMCriticalSection();
        }
    }

    if (event && this->isPerfCountersEnabled()) {
        hwPerfCounter = event->getHwPerfCounterNode();
    }

    HardwareInterface<GfxFamily>::dispatchWalker(
        *this,
        multiDispatchInfo,
        csrDeps,
        blockedCommandsData,
        hwTimeStamps,
        hwPerfCounter,
        &timestampPacketDependencies,
        timestampPacketContainer.get(),
        commandType);

    if (DebugManager.flags.AddPatchInfoCommentsForAUBDump.get()) {
        for (auto &dispatchInfo : multiDispatchInfo) {
            for (auto &patchInfoData : dispatchInfo.getKernel()->getPatchInfoDataList()) {
                getGpgpuCommandStreamReceiver().getFlatBatchBufferHelper().setPatchInfoData(patchInfoData);
            }
        }
    }

    getGpgpuCommandStreamReceiver().setRequiredScratchSizes(multiDispatchInfo.getRequiredScratchSize(), multiDispatchInfo.getRequiredPrivateScratchSize());
}

template <typename GfxFamily>
BlitProperties CommandQueueHw<GfxFamily>::processDispatchForBlitEnqueue(const MultiDispatchInfo &multiDispatchInfo,
                                                                        TimestampPacketDependencies &timestampPacketDependencies,
                                                                        const EventsRequest &eventsRequest, LinearStream *commandStream,
                                                                        uint32_t commandType, bool queueBlocked) {
    auto blitDirection = ClBlitProperties::obtainBlitDirection(commandType);

    auto blitCommandStreamReceiver = getBcsCommandStreamReceiver();

    auto blitProperties = ClBlitProperties::constructProperties(blitDirection, *blitCommandStreamReceiver,
                                                                multiDispatchInfo.peekBuiltinOpParams());
    if (!queueBlocked) {
        eventsRequest.fillCsrDependencies(blitProperties.csrDependencies, *blitCommandStreamReceiver,
                                          CsrDependencies::DependenciesType::All);

        blitProperties.csrDependencies.push_back(&timestampPacketDependencies.cacheFlushNodes);
        blitProperties.csrDependencies.push_back(&timestampPacketDependencies.previousEnqueueNodes);
        blitProperties.csrDependencies.push_back(&timestampPacketDependencies.barrierNodes);
    }

    auto currentTimestampPacketNode = timestampPacketContainer->peekNodes().at(0);
    blitProperties.outputTimestampPacket = currentTimestampPacketNode;

    if (commandStream) {
        if (timestampPacketDependencies.cacheFlushNodes.peekNodes().size() > 0) {
            auto cacheFlushTimestampPacketGpuAddress = TimestampPacketHelper::getContextEndGpuAddress(*timestampPacketDependencies.cacheFlushNodes.peekNodes()[0]);
            PipeControlArgs args(true);
            MemorySynchronizationCommands<GfxFamily>::addPipeControlAndProgramPostSyncOperation(
                *commandStream,
                GfxFamily::PIPE_CONTROL::POST_SYNC_OPERATION::POST_SYNC_OPERATION_WRITE_IMMEDIATE_DATA,
                cacheFlushTimestampPacketGpuAddress,
                0,
                device->getHardwareInfo(),
                args);
        }

        TimestampPacketHelper::programSemaphoreWithImplicitDependency<GfxFamily>(*commandStream, *currentTimestampPacketNode,
                                                                                 getGpgpuCommandStreamReceiver().getOsContext().getNumSupportedDevices());
    }
    return blitProperties;
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::processDispatchForBlitAuxTranslation(const MultiDispatchInfo &multiDispatchInfo,
                                                                     BlitPropertiesContainer &blitPropertiesContainer,
                                                                     TimestampPacketDependencies &timestampPacketDependencies,
                                                                     const EventsRequest &eventsRequest, bool queueBlocked) {
    auto rootDeviceIndex = getDevice().getRootDeviceIndex();
    auto nodesAllocator = getGpgpuCommandStreamReceiver().getTimestampPacketAllocator();
    auto numKernelObjs = multiDispatchInfo.getKernelObjsForAuxTranslation()->size();
    blitPropertiesContainer.resize(numKernelObjs * 2);

    auto bufferIndex = 0;
    for (auto &kernelObj : *multiDispatchInfo.getKernelObjsForAuxTranslation()) {
        GraphicsAllocation *allocation = nullptr;
        if (kernelObj.type == KernelObjForAuxTranslation::Type::MEM_OBJ) {
            auto buffer = static_cast<Buffer *>(kernelObj.object);
            allocation = buffer->getGraphicsAllocation(rootDeviceIndex);
        } else {
            DEBUG_BREAK_IF(kernelObj.type != KernelObjForAuxTranslation::Type::GFX_ALLOC);
            allocation = static_cast<GraphicsAllocation *>(kernelObj.object);
        }
        {
            // Aux to NonAux
            blitPropertiesContainer[bufferIndex] = BlitProperties::constructPropertiesForAuxTranslation(
                AuxTranslationDirection::AuxToNonAux, allocation, getGpgpuCommandStreamReceiver().getClearColorAllocation());
            auto auxToNonAuxNode = nodesAllocator->getTag();
            timestampPacketDependencies.auxToNonAuxNodes.add(auxToNonAuxNode);
        }

        {
            // NonAux to Aux
            blitPropertiesContainer[bufferIndex + numKernelObjs] = BlitProperties::constructPropertiesForAuxTranslation(
                AuxTranslationDirection::NonAuxToAux, allocation, getGpgpuCommandStreamReceiver().getClearColorAllocation());
            auto nonAuxToAuxNode = nodesAllocator->getTag();
            timestampPacketDependencies.nonAuxToAuxNodes.add(nonAuxToAuxNode);
        }
        bufferIndex++;
    }

    if (!queueBlocked) {
        CsrDependencies csrDeps;
        eventsRequest.fillCsrDependencies(csrDeps, *getBcsCommandStreamReceiver(), CsrDependencies::DependenciesType::All);
        BlitProperties::setupDependenciesForAuxTranslation(blitPropertiesContainer, timestampPacketDependencies,
                                                           *this->timestampPacketContainer, csrDeps,
                                                           getGpgpuCommandStreamReceiver(), *getBcsCommandStreamReceiver());
    }
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::processDispatchForCacheFlush(Surface **surfaces,
                                                             size_t numSurfaces,
                                                             LinearStream *commandStream,
                                                             CsrDependencies &csrDeps) {

    TimestampPacketHelper::programCsrDependencies<GfxFamily>(*commandStream, csrDeps, getGpgpuCommandStreamReceiver().getOsContext().getNumSupportedDevices());

    uint64_t postSyncAddress = 0;
    if (getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {
        auto timestampPacketNodeForPostSync = timestampPacketContainer->peekNodes().at(0);
        timestampPacketNodeForPostSync->setProfilingCapable(false);
        postSyncAddress = TimestampPacketHelper::getContextEndGpuAddress(*timestampPacketNodeForPostSync);
    }

    submitCacheFlush(surfaces, numSurfaces, commandStream, postSyncAddress);
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::processDeviceEnqueue(DeviceQueueHw<GfxFamily> *devQueueHw,
                                                     const MultiDispatchInfo &multiDispatchInfo,
                                                     TagNode<HwTimeStamps> *hwTimeStamps,
                                                     bool &blocking) {
    auto parentKernel = multiDispatchInfo.peekParentKernel();
    auto rootDeviceIndex = devQueueHw->getDevice().getRootDeviceIndex();
    size_t minSizeSSHForEM = HardwareCommandsHelper<GfxFamily>::getSshSizeForExecutionModel(*parentKernel, rootDeviceIndex);
    bool isCcsUsed = EngineHelpers::isCcs(gpgpuEngine->osContext->getEngineType());

    uint32_t taskCount = getGpgpuCommandStreamReceiver().peekTaskCount() + 1;
    devQueueHw->setupExecutionModelDispatch(getIndirectHeap(IndirectHeap::SURFACE_STATE, minSizeSSHForEM),
                                            *devQueueHw->getIndirectHeap(IndirectHeap::DYNAMIC_STATE),
                                            parentKernel,
                                            (uint32_t)multiDispatchInfo.size(),
                                            getGpgpuCommandStreamReceiver().getTagAllocation()->getGpuAddress(),
                                            taskCount,
                                            hwTimeStamps,
                                            isCcsUsed);

    SchedulerKernel &scheduler = getContext().getSchedulerKernel();

    scheduler.setArgs(devQueueHw->getQueueBuffer(),
                      devQueueHw->getStackBuffer(),
                      devQueueHw->getEventPoolBuffer(),
                      devQueueHw->getSlbBuffer(),
                      devQueueHw->getDshBuffer(),
                      parentKernel->getKernelReflectionSurface(),
                      devQueueHw->getQueueStorageBuffer(),
                      this->getIndirectHeap(IndirectHeap::SURFACE_STATE, 0u).getGraphicsAllocation(),
                      devQueueHw->getDebugQueue());

    auto preemptionMode = PreemptionHelper::taskPreemptionMode(getDevice(), multiDispatchInfo);
    GpgpuWalkerHelper<GfxFamily>::dispatchScheduler(
        *this->commandStream,
        *devQueueHw,
        preemptionMode,
        scheduler,
        &getIndirectHeap(IndirectHeap::SURFACE_STATE, 0u),
        devQueueHw->getIndirectHeap(IndirectHeap::DYNAMIC_STATE),
        isCcsUsed);

    scheduler.makeResident(getGpgpuCommandStreamReceiver());

    parentKernel->getProgram()->getBlockKernelManager()->makeInternalAllocationsResident(getGpgpuCommandStreamReceiver());

    if (parentKernel->isAuxTranslationRequired()) {
        blocking = true;
    }
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::obtainTaskLevelAndBlockedStatus(unsigned int &taskLevel, cl_uint &numEventsInWaitList, const cl_event *&eventWaitList, bool &blockQueueStatus, unsigned int commandType) {
    auto isQueueBlockedStatus = isQueueBlocked();
    taskLevel = getTaskLevelFromWaitList(this->taskLevel, numEventsInWaitList, eventWaitList);
    blockQueueStatus = (taskLevel == CompletionStamp::notReady) || isQueueBlockedStatus;

    auto taskLevelUpdateRequired = isTaskLevelUpdateRequired(taskLevel, eventWaitList, numEventsInWaitList, commandType);
    if (taskLevelUpdateRequired) {
        taskLevel++;
        this->taskLevel = taskLevel;
    }

    DBG_LOG(EventsDebugEnable, "blockQueue", blockQueueStatus, "virtualEvent", virtualEvent, "taskLevel", taskLevel);
}

template <typename GfxFamily>
bool CommandQueueHw<GfxFamily>::isTaskLevelUpdateRequired(const uint32_t &taskLevel, const cl_event *eventWaitList, const cl_uint &numEventsInWaitList, unsigned int commandType) {
    bool updateTaskLevel = true;
    //if we are blocked by user event then no update
    if (taskLevel == CompletionStamp::notReady) {
        updateTaskLevel = false;
    }
    //if we are executing command without kernel then it will inherit state from
    //previous commands, barrier is exception
    if (isCommandWithoutKernel(commandType) && commandType != CL_COMMAND_BARRIER) {
        updateTaskLevel = false;
    }
    //ooq special cases starts here
    if (this->isOOQEnabled()) {
        //if no wait list and barrier , do not update task level
        if (eventWaitList == nullptr && commandType != CL_COMMAND_BARRIER) {
            updateTaskLevel = false;
        }
        //if we have waitlist then deduce task level from waitlist and check if it is higher then current task level of queue
        if (eventWaitList != nullptr) {
            auto taskLevelFromEvents = getTaskLevelFromWaitList(0, numEventsInWaitList, eventWaitList);
            taskLevelFromEvents++;
            if (taskLevelFromEvents <= this->taskLevel) {
                updateTaskLevel = false;
            }
        }
    }
    return updateTaskLevel;
}

template <typename GfxFamily>
template <uint32_t commandType>
CompletionStamp CommandQueueHw<GfxFamily>::enqueueNonBlocked(
    Surface **surfaces,
    size_t surfaceCount,
    LinearStream &commandStream,
    size_t commandStreamStart,
    bool &blocking,
    const MultiDispatchInfo &multiDispatchInfo,
    const EnqueueProperties &enqueueProperties,
    TimestampPacketDependencies &timestampPacketDependencies,
    EventsRequest &eventsRequest,
    EventBuilder &eventBuilder,
    uint32_t taskLevel,
    PrintfHandler *printfHandler) {

    UNRECOVERABLE_IF(multiDispatchInfo.empty());

    auto implicitFlush = false;

    if (printfHandler) {
        blocking = true;
        printfHandler->makeResident(getGpgpuCommandStreamReceiver());
    }

    auto rootDeviceIndex = device->getRootDeviceIndex();
    if (multiDispatchInfo.peekMainKernel()->usesSyncBuffer(rootDeviceIndex)) {
        device->syncBufferHandler->makeResident(getGpgpuCommandStreamReceiver());
    }

    if (timestampPacketContainer) {
        timestampPacketContainer->makeResident(getGpgpuCommandStreamReceiver());
        timestampPacketDependencies.previousEnqueueNodes.makeResident(getGpgpuCommandStreamReceiver());
        timestampPacketDependencies.cacheFlushNodes.makeResident(getGpgpuCommandStreamReceiver());
    }

    bool anyUncacheableArgs = false;
    auto requiresCoherency = false;
    for (auto surface : CreateRange(surfaces, surfaceCount)) {
        surface->makeResident(getGpgpuCommandStreamReceiver());
        requiresCoherency |= surface->IsCoherent;
        if (!surface->allowsL3Caching()) {
            anyUncacheableArgs = true;
        }
    }

    auto mediaSamplerRequired = false;
    uint32_t numGrfRequired = GrfConfig::DefaultGrfNumber;
    auto specialPipelineSelectMode = false;
    Kernel *kernel = nullptr;
    bool usePerDssBackedBuffer = false;
    bool auxTranslationRequired = false;
    bool useGlobalAtomics = false;

    for (auto &dispatchInfo : multiDispatchInfo) {
        if (kernel != dispatchInfo.getKernel()) {
            kernel = dispatchInfo.getKernel();
        } else {
            continue;
        }
        kernel->makeResident(getGpgpuCommandStreamReceiver());
        requiresCoherency |= kernel->requiresCoherency();
        mediaSamplerRequired |= kernel->isVmeKernel();
        auto numGrfRequiredByKernel = static_cast<uint32_t>(kernel->getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelAttributes.numGrfRequired);
        numGrfRequired = std::max(numGrfRequired, numGrfRequiredByKernel);
        specialPipelineSelectMode |= kernel->requiresSpecialPipelineSelectMode();
        auxTranslationRequired |= kernel->isAuxTranslationRequired();
        if (kernel->hasUncacheableStatelessArgs()) {
            anyUncacheableArgs = true;
        }

        if (kernel->requiresPerDssBackedBuffer(rootDeviceIndex)) {
            usePerDssBackedBuffer = true;
        }

        if (kernel->getDefaultKernelInfo().kernelDescriptor.kernelAttributes.flags.useGlobalAtomics) {
            useGlobalAtomics = true;
        }
    }

    if (mediaSamplerRequired) {
        DEBUG_BREAK_IF(device->getDeviceInfo().preemptionSupported != false);
    }

    TimeStampData submitTimeStamp = {};
    if (isProfilingEnabled() && eventBuilder.getEvent()) {
        this->getDevice().getOSTime()->getCpuTime(&submitTimeStamp.CPUTimeinNS);
        eventBuilder.getEvent()->setSubmitTimeStamp(&submitTimeStamp);

        auto hwTimestampNode = eventBuilder.getEvent()->getHwTimeStampNode();
        if (hwTimestampNode) {
            getGpgpuCommandStreamReceiver().makeResident(*hwTimestampNode->getBaseGraphicsAllocation());
        }

        if (isPerfCountersEnabled()) {
            getGpgpuCommandStreamReceiver().makeResident(*eventBuilder.getEvent()->getHwPerfCounterNode()->getBaseGraphicsAllocation());
        }
    }

    IndirectHeap *dsh = nullptr;
    IndirectHeap *ioh = nullptr;

    if (multiDispatchInfo.peekParentKernel()) {
        DeviceQueueHw<GfxFamily> *pDevQueue = castToObject<DeviceQueueHw<GfxFamily>>(this->getContext().getDefaultDeviceQueue());
        DEBUG_BREAK_IF(pDevQueue == nullptr);
        dsh = pDevQueue->getIndirectHeap(IndirectHeap::DYNAMIC_STATE);
        // In ExecutionModel IOH is the same as DSH to eliminate StateBaseAddress reprogramming for scheduler kernel and blocks.
        ioh = dsh;
        implicitFlush = true;
    } else {
        dsh = &getIndirectHeap(IndirectHeap::DYNAMIC_STATE, 0u);
        ioh = &getIndirectHeap(IndirectHeap::INDIRECT_OBJECT, 0u);
    }

    auto allocNeedsFlushDC = false;
    if (!device->isFullRangeSvm()) {
        if (std::any_of(getGpgpuCommandStreamReceiver().getResidencyAllocations().begin(), getGpgpuCommandStreamReceiver().getResidencyAllocations().end(), [](const auto allocation) { return allocation->isFlushL3Required(); })) {
            allocNeedsFlushDC = true;
        }
    }

    auto memoryCompressionState = getGpgpuCommandStreamReceiver().getMemoryCompressionState(auxTranslationRequired);

    DispatchFlags dispatchFlags(
        {},                                                                                         //csrDependencies
        &timestampPacketDependencies.barrierNodes,                                                  //barrierTimestampPacketNodes
        {},                                                                                         //pipelineSelectArgs
        this->flushStamp->getStampReference(),                                                      //flushStampReference
        getThrottle(),                                                                              //throttle
        PreemptionHelper::taskPreemptionMode(getDevice(), multiDispatchInfo),                       //preemptionMode
        numGrfRequired,                                                                             //numGrfRequired
        L3CachingSettings::l3CacheOn,                                                               //l3CacheSettings
        kernel->getThreadArbitrationPolicy(),                                                       //threadArbitrationPolicy
        kernel->getAdditionalKernelExecInfo(),                                                      //additionalKernelExecInfo
        kernel->getExecutionType(),                                                                 //kernelExecutionType
        memoryCompressionState,                                                                     //memoryCompressionState
        getSliceCount(),                                                                            //sliceCount
        blocking,                                                                                   //blocking
        shouldFlushDC(commandType, printfHandler) || allocNeedsFlushDC,                             //dcFlush
        multiDispatchInfo.usesSlm() || multiDispatchInfo.peekParentKernel(),                        //useSLM
        true,                                                                                       //guardCommandBufferWithPipeControl
        commandType == CL_COMMAND_NDRANGE_KERNEL,                                                   //GSBA32BitRequired
        requiresCoherency,                                                                          //requiresCoherency
        (QueuePriority::LOW == priority),                                                           //lowPriority
        implicitFlush,                                                                              //implicitFlush
        !eventBuilder.getEvent() || getGpgpuCommandStreamReceiver().isNTo1SubmissionModelEnabled(), //outOfOrderExecutionAllowed
        false,                                                                                      //epilogueRequired
        usePerDssBackedBuffer,                                                                      //usePerDssBackedBuffer
        kernel->isSingleSubdevicePreferred(),                                                       //useSingleSubdevice
        useGlobalAtomics,                                                                           //useGlobalAtomics
        kernel->getTotalNumDevicesInContext()                                                       //numDevicesInContext
    );

    dispatchFlags.pipelineSelectArgs.mediaSamplerRequired = mediaSamplerRequired;
    dispatchFlags.pipelineSelectArgs.specialPipelineSelectMode = specialPipelineSelectMode;

    if (getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {
        eventsRequest.fillCsrDependencies(dispatchFlags.csrDependencies, getGpgpuCommandStreamReceiver(), CsrDependencies::DependenciesType::OutOfCsr);
        dispatchFlags.csrDependencies.makeResident(getGpgpuCommandStreamReceiver());
    }

    DEBUG_BREAK_IF(taskLevel >= CompletionStamp::notReady);

    if (anyUncacheableArgs) {
        dispatchFlags.l3CacheSettings = L3CachingSettings::l3CacheOff;
    } else if (!kernel->areStatelessWritesUsed()) {
        dispatchFlags.l3CacheSettings = L3CachingSettings::l3AndL1On;
    }

    if (this->dispatchHints != 0) {
        dispatchFlags.engineHints = this->dispatchHints;
        dispatchFlags.epilogueRequired = true;
    }

    if (gtpinIsGTPinInitialized()) {
        gtpinNotifyPreFlushTask(this);
    }

    if (enqueueProperties.blitPropertiesContainer->size() > 0) {
        this->bcsTaskCount = getBcsCommandStreamReceiver()->blitBuffer(*enqueueProperties.blitPropertiesContainer, false, this->isProfilingEnabled());
        dispatchFlags.implicitFlush = true;
    }

    PRINT_DEBUG_STRING(DebugManager.flags.PrintDebugMessages.get(), stdout, "preemption = %d.\n", static_cast<int>(dispatchFlags.preemptionMode));
    CompletionStamp completionStamp = getGpgpuCommandStreamReceiver().flushTask(
        commandStream,
        commandStreamStart,
        *dsh,
        *ioh,
        getIndirectHeap(IndirectHeap::SURFACE_STATE, 0u),
        taskLevel,
        dispatchFlags,
        getDevice());

    if (gtpinIsGTPinInitialized()) {
        gtpinNotifyFlushTask(completionStamp.taskCount);
    }

    return completionStamp;
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::enqueueBlocked(
    uint32_t commandType,
    Surface **surfaces,
    size_t surfaceCount,
    const MultiDispatchInfo &multiDispatchInfo,
    TimestampPacketDependencies &timestampPacketDependencies,
    std::unique_ptr<KernelOperation> &blockedCommandsData,
    const EnqueueProperties &enqueueProperties,
    EventsRequest &eventsRequest,
    EventBuilder &externalEventBuilder,
    std::unique_ptr<PrintfHandler> printfHandler) {

    TakeOwnershipWrapper<CommandQueueHw<GfxFamily>> queueOwnership(*this);

    //store previous virtual event as it will add dependecies to new virtual event
    if (this->virtualEvent) {
        DBG_LOG(EventsDebugEnable, "enqueueBlocked", "previousVirtualEvent", this->virtualEvent);
    }

    EventBuilder internalEventBuilder;
    EventBuilder *eventBuilder;
    // check if event will be exposed externally
    if (externalEventBuilder.getEvent()) {
        externalEventBuilder.getEvent()->incRefInternal();
        eventBuilder = &externalEventBuilder;
        DBG_LOG(EventsDebugEnable, "enqueueBlocked", "output event as virtualEvent", virtualEvent);
    } else {
        // it will be an internal event
        internalEventBuilder.create<VirtualEvent>(this, context);
        eventBuilder = &internalEventBuilder;
        DBG_LOG(EventsDebugEnable, "enqueueBlocked", "new virtualEvent", eventBuilder->getEvent());
    }
    auto outEvent = eventBuilder->getEvent();

    //update queue taskCount
    taskCount = outEvent->getCompletionStamp();

    std::unique_ptr<Command> command;
    bool storeTimestampPackets = false;

    if (blockedCommandsData) {
        if (enqueueProperties.blitPropertiesContainer) {
            blockedCommandsData->blitPropertiesContainer = *enqueueProperties.blitPropertiesContainer;
            blockedCommandsData->blitEnqueue = true;
        }

        storeTimestampPackets = (timestampPacketContainer != nullptr);
    }

    if (enqueueProperties.operation != EnqueueProperties::Operation::GpuKernel) {
        command = std::make_unique<CommandWithoutKernel>(*this, blockedCommandsData);
    } else {
        //store task data in event
        std::vector<Surface *> allSurfaces;
        Kernel *kernel = nullptr;
        for (auto &dispatchInfo : multiDispatchInfo) {
            if (kernel != dispatchInfo.getKernel()) {
                kernel = dispatchInfo.getKernel();
            } else {
                continue;
            }
            kernel->getResidency(allSurfaces, device->getRootDeviceIndex());
        }
        for (auto &surface : CreateRange(surfaces, surfaceCount)) {
            allSurfaces.push_back(surface->duplicate());
        }

        PreemptionMode preemptionMode = PreemptionHelper::taskPreemptionMode(getDevice(), multiDispatchInfo);
        bool slmUsed = multiDispatchInfo.usesSlm() || multiDispatchInfo.peekParentKernel();
        command = std::make_unique<CommandComputeKernel>(*this,
                                                         blockedCommandsData,
                                                         allSurfaces,
                                                         shouldFlushDC(commandType, printfHandler.get()),
                                                         slmUsed,
                                                         commandType == CL_COMMAND_NDRANGE_KERNEL,
                                                         std::move(printfHandler),
                                                         preemptionMode,
                                                         multiDispatchInfo.peekMainKernel(),
                                                         (uint32_t)multiDispatchInfo.size());
    }
    if (storeTimestampPackets) {
        for (cl_uint i = 0; i < eventsRequest.numEventsInWaitList; i++) {
            auto event = castToObjectOrAbort<Event>(eventsRequest.eventWaitList[i]);
            event->incRefInternal();
        }
        command->setTimestampPacketNode(*timestampPacketContainer, std::move(timestampPacketDependencies));
        command->setEventsRequest(eventsRequest);
    }
    outEvent->setCommand(std::move(command));

    eventBuilder->addParentEvents(ArrayRef<const cl_event>(eventsRequest.eventWaitList, eventsRequest.numEventsInWaitList));
    eventBuilder->addParentEvent(this->virtualEvent);
    eventBuilder->finalize();

    if (this->virtualEvent) {
        this->virtualEvent->decRefInternal();
    }

    this->virtualEvent = outEvent;
}

template <typename GfxFamily>
CompletionStamp CommandQueueHw<GfxFamily>::enqueueCommandWithoutKernel(
    Surface **surfaces,
    size_t surfaceCount,
    LinearStream *commandStream,
    size_t commandStreamStart,
    bool &blocking,
    const EnqueueProperties &enqueueProperties,
    TimestampPacketDependencies &timestampPacketDependencies,
    EventsRequest &eventsRequest,
    EventBuilder &eventBuilder,
    uint32_t taskLevel) {

    CompletionStamp completionStamp = {this->taskCount, this->taskLevel, this->flushStamp->peekStamp()};
    bool flushGpgpuCsr = true;

    if ((enqueueProperties.operation == EnqueueProperties::Operation::Blit) && !isGpgpuSubmissionForBcsRequired(false)) {
        flushGpgpuCsr = false;
    }

    if (eventBuilder.getEvent() && isProfilingEnabled()) {
        TimeStampData submitTimeStamp;

        getDevice().getOSTime()->getCpuGpuTime(&submitTimeStamp);
        eventBuilder.getEvent()->setSubmitTimeStamp(&submitTimeStamp);
    }

    if (flushGpgpuCsr) {
        if (timestampPacketContainer) {
            timestampPacketContainer->makeResident(getGpgpuCommandStreamReceiver());
            timestampPacketDependencies.previousEnqueueNodes.makeResident(getGpgpuCommandStreamReceiver());
            timestampPacketDependencies.cacheFlushNodes.makeResident(getGpgpuCommandStreamReceiver());
        }

        for (auto surface : CreateRange(surfaces, surfaceCount)) {
            surface->makeResident(getGpgpuCommandStreamReceiver());
        }

        DispatchFlags dispatchFlags(
            {},                                                                  //csrDependencies
            &timestampPacketDependencies.barrierNodes,                           //barrierTimestampPacketNodes
            {},                                                                  //pipelineSelectArgs
            flushStamp->getStampReference(),                                     //flushStampReference
            getThrottle(),                                                       //throttle
            device->getPreemptionMode(),                                         //preemptionMode
            GrfConfig::NotApplicable,                                            //numGrfRequired
            L3CachingSettings::NotApplicable,                                    //l3CacheSettings
            ThreadArbitrationPolicy::NotPresent,                                 //threadArbitrationPolicy
            AdditionalKernelExecInfo::NotApplicable,                             //additionalKernelExecInfo
            KernelExecutionType::NotApplicable,                                  //kernelExecutionType
            MemoryCompressionState::NotApplicable,                               //memoryCompressionState
            getSliceCount(),                                                     //sliceCount
            blocking,                                                            //blocking
            false,                                                               //dcFlush
            false,                                                               //useSLM
            true,                                                                //guardCommandBufferWithPipeControl
            false,                                                               //GSBA32BitRequired
            false,                                                               //requiresCoherency
            false,                                                               //lowPriority
            (enqueueProperties.operation == EnqueueProperties::Operation::Blit), //implicitFlush
            getGpgpuCommandStreamReceiver().isNTo1SubmissionModelEnabled(),      //outOfOrderExecutionAllowed
            false,                                                               //epilogueRequired
            false,                                                               //usePerDssBackedBuffer
            false,                                                               //useSingleSubdevice
            false,                                                               //useGlobalAtomics
            1u);                                                                 //numDevicesInContext

        if (getGpgpuCommandStreamReceiver().peekTimestampPacketWriteEnabled()) {
            eventsRequest.fillCsrDependencies(dispatchFlags.csrDependencies, getGpgpuCommandStreamReceiver(), CsrDependencies::DependenciesType::OutOfCsr);
            dispatchFlags.csrDependencies.makeResident(getGpgpuCommandStreamReceiver());
        }

        completionStamp = getGpgpuCommandStreamReceiver().flushTask(
            *commandStream,
            commandStreamStart,
            getIndirectHeap(IndirectHeap::DYNAMIC_STATE, 0u),
            getIndirectHeap(IndirectHeap::INDIRECT_OBJECT, 0u),
            getIndirectHeap(IndirectHeap::SURFACE_STATE, 0u),
            taskLevel,
            dispatchFlags,
            getDevice());
    }

    if (enqueueProperties.operation == EnqueueProperties::Operation::Blit) {
        UNRECOVERABLE_IF(!enqueueProperties.blitPropertiesContainer);
        this->bcsTaskCount = getBcsCommandStreamReceiver()->blitBuffer(*enqueueProperties.blitPropertiesContainer, false, this->isProfilingEnabled());
    }

    return completionStamp;
}

template <typename GfxFamily>
void CommandQueueHw<GfxFamily>::computeOffsetsValueForRectCommands(size_t *bufferOffset,
                                                                   size_t *hostOffset,
                                                                   const size_t *bufferOrigin,
                                                                   const size_t *hostOrigin,
                                                                   const size_t *region,
                                                                   size_t bufferRowPitch,
                                                                   size_t bufferSlicePitch,
                                                                   size_t hostRowPitch,
                                                                   size_t hostSlicePitch) {
    size_t computedBufferRowPitch = bufferRowPitch ? bufferRowPitch : region[0];
    size_t computedBufferSlicePitch = bufferSlicePitch ? bufferSlicePitch : region[1] * computedBufferRowPitch;
    size_t computedHostRowPitch = hostRowPitch ? hostRowPitch : region[0];
    size_t computedHostSlicePitch = hostSlicePitch ? hostSlicePitch : region[1] * computedHostRowPitch;
    *bufferOffset = bufferOrigin[2] * computedBufferSlicePitch + bufferOrigin[1] * computedBufferRowPitch + bufferOrigin[0];
    *hostOffset = hostOrigin[2] * computedHostSlicePitch + hostOrigin[1] * computedHostRowPitch + hostOrigin[0];
}

template <typename GfxFamily>
size_t CommandQueueHw<GfxFamily>::calculateHostPtrSizeForImage(const size_t *region, size_t rowPitch, size_t slicePitch, Image *image) {
    auto bytesPerPixel = image->getSurfaceFormatInfo().surfaceFormat.ImageElementSizeInBytes;
    auto dstRowPitch = rowPitch ? rowPitch : region[0] * bytesPerPixel;
    auto dstSlicePitch = slicePitch ? slicePitch : ((image->getImageDesc().image_type == CL_MEM_OBJECT_IMAGE1D_ARRAY ? 1 : region[1]) * dstRowPitch);

    return Image::calculateHostPtrSize(region, dstRowPitch, dstSlicePitch, bytesPerPixel, image->getImageDesc().image_type);
}

template <typename GfxFamily>
template <uint32_t cmdType>
void CommandQueueHw<GfxFamily>::enqueueBlit(const MultiDispatchInfo &multiDispatchInfo, cl_uint numEventsInWaitList, const cl_event *eventWaitList, cl_event *event, bool blocking) {
    auto commandStreamRecieverOwnership = getGpgpuCommandStreamReceiver().obtainUniqueOwnership();

    EventsRequest eventsRequest(numEventsInWaitList, eventWaitList, event);
    EventBuilder eventBuilder;

    setupEvent(eventBuilder, eventsRequest.outEvent, cmdType);

    std::unique_ptr<KernelOperation> blockedCommandsData;
    TakeOwnershipWrapper<CommandQueueHw<GfxFamily>> queueOwnership(*this);

    auto blockQueue = false;
    auto taskLevel = 0u;
    obtainTaskLevelAndBlockedStatus(taskLevel, eventsRequest.numEventsInWaitList, eventsRequest.eventWaitList, blockQueue, cmdType);
    auto clearAllDependencies = queueDependenciesClearRequired();

    enqueueHandlerHook(cmdType, multiDispatchInfo);
    aubCaptureHook(blocking, clearAllDependencies, multiDispatchInfo);

    if (DebugManager.flags.MakeEachEnqueueBlocking.get()) {
        blocking = true;
    }

    TimestampPacketDependencies timestampPacketDependencies;
    BlitPropertiesContainer blitPropertiesContainer;
    CsrDependencies csrDeps;

    eventsRequest.fillCsrDependencies(csrDeps, *getBcsCommandStreamReceiver(), CsrDependencies::DependenciesType::All);
    auto allocator = getBcsCommandStreamReceiver()->getTimestampPacketAllocator();

    if (isCacheFlushForBcsRequired() && isGpgpuSubmissionForBcsRequired(blockQueue)) {
        timestampPacketDependencies.cacheFlushNodes.add(allocator->getTag());
    }

    if (!blockQueue && getGpgpuCommandStreamReceiver().isStallingPipeControlOnNextFlushRequired()) {
        timestampPacketDependencies.barrierNodes.add(allocator->getTag());
    }

    obtainNewTimestampPacketNodes(1, timestampPacketDependencies.previousEnqueueNodes, clearAllDependencies, true);
    csrDeps.push_back(&timestampPacketDependencies.previousEnqueueNodes);

    LinearStream *gpgpuCommandStream = {};
    size_t gpgpuCommandStreamStart = {};
    if (isGpgpuSubmissionForBcsRequired(blockQueue)) {
        gpgpuCommandStream = obtainCommandStream<cmdType>(csrDeps, true, blockQueue, multiDispatchInfo, eventsRequest, blockedCommandsData, nullptr, 0);
        gpgpuCommandStreamStart = gpgpuCommandStream->getUsed();
    }

    if (eventBuilder.getEvent()) {
        eventBuilder.getEvent()->addTimestampPacketNodes(*timestampPacketContainer);
    }

    blitPropertiesContainer.push_back(processDispatchForBlitEnqueue(multiDispatchInfo, timestampPacketDependencies,
                                                                    eventsRequest, gpgpuCommandStream, cmdType, blockQueue));

    CompletionStamp completionStamp = {CompletionStamp::notReady, taskLevel, 0};

    const EnqueueProperties enqueueProperties(true, false, false, false, &blitPropertiesContainer);

    if (!blockQueue) {
        csrDeps.makeResident(getGpgpuCommandStreamReceiver());

        completionStamp = enqueueCommandWithoutKernel(nullptr, 0, gpgpuCommandStream, gpgpuCommandStreamStart, blocking, enqueueProperties, timestampPacketDependencies, eventsRequest, eventBuilder, taskLevel);

        if (eventBuilder.getEvent()) {
            eventBuilder.getEvent()->flushStamp->replaceStampObject(this->flushStamp->getStampReference());
        }

        this->latestSentEnqueueType = enqueueProperties.operation;
    }
    updateFromCompletionStamp(completionStamp, eventBuilder.getEvent());

    if (blockQueue) {
        enqueueBlocked(cmdType, nullptr, 0, multiDispatchInfo, timestampPacketDependencies, blockedCommandsData, enqueueProperties, eventsRequest, eventBuilder, nullptr);
    }

    queueOwnership.unlock();
    commandStreamRecieverOwnership.unlock();

    if (blocking) {
        waitUntilComplete(blockQueue, nullptr);
    }
}

template <typename GfxFamily>
template <uint32_t cmdType, size_t surfaceCount>
void CommandQueueHw<GfxFamily>::dispatchBcsOrGpgpuEnqueue(MultiDispatchInfo &dispatchInfo, Surface *(&surfaces)[surfaceCount], EBuiltInOps::Type builtInOperation, cl_uint numEventsInWaitList, const cl_event *eventWaitList, cl_event *event, bool blocking, bool blitAllowed) {
    const bool blitPreferred = blitEnqueuePreferred(cmdType, dispatchInfo.peekBuiltinOpParams());
    const bool blitRequired = isCopyOnly;
    const bool blit = blitAllowed && (blitPreferred || blitRequired);

    if (blit) {
        enqueueBlit<cmdType>(dispatchInfo, numEventsInWaitList, eventWaitList, event, blocking);
    } else {
        auto &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(builtInOperation,
                                                                                this->getClDevice());
        BuiltInOwnershipWrapper builtInLock(builder);

        builder.buildDispatchInfos(dispatchInfo);

        enqueueHandler<cmdType>(
            surfaces,
            blocking,
            dispatchInfo,
            numEventsInWaitList,
            eventWaitList,
            event);
    }
}

template <typename GfxFamily>
bool CommandQueueHw<GfxFamily>::isBlitAuxTranslationRequired(const MultiDispatchInfo &multiDispatchInfo) {
    return multiDispatchInfo.getKernelObjsForAuxTranslation() &&
           (multiDispatchInfo.getKernelObjsForAuxTranslation()->size() > 0) &&
           (HwHelperHw<GfxFamily>::get().getAuxTranslationMode(device->getHardwareInfo()) == AuxTranslationMode::Blit);
}

} // namespace NEO
