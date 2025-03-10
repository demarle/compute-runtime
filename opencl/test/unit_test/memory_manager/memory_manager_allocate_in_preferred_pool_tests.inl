/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/memory_manager/os_agnostic_memory_manager.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/mocks/mock_graphics_allocation.h"

#include "opencl/test/unit_test/helpers/unit_test_helper.h"
#include "opencl/test/unit_test/mocks/mock_allocation_properties.h"
#include "opencl/test/unit_test/mocks/mock_execution_environment.h"
#include "opencl/test/unit_test/mocks/mock_memory_manager.h"
#include "opencl/test/unit_test/mocks/mock_os_context.h"
#include "test.h"

using namespace NEO;
class MemoryManagerGetAlloctionDataTest : public testing::TestWithParam<GraphicsAllocation::AllocationType> {
  public:
    void SetUp() override {}
    void TearDown() override {}
};

using MemoryManagerGetAlloctionDataTests = ::testing::Test;

TEST_F(MemoryManagerGetAlloctionDataTests, givenHostMemoryAllocationTypeAndAllocateMemoryFlagAndNullptrWhenAllocationDataIsQueriedThenCorrectFlagsAndSizeAreSet) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY, false, mockDeviceBitfield);
    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.useSystemMemory);
    EXPECT_EQ(10u, allocData.size);
    EXPECT_EQ(nullptr, allocData.hostPtr);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenNonHostMemoryAllocatoinTypeWhenAllocationDataIsQueriedThenUseSystemMemoryFlagsIsNotSet) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::BUFFER, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_EQ(10u, allocData.size);
    EXPECT_EQ(nullptr, allocData.hostPtr);
}

HWTEST_F(MemoryManagerGetAlloctionDataTests, givenCommandBufferAllocationTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::COMMAND_BUFFER, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenAllocateMemoryFlagTrueWhenHostPtrIsNotNullThenAllocationDataHasHostPtrNulled) {
    AllocationData allocData;
    char memory = 0;
    AllocationProperties properties(mockRootDeviceIndex, true, sizeof(memory), GraphicsAllocation::AllocationType::BUFFER, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, &memory, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_EQ(sizeof(memory), allocData.size);
    EXPECT_EQ(nullptr, allocData.hostPtr);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenBufferTypeWhenAllocationDataIsQueriedThenForcePinFlagIsSet) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::BUFFER, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.forcePin);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenBufferHostMemoryTypeWhenAllocationDataIsQueriedThenForcePinFlagIsSet) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.forcePin);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenBufferCompressedTypeWhenAllocationDataIsQueriedThenForcePinFlagIsSet) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::BUFFER_COMPRESSED, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.forcePin);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenWriteCombinedTypeWhenAllocationDataIsQueriedThenForcePinFlagIsSet) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, true, 10, GraphicsAllocation::AllocationType::WRITE_COMBINED, false, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.forcePin);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenDefaultAllocationFlagsWhenAllocationDataIsQueriedThenAllocateMemoryIsFalse) {
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, false, 0, GraphicsAllocation::AllocationType::BUFFER_COMPRESSED, false, mockDeviceBitfield);
    char memory;
    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, &memory, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_FALSE(allocData.flags.allocateMemory);
}

TEST_F(MemoryManagerGetAlloctionDataTests, givenDebugModeWhenCertainAllocationTypesAreSelectedThenSystemPlacementIsChoosen) {
    DebugManagerStateRestore restorer;
    auto allocationType = GraphicsAllocation::AllocationType::BUFFER;
    auto mask = 1llu << (static_cast<int64_t>(allocationType) - 1);
    DebugManager.flags.ForceSystemMemoryPlacement.set(mask);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 0, allocationType, mockDeviceBitfield);
    allocData.flags.useSystemMemory = false;

    MockMemoryManager::overrideAllocationData(allocData, properties);
    EXPECT_TRUE(allocData.flags.useSystemMemory);

    allocData.flags.useSystemMemory = false;
    allocationType = GraphicsAllocation::AllocationType::WRITE_COMBINED;
    mask |= 1llu << (static_cast<int64_t>(allocationType) - 1);
    DebugManager.flags.ForceSystemMemoryPlacement.set(mask);

    AllocationProperties properties2(mockRootDeviceIndex, 0, allocationType, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocData, properties2);
    EXPECT_TRUE(allocData.flags.useSystemMemory);

    allocData.flags.useSystemMemory = false;

    MockMemoryManager::overrideAllocationData(allocData, properties);
    EXPECT_TRUE(allocData.flags.useSystemMemory);

    allocData.flags.useSystemMemory = false;
    allocationType = GraphicsAllocation::AllocationType::IMAGE;
    mask = 1llu << (static_cast<int64_t>(allocationType) - 1);
    DebugManager.flags.ForceSystemMemoryPlacement.set(mask);

    MockMemoryManager::overrideAllocationData(allocData, properties);
    EXPECT_FALSE(allocData.flags.useSystemMemory);
}

