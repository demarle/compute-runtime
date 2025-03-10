/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/command_stream_receiver.h"

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/command_stream/experimental_command_buffer.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/device/device.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/array_count.h"
#include "shared/source/helpers/cache_policy.h"
#include "shared/source/helpers/flush_stamp.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/helpers/pause_on_gpu_properties.h"
#include "shared/source/helpers/string.h"
#include "shared/source/helpers/timestamp_packet.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/surface.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/source/utilities/cpuintrinsics.h"
#include "shared/source/utilities/tag_allocator.h"

namespace NEO {

// Global table of CommandStreamReceiver factories for HW and tests
CommandStreamReceiverCreateFunc commandStreamReceiverFactory[2 * IGFX_MAX_CORE] = {};

CommandStreamReceiver::CommandStreamReceiver(ExecutionEnvironment &executionEnvironment,
                                             uint32_t rootDeviceIndex,
                                             const DeviceBitfield deviceBitfield)
    : executionEnvironment(executionEnvironment), rootDeviceIndex(rootDeviceIndex), deviceBitfield(deviceBitfield) {
    residencyAllocations.reserve(20);

    latestSentStatelessMocsConfig = CacheSettings::unknownMocs;
    submissionAggregator.reset(new SubmissionAggregator());
    if (DebugManager.flags.CsrDispatchMode.get()) {
        this->dispatchMode = (DispatchMode)DebugManager.flags.CsrDispatchMode.get();
    }
    flushStamp.reset(new FlushStampTracker(true));
    for (int i = 0; i < IndirectHeap::NUM_TYPES; ++i) {
        indirectHeap[i] = nullptr;
    }
    internalAllocationStorage = std::make_unique<InternalAllocationStorage>(*this);

    if (deviceBitfield.count() > 1 && DebugManager.flags.EnableStaticPartitioning.get() != 0) {
        this->staticWorkPartitioningEnabled = true;
    }
}

CommandStreamReceiver::~CommandStreamReceiver() {
    if (userPauseConfirmation) {
        {
            std::unique_lock<SpinLock> lock{debugPauseStateLock};
            *debugPauseStateAddress = DebugPauseState::terminate;
        }

        userPauseConfirmation->join();
    }

    for (int i = 0; i < IndirectHeap::NUM_TYPES; ++i) {
        if (indirectHeap[i] != nullptr) {
            auto allocation = indirectHeap[i]->getGraphicsAllocation();
            if (allocation != nullptr) {
                internalAllocationStorage->storeAllocation(std::unique_ptr<GraphicsAllocation>(allocation), REUSABLE_ALLOCATION);
            }
            delete indirectHeap[i];
        }
    }
    cleanupResources();

    profilingTimeStampAllocator.reset();
    perfCounterAllocator.reset();
    timestampPacketAllocator.reset();

    internalAllocationStorage->cleanAllocationList(-1, REUSABLE_ALLOCATION);
    internalAllocationStorage->cleanAllocationList(-1, TEMPORARY_ALLOCATION);
    getMemoryManager()->unregisterEngineForCsr(this);
}

bool CommandStreamReceiver::submitBatchBuffer(BatchBuffer &batchBuffer, ResidencyContainer &allocationsForResidency) {
    this->latestFlushedTaskCount = taskCount + 1;
    this->latestSentTaskCount = taskCount + 1;

    auto ret = this->flush(batchBuffer, allocationsForResidency);
    taskCount++;

    return ret;
}

void CommandStreamReceiver::makeResident(GraphicsAllocation &gfxAllocation) {
    auto submissionTaskCount = this->taskCount + 1;
    if (gfxAllocation.isResidencyTaskCountBelow(submissionTaskCount, osContext->getContextId())) {
        this->getResidencyAllocations().push_back(&gfxAllocation);
        checkForNewResources(submissionTaskCount, gfxAllocation.getTaskCount(osContext->getContextId()), gfxAllocation);
        gfxAllocation.updateTaskCount(submissionTaskCount, osContext->getContextId());
        if (!gfxAllocation.isResident(osContext->getContextId())) {
            this->totalMemoryUsed += gfxAllocation.getUnderlyingBufferSize();
        }
    }
    gfxAllocation.updateResidencyTaskCount(submissionTaskCount, osContext->getContextId());
}

void CommandStreamReceiver::processEviction() {
    this->getEvictionAllocations().clear();
}

void CommandStreamReceiver::makeNonResident(GraphicsAllocation &gfxAllocation) {
    if (gfxAllocation.isResident(osContext->getContextId())) {
        if (gfxAllocation.peekEvictable()) {
            this->getEvictionAllocations().push_back(&gfxAllocation);
        } else {
            gfxAllocation.setEvictable(true);
        }
    }

    gfxAllocation.releaseResidencyInOsContext(this->osContext->getContextId());
}

void CommandStreamReceiver::makeSurfacePackNonResident(ResidencyContainer &allocationsForResidency) {
    for (auto &surface : allocationsForResidency) {
        this->makeNonResident(*surface);
    }
    allocationsForResidency.clear();
    this->processEviction();
}

void CommandStreamReceiver::makeResidentHostPtrAllocation(GraphicsAllocation *gfxAllocation) {
    makeResident(*gfxAllocation);
}

void CommandStreamReceiver::waitForTaskCountAndCleanAllocationList(uint32_t requiredTaskCount, uint32_t allocationUsage) {
    auto address = tagAddress;
    if (address) {
        while (*address < requiredTaskCount)
            ;
    }
    internalAllocationStorage->cleanAllocationList(requiredTaskCount, allocationUsage);
}

void CommandStreamReceiver::waitForTaskCountAndCleanTemporaryAllocationList(uint32_t requiredTaskCount) {
    waitForTaskCountAndCleanAllocationList(requiredTaskCount, TEMPORARY_ALLOCATION);
};

void CommandStreamReceiver::ensureCommandBufferAllocation(LinearStream &commandStream, size_t minimumRequiredSize, size_t additionalAllocationSize) {
    if (commandStream.getAvailableSpace() >= minimumRequiredSize) {
        return;
    }

    const auto allocationSize = alignUp(minimumRequiredSize + additionalAllocationSize, MemoryConstants::pageSize64k);
    constexpr static auto allocationType = GraphicsAllocation::AllocationType::COMMAND_BUFFER;
    auto allocation = this->getInternalAllocationStorage()->obtainReusableAllocation(allocationSize, allocationType).release();
    if (allocation == nullptr) {
        const AllocationProperties commandStreamAllocationProperties{rootDeviceIndex, true, allocationSize, allocationType,
                                                                     isMultiOsContextCapable(), false, osContext->getDeviceBitfield()};
        allocation = this->getMemoryManager()->allocateGraphicsMemoryWithProperties(commandStreamAllocationProperties);
    }
    DEBUG_BREAK_IF(allocation == nullptr);

    if (commandStream.getGraphicsAllocation() != nullptr) {
        getInternalAllocationStorage()->storeAllocation(std::unique_ptr<GraphicsAllocation>(commandStream.getGraphicsAllocation()), REUSABLE_ALLOCATION);
    }

    commandStream.replaceBuffer(allocation->getUnderlyingBuffer(), allocationSize - additionalAllocationSize);
    commandStream.replaceGraphicsAllocation(allocation);
}

MemoryManager *CommandStreamReceiver::getMemoryManager() const {
    DEBUG_BREAK_IF(!executionEnvironment.memoryManager);
    return executionEnvironment.memoryManager.get();
}

LinearStream &CommandStreamReceiver::getCS(size_t minRequiredSize) {
    constexpr static auto additionalAllocationSize = MemoryConstants::cacheLineSize + CSRequirements::csOverfetchSize;
    ensureCommandBufferAllocation(this->commandStream, minRequiredSize, additionalAllocationSize);
    return commandStream;
}

OSInterface *CommandStreamReceiver::getOSInterface() const {
    return executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->osInterface.get();
}

uint64_t CommandStreamReceiver::getWorkPartitionAllocationGpuAddress() const {
    if (isStaticWorkPartitioningEnabled()) {
        return getWorkPartitionAllocation()->getGpuAddress();
    }
    return 0;
}

bool CommandStreamReceiver::isRcs() const {
    return this->osContext->getEngineType() == aub_stream::ENGINE_RCS;
}

void CommandStreamReceiver::cleanupResources() {
    waitForTaskCountAndCleanAllocationList(this->latestFlushedTaskCount, TEMPORARY_ALLOCATION);
    waitForTaskCountAndCleanAllocationList(this->latestFlushedTaskCount, REUSABLE_ALLOCATION);

    if (debugSurface) {
        getMemoryManager()->freeGraphicsMemory(debugSurface);
        debugSurface = nullptr;
    }

    if (commandStream.getCpuBase()) {
        getMemoryManager()->freeGraphicsMemory(commandStream.getGraphicsAllocation());
        commandStream.replaceGraphicsAllocation(nullptr);
        commandStream.replaceBuffer(nullptr, 0);
    }

    if (tagsMultiAllocation) {
        for (auto graphicsAllocation : tagsMultiAllocation->getGraphicsAllocations()) {
            getMemoryManager()->freeGraphicsMemory(graphicsAllocation);
        }
        delete tagsMultiAllocation;
        tagsMultiAllocation = nullptr;
        tagAllocation = nullptr;
        tagAddress = nullptr;
    }

    if (globalFenceAllocation) {
        getMemoryManager()->freeGraphicsMemory(globalFenceAllocation);
        globalFenceAllocation = nullptr;
    }

    if (preemptionAllocation) {
        getMemoryManager()->freeGraphicsMemory(preemptionAllocation);
        preemptionAllocation = nullptr;
    }

    if (perDssBackedBuffer) {
        getMemoryManager()->freeGraphicsMemory(perDssBackedBuffer);
        perDssBackedBuffer = nullptr;
    }

    if (clearColorAllocation) {
        getMemoryManager()->freeGraphicsMemory(clearColorAllocation);
        clearColorAllocation = nullptr;
    }

    if (workPartitionAllocation) {
        getMemoryManager()->freeGraphicsMemory(workPartitionAllocation);
        workPartitionAllocation = nullptr;
    }
}

bool CommandStreamReceiver::waitForCompletionWithTimeout(bool enableTimeout, int64_t timeoutMicroseconds, uint32_t taskCountToWait) {
    std::chrono::high_resolution_clock::time_point time1, time2;
    int64_t timeDiff = 0;

    uint32_t latestSentTaskCount = this->latestFlushedTaskCount;
    if (latestSentTaskCount < taskCountToWait) {
        if (!this->flushBatchedSubmissions()) {
            return false;
        }
    }

    time1 = std::chrono::high_resolution_clock::now();
    while (*getTagAddress() < taskCountToWait && timeDiff <= timeoutMicroseconds) {
        std::this_thread::yield();
        CpuIntrinsics::pause();

        if (enableTimeout) {
            time2 = std::chrono::high_resolution_clock::now();
            timeDiff = std::chrono::duration_cast<std::chrono::microseconds>(time2 - time1).count();
        }
    }
    if (*getTagAddress() >= taskCountToWait) {
        return true;
    }
    return false;
}

void CommandStreamReceiver::setTagAllocation(GraphicsAllocation *allocation) {
    this->tagAllocation = allocation;
    UNRECOVERABLE_IF(allocation == nullptr);
    this->tagAddress = reinterpret_cast<uint32_t *>(allocation->getUnderlyingBuffer());
    this->debugPauseStateAddress = reinterpret_cast<DebugPauseState *>(
        reinterpret_cast<uint8_t *>(allocation->getUnderlyingBuffer()) + debugPauseStateAddressOffset);
}

MultiGraphicsAllocation &CommandStreamReceiver::createTagsMultiAllocation() {
    std::vector<uint32_t> rootDeviceIndices;

    for (auto index = 0u; index < this->executionEnvironment.rootDeviceEnvironments.size(); index++) {
        if (this->executionEnvironment.rootDeviceEnvironments[index].get()->getHardwareInfo()->platform.eProductFamily ==
            this->executionEnvironment.rootDeviceEnvironments[this->rootDeviceIndex].get()->getHardwareInfo()->platform.eProductFamily) {
            rootDeviceIndices.push_back(index);
        }
    }

    auto maxRootDeviceIndex = *std::max_element(rootDeviceIndices.begin(), rootDeviceIndices.end(), std::less<uint32_t const>());
    auto allocations = new MultiGraphicsAllocation(maxRootDeviceIndex);

    AllocationProperties unifiedMemoryProperties{rootDeviceIndices.at(0), MemoryConstants::pageSize, GraphicsAllocation::AllocationType::TAG_BUFFER, systemMemoryBitfield};

    this->getMemoryManager()->createMultiGraphicsAllocationInSystemMemoryPool(rootDeviceIndices, unifiedMemoryProperties, *allocations);
    return *allocations;
}

FlushStamp CommandStreamReceiver::obtainCurrentFlushStamp() const {
    return flushStamp->peekStamp();
}

void CommandStreamReceiver::setRequiredScratchSizes(uint32_t newRequiredScratchSize, uint32_t newRequiredPrivateScratchSize) {
    if (newRequiredScratchSize > requiredScratchSize) {
        requiredScratchSize = newRequiredScratchSize;
    }
    if (newRequiredPrivateScratchSize > requiredPrivateScratchSize) {
        requiredPrivateScratchSize = newRequiredPrivateScratchSize;
    }
}

GraphicsAllocation *CommandStreamReceiver::getScratchAllocation() {
    return scratchSpaceController->getScratchSpaceAllocation();
}

void CommandStreamReceiver::initProgrammingFlags() {
    isPreambleSent = false;
    GSBAFor32BitProgrammed = false;
    bindingTableBaseAddressRequired = true;
    mediaVfeStateDirty = true;
    lastVmeSubslicesConfig = false;

    lastSentL3Config = 0;
    lastSentCoherencyRequest = -1;
    lastMediaSamplerConfig = -1;
    lastPreemptionMode = PreemptionMode::Initial;
    latestSentStatelessMocsConfig = 0;
    lastSentUseGlobalAtomics = false;
}

void CommandStreamReceiver::programForAubSubCapture(bool wasActiveInPreviousEnqueue, bool isActive) {
    if (!wasActiveInPreviousEnqueue && isActive) {
        // force CSR reprogramming upon subcapture activation
        this->initProgrammingFlags();
    }
    if (wasActiveInPreviousEnqueue && !isActive) {
        // flush BB upon subcapture deactivation
        this->flushBatchedSubmissions();
    }
}

ResidencyContainer &CommandStreamReceiver::getResidencyAllocations() {
    return this->residencyAllocations;
}

ResidencyContainer &CommandStreamReceiver::getEvictionAllocations() {
    return this->evictionAllocations;
}

AubSubCaptureStatus CommandStreamReceiver::checkAndActivateAubSubCapture(const MultiDispatchInfo &dispatchInfo) { return {false, false}; }

void CommandStreamReceiver::addAubComment(const char *comment) {}

GraphicsAllocation *CommandStreamReceiver::allocateDebugSurface(size_t size) {
    UNRECOVERABLE_IF(debugSurface != nullptr);
    debugSurface = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, size, GraphicsAllocation::AllocationType::INTERNAL_HOST_MEMORY, getOsContext().getDeviceBitfield()});
    return debugSurface;
}

