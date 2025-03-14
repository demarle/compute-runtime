/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/aub/aub_subcapture.h"
#include "shared/source/program/sync_buffer_handler.h"
#include "shared/test/common/cmd_parse/hw_parse.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/source/event/user_event.h"
#include "opencl/source/platform/platform.h"
#include "opencl/test/unit_test/command_stream/thread_arbitration_policy_helper.h"
#include "opencl/test/unit_test/fixtures/enqueue_handler_fixture.h"
#include "opencl/test/unit_test/helpers/unit_test_helper.h"
#include "opencl/test/unit_test/mocks/mock_aub_csr.h"
#include "opencl/test/unit_test/mocks/mock_aub_subcapture_manager.h"
#include "opencl/test/unit_test/mocks/mock_command_queue.h"
#include "opencl/test/unit_test/mocks/mock_context.h"
#include "opencl/test/unit_test/mocks/mock_csr.h"
#include "opencl/test/unit_test/mocks/mock_internal_allocation_storage.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_mdi.h"
#include "opencl/test/unit_test/mocks/mock_platform.h"
#include "opencl/test/unit_test/test_macros/test_checks_ocl.h"
#include "test.h"

using namespace NEO;

HWTEST_F(EnqueueHandlerTest, WhenEnqueingHandlerWithKernelThenProcessEvictionOnCsrIsCalled) {
    int32_t tag;
    auto csr = new MockCsrBase<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(csr);

    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    size_t gws[] = {1, 1, 1};
    mockCmdQ->enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);

    EXPECT_TRUE(csr->processEvictionCalled);
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWithKernelWhenAubCsrIsActiveThenAddCommentWithKernelName) {
    int32_t tag;
    auto aubCsr = new MockCsrAub<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(aubCsr);

    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    size_t gws[] = {1, 1, 1};
    mockKernel.kernelInfo.kernelDescriptor.kernelMetadata.kernelName = "kernel_name";
    mockCmdQ->enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);

    EXPECT_TRUE(aubCsr->addAubCommentCalled);

    EXPECT_EQ(1u, aubCsr->aubCommentMessages.size());
    EXPECT_STREQ("kernel_name", aubCsr->aubCommentMessages[0].c_str());
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWithKernelSplitWhenAubCsrIsActiveThenAddCommentWithKernelName) {
    int32_t tag;
    auto aubCsr = new MockCsrAub<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(aubCsr);

    MockKernelWithInternals kernel1(*pClDevice);
    MockKernelWithInternals kernel2(*pClDevice);
    kernel1.kernelInfo.kernelDescriptor.kernelMetadata.kernelName = "kernel_1";
    kernel2.kernelInfo.kernelDescriptor.kernelMetadata.kernelName = "kernel_2";

    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));
    MockMultiDispatchInfo multiDispatchInfo(pClDevice, std::vector<Kernel *>({kernel1.mockKernel, kernel2.mockKernel}));

    mockCmdQ->template enqueueHandler<CL_COMMAND_WRITE_BUFFER>(nullptr, 0, true, multiDispatchInfo, 0, nullptr, nullptr);

    EXPECT_TRUE(aubCsr->addAubCommentCalled);

    EXPECT_EQ(2u, aubCsr->aubCommentMessages.size());
    EXPECT_STREQ("kernel_1", aubCsr->aubCommentMessages[0].c_str());
    EXPECT_STREQ("kernel_2", aubCsr->aubCommentMessages[1].c_str());
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWithEmptyDispatchInfoWhenAubCsrIsActiveThenDontAddCommentWithKernelName) {
    int32_t tag;
    auto aubCsr = new MockCsrAub<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(aubCsr);

    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    size_t gws[] = {0, 0, 0};
    mockKernel.kernelInfo.kernelDescriptor.kernelMetadata.kernelName = "kernel_name";
    mockCmdQ->enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);

    EXPECT_FALSE(aubCsr->addAubCommentCalled);
}