typedef MemoryManagerGetAlloctionDataTest MemoryManagerGetAlloctionData32BitAnd64kbPagesAllowedTest;

TEST_P(MemoryManagerGetAlloctionData32BitAnd64kbPagesAllowedTest, givenAllocationTypesWith32BitAnd64kbPagesAllowedWhenAllocationDataIsQueriedThenProperFlagsAreSet) {
    AllocationData allocData;
    auto allocType = GetParam();
    AllocationProperties properties(mockRootDeviceIndex, 0, allocType, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.mockExecutionEnvironment->initGmm();
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_TRUE(allocData.flags.allow32Bit);
    EXPECT_TRUE(allocData.flags.allow64kbPages);
    EXPECT_EQ(allocType, allocData.type);
}

TEST_P(MemoryManagerGetAlloctionData32BitAnd64kbPagesAllowedTest, given64kbAllowedAllocationTypeWhenAllocatingThenPreferRenderCompressionOnlyForSpecificTypes) {
    auto allocType = GetParam();
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, allocType, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager(true, false);
    mockMemoryManager.mockExecutionEnvironment->initGmm();
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    bool bufferCompressedType = (allocType == GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
    EXPECT_TRUE(allocData.flags.allow64kbPages);
    auto allocation = mockMemoryManager.allocateGraphicsMemory(allocData);

    EXPECT_TRUE(mockMemoryManager.allocation64kbPageCreated);
    EXPECT_EQ(mockMemoryManager.preferRenderCompressedFlagPassed, bufferCompressedType);

    mockMemoryManager.freeGraphicsMemory(allocation);
}

typedef MemoryManagerGetAlloctionDataTest MemoryManagerGetAlloctionData32BitAnd64kbPagesNotAllowedTest;

TEST_P(MemoryManagerGetAlloctionData32BitAnd64kbPagesNotAllowedTest, givenAllocationTypesWith32BitAnd64kbPagesDisallowedWhenAllocationDataIsQueriedThenFlagsAreNotSet) {
    AllocationData allocData;
    auto allocType = GetParam();
    AllocationProperties properties(mockRootDeviceIndex, 0, allocType, mockDeviceBitfield);

    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));

    EXPECT_FALSE(allocData.flags.allow32Bit);
    EXPECT_FALSE(allocData.flags.allow64kbPages);
    EXPECT_EQ(allocType, allocData.type);
}

static const GraphicsAllocation::AllocationType allocationTypesWith32BitAnd64KbPagesAllowed[] = {GraphicsAllocation::AllocationType::BUFFER,
                                                                                                 GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY,
                                                                                                 GraphicsAllocation::AllocationType::BUFFER_COMPRESSED,
                                                                                                 GraphicsAllocation::AllocationType::PIPE,
                                                                                                 GraphicsAllocation::AllocationType::SCRATCH_SURFACE,
                                                                                                 GraphicsAllocation::AllocationType::WORK_PARTITION_SURFACE,
                                                                                                 GraphicsAllocation::AllocationType::PRIVATE_SURFACE,
                                                                                                 GraphicsAllocation::AllocationType::PRINTF_SURFACE,
                                                                                                 GraphicsAllocation::AllocationType::CONSTANT_SURFACE,
                                                                                                 GraphicsAllocation::AllocationType::GLOBAL_SURFACE,
                                                                                                 GraphicsAllocation::AllocationType::WRITE_COMBINED};

INSTANTIATE_TEST_CASE_P(Allow32BitAnd64kbPagesTypes,
                        MemoryManagerGetAlloctionData32BitAnd64kbPagesAllowedTest,
                        ::testing::ValuesIn(allocationTypesWith32BitAnd64KbPagesAllowed));