IndirectHeap &CommandStreamReceiver::getIndirectHeap(IndirectHeap::Type heapType,
                                                     size_t minRequiredSize) {
    DEBUG_BREAK_IF(static_cast<uint32_t>(heapType) >= arrayCount(indirectHeap));
    auto &heap = indirectHeap[heapType];
    GraphicsAllocation *heapMemory = nullptr;

    if (heap)
        heapMemory = heap->getGraphicsAllocation();

    if (heap && heap->getAvailableSpace() < minRequiredSize && heapMemory) {
        internalAllocationStorage->storeAllocation(std::unique_ptr<GraphicsAllocation>(heapMemory), REUSABLE_ALLOCATION);
        heapMemory = nullptr;
    }

    if (!heapMemory) {
        allocateHeapMemory(heapType, minRequiredSize, heap);
    }

    return *heap;
}

void CommandStreamReceiver::allocateHeapMemory(IndirectHeap::Type heapType,
                                               size_t minRequiredSize, IndirectHeap *&indirectHeap) {
    size_t reservedSize = 0;
    auto finalHeapSize = defaultHeapSize;
    if (IndirectHeap::SURFACE_STATE == heapType) {
        finalHeapSize = defaultSshSize;
    }
    bool requireInternalHeap = IndirectHeap::INDIRECT_OBJECT == heapType ? true : false;

    if (DebugManager.flags.AddPatchInfoCommentsForAUBDump.get()) {
        requireInternalHeap = false;
    }

    minRequiredSize += reservedSize;

    finalHeapSize = alignUp(std::max(finalHeapSize, minRequiredSize), MemoryConstants::pageSize);
    auto allocationType = GraphicsAllocation::AllocationType::LINEAR_STREAM;
    if (requireInternalHeap) {
        allocationType = GraphicsAllocation::AllocationType::INTERNAL_HEAP;
    }
    auto heapMemory = internalAllocationStorage->obtainReusableAllocation(finalHeapSize, allocationType).release();

    if (!heapMemory) {
        heapMemory = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, true, finalHeapSize, allocationType,
                                                                               isMultiOsContextCapable(), false, osContext->getDeviceBitfield()});
    } else {
        finalHeapSize = std::max(heapMemory->getUnderlyingBufferSize(), finalHeapSize);
    }

    if (IndirectHeap::SURFACE_STATE == heapType) {
        DEBUG_BREAK_IF(minRequiredSize > defaultSshSize - MemoryConstants::pageSize);
        finalHeapSize = defaultSshSize - MemoryConstants::pageSize;
    }

    if (indirectHeap) {
        indirectHeap->replaceBuffer(heapMemory->getUnderlyingBuffer(), finalHeapSize);
        indirectHeap->replaceGraphicsAllocation(heapMemory);
    } else {
        indirectHeap = new IndirectHeap(heapMemory, requireInternalHeap);
        indirectHeap->overrideMaxSize(finalHeapSize);
    }
    scratchSpaceController->reserveHeap(heapType, indirectHeap);
}