struct EnqueueHandlerWithAubSubCaptureTests : public EnqueueHandlerTest {
    template <typename FamilyType>
    class MockCmdQWithAubSubCapture : public CommandQueueHw<FamilyType> {
      public:
        MockCmdQWithAubSubCapture(Context *context, ClDevice *device) : CommandQueueHw<FamilyType>(context, device, nullptr, false) {}

        void waitUntilComplete(uint32_t gpgpuTaskCountToWait, uint32_t bcsTaskCountToWait, FlushStamp flushStampToWait, bool useQuickKmdSleep) override {
            waitUntilCompleteCalled = true;
            CommandQueueHw<FamilyType>::waitUntilComplete(gpgpuTaskCountToWait, bcsTaskCountToWait, flushStampToWait, useQuickKmdSleep);
        }

        void obtainNewTimestampPacketNodes(size_t numberOfNodes, TimestampPacketContainer &previousNodes, bool clearAllDependencies, bool blitEnqueue) override {
            timestampPacketDependenciesCleared = clearAllDependencies;
            CommandQueueHw<FamilyType>::obtainNewTimestampPacketNodes(numberOfNodes, previousNodes, clearAllDependencies, blitEnqueue);
        }

        bool waitUntilCompleteCalled = false;
        bool timestampPacketDependenciesCleared = false;
    };
};

HWTEST_F(EnqueueHandlerWithAubSubCaptureTests, givenEnqueueHandlerWithAubSubCaptureWhenSubCaptureIsNotActiveThenEnqueueIsMadeBlocking) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.AUBDumpSubCaptureMode.set(1);

    auto aubCsr = new MockAubCsr<FamilyType>("", true, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(aubCsr);

    AubSubCaptureCommon subCaptureCommon;
    subCaptureCommon.subCaptureMode = AubSubCaptureManager::SubCaptureMode::Filter;
    subCaptureCommon.subCaptureFilter.dumpKernelName = "invalid_kernel_name";
    auto subCaptureManagerMock = new AubSubCaptureManagerMock("file_name.aub", subCaptureCommon);
    aubCsr->subCaptureManager.reset(subCaptureManagerMock);

    MockCmdQWithAubSubCapture<FamilyType> cmdQ(context, pClDevice);
    MockKernelWithInternals mockKernel(*pClDevice);
    size_t gws[3] = {1, 0, 0};
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);

    EXPECT_TRUE(cmdQ.waitUntilCompleteCalled);
}

HWTEST_F(EnqueueHandlerWithAubSubCaptureTests, givenEnqueueHandlerWithAubSubCaptureWhenSubCaptureGetsActivatedThenTimestampPacketDependenciesAreClearedAndNextRemainUncleared) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.AUBDumpSubCaptureMode.set(1);
    DebugManager.flags.EnableTimestampPacket.set(true);

    auto aubCsr = new MockAubCsr<FamilyType>("", true, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(aubCsr);

    AubSubCaptureCommon subCaptureCommon;
    subCaptureCommon.subCaptureMode = AubSubCaptureManager::SubCaptureMode::Filter;
    subCaptureCommon.subCaptureFilter.dumpKernelName = "";
    subCaptureCommon.subCaptureFilter.dumpKernelStartIdx = 0;
    subCaptureCommon.subCaptureFilter.dumpKernelEndIdx = 1;
    auto subCaptureManagerMock = new AubSubCaptureManagerMock("file_name.aub", subCaptureCommon);
    aubCsr->subCaptureManager.reset(subCaptureManagerMock);

    MockCmdQWithAubSubCapture<FamilyType> cmdQ(context, pClDevice);
    MockKernelWithInternals mockKernel(*pClDevice);
    mockKernel.kernelInfo.kernelDescriptor.kernelMetadata.kernelName = "kernelName";
    size_t gws[3] = {1, 0, 0};

    // activate subcapture
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_TRUE(cmdQ.timestampPacketDependenciesCleared);

    // keep subcapture active
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_FALSE(cmdQ.timestampPacketDependenciesCleared);

    // deactivate subcapture
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_FALSE(cmdQ.timestampPacketDependenciesCleared);
}

