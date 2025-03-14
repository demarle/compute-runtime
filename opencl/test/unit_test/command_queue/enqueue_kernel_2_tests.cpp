/*
 * Copyright (C) 2018-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/memory_manager/allocations_list.h"
#include "shared/test/common/cmd_parse/gen_cmd_parse.h"
#include "shared/test/common/cmd_parse/hw_parse.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/test/unit_test/command_queue/enqueue_fixture.h"
#include "opencl/test/unit_test/fixtures/hello_world_fixture.h"
#include "opencl/test/unit_test/gen_common/gen_commands_common_validation.h"
#include "opencl/test/unit_test/mocks/mock_buffer.h"
#include "opencl/test/unit_test/mocks/mock_command_queue.h"
#include "opencl/test/unit_test/mocks/mock_csr.h"
#include "opencl/test/unit_test/mocks/mock_device_queue.h"
#include "opencl/test/unit_test/test_macros/test_checks_ocl.h"

#include "reg_configs_common.h"

using namespace NEO;

struct TestParam2 {
    uint32_t scratchSize;
} TestParamTable2[] = {{1024u}, {2048u}, {4096u}, {8192u}, {16384u}};

struct TestParam {
    cl_uint globalWorkSizeX;
    cl_uint globalWorkSizeY;
    cl_uint globalWorkSizeZ;
    cl_uint localWorkSizeX;
    cl_uint localWorkSizeY;
    cl_uint localWorkSizeZ;
} TestParamTable[] = {
    {1, 1, 1, 1, 1, 1},
    {16, 1, 1, 1, 1, 1},
    {16, 1, 1, 16, 1, 1},
    {32, 1, 1, 1, 1, 1},
    {32, 1, 1, 16, 1, 1},
    {32, 1, 1, 32, 1, 1},
    {64, 1, 1, 1, 1, 1},
    {64, 1, 1, 16, 1, 1},
    {64, 1, 1, 32, 1, 1},
    {64, 1, 1, 64, 1, 1},
    {190, 1, 1, 95, 1, 1},
    {510, 1, 1, 255, 1, 1},
    {512, 1, 1, 256, 1, 1}},
  OneEntryTestParamTable[] = {
      {1, 1, 1, 1, 1, 1},
};
template <typename InputType>
struct EnqueueKernelTypeTest : public HelloWorldFixture<HelloWorldFixtureFactory>,
                               public HardwareParse,
                               ::testing::TestWithParam<InputType> {
    typedef HelloWorldFixture<HelloWorldFixtureFactory> ParentClass;
    using ParentClass::pCmdBuffer;
    using ParentClass::pCS;

    EnqueueKernelTypeTest() {
    }
    void FillValues() {
        globalWorkSize[0] = 1;
        globalWorkSize[1] = 1;
        globalWorkSize[2] = 1;
        localWorkSize[0] = 1;
        localWorkSize[1] = 1;
        localWorkSize[2] = 1;
    };

    template <typename FamilyType, bool ParseCommands>
    typename std::enable_if<false == ParseCommands, void>::type enqueueKernel(Kernel *inputKernel = nullptr) {
        cl_uint workDim = 1;
        size_t globalWorkOffset[3] = {0, 0, 0};

        cl_uint numEventsInWaitList = 0;
        cl_event *eventWaitList = nullptr;
        cl_event *event = nullptr;

        FillValues();
        // Compute # of expected work items
        expectedWorkItems = 1;
        for (auto i = 0u; i < workDim; i++) {
            expectedWorkItems *= globalWorkSize[i];
        }

        auto usedKernel = inputKernel ? inputKernel : pKernel;

        auto retVal = pCmdQ->enqueueKernel(
            usedKernel,
            workDim,
            globalWorkOffset,
            globalWorkSize,
            localWorkSize,
            numEventsInWaitList,
            eventWaitList,
            event);
        ASSERT_EQ(CL_SUCCESS, retVal);
    }

    template <typename FamilyType, bool ParseCommands>
    typename std::enable_if<ParseCommands, void>::type enqueueKernel(Kernel *inputKernel = nullptr) {
        enqueueKernel<FamilyType, false>(inputKernel);

        parseCommands<FamilyType>(*pCmdQ);
    }

    template <typename FamilyType>
    void enqueueKernel(Kernel *inputKernel = nullptr) {
        enqueueKernel<FamilyType, true>(inputKernel);
    }

    void SetUp() override {
        ParentClass::SetUp();
        HardwareParse::SetUp();
    }
    void TearDown() override {
        HardwareParse::TearDown();
        ParentClass::TearDown();
    }
    size_t globalWorkSize[3];
    size_t localWorkSize[3];
    size_t expectedWorkItems = 0;
};

template <>
void EnqueueKernelTypeTest<TestParam>::FillValues() {
    const TestParam &param = GetParam();
    globalWorkSize[0] = param.globalWorkSizeX;
    globalWorkSize[1] = param.globalWorkSizeY;
    globalWorkSize[2] = param.globalWorkSizeZ;
    localWorkSize[0] = param.localWorkSizeX;
    localWorkSize[1] = param.localWorkSizeY;
    localWorkSize[2] = param.localWorkSizeZ;
}

typedef EnqueueKernelTypeTest<TestParam> EnqueueWorkItemTests;
typedef EnqueueKernelTypeTest<TestParam> EnqueueWorkItemTestsWithLimitedParamSet;

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTests, GPGPUWalker) {
    typedef typename FamilyType::PARSE PARSE;
    typedef typename PARSE::GPGPU_WALKER GPGPU_WALKER;

    enqueueKernel<FamilyType>();

    ASSERT_NE(cmdList.end(), itorWalker);
    auto *cmd = (GPGPU_WALKER *)*itorWalker;

    // Verify GPGPU_WALKER parameters
    EXPECT_NE(0u, cmd->getThreadGroupIdXDimension());
    EXPECT_NE(0u, cmd->getThreadGroupIdYDimension());
    EXPECT_NE(0u, cmd->getThreadGroupIdZDimension());
    EXPECT_NE(0u, cmd->getRightExecutionMask());
    EXPECT_NE(0u, cmd->getBottomExecutionMask());
    EXPECT_EQ(GPGPU_WALKER::SIMD_SIZE_SIMD32, cmd->getSimdSize());
    EXPECT_NE(0u, cmd->getIndirectDataLength());
    EXPECT_FALSE(cmd->getIndirectParameterEnable());

    // Compute the SIMD lane mask
    size_t simd =
        cmd->getSimdSize() == GPGPU_WALKER::SIMD_SIZE_SIMD32 ? 32 : cmd->getSimdSize() == GPGPU_WALKER::SIMD_SIZE_SIMD16 ? 16
                                                                                                                         : 8;
    uint64_t simdMask = maxNBitValue(simd);

    // Mask off lanes based on the execution masks
    auto laneMaskRight = cmd->getRightExecutionMask() & simdMask;
    auto lanesPerThreadX = 0;
    while (laneMaskRight) {
        lanesPerThreadX += laneMaskRight & 1;
        laneMaskRight >>= 1;
    }

    auto numWorkItems = ((cmd->getThreadWidthCounterMaximum() - 1) * simd + lanesPerThreadX) * cmd->getThreadGroupIdXDimension();
    EXPECT_EQ(expectedWorkItems, numWorkItems);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, LoadRegisterImmediateL3CNTLREG) {
    enqueueKernel<FamilyType>();
    validateL3Programming<FamilyType>(cmdList, itorWalker);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, WhenEnqueueIsDoneThenStateBaseAddressIsProperlyProgrammed) {
    enqueueKernel<FamilyType>();
    auto &ultCsr = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto &hwHelper = HwHelper::get(pDevice->getHardwareInfo().platform.eRenderCoreFamily);

    validateStateBaseAddress<FamilyType>(ultCsr.getMemoryManager()->getInternalHeapBaseAddress(ultCsr.rootDeviceIndex, pIOH->getGraphicsAllocation()->isAllocatedInLocalMemoryPool()),
                                         ultCsr.getMemoryManager()->getInternalHeapBaseAddress(ultCsr.rootDeviceIndex, !hwHelper.useSystemMemoryPlacementForISA(pDevice->getHardwareInfo())),
                                         pDSH, pIOH, pSSH, itorPipelineSelect, itorWalker, cmdList,
                                         context->getMemoryManager()->peekForce32BitAllocations() ? context->getMemoryManager()->getExternalHeapBaseAddress(ultCsr.rootDeviceIndex, false) : 0llu);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, MediaInterfaceDescriptorLoad) {
    typedef typename FamilyType::PARSE PARSE;
    typedef typename PARSE::MEDIA_INTERFACE_DESCRIPTOR_LOAD MEDIA_INTERFACE_DESCRIPTOR_LOAD;
    typedef typename PARSE::INTERFACE_DESCRIPTOR_DATA INTERFACE_DESCRIPTOR_DATA;
    enqueueKernel<FamilyType>();

    // All state should be programmed before walker
    auto itorCmd = find<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(itorPipelineSelect, itorWalker);
    ASSERT_NE(itorWalker, itorCmd);

    auto *cmd = genCmdCast<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(*itorCmd);

    // Verify we have a valid length -- multiple of INTERFACE_DESCRIPTOR_DATAs
    EXPECT_EQ(0u, cmd->getInterfaceDescriptorTotalLength() % sizeof(INTERFACE_DESCRIPTOR_DATA));

    // Validate the start address
    size_t alignmentStartAddress = 64 * sizeof(uint8_t);
    EXPECT_EQ(0u, cmd->getInterfaceDescriptorDataStartAddress() % alignmentStartAddress);

    // Validate the length
    EXPECT_NE(0u, cmd->getInterfaceDescriptorTotalLength());
    size_t alignmentTotalLength = 32 * sizeof(uint8_t);
    EXPECT_EQ(0u, cmd->getInterfaceDescriptorTotalLength() % alignmentTotalLength);

    // Generically validate this command
    PARSE::template validateCommand<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(cmdList.begin(), itorCmd);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, InterfaceDescriptorData) {
    typedef typename FamilyType::PARSE PARSE;
    typedef typename PARSE::MEDIA_INTERFACE_DESCRIPTOR_LOAD MEDIA_INTERFACE_DESCRIPTOR_LOAD;
    typedef typename PARSE::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;
    typedef typename PARSE::INTERFACE_DESCRIPTOR_DATA INTERFACE_DESCRIPTOR_DATA;
    enqueueKernel<FamilyType>();

    // Extract the MIDL command
    auto itorCmd = find<MEDIA_INTERFACE_DESCRIPTOR_LOAD *>(itorPipelineSelect, itorWalker);
    ASSERT_NE(itorWalker, itorCmd);
    auto *cmdMIDL = (MEDIA_INTERFACE_DESCRIPTOR_LOAD *)*itorCmd;

    // Extract the SBA command
    itorCmd = find<STATE_BASE_ADDRESS *>(cmdList.begin(), itorWalker);
    ASSERT_NE(itorWalker, itorCmd);
    auto *cmdSBA = (STATE_BASE_ADDRESS *)*itorCmd;

    // Extrach the DSH
    auto DSH = cmdSBA->getDynamicStateBaseAddress();
    ASSERT_NE(0u, DSH);

    // IDD should be located within DSH
    auto iddStart = cmdMIDL->getInterfaceDescriptorDataStartAddress();
    auto IDDEnd = iddStart + cmdMIDL->getInterfaceDescriptorTotalLength();
    ASSERT_LE(IDDEnd, cmdSBA->getDynamicStateBufferSize() * MemoryConstants::pageSize);

    auto &IDD = *(INTERFACE_DESCRIPTOR_DATA *)cmdInterfaceDescriptorData;

    // Validate the kernel start pointer.  Technically, a kernel can start at address 0 but let's force a value.
    auto kernelStartPointer = ((uint64_t)IDD.getKernelStartPointerHigh() << 32) + IDD.getKernelStartPointer();
    EXPECT_LE(kernelStartPointer, cmdSBA->getInstructionBufferSize() * MemoryConstants::pageSize);

    EXPECT_NE(0u, IDD.getNumberOfThreadsInGpgpuThreadGroup());
    EXPECT_NE(0u, IDD.getCrossThreadConstantDataReadLength());
    EXPECT_NE(0u, IDD.getConstantIndirectUrbEntryReadLength());
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, givenDebugVariableToOverrideMOCSWhenStateBaseAddressIsBeingProgrammedThenItContainsDesiredIndex) {
    DebugManagerStateRestore restore;
    DebugManager.flags.OverrideStatelessMocsIndex.set(1);
    typedef typename FamilyType::PARSE PARSE;
    typedef typename PARSE::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;
    enqueueKernel<FamilyType>();

    // Extract the SBA command
    auto itorCmd = find<STATE_BASE_ADDRESS *>(cmdList.begin(), cmdList.end());
    ASSERT_NE(itorWalker, itorCmd);
    auto *cmdSBA = (STATE_BASE_ADDRESS *)*itorCmd;
    auto mocsProgrammed = cmdSBA->getStatelessDataPortAccessMemoryObjectControlStateIndexToMocsTables() >> 1;
    EXPECT_EQ(1u, mocsProgrammed);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, PipelineSelect) {
    enqueueKernel<FamilyType>();
    int numCommands = getNumberOfPipelineSelectsThatEnablePipelineSelect<FamilyType>();
    EXPECT_EQ(1, numCommands);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueWorkItemTestsWithLimitedParamSet, MediaVFEState) {
    enqueueKernel<FamilyType>();
    validateMediaVFEState<FamilyType>(&pDevice->getHardwareInfo(), cmdMediaVfeState, cmdList, itorMediaVfeState);
}

INSTANTIATE_TEST_CASE_P(EnqueueKernel,
                        EnqueueWorkItemTests,
                        ::testing::ValuesIn(TestParamTable));

INSTANTIATE_TEST_CASE_P(EnqueueKernel,
                        EnqueueWorkItemTestsWithLimitedParamSet,
                        ::testing::ValuesIn(OneEntryTestParamTable));

typedef EnqueueKernelTypeTest<TestParam2> EnqueueScratchSpaceTests;

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueScratchSpaceTests, GivenKernelRequiringScratchWhenItIsEnqueuedWithDifferentScratchSizesThenMediaVFEStateAndStateBaseAddressAreProperlyProgrammed) {
    typedef typename FamilyType::PARSE PARSE;
    typedef typename PARSE::MEDIA_VFE_STATE MEDIA_VFE_STATE;
    typedef typename PARSE::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;

    auto &csr = pDevice->getUltCommandStreamReceiver<FamilyType>();

    csr.getMemoryManager()->setForce32BitAllocations(false);

    EXPECT_TRUE(csr.getAllocationsForReuse().peekIsEmpty());

    auto scratchSize = GetParam().scratchSize;

    MockKernelWithInternals mockKernel(*pClDevice);

    SPatchMediaVFEState mediaVFEstate;
    mediaVFEstate.PerThreadScratchSpace = scratchSize;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, mediaVFEstate, 0);

    uint32_t sizeToProgram = (scratchSize / static_cast<uint32_t>(MemoryConstants::kiloByte));
    uint32_t bitValue = 0u;
    while (sizeToProgram >>= 1) {
        bitValue++;
    }

    auto valueToProgram = PreambleHelper<FamilyType>::getScratchSizeValueToProgramMediaVfeState(scratchSize);
    EXPECT_EQ(bitValue, valueToProgram);

    enqueueKernel<FamilyType>(mockKernel);

    // All state should be programmed before walker
    auto itorCmd = find<MEDIA_VFE_STATE *>(itorPipelineSelect, itorWalker);
    auto itorCmdForStateBase = find<STATE_BASE_ADDRESS *>(itorPipelineSelect, itorWalker);

    ASSERT_NE(itorWalker, itorCmd);
    ASSERT_NE(itorWalker, itorCmdForStateBase);

    auto *cmd = (MEDIA_VFE_STATE *)*itorCmd;
    auto *sba = (STATE_BASE_ADDRESS *)*itorCmdForStateBase;

    const HardwareInfo &hwInfo = *defaultHwInfo;
    uint32_t threadPerEU = (hwInfo.gtSystemInfo.ThreadCount / hwInfo.gtSystemInfo.EUCount) + hwInfo.capabilityTable.extraQuantityThreadsPerEU;
    uint32_t maxNumberOfThreads = hwInfo.gtSystemInfo.EUCount * threadPerEU;

    // Verify we have a valid length
    EXPECT_EQ(maxNumberOfThreads, cmd->getMaximumNumberOfThreads());
    EXPECT_NE(0u, cmd->getNumberOfUrbEntries());
    EXPECT_NE(0u, cmd->getUrbEntryAllocationSize());
    EXPECT_EQ(bitValue, cmd->getPerThreadScratchSpace());
    EXPECT_EQ(bitValue, cmd->getStackSize());
    auto graphicsAllocation = csr.getScratchAllocation();
    auto GSHaddress = sba->getGeneralStateBaseAddress();
    if (is32bit) {
        EXPECT_NE(0u, cmd->getScratchSpaceBasePointer());
        EXPECT_EQ(0u, GSHaddress);
    } else {
        EXPECT_EQ(ScratchSpaceConstants::scratchSpaceOffsetFor64Bit, cmd->getScratchSpaceBasePointer());
        EXPECT_EQ(GSHaddress + ScratchSpaceConstants::scratchSpaceOffsetFor64Bit, graphicsAllocation->getGpuAddress());
    }

    auto allocationSize = scratchSize * pDevice->getDeviceInfo().computeUnitsUsedForScratch;
    EXPECT_EQ(graphicsAllocation->getUnderlyingBufferSize(), allocationSize);

    // Generically validate this command
    PARSE::template validateCommand<MEDIA_VFE_STATE *>(cmdList.begin(), itorCmd);

    scratchSize *= 2;
    //skip if size to big 4MB, no point in stressing memory allocator.
    if (allocationSize > 4194304) {
        return;
    }

    mediaVFEstate.PerThreadScratchSpace = scratchSize;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, mediaVFEstate, 0);

    auto itorfirstBBEnd = find<typename FamilyType::MI_BATCH_BUFFER_END *>(itorWalker, cmdList.end());
    ASSERT_NE(cmdList.end(), itorfirstBBEnd);

    enqueueKernel<FamilyType>(mockKernel);
    bitValue++;

    itorCmd = find<MEDIA_VFE_STATE *>(itorfirstBBEnd, cmdList.end());
    itorCmdForStateBase = find<STATE_BASE_ADDRESS *>(itorWalker, cmdList.end());
    ASSERT_NE(itorWalker, itorCmd);
    if (is64bit) {
        ASSERT_NE(itorCmdForStateBase, itorCmd);
    } else {
        //no SBA not dirty
        ASSERT_EQ(itorCmdForStateBase, cmdList.end());
    }

    auto *cmd2 = (MEDIA_VFE_STATE *)*itorCmd;

    // Verify we have a valid length
    EXPECT_EQ(maxNumberOfThreads, cmd2->getMaximumNumberOfThreads());
    EXPECT_NE(0u, cmd2->getNumberOfUrbEntries());
    EXPECT_NE(0u, cmd2->getUrbEntryAllocationSize());
    EXPECT_EQ(bitValue, cmd2->getPerThreadScratchSpace());
    EXPECT_EQ(bitValue, cmd2->getStackSize());
    auto graphicsAllocation2 = csr.getScratchAllocation();

    if (is32bit) {
        auto scratchBase = cmd2->getScratchSpaceBasePointer();
        EXPECT_NE(0u, scratchBase);
        auto graphicsAddress = graphicsAllocation2->getGpuAddress();
        EXPECT_EQ(graphicsAddress, scratchBase);
    } else {
        auto *sba2 = (STATE_BASE_ADDRESS *)*itorCmdForStateBase;
        auto GSHaddress2 = sba2->getGeneralStateBaseAddress();
        EXPECT_NE(0u, GSHaddress2);
        EXPECT_EQ(ScratchSpaceConstants::scratchSpaceOffsetFor64Bit, cmd2->getScratchSpaceBasePointer());
        EXPECT_NE(GSHaddress2, GSHaddress);
    }
    EXPECT_EQ(graphicsAllocation->getUnderlyingBufferSize(), allocationSize);
    EXPECT_NE(graphicsAllocation2, graphicsAllocation);

    // Generically validate this command
    PARSE::template validateCommand<MEDIA_VFE_STATE *>(cmdList.begin(), itorCmd);

    // Trigger SBA generation
    IndirectHeap dirtyDsh(nullptr);
    csr.dshState.updateAndCheck(&dirtyDsh);

    enqueueKernel<FamilyType>(mockKernel);
    auto finalItorToSBA = find<STATE_BASE_ADDRESS *>(itorCmd, cmdList.end());
    ASSERT_NE(finalItorToSBA, cmdList.end());
    auto *finalSba2 = (STATE_BASE_ADDRESS *)*finalItorToSBA;
    auto GSBaddress = finalSba2->getGeneralStateBaseAddress();
    if (is32bit) {
        EXPECT_EQ(0u, GSBaddress);
    } else if (is64bit) {
        EXPECT_EQ(graphicsAllocation2->getGpuAddress(), GSBaddress + ScratchSpaceConstants::scratchSpaceOffsetFor64Bit);
    }

    EXPECT_TRUE(csr.getAllocationsForReuse().peekIsEmpty());
}

INSTANTIATE_TEST_CASE_P(EnqueueKernel,
                        EnqueueScratchSpaceTests,
                        ::testing::ValuesIn(TestParamTable2));

typedef EnqueueKernelTypeTest<int> EnqueueKernelWithScratch;

HWTEST_P(EnqueueKernelWithScratch, GivenKernelRequiringScratchWhenItIsEnqueuedWithDifferentScratchSizesThenPreviousScratchAllocationIsMadeNonResidentPriorStoringOnResueList) {
    auto mockCsr = new MockCsrHw<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(mockCsr);

    uint32_t scratchSize = 1024u;

    MockKernelWithInternals mockKernel(*pClDevice);

    SPatchMediaVFEState mediaVFEstate;
    mediaVFEstate.PerThreadScratchSpace = scratchSize;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, mediaVFEstate, 0);

    uint32_t sizeToProgram = (scratchSize / static_cast<uint32_t>(MemoryConstants::kiloByte));
    uint32_t bitValue = 0u;
    while (sizeToProgram >>= 1) {
        bitValue++;
    }

    auto valueToProgram = PreambleHelper<FamilyType>::getScratchSizeValueToProgramMediaVfeState(scratchSize);
    EXPECT_EQ(bitValue, valueToProgram);

    enqueueKernel<FamilyType, false>(mockKernel);

    auto graphicsAllocation = mockCsr->getScratchAllocation();

    EXPECT_TRUE(mockCsr->isMadeResident(graphicsAllocation));

    // Enqueue With ScratchSize bigger than previous
    scratchSize = 8196;
    mediaVFEstate.PerThreadScratchSpace = scratchSize;

    enqueueKernel<FamilyType, false>(mockKernel);

    EXPECT_TRUE(mockCsr->isMadeNonResident(graphicsAllocation));
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueKernelWithScratch, givenDeviceForcing32bitAllocationsWhenKernelWithScratchIsEnqueuedThenGeneralStateHeapBaseAddressIsCorrectlyProgrammedAndMediaVFEStateContainsProgramming) {

    typedef typename FamilyType::PARSE PARSE;
    typedef typename PARSE::MEDIA_VFE_STATE MEDIA_VFE_STATE;
    typedef typename PARSE::STATE_BASE_ADDRESS STATE_BASE_ADDRESS;

    if (is64bit) {
        CommandStreamReceiver *csr = &pDevice->getGpgpuCommandStreamReceiver();
        auto memoryManager = csr->getMemoryManager();
        memoryManager->setForce32BitAllocations(true);

        auto scratchSize = 1024;

        MockKernelWithInternals mockKernel(*pClDevice);

        SPatchMediaVFEState mediaVFEstate;
        mediaVFEstate.PerThreadScratchSpace = scratchSize;
        populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, mediaVFEstate, 0);

        enqueueKernel<FamilyType>(mockKernel);
        auto graphicsAllocation = csr->getScratchAllocation();
        EXPECT_TRUE(graphicsAllocation->is32BitAllocation());
        auto graphicsAddress = (uint64_t)graphicsAllocation->getGpuAddress();
        auto baseAddress = graphicsAllocation->getGpuBaseAddress();

        // All state should be programmed before walker
        auto itorCmd = find<MEDIA_VFE_STATE *>(itorPipelineSelect, itorWalker);
        auto itorCmdForStateBase = find<STATE_BASE_ADDRESS *>(itorPipelineSelect, itorWalker);

        auto *mediaVfeState = (MEDIA_VFE_STATE *)*itorCmd;
        auto scratchBaseLowPart = (uint64_t)mediaVfeState->getScratchSpaceBasePointer();
        auto scratchBaseHighPart = (uint64_t)mediaVfeState->getScratchSpaceBasePointerHigh();
        uint64_t scratchBaseAddr = scratchBaseHighPart << 32 | scratchBaseLowPart;

        EXPECT_EQ(graphicsAddress - baseAddress, scratchBaseAddr);

        ASSERT_NE(itorCmdForStateBase, itorWalker);
        auto *sba = (STATE_BASE_ADDRESS *)*itorCmdForStateBase;

        auto GSHaddress = sba->getGeneralStateBaseAddress();

        EXPECT_EQ(memoryManager->getExternalHeapBaseAddress(graphicsAllocation->getRootDeviceIndex(), graphicsAllocation->isAllocatedInLocalMemoryPool()), GSHaddress);

        //now re-try to see if SBA is not programmed

        scratchSize *= 2;

        mediaVFEstate.PerThreadScratchSpace = scratchSize;

        enqueueKernel<FamilyType>(mockKernel);

        itorCmdForStateBase = find<STATE_BASE_ADDRESS *>(itorWalker, cmdList.end());
        EXPECT_EQ(itorCmdForStateBase, cmdList.end());
    }
}

INSTANTIATE_TEST_CASE_P(EnqueueKernel,
                        EnqueueKernelWithScratch, testing::Values(1));

TestParam TestParamPrintf[] = {
    {1, 1, 1, 1, 1, 1}};

typedef EnqueueKernelTypeTest<TestParam> EnqueueKernelPrintfTest;

HWTEST_P(EnqueueKernelPrintfTest, GivenKernelWithPrintfThenPatchCrossThreadData) {
    typedef typename FamilyType::PARSE PARSE;

    MockKernelWithInternals mockKernel(*pClDevice);
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;
    mockKernel.crossThreadData[64] = 0;

    SPatchAllocateStatelessPrintfSurface patchData;
    patchData.Size = 256;
    patchData.DataParamOffset = 64;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, patchData);

    enqueueKernel<FamilyType, false>(mockKernel);

    EXPECT_EQ(mockKernel.crossThreadData[64], 0);
}

HWTEST_P(EnqueueKernelPrintfTest, GivenKernelWithPrintfWhenBeingDispatchedThenL3CacheIsFlushed) {
    typedef typename FamilyType::PARSE PARSE;

    MockCommandQueueHw<FamilyType> mockCmdQueue(context, pClDevice, nullptr);
    MockKernelWithInternals mockKernel(*pClDevice);
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;
    mockKernel.crossThreadData[64] = 0;

    SPatchAllocateStatelessPrintfSurface patchData;
    patchData.Size = 256;
    patchData.DataParamOffset = 64;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, patchData);

    auto &csr = mockCmdQueue.getGpgpuCommandStreamReceiver();
    auto latestSentTaskCount = csr.peekTaskCount();

    cl_uint workDim = 1;
    size_t globalWorkOffset[3] = {0, 0, 0};

    cl_uint numEventsInWaitList = 0;
    cl_event *eventWaitList = nullptr;
    cl_event *event = nullptr;

    FillValues();
    // Compute # of expected work items
    expectedWorkItems = 1;
    for (auto i = 0u; i < workDim; i++) {
        expectedWorkItems *= globalWorkSize[i];
    }

    auto retVal = mockCmdQueue.enqueueKernel(
        mockKernel,
        workDim,
        globalWorkOffset,
        globalWorkSize,
        localWorkSize,
        numEventsInWaitList,
        eventWaitList,
        event);
    ASSERT_EQ(CL_SUCCESS, retVal);

    auto newLatestSentTaskCount = csr.peekTaskCount();
    EXPECT_GT(newLatestSentTaskCount, latestSentTaskCount);
    EXPECT_EQ(mockCmdQueue.latestTaskCountWaited, newLatestSentTaskCount);
}

HWCMDTEST_P(IGFX_GEN8_CORE, EnqueueKernelPrintfTest, GivenKernelWithPrintfBlockedByEventWhenEventUnblockedThenL3CacheIsFlushed) {
    typedef typename FamilyType::PARSE PARSE;

    UserEvent userEvent(context);

    MockCommandQueueHw<FamilyType> mockCommandQueue(context, pClDevice, nullptr);
    MockKernelWithInternals mockKernel(*pClDevice);
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;
    mockKernel.crossThreadData[64] = 0;

    SPatchAllocateStatelessPrintfSurface patchData;
    patchData.Size = 256;
    patchData.DataParamOffset = 64;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, patchData);
    auto &csr = mockCommandQueue.getGpgpuCommandStreamReceiver();
    auto latestSentDcFlushTaskCount = csr.peekTaskCount();

    cl_uint workDim = 1;
    size_t globalWorkOffset[3] = {0, 0, 0};

    FillValues();

    cl_event blockedEvent = &userEvent;
    auto retVal = mockCommandQueue.enqueueKernel(
        mockKernel,
        workDim,
        globalWorkOffset,
        globalWorkSize,
        localWorkSize,
        1,
        &blockedEvent,
        nullptr);
    ASSERT_EQ(CL_SUCCESS, retVal);

    userEvent.setStatus(CL_COMPLETE);

    parseCommands<FamilyType>(mockCommandQueue);

    auto newLatestSentDCFlushTaskCount = csr.peekTaskCount();
    EXPECT_GT(newLatestSentDCFlushTaskCount, latestSentDcFlushTaskCount);
    EXPECT_EQ(mockCommandQueue.latestTaskCountWaited, newLatestSentDCFlushTaskCount);
}

HWTEST_P(EnqueueKernelPrintfTest, GivenKernelWithPrintfBlockedByEventWhenEventUnblockedThenOutputPrinted) {
    typedef typename FamilyType::PARSE PARSE;

    // In scenarios with 32bit allocator and 64 bit tests this code won't work
    // due to inability to retrieve original buffer pointer as it is done in this test.
    auto memoryManager = pDevice->getMemoryManager();
    if (!memoryManager->peekForce32BitAllocations() && !memoryManager->isLimitedRange(0)) {
        testing::internal::CaptureStdout();

        auto userEvent = make_releaseable<UserEvent>(context);

        MockKernelWithInternals mockKernel(*pClDevice);
        mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;

        SPatchAllocateStatelessPrintfSurface patchData;
        patchData.SurfaceStateHeapOffset = undefined<uint32_t>;
        patchData.Size = 256;
        patchData.DataParamSize = 8;
        patchData.DataParamOffset = 0;
        populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, patchData);

        auto crossThreadData = reinterpret_cast<uint64_t *>(mockKernel.mockKernel->getCrossThreadData(rootDeviceIndex));

        std::string testString = "test";

        mockKernel.kernelInfo.kernelDescriptor.kernelMetadata.printfStringsMap.insert(std::make_pair(0, testString));

        cl_uint workDim = 1;
        size_t globalWorkOffset[3] = {0, 0, 0};

        FillValues();

        cl_event blockedEvent = userEvent.get();
        auto retVal = pCmdQ->enqueueKernel(
            mockKernel,
            workDim,
            globalWorkOffset,
            globalWorkSize,
            localWorkSize,
            1,
            &blockedEvent,
            nullptr);

        ASSERT_EQ(CL_SUCCESS, retVal);

        auto printfAllocation = reinterpret_cast<uint32_t *>(*crossThreadData);
        printfAllocation[0] = 8;
        printfAllocation[1] = 0;

        userEvent->setStatus(CL_COMPLETE);

        std::string output = testing::internal::GetCapturedStdout();
        EXPECT_STREQ("test", output.c_str());
    }
}

INSTANTIATE_TEST_CASE_P(EnqueueKernel,
                        EnqueueKernelPrintfTest,
                        ::testing::ValuesIn(TestParamPrintf));

using EnqueueKernelTests = ::testing::Test;

HWTEST_F(EnqueueKernelTests, whenEnqueueingKernelThenCsrCorrectlySetsRequiredThreadArbitrationPolicy) {
    struct myCsr : public UltCommandStreamReceiver<FamilyType> {
        using CommandStreamReceiverHw<FamilyType>::requiredThreadArbitrationPolicy;
    };

    cl_uint workDim = 1;
    size_t globalWorkOffset[3] = {0, 0, 0};
    size_t globalWorkSize[3] = {1, 1, 1};
    size_t localWorkSize[3] = {1, 1, 1};

    UltClDeviceFactory clDeviceFactory{1, 0};
    MockContext context{clDeviceFactory.rootDevices[0]};

    SPatchExecutionEnvironment sPatchExecEnv = {};

    sPatchExecEnv.SubgroupIndependentForwardProgressRequired = true;
    MockKernelWithInternals mockKernelWithInternalsWithIfpRequired{*clDeviceFactory.rootDevices[0], sPatchExecEnv};

    sPatchExecEnv.SubgroupIndependentForwardProgressRequired = false;
    MockKernelWithInternals mockKernelWithInternalsWithIfpNotRequired{*clDeviceFactory.rootDevices[0], sPatchExecEnv};

    cl_int retVal;
    std::unique_ptr<CommandQueue> pCommandQueue{CommandQueue::create(&context, clDeviceFactory.rootDevices[0], nullptr, true, retVal)};
    auto &csr = static_cast<myCsr &>(pCommandQueue->getGpgpuCommandStreamReceiver());

    pCommandQueue->enqueueKernel(
        mockKernelWithInternalsWithIfpRequired.mockKernel,
        workDim,
        globalWorkOffset,
        globalWorkSize,
        localWorkSize,
        0,
        nullptr,
        nullptr);
    pCommandQueue->flush();
    EXPECT_EQ(HwHelperHw<FamilyType>::get().getDefaultThreadArbitrationPolicy(), csr.requiredThreadArbitrationPolicy);

    pCommandQueue->enqueueKernel(
        mockKernelWithInternalsWithIfpNotRequired.mockKernel,
        workDim,
        globalWorkOffset,
        globalWorkSize,
        localWorkSize,
        0,
        nullptr,
        nullptr);
    pCommandQueue->flush();
    EXPECT_EQ(ThreadArbitrationPolicy::AgeBased, csr.requiredThreadArbitrationPolicy);

    pCommandQueue->enqueueKernel(
        mockKernelWithInternalsWithIfpRequired.mockKernel,
        workDim,
        globalWorkOffset,
        globalWorkSize,
        localWorkSize,
        0,
        nullptr,
        nullptr);
    pCommandQueue->flush();
    EXPECT_EQ(HwHelperHw<FamilyType>::get().getDefaultThreadArbitrationPolicy(), csr.requiredThreadArbitrationPolicy);
}

typedef HelloWorldFixture<HelloWorldFixtureFactory> EnqueueKernelFixture;
typedef Test<EnqueueKernelFixture> EnqueueKernelTest;

struct EnqueueAuxKernelTests : public EnqueueKernelTest {
    template <typename FamilyType>
    class MyCmdQ : public CommandQueueHw<FamilyType> {
      public:
        using CommandQueueHw<FamilyType>::commandStream;
        using CommandQueueHw<FamilyType>::gpgpuEngine;
        using CommandQueueHw<FamilyType>::bcsEngine;
        MyCmdQ(Context *context, ClDevice *device) : CommandQueueHw<FamilyType>(context, device, nullptr, false) {}
        void dispatchAuxTranslationBuiltin(MultiDispatchInfo &multiDispatchInfo, AuxTranslationDirection auxTranslationDirection) override {
            CommandQueueHw<FamilyType>::dispatchAuxTranslationBuiltin(multiDispatchInfo, auxTranslationDirection);
            auxTranslationDirections.push_back(auxTranslationDirection);
            Kernel *lastKernel = nullptr;
            for (const auto &dispatchInfo : multiDispatchInfo) {
                lastKernel = dispatchInfo.getKernel();
                dispatchInfos.emplace_back(dispatchInfo);
            }
            dispatchAuxTranslationInputs.emplace_back(lastKernel, multiDispatchInfo.size(), *multiDispatchInfo.getKernelObjsForAuxTranslation(),
                                                      auxTranslationDirection);
        }

        void waitUntilComplete(uint32_t gpgpuTaskCountToWait, uint32_t bcsTaskCountToWait, FlushStamp flushStampToWait, bool useQuickKmdSleep) override {
            waitCalled++;
            CommandQueueHw<FamilyType>::waitUntilComplete(gpgpuTaskCountToWait, bcsTaskCountToWait, flushStampToWait, useQuickKmdSleep);
        }

        std::vector<AuxTranslationDirection> auxTranslationDirections;
        std::vector<DispatchInfo> dispatchInfos;
        std::vector<std::tuple<Kernel *, size_t, KernelObjsForAuxTranslation, AuxTranslationDirection>> dispatchAuxTranslationInputs;
        uint32_t waitCalled = 0;
    };

    void SetUp() override {
        DebugManager.flags.ForceAuxTranslationMode.set(static_cast<int32_t>(AuxTranslationMode::Builtin));
        EnqueueKernelTest::SetUp();
    }

    DebugManagerStateRestore dbgRestore;
};

HWTEST_F(EnqueueAuxKernelTests, givenKernelWithRequiredAuxTranslationAndWithoutArgumentsWhenEnqueuedThenNoGuardKernelWithAuxTranslations) {
    MockKernelWithInternals mockKernel(*pClDevice, context);
    MyCmdQ<FamilyType> cmdQ(context, pClDevice);
    size_t gws[3] = {1, 0, 0};

    mockKernel.mockKernel->auxTranslationRequired = true;
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(0u, cmdQ.dispatchAuxTranslationInputs.size());
}

HWTEST_F(EnqueueAuxKernelTests, givenMultipleArgsWhenAuxTranslationIsRequiredThenPickOnlyApplicableBuffers) {
    REQUIRE_AUX_RESOLVES();

    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.RenderCompressedBuffersEnabled.set(1);

    MyCmdQ<FamilyType> cmdQ(context, pClDevice);
    size_t gws[3] = {1, 0, 0};
    MockBuffer buffer0, buffer1, buffer2, buffer3;
    cl_mem clMem0 = &buffer0;
    cl_mem clMem1 = &buffer1;
    cl_mem clMem2 = &buffer2;
    cl_mem clMem3 = &buffer3;
    buffer0.getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER);
    buffer1.getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER);
    buffer2.getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
    buffer3.getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);

    MockKernelWithInternals mockKernel(*pClDevice, context);
    mockKernel.kernelInfo.kernelArgInfo.resize(6);
    for (auto &kernelInfo : mockKernel.kernelInfo.kernelArgInfo) {
        kernelInfo.kernelArgPatchInfoVector.resize(1);
    }

    mockKernel.kernelInfo.kernelArgInfo.at(0).isBuffer = true;
    mockKernel.kernelInfo.kernelArgInfo.at(0).pureStatefulBufferAccess = false;
    mockKernel.kernelInfo.kernelArgInfo.at(1).isBuffer = true;
    mockKernel.kernelInfo.kernelArgInfo.at(1).pureStatefulBufferAccess = true;
    mockKernel.kernelInfo.kernelArgInfo.at(2).isBuffer = true;
    mockKernel.kernelInfo.kernelArgInfo.at(2).pureStatefulBufferAccess = false;
    mockKernel.kernelInfo.kernelArgInfo.at(3).isBuffer = true;
    mockKernel.kernelInfo.kernelArgInfo.at(3).pureStatefulBufferAccess = true;
    mockKernel.kernelInfo.kernelArgInfo.at(4).isBuffer = true;
    mockKernel.kernelInfo.kernelArgInfo.at(4).pureStatefulBufferAccess = false;
    mockKernel.kernelInfo.kernelArgInfo.at(5).isBuffer = true;
    mockKernel.kernelInfo.kernelArgInfo.at(5).pureStatefulBufferAccess = false;

    mockKernel.mockKernel->initialize();
    EXPECT_TRUE(mockKernel.mockKernel->auxTranslationRequired);

    mockKernel.mockKernel->setArgBuffer(0, sizeof(cl_mem *), &clMem0);                    // stateless on regular buffer - dont insert
    mockKernel.mockKernel->setArgBuffer(1, sizeof(cl_mem *), &clMem1);                    // stateful on regular buffer - dont insert
    mockKernel.mockKernel->setArgBuffer(2, sizeof(cl_mem *), &clMem2);                    // stateless on BUFFER_COMPRESSED - insert
    mockKernel.mockKernel->setArgBuffer(3, sizeof(cl_mem *), &clMem3);                    // stateful on BUFFER_COMPRESSED - dont insert
    mockKernel.mockKernel->setArgBuffer(4, sizeof(cl_mem *), nullptr);                    // nullptr - dont insert
    mockKernel.mockKernel->kernelArguments.at(5).type = Kernel::kernelArgType::IMAGE_OBJ; // non-buffer arg - dont insert

    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(2u, cmdQ.dispatchAuxTranslationInputs.size());

    EXPECT_EQ(1u, std::get<KernelObjsForAuxTranslation>(cmdQ.dispatchAuxTranslationInputs.at(0)).size()); // before kernel
    EXPECT_EQ(1u, std::get<KernelObjsForAuxTranslation>(cmdQ.dispatchAuxTranslationInputs.at(1)).size()); // after kernel

    EXPECT_EQ(&buffer2, (*std::get<KernelObjsForAuxTranslation>(cmdQ.dispatchAuxTranslationInputs.at(0)).begin()).object);
    EXPECT_EQ(&buffer2, (*std::get<KernelObjsForAuxTranslation>(cmdQ.dispatchAuxTranslationInputs.at(1)).begin()).object);

    auto cmdStream = cmdQ.commandStream;
    auto sizeUsed = cmdStream->getUsed();
    GenCmdList cmdList;
    ASSERT_TRUE(FamilyType::PARSE::parseCommandBuffer(cmdList, cmdStream->getCpuBase(), sizeUsed));

    auto pipeControls = findAll<typename FamilyType::PIPE_CONTROL *>(cmdList.begin(), cmdList.end());

    auto additionalPcCount = MemorySynchronizationCommands<FamilyType>::getSizeForPipeControlWithPostSyncOperation(
                                 pDevice->getHardwareInfo()) /
                             sizeof(typename FamilyType::PIPE_CONTROL);

    // |AuxToNonAux|NDR|NonAuxToAux|
    ASSERT_EQ(4u + additionalPcCount, pipeControls.size());

    ASSERT_EQ(2u, cmdQ.auxTranslationDirections.size());
    EXPECT_EQ(AuxTranslationDirection::AuxToNonAux, cmdQ.auxTranslationDirections[0]);
    EXPECT_EQ(AuxTranslationDirection::NonAuxToAux, cmdQ.auxTranslationDirections[1]);
}

HWTEST_F(EnqueueAuxKernelTests, givenKernelWithRequiredAuxTranslationWhenEnqueuedThenDispatchAuxTranslationBuiltin) {
    MockKernelWithInternals mockKernel(*pClDevice, context);
    MyCmdQ<FamilyType> cmdQ(context, pClDevice);
    size_t gws[3] = {1, 0, 0};
    MockBuffer buffer;
    cl_mem clMem = &buffer;

    buffer.getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
    mockKernel.kernelInfo.kernelArgInfo.resize(1);
    mockKernel.kernelInfo.kernelArgInfo.at(0).kernelArgPatchInfoVector.resize(1);
    mockKernel.kernelInfo.kernelArgInfo.at(0).pureStatefulBufferAccess = false;
    mockKernel.mockKernel->initialize();
    mockKernel.mockKernel->auxTranslationRequired = true;
    mockKernel.mockKernel->setArgBuffer(0, sizeof(cl_mem *), &clMem);

    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(2u, cmdQ.dispatchAuxTranslationInputs.size());

    // before kernel
    EXPECT_EQ(1u, std::get<size_t>(cmdQ.dispatchAuxTranslationInputs.at(0))); // aux before NDR
    auto kernelBefore = std::get<Kernel *>(cmdQ.dispatchAuxTranslationInputs.at(0));
    EXPECT_EQ("fullCopy", kernelBefore->getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelMetadata.kernelName);
    EXPECT_TRUE(kernelBefore->isBuiltIn);

    // after kernel
    EXPECT_EQ(3u, std::get<size_t>(cmdQ.dispatchAuxTranslationInputs.at(1))); // aux + NDR + aux
    auto kernelAfter = std::get<Kernel *>(cmdQ.dispatchAuxTranslationInputs.at(1));
    EXPECT_EQ("fullCopy", kernelAfter->getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelMetadata.kernelName);
    EXPECT_TRUE(kernelAfter->isBuiltIn);
}

HWTEST_F(EnqueueAuxKernelTests, givenDebugVariableDisablingBuiltinTranslationWhenDispatchingKernelWithRequiredAuxTranslationThenDontDispatch) {
    DebugManager.flags.ForceAuxTranslationMode.set(static_cast<int32_t>(AuxTranslationMode::Blit));
    pDevice->getUltCommandStreamReceiver<FamilyType>().timestampPacketWriteEnabled = true;

    auto hwInfo = pDevice->getExecutionEnvironment()->rootDeviceEnvironments[rootDeviceIndex]->getMutableHardwareInfo();

    MockKernelWithInternals mockKernel(*pClDevice, context);
    MyCmdQ<FamilyType> cmdQ(context, pClDevice);
    cmdQ.bcsEngine = cmdQ.gpgpuEngine;

    hwInfo->capabilityTable.blitterOperationsSupported = true;

    size_t gws[3] = {1, 0, 0};
    MockBuffer buffer;
    cl_mem clMem = &buffer;

    buffer.getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
    mockKernel.kernelInfo.kernelArgInfo.resize(1);
    mockKernel.kernelInfo.kernelArgInfo.at(0).kernelArgPatchInfoVector.resize(1);
    mockKernel.kernelInfo.kernelArgInfo.at(0).pureStatefulBufferAccess = false;
    mockKernel.mockKernel->initialize();
    mockKernel.mockKernel->auxTranslationRequired = true;
    mockKernel.mockKernel->setArgBuffer(0, sizeof(cl_mem *), &clMem);

    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(0u, cmdQ.dispatchAuxTranslationInputs.size());
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueKernelTest, givenCacheFlushAfterWalkerEnabledWhenAllocationRequiresCacheFlushThenFlushCommandPresentAfterWalker) {
    using GPGPU_WALKER = typename FamilyType::GPGPU_WALKER;
    using PIPE_CONTROL = typename FamilyType::PIPE_CONTROL;

    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.EnableCacheFlushAfterWalker.set(1);

    MockKernelWithInternals mockKernel(*pClDevice, context);
    CommandQueueHw<FamilyType> cmdQ(context, pClDevice, nullptr, false);

    size_t gws[3] = {1, 0, 0};

    mockKernel.mockKernel->svmAllocationsRequireCacheFlush = true;

    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);

    HardwareParse hwParse;
    hwParse.parseCommands<FamilyType>(cmdQ.getCS(0), 0);
    auto itorCmd = find<GPGPU_WALKER *>(hwParse.cmdList.begin(), hwParse.cmdList.end());
    ASSERT_NE(hwParse.cmdList.end(), itorCmd);
    itorCmd = find<PIPE_CONTROL *>(itorCmd, hwParse.cmdList.end());
    auto pipeControl = genCmdCast<PIPE_CONTROL *>(*itorCmd);
    ASSERT_NE(nullptr, pipeControl);
    EXPECT_TRUE(pipeControl->getCommandStreamerStallEnable());
    EXPECT_TRUE(pipeControl->getDcFlushEnable());
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueAuxKernelTests, givenParentKernelWhenAuxTranslationIsRequiredThenMakeEnqueueBlocking) {
    REQUIRE_DEVICE_ENQUEUE_OR_SKIP(pClDevice);

    MyCmdQ<FamilyType> cmdQ(context, pClDevice);
    size_t gws[3] = {1, 0, 0};

    cl_queue_properties queueProperties = {};
    auto mockDevQueue = std::make_unique<MockDeviceQueueHw<FamilyType>>(context, pClDevice, queueProperties);
    context->setDefaultDeviceQueue(mockDevQueue.get());
    std::unique_ptr<MockParentKernel> parentKernel(MockParentKernel::create(*context, false, false, false, false, false));
    parentKernel->initialize();

    parentKernel->auxTranslationRequired = false;
    cmdQ.enqueueKernel(parentKernel.get(), 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(0u, cmdQ.waitCalled);
    mockDevQueue->getIgilQueue()->m_controls.m_CriticalSection = 0;

    parentKernel->auxTranslationRequired = true;
    cmdQ.enqueueKernel(parentKernel.get(), 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(1u, cmdQ.waitCalled);
}

HWCMDTEST_F(IGFX_GEN8_CORE, EnqueueAuxKernelTests, givenParentKernelButNoDeviceQueueWhenEnqueueIsCalledItReturnsInvalidOperation) {
    REQUIRE_DEVICE_ENQUEUE_OR_SKIP(pClDevice);

    MyCmdQ<FamilyType> cmdQ(context, pClDevice);
    size_t gws[3] = {1, 0, 0};

    std::unique_ptr<MockParentKernel> parentKernel(MockParentKernel::create(*context, false, false, false, false, false));
    parentKernel->initialize();

    auto status = cmdQ.enqueueKernel(parentKernel.get(), 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(CL_INVALID_OPERATION, status);
}