void CommandStreamReceiver::releaseIndirectHeap(IndirectHeap::Type heapType) {
    DEBUG_BREAK_IF(static_cast<uint32_t>(heapType) >= arrayCount(indirectHeap));
    auto &heap = indirectHeap[heapType];

    if (heap) {
        auto heapMemory = heap->getGraphicsAllocation();
        if (heapMemory != nullptr)
            internalAllocationStorage->storeAllocation(std::unique_ptr<GraphicsAllocation>(heapMemory), REUSABLE_ALLOCATION);
        heap->replaceBuffer(nullptr, 0);
        heap->replaceGraphicsAllocation(nullptr);
    }
}

void CommandStreamReceiver::setExperimentalCmdBuffer(std::unique_ptr<ExperimentalCommandBuffer> &&cmdBuffer) {
    experimentalCmdBuffer = std::move(cmdBuffer);
}

void *CommandStreamReceiver::asyncDebugBreakConfirmation(void *arg) {
    auto self = reinterpret_cast<CommandStreamReceiver *>(arg);

    do {
        auto debugPauseStateValue = DebugPauseState::waitingForUserStartConfirmation;
        if (DebugManager.flags.PauseOnGpuMode.get() != PauseOnGpuProperties::PauseMode::AfterWorkload) {
            do {
                {
                    std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                    debugPauseStateValue = *self->debugPauseStateAddress;
                }

                if (debugPauseStateValue == DebugPauseState::terminate) {
                    return nullptr;
                }
                std::this_thread::yield();
            } while (debugPauseStateValue != DebugPauseState::waitingForUserStartConfirmation);
            std::cout << "Debug break: Press enter to start workload" << std::endl;
            self->debugConfirmationFunction();
            debugPauseStateValue = DebugPauseState::hasUserStartConfirmation;
            {
                std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                *self->debugPauseStateAddress = debugPauseStateValue;
            }
        }

        if (DebugManager.flags.PauseOnGpuMode.get() != PauseOnGpuProperties::PauseMode::BeforeWorkload) {
            do {
                {
                    std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                    debugPauseStateValue = *self->debugPauseStateAddress;
                }
                if (debugPauseStateValue == DebugPauseState::terminate) {
                    return nullptr;
                }
                std::this_thread::yield();
            } while (debugPauseStateValue != DebugPauseState::waitingForUserEndConfirmation);

            std::cout << "Debug break: Workload ended, press enter to continue" << std::endl;
            self->debugConfirmationFunction();

            {
                std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                *self->debugPauseStateAddress = DebugPauseState::hasUserEndConfirmation;
            }
        }
    } while (DebugManager.flags.PauseOnEnqueue.get() == PauseOnGpuProperties::DebugFlagValues::OnEachEnqueue || DebugManager.flags.PauseOnBlitCopy.get() == PauseOnGpuProperties::DebugFlagValues::OnEachEnqueue);
    return nullptr;
}