static const GraphicsAllocation::AllocationType allocationTypesWith32BitAnd64KbPagesNotAllowed[] = {GraphicsAllocation::AllocationType::COMMAND_BUFFER,
                                                                                                    GraphicsAllocation::AllocationType::TIMESTAMP_PACKET_TAG_BUFFER,
                                                                                                    GraphicsAllocation::AllocationType::PROFILING_TAG_BUFFER,
                                                                                                    GraphicsAllocation::AllocationType::IMAGE,
                                                                                                    GraphicsAllocation::AllocationType::INSTRUCTION_HEAP,
                                                                                                    GraphicsAllocation::AllocationType::SHARED_RESOURCE_COPY};

INSTANTIATE_TEST_CASE_P(Disallow32BitAnd64kbPagesTypes,
                        MemoryManagerGetAlloctionData32BitAnd64kbPagesNotAllowedTest,
                        ::testing::ValuesIn(allocationTypesWith32BitAnd64KbPagesNotAllowed));

TEST(MemoryManagerTest, givenForced32BitSetWhenGraphicsMemoryFor32BitAllowedTypeIsAllocatedThen32BitAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, false, executionEnvironment);
    memoryManager.setForce32BitAllocations(true);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    if (is64bit) {
        EXPECT_TRUE(allocation->is32BitAllocation());
        EXPECT_EQ(MemoryPool::System4KBPagesWith32BitGpuAddressing, allocation->getMemoryPool());
    } else {
        EXPECT_FALSE(allocation->is32BitAllocation());
        EXPECT_EQ(MemoryPool::System4KBPages, allocation->getMemoryPool());
    }

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenEnabledShareableWhenGraphicsAllocationIsAllocatedThenAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    executionEnvironment.initGmm();
    MockMemoryManager memoryManager(false, false, executionEnvironment);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);
    properties.flags.shareable = true;

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(allocData.flags.shareable, 1u);

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenEnabledShareableWhenGraphicsAllocationIsCalledAndSystemMemoryFailsThenNullAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    executionEnvironment.initGmm();
    MockMemoryManager memoryManager(false, false, executionEnvironment);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);
    properties.flags.shareable = true;

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(allocData.flags.shareable, 1u);

    memoryManager.failAllocateSystemMemory = true;
    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_EQ(nullptr, allocation);

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenForced32BitEnabledWhenGraphicsMemoryWihtoutAllow32BitFlagIsAllocatedThenNon32BitAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(executionEnvironment);
    memoryManager.setForce32BitAllocations(true);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));
    allocData.flags.allow32Bit = false;

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    EXPECT_FALSE(allocation->is32BitAllocation());

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenForced32BitDisabledWhenGraphicsMemoryWith32BitFlagFor32BitAllowedTypeIsAllocatedThenNon32BitAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(executionEnvironment);
    memoryManager.setForce32BitAllocations(false);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    EXPECT_FALSE(allocation->is32BitAllocation());

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenEnabled64kbPagesWhenGraphicsMemoryMustBeHostMemoryAndIsAllocatedWithNullptrForBufferThen64kbAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    executionEnvironment.initGmm();
    MockMemoryManager memoryManager(true, false, executionEnvironment);
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(allocation->getUnderlyingBuffer()) & MemoryConstants::page64kMask);
    EXPECT_EQ(0u, allocation->getGpuAddress() & MemoryConstants::page64kMask);
    EXPECT_EQ(0u, allocation->getUnderlyingBufferSize() & MemoryConstants::page64kMask);
    EXPECT_EQ(MemoryPool::System64KBPages, allocation->getMemoryPool());

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenEnabled64kbPagesWhenGraphicsMemoryWithoutAllow64kbPagesFlagsIsAllocatedThenNon64kbAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(true, false, executionEnvironment);
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));
    allocData.flags.allow64kbPages = false;

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    EXPECT_FALSE(memoryManager.allocation64kbPageCreated);
    EXPECT_TRUE(memoryManager.allocationCreated);

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenDisabled64kbPagesWhenGraphicsMemoryMustBeHostMemoryAndIsAllocatedWithNullptrForBufferThenNon64kbAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, false, executionEnvironment);
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    EXPECT_FALSE(memoryManager.allocation64kbPageCreated);
    EXPECT_TRUE(memoryManager.allocationCreated);
    EXPECT_EQ(MemoryPool::System4KBPages, allocation->getMemoryPool());

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenForced32BitAndEnabled64kbPagesWhenGraphicsMemoryMustBeHostMemoryAndIsAllocatedWithNullptrForBufferThen32BitAllocationOver64kbIsChosen) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, false, executionEnvironment);
    memoryManager.setForce32BitAllocations(true);

    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, 10, GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY, mockDeviceBitfield);

    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    if (is64bit) {
        EXPECT_TRUE(allocation->is32BitAllocation());
    } else {
        EXPECT_FALSE(allocation->is32BitAllocation());
    }

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenEnabled64kbPagesWhenGraphicsMemoryIsAllocatedWithHostPtrForBufferThenExistingMemoryIsUsedForAllocation) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(true, false, executionEnvironment);
    AllocationData allocData;
    AllocationProperties properties(mockRootDeviceIndex, false, 1, GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY, false, mockDeviceBitfield);

    char memory[1];
    memoryManager.getAllocationData(allocData, properties, &memory, memoryManager.createStorageInfoFromProperties(properties));

    auto allocation = memoryManager.allocateGraphicsMemory(allocData);
    ASSERT_NE(nullptr, allocation);
    EXPECT_EQ(1u, allocation->fragmentsStorage.fragmentCount);
    EXPECT_EQ(MemoryPool::System4KBPages, allocation->getMemoryPool());

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenMemoryManagerWhenGraphicsMemoryAllocationInDevicePoolFailsThenFallbackAllocationIsReturned) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, true, executionEnvironment);

    memoryManager.failInDevicePool = true;

    auto allocation = memoryManager.allocateGraphicsMemoryWithProperties({mockRootDeviceIndex, MemoryConstants::pageSize, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield});
    ASSERT_NE(nullptr, allocation);
    EXPECT_TRUE(memoryManager.allocationCreated);
    EXPECT_EQ(MemoryPool::System4KBPages, allocation->getMemoryPool());

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenMemoryManagerWhenBufferTypeIsPassedThenAllocateGraphicsMemoryInPreferredPoolCanAllocateInDevicePool) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, true, executionEnvironment);

    auto allocation = memoryManager.allocateGraphicsMemoryWithProperties({mockRootDeviceIndex, MemoryConstants::pageSize, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield});
    EXPECT_NE(nullptr, allocation);
    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenMemoryManagerWhenBufferTypeIsPassedAndAllocateInDevicePoolFailsWithErrorThenAllocateGraphicsMemoryInPreferredPoolReturnsNullptr) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, true, executionEnvironment);

    memoryManager.failInDevicePoolWithError = true;

    auto allocation = memoryManager.allocateGraphicsMemoryWithProperties({mockRootDeviceIndex, MemoryConstants::pageSize, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield});
    ASSERT_EQ(nullptr, allocation);
    EXPECT_FALSE(memoryManager.allocationInDevicePoolCreated);

    memoryManager.freeGraphicsMemory(allocation);
}