template <typename GfxFamily>
class MyCommandQueueHw : public CommandQueueHw<GfxFamily> {
    typedef CommandQueueHw<GfxFamily> BaseClass;

  public:
    MyCommandQueueHw(Context *context, ClDevice *device, cl_queue_properties *properties) : BaseClass(context, device, properties, false){};
    Vec3<size_t> lws = {1, 1, 1};
    Vec3<size_t> elws = {1, 1, 1};
    void enqueueHandlerHook(const unsigned int commandType, const MultiDispatchInfo &multiDispatchInfo) override {
        elws = multiDispatchInfo.begin()->getEnqueuedWorkgroupSize();
        lws = multiDispatchInfo.begin()->getActualWorkgroupSize();
    }
};

HWTEST_F(EnqueueHandlerTest, givenLocalWorkgroupSizeGreaterThenGlobalWorkgroupSizeWhenEnqueueKernelThenLwsIsClamped) {
    int32_t tag;
    auto csr = new MockCsrBase<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(csr);
    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockProgram = mockKernel.mockProgram;
    mockProgram->setAllowNonUniform(true);
    MyCommandQueueHw<FamilyType> myCmdQ(context, pClDevice, 0);

    size_t lws1d[] = {4, 1, 1};
    size_t gws1d[] = {2, 1, 1};
    myCmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws1d, lws1d, 0, nullptr, nullptr);
    EXPECT_EQ(myCmdQ.elws.x, lws1d[0]);
    EXPECT_EQ(myCmdQ.lws.x, gws1d[0]);

    size_t lws2d[] = {3, 3, 1};
    size_t gws2d[] = {2, 1, 1};
    myCmdQ.enqueueKernel(mockKernel.mockKernel, 2, nullptr, gws2d, lws2d, 0, nullptr, nullptr);
    EXPECT_EQ(myCmdQ.elws.x, lws2d[0]);
    EXPECT_EQ(myCmdQ.elws.y, lws2d[1]);
    EXPECT_EQ(myCmdQ.lws.x, gws2d[0]);
    EXPECT_EQ(myCmdQ.lws.y, gws2d[1]);

    size_t lws3d[] = {5, 4, 3};
    size_t gws3d[] = {2, 2, 2};
    myCmdQ.enqueueKernel(mockKernel.mockKernel, 3, nullptr, gws3d, lws3d, 0, nullptr, nullptr);
    EXPECT_EQ(myCmdQ.elws.x, lws3d[0]);
    EXPECT_EQ(myCmdQ.elws.y, lws3d[1]);
    EXPECT_EQ(myCmdQ.elws.z, lws3d[2]);
    EXPECT_EQ(myCmdQ.lws.x, gws3d[0]);
    EXPECT_EQ(myCmdQ.lws.y, gws3d[1]);
    EXPECT_EQ(myCmdQ.lws.z, gws3d[2]);
}

HWTEST_F(EnqueueHandlerTest, givenLocalWorkgroupSizeGreaterThenGlobalWorkgroupSizeAndNonUniformWorkGroupWhenEnqueueKernelThenClIvalidWorkGroupSizeIsReturned) {
    int32_t tag;
    auto csr = new MockCsrBase<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(csr);
    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockProgram = mockKernel.mockProgram;
    mockProgram->setAllowNonUniform(false);
    MyCommandQueueHw<FamilyType> myCmdQ(context, pClDevice, 0);

    size_t lws1d[] = {4, 1, 1};
    size_t gws1d[] = {2, 1, 1};
    cl_int retVal = myCmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws1d, lws1d, 0, nullptr, nullptr);
    EXPECT_EQ(retVal, CL_INVALID_WORK_GROUP_SIZE);
}