bool CommandStreamReceiver::initializeTagAllocation() {
    this->tagsMultiAllocation = &this->createTagsMultiAllocation();

    auto tagAllocation = tagsMultiAllocation->getGraphicsAllocation(rootDeviceIndex);
    if (!tagAllocation) {
        return false;
    }

    this->setTagAllocation(tagAllocation);
    *this->tagAddress = DebugManager.flags.EnableNullHardware.get() ? -1 : initialHardwareTag;
    *this->debugPauseStateAddress = DebugManager.flags.EnableNullHardware.get() ? DebugPauseState::disabled : DebugPauseState::waitingForFirstSemaphore;

    PRINT_DEBUG_STRING(DebugManager.flags.PrintTagAllocationAddress.get(), stdout,
                       "\nCreated tag allocation %p for engine %u\n",
                       this->tagAddress, static_cast<uint32_t>(osContext->getEngineType()));

    if (DebugManager.flags.PauseOnEnqueue.get() != -1 || DebugManager.flags.PauseOnBlitCopy.get() != -1) {
        userPauseConfirmation = Thread::create(CommandStreamReceiver::asyncDebugBreakConfirmation, reinterpret_cast<void *>(this));
    }

    return true;
}

bool CommandStreamReceiver::createWorkPartitionAllocation(const Device &device) {
    if (!staticWorkPartitioningEnabled) {
        return false;
    }
    UNRECOVERABLE_IF(device.getNumAvailableDevices() < 2);

    AllocationProperties properties{this->rootDeviceIndex, true, 4096u, GraphicsAllocation::AllocationType::WORK_PARTITION_SURFACE, true, false, deviceBitfield};
    this->workPartitionAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties(properties);
    if (this->workPartitionAllocation == nullptr) {
        return false;
    }

    for (uint32_t deviceIndex = 0; deviceIndex < deviceBitfield.size(); deviceIndex++) {
        if (!deviceBitfield.test(deviceIndex)) {
            continue;
        }

        const uint32_t copySrc = deviceIndex;
        const Vec3<size_t> copySrcSize = {sizeof(copySrc), 1, 1};
        DeviceBitfield copyBitfield{};
        copyBitfield.set(deviceIndex);
        BlitOperationResult blitResult = BlitHelper::blitMemoryToAllocationBanks(device, workPartitionAllocation, 0, &copySrc, copySrcSize, copyBitfield);

        if (blitResult != BlitOperationResult::Success) {
            return false;
        }
    }

    return true;
}