TEST(MemoryManagerTest, givenSvmAllocationTypeWhenGetAllocationDataIsCalledThenAllocatingMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::SVM_ZERO_COPY, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.allocateMemory);
}

TEST(MemoryManagerTest, givenSvmAllocationTypeWhenGetAllocationDataIsCalledThen64kbPagesAreAllowedAnd32BitAllocationIsDisallowed) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::SVM_ZERO_COPY, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.allow64kbPages);
    EXPECT_FALSE(allocData.flags.allow32Bit);
}

TEST(MemoryManagerTest, givenTagBufferTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::TAG_BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenGlobalFenceTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::GLOBAL_FENCE, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenPreemptionTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::PREEMPTION, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenSharedContextImageTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::SHARED_CONTEXT_IMAGE, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenMCSTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::MCS, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenPipeTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::PIPE, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenGlobalSurfaceTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::GLOBAL_SURFACE, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenWriteCombinedTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::WRITE_COMBINED, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDeviceQueueBufferTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::DEVICE_QUEUE_BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenInternalHostMemoryTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::INTERNAL_HOST_MEMORY, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenFillPatternTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::FILL_PATTERN, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

using GetAllocationDataTestHw = ::testing::Test;

HWTEST_F(GetAllocationDataTestHw, givenLinearStreamTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::LINEAR_STREAM, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

HWTEST_F(GetAllocationDataTestHw, givenTimestampPacketTagBufferTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequestedAndRequireCpuAccess) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::TIMESTAMP_PACKET_TAG_BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(UnitTestHelper<FamilyType>::requiresTimestampPacketsInSystemMemory(), allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

TEST(MemoryManagerTest, givenProfilingTagBufferTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::PROFILING_TAG_BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
    EXPECT_FALSE(allocData.flags.requiresCpuAccess);
}

TEST(MemoryManagerTest, givenAllocationPropertiesWithMultiOsContextCapableFlagEnabledWhenAllocateMemoryThenAllocationDataIsMultiOsContextCapable) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, false, executionEnvironment);
    AllocationProperties properties{mockRootDeviceIndex, MemoryConstants::pageSize, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield};
    properties.flags.multiOsContextCapable = true;

    AllocationData allocData;
    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.multiOsContextCapable);
}