HWTEST_F(EnqueueHandlerTest, WhenEnqueuingHandlerCallOnEnqueueMarkerThenCallProcessEvictionOnCsrIsNotCalled) {
    int32_t tag;
    auto csr = new MockCsrBase<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(csr);

    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    mockCmdQ->enqueueMarkerWithWaitList(
        0,
        nullptr,
        nullptr);

    EXPECT_FALSE(csr->processEvictionCalled);
    EXPECT_EQ(0u, csr->madeResidentGfxAllocations.size());
    EXPECT_EQ(0u, csr->madeNonResidentGfxAllocations.size());
}

HWTEST_F(EnqueueHandlerTest, WhenEnqueuingHandlerForMarkerOnUnblockedQueueThenTaskLevelIsNotIncremented) {
    int32_t tag;
    auto csr = new MockCsrBase<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(csr);

    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    // put queue into initial unblocked state
    mockCmdQ->taskLevel = 0;

    mockCmdQ->enqueueMarkerWithWaitList(
        0,
        nullptr,
        nullptr);

    EXPECT_EQ(0u, mockCmdQ->taskLevel);
}

HWTEST_F(EnqueueHandlerTest, WhenEnqueuingHandlerForMarkerOnBlockedQueueThenTaskLevelIsNotIncremented) {
    int32_t tag;
    auto csr = new MockCsrBase<FamilyType>(tag, *pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    pDevice->resetCommandStreamReceiver(csr);

    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    // put queue into initial blocked state
    mockCmdQ->taskLevel = CompletionStamp::notReady;

    mockCmdQ->enqueueMarkerWithWaitList(
        0,
        nullptr,
        nullptr);

    EXPECT_EQ(CompletionStamp::notReady, mockCmdQ->taskLevel);
}

HWTEST_F(EnqueueHandlerTest, WhenEnqueuingBlockedWithoutReturnEventThenVirtualEventIsCreatedAndCommandQueueInternalRefCountIsIncremeted) {

    MockKernelWithInternals kernelInternals(*pClDevice, context);

    Kernel *kernel = kernelInternals.mockKernel;

    MockMultiDispatchInfo multiDispatchInfo(pClDevice, kernel);

    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    // put queue into initial blocked state
    mockCmdQ->taskLevel = CompletionStamp::notReady;

    auto initialRefCountInternal = mockCmdQ->getRefInternalCount();

    bool blocking = false;
    mockCmdQ->template enqueueHandler<CL_COMMAND_NDRANGE_KERNEL>(nullptr,
                                                                 0,
                                                                 blocking,
                                                                 multiDispatchInfo,
                                                                 0,
                                                                 nullptr,
                                                                 nullptr);

    EXPECT_NE(nullptr, mockCmdQ->virtualEvent);

    auto refCountInternal = mockCmdQ->getRefInternalCount();
    EXPECT_EQ(initialRefCountInternal + 1, refCountInternal);

    mockCmdQ->virtualEvent->setStatus(CL_COMPLETE);
    mockCmdQ->isQueueBlocked();
    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, WhenEnqueuingBlockedThenVirtualEventIsSetAsCurrentCmdQVirtualEvent) {

    MockKernelWithInternals kernelInternals(*pClDevice, context);

    Kernel *kernel = kernelInternals.mockKernel;

    MockMultiDispatchInfo multiDispatchInfo(pClDevice, kernel);

    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    // put queue into initial blocked state
    mockCmdQ->taskLevel = CompletionStamp::notReady;

    bool blocking = false;
    mockCmdQ->template enqueueHandler<CL_COMMAND_NDRANGE_KERNEL>(nullptr,
                                                                 0,
                                                                 blocking,
                                                                 multiDispatchInfo,
                                                                 0,
                                                                 nullptr,
                                                                 nullptr);

    ASSERT_NE(nullptr, mockCmdQ->virtualEvent);

    mockCmdQ->virtualEvent->setStatus(CL_COMPLETE);
    mockCmdQ->isQueueBlocked();
    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, WhenEnqueuingWithOutputEventThenEventIsRegistered) {
    MockKernelWithInternals kernelInternals(*pClDevice, context);
    Kernel *kernel = kernelInternals.mockKernel;
    MockMultiDispatchInfo multiDispatchInfo(pClDevice, kernel);
    cl_event outputEvent = nullptr;

    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    bool blocking = false;
    mockCmdQ->template enqueueHandler<CL_COMMAND_NDRANGE_KERNEL>(nullptr,
                                                                 0,
                                                                 blocking,
                                                                 multiDispatchInfo,
                                                                 0,
                                                                 nullptr,
                                                                 &outputEvent);
    ASSERT_NE(nullptr, outputEvent);
    Event *event = castToObjectOrAbort<Event>(outputEvent);
    ASSERT_NE(nullptr, event);

    event->release();
    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWhenAddPatchInfoCommentsForAUBDumpIsNotSetThenPatchInfoDataIsNotTransferredToCSR) {
    auto csr = new MockCsrHw2<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto mockHelper = new MockFlatBatchBufferHelper<FamilyType>(*pDevice->executionEnvironment);
    csr->overwriteFlatBatchBufferHelper(mockHelper);
    pDevice->resetCommandStreamReceiver(csr);

    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    size_t gws[] = {1, 1, 1};

    PatchInfoData patchInfoData = {0xaaaaaaaa, 0, PatchInfoAllocationType::KernelArg, 0xbbbbbbbb, 0, PatchInfoAllocationType::IndirectObjectHeap};
    mockKernel.mockKernel->getPatchInfoDataList().push_back(patchInfoData);

    EXPECT_CALL(*mockHelper, setPatchInfoData(::testing::_)).Times(0);
    mockCmdQ->enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWhenAddPatchInfoCommentsForAUBDumpIsSetThenPatchInfoDataIsTransferredToCSR) {
    DebugManagerStateRestore dbgRestore;
    DebugManager.flags.AddPatchInfoCommentsForAUBDump.set(true);
    DebugManager.flags.FlattenBatchBufferForAUBDump.set(true);

    auto csr = new MockCsrHw2<FamilyType>(*pDevice->executionEnvironment, pDevice->getRootDeviceIndex(), pDevice->getDeviceBitfield());
    auto mockHelper = new MockFlatBatchBufferHelper<FamilyType>(*pDevice->executionEnvironment);
    csr->overwriteFlatBatchBufferHelper(mockHelper);
    pDevice->resetCommandStreamReceiver(csr);

    MockKernelWithInternals mockKernel(*pClDevice);
    auto mockCmdQ = std::unique_ptr<MockCommandQueueHw<FamilyType>>(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));

    size_t gws[] = {1, 1, 1};

    PatchInfoData patchInfoData = {0xaaaaaaaa, 0, PatchInfoAllocationType::KernelArg, 0xbbbbbbbb, 0, PatchInfoAllocationType::IndirectObjectHeap};
    mockKernel.mockKernel->getPatchInfoDataList().push_back(patchInfoData);

    EXPECT_CALL(*mockHelper, setPatchInfoData(::testing::_)).Times(8);
    EXPECT_CALL(*mockHelper, registerCommandChunk(::testing::_)).Times(1);
    EXPECT_CALL(*mockHelper, registerBatchBufferStartAddress(::testing::_, ::testing::_)).Times(1);
    mockCmdQ->enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
}