bool CommandStreamReceiver::createGlobalFenceAllocation() {
    auto hwInfo = executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->getHardwareInfo();
    if (!HwHelper::get(hwInfo->platform.eRenderCoreFamily).isFenceAllocationRequired(*hwInfo)) {
        return true;
    }

    DEBUG_BREAK_IF(this->globalFenceAllocation != nullptr);
    this->globalFenceAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, MemoryConstants::pageSize, GraphicsAllocation::AllocationType::GLOBAL_FENCE, osContext->getDeviceBitfield()});
    return this->globalFenceAllocation != nullptr;
}

bool CommandStreamReceiver::createPreemptionAllocation() {
    auto hwInfo = executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->getHardwareInfo();
    AllocationProperties properties{rootDeviceIndex, hwInfo->capabilityTable.requiredPreemptionSurfaceSize, GraphicsAllocation::AllocationType::PREEMPTION, osContext->getDeviceBitfield()};
    properties.flags.uncacheable = hwInfo->workaroundTable.waCSRUncachable;
    properties.alignment = 256 * MemoryConstants::kiloByte;
    this->preemptionAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties(properties);
    return this->preemptionAllocation != nullptr;
}

std::unique_lock<CommandStreamReceiver::MutexType> CommandStreamReceiver::obtainUniqueOwnership() {
    return std::unique_lock<CommandStreamReceiver::MutexType>(this->ownershipMutex);
}
AllocationsList &CommandStreamReceiver::getTemporaryAllocations() { return internalAllocationStorage->getTemporaryAllocations(); }
AllocationsList &CommandStreamReceiver::getAllocationsForReuse() { return internalAllocationStorage->getAllocationsForReuse(); }