TEST(MemoryManagerTest, givenAllocationPropertiesWithMultiOsContextCapableFlagDisabledWhenAllocateMemoryThenAllocationDataIsNotMultiOsContextCapable) {
    MockExecutionEnvironment executionEnvironment(defaultHwInfo.get());
    MockMemoryManager memoryManager(false, false, executionEnvironment);
    AllocationProperties properties{mockRootDeviceIndex, MemoryConstants::pageSize, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield};
    properties.flags.multiOsContextCapable = false;

    AllocationData allocData;
    memoryManager.getAllocationData(allocData, properties, nullptr, memoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.multiOsContextCapable);
}

TEST(MemoryManagerTest, givenConstantSurfaceTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::CONSTANT_SURFACE, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
}

HWTEST_F(GetAllocationDataTestHw, givenInternalHeapTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::INTERNAL_HEAP, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

HWTEST_F(GetAllocationDataTestHw, givenKernelIsaTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::KERNEL_ISA, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_NE(defaultHwInfo->featureTable.ftrLocalMemory, allocData.flags.useSystemMemory);

    AllocationProperties properties2{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::KERNEL_ISA_INTERNAL, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties2, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_NE(defaultHwInfo->featureTable.ftrLocalMemory, allocData.flags.useSystemMemory);
}

HWTEST_F(GetAllocationDataTestHw, givenLinearStreamWhenGetAllocationDataIsCalledThenSystemMemoryIsNotRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::LINEAR_STREAM, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

HWTEST_F(GetAllocationDataTestHw, givenPrintfAllocationWhenGetAllocationDataIsCalledThenDontUseSystemMemoryAndRequireCpuAccess) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::PRINTF_SURFACE, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

TEST(MemoryManagerTest, givenExternalHostMemoryWhenGetAllocationDataIsCalledThenItHasProperFieldsSet) {
    AllocationData allocData;
    auto hostPtr = reinterpret_cast<void *>(0x1234);
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, false, 1, GraphicsAllocation::AllocationType::EXTERNAL_HOST_PTR, false, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, hostPtr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
    EXPECT_FALSE(allocData.flags.allocateMemory);
    EXPECT_FALSE(allocData.flags.allow32Bit);
    EXPECT_FALSE(allocData.flags.allow64kbPages);
    EXPECT_EQ(allocData.hostPtr, hostPtr);
}

TEST(MemoryManagerTest, GivenAllocationPropertiesWhenGettingAllocationDataThenSameRootDeviceIndexIsUsed) {
    const uint32_t rootDevicesCount = 100u;

    AllocationData allocData;
    MockExecutionEnvironment executionEnvironment{defaultHwInfo.get(), true, rootDevicesCount};
    MockMemoryManager mockMemoryManager{executionEnvironment};
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(allocData.rootDeviceIndex, 0u);

    AllocationProperties properties2{rootDevicesCount - 1, 1, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties2, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(allocData.rootDeviceIndex, properties2.rootDeviceIndex);
}

TEST(MemoryManagerTest, givenMapAllocationWhenGetAllocationDataIsCalledThenItHasProperFieldsSet) {
    AllocationData allocData;
    auto hostPtr = reinterpret_cast<void *>(0x1234);
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, false, 1, GraphicsAllocation::AllocationType::MAP_ALLOCATION, false, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, hostPtr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
    EXPECT_FALSE(allocData.flags.allocateMemory);
    EXPECT_FALSE(allocData.flags.allow32Bit);
    EXPECT_FALSE(allocData.flags.allow64kbPages);
    EXPECT_EQ(allocData.hostPtr, hostPtr);
}

HWTEST_F(GetAllocationDataTestHw, givenRingBufferAllocationWhenGetAllocationDataIsCalledThenItHasProperFieldsSet) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 0x10000u, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.allocateMemory);
    EXPECT_FALSE(allocData.flags.allow32Bit);
    EXPECT_FALSE(allocData.flags.allow64kbPages);
    EXPECT_EQ(0x10000u, allocData.size);
    EXPECT_EQ(nullptr, allocData.hostPtr);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

HWTEST_F(GetAllocationDataTestHw, givenSemaphoreBufferAllocationWhenGetAllocationDataIsCalledThenItHasProperFieldsSet) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 0x1000u, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_FALSE(allocData.flags.useSystemMemory);
    EXPECT_TRUE(allocData.flags.allocateMemory);
    EXPECT_FALSE(allocData.flags.allow32Bit);
    EXPECT_FALSE(allocData.flags.allow64kbPages);
    EXPECT_EQ(0x1000u, allocData.size);
    EXPECT_EQ(nullptr, allocData.hostPtr);
    EXPECT_TRUE(allocData.flags.requiresCpuAccess);
}

TEST(MemoryManagerTest, givenDirectBufferPlacementSetWhenDefaultIsUsedThenExpectNoFlagsChanged) {
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.requiresCpuAccess);
    EXPECT_EQ(0u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDirectBufferPlacementSetWhenOverrideToNonSystemThenExpectNonSystemFlags) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionBufferPlacement.set(0);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(1u, allocationData.flags.requiresCpuAccess);
    EXPECT_EQ(0u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDirectBufferPlacementSetWhenOverrideToSystemThenExpectNonFlags) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionBufferPlacement.set(1);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.requiresCpuAccess);
    EXPECT_EQ(1u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDirectSemaphorePlacementSetWhenDefaultIsUsedThenExpectNoFlagsChanged) {
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.requiresCpuAccess);
    EXPECT_EQ(0u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDirectSemaphorePlacementSetWhenOverrideToNonSystemThenExpectNonSystemFlags) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionSemaphorePlacement.set(0);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(1u, allocationData.flags.requiresCpuAccess);
    EXPECT_EQ(0u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDirectSemaphorePlacementSetWhenOverrideToSystemThenExpectNonFlags) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionSemaphorePlacement.set(1);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield);
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.requiresCpuAccess);
    EXPECT_EQ(1u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDirectBufferAddressingWhenOverrideToNo48BitThenExpect48BitFlagFalse) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionBufferAddressing.set(0);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield);
    allocationData.flags.resource48Bit = 1;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.resource48Bit);
}