HWTEST_F(EnqueueHandlerTest, givenExternallySynchronizedParentEventWhenRequestingEnqueueWithoutGpuSubmissionThenTaskCountIsNotInherited) {
    struct ExternallySynchEvent : UserEvent {
        ExternallySynchEvent() : UserEvent() {
            setStatus(CL_COMPLETE);
            this->updateTaskCount(7, 0);
        }
        bool isExternallySynchronized() const override {
            return true;
        }
    };

    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    ExternallySynchEvent synchEvent;
    cl_event inEv = &synchEvent;
    cl_event outEv = nullptr;

    bool blocking = false;
    MultiDispatchInfo emptyDispatchInfo;
    mockCmdQ->template enqueueHandler<CL_COMMAND_MARKER>(nullptr,
                                                         0,
                                                         blocking,
                                                         emptyDispatchInfo,
                                                         1U,
                                                         &inEv,
                                                         &outEv);
    Event *ouputEvent = castToObject<Event>(outEv);
    ASSERT_NE(nullptr, ouputEvent);
    EXPECT_EQ(0U, ouputEvent->peekTaskCount());

    ouputEvent->release();
    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWhenSubCaptureIsOffThenActivateSubCaptureIsNotCalled) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.AUBDumpSubCaptureMode.set(static_cast<int32_t>(AubSubCaptureManager::SubCaptureMode::Off));

    MockKernelWithInternals kernelInternals(*pClDevice, context);
    Kernel *kernel = kernelInternals.mockKernel;
    MockMultiDispatchInfo multiDispatchInfo(pClDevice, kernel);

    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    mockCmdQ->template enqueueHandler<CL_COMMAND_NDRANGE_KERNEL>(nullptr,
                                                                 0,
                                                                 false,
                                                                 multiDispatchInfo,
                                                                 0,
                                                                 nullptr,
                                                                 nullptr);
    EXPECT_FALSE(pDevice->getUltCommandStreamReceiver<FamilyType>().checkAndActivateAubSubCaptureCalled);

    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWhenSubCaptureIsOnThenActivateSubCaptureIsCalled) {
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.AUBDumpSubCaptureMode.set(static_cast<int32_t>(AubSubCaptureManager::SubCaptureMode::Filter));

    MockKernelWithInternals kernelInternals(*pClDevice, context);
    Kernel *kernel = kernelInternals.mockKernel;
    MockMultiDispatchInfo multiDispatchInfo(pClDevice, kernel);

    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    mockCmdQ->template enqueueHandler<CL_COMMAND_NDRANGE_KERNEL>(nullptr,
                                                                 0,
                                                                 false,
                                                                 multiDispatchInfo,
                                                                 0,
                                                                 nullptr,
                                                                 nullptr);
    EXPECT_TRUE(pDevice->getUltCommandStreamReceiver<FamilyType>().checkAndActivateAubSubCaptureCalled);

    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, givenEnqueueHandlerWhenClSetKernelExecInfoAlreadysetKernelThreadArbitrationPolicyThenRequiredThreadArbitrationPolicyIsSetProperly) {
    REQUIRE_SVM_OR_SKIP(pClDevice);
    DebugManagerStateRestore stateRestore;
    DebugManager.flags.AUBDumpSubCaptureMode.set(static_cast<int32_t>(AubSubCaptureManager::SubCaptureMode::Filter));

    MockKernelWithInternals kernelInternals(*pClDevice, context);
    Kernel *kernel = kernelInternals.mockKernel;
    MockMultiDispatchInfo multiDispatchInfo(pClDevice, kernel);

    uint32_t euThreadSetting = CL_KERNEL_EXEC_INFO_THREAD_ARBITRATION_POLICY_ROUND_ROBIN_INTEL;
    size_t ptrSizeInBytes = 1 * sizeof(uint32_t *);
    clSetKernelExecInfo(
        kernelInternals.mockMultiDeviceKernel,               // cl_kernel kernel
        CL_KERNEL_EXEC_INFO_THREAD_ARBITRATION_POLICY_INTEL, // cl_kernel_exec_info param_name
        ptrSizeInBytes,                                      // size_t param_value_size
        &euThreadSetting                                     // const void *param_value
    );
    auto mockCmdQ = new MockCommandQueueHw<FamilyType>(context, pClDevice, 0);

    mockCmdQ->template enqueueHandler<CL_COMMAND_NDRANGE_KERNEL>(nullptr,
                                                                 0,
                                                                 false,
                                                                 multiDispatchInfo,
                                                                 0,
                                                                 nullptr,
                                                                 nullptr);
    EXPECT_EQ(getNewKernelArbitrationPolicy(euThreadSetting), pDevice->getUltCommandStreamReceiver<FamilyType>().requiredThreadArbitrationPolicy);

    mockCmdQ->release();
}