bool CommandStreamReceiver::createAllocationForHostSurface(HostPtrSurface &surface, bool requiresL3Flush) {
    auto allocation = internalAllocationStorage->obtainTemporaryAllocationWithPtr(surface.getSurfaceSize(), surface.getMemoryPointer(), GraphicsAllocation::AllocationType::EXTERNAL_HOST_PTR);

    if (allocation == nullptr) {
        auto memoryManager = getMemoryManager();
        AllocationProperties properties{rootDeviceIndex,
                                        false, // allocateMemory
                                        surface.getSurfaceSize(), GraphicsAllocation::AllocationType::EXTERNAL_HOST_PTR,
                                        false, // isMultiStorageAllocation
                                        osContext->getDeviceBitfield()};
        properties.flags.flushL3RequiredForRead = properties.flags.flushL3RequiredForWrite = requiresL3Flush;
        allocation.reset(memoryManager->allocateGraphicsMemoryWithProperties(properties, surface.getMemoryPointer()));
        if (allocation == nullptr && surface.peekIsPtrCopyAllowed()) {
            // Try with no host pointer allocation and copy
            allocation.reset(memoryManager->allocateInternalGraphicsMemoryWithHostCopy(rootDeviceIndex,
                                                                                       internalAllocationStorage->getDeviceBitfield(),
                                                                                       surface.getMemoryPointer(),
                                                                                       surface.getSurfaceSize()));
        }
    }

    if (allocation == nullptr) {
        return false;
    }
    allocation->updateTaskCount(CompletionStamp::notReady, osContext->getContextId());
    surface.setAllocation(allocation.get());
    internalAllocationStorage->storeAllocation(std::move(allocation), TEMPORARY_ALLOCATION);
    return true;
}

