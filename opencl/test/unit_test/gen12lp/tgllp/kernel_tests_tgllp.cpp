/*
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/test/common/mocks/mock_device.h"

#include "opencl/source/helpers/hardware_commands_helper.h"
#include "opencl/test/unit_test/mocks/mock_cl_device.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "test.h"

using namespace NEO;

using KernelTgllpTests = ::testing::Test;

TGLLPTEST_F(KernelTgllpTests, GivenUseOffsetToSkipSetFFIDGPWorkaroundActiveWhenSettingKernelStartOffsetThenAdditionalOffsetIsSet) {
    const uint64_t defaultKernelStartOffset = 0;
    const uint64_t additionalOffsetDueToFfid = 0x1234;
    SPatchThreadPayload threadPayload{};
    threadPayload.OffsetToSkipSetFFIDGP = additionalOffsetDueToFfid;
    auto hwInfo = *defaultHwInfo;
    auto &hwHelper = HwHelper::get(hwInfo.platform.eRenderCoreFamily);

    unsigned short steppings[] = {REVISION_A0, REVISION_A1};
    for (auto stepping : steppings) {

        hwInfo.platform.usRevId = hwHelper.getHwRevIdFromStepping(stepping, hwInfo);
        auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(&hwInfo));
        auto rootDeviceIndex = device->getRootDeviceIndex();
        MockKernelWithInternals mockKernelWithInternals{*device};
        populateKernelDescriptor(mockKernelWithInternals.kernelInfo.kernelDescriptor, threadPayload);

        for (auto isCcsUsed : ::testing::Bool()) {
            uint64_t kernelStartOffset = mockKernelWithInternals.mockKernel->getKernelStartOffset(false, false, isCcsUsed, rootDeviceIndex);

            if (stepping == REVISION_A0 && isCcsUsed) {
                EXPECT_EQ(defaultKernelStartOffset + additionalOffsetDueToFfid, kernelStartOffset);
            } else {
                EXPECT_EQ(defaultKernelStartOffset, kernelStartOffset);
            }
        }
    }
}