TEST(MemoryManagerTest, givenDirectBufferAddressingWhenOverrideTo48BitThenExpect48BitFlagTrue) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionBufferAddressing.set(1);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield);
    allocationData.flags.resource48Bit = 0;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(1u, allocationData.flags.resource48Bit);
}

TEST(MemoryManagerTest, givenDirectBufferAddressingDefaultWhenNoOverrideThenExpect48BitFlagSame) {
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::RING_BUFFER, mockDeviceBitfield);
    allocationData.flags.resource48Bit = 0;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.resource48Bit);

    allocationData.flags.resource48Bit = 1;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(1u, allocationData.flags.resource48Bit);
}

TEST(MemoryManagerTest, givenDirectSemaphoreAddressingWhenOverrideToNo48BitThenExpect48BitFlagFalse) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionSemaphoreAddressing.set(0);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield);
    allocationData.flags.resource48Bit = 1;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.resource48Bit);
}

TEST(MemoryManagerTest, givenDirectSemaphoreAddressingWhenOverrideTo48BitThenExpect48BitFlagTrue) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DirectSubmissionSemaphoreAddressing.set(1);
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield);
    allocationData.flags.resource48Bit = 0;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(1u, allocationData.flags.resource48Bit);
}

TEST(MemoryManagerTest, givenDirectSemaphoreAddressingDefaultWhenNoOverrideThenExpect48BitFlagSame) {
    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER, mockDeviceBitfield);
    allocationData.flags.resource48Bit = 0;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(0u, allocationData.flags.resource48Bit);

    allocationData.flags.resource48Bit = 1;
    MockMemoryManager::overrideAllocationData(allocationData, properties);

    EXPECT_EQ(1u, allocationData.flags.resource48Bit);
}