HWTEST_F(EnqueueHandlerTest, givenKernelUsingSyncBufferWhenEnqueuingKernelThenSshIsCorrectlyProgrammed) {
    using BINDING_TABLE_STATE = typename FamilyType::BINDING_TABLE_STATE;
    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;

    struct MockSyncBufferHandler : SyncBufferHandler {
        using SyncBufferHandler::graphicsAllocation;
    };

    SPatchAllocateSyncBuffer sPatchAllocateSyncBuffer{};
    sPatchAllocateSyncBuffer.SurfaceStateHeapOffset = 0;
    sPatchAllocateSyncBuffer.DataParamOffset = 0;
    sPatchAllocateSyncBuffer.DataParamSize = sizeof(uint8_t);

    SPatchBindingTableState sPatchBindingTableState{};
    sPatchBindingTableState.Offset = sizeof(RENDER_SURFACE_STATE);
    sPatchBindingTableState.Count = 1;
    sPatchBindingTableState.SurfaceStateOffset = 0;

    pClDevice->allocateSyncBufferHandler();

    size_t offset = 0;
    size_t size = 1;

    size_t sshUsageWithoutSyncBuffer;

    {
        MockKernelWithInternals kernelInternals{*pClDevice, context};
        kernelInternals.kernelInfo.usesSsh = true;
        kernelInternals.kernelInfo.requiresSshForBuffers = true;
        auto kernel = kernelInternals.mockKernel;
        kernel->initialize();

        auto mockCmdQ = clUniquePtr(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));
        mockCmdQ->enqueueKernel(kernel, 1, &offset, &size, &size, 0, nullptr, nullptr);

        sshUsageWithoutSyncBuffer = mockCmdQ->getIndirectHeap(IndirectHeap::SURFACE_STATE, 0).getUsed();
    }

    {
        MockKernelWithInternals kernelInternals{*pClDevice, context};
        kernelInternals.kernelInfo.usesSsh = true;
        kernelInternals.kernelInfo.requiresSshForBuffers = true;
        populateKernelDescriptor(kernelInternals.kernelInfo.kernelDescriptor, sPatchAllocateSyncBuffer);
        populateKernelDescriptor(kernelInternals.kernelInfo.kernelDescriptor, sPatchBindingTableState);
        kernelInternals.kernelInfo.heapInfo.SurfaceStateHeapSize = sizeof(RENDER_SURFACE_STATE) + sizeof(BINDING_TABLE_STATE);
        auto kernel = kernelInternals.mockKernel;
        kernel->initialize();

        auto bindingTableState = reinterpret_cast<BINDING_TABLE_STATE *>(
            ptrOffset(kernel->getSurfaceStateHeap(rootDeviceIndex), sPatchBindingTableState.Offset));
        bindingTableState->setSurfaceStatePointer(0);

        auto mockCmdQ = clUniquePtr(new MockCommandQueueHw<FamilyType>(context, pClDevice, 0));
        mockCmdQ->enqueueKernel(kernel, 1, &offset, &size, &size, 0, nullptr, nullptr);

        auto &surfaceStateHeap = mockCmdQ->getIndirectHeap(IndirectHeap::SURFACE_STATE, 0);
        EXPECT_EQ(sshUsageWithoutSyncBuffer + kernelInternals.kernelInfo.heapInfo.SurfaceStateHeapSize, surfaceStateHeap.getUsed());

        HardwareParse hwParser;
        hwParser.parseCommands<FamilyType>(*mockCmdQ);

        auto &surfaceState = hwParser.getSurfaceState<FamilyType>(&surfaceStateHeap, 0);
        auto pSyncBufferHandler = static_cast<MockSyncBufferHandler *>(pClDevice->syncBufferHandler.get());
        EXPECT_EQ(pSyncBufferHandler->graphicsAllocation->getGpuAddress(), surfaceState.getSurfaceBaseAddress());
    }
}