TagAllocator<HwTimeStamps> *CommandStreamReceiver::getEventTsAllocator() {
    if (profilingTimeStampAllocator.get() == nullptr) {
        profilingTimeStampAllocator = std::make_unique<TagAllocator<HwTimeStamps>>(
            rootDeviceIndex, getMemoryManager(), getPreferredTagPoolSize(), MemoryConstants::cacheLineSize, sizeof(HwTimeStamps), false, osContext->getDeviceBitfield());
    }
    return profilingTimeStampAllocator.get();
}

TagAllocator<HwPerfCounter> *CommandStreamReceiver::getEventPerfCountAllocator(const uint32_t tagSize) {
    if (perfCounterAllocator.get() == nullptr) {
        perfCounterAllocator = std::make_unique<TagAllocator<HwPerfCounter>>(
            rootDeviceIndex, getMemoryManager(), getPreferredTagPoolSize(), MemoryConstants::cacheLineSize, tagSize, false, osContext->getDeviceBitfield());
    }
    return perfCounterAllocator.get();
}

TagAllocator<TimestampPacketStorage> *CommandStreamReceiver::getTimestampPacketAllocator() {
    if (timestampPacketAllocator.get() == nullptr) {
        // dont release nodes in aub/tbx mode, to avoid removing semaphores optimization or reusing returned tags
        bool doNotReleaseNodes = (getType() > CommandStreamReceiverType::CSR_HW) ||
                                 DebugManager.flags.DisableTimestampPacketOptimizations.get();

        timestampPacketAllocator = std::make_unique<TagAllocator<TimestampPacketStorage>>(
            rootDeviceIndex, getMemoryManager(), getPreferredTagPoolSize(), MemoryConstants::cacheLineSize * 4,
            sizeof(TimestampPacketStorage), doNotReleaseNodes, osContext->getDeviceBitfield());
    }
    return timestampPacketAllocator.get();
}