TEST(MemoryManagerTest, givenForceNonSystemMaskWhenAllocationTypeMatchesMaskThenExpectSystemFlagFalse) {
    DebugManagerStateRestore restorer;
    auto allocationType = GraphicsAllocation::AllocationType::BUFFER;
    auto mask = 1llu << (static_cast<int64_t>(allocationType) - 1);
    DebugManager.flags.ForceNonSystemMemoryPlacement.set(mask);

    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::BUFFER, mockDeviceBitfield);
    allocationData.flags.useSystemMemory = 1;
    MockMemoryManager::overrideAllocationData(allocationData, properties);
    EXPECT_EQ(0u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenForceNonSystemMaskWhenAllocationTypeNotMatchesMaskThenExpectSystemFlagTrue) {
    DebugManagerStateRestore restorer;
    auto allocationType = GraphicsAllocation::AllocationType::BUFFER;
    auto mask = 1llu << (static_cast<int64_t>(allocationType) - 1);
    DebugManager.flags.ForceNonSystemMemoryPlacement.set(mask);

    AllocationData allocationData;
    AllocationProperties properties(mockRootDeviceIndex, 0x1000, GraphicsAllocation::AllocationType::COMMAND_BUFFER, mockDeviceBitfield);
    allocationData.flags.useSystemMemory = 1;
    MockMemoryManager::overrideAllocationData(allocationData, properties);
    EXPECT_EQ(1u, allocationData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenDebugContextSaveAreaTypeWhenGetAllocationDataIsCalledThenSystemMemoryIsRequested) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::DEBUG_CONTEXT_SAVE_AREA, mockDeviceBitfield};
    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocData.flags.useSystemMemory);
}

TEST(MemoryManagerTest, givenPropertiesWithOsContextWhenGetAllocationDataIsCalledThenOsContextIsSet) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{0, 1, GraphicsAllocation::AllocationType::DEBUG_CONTEXT_SAVE_AREA, mockDeviceBitfield};

    MockOsContext osContext(0u, 1,
                            HwHelper::get(defaultHwInfo->platform.eRenderCoreFamily).getGpgpuEngineInstances(*defaultHwInfo)[0],
                            PreemptionMode::Disabled, false);

    properties.osContext = &osContext;

    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(&osContext, allocData.osContext);
}

