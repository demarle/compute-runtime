/*
 * Copyright (C) 2020-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/device_factory.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/test/common/fixtures/device_fixture.h"

#include <atomic>
#include <memory>

using namespace NEO;

extern std::atomic<uintptr_t> lastClFlushedPtr;

struct DirectSubmissionFixture : public DeviceFixture {
    void SetUp() {
        DeviceFixture::SetUp();
        DeviceFactory::prepareDeviceEnvironments(*pDevice->getExecutionEnvironment());

        osContext.reset(OsContext::create(nullptr, 0u, pDevice->getDeviceBitfield(), EngineTypeUsage{aub_stream::ENGINE_RCS, EngineUsage::Regular}, PreemptionMode::ThreadGroup, false));
    }

    std::unique_ptr<OsContext> osContext;
};

struct DirectSubmissionDispatchBufferFixture : public DirectSubmissionFixture {
    void SetUp() {
        DirectSubmissionFixture::SetUp();
        MemoryManager *memoryManager = pDevice->getExecutionEnvironment()->memoryManager.get();
        const AllocationProperties commandBufferProperties{pDevice->getRootDeviceIndex(), 0x1000,
                                                           GraphicsAllocation::AllocationType::COMMAND_BUFFER, pDevice->getDeviceBitfield()};
        commandBuffer = memoryManager->allocateGraphicsMemoryWithProperties(commandBufferProperties);

        batchBuffer.endCmdPtr = &bbStart[0];
        batchBuffer.commandBufferAllocation = commandBuffer;
        batchBuffer.usedSize = 0x40;
    }

    void TearDown() {
        MemoryManager *memoryManager = pDevice->getExecutionEnvironment()->memoryManager.get();
        memoryManager->freeGraphicsMemory(commandBuffer);

        DirectSubmissionFixture::TearDown();
    }

    BatchBuffer batchBuffer;
    uint8_t bbStart[64];
    GraphicsAllocation *commandBuffer;
};