struct EnqueueHandlerTestBasic : public ::testing::Test {
    template <typename FamilyType>
    std::unique_ptr<MockCommandQueueHw<FamilyType>> setupFixtureAndCreateMockCommandQueue() {
        auto executionEnvironment = platform()->peekExecutionEnvironment();

        device = std::make_unique<MockClDevice>(MockDevice::createWithExecutionEnvironment<MockDevice>(nullptr, executionEnvironment, 0u));
        context = std::make_unique<MockContext>(device.get());

        auto mockCmdQ = std::make_unique<MockCommandQueueHw<FamilyType>>(context.get(), device.get(), nullptr);

        auto &ultCsr = static_cast<UltCommandStreamReceiver<FamilyType> &>(mockCmdQ->getGpgpuCommandStreamReceiver());
        ultCsr.taskCount = initialTaskCount;

        mockInternalAllocationStorage = new MockInternalAllocationStorage(ultCsr);
        ultCsr.internalAllocationStorage.reset(mockInternalAllocationStorage);

        return mockCmdQ;
    }

    MockInternalAllocationStorage *mockInternalAllocationStorage = nullptr;
    const uint32_t initialTaskCount = 100;
    std::unique_ptr<MockClDevice> device;
    std::unique_ptr<MockContext> context;
};