TEST(MemoryManagerTest, givenPropertiesWithGpuAddressWhenGetAllocationDataIsCalledThenGpuAddressIsSet) {
    AllocationData allocData;
    MockMemoryManager mockMemoryManager;
    AllocationProperties properties{mockRootDeviceIndex, 1, GraphicsAllocation::AllocationType::DEBUG_CONTEXT_SAVE_AREA, mockDeviceBitfield};

    properties.gpuAddress = 0x4000;

    mockMemoryManager.getAllocationData(allocData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(properties.gpuAddress, allocData.gpuAddress);
}

using MemoryManagerGetAlloctionDataHaveToBeForcedTo48BitTest = testing::TestWithParam<std::tuple<GraphicsAllocation::AllocationType, bool>>;

TEST_P(MemoryManagerGetAlloctionDataHaveToBeForcedTo48BitTest, givenAllocationTypesHaveToBeForcedTo48BitThenAllocationDataResource48BitIsSet) {
    GraphicsAllocation::AllocationType allocationType;
    bool propertiesFlag48Bit;

    std::tie(allocationType, propertiesFlag48Bit) = GetParam();

    AllocationProperties properties(mockRootDeviceIndex, 0, allocationType, mockDeviceBitfield);
    properties.flags.resource48Bit = propertiesFlag48Bit;

    AllocationData allocationData;
    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocationData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_TRUE(allocationData.flags.resource48Bit);
}

using MemoryManagerGetAlloctionDataHaveNotToBeForcedTo48BitTest = testing::TestWithParam<std::tuple<GraphicsAllocation::AllocationType, bool>>;

TEST_P(MemoryManagerGetAlloctionDataHaveNotToBeForcedTo48BitTest, givenAllocationTypesHaveNotToBeForcedTo48BitThenAllocationDataResource48BitIsSetProperly) {
    GraphicsAllocation::AllocationType allocationType;
    bool propertiesFlag48Bit;

    std::tie(allocationType, propertiesFlag48Bit) = GetParam();

    AllocationProperties properties(mockRootDeviceIndex, 0, allocationType, mockDeviceBitfield);
    properties.flags.resource48Bit = propertiesFlag48Bit;

    AllocationData allocationData;
    MockMemoryManager mockMemoryManager;
    mockMemoryManager.getAllocationData(allocationData, properties, nullptr, mockMemoryManager.createStorageInfoFromProperties(properties));
    EXPECT_EQ(allocationData.flags.resource48Bit, propertiesFlag48Bit);
}

static const GraphicsAllocation::AllocationType allocationHaveToBeForcedTo48Bit[] = {
    GraphicsAllocation::AllocationType::COMMAND_BUFFER,
    GraphicsAllocation::AllocationType::DEVICE_QUEUE_BUFFER,
    GraphicsAllocation::AllocationType::IMAGE,
    GraphicsAllocation::AllocationType::INDIRECT_OBJECT_HEAP,
    GraphicsAllocation::AllocationType::INSTRUCTION_HEAP,
    GraphicsAllocation::AllocationType::INTERNAL_HEAP,
    GraphicsAllocation::AllocationType::KERNEL_ISA,
    GraphicsAllocation::AllocationType::LINEAR_STREAM,
    GraphicsAllocation::AllocationType::MCS,
    GraphicsAllocation::AllocationType::SCRATCH_SURFACE,
    GraphicsAllocation::AllocationType::WORK_PARTITION_SURFACE,
    GraphicsAllocation::AllocationType::SHARED_CONTEXT_IMAGE,
    GraphicsAllocation::AllocationType::SHARED_IMAGE,
    GraphicsAllocation::AllocationType::SHARED_RESOURCE_COPY,
    GraphicsAllocation::AllocationType::SURFACE_STATE_HEAP,
    GraphicsAllocation::AllocationType::TIMESTAMP_PACKET_TAG_BUFFER,
};

static const GraphicsAllocation::AllocationType allocationHaveNotToBeForcedTo48Bit[] = {
    GraphicsAllocation::AllocationType::BUFFER,
    GraphicsAllocation::AllocationType::BUFFER_COMPRESSED,
    GraphicsAllocation::AllocationType::BUFFER_HOST_MEMORY,
    GraphicsAllocation::AllocationType::CONSTANT_SURFACE,
    GraphicsAllocation::AllocationType::EXTERNAL_HOST_PTR,
    GraphicsAllocation::AllocationType::FILL_PATTERN,
    GraphicsAllocation::AllocationType::GLOBAL_SURFACE,
    GraphicsAllocation::AllocationType::INTERNAL_HOST_MEMORY,
    GraphicsAllocation::AllocationType::MAP_ALLOCATION,
    GraphicsAllocation::AllocationType::PIPE,
    GraphicsAllocation::AllocationType::PREEMPTION,
    GraphicsAllocation::AllocationType::PRINTF_SURFACE,
    GraphicsAllocation::AllocationType::PRIVATE_SURFACE,
    GraphicsAllocation::AllocationType::PROFILING_TAG_BUFFER,
    GraphicsAllocation::AllocationType::SHARED_BUFFER,
    GraphicsAllocation::AllocationType::SVM_CPU,
    GraphicsAllocation::AllocationType::SVM_GPU,
    GraphicsAllocation::AllocationType::SVM_ZERO_COPY,
    GraphicsAllocation::AllocationType::TAG_BUFFER,
    GraphicsAllocation::AllocationType::GLOBAL_FENCE,
    GraphicsAllocation::AllocationType::WRITE_COMBINED,
    GraphicsAllocation::AllocationType::RING_BUFFER,
    GraphicsAllocation::AllocationType::SEMAPHORE_BUFFER,
    GraphicsAllocation::AllocationType::DEBUG_CONTEXT_SAVE_AREA,
};

INSTANTIATE_TEST_CASE_P(ForceTo48Bit,
                        MemoryManagerGetAlloctionDataHaveToBeForcedTo48BitTest,
                        ::testing::Combine(
                            ::testing::ValuesIn(allocationHaveToBeForcedTo48Bit),
                            ::testing::Bool()));

INSTANTIATE_TEST_CASE_P(NotForceTo48Bit,
                        MemoryManagerGetAlloctionDataHaveNotToBeForcedTo48BitTest,
                        ::testing::Combine(
                            ::testing::ValuesIn(allocationHaveNotToBeForcedTo48Bit),
                            ::testing::Bool()));