size_t CommandStreamReceiver::getPreferredTagPoolSize() const {
    if (DebugManager.flags.DisableTimestampPacketOptimizations.get()) {
        return 1;
    }

    return 2048;
}

bool CommandStreamReceiver::expectMemory(const void *gfxAddress, const void *srcAddress,
                                         size_t length, uint32_t compareOperation) {
    auto isMemoryEqual = (memcmp(gfxAddress, srcAddress, length) == 0);
    auto isEqualMemoryExpected = (compareOperation == AubMemDump::CmdServicesMemTraceMemoryCompare::CompareOperationValues::CompareEqual);

    return (isMemoryEqual == isEqualMemoryExpected);
}

bool CommandStreamReceiver::needsPageTableManager(aub_stream::EngineType engineType) const {
    auto hwInfo = executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->getHardwareInfo();
    auto defaultEngineType = getChosenEngineType(*hwInfo);
    if (engineType != defaultEngineType) {
        return false;
    }
    auto rootDeviceEnvironment = executionEnvironment.rootDeviceEnvironments[rootDeviceIndex].get();
    if (rootDeviceEnvironment->pageTableManager.get() != nullptr) {
        return false;
    }
    return HwHelper::get(hwInfo->platform.eRenderCoreFamily).isPageTableManagerSupported(*hwInfo);
}

void CommandStreamReceiver::printDeviceIndex() {
    if (DebugManager.flags.PrintDeviceAndEngineIdOnSubmission.get()) {
        printf("Submission to RootDevice Index: %u, Sub-Devices Mask: %lu, EngineId: %u\n", this->getRootDeviceIndex(), this->osContext->getDeviceBitfield().to_ulong(), this->osContext->getEngineType());
    }
}

void CommandStreamReceiver::checkForNewResources(uint32_t submittedTaskCount, uint32_t allocationTaskCount, GraphicsAllocation &gfxAllocation) {
    if (useNewResourceImplicitFlush) {
        if (allocationTaskCount == GraphicsAllocation::objectNotUsed && !GraphicsAllocation::isIsaAllocationType(gfxAllocation.getAllocationType())) {
            newResources = true;
            if (DebugManager.flags.ProvideVerboseImplicitFlush.get()) {
                printf("New resource detected of type %llu\n", static_cast<unsigned long long>(gfxAllocation.getAllocationType()));
            }
        }
    }
}

bool CommandStreamReceiver::checkImplicitFlushForGpuIdle() {
    if (useGpuIdleImplicitFlush) {
        if (this->taskCount == *getTagAddress()) {
            return true;
        }
    }
    return false;
}

} // namespace NEO