HWTEST_F(EnqueueHandlerTestBasic, givenEnqueueHandlerWhenCommandIsBlokingThenCompletionStampTaskCountIsPassedToWaitForTaskCountAndCleanAllocationListAsRequiredTaskCount) {
    auto mockCmdQ = setupFixtureAndCreateMockCommandQueue<FamilyType>();
    MockKernelWithInternals kernelInternals(*device, context.get());
    Kernel *kernel = kernelInternals.mockKernel;
    MockMultiDispatchInfo multiDispatchInfo(device.get(), kernel);
    mockCmdQ->template enqueueHandler<CL_COMMAND_WRITE_BUFFER>(nullptr,
                                                               0,
                                                               true,
                                                               multiDispatchInfo,
                                                               0,
                                                               nullptr,
                                                               nullptr);
    EXPECT_EQ(initialTaskCount + 1, mockInternalAllocationStorage->lastCleanAllocationsTaskCount);
}

HWTEST_F(EnqueueHandlerTestBasic, givenBlockedEnqueueHandlerWhenCommandIsBlokingThenCompletionStampTaskCountIsPassedToWaitForTaskCountAndCleanAllocationListAsRequiredTaskCount) {
    auto mockCmdQ = setupFixtureAndCreateMockCommandQueue<FamilyType>();

    MockKernelWithInternals kernelInternals(*device, context.get());
    Kernel *kernel = kernelInternals.mockKernel;
    MockMultiDispatchInfo multiDispatchInfo(device.get(), kernel);

    UserEvent userEvent;
    cl_event waitlist[] = {&userEvent};

    std::thread t0([&mockCmdQ, &userEvent]() {
        while (!mockCmdQ->isQueueBlocked()) {
        }
        userEvent.setStatus(CL_COMPLETE);
    });
    mockCmdQ->template enqueueHandler<CL_COMMAND_WRITE_BUFFER>(nullptr,
                                                               0,
                                                               true,
                                                               multiDispatchInfo,
                                                               1,
                                                               waitlist,
                                                               nullptr);
    EXPECT_EQ(initialTaskCount + 1, mockInternalAllocationStorage->lastCleanAllocationsTaskCount);

    t0.join();
}
