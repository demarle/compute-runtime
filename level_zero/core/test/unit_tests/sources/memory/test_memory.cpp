/*
 * Copyright (C) 2020-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/helpers/string.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"

#include "opencl/test/unit_test/mocks/mock_memory_manager.h"
#include "test.h"

#include "level_zero/core/source/context/context_imp.h"
#include "level_zero/core/source/driver/host_pointer_manager.h"
#include "level_zero/core/test/unit_tests/fixtures/device_fixture.h"
#include "level_zero/core/test/unit_tests/mocks/mock_built_ins.h"

namespace L0 {
namespace ult {

using MemoryTest = Test<DeviceFixture>;

TEST_F(MemoryTest, givenDevicePointerThenDriverGetAllocPropertiesReturnsDeviceHandle) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_device_handle_t deviceHandle;

    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_DEVICE);
    EXPECT_EQ(deviceHandle, device->toHandle());

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}
TEST_F(MemoryTest, whenAllocatingDeviceMemoryWithUncachedFlagThenLocallyUncachedResourceIsSet) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    deviceDesc.flags = ZE_DEVICE_MEM_ALLOC_FLAG_BIAS_UNCACHED;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    auto allocData = driverHandle->getSvmAllocsManager()->getSVMAlloc(ptr);
    EXPECT_NE(nullptr, allocData);
    EXPECT_EQ(allocData->allocationFlagsProperty.flags.locallyUncachedResource, 1u);

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(MemoryTest, whenAllocatingSharedMemoryWithUncachedFlagThenLocallyUncachedResourceIsSet) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    deviceDesc.flags = ZE_DEVICE_MEM_ALLOC_FLAG_BIAS_UNCACHED;
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(device->toHandle(),
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    auto allocData = driverHandle->getSvmAllocsManager()->getSVMAlloc(ptr);
    EXPECT_NE(nullptr, allocData);
    EXPECT_EQ(allocData->allocationFlagsProperty.flags.locallyUncachedResource, 1u);

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

struct SVMAllocsManagerRelaxedSizeMock : public NEO::SVMAllocsManager {
    SVMAllocsManagerRelaxedSizeMock(MemoryManager *memoryManager) : NEO::SVMAllocsManager(memoryManager, false) {}
    void *createUnifiedMemoryAllocation(size_t size,
                                        const UnifiedMemoryProperties &svmProperties) override {
        return alignedMalloc(4096u, 4096u);
    }

    void *createSharedUnifiedMemoryAllocation(size_t size,
                                              const UnifiedMemoryProperties &svmProperties,
                                              void *cmdQ) override {
        return alignedMalloc(4096u, 4096u);
    }
};

struct DriverHandleRelaxedSizeMock : public DriverHandleImp {
    ze_result_t freeMem(const void *ptr) override {
        alignedFree(const_cast<void *>(ptr));
        return ZE_RESULT_SUCCESS;
    }
};

struct MemoryRelaxedSizeTests : public ::testing::Test {
    void SetUp() override {
        neoDevice =
            NEO::MockDevice::createWithNewExecutionEnvironment<NEO::MockDevice>(NEO::defaultHwInfo.get());
        auto mockBuiltIns = new MockBuiltins();
        neoDevice->executionEnvironment->rootDeviceEnvironments[0]->builtins.reset(mockBuiltIns);
        NEO::DeviceVector devices;
        devices.push_back(std::unique_ptr<NEO::Device>(neoDevice));
        driverHandle = std::make_unique<DriverHandleRelaxedSizeMock>();
        driverHandle->initialize(std::move(devices));
        prevSvmAllocsManager = driverHandle->svmAllocsManager;
        currSvmAllocsManager = new SVMAllocsManagerRelaxedSizeMock(driverHandle->memoryManager);
        driverHandle->svmAllocsManager = currSvmAllocsManager;
        device = driverHandle->devices[0];
    }

    void TearDown() override {
        driverHandle->svmAllocsManager = prevSvmAllocsManager;
        delete currSvmAllocsManager;
    }
    NEO::SVMAllocsManager *prevSvmAllocsManager;
    NEO::SVMAllocsManager *currSvmAllocsManager;
    std::unique_ptr<DriverHandleRelaxedSizeMock> driverHandle;
    NEO::MockDevice *neoDevice = nullptr;
    L0::Device *device = nullptr;
};

TEST_F(MemoryRelaxedSizeTests,
       givenCallToDeviceAllocWithAllowedSizeAndWithoutRelaxedFlagThenAllocationIsMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize - 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToDeviceAllocWithLargerThanAllowdSizeAndWithoutRelaxedFlagThenAllocationIsNotMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_SIZE, result);
    EXPECT_EQ(nullptr, ptr);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToDeviceAllocWithLargerThanAllowdSizeAndRelaxedFlagThenAllocationIsMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    deviceDesc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
    ze_relaxed_allocation_limits_exp_desc_t relaxedSizeDesc = {};
    relaxedSizeDesc.stype = ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC;
    relaxedSizeDesc.flags = ZE_RELAXED_ALLOCATION_LIMITS_EXP_FLAG_MAX_SIZE;
    deviceDesc.pNext = &relaxedSizeDesc;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToDeviceAllocWithLargerThanAllowdSizeAndRelaxedFlagWithIncorrectFlagThenAllocationIsNotMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    deviceDesc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
    ze_relaxed_allocation_limits_exp_desc_t relaxedSizeDesc = {};
    relaxedSizeDesc.stype = ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC;
    relaxedSizeDesc.flags = static_cast<ze_relaxed_allocation_limits_exp_flag_t>(ZE_BIT(1));
    deviceDesc.pNext = &relaxedSizeDesc;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, result);
    EXPECT_EQ(nullptr, ptr);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToSharedAllocWithAllowedSizeAndWithoutRelaxedFlagThenAllocationIsMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize - 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(device->toHandle(),
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToSharedAllocWithLargerThanAllowedSizeAndWithoutRelaxedFlagThenAllocationIsNotMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(device->toHandle(),
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_SIZE, result);
    EXPECT_EQ(nullptr, ptr);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToSharedAllocWithLargerThanAllowdSizeAndRelaxedFlagThenAllocationIsMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    deviceDesc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
    ze_relaxed_allocation_limits_exp_desc_t relaxedSizeDesc = {};
    relaxedSizeDesc.stype = ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC;
    relaxedSizeDesc.flags = ZE_RELAXED_ALLOCATION_LIMITS_EXP_FLAG_MAX_SIZE;
    deviceDesc.pNext = &relaxedSizeDesc;
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(device->toHandle(),
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryRelaxedSizeTests,
       givenCallToSharedAllocWithLargerThanAllowdSizeAndRelaxedFlagWithIncorrectFlagThenAllocationIsNotMade) {
    size_t size = device->getNEODevice()->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    deviceDesc.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
    ze_relaxed_allocation_limits_exp_desc_t relaxedSizeDesc = {};
    relaxedSizeDesc.stype = ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC;
    relaxedSizeDesc.flags = static_cast<ze_relaxed_allocation_limits_exp_flag_t>(ZE_BIT(1));
    deviceDesc.pNext = &relaxedSizeDesc;
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(device->toHandle(),
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, result);
    EXPECT_EQ(nullptr, ptr);
}

struct DriverHandleGetFdMock : public DriverHandleImp {
    void *importFdHandle(ze_device_handle_t hDevice, uint64_t handle) override {
        if (mockFd == allocationMap.second) {
            return allocationMap.first;
        }
        return nullptr;
    }

    ze_result_t allocDeviceMem(ze_device_handle_t hDevice, const ze_device_mem_alloc_desc_t *deviceDesc, size_t size,
                               size_t alignment, void **ptr) override {
        ze_result_t res = DriverHandleImp::allocDeviceMem(hDevice, deviceDesc, size, alignment, ptr);
        if (ZE_RESULT_SUCCESS == res) {
            allocationMap.first = *ptr;
            allocationMap.second = mockFd;
        }

        return res;
    }

    ze_result_t getMemAllocProperties(const void *ptr,
                                      ze_memory_allocation_properties_t *pMemAllocProperties,
                                      ze_device_handle_t *phDevice) override {
        ze_result_t res = DriverHandleImp::getMemAllocProperties(ptr, pMemAllocProperties, phDevice);
        if (ZE_RESULT_SUCCESS == res && pMemAllocProperties->pNext) {
            ze_external_memory_export_fd_t *extendedMemoryExportProperties =
                reinterpret_cast<ze_external_memory_export_fd_t *>(pMemAllocProperties->pNext);
            extendedMemoryExportProperties->fd = mockFd;
        }

        return res;
    }

    const int mockFd = 57;
    std::pair<void *, int> allocationMap;
};

struct MemoryExportImportTest : public ::testing::Test {
    void SetUp() override {
        neoDevice = NEO::MockDevice::createWithNewExecutionEnvironment<NEO::MockDevice>(NEO::defaultHwInfo.get());
        auto mockBuiltIns = new MockBuiltins();
        neoDevice->executionEnvironment->rootDeviceEnvironments[0]->builtins.reset(mockBuiltIns);
        NEO::DeviceVector devices;
        devices.push_back(std::unique_ptr<NEO::Device>(neoDevice));
        driverHandle = std::make_unique<DriverHandleGetFdMock>();
        driverHandle->initialize(std::move(devices));
        device = driverHandle->devices[0];
    }

    void TearDown() override {
    }
    std::unique_ptr<DriverHandleGetFdMock> driverHandle;
    NEO::MockDevice *neoDevice = nullptr;
    L0::Device *device = nullptr;
};

TEST_F(MemoryExportImportTest,
       givenCallToDeviceAllocWithExtendedExportDescriptorAndNonSupportedFlagThenUnsuportedEnumerationIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_external_memory_export_desc_t extendedDesc = {};
    extendedDesc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC;
    extendedDesc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD;
    deviceDesc.pNext = &extendedDesc;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION, result);
    EXPECT_EQ(nullptr, ptr);
}

TEST_F(MemoryExportImportTest,
       givenCallToDeviceAllocWithExtendedExportDescriptorAndSupportedFlagThenAllocationIsMade) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_external_memory_export_desc_t extendedDesc = {};
    extendedDesc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC;
    extendedDesc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    deviceDesc.pNext = &extendedDesc;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryExportImportTest,
       givenCallToMemAllocPropertiesWithExtendedExportPropertiesAndUnsupportedFlagThenUnsupportedEnumerationIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_external_memory_export_fd_t extendedProperties = {};
    extendedProperties.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD;
    extendedProperties.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD;
    extendedProperties.fd = std::numeric_limits<int>::max();
    memoryProperties.pNext = &extendedProperties;

    ze_device_handle_t deviceHandle;
    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION, result);
    EXPECT_EQ(extendedProperties.fd, std::numeric_limits<int>::max());

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryExportImportTest,
       givenCallToMemAllocPropertiesWithExtendedExportPropertiesForNonDeviceAllocationThenUnsupportedFeatureIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocHostMem(&hostDesc, size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_external_memory_export_fd_t extendedProperties = {};
    extendedProperties.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD;
    extendedProperties.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    extendedProperties.fd = std::numeric_limits<int>::max();
    memoryProperties.pNext = &extendedProperties;

    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, nullptr);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_FEATURE, result);
    EXPECT_EQ(extendedProperties.fd, std::numeric_limits<int>::max());

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryExportImportTest,
       givenCallToMemAllocPropertiesWithExtendedExportPropertiesAndSupportedFlagThenValidFileDescriptorIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_external_memory_export_fd_t extendedProperties = {};
    extendedProperties.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD;
    extendedProperties.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    extendedProperties.fd = std::numeric_limits<int>::max();
    memoryProperties.pNext = &extendedProperties;

    ze_device_handle_t deviceHandle;
    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_DEVICE);
    EXPECT_EQ(deviceHandle, device->toHandle());
    EXPECT_NE(extendedProperties.fd, std::numeric_limits<int>::max());
    EXPECT_EQ(extendedProperties.fd, driverHandle->mockFd);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryExportImportTest,
       givenCallToDeviceAllocWithExtendedImportDescriptorAndNonSupportedFlagThenUnsupportedEnumerationIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_external_memory_export_desc_t extendedDesc = {};
    extendedDesc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC;
    extendedDesc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    deviceDesc.pNext = &extendedDesc;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_external_memory_export_fd_t extendedProperties = {};
    extendedProperties.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD;
    extendedProperties.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    extendedProperties.fd = std::numeric_limits<int>::max();
    memoryProperties.pNext = &extendedProperties;

    ze_device_handle_t deviceHandle;
    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_DEVICE);
    EXPECT_EQ(deviceHandle, device->toHandle());
    EXPECT_NE(extendedProperties.fd, std::numeric_limits<int>::max());
    EXPECT_EQ(extendedProperties.fd, driverHandle->mockFd);

    ze_device_mem_alloc_desc_t importDeviceDesc = {};
    ze_external_memory_import_fd_t extendedImportDesc = {};
    extendedImportDesc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMPORT_FD;
    extendedImportDesc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD;
    extendedImportDesc.fd = extendedProperties.fd;
    importDeviceDesc.pNext = &extendedImportDesc;

    void *importedPtr = nullptr;
    result = driverHandle->allocDeviceMem(device->toHandle(),
                                          &importDeviceDesc,
                                          size, alignment, &importedPtr);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION, result);
    EXPECT_EQ(nullptr, importedPtr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryExportImportTest,
       givenCallToDeviceAllocWithExtendedImportDescriptorAndSupportedFlagThenSuccessIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_external_memory_export_desc_t extendedDesc = {};
    extendedDesc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC;
    extendedDesc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    deviceDesc.pNext = &extendedDesc;
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_external_memory_export_fd_t extendedProperties = {};
    extendedProperties.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD;
    extendedProperties.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    extendedProperties.fd = std::numeric_limits<int>::max();
    memoryProperties.pNext = &extendedProperties;

    ze_device_handle_t deviceHandle;
    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_DEVICE);
    EXPECT_EQ(deviceHandle, device->toHandle());
    EXPECT_NE(extendedProperties.fd, std::numeric_limits<int>::max());
    EXPECT_EQ(extendedProperties.fd, driverHandle->mockFd);

    ze_device_mem_alloc_desc_t importDeviceDesc = {};
    ze_external_memory_import_fd_t extendedImportDesc = {};
    extendedImportDesc.stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMPORT_FD;
    extendedImportDesc.flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF;
    extendedImportDesc.fd = extendedProperties.fd;
    importDeviceDesc.pNext = &extendedImportDesc;

    void *importedPtr = nullptr;
    result = driverHandle->allocDeviceMem(device->toHandle(),
                                          &importDeviceDesc,
                                          size, alignment, &importedPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

using MemoryIPCTests = MemoryExportImportTest;

TEST_F(MemoryIPCTests,
       givenCallToGetIpcHandleWithNotKnownPointerThenInvalidArgumentIsReturned) {

    uint32_t value = 0;

    ze_ipc_mem_handle_t ipcHandle;
    ze_result_t result = driverHandle->getIpcMemHandle(&value, &ipcHandle);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, result);
}

TEST_F(MemoryIPCTests,
       givenCallToGetIpcHandleWithDeviceAllocationThenIpcHandleIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_ipc_mem_handle_t ipcHandle;
    result = driverHandle->getIpcMemHandle(ptr, &ipcHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryIPCTests,
       whenCallingOpenIpcHandleWithIpcHandleThenDeviceAllocationIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_ipc_mem_handle_t ipcHandle = {};
    result = driverHandle->getIpcMemHandle(ptr, &ipcHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    ze_ipc_memory_flag_t flags = {};
    void *ipcPtr;
    result = driverHandle->openIpcMemHandle(device->toHandle(), ipcHandle, flags, &ipcPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(ipcPtr, ptr);

    result = driverHandle->closeIpcMemHandle(ipcPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryIPCTests,
       whenCallingOpenIpcHandleWithIpcHandleAndUsingContextThenDeviceAllocationIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_context_handle_t hContext;
    ze_context_desc_t desc;

    ze_result_t result = driverHandle->createContext(&desc, 0u, nullptr, &hContext);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    ContextImp *contextImp = static_cast<ContextImp *>(L0::Context::fromHandle(hContext));

    ze_device_mem_alloc_desc_t deviceDesc = {};
    result = contextImp->allocDeviceMem(device->toHandle(),
                                        &deviceDesc,
                                        size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_ipc_mem_handle_t ipcHandle = {};
    result = contextImp->getIpcMemHandle(ptr, &ipcHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    ze_ipc_memory_flag_t flags = {};
    void *ipcPtr;
    result = contextImp->openIpcMemHandle(device->toHandle(), ipcHandle, flags, &ipcPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(ipcPtr, ptr);

    result = contextImp->closeIpcMemHandle(ipcPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = contextImp->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = contextImp->destroy();
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryIPCTests,
       givenCallingGetIpcHandleWithDeviceAllocationAndUsingContextThenIpcHandleIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_context_handle_t hContext;
    ze_context_desc_t desc;

    ze_result_t result = driverHandle->createContext(&desc, 0u, nullptr, &hContext);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    ContextImp *contextImp = static_cast<ContextImp *>(L0::Context::fromHandle(hContext));

    ze_device_mem_alloc_desc_t deviceDesc = {};
    result = contextImp->allocDeviceMem(device->toHandle(),
                                        &deviceDesc,
                                        size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_ipc_mem_handle_t ipcHandle;
    result = contextImp->getIpcMemHandle(ptr, &ipcHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = contextImp->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    result = contextImp->destroy();
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

TEST_F(MemoryIPCTests,
       whenCallingOpenIpcHandleWithIncorrectHandleThenInvalidArgumentIsReturned) {
    ze_ipc_mem_handle_t ipcHandle = {};
    ze_ipc_memory_flag_t flags = {};
    void *ipcPtr;
    ze_result_t res = driverHandle->openIpcMemHandle(device->toHandle(), ipcHandle, flags, &ipcPtr);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, res);
}

struct DriverHandleGetIpcHandleMock : public DriverHandleImp {
    void *importFdHandle(ze_device_handle_t hDevice, uint64_t handle) override {
        EXPECT_EQ(handle, static_cast<uint64_t>(mockFd));
        if (mockFd == allocationMap.second) {
            return allocationMap.first;
        }
        return nullptr;
    }

    ze_result_t getIpcMemHandle(const void *ptr, ze_ipc_mem_handle_t *pIpcHandle) override {
        uint64_t handle = mockFd;
        memcpy_s(reinterpret_cast<void *>(pIpcHandle->data),
                 sizeof(ze_ipc_mem_handle_t),
                 &handle,
                 sizeof(handle));

        return ZE_RESULT_SUCCESS;
    }

    ze_result_t allocDeviceMem(ze_device_handle_t hDevice, const ze_device_mem_alloc_desc_t *deviceDesc, size_t size,
                               size_t alignment, void **ptr) override {
        ze_result_t res = DriverHandleImp::allocDeviceMem(hDevice, deviceDesc, size, alignment, ptr);
        if (ZE_RESULT_SUCCESS == res) {
            allocationMap.first = *ptr;
            allocationMap.second = mockFd;
        }

        return res;
    }

    const int mockFd = 999;
    std::pair<void *, int> allocationMap;
};

struct MemoryGetIpcHandleTest : public ::testing::Test {
    void SetUp() override {
        neoDevice = NEO::MockDevice::createWithNewExecutionEnvironment<NEO::MockDevice>(NEO::defaultHwInfo.get());
        auto mockBuiltIns = new MockBuiltins();
        neoDevice->executionEnvironment->rootDeviceEnvironments[0]->builtins.reset(mockBuiltIns);
        NEO::DeviceVector devices;
        devices.push_back(std::unique_ptr<NEO::Device>(neoDevice));
        driverHandle = std::make_unique<DriverHandleGetIpcHandleMock>();
        driverHandle->initialize(std::move(devices));
        device = driverHandle->devices[0];
    }

    void TearDown() override {
    }
    std::unique_ptr<DriverHandleGetIpcHandleMock> driverHandle;
    NEO::MockDevice *neoDevice = nullptr;
    L0::Device *device = nullptr;
};

TEST_F(MemoryGetIpcHandleTest,
       whenCallingOpenIpcHandleWithIpcHandleThenFdHandleIsCorrectlyRead) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(device->toHandle(),
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_ipc_mem_handle_t ipcHandle = {};
    result = driverHandle->getIpcMemHandle(ptr, &ipcHandle);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);

    ze_ipc_memory_flag_t flags = {};
    void *ipcPtr;
    result = driverHandle->openIpcMemHandle(device->toHandle(), ipcHandle, flags, &ipcPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(ipcPtr, ptr);

    result = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
}

using DeviceMemorySizeTest = Test<DeviceFixture>;

TEST_F(DeviceMemorySizeTest, givenSizeGreaterThanLimitThenDeviceAllocationFails) {
    size_t size = neoDevice->getHardwareCapabilities().maxMemAllocSize + 1;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = driverHandle->allocDeviceMem(nullptr,
                                                      &deviceDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_UNSUPPORTED_SIZE, result);
}

TEST_F(MemoryTest, givenSharedPointerThenDriverGetAllocPropertiesReturnsDeviceHandle) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(device->toHandle(),
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_device_handle_t deviceHandle;

    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_SHARED);
    EXPECT_EQ(deviceHandle, device->toHandle());

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(MemoryTest, givenHostPointerThenDriverGetAllocPropertiesReturnsNullDevice) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocHostMem(&hostDesc,
                                                    size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_device_handle_t deviceHandle;

    result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_HOST);
    EXPECT_EQ(deviceHandle, nullptr);

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(MemoryTest, givenSystemAllocatedPointerThenDriverGetAllocPropertiesReturnsUnknownType) {
    size_t size = 10;
    int *ptr = new int[size];

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_device_handle_t deviceHandle;
    ze_result_t result = driverHandle->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_UNKNOWN);

    delete[] ptr;
}

TEST_F(MemoryTest, givenSharedPointerAndDeviceHandleAsNullThenDriverReturnsSuccessAndReturnsPointerToSharedAllocation) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ASSERT_NE(nullptr, device->toHandle());
    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(nullptr,
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(MemoryTest, givenNoDeviceWhenAllocatingSharedMemoryThenDeviceInAllocationIsNullptr) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ASSERT_NE(nullptr, device->toHandle());
    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocSharedMem(nullptr,
                                                      &deviceDesc,
                                                      &hostDesc,
                                                      size, alignment, &ptr);
    auto alloc = driverHandle->svmAllocsManager->getSVMAlloc(ptr);
    EXPECT_EQ(alloc->device, nullptr);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(MemoryTest, givenCallToCheckMemoryAccessFromDeviceWithInvalidPointerThenInvalidArgumentIsReturned) {
    void *ptr = nullptr;
    ze_result_t res = driverHandle->checkMemoryAccessFromDevice(device, ptr);
    EXPECT_EQ(ZE_RESULT_ERROR_INVALID_ARGUMENT, res);
}

TEST_F(MemoryTest, givenCallToCheckMemoryAccessFromDeviceWithValidDeviceAllocationPointerThenSuccessIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t res = driverHandle->allocDeviceMem(device->toHandle(),
                                                   &deviceDesc,
                                                   size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
    EXPECT_NE(nullptr, ptr);

    res = driverHandle->checkMemoryAccessFromDevice(device, ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    res = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
}

TEST_F(MemoryTest, givenCallToCheckMemoryAccessFromDeviceWithValidSharedAllocationPointerThenSuccessIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t res = driverHandle->allocSharedMem(device->toHandle(),
                                                   &deviceDesc,
                                                   &hostDesc,
                                                   size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
    EXPECT_NE(nullptr, ptr);

    res = driverHandle->checkMemoryAccessFromDevice(device, ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    res = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
}

TEST_F(MemoryTest, givenCallToCheckMemoryAccessFromDeviceWithValidHostAllocationPointerThenSuccessIsReturned) {
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t res = driverHandle->allocHostMem(&hostDesc,
                                                 size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
    EXPECT_NE(nullptr, ptr);

    res = driverHandle->checkMemoryAccessFromDevice(device, ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);

    res = driverHandle->freeMem(ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, res);
}

struct MemoryBitfieldTest : testing::Test {
    void SetUp() override {
        auto executionEnvironment = new NEO::ExecutionEnvironment();
        executionEnvironment->prepareRootDeviceEnvironments(1);
        executionEnvironment->rootDeviceEnvironments[0]->setHwInfo(defaultHwInfo.get());
        memoryManager = new NEO::MockMemoryManager(*executionEnvironment);
        executionEnvironment->memoryManager.reset(memoryManager);
        neoDevice = new NEO::RootDevice(executionEnvironment, 0);
        static_cast<RootDevice *>(neoDevice)->createDeviceImpl();
        NEO::DeviceVector devices;
        devices.push_back(std::unique_ptr<NEO::Device>(neoDevice));
        driverHandle = std::make_unique<Mock<L0::DriverHandleImp>>();
        driverHandle->initialize(std::move(devices));
        device = driverHandle->devices[0];
        memoryManager->recentlyPassedDeviceBitfield = {};

        ASSERT_NE(nullptr, driverHandle->devices[0]->toHandle());
        EXPECT_NE(neoDevice->getDeviceBitfield(), memoryManager->recentlyPassedDeviceBitfield);
    }

    void TearDown() override {
        auto result = driverHandle->freeMem(ptr);
        ASSERT_EQ(result, ZE_RESULT_SUCCESS);
    }

    std::unique_ptr<Mock<L0::DriverHandleImp>> driverHandle;
    NEO::Device *neoDevice = nullptr;
    L0::Device *device = nullptr;
    NEO::MockMemoryManager *memoryManager;
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;
};

TEST_F(MemoryBitfieldTest, givenDeviceWithValidBitfieldWhenAllocatingDeviceMemoryThenPassProperBitfield) {
    ze_device_mem_alloc_desc_t deviceDesc = {};
    auto result = driverHandle->allocDeviceMem(device->toHandle(),
                                               &deviceDesc,
                                               size, alignment, &ptr);
    EXPECT_EQ(neoDevice->getDeviceBitfield(), memoryManager->recentlyPassedDeviceBitfield);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);
}
TEST(MemoryBitfieldTests, givenDeviceWithValidBitfieldWhenAllocatingSharedMemoryThenPassProperBitfield) {
    DebugManagerStateRestore restorer;
    size_t size = 10;
    size_t alignment = 1u;
    void *ptr = nullptr;
    auto executionEnvironment = new NEO::ExecutionEnvironment();
    executionEnvironment->prepareRootDeviceEnvironments(2);
    for (size_t i = 0; i < executionEnvironment->rootDeviceEnvironments.size(); i++) {
        executionEnvironment->rootDeviceEnvironments[i]->setHwInfo(defaultHwInfo.get());
    }
    auto memoryManager = new NEO::MockMemoryManager(*executionEnvironment);
    executionEnvironment->memoryManager.reset(memoryManager);
    NEO::Device *neoDevice0 = new NEO::RootDevice(executionEnvironment, 0);
    static_cast<NEO::RootDevice *>(neoDevice0)->createDeviceImpl();
    DebugManager.flags.CreateMultipleSubDevices.set(4);
    NEO::Device *neoDevice1 = new NEO::RootDevice(executionEnvironment, 1);
    static_cast<NEO::RootDevice *>(neoDevice1)->createDeviceImpl();
    NEO::DeviceVector devices;
    devices.push_back(std::unique_ptr<NEO::Device>(neoDevice0));
    devices.push_back(std::unique_ptr<NEO::Device>(neoDevice1));
    auto driverHandle = std::make_unique<Mock<L0::DriverHandleImp>>();
    driverHandle->initialize(std::move(devices));

    memoryManager->recentlyPassedDeviceBitfield = {};
    ASSERT_NE(nullptr, driverHandle->devices[1]->toHandle());
    EXPECT_NE(neoDevice0->getDeviceBitfield(), neoDevice1->getDeviceBitfield());
    EXPECT_NE(neoDevice0->getDeviceBitfield(), memoryManager->recentlyPassedDeviceBitfield);
    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    auto result = driverHandle->allocSharedMem(nullptr,
                                               &deviceDesc,
                                               &hostDesc,
                                               size, alignment, &ptr);
    EXPECT_EQ(neoDevice0->getDeviceBitfield(), memoryManager->recentlyPassedDeviceBitfield);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);
    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);

    memoryManager->recentlyPassedDeviceBitfield = {};
    EXPECT_NE(neoDevice1->getDeviceBitfield(), memoryManager->recentlyPassedDeviceBitfield);
    result = driverHandle->allocSharedMem(driverHandle->devices[1]->toHandle(),
                                          &deviceDesc,
                                          &hostDesc,
                                          size, alignment, &ptr);
    EXPECT_EQ(neoDevice1->getDeviceBitfield(), memoryManager->recentlyPassedDeviceBitfield);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);
    result = driverHandle->freeMem(ptr);
    ASSERT_EQ(result, ZE_RESULT_SUCCESS);
}

struct AllocHostMemoryTest : public ::testing::Test {
    void SetUp() override {
        std::vector<std::unique_ptr<NEO::Device>> devices;
        NEO::ExecutionEnvironment *executionEnvironment = new NEO::ExecutionEnvironment();
        executionEnvironment->prepareRootDeviceEnvironments(numRootDevices);
        for (auto i = 0u; i < executionEnvironment->rootDeviceEnvironments.size(); i++) {
            executionEnvironment->rootDeviceEnvironments[i]->setHwInfo(NEO::defaultHwInfo.get());
        }

        for (auto i = 0u; i < executionEnvironment->rootDeviceEnvironments.size(); i++) {
            devices.push_back(std::unique_ptr<NEO::MockDevice>(NEO::MockDevice::createWithExecutionEnvironment<NEO::MockDevice>(NEO::defaultHwInfo.get(),
                                                                                                                                executionEnvironment, i)));
        }

        driverHandle = std::make_unique<Mock<L0::DriverHandleImp>>();
        driverHandle->initialize(std::move(devices));
    }

    DebugManagerStateRestore restorer;
    std::unique_ptr<Mock<L0::DriverHandleImp>> driverHandle;
    const uint32_t numRootDevices = 2u;
};

TEST_F(AllocHostMemoryTest,
       whenCallingAllocHostMemThenAllocateGraphicsMemoryWithPropertiesIsCalledTheNumberOfTimesOfRootDevices) {
    void *ptr = nullptr;

    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->isMockHostMemoryManager = true;
    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->allocateGraphicsMemoryWithPropertiesCount = 0;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocHostMem(&hostDesc,
                                                    4096u, 0u, &ptr);
    EXPECT_EQ(static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->allocateGraphicsMemoryWithPropertiesCount, numRootDevices);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    driverHandle->freeMem(ptr);
}

TEST_F(AllocHostMemoryTest,
       whenCallingAllocHostMemAndFailingOnCreatingGraphicsAllocationThenNullIsReturned) {

    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->isMockHostMemoryManager = true;
    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->forceFailureInPrimaryAllocation = true;

    void *ptr = nullptr;

    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->allocateGraphicsMemoryWithPropertiesCount = 0;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocHostMem(&hostDesc,
                                                    4096u, 0u, &ptr);
    EXPECT_EQ(static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->allocateGraphicsMemoryWithPropertiesCount, 1u);
    EXPECT_EQ(ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY, result);
    EXPECT_EQ(nullptr, ptr);
}

TEST_F(AllocHostMemoryTest,
       whenCallingAllocHostMemAndFailingOnCreatingGraphicsAllocationWithHostPointerThenNullIsReturned) {

    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->isMockHostMemoryManager = true;
    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->forceFailureInAllocationWithHostPointer = true;

    void *ptr = nullptr;

    static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->allocateGraphicsMemoryWithPropertiesCount = 0;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = driverHandle->allocHostMem(&hostDesc,
                                                    4096u, 0u, &ptr);
    EXPECT_EQ(static_cast<MockMemoryManager *>(driverHandle.get()->getMemoryManager())->allocateGraphicsMemoryWithPropertiesCount, numRootDevices);
    EXPECT_EQ(ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY, result);
    EXPECT_EQ(nullptr, ptr);
}

using ContextMemoryTest = Test<ContextFixture>;

TEST_F(ContextMemoryTest, whenAllocatingSharedAllocationFromContextThenAllocationSucceeds) {
    size_t size = 10u;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = context->allocSharedMem(device->toHandle(),
                                                 &deviceDesc,
                                                 &hostDesc,
                                                 size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = context->freeMem(ptr);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(ContextMemoryTest, whenAllocatingHostAllocationFromContextThenAllocationSucceeds) {
    size_t size = 10u;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_host_mem_alloc_desc_t hostDesc = {};
    ze_result_t result = context->allocHostMem(&hostDesc,
                                               size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = context->freeMem(ptr);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(ContextMemoryTest, whenAllocatingDeviceAllocationFromContextThenAllocationSucceeds) {
    size_t size = 10u;
    size_t alignment = 1u;
    void *ptr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = context->allocDeviceMem(device->toHandle(),
                                                 &deviceDesc,
                                                 size, alignment, &ptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, ptr);

    result = context->freeMem(ptr);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(ContextMemoryTest, whenRetrievingAddressRangeForDeviceAllocationThenRangeIsCorrect) {
    size_t allocSize = 4096u;
    size_t alignment = 1u;
    void *allocPtr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = context->allocDeviceMem(device->toHandle(),
                                                 &deviceDesc,
                                                 allocSize, alignment, &allocPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, allocPtr);

    void *base = nullptr;
    size_t size = 0u;
    void *pPtr = reinterpret_cast<void *>(reinterpret_cast<uint64_t>(allocPtr) + 77);
    result = context->getMemAddressRange(pPtr, &base, &size);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(base, allocPtr);
    EXPECT_GE(size, allocSize);

    result = context->freeMem(allocPtr);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(ContextMemoryTest, whenRetrievingAddressRangeForDeviceAllocationWithNoBaseArgumentThenSizeIsCorrectAndSuccessIsReturned) {
    size_t allocSize = 4096u;
    size_t alignment = 1u;
    void *allocPtr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = context->allocDeviceMem(device->toHandle(),
                                                 &deviceDesc,
                                                 allocSize, alignment, &allocPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, allocPtr);

    size_t size = 0u;
    void *pPtr = reinterpret_cast<void *>(reinterpret_cast<uint64_t>(allocPtr) + 77);
    result = context->getMemAddressRange(pPtr, nullptr, &size);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_GE(size, allocSize);

    result = context->freeMem(allocPtr);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(ContextMemoryTest, whenRetrievingAddressRangeForDeviceAllocationWithNoSizeArgumentThenRangeIsCorrectAndSuccessIsReturned) {
    size_t allocSize = 4096u;
    size_t alignment = 1u;
    void *allocPtr = nullptr;

    ze_device_mem_alloc_desc_t deviceDesc = {};
    ze_result_t result = context->allocDeviceMem(device->toHandle(),
                                                 &deviceDesc,
                                                 allocSize, alignment, &allocPtr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_NE(nullptr, allocPtr);

    void *base = nullptr;
    void *pPtr = reinterpret_cast<void *>(reinterpret_cast<uint64_t>(allocPtr) + 77);
    result = context->getMemAddressRange(pPtr, &base, nullptr);
    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(base, allocPtr);

    result = context->freeMem(allocPtr);
    EXPECT_EQ(result, ZE_RESULT_SUCCESS);
}

TEST_F(ContextMemoryTest, whenRetrievingAddressRangeForUnknownDeviceAllocationThenResultUnknownIsReturned) {
    void *base = nullptr;
    size_t size = 0u;
    uint64_t var = 0;
    ze_result_t res = context->getMemAddressRange(&var, &base, &size);
    EXPECT_EQ(ZE_RESULT_ERROR_UNKNOWN, res);
}

TEST_F(ContextMemoryTest, givenSystemAllocatedPointerThenGetAllocPropertiesReturnsUnknownType) {
    size_t size = 10;
    int *ptr = new int[size];

    ze_memory_allocation_properties_t memoryProperties = {};
    ze_device_handle_t deviceHandle;
    ze_result_t result = context->getMemAllocProperties(ptr, &memoryProperties, &deviceHandle);

    EXPECT_EQ(ZE_RESULT_SUCCESS, result);
    EXPECT_EQ(memoryProperties.type, ZE_MEMORY_TYPE_UNKNOWN);

    delete[] ptr;
}

} // namespace ult
} // namespace L0
