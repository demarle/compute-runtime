/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/memory_manager/unified_memory_manager.h"

#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/helpers/aligned_memory.h"
#include "shared/source/memory_manager/memory_manager.h"

#include "opencl/source/mem_obj/mem_obj_helper.h"

namespace NEO {

void SVMAllocsManager::MapBasedAllocationTracker::insert(SvmAllocationData allocationsPair) {
    allocations.insert(std::make_pair(reinterpret_cast<void *>(allocationsPair.gpuAllocations.getDefaultGraphicsAllocation()->getGpuAddress()), allocationsPair));
}

void SVMAllocsManager::MapBasedAllocationTracker::remove(SvmAllocationData allocationsPair) {
    SvmAllocationContainer::iterator iter;
    iter = allocations.find(reinterpret_cast<void *>(allocationsPair.gpuAllocations.getDefaultGraphicsAllocation()->getGpuAddress()));
    allocations.erase(iter);
}

SvmAllocationData *SVMAllocsManager::MapBasedAllocationTracker::get(const void *ptr) {
    SvmAllocationContainer::iterator Iter, End;
    SvmAllocationData *svmAllocData;
    if ((ptr == nullptr) || (allocations.size() == 0)) {
        return nullptr;
    }
    End = allocations.end();
    Iter = allocations.lower_bound(ptr);
    if (((Iter != End) && (Iter->first != ptr)) ||
        (Iter == End)) {
        if (Iter == allocations.begin()) {
            Iter = End;
        } else {
            Iter--;
        }
    }
    if (Iter != End) {
        svmAllocData = &Iter->second;
        char *charPtr = reinterpret_cast<char *>(svmAllocData->gpuAllocations.getDefaultGraphicsAllocation()->getGpuAddress());
        if (ptr < (charPtr + svmAllocData->size)) {
            return svmAllocData;
        }
    }
    return nullptr;
}

void SVMAllocsManager::MapOperationsTracker::insert(SvmMapOperation mapOperation) {
    operations.insert(std::make_pair(mapOperation.regionSvmPtr, mapOperation));
}

void SVMAllocsManager::MapOperationsTracker::remove(const void *regionPtr) {
    SvmMapOperationsContainer::iterator iter;
    iter = operations.find(regionPtr);
    operations.erase(iter);
}

SvmMapOperation *SVMAllocsManager::MapOperationsTracker::get(const void *regionPtr) {
    SvmMapOperationsContainer::iterator iter;
    iter = operations.find(regionPtr);
    if (iter == operations.end()) {
        return nullptr;
    }
    return &iter->second;
}

void SVMAllocsManager::addInternalAllocationsToResidencyContainer(uint32_t rootDeviceIndex,
                                                                  ResidencyContainer &residencyContainer,
                                                                  uint32_t requestedTypesMask) {
    std::unique_lock<SpinLock> lock(mtx);
    for (auto &allocation : this->SVMAllocs.allocations) {
        if (rootDeviceIndex >= allocation.second.gpuAllocations.getGraphicsAllocations().size()) {
            continue;
        }

        if (!(allocation.second.memoryType & requestedTypesMask) ||
            (nullptr == allocation.second.gpuAllocations.getGraphicsAllocation(rootDeviceIndex))) {
            continue;
        }

        auto alloc = allocation.second.gpuAllocations.getGraphicsAllocation(rootDeviceIndex);
        if (residencyContainer.end() == std::find(residencyContainer.begin(), residencyContainer.end(), alloc)) {
            residencyContainer.push_back(alloc);
        }
    }
}

void SVMAllocsManager::makeInternalAllocationsResident(CommandStreamReceiver &commandStreamReceiver, uint32_t requestedTypesMask) {
    std::unique_lock<SpinLock> lock(mtx);
    for (auto &allocation : this->SVMAllocs.allocations) {
        if (allocation.second.memoryType & requestedTypesMask) {
            auto gpuAllocation = allocation.second.gpuAllocations.getGraphicsAllocation(commandStreamReceiver.getRootDeviceIndex());
            UNRECOVERABLE_IF(nullptr == gpuAllocation);
            commandStreamReceiver.makeResident(*gpuAllocation);
        }
    }
}

SVMAllocsManager::SVMAllocsManager(MemoryManager *memoryManager, bool multiOsContextSupport)
    : memoryManager(memoryManager), multiOsContextSupport(multiOsContextSupport) {
}

void *SVMAllocsManager::createSVMAlloc(size_t size, const SvmAllocationProperties svmProperties,
                                       const std::set<uint32_t> &rootDeviceIndices,
                                       const std::map<uint32_t, DeviceBitfield> &subdeviceBitfields) {
    if (size == 0)
        return nullptr;

    if (!memoryManager->isLocalMemorySupported(*rootDeviceIndices.begin())) {
        return createZeroCopySvmAllocation(size, svmProperties, rootDeviceIndices, subdeviceBitfields);
    } else {
        UnifiedMemoryProperties unifiedMemoryProperties(InternalMemoryType::NOT_SPECIFIED, rootDeviceIndices, subdeviceBitfields);
        return createUnifiedAllocationWithDeviceStorage(size, svmProperties, unifiedMemoryProperties);
    }
}

void *SVMAllocsManager::createHostUnifiedMemoryAllocation(size_t size,
                                                          const UnifiedMemoryProperties &memoryProperties) {
    size_t alignedSize = alignUp<size_t>(size, MemoryConstants::pageSize64k);

    GraphicsAllocation::AllocationType allocationType = getGraphicsAllocationType(memoryProperties);

    std::vector<uint32_t> rootDeviceIndicesVector(memoryProperties.rootDeviceIndices.begin(), memoryProperties.rootDeviceIndices.end());

    uint32_t rootDeviceIndex = rootDeviceIndicesVector.at(0);
    auto &deviceBitfield = memoryProperties.subdeviceBitfields.at(rootDeviceIndex);

    AllocationProperties unifiedMemoryProperties{rootDeviceIndex,
                                                 true,
                                                 alignedSize,
                                                 allocationType,
                                                 false,
                                                 (deviceBitfield.count() > 1) && multiOsContextSupport,
                                                 deviceBitfield};
    unifiedMemoryProperties.flags.shareable = memoryProperties.allocationFlags.flags.shareable;
    unifiedMemoryProperties.flags.isUSMHostAllocation = true;
    unifiedMemoryProperties.flags.isUSMDeviceAllocation = false;
    unifiedMemoryProperties.cacheRegion = MemoryPropertiesHelper::getCacheRegion(memoryProperties.allocationFlags);

    auto maxRootDeviceIndex = *std::max_element(rootDeviceIndicesVector.begin(), rootDeviceIndicesVector.end(), std::less<uint32_t const>());
    SvmAllocationData allocData(maxRootDeviceIndex);

    void *usmPtr = memoryManager->createMultiGraphicsAllocationInSystemMemoryPool(rootDeviceIndicesVector, unifiedMemoryProperties, allocData.gpuAllocations);
    if (!usmPtr) {
        return nullptr;
    }

    allocData.cpuAllocation = nullptr;
    allocData.size = size;
    allocData.memoryType = memoryProperties.memoryType;
    allocData.allocationFlagsProperty = memoryProperties.allocationFlags;
    allocData.device = nullptr;

    std::unique_lock<SpinLock> lock(mtx);
    this->SVMAllocs.insert(allocData);

    return usmPtr;
}

void *SVMAllocsManager::createUnifiedMemoryAllocation(size_t size,
                                                      const UnifiedMemoryProperties &memoryProperties) {
    auto rootDeviceIndex = memoryProperties.device
                               ? memoryProperties.device->getRootDeviceIndex()
                               : *memoryProperties.rootDeviceIndices.begin();
    auto &deviceBitfield = memoryProperties.subdeviceBitfields.at(rootDeviceIndex);

    size_t alignedSize = alignUp<size_t>(size, MemoryConstants::pageSize64k);

    GraphicsAllocation::AllocationType allocationType = getGraphicsAllocationType(memoryProperties);

    AllocationProperties unifiedMemoryProperties{rootDeviceIndex,
                                                 true,
                                                 alignedSize,
                                                 allocationType,
                                                 false,
                                                 (deviceBitfield.count() > 1) && multiOsContextSupport,
                                                 deviceBitfield};
    unifiedMemoryProperties.flags.shareable = memoryProperties.allocationFlags.flags.shareable;
    unifiedMemoryProperties.flags.isUSMDeviceAllocation = true;
    unifiedMemoryProperties.cacheRegion = MemoryPropertiesHelper::getCacheRegion(memoryProperties.allocationFlags);

    if (memoryProperties.memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY) {
        unifiedMemoryProperties.flags.isUSMHostAllocation = true;
        unifiedMemoryProperties.flags.isUSMDeviceAllocation = false;
    }

    GraphicsAllocation *unifiedMemoryAllocation = memoryManager->allocateGraphicsMemoryWithProperties(unifiedMemoryProperties);
    if (!unifiedMemoryAllocation) {
        return nullptr;
    }
    setUnifiedAllocationProperties(unifiedMemoryAllocation, {});

    SvmAllocationData allocData(rootDeviceIndex);
    allocData.gpuAllocations.addAllocation(unifiedMemoryAllocation);
    allocData.cpuAllocation = nullptr;
    allocData.size = size;
    allocData.memoryType = memoryProperties.memoryType;
    allocData.allocationFlagsProperty = memoryProperties.allocationFlags;
    allocData.device = memoryProperties.device;

    std::unique_lock<SpinLock> lock(mtx);
    this->SVMAllocs.insert(allocData);
    return reinterpret_cast<void *>(unifiedMemoryAllocation->getGpuAddress());
}

void *SVMAllocsManager::createSharedUnifiedMemoryAllocation(size_t size,
                                                            const UnifiedMemoryProperties &memoryProperties,
                                                            void *cmdQ) {
    if (memoryProperties.rootDeviceIndices.size() > 1 && !memoryProperties.device) {
        return createHostUnifiedMemoryAllocation(size, memoryProperties);
    }

    auto supportDualStorageSharedMemory = memoryManager->isLocalMemorySupported(*memoryProperties.rootDeviceIndices.begin());

    if (DebugManager.flags.AllocateSharedAllocationsWithCpuAndGpuStorage.get() != -1) {
        supportDualStorageSharedMemory = !!DebugManager.flags.AllocateSharedAllocationsWithCpuAndGpuStorage.get();
    }

    if (supportDualStorageSharedMemory) {
        bool useKmdMigration = memoryManager->isKmdMigrationAvailable(*memoryProperties.rootDeviceIndices.begin());
        void *unifiedMemoryPointer = nullptr;

        if (useKmdMigration) {
            unifiedMemoryPointer = createUnifiedKmdMigratedAllocation(size, {}, memoryProperties);
            if (!unifiedMemoryPointer) {
                return nullptr;
            }
        } else {
            unifiedMemoryPointer = createUnifiedAllocationWithDeviceStorage(size, {}, memoryProperties);
            if (!unifiedMemoryPointer) {
                return nullptr;
            }

            UNRECOVERABLE_IF(cmdQ == nullptr);
            auto pageFaultManager = this->memoryManager->getPageFaultManager();
            pageFaultManager->insertAllocation(unifiedMemoryPointer, size, this, cmdQ, memoryProperties.allocationFlags);
        }

        auto unifiedMemoryAllocation = this->getSVMAlloc(unifiedMemoryPointer);
        unifiedMemoryAllocation->memoryType = memoryProperties.memoryType;
        unifiedMemoryAllocation->allocationFlagsProperty = memoryProperties.allocationFlags;

        return unifiedMemoryPointer;
    }
    return createUnifiedMemoryAllocation(size, memoryProperties);
}

void *SVMAllocsManager::createUnifiedKmdMigratedAllocation(size_t size, const SvmAllocationProperties &svmProperties, const UnifiedMemoryProperties &unifiedMemoryProperties) {

    auto rootDeviceIndex = unifiedMemoryProperties.device
                               ? unifiedMemoryProperties.device->getRootDeviceIndex()
                               : *unifiedMemoryProperties.rootDeviceIndices.begin();
    auto &deviceBitfield = unifiedMemoryProperties.subdeviceBitfields.at(rootDeviceIndex);
    size_t alignedSize = alignUp<size_t>(size, 2 * MemoryConstants::megaByte);
    AllocationProperties gpuProperties{rootDeviceIndex,
                                       true,
                                       alignedSize,
                                       GraphicsAllocation::AllocationType::UNIFIED_SHARED_MEMORY,
                                       false,
                                       false,
                                       deviceBitfield};

    gpuProperties.alignment = 2 * MemoryConstants::megaByte;
    auto cacheRegion = MemoryPropertiesHelper::getCacheRegion(unifiedMemoryProperties.allocationFlags);
    MemoryPropertiesHelper::fillCachePolicyInProperties(gpuProperties, false, svmProperties.readOnly, false, cacheRegion);
    GraphicsAllocation *allocationGpu = memoryManager->allocateGraphicsMemoryWithProperties(gpuProperties);
    if (!allocationGpu) {
        return nullptr;
    }
    setUnifiedAllocationProperties(allocationGpu, svmProperties);

    SvmAllocationData allocData(rootDeviceIndex);
    allocData.gpuAllocations.addAllocation(allocationGpu);
    allocData.cpuAllocation = nullptr;
    allocData.device = unifiedMemoryProperties.device;
    allocData.size = size;

    std::unique_lock<SpinLock> lock(mtx);
    this->SVMAllocs.insert(allocData);
    return allocationGpu->getUnderlyingBuffer();
}

void SVMAllocsManager::setUnifiedAllocationProperties(GraphicsAllocation *allocation, const SvmAllocationProperties &svmProperties) {
    allocation->setMemObjectsAllocationWithWritableFlags(!svmProperties.readOnly && !svmProperties.hostPtrReadOnly);
    allocation->setCoherent(svmProperties.coherent);
}

SvmAllocationData *SVMAllocsManager::getSVMAlloc(const void *ptr) {
    std::unique_lock<SpinLock> lock(mtx);
    return SVMAllocs.get(ptr);
}

void SVMAllocsManager::insertSVMAlloc(const SvmAllocationData &svmAllocData) {
    std::unique_lock<SpinLock> lock(mtx);
    SVMAllocs.insert(svmAllocData);
}

void SVMAllocsManager::removeSVMAlloc(const SvmAllocationData &svmAllocData) {
    std::unique_lock<SpinLock> lock(mtx);
    SVMAllocs.remove(svmAllocData);
}

bool SVMAllocsManager::freeSVMAlloc(void *ptr, bool blocking) {
    SvmAllocationData *svmData = getSVMAlloc(ptr);
    if (svmData) {
        if (blocking) {
            if (svmData->cpuAllocation) {
                this->memoryManager->waitForEnginesCompletion(*svmData->cpuAllocation);
            }

            for (auto &gpuAllocation : svmData->gpuAllocations.getGraphicsAllocations()) {
                if (gpuAllocation) {
                    this->memoryManager->waitForEnginesCompletion(*gpuAllocation);
                }
            }
        }

        auto pageFaultManager = this->memoryManager->getPageFaultManager();
        if (pageFaultManager) {
            pageFaultManager->removeAllocation(ptr);
        }
        std::unique_lock<SpinLock> lock(mtx);
        if (svmData->gpuAllocations.getAllocationType() == GraphicsAllocation::AllocationType::SVM_ZERO_COPY) {
            freeZeroCopySvmAllocation(svmData);
        } else {
            freeSvmAllocationWithDeviceStorage(svmData);
        }
        return true;
    }
    return false;
}

void *SVMAllocsManager::createZeroCopySvmAllocation(size_t size, const SvmAllocationProperties &svmProperties,
                                                    const std::set<uint32_t> &rootDeviceIndices,
                                                    const std::map<uint32_t, DeviceBitfield> &subdeviceBitfields) {

    auto rootDeviceIndex = *rootDeviceIndices.begin();
    auto &deviceBitfield = subdeviceBitfields.at(rootDeviceIndex);
    AllocationProperties properties{rootDeviceIndex,
                                    true, // allocateMemory
                                    size,
                                    GraphicsAllocation::AllocationType::SVM_ZERO_COPY,
                                    false, // isMultiStorageAllocation
                                    deviceBitfield};
    MemoryPropertiesHelper::fillCachePolicyInProperties(properties, false, svmProperties.readOnly, false, properties.cacheRegion);
    GraphicsAllocation *allocation = memoryManager->allocateGraphicsMemoryWithProperties(properties);
    if (!allocation) {
        return nullptr;
    }
    allocation->setMemObjectsAllocationWithWritableFlags(!svmProperties.readOnly && !svmProperties.hostPtrReadOnly);
    allocation->setCoherent(svmProperties.coherent);

    SvmAllocationData allocData(rootDeviceIndex);
    allocData.gpuAllocations.addAllocation(allocation);
    allocData.size = size;

    std::unique_lock<SpinLock> lock(mtx);
    this->SVMAllocs.insert(allocData);
    return allocation->getUnderlyingBuffer();
}

void *SVMAllocsManager::createUnifiedAllocationWithDeviceStorage(size_t size, const SvmAllocationProperties &svmProperties, const UnifiedMemoryProperties &unifiedMemoryProperties) {
    auto rootDeviceIndex = unifiedMemoryProperties.device
                               ? unifiedMemoryProperties.device->getRootDeviceIndex()
                               : *unifiedMemoryProperties.rootDeviceIndices.begin();
    size_t alignedSize = alignUp<size_t>(size, 2 * MemoryConstants::megaByte);
    const DeviceBitfield &subDevices = unifiedMemoryProperties.subdeviceBitfields.at(rootDeviceIndex);
    AllocationProperties cpuProperties{rootDeviceIndex,
                                       true, // allocateMemory
                                       alignedSize, GraphicsAllocation::AllocationType::SVM_CPU,
                                       false, // isMultiStorageAllocation
                                       subDevices};
    cpuProperties.alignment = 2 * MemoryConstants::megaByte;
    auto cacheRegion = MemoryPropertiesHelper::getCacheRegion(unifiedMemoryProperties.allocationFlags);
    MemoryPropertiesHelper::fillCachePolicyInProperties(cpuProperties, false, svmProperties.readOnly, false, cacheRegion);
    GraphicsAllocation *allocationCpu = memoryManager->allocateGraphicsMemoryWithProperties(cpuProperties);
    if (!allocationCpu) {
        return nullptr;
    }
    setUnifiedAllocationProperties(allocationCpu, svmProperties);
    void *svmPtr = allocationCpu->getUnderlyingBuffer();

    AllocationProperties gpuProperties{rootDeviceIndex,
                                       false,
                                       alignedSize,
                                       GraphicsAllocation::AllocationType::SVM_GPU,
                                       false,
                                       (subDevices.count() > 1) && multiOsContextSupport,
                                       subDevices};

    gpuProperties.alignment = 2 * MemoryConstants::megaByte;
    MemoryPropertiesHelper::fillCachePolicyInProperties(gpuProperties, false, svmProperties.readOnly, false, cacheRegion);
    GraphicsAllocation *allocationGpu = memoryManager->allocateGraphicsMemoryWithProperties(gpuProperties, svmPtr);
    if (!allocationGpu) {
        memoryManager->freeGraphicsMemory(allocationCpu);
        return nullptr;
    }
    setUnifiedAllocationProperties(allocationGpu, svmProperties);

    SvmAllocationData allocData(rootDeviceIndex);
    allocData.gpuAllocations.addAllocation(allocationGpu);
    allocData.cpuAllocation = allocationCpu;
    allocData.device = unifiedMemoryProperties.device;
    allocData.size = size;

    std::unique_lock<SpinLock> lock(mtx);
    this->SVMAllocs.insert(allocData);
    return svmPtr;
}

void SVMAllocsManager::freeZeroCopySvmAllocation(SvmAllocationData *svmData) {
    GraphicsAllocation *gpuAllocation = svmData->gpuAllocations.getDefaultGraphicsAllocation();
    SVMAllocs.remove(*svmData);

    memoryManager->freeGraphicsMemory(gpuAllocation);
}

void SVMAllocsManager::freeSvmAllocationWithDeviceStorage(SvmAllocationData *svmData) {
    auto graphicsAllocations = svmData->gpuAllocations.getGraphicsAllocations();
    GraphicsAllocation *cpuAllocation = svmData->cpuAllocation;
    SVMAllocs.remove(*svmData);

    for (auto gpuAllocation : graphicsAllocations) {
        memoryManager->freeGraphicsMemory(gpuAllocation);
    }
    memoryManager->freeGraphicsMemory(cpuAllocation);
}

bool SVMAllocsManager::hasHostAllocations() {
    std::unique_lock<SpinLock> lock(mtx);
    for (auto &allocation : this->SVMAllocs.allocations) {
        if (allocation.second.memoryType == InternalMemoryType::HOST_UNIFIED_MEMORY) {
            return true;
        }
    }
    return false;
}

SvmMapOperation *SVMAllocsManager::getSvmMapOperation(const void *ptr) {
    std::unique_lock<SpinLock> lock(mtx);
    return svmMapOperations.get(ptr);
}

void SVMAllocsManager::insertSvmMapOperation(void *regionSvmPtr, size_t regionSize, void *baseSvmPtr, size_t offset, bool readOnlyMap) {
    SvmMapOperation svmMapOperation;
    svmMapOperation.regionSvmPtr = regionSvmPtr;
    svmMapOperation.baseSvmPtr = baseSvmPtr;
    svmMapOperation.offset = offset;
    svmMapOperation.regionSize = regionSize;
    svmMapOperation.readOnlyMap = readOnlyMap;
    std::unique_lock<SpinLock> lock(mtx);
    svmMapOperations.insert(svmMapOperation);
}

void SVMAllocsManager::removeSvmMapOperation(const void *regionSvmPtr) {
    std::unique_lock<SpinLock> lock(mtx);
    svmMapOperations.remove(regionSvmPtr);
}

} // namespace NEO
