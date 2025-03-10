/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/command_stream_receiver_hw.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/flush_stamp.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/memory_manager/allocations_list.h"
#include "shared/source/memory_manager/os_agnostic_memory_manager.h"
#include "shared/source/memory_manager/unified_memory_manager.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/mocks/mock_graphics_allocation.h"
#include "shared/test/unit_test/page_fault_manager/mock_cpu_page_fault_manager.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/source/built_ins/builtins_dispatch_builder.h"
#include "opencl/source/helpers/cl_hw_helper.h"
#include "opencl/source/helpers/memory_properties_helpers.h"
#include "opencl/source/helpers/surface_formats.h"
#include "opencl/source/kernel/kernel.h"
#include "opencl/source/mem_obj/image.h"
#include "opencl/test/unit_test/fixtures/cl_device_fixture.h"
#include "opencl/test/unit_test/fixtures/device_host_queue_fixture.h"
#include "opencl/test/unit_test/fixtures/execution_model_fixture.h"
#include "opencl/test/unit_test/fixtures/memory_management_fixture.h"
#include "opencl/test/unit_test/fixtures/multi_root_device_fixture.h"
#include "opencl/test/unit_test/helpers/gtest_helpers.h"
#include "opencl/test/unit_test/libult/ult_command_stream_receiver.h"
#include "opencl/test/unit_test/mocks/mock_allocation_properties.h"
#include "opencl/test/unit_test/mocks/mock_command_queue.h"
#include "opencl/test/unit_test/mocks/mock_context.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_memory_manager.h"
#include "opencl/test/unit_test/mocks/mock_program.h"
#include "opencl/test/unit_test/mocks/mock_timestamp_container.h"
#include "opencl/test/unit_test/program/program_from_binary.h"
#include "opencl/test/unit_test/program/program_tests.h"
#include "opencl/test/unit_test/test_macros/test_checks_ocl.h"
#include "test.h"

#include <memory>

using namespace NEO;
using namespace DeviceHostQueue;

using KernelTest = ::testing::Test;

class KernelTests : public ProgramFromBinaryFixture {
  public:
    ~KernelTests() override = default;

  protected:
    void SetUp() override {
        ProgramFromBinaryFixture::SetUp("CopyBuffer_simd32", "CopyBuffer");
        ASSERT_NE(nullptr, pProgram);
        ASSERT_EQ(CL_SUCCESS, retVal);

        retVal = pProgram->build(
            pProgram->getDevices(),
            nullptr,
            false);
        ASSERT_EQ(CL_SUCCESS, retVal);

        // create a kernel
        pKernel = Kernel::create<MockKernel>(
            pProgram,
            pProgram->getKernelInfosForKernel(kernelName),
            *pClDevice,
            &retVal);

        ASSERT_EQ(CL_SUCCESS, retVal);
        ASSERT_NE(nullptr, pKernel);
    }

    void TearDown() override {
        delete pKernel;
        pKernel = nullptr;
        knownSource.reset();
        ProgramFromBinaryFixture::TearDown();
    }

    MockKernel *pKernel = nullptr;
    cl_int retVal = CL_SUCCESS;
};

TEST(KernelTest, WhenKernelIsCreatedThenCorrectMembersAreMemObjects) {
    EXPECT_TRUE(Kernel::isMemObj(Kernel::BUFFER_OBJ));
    EXPECT_TRUE(Kernel::isMemObj(Kernel::IMAGE_OBJ));
    EXPECT_TRUE(Kernel::isMemObj(Kernel::PIPE_OBJ));

    EXPECT_FALSE(Kernel::isMemObj(Kernel::SAMPLER_OBJ));
    EXPECT_FALSE(Kernel::isMemObj(Kernel::ACCELERATOR_OBJ));
    EXPECT_FALSE(Kernel::isMemObj(Kernel::NONE_OBJ));
    EXPECT_FALSE(Kernel::isMemObj(Kernel::SVM_ALLOC_OBJ));
}

TEST_F(KernelTests, WhenKernelIsCreatedThenKernelHeapIsCorrect) {
    EXPECT_EQ(pKernel->getKernelInfo(rootDeviceIndex).heapInfo.pKernelHeap, pKernel->getKernelHeap(rootDeviceIndex));
    EXPECT_EQ(pKernel->getKernelInfo(rootDeviceIndex).heapInfo.KernelHeapSize, pKernel->getKernelHeapSize(rootDeviceIndex));
}

TEST_F(KernelTests, GivenInvalidParamNameWhenGettingInfoThenInvalidValueErrorIsReturned) {
    size_t paramValueSizeRet = 0;

    // get size
    retVal = pKernel->getInfo(
        0,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_INVALID_VALUE, retVal);
}

TEST_F(KernelTests, GivenInvalidParametersWhenGettingInfoThenValueSizeRetIsNotUpdated) {
    size_t paramValueSizeRet = 0x1234;

    // get size
    retVal = pKernel->getInfo(
        0,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(0x1234u, paramValueSizeRet);
}

TEST_F(KernelTests, GivenKernelFunctionNameWhenGettingInfoThenKernelFunctionNameIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_FUNCTION_NAME;
    size_t paramValueSize = 0;
    char *paramValue = nullptr;
    size_t paramValueSizeRet = 0;

    // get size
    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        nullptr,
        &paramValueSizeRet);
    EXPECT_NE(0u, paramValueSizeRet);
    ASSERT_EQ(CL_SUCCESS, retVal);

    // allocate space for name
    paramValue = new char[paramValueSizeRet];

    // get the name
    paramValueSize = paramValueSizeRet;

    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        paramValue,
        nullptr);

    EXPECT_NE(nullptr, paramValue);
    EXPECT_EQ(0, strcmp(paramValue, kernelName));
    EXPECT_EQ(CL_SUCCESS, retVal);

    delete[] paramValue;
}

TEST_F(KernelTests, GivenKernelBinaryProgramIntelWhenGettingInfoThenKernelBinaryIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_BINARY_PROGRAM_INTEL;
    size_t paramValueSize = 0;
    char *paramValue = nullptr;
    size_t paramValueSizeRet = 0;
    const char *pKernelData = reinterpret_cast<const char *>(pKernel->getKernelHeap(rootDeviceIndex));
    EXPECT_NE(nullptr, pKernelData);

    // get size of kernel binary
    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        nullptr,
        &paramValueSizeRet);
    EXPECT_NE(0u, paramValueSizeRet);
    ASSERT_EQ(CL_SUCCESS, retVal);

    // allocate space for kernel binary
    paramValue = new char[paramValueSizeRet];

    // get kernel binary
    paramValueSize = paramValueSizeRet;
    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        paramValue,
        nullptr);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_NE(nullptr, paramValue);
    EXPECT_EQ(0, memcmp(paramValue, pKernelData, paramValueSize));

    delete[] paramValue;
}

TEST_F(KernelTests, givenBinaryWhenItIsQueriedForGpuAddressThenAbsoluteAddressIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_BINARY_GPU_ADDRESS_INTEL;
    uint64_t paramValue = 0llu;
    size_t paramValueSize = sizeof(paramValue);
    size_t paramValueSizeRet = 0;

    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    auto expectedGpuAddress = GmmHelper::decanonize(pKernel->getKernelInfo(rootDeviceIndex).kernelAllocation->getGpuAddress());
    EXPECT_EQ(expectedGpuAddress, paramValue);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
}

TEST_F(KernelTests, GivenKernelNumArgsWhenGettingInfoThenNumberOfKernelArgsIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_NUM_ARGS;
    size_t paramValueSize = sizeof(cl_uint);
    cl_uint paramValue = 0;
    size_t paramValueSizeRet = 0;

    // get size
    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(sizeof(cl_uint), paramValueSizeRet);
    EXPECT_EQ(2u, paramValue);
    EXPECT_EQ(CL_SUCCESS, retVal);
}

TEST_F(KernelTests, GivenKernelProgramWhenGettingInfoThenProgramIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_PROGRAM;
    size_t paramValueSize = sizeof(cl_program);
    cl_program paramValue = 0;
    size_t paramValueSizeRet = 0;
    cl_program prog = pProgram;

    // get size
    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(sizeof(cl_program), paramValueSizeRet);
    EXPECT_EQ(prog, paramValue);
}

TEST_F(KernelTests, GivenKernelContextWhenGettingInfoThenKernelContextIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_CONTEXT;
    cl_context paramValue = 0;
    size_t paramValueSize = sizeof(paramValue);
    size_t paramValueSizeRet = 0;
    cl_context context = pContext;

    // get size
    retVal = pKernel->getInfo(
        paramName,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
    EXPECT_EQ(context, paramValue);
}

TEST_F(KernelTests, GivenKernelWorkGroupSizeWhenGettingWorkGroupInfoThenWorkGroupSizeIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_WORK_GROUP_SIZE;
    size_t paramValue = 0;
    size_t paramValueSize = sizeof(paramValue);
    size_t paramValueSizeRet = 0;

    auto kernelMaxWorkGroupSize = pDevice->getDeviceInfo().maxWorkGroupSize - 1;
    pKernel->kernelDeviceInfos[rootDeviceIndex].maxKernelWorkGroupSize = static_cast<uint32_t>(kernelMaxWorkGroupSize);

    retVal = pKernel->getWorkGroupInfo(
        *pClDevice,
        paramName,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
    EXPECT_EQ(kernelMaxWorkGroupSize, paramValue);
}

TEST_F(KernelTests, GivenKernelCompileWorkGroupSizeWhenGettingWorkGroupInfoThenCompileWorkGroupSizeIsReturned) {
    cl_kernel_info paramName = CL_KERNEL_COMPILE_WORK_GROUP_SIZE;
    size_t paramValue[3];
    size_t paramValueSize = sizeof(paramValue);
    size_t paramValueSizeRet = 0;

    retVal = pKernel->getWorkGroupInfo(
        *pClDevice,
        paramName,
        paramValueSize,
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(paramValueSize, paramValueSizeRet);
}

TEST_F(KernelTests, GivenInvalidParamNameWhenGettingWorkGroupInfoThenInvalidValueErrorIsReturned) {
    size_t paramValueSizeRet = 0x1234u;

    retVal = pKernel->getWorkGroupInfo(
        *pClDevice,
        0,
        0,
        nullptr,
        &paramValueSizeRet);

    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(0x1234u, paramValueSizeRet);
}

class KernelFromBinaryTest : public ProgramSimpleFixture {
  public:
    void SetUp() override {
        ProgramSimpleFixture::SetUp();
    }
    void TearDown() override {
        ProgramSimpleFixture::TearDown();
    }
};
typedef Test<KernelFromBinaryTest> KernelFromBinaryTests;

TEST_F(KernelFromBinaryTests, GivenKernelNumArgsWhenGettingInfoThenNumberOfKernelArgsIsReturned) {
    CreateProgramFromBinary(pContext, pContext->getDevices(), "kernel_num_args");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        pProgram->getDevices(),
        nullptr,
        false);

    ASSERT_EQ(CL_SUCCESS, retVal);

    auto kernelInfos = pProgram->getKernelInfosForKernel("test");

    // create a kernel
    auto pKernel = Kernel::create(
        pProgram,
        kernelInfos,
        *pClDevice,
        &retVal);

    ASSERT_EQ(CL_SUCCESS, retVal);

    cl_uint paramValue = 0;
    size_t paramValueSizeRet = 0;

    // get size
    retVal = pKernel->getInfo(
        CL_KERNEL_NUM_ARGS,
        sizeof(cl_uint),
        &paramValue,
        &paramValueSizeRet);

    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(sizeof(cl_uint), paramValueSizeRet);
    EXPECT_EQ(3u, paramValue);

    delete pKernel;
}

TEST_F(KernelFromBinaryTests, WhenRegularKernelIsCreatedThenItIsNotBuiltIn) {
    CreateProgramFromBinary(pContext, pContext->getDevices(), "simple_kernels");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        pProgram->getDevices(),
        nullptr,
        false);

    ASSERT_EQ(CL_SUCCESS, retVal);

    auto kernelInfos = pProgram->getKernelInfosForKernel("simple_kernel_0");

    // create a kernel
    auto pKernel = Kernel::create(
        pProgram,
        kernelInfos,
        *pClDevice,
        &retVal);

    ASSERT_EQ(CL_SUCCESS, retVal);
    ASSERT_NE(nullptr, pKernel);

    // get builtIn property
    bool isBuiltIn = pKernel->isBuiltIn;

    EXPECT_FALSE(isBuiltIn);

    delete pKernel;
}

TEST_F(KernelFromBinaryTests, givenArgumentDeclaredAsConstantWhenKernelIsCreatedThenArgumentIsMarkedAsReadOnly) {
    CreateProgramFromBinary(pContext, pContext->getDevices(), "simple_kernels");

    ASSERT_NE(nullptr, pProgram);
    retVal = pProgram->build(
        pProgram->getDevices(),
        nullptr,
        false);

    ASSERT_EQ(CL_SUCCESS, retVal);

    auto pKernelInfo = pProgram->getKernelInfo("simple_kernel_6", rootDeviceIndex);
    EXPECT_TRUE(pKernelInfo->kernelArgInfo[1].isReadOnly);
    pKernelInfo = pProgram->getKernelInfo("simple_kernel_1", rootDeviceIndex);
    EXPECT_TRUE(pKernelInfo->kernelArgInfo[0].isReadOnly);
}

typedef Test<ClDeviceFixture> KernelPrivateSurfaceTest;
typedef Test<ClDeviceFixture> KernelGlobalSurfaceTest;
typedef Test<ClDeviceFixture> KernelConstantSurfaceTest;

struct KernelWithDeviceQueueFixture : public ClDeviceFixture,
                                      public DeviceQueueFixture,
                                      public testing::Test {
    void SetUp() override {
        ClDeviceFixture::SetUp();
        DeviceQueueFixture::SetUp(&context, pClDevice);
    }
    void TearDown() override {
        DeviceQueueFixture::TearDown();
        ClDeviceFixture::TearDown();
    }

    MockContext context;
};

typedef KernelWithDeviceQueueFixture KernelDefaultDeviceQueueSurfaceTest;
typedef KernelWithDeviceQueueFixture KernelEventPoolSurfaceTest;

class CommandStreamReceiverMock : public CommandStreamReceiver {
    typedef CommandStreamReceiver BaseClass;

  public:
    using CommandStreamReceiver::executionEnvironment;

    using BaseClass::CommandStreamReceiver;

    void flushTagUpdate() override{};
    void updateTagFromWait() override{};

    bool isMultiOsContextCapable() const override { return false; }

    MemoryCompressionState getMemoryCompressionState(bool auxTranslationRequired) const override {
        return MemoryCompressionState::NotApplicable;
    }

    CommandStreamReceiverMock() : BaseClass(*(new ExecutionEnvironment), 0, 1) {
        this->mockExecutionEnvironment.reset(&this->executionEnvironment);
        executionEnvironment.prepareRootDeviceEnvironments(1);
        executionEnvironment.rootDeviceEnvironments[0]->setHwInfo(defaultHwInfo.get());
        executionEnvironment.initializeMemoryManager();
    }

    void makeResident(GraphicsAllocation &graphicsAllocation) override {
        residency[graphicsAllocation.getUnderlyingBuffer()] = graphicsAllocation.getUnderlyingBufferSize();
        if (passResidencyCallToBaseClass) {
            CommandStreamReceiver::makeResident(graphicsAllocation);
        }
    }

    void makeNonResident(GraphicsAllocation &graphicsAllocation) override {
        residency.erase(graphicsAllocation.getUnderlyingBuffer());
        if (passResidencyCallToBaseClass) {
            CommandStreamReceiver::makeNonResident(graphicsAllocation);
        }
    }

    bool flush(BatchBuffer &batchBuffer, ResidencyContainer &allocationsForResidency) override {
        return true;
    }

    void waitForTaskCountWithKmdNotifyFallback(uint32_t taskCountToWait, FlushStamp flushStampToWait, bool quickKmdSleep, bool forcePowerSavingMode) override {
    }
    uint32_t blitBuffer(const BlitPropertiesContainer &blitPropertiesContainer, bool blocking, bool profilingEnabled) override { return taskCount; };

    CompletionStamp flushTask(
        LinearStream &commandStream,
        size_t commandStreamStart,
        const IndirectHeap &dsh,
        const IndirectHeap &ioh,
        const IndirectHeap &ssh,
        uint32_t taskLevel,
        DispatchFlags &dispatchFlags,
        Device &device) override {
        CompletionStamp cs = {};
        return cs;
    }

    bool flushBatchedSubmissions() override { return true; }

    CommandStreamReceiverType getType() override {
        return CommandStreamReceiverType::CSR_HW;
    }

    void programHardwareContext(LinearStream &cmdStream) override {}
    size_t getCmdsSizeForHardwareContext() const override {
        return 0;
    }
    GraphicsAllocation *getClearColorAllocation() override { return nullptr; }

    std::map<const void *, size_t> residency;
    bool passResidencyCallToBaseClass = true;
    std::unique_ptr<ExecutionEnvironment> mockExecutionEnvironment;
};

TEST_F(KernelPrivateSurfaceTest, WhenChangingResidencyThenCsrResidencySizeIsUpdated) {
    ASSERT_NE(nullptr, pDevice);

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    // setup private memory
    SPatchAllocateStatelessPrivateSurface tokenSPS;
    tokenSPS.SurfaceStateHeapOffset = 64;
    tokenSPS.DataParamOffset = 40;
    tokenSPS.DataParamSize = 8;
    tokenSPS.PerThreadPrivateMemorySize = 112;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenSPS);

    SPatchDataParameterStream tokenDPS;
    tokenDPS.DataParameterStreamSize = 64;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenDPS);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // create kernel
    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    // Test it
    auto executionEnvironment = pDevice->getExecutionEnvironment();
    std::unique_ptr<CommandStreamReceiverMock> csr(new CommandStreamReceiverMock(*executionEnvironment, 0, 1));
    csr->setupContext(*pDevice->getDefaultEngine().osContext);
    csr->residency.clear();
    EXPECT_EQ(0u, csr->residency.size());

    pKernel->makeResident(*csr.get());
    EXPECT_EQ(1u, csr->residency.size());

    csr->makeSurfacePackNonResident(csr->getResidencyAllocations());
    EXPECT_EQ(0u, csr->residency.size());

    delete pKernel;
}

TEST_F(KernelPrivateSurfaceTest, givenKernelWithPrivateSurfaceThatIsInUseByGpuWhenKernelIsBeingDestroyedThenAllocationIsAddedToDeferredFreeList) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    SPatchAllocateStatelessPrivateSurface tokenSPS;
    tokenSPS.SurfaceStateHeapOffset = 64;
    tokenSPS.DataParamOffset = 40;
    tokenSPS.DataParamSize = 8;
    tokenSPS.PerThreadPrivateMemorySize = 112;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenSPS);

    SPatchDataParameterStream tokenDPS;
    tokenDPS.DataParameterStreamSize = 64;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenDPS);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    pKernel->initialize();

    auto &csr = pDevice->getGpgpuCommandStreamReceiver();

    auto privateSurface = pKernel->kernelDeviceInfos[pDevice->getRootDeviceIndex()].privateSurface;
    auto tagAddress = csr.getTagAddress();

    privateSurface->updateTaskCount(*tagAddress + 1, csr.getOsContext().getContextId());

    EXPECT_TRUE(csr.getTemporaryAllocations().peekIsEmpty());
    pKernel.reset(nullptr);

    EXPECT_FALSE(csr.getTemporaryAllocations().peekIsEmpty());
    EXPECT_EQ(csr.getTemporaryAllocations().peekHead(), privateSurface);
}

TEST_F(KernelPrivateSurfaceTest, WhenPrivateSurfaceAllocationFailsThenOutOfResourcesErrorIsReturned) {
    ASSERT_NE(nullptr, pDevice);

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    // setup private memory
    SPatchAllocateStatelessPrivateSurface tokenSPS;
    tokenSPS.SurfaceStateHeapOffset = 64;
    tokenSPS.DataParamOffset = 40;
    tokenSPS.DataParamSize = 8;
    tokenSPS.PerThreadPrivateMemorySize = 112;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenSPS);

    SPatchDataParameterStream tokenDPS;
    tokenDPS.DataParameterStreamSize = 64;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenDPS);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // create kernel
    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    MemoryManagementFixture::InjectedFunction method = [&](size_t failureIndex) {
        MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

        if (MemoryManagement::nonfailingAllocation == failureIndex) {
            EXPECT_EQ(CL_SUCCESS, pKernel->initialize());
        } else {
            EXPECT_EQ(CL_OUT_OF_RESOURCES, pKernel->initialize());
        }
        delete pKernel;
    };
    auto f = new MemoryManagementFixture();
    f->SetUp();
    f->injectFailures(method);
    f->TearDown();
    delete f;
}

TEST_F(KernelPrivateSurfaceTest, given32BitDeviceWhenKernelIsCreatedThenPrivateSurfaceIs32BitAllocation) {
    if (is64bit) {
        pDevice->getMemoryManager()->setForce32BitAllocations(true);

        // define kernel info
        auto pKernelInfo = std::make_unique<KernelInfo>();

        // setup private memory
        SPatchAllocateStatelessPrivateSurface tokenSPS;
        tokenSPS.SurfaceStateHeapOffset = 64;
        tokenSPS.DataParamOffset = 40;
        tokenSPS.DataParamSize = 4;
        tokenSPS.PerThreadPrivateMemorySize = 112;
        populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenSPS);

        SPatchDataParameterStream tokenDPS;
        tokenDPS.DataParameterStreamSize = 64;
        populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenDPS);

        pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

        // create kernel
        MockContext context;
        MockProgram program(&context, false, toClDeviceVector(*pClDevice));
        MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

        ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

        EXPECT_TRUE(pKernel->kernelDeviceInfos[pDevice->getRootDeviceIndex()].privateSurface->is32BitAllocation());

        delete pKernel;
    }
}

HWTEST_F(KernelPrivateSurfaceTest, givenStatefulKernelWhenKernelIsCreatedThenPrivateMemorySurfaceStateIsPatchedWithCpuAddress) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup constant memory
    SPatchAllocateStatelessPrivateSurface allocateStatelessPrivateMemorySurface;
    allocateStatelessPrivateMemorySurface.SurfaceStateHeapOffset = 0;
    allocateStatelessPrivateMemorySurface.DataParamOffset = 0;
    allocateStatelessPrivateMemorySurface.DataParamSize = 8;
    allocateStatelessPrivateMemorySurface.PerThreadPrivateMemorySize = 16;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessPrivateMemorySurface);

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));

    // create kernel
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_NE(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    auto bufferAddress = pKernel->kernelDeviceInfos[pDevice->getRootDeviceIndex()].privateSurface->getGpuAddress();

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.privateMemoryAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(bufferAddress, surfaceAddress);

    delete pKernel;
}

TEST_F(KernelPrivateSurfaceTest, givenStatelessKernelWhenKernelIsCreatedThenPrivateMemorySurfaceStateIsNotPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup global memory
    char buffer[16];
    MockGraphicsAllocation gfxAlloc(buffer, sizeof(buffer));

    MockContext context(pClDevice);
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    program.setConstantSurface(&gfxAlloc);

    // create kernel
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateful path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));
    EXPECT_EQ(nullptr, pKernel->getSurfaceStateHeap(rootDeviceIndex));

    program.setConstantSurface(nullptr);
    delete pKernel;
}

TEST_F(KernelPrivateSurfaceTest, givenNullDataParameterStreamWhenGettingConstantBufferSizeThenZeroIsReturned) {
    auto pKernelInfo = std::make_unique<KernelInfo>();

    EXPECT_EQ(0u, pKernelInfo->getConstantBufferSize());
}

TEST_F(KernelPrivateSurfaceTest, givenNonNullDataParameterStreamWhenGettingConstantBufferSizeThenCorrectSizeIsReturned) {
    auto pKernelInfo = std::make_unique<KernelInfo>();

    SPatchDataParameterStream tokenDPS;
    tokenDPS.DataParameterStreamSize = 64;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenDPS);

    EXPECT_EQ(64u, pKernelInfo->getConstantBufferSize());
}

TEST_F(KernelPrivateSurfaceTest, GivenKernelWhenPrivateSurfaceTooBigAndGpuPointerSize4ThenReturnOutOfResources) {
    auto pKernelInfo = std::make_unique<KernelInfo>();

    SPatchAllocateStatelessPrivateSurface allocateStatelessPrivateSurface = {};
    allocateStatelessPrivateSurface.PerThreadPrivateMemorySize = std::numeric_limits<uint32_t>::max();
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessPrivateSurface);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    pKernelInfo->gpuPointerSize = 4;
    pDevice->getMemoryManager()->setForce32BitAllocations(false);
    if (pDevice->getDeviceInfo().computeUnitsUsedForScratch == 0)
        pDevice->deviceInfo.computeUnitsUsedForScratch = 120;
    EXPECT_EQ(CL_OUT_OF_RESOURCES, pKernel->initialize());
}

TEST_F(KernelPrivateSurfaceTest, GivenKernelWhenPrivateSurfaceTooBigAndGpuPointerSize4And32BitAllocationsThenReturnOutOfResources) {
    auto pKernelInfo = std::make_unique<KernelInfo>();

    SPatchAllocateStatelessPrivateSurface allocateStatelessPrivateSurface = {};
    allocateStatelessPrivateSurface.PerThreadPrivateMemorySize = std::numeric_limits<uint32_t>::max();
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessPrivateSurface);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    pKernelInfo->gpuPointerSize = 4;
    pDevice->getMemoryManager()->setForce32BitAllocations(true);
    if (pDevice->getDeviceInfo().computeUnitsUsedForScratch == 0)
        pDevice->deviceInfo.computeUnitsUsedForScratch = 120;
    EXPECT_EQ(CL_OUT_OF_RESOURCES, pKernel->initialize());
}

TEST_F(KernelPrivateSurfaceTest, GivenKernelWhenPrivateSurfaceTooBigAndGpuPointerSize8And32BitAllocationsThenReturnOutOfResources) {
    auto pKernelInfo = std::make_unique<KernelInfo>();

    SPatchAllocateStatelessPrivateSurface allocateStatelessPrivateSurface = {};
    allocateStatelessPrivateSurface.PerThreadPrivateMemorySize = std::numeric_limits<uint32_t>::max();
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessPrivateSurface);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    pKernelInfo->gpuPointerSize = 8;
    pDevice->getMemoryManager()->setForce32BitAllocations(true);
    if (pDevice->getDeviceInfo().computeUnitsUsedForScratch == 0)
        pDevice->deviceInfo.computeUnitsUsedForScratch = 120;
    EXPECT_EQ(CL_OUT_OF_RESOURCES, pKernel->initialize());
}

TEST_F(KernelGlobalSurfaceTest, givenBuiltInKernelWhenKernelIsCreatedThenGlobalSurfaceIsPatchedWithCpuAddress) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    // setup global memory
    SPatchAllocateStatelessGlobalMemorySurfaceWithInitialization allocateStatelessGlobalMemorySurfaceWithInitialization;
    allocateStatelessGlobalMemorySurfaceWithInitialization.DataParamOffset = 0;
    allocateStatelessGlobalMemorySurfaceWithInitialization.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessGlobalMemorySurfaceWithInitialization);

    SPatchDataParameterStream tempSPatchDataParameterStream;
    tempSPatchDataParameterStream.DataParameterStreamSize = 16;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tempSPatchDataParameterStream);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    char buffer[16];

    GraphicsAllocation gfxAlloc(0, GraphicsAllocation::AllocationType::UNKNOWN, buffer, (uint64_t)buffer - 8u, 8, (osHandle)1u, MemoryPool::MemoryNull);
    uint64_t bufferAddress = (uint64_t)gfxAlloc.getUnderlyingBuffer();

    // create kernel
    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    program.setGlobalSurface(&gfxAlloc);
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    pKernel->isBuiltIn = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(bufferAddress, *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    program.setGlobalSurface(nullptr);
    delete pKernel;
}

TEST_F(KernelGlobalSurfaceTest, givenNDRangeKernelWhenKernelIsCreatedThenGlobalSurfaceIsPatchedWithBaseAddressOffset) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    // setup global memory
    SPatchAllocateStatelessGlobalMemorySurfaceWithInitialization allocateStatelessGlobalMemorySurfaceWithInitialization;
    allocateStatelessGlobalMemorySurfaceWithInitialization.DataParamOffset = 0;
    allocateStatelessGlobalMemorySurfaceWithInitialization.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessGlobalMemorySurfaceWithInitialization);

    SPatchDataParameterStream tempSPatchDataParameterStream;
    tempSPatchDataParameterStream.DataParameterStreamSize = 16;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tempSPatchDataParameterStream);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    char buffer[16];

    GraphicsAllocation gfxAlloc(0, GraphicsAllocation::AllocationType::UNKNOWN, buffer, (uint64_t)buffer - 8u, 8, MemoryPool::MemoryNull, 0u);
    uint64_t bufferAddress = gfxAlloc.getGpuAddress();

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    program.setGlobalSurface(&gfxAlloc);
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(bufferAddress, *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    program.setGlobalSurface(nullptr);

    delete pKernel;
}

HWTEST_F(KernelGlobalSurfaceTest, givenStatefulKernelWhenKernelIsCreatedThenGlobalMemorySurfaceStateIsPatchedWithCpuAddress) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup global memory
    SPatchAllocateStatelessGlobalMemorySurfaceWithInitialization allocateStatelessGlobalMemorySurfaceWithInitialization;
    allocateStatelessGlobalMemorySurfaceWithInitialization.SurfaceStateHeapOffset = 0;
    allocateStatelessGlobalMemorySurfaceWithInitialization.DataParamOffset = 0;
    allocateStatelessGlobalMemorySurfaceWithInitialization.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessGlobalMemorySurfaceWithInitialization);

    char buffer[16];
    MockGraphicsAllocation gfxAlloc(buffer, sizeof(buffer));
    auto bufferAddress = gfxAlloc.getGpuAddress();

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    program.setGlobalSurface(&gfxAlloc);

    // create kernel
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_NE(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.globalVariablesSurfaceAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(bufferAddress, surfaceAddress);

    program.setGlobalSurface(nullptr);
    delete pKernel;
}

TEST_F(KernelGlobalSurfaceTest, givenStatelessKernelWhenKernelIsCreatedThenGlobalMemorySurfaceStateIsNotPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup global memory
    char buffer[16];
    MockGraphicsAllocation gfxAlloc(buffer, sizeof(buffer));

    MockProgram program(toClDeviceVector(*pClDevice));
    program.setGlobalSurface(&gfxAlloc);

    // create kernel
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateful path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));
    EXPECT_EQ(nullptr, pKernel->getSurfaceStateHeap(rootDeviceIndex));

    program.setGlobalSurface(nullptr);
    delete pKernel;
}

TEST_F(KernelConstantSurfaceTest, givenBuiltInKernelWhenKernelIsCreatedThenConstantSurfaceIsPatchedWithCpuAddress) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    // setup constant memory
    SPatchAllocateStatelessConstantMemorySurfaceWithInitialization allocateStatelessConstantMemorySurfaceWithInitialization;
    allocateStatelessConstantMemorySurfaceWithInitialization.DataParamOffset = 0;
    allocateStatelessConstantMemorySurfaceWithInitialization.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessConstantMemorySurfaceWithInitialization);

    SPatchDataParameterStream tempSPatchDataParameterStream;
    tempSPatchDataParameterStream.DataParameterStreamSize = 16;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tempSPatchDataParameterStream);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    char buffer[16];

    GraphicsAllocation gfxAlloc(0, GraphicsAllocation::AllocationType::UNKNOWN, buffer, (uint64_t)buffer - 8u, 8, (osHandle)1u, MemoryPool::MemoryNull);
    uint64_t bufferAddress = (uint64_t)gfxAlloc.getUnderlyingBuffer();

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    program.setConstantSurface(&gfxAlloc);
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    pKernel->isBuiltIn = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(bufferAddress, *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    program.setConstantSurface(nullptr);
    delete pKernel;
}

TEST_F(KernelConstantSurfaceTest, givenNDRangeKernelWhenKernelIsCreatedThenConstantSurfaceIsPatchedWithBaseAddressOffset) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();

    // setup constant memory
    SPatchAllocateStatelessConstantMemorySurfaceWithInitialization allocateStatelessConstantMemorySurfaceWithInitialization;
    allocateStatelessConstantMemorySurfaceWithInitialization.DataParamOffset = 0;
    allocateStatelessConstantMemorySurfaceWithInitialization.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessConstantMemorySurfaceWithInitialization);

    SPatchDataParameterStream tempSPatchDataParameterStream;
    tempSPatchDataParameterStream.DataParameterStreamSize = 16;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tempSPatchDataParameterStream);

    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    char buffer[16];

    GraphicsAllocation gfxAlloc(0, GraphicsAllocation::AllocationType::UNKNOWN, buffer, (uint64_t)buffer - 8u, 8, MemoryPool::MemoryNull, 0u);
    uint64_t bufferAddress = gfxAlloc.getGpuAddress();

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    program.setConstantSurface(&gfxAlloc);
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(bufferAddress, *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    program.setConstantSurface(nullptr);

    delete pKernel;
}

HWTEST_F(KernelConstantSurfaceTest, givenStatefulKernelWhenKernelIsCreatedThenConstantMemorySurfaceStateIsPatchedWithCpuAddress) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup constant memory
    SPatchAllocateStatelessConstantMemorySurfaceWithInitialization allocateStatelessConstantMemorySurfaceWithInitialization;
    allocateStatelessConstantMemorySurfaceWithInitialization.SurfaceStateHeapOffset = 0;
    allocateStatelessConstantMemorySurfaceWithInitialization.DataParamOffset = 0;
    allocateStatelessConstantMemorySurfaceWithInitialization.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessConstantMemorySurfaceWithInitialization);

    char buffer[16];
    MockGraphicsAllocation gfxAlloc(buffer, sizeof(buffer));
    auto bufferAddress = gfxAlloc.getGpuAddress();

    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    program.setConstantSurface(&gfxAlloc);

    // create kernel
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_NE(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.globalConstantsSurfaceAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(bufferAddress, surfaceAddress);

    program.setConstantSurface(nullptr);
    delete pKernel;
}

TEST_F(KernelConstantSurfaceTest, givenStatelessKernelWhenKernelIsCreatedThenConstantMemorySurfaceStateIsNotPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup global memory
    char buffer[16];
    MockGraphicsAllocation gfxAlloc(buffer, sizeof(buffer));

    MockProgram program(toClDeviceVector(*pClDevice));
    program.setConstantSurface(&gfxAlloc);

    // create kernel
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateful path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));
    EXPECT_EQ(nullptr, pKernel->getSurfaceStateHeap(rootDeviceIndex));

    program.setConstantSurface(nullptr);
    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelEventPoolSurfaceTest, givenStatefulKernelWhenKernelIsCreatedThenEventPoolSurfaceStateIsPatchedWithNullSurface) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup event pool surface
    SPatchAllocateStatelessEventPoolSurface allocateStatelessEventPoolSurface;
    allocateStatelessEventPoolSurface.SurfaceStateHeapOffset = 0;
    allocateStatelessEventPoolSurface.DataParamOffset = 0;
    allocateStatelessEventPoolSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessEventPoolSurface);

    // create kernel
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_NE(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.deviceSideEnqueueEventPoolSurfaceAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(0u, surfaceAddress);
    auto surfaceType = surfaceState->getSurfaceType();
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_NULL, surfaceType);

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelEventPoolSurfaceTest, givenStatefulKernelWhenEventPoolIsPatchedThenEventPoolSurfaceStateIsProgrammed) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup event pool surface
    SPatchAllocateStatelessEventPoolSurface allocateStatelessEventPoolSurface;
    allocateStatelessEventPoolSurface.SurfaceStateHeapOffset = 0;
    allocateStatelessEventPoolSurface.DataParamOffset = 0;
    allocateStatelessEventPoolSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessEventPoolSurface);

    // create kernel
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    pKernel->patchEventPool(pDevQueue);

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.deviceSideEnqueueEventPoolSurfaceAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(pDevQueue->getEventPoolBuffer()->getGpuAddress(), surfaceAddress);
    auto surfaceType = surfaceState->getSurfaceType();
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_BUFFER, surfaceType);

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelEventPoolSurfaceTest, givenKernelWithNullEventPoolInKernelInfoWhenEventPoolIsPatchedThenAddressIsNotPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateful path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    uint64_t crossThreadData = 123;

    pKernel->setCrossThreadData(&crossThreadData, sizeof(uint64_t));

    pKernel->patchEventPool(pDevQueue);

    EXPECT_EQ(123u, *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelEventPoolSurfaceTest, givenStatelessKernelWhenKernelIsCreatedThenEventPoolSurfaceStateIsNotPatched) {
    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup event pool surface
    SPatchAllocateStatelessEventPoolSurface allocateStatelessEventPoolSurface;
    allocateStatelessEventPoolSurface.SurfaceStateHeapOffset = 0;
    allocateStatelessEventPoolSurface.DataParamOffset = 0;
    allocateStatelessEventPoolSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessEventPoolSurface);

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateful path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());
    if (pClDevice->areOcl21FeaturesSupported() == false) {
        EXPECT_EQ(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));
    } else {
    }

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelEventPoolSurfaceTest, givenStatelessKernelWhenEventPoolIsPatchedThenCrossThreadDataIsPatched) {
    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup event pool surface
    SPatchAllocateStatelessEventPoolSurface allocateStatelessEventPoolSurface;
    allocateStatelessEventPoolSurface.SurfaceStateHeapOffset = 0;
    allocateStatelessEventPoolSurface.DataParamOffset = 0;
    allocateStatelessEventPoolSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessEventPoolSurface);

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateful path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    uint64_t crossThreadData = 0;

    pKernel->setCrossThreadData(&crossThreadData, sizeof(uint64_t));

    pKernel->patchEventPool(pDevQueue);

    EXPECT_EQ(pDevQueue->getEventPoolBuffer()->getGpuAddressToPatch(), *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelDefaultDeviceQueueSurfaceTest, givenStatefulKernelWhenKernelIsCreatedThenDefaultDeviceQueueSurfaceStateIsPatchedWithNullSurface) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup default device queue surface
    SPatchAllocateStatelessDefaultDeviceQueueSurface allocateStatelessDefaultDeviceQueueSurface = {};
    allocateStatelessDefaultDeviceQueueSurface.SurfaceStateHeapOffset = 0;
    allocateStatelessDefaultDeviceQueueSurface.DataParamOffset = 0;
    allocateStatelessDefaultDeviceQueueSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessDefaultDeviceQueueSurface);

    // create kernel
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_NE(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.deviceSideEnqueueDefaultQueueSurfaceAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(0u, surfaceAddress);
    auto surfaceType = surfaceState->getSurfaceType();
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_NULL, surfaceType);

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelDefaultDeviceQueueSurfaceTest, givenStatefulKernelWhenDefaultDeviceQueueIsPatchedThenSurfaceStateIsCorrectlyProgrammed) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup default device queue surface
    SPatchAllocateStatelessDefaultDeviceQueueSurface allocateStatelessDefaultDeviceQueueSurface = {};
    allocateStatelessDefaultDeviceQueueSurface.SurfaceStateHeapOffset = 0;
    allocateStatelessDefaultDeviceQueueSurface.DataParamOffset = 0;
    allocateStatelessDefaultDeviceQueueSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessDefaultDeviceQueueSurface);

    // create kernel
    MockProgram program(&context, false, toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // setup surface state heap
    char surfaceStateHeap[0x80];
    pKernelInfo->heapInfo.pSsh = surfaceStateHeap;
    pKernelInfo->heapInfo.SurfaceStateHeapSize = sizeof(surfaceStateHeap);

    // define stateful path
    pKernelInfo->usesSsh = true;
    pKernelInfo->requiresSshForBuffers = true;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    pKernel->patchDefaultDeviceQueue(pDevQueue);

    EXPECT_NE(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    typedef typename FamilyType::RENDER_SURFACE_STATE RENDER_SURFACE_STATE;
    auto surfaceState = reinterpret_cast<const RENDER_SURFACE_STATE *>(
        ptrOffset(pKernel->getSurfaceStateHeap(rootDeviceIndex),
                  pKernelInfo->kernelDescriptor.payloadMappings.implicitArgs.deviceSideEnqueueDefaultQueueSurfaceAddress.bindful));
    auto surfaceAddress = surfaceState->getSurfaceBaseAddress();

    EXPECT_EQ(pDevQueue->getQueueBuffer()->getGpuAddress(), surfaceAddress);
    auto surfaceType = surfaceState->getSurfaceType();
    EXPECT_EQ(RENDER_SURFACE_STATE::SURFACE_TYPE_SURFTYPE_BUFFER, surfaceType);

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelDefaultDeviceQueueSurfaceTest, givenStatelessKernelWhenKernelIsCreatedThenDefaultDeviceQueueSurfaceStateIsNotPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

    // setup default device queue surface
    SPatchAllocateStatelessDefaultDeviceQueueSurface allocateStatelessDefaultDeviceQueueSurface = {};
    allocateStatelessDefaultDeviceQueueSurface.DataParamOffset = 0;
    allocateStatelessDefaultDeviceQueueSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessDefaultDeviceQueueSurface);

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateless path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(0u, pKernel->getSurfaceStateHeapSize(rootDeviceIndex));

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelDefaultDeviceQueueSurfaceTest, givenKernelWithNullDeviceQueueKernelInfoWhenDefaultDeviceQueueIsPatchedThenAddressIsNotPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;
    pKernelInfo->kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateless path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    uint64_t crossThreadData = 123;

    pKernel->setCrossThreadData(&crossThreadData, sizeof(uint64_t));

    pKernel->patchDefaultDeviceQueue(pDevQueue);

    EXPECT_EQ(123u, *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    delete pKernel;
}

HWCMDTEST_F(IGFX_GEN8_CORE, KernelDefaultDeviceQueueSurfaceTest, givenStatelessKernelWhenDefaultDeviceQueueIsPatchedThenCrossThreadDataIsPatched) {

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;
    pKernelInfo->kernelDescriptor.kernelAttributes.bufferAddressingMode = KernelDescriptor::Stateless;

    // setup default device queue surface
    SPatchAllocateStatelessDefaultDeviceQueueSurface allocateStatelessDefaultDeviceQueueSurface = {};
    allocateStatelessDefaultDeviceQueueSurface.DataParamOffset = 0;
    allocateStatelessDefaultDeviceQueueSurface.DataParamSize = 8;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocateStatelessDefaultDeviceQueueSurface);

    // create kernel
    MockProgram program(toClDeviceVector(*pClDevice));
    MockKernel *pKernel = new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    // define stateless path
    pKernelInfo->usesSsh = false;
    pKernelInfo->requiresSshForBuffers = false;

    uint64_t crossThreadData = 0;

    pKernel->setCrossThreadData(&crossThreadData, sizeof(uint64_t));

    pKernel->patchDefaultDeviceQueue(pDevQueue);

    EXPECT_EQ(pDevQueue->getQueueBuffer()->getGpuAddressToPatch(), *(uint64_t *)pKernel->getCrossThreadData(rootDeviceIndex));

    delete pKernel;
}

typedef Test<ClDeviceFixture> KernelResidencyTest;

HWTEST_F(KernelResidencyTest, givenKernelWhenMakeResidentIsCalledThenKernelIsaIsMadeResident) {
    ASSERT_NE(nullptr, pDevice);
    char pCrossThreadData[64];

    // define kernel info
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    // setup kernel arg offsets
    KernelArgPatchInfo kernelArgPatchInfo;

    pKernelInfo->kernelArgInfo.resize(3);
    pKernelInfo->kernelArgInfo[2].kernelArgPatchInfoVector.push_back(kernelArgPatchInfo);
    pKernelInfo->kernelArgInfo[1].kernelArgPatchInfoVector.push_back(kernelArgPatchInfo);
    pKernelInfo->kernelArgInfo[0].kernelArgPatchInfoVector.push_back(kernelArgPatchInfo);

    pKernelInfo->kernelArgInfo[2].kernelArgPatchInfoVector[0].crossthreadOffset = 0x10;
    pKernelInfo->kernelArgInfo[1].kernelArgPatchInfoVector[0].crossthreadOffset = 0x20;
    pKernelInfo->kernelArgInfo[0].kernelArgPatchInfoVector[0].crossthreadOffset = 0x30;

    MockProgram program(toClDeviceVector(*pClDevice));
    MockContext ctx;
    program.setContext(&ctx);
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());
    pKernel->setCrossThreadData(pCrossThreadData, sizeof(pCrossThreadData));

    EXPECT_EQ(0u, commandStreamReceiver.makeResidentAllocations.size());
    pKernel->makeResident(pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_EQ(1u, commandStreamReceiver.makeResidentAllocations.size());
    EXPECT_TRUE(commandStreamReceiver.isMadeResident(pKernel->getKernelInfo(rootDeviceIndex).getGraphicsAllocation()));

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenMakeResidentIsCalledThenExportedFunctionsIsaAllocationIsMadeResident) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    MockProgram program(toClDeviceVector(*pClDevice));
    auto exportedFunctionsSurface = std::make_unique<MockGraphicsAllocation>();
    program.buildInfos[pDevice->getRootDeviceIndex()].exportedFunctionsSurface = exportedFunctionsSurface.get();
    MockContext ctx;
    program.setContext(&ctx);
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(0u, commandStreamReceiver.makeResidentAllocations.size());
    pKernel->makeResident(pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_TRUE(commandStreamReceiver.isMadeResident(program.buildInfos[pDevice->getRootDeviceIndex()].exportedFunctionsSurface));

    // check getResidency as well
    std::vector<NEO::Surface *> residencySurfaces;
    pKernel->getResidency(residencySurfaces, rootDeviceIndex);
    std::unique_ptr<NEO::ExecutionEnvironment> mockCsrExecEnv;
    {
        CommandStreamReceiverMock csrMock;
        csrMock.passResidencyCallToBaseClass = false;
        for (const auto &s : residencySurfaces) {
            s->makeResident(csrMock);
            delete s;
        }
        EXPECT_EQ(1U, csrMock.residency.count(exportedFunctionsSurface->getUnderlyingBuffer()));
        mockCsrExecEnv = std::move(csrMock.mockExecutionEnvironment);
    }

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenMakeResidentIsCalledThenGlobalBufferIsMadeResident) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    MockProgram program(toClDeviceVector(*pClDevice));
    MockContext ctx;
    program.setContext(&ctx);
    program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface = new MockGraphicsAllocation();
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_EQ(0u, commandStreamReceiver.makeResidentAllocations.size());
    pKernel->makeResident(pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_TRUE(commandStreamReceiver.isMadeResident(program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface));

    std::vector<NEO::Surface *> residencySurfaces;
    pKernel->getResidency(residencySurfaces, rootDeviceIndex);
    std::unique_ptr<NEO::ExecutionEnvironment> mockCsrExecEnv;
    {
        CommandStreamReceiverMock csrMock;
        csrMock.passResidencyCallToBaseClass = false;
        for (const auto &s : residencySurfaces) {
            s->makeResident(csrMock);
            delete s;
        }
        EXPECT_EQ(1U, csrMock.residency.count(program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface->getUnderlyingBuffer()));
        mockCsrExecEnv = std::move(csrMock.mockExecutionEnvironment);
    }

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenItUsesIndirectUnifiedMemoryDeviceAllocationThenTheyAreMadeResident) {
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto properties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::DEVICE_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, properties);

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());

    EXPECT_EQ(0u, commandStreamReceiver.getResidencyAllocations().size());

    mockKernel.mockKernel->setUnifiedMemoryProperty(CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, true);

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());

    EXPECT_EQ(1u, commandStreamReceiver.getResidencyAllocations().size());

    EXPECT_EQ(commandStreamReceiver.getResidencyAllocations()[0]->getGpuAddress(), castToUint64(unifiedMemoryAllocation));

    mockKernel.mockKernel->setUnifiedMemoryProperty(CL_KERNEL_EXEC_INFO_SVM_PTRS, true);

    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelUsingIndirectHostMemoryWhenMakeResidentIsCalledThenOnlyHostAllocationsAreMadeResident) {
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto deviceProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::DEVICE_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto hostProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::HOST_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedDeviceMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, deviceProperties);
    auto unifiedHostMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, hostProperties);

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_EQ(0u, commandStreamReceiver.getResidencyAllocations().size());
    mockKernel.mockKernel->setUnifiedMemoryProperty(CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, true);

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_EQ(1u, commandStreamReceiver.getResidencyAllocations().size());
    EXPECT_EQ(commandStreamReceiver.getResidencyAllocations()[0]->getGpuAddress(), castToUint64(unifiedHostMemoryAllocation));

    svmAllocationsManager->freeSVMAlloc(unifiedDeviceMemoryAllocation);
    svmAllocationsManager->freeSVMAlloc(unifiedHostMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelUsingIndirectSharedMemoryWhenMakeResidentIsCalledThenOnlySharedAllocationsAreMadeResident) {
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto sharedProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::SHARED_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto hostProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::HOST_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedSharedMemoryAllocation = svmAllocationsManager->createSharedUnifiedMemoryAllocation(4096u, sharedProperties, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()));
    auto unifiedHostMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, hostProperties);

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_EQ(0u, commandStreamReceiver.getResidencyAllocations().size());
    mockKernel.mockKernel->setUnifiedMemoryProperty(CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, true);

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_EQ(1u, commandStreamReceiver.getResidencyAllocations().size());
    EXPECT_EQ(commandStreamReceiver.getResidencyAllocations()[0]->getGpuAddress(), castToUint64(unifiedSharedMemoryAllocation));

    svmAllocationsManager->freeSVMAlloc(unifiedSharedMemoryAllocation);
    svmAllocationsManager->freeSVMAlloc(unifiedHostMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenDeviceUnifiedMemoryAndPageFaultManagerWhenMakeResidentIsCalledThenAllocationIsNotDecommited) {
    auto mockPageFaultManager = new MockPageFaultManager();
    static_cast<MockMemoryManager *>(this->pDevice->getExecutionEnvironment()->memoryManager.get())->pageFaultManager.reset(mockPageFaultManager);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto deviceProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::DEVICE_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, deviceProperties);
    auto unifiedMemoryGraphicsAllocation = svmAllocationsManager->getSVMAlloc(unifiedMemoryAllocation);

    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    mockKernel.mockKernel->setUnifiedMemoryExecInfo(unifiedMemoryGraphicsAllocation->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex()));
    EXPECT_EQ(1u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());

    mockKernel.mockKernel->makeResident(commandStreamReceiver);

    EXPECT_EQ(mockPageFaultManager->allowMemoryAccessCalled, 0);
    EXPECT_EQ(mockPageFaultManager->protectMemoryCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToGpuCalled, 0);

    mockKernel.mockKernel->clearUnifiedMemoryExecInfo();
    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenSharedUnifiedMemoryAndPageFaultManagerWhenMakeResidentIsCalledThenAllocationIsDecommited) {
    auto mockPageFaultManager = new MockPageFaultManager();
    static_cast<MockMemoryManager *>(this->pDevice->getExecutionEnvironment()->memoryManager.get())->pageFaultManager.reset(mockPageFaultManager);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto sharedProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::SHARED_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createSharedUnifiedMemoryAllocation(4096u, sharedProperties, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()));
    auto unifiedMemoryGraphicsAllocation = svmAllocationsManager->getSVMAlloc(unifiedMemoryAllocation);
    mockPageFaultManager->insertAllocation(unifiedMemoryAllocation, 4096u, svmAllocationsManager, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()), {});

    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);

    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    mockKernel.mockKernel->setUnifiedMemoryExecInfo(unifiedMemoryGraphicsAllocation->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex()));
    EXPECT_EQ(1u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());

    mockKernel.mockKernel->makeResident(commandStreamReceiver);

    EXPECT_EQ(mockPageFaultManager->allowMemoryAccessCalled, 0);
    EXPECT_EQ(mockPageFaultManager->protectMemoryCalled, 1);
    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToGpuCalled, 1);

    EXPECT_EQ(mockPageFaultManager->protectedMemoryAccessAddress, unifiedMemoryAllocation);
    EXPECT_EQ(mockPageFaultManager->protectedSize, 4096u);
    EXPECT_EQ(mockPageFaultManager->transferToGpuAddress, unifiedMemoryAllocation);

    mockKernel.mockKernel->clearUnifiedMemoryExecInfo();
    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenSharedUnifiedMemoryAndNotRequiredMemSyncWhenMakeResidentIsCalledThenAllocationIsNotDecommited) {
    auto mockPageFaultManager = new MockPageFaultManager();
    static_cast<MockMemoryManager *>(this->pDevice->getExecutionEnvironment()->memoryManager.get())->pageFaultManager.reset(mockPageFaultManager);
    MockKernelWithInternals mockKernel(*this->pClDevice, nullptr, true);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto sharedProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::SHARED_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createSharedUnifiedMemoryAllocation(4096u, sharedProperties, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()));
    auto unifiedMemoryGraphicsAllocation = svmAllocationsManager->getSVMAlloc(unifiedMemoryAllocation);
    mockPageFaultManager->insertAllocation(unifiedMemoryAllocation, 4096u, svmAllocationsManager, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()), {});

    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    auto gpuAllocation = unifiedMemoryGraphicsAllocation->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex());
    mockKernel.mockKernel->kernelArguments[0] = {Kernel::kernelArgType::SVM_ALLOC_OBJ, gpuAllocation, unifiedMemoryAllocation, 4096u, gpuAllocation, sizeof(uintptr_t)};
    mockKernel.mockKernel->setUnifiedMemorySyncRequirement(false);

    mockKernel.mockKernel->makeResident(commandStreamReceiver);

    EXPECT_EQ(mockPageFaultManager->allowMemoryAccessCalled, 0);
    EXPECT_EQ(mockPageFaultManager->protectMemoryCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToGpuCalled, 0);

    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenSharedUnifiedMemoryRequiredMemSyncWhenMakeResidentIsCalledThenAllocationIsDecommited) {
    auto mockPageFaultManager = new MockPageFaultManager();
    static_cast<MockMemoryManager *>(this->pDevice->getExecutionEnvironment()->memoryManager.get())->pageFaultManager.reset(mockPageFaultManager);
    MockKernelWithInternals mockKernel(*this->pClDevice, nullptr, true);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto sharedProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::SHARED_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createSharedUnifiedMemoryAllocation(4096u, sharedProperties, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()));
    auto unifiedMemoryGraphicsAllocation = svmAllocationsManager->getSVMAlloc(unifiedMemoryAllocation);
    mockPageFaultManager->insertAllocation(unifiedMemoryAllocation, 4096u, svmAllocationsManager, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()), {});

    auto gpuAllocation = unifiedMemoryGraphicsAllocation->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex());
    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    mockKernel.mockKernel->kernelArguments[0] = {Kernel::kernelArgType::SVM_ALLOC_OBJ, gpuAllocation, unifiedMemoryAllocation, 4096u, gpuAllocation, sizeof(uintptr_t)};
    mockKernel.mockKernel->setUnifiedMemorySyncRequirement(true);

    mockKernel.mockKernel->makeResident(commandStreamReceiver);

    EXPECT_EQ(mockPageFaultManager->allowMemoryAccessCalled, 0);
    EXPECT_EQ(mockPageFaultManager->protectMemoryCalled, 1);
    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToGpuCalled, 1);

    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenSharedUnifiedMemoryAllocPageFaultManagerAndIndirectAllocsAllowedWhenMakeResidentIsCalledThenAllocationIsDecommited) {
    auto mockPageFaultManager = new MockPageFaultManager();
    static_cast<MockMemoryManager *>(this->pDevice->getExecutionEnvironment()->memoryManager.get())->pageFaultManager.reset(mockPageFaultManager);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto sharedProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::SHARED_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createSharedUnifiedMemoryAllocation(4096u, sharedProperties, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()));
    mockPageFaultManager->insertAllocation(unifiedMemoryAllocation, 4096u, svmAllocationsManager, mockKernel.mockContext->getSpecialQueue(pDevice->getRootDeviceIndex()), {});

    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed = true;

    mockKernel.mockKernel->makeResident(commandStreamReceiver);

    EXPECT_EQ(mockPageFaultManager->allowMemoryAccessCalled, 0);
    EXPECT_EQ(mockPageFaultManager->protectMemoryCalled, 1);
    EXPECT_EQ(mockPageFaultManager->transferToCpuCalled, 0);
    EXPECT_EQ(mockPageFaultManager->transferToGpuCalled, 1);

    EXPECT_EQ(mockPageFaultManager->protectedMemoryAccessAddress, unifiedMemoryAllocation);
    EXPECT_EQ(mockPageFaultManager->protectedSize, 4096u);
    EXPECT_EQ(mockPageFaultManager->transferToGpuAddress, unifiedMemoryAllocation);

    mockKernel.mockKernel->clearUnifiedMemoryExecInfo();
    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenSetKernelExecInfoWithUnifiedMemoryIsCalledThenAllocationIsStoredWithinKernel) {
    MockKernelWithInternals mockKernel(*this->pClDevice);
    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto deviceProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::DEVICE_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, deviceProperties);
    auto unifiedMemoryGraphicsAllocation = svmAllocationsManager->getSVMAlloc(unifiedMemoryAllocation);

    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());

    mockKernel.mockKernel->setUnifiedMemoryExecInfo(unifiedMemoryGraphicsAllocation->gpuAllocations.getGraphicsAllocation(pDevice->getRootDeviceIndex()));

    EXPECT_EQ(1u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    EXPECT_EQ(mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations[0]->getGpuAddress(), castToUint64(unifiedMemoryAllocation));

    mockKernel.mockKernel->makeResident(this->pDevice->getGpgpuCommandStreamReceiver());
    EXPECT_EQ(1u, commandStreamReceiver.getResidencyAllocations().size());
    EXPECT_EQ(commandStreamReceiver.getResidencyAllocations()[0]->getGpuAddress(), castToUint64(unifiedMemoryAllocation));

    mockKernel.mockKernel->clearUnifiedMemoryExecInfo();
    EXPECT_EQ(0u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryIsCalledThenAllocationIsStoredWithinKernel) {
    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);

    auto svmAllocationsManager = mockKernel.mockContext->getSVMAllocsManager();
    auto deviceProperties = SVMAllocsManager::UnifiedMemoryProperties(InternalMemoryType::DEVICE_UNIFIED_MEMORY, mockKernel.mockContext->getRootDeviceIndices(), mockKernel.mockContext->getDeviceBitfields());
    auto unifiedMemoryAllocation = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, deviceProperties);

    auto unifiedMemoryAllocation2 = svmAllocationsManager->createUnifiedMemoryAllocation(4096u, deviceProperties);

    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_USM_PTRS_INTEL, sizeof(unifiedMemoryAllocation), &unifiedMemoryAllocation);
    EXPECT_EQ(CL_SUCCESS, status);

    EXPECT_EQ(1u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    EXPECT_EQ(mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations[0]->getGpuAddress(), castToUint64(unifiedMemoryAllocation));

    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_USM_PTRS_INTEL, sizeof(unifiedMemoryAllocation), &unifiedMemoryAllocation2);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_EQ(1u, mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations.size());
    EXPECT_EQ(mockKernel.mockKernel->kernelUnifiedMemoryGfxAllocations[0]->getGpuAddress(), castToUint64(unifiedMemoryAllocation2));

    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation);
    svmAllocationsManager->freeSVMAlloc(unifiedMemoryAllocation2);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryDevicePropertyAndDisableIndirectAccessNotSetThenKernelControlIsChanged) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(0);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectDeviceAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectDeviceAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_TRUE(mockKernel.mockKernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed);
    enableIndirectDeviceAccess = CL_FALSE;
    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectDeviceAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryDevicePropertyAndDisableIndirectAccessSetThenKernelControlIsNotSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(1);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectDeviceAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectDeviceAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryDevicePropertyAndDisableIndirectAccessNotSetAndNoIndirectAccessInKernelThenKernelControlIsNotSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(0);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    mockKernel.mockKernel->kernelHasIndirectAccess = false;
    cl_bool enableIndirectDeviceAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectDeviceAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryDevicePropertyIsCalledThenKernelControlIsChanged) {
    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectDeviceAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectDeviceAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_TRUE(mockKernel.mockKernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed);
    enableIndirectDeviceAccess = CL_FALSE;
    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_DEVICE_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectDeviceAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectDeviceAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryHostPropertyAndDisableIndirectAccessNotSetThenKernelControlIsChanged) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(0);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectHostAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectHostAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_TRUE(mockKernel.mockKernel->unifiedMemoryControls.indirectHostAllocationsAllowed);
    enableIndirectHostAccess = CL_FALSE;
    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectHostAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectHostAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryHostPropertyAndDisableIndirectAccessSetThenKernelControlIsNotSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(1);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectHostAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectHostAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectHostAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryHostPropertyAndDisableIndirectAccessNotSetAndNoIndirectAccessInKernelThenKernelControlIsNotSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(0);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    mockKernel.mockKernel->kernelHasIndirectAccess = false;
    cl_bool enableIndirectHostAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectHostAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectHostAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemoryHostPropertyIsCalledThenKernelControlIsChanged) {
    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectHostAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectHostAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_TRUE(mockKernel.mockKernel->unifiedMemoryControls.indirectHostAllocationsAllowed);
    enableIndirectHostAccess = CL_FALSE;
    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_HOST_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectHostAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectHostAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemorySharedPropertyAndDisableIndirectAccessNotSetThenKernelControlIsChanged) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(0);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectSharedAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectSharedAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_TRUE(mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed);
    enableIndirectSharedAccess = CL_FALSE;
    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectSharedAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemorySharedPropertyAndDisableIndirectAccessSetThenKernelControlIsNotSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(1);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectSharedAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectSharedAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemorySharedPropertyAndDisableIndirectAccessNotSetAndNoIndirectAccessInKernelThenKernelControlIsNotSet) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.DisableIndirectAccess.set(0);

    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    mockKernel.mockKernel->kernelHasIndirectAccess = false;
    cl_bool enableIndirectSharedAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectSharedAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWhenclSetKernelExecInfoWithUnifiedMemorySharedPropertyIsCalledThenKernelControlIsChanged) {
    REQUIRE_SVM_OR_SKIP(pClDevice);
    MockKernelWithInternals mockKernel(*this->pClDevice);
    cl_bool enableIndirectSharedAccess = CL_TRUE;
    auto status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectSharedAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_TRUE(mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed);
    enableIndirectSharedAccess = CL_FALSE;
    status = clSetKernelExecInfo(mockKernel.mockMultiDeviceKernel, CL_KERNEL_EXEC_INFO_INDIRECT_SHARED_ACCESS_INTEL, sizeof(cl_bool), &enableIndirectSharedAccess);
    EXPECT_EQ(CL_SUCCESS, status);
    EXPECT_FALSE(mockKernel.mockKernel->unifiedMemoryControls.indirectSharedAllocationsAllowed);
}

HWTEST_F(KernelResidencyTest, givenKernelWithNoKernelArgLoadNorKernelArgStoreNorKernelArgAtomicThenKernelHasIndirectAccessIsSetToFalse) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgLoad = false;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgStore = false;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgAtomic = false;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    MockProgram program(toClDeviceVector(*pClDevice));
    MockContext ctx;
    program.setContext(&ctx);
    program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface = new MockGraphicsAllocation();
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_FALSE(pKernel->getHasIndirectAccess());

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWithNoKernelArgLoadThenKernelHasIndirectAccessIsSetToTrue) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgLoad = true;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgStore = false;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgAtomic = false;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    MockProgram program(toClDeviceVector(*pClDevice));
    MockContext ctx;
    program.setContext(&ctx);
    program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface = new MockGraphicsAllocation();
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_TRUE(pKernel->getHasIndirectAccess());

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWithNoKernelArgStoreThenKernelHasIndirectAccessIsSetToTrue) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgLoad = false;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgStore = true;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgAtomic = false;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    MockProgram program(toClDeviceVector(*pClDevice));
    MockContext ctx;
    program.setContext(&ctx);
    program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface = new MockGraphicsAllocation();
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_TRUE(pKernel->getHasIndirectAccess());

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

HWTEST_F(KernelResidencyTest, givenKernelWithNoKernelArgAtomicThenKernelHasIndirectAccessIsSetToTrue) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgLoad = false;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgStore = false;
    pKernelInfo->kernelDescriptor.kernelAttributes.hasNonKernelArgAtomic = true;

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    commandStreamReceiver.storeMakeResidentAllocations = true;

    auto memoryManager = commandStreamReceiver.getMemoryManager();
    pKernelInfo->kernelAllocation = memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{pDevice->getRootDeviceIndex(), MemoryConstants::pageSize});

    MockProgram program(toClDeviceVector(*pClDevice));
    MockContext ctx;
    program.setContext(&ctx);
    program.buildInfos[pDevice->getRootDeviceIndex()].globalSurface = new MockGraphicsAllocation();
    std::unique_ptr<MockKernel> pKernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));
    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());

    EXPECT_TRUE(pKernel->getHasIndirectAccess());

    memoryManager->freeGraphicsMemory(pKernelInfo->kernelAllocation);
}

TEST(KernelConfigTests, givenTwoKernelConfigsWhenCompareThenResultsAreCorrect) {
    Vec3<size_t> lws{1, 1, 1};
    Vec3<size_t> gws{1, 1, 1};
    Vec3<size_t> offsets{1, 1, 1};
    MockKernel::KernelConfig config{gws, lws, offsets};
    MockKernel::KernelConfig config2{gws, lws, offsets};
    EXPECT_TRUE(config == config2);

    config2.offsets.z = 2;
    EXPECT_FALSE(config == config2);

    config2.lws.z = 2;
    config2.offsets.z = 1;
    EXPECT_FALSE(config == config2);

    config2.lws.z = 1;
    config2.gws.z = 2;
    EXPECT_FALSE(config == config2);
}

HWTEST_F(KernelResidencyTest, givenEnableFullKernelTuningWhenPerformTunningThenKernelConfigDataIsTracked) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableKernelTunning.set(2u);

    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();
    MockKernelWithInternals mockKernel(*this->pClDevice);

    Vec3<size_t> lws{1, 1, 1};
    Vec3<size_t> gws{1, 1, 1};
    Vec3<size_t> offsets{1, 1, 1};
    MockKernel::KernelConfig config{gws, lws, offsets};

    MockTimestampPacketContainer container(*commandStreamReceiver.getTimestampPacketAllocator(), 1);
    MockTimestampPacketContainer subdeviceContainer(*commandStreamReceiver.getTimestampPacketAllocator(), 2);

    auto result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_EQ(result, mockKernel.mockKernel->kernelSubmissionMap.end());

    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::STANDARD_TUNNING_IN_PROGRESS);
    EXPECT_FALSE(mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);

    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &subdeviceContainer);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::SUBDEVICE_TUNNING_IN_PROGRESS);
    EXPECT_TRUE(mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);

    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::SUBDEVICE_TUNNING_IN_PROGRESS);
    EXPECT_FALSE(mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);

    container.getNode(0u)->tagForCpuAccess->packets->globalEnd = 2u;
    container.getNode(0u)->tagForCpuAccess->packets->contextEnd = 2u;
    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::SUBDEVICE_TUNNING_IN_PROGRESS);
    EXPECT_FALSE(mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);

    subdeviceContainer.getNode(0u)->tagForCpuAccess->packets->globalEnd = 2u;
    subdeviceContainer.getNode(0u)->tagForCpuAccess->packets->contextEnd = 2u;
    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_NE(result->second.kernelStandardTimestamps.get(), nullptr);
    EXPECT_NE(result->second.kernelSubdeviceTimestamps.get(), nullptr);
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::SUBDEVICE_TUNNING_IN_PROGRESS);
    EXPECT_FALSE(mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);

    subdeviceContainer.getNode(1u)->tagForCpuAccess->packets->globalEnd = 2u;
    subdeviceContainer.getNode(1u)->tagForCpuAccess->packets->contextEnd = 2u;
    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_EQ(result->second.kernelStandardTimestamps.get(), nullptr);
    EXPECT_EQ(result->second.kernelSubdeviceTimestamps.get(), nullptr);
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::TUNNING_DONE);
    EXPECT_EQ(result->second.singleSubdevicePrefered, mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);

    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);
    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_NE(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_EQ(result->second.status, MockKernel::TunningStatus::TUNNING_DONE);
    EXPECT_EQ(result->second.singleSubdevicePrefered, mockKernel.mockKernel->singleSubdevicePreferedInCurrentEnqueue);
}

HWTEST_F(KernelResidencyTest, givenSimpleKernelTunningAndNoAtomicsWhenPerformTunningThenSingleSubdeviceIsPreferred) {
    DebugManagerStateRestore restorer;
    DebugManager.flags.EnableKernelTunning.set(1u);

    auto &commandStreamReceiver = this->pDevice->getUltCommandStreamReceiver<FamilyType>();
    MockKernelWithInternals mockKernel(*this->pClDevice);

    Vec3<size_t> lws{1, 1, 1};
    Vec3<size_t> gws{1, 1, 1};
    Vec3<size_t> offsets{1, 1, 1};
    MockKernel::KernelConfig config{gws, lws, offsets};

    MockTimestampPacketContainer container(*commandStreamReceiver.getTimestampPacketAllocator(), 1);

    auto result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_EQ(result, mockKernel.mockKernel->kernelSubmissionMap.end());

    mockKernel.mockKernel->performKernelTunning(commandStreamReceiver, lws, gws, offsets, &container);

    result = mockKernel.mockKernel->kernelSubmissionMap.find(config);
    EXPECT_EQ(result, mockKernel.mockKernel->kernelSubmissionMap.end());
    EXPECT_NE(mockKernel.mockKernel->isSingleSubdevicePreferred(), mockKernel.mockKernel->getKernelInfo(0u).kernelDescriptor.kernelAttributes.flags.useGlobalAtomics);
}

TEST(KernelImageDetectionTests, givenKernelWithImagesOnlyWhenItIsAskedIfItHasImagesOnlyThenTrueIsReturned) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelArgInfo.resize(3);
    pKernelInfo->kernelArgInfo[2].isImage = true;
    pKernelInfo->kernelArgInfo[1].isMediaBlockImage = true;
    pKernelInfo->kernelArgInfo[0].isMediaImage = true;

    const auto rootDeviceIndex = 0u;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get(), rootDeviceIndex));
    auto context = clUniquePtr(new MockContext(device.get()));
    auto program = clUniquePtr(new MockProgram(context.get(), false, toClDeviceVector(*device)));
    auto kernel = std::make_unique<MockKernel>(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *device);
    EXPECT_FALSE(kernel->usesOnlyImages());
    kernel->initialize();
    EXPECT_TRUE(kernel->usesOnlyImages());
}

TEST(KernelImageDetectionTests, givenKernelWithImagesAndBuffersWhenItIsAskedIfItHasImagesOnlyThenFalseIsReturned) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelArgInfo.resize(3);
    pKernelInfo->kernelArgInfo[2].isImage = true;
    pKernelInfo->kernelArgInfo[1].isBuffer = true;
    pKernelInfo->kernelArgInfo[0].isMediaImage = true;

    const auto rootDeviceIndex = 0u;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get(), rootDeviceIndex));
    auto context = clUniquePtr(new MockContext(device.get()));
    auto program = clUniquePtr(new MockProgram(context.get(), false, toClDeviceVector(*device)));
    auto kernel = std::make_unique<MockKernel>(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *device);
    EXPECT_FALSE(kernel->usesOnlyImages());
    kernel->initialize();
    EXPECT_FALSE(kernel->usesOnlyImages());
}

TEST(KernelImageDetectionTests, givenKernelWithNoImagesWhenItIsAskedIfItHasImagesOnlyThenFalseIsReturned) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;
    pKernelInfo->kernelArgInfo.resize(1);
    pKernelInfo->kernelArgInfo[0].isBuffer = true;

    const auto rootDeviceIndex = 0u;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get(), rootDeviceIndex));
    auto context = clUniquePtr(new MockContext(device.get()));
    auto program = clUniquePtr(new MockProgram(context.get(), false, toClDeviceVector(*device)));
    auto kernel = std::make_unique<MockKernel>(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *device);
    EXPECT_FALSE(kernel->usesOnlyImages());
    kernel->initialize();
    EXPECT_FALSE(kernel->usesOnlyImages());
}

HWTEST_F(KernelResidencyTest, WhenMakingArgsResidentThenImageFromImageCheckIsCorrect) {
    ASSERT_NE(nullptr, pDevice);

    //create NV12 image
    cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_HOST_NO_ACCESS;
    cl_image_format imageFormat;
    imageFormat.image_channel_data_type = CL_UNORM_INT8;
    imageFormat.image_channel_order = CL_NV12_INTEL;
    auto surfaceFormat = Image::getSurfaceFormatFromTable(
        flags, &imageFormat, pClDevice->getHardwareInfo().capabilityTable.supportsOcl21Features);

    cl_image_desc imageDesc = {};
    imageDesc.image_type = CL_MEM_OBJECT_IMAGE2D;
    imageDesc.image_width = 16;
    imageDesc.image_height = 16;
    imageDesc.image_depth = 1;

    cl_int retVal;
    MockContext context;
    std::unique_ptr<NEO::Image> imageNV12(
        Image::create(&context, MemoryPropertiesHelper::createMemoryProperties(flags, 0, 0, &context.getDevice(0)->getDevice()),
                      flags, 0, surfaceFormat, &imageDesc, nullptr, retVal));
    EXPECT_EQ(imageNV12->getMediaPlaneType(), 0u);

    //create Y plane
    imageFormat.image_channel_order = CL_R;
    flags = CL_MEM_READ_ONLY;
    surfaceFormat = Image::getSurfaceFormatFromTable(
        flags, &imageFormat, context.getDevice(0)->getHardwareInfo().capabilityTable.supportsOcl21Features);

    imageDesc.image_width = 0;
    imageDesc.image_height = 0;
    imageDesc.image_depth = 0;
    imageDesc.mem_object = imageNV12.get();

    std::unique_ptr<NEO::Image> imageY(
        Image::create(&context, MemoryPropertiesHelper::createMemoryProperties(flags, 0, 0, &context.getDevice(0)->getDevice()),
                      flags, 0, surfaceFormat, &imageDesc, nullptr, retVal));
    EXPECT_EQ(imageY->getMediaPlaneType(), 0u);

    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;

    KernelArgInfo kernelArgInfo;
    kernelArgInfo.isImage = true;

    pKernelInfo->kernelArgInfo.push_back(std::move(kernelArgInfo));

    auto program = std::make_unique<MockProgram>(toClDeviceVector(*pClDevice));
    program->setContext(&context);
    std::unique_ptr<MockKernel> pKernel(new MockKernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice));

    ASSERT_EQ(CL_SUCCESS, pKernel->initialize());
    pKernel->storeKernelArg(0, Kernel::IMAGE_OBJ, (cl_mem)imageY.get(), NULL, 0);
    pKernel->makeResident(pDevice->getGpgpuCommandStreamReceiver());

    EXPECT_FALSE(imageNV12->isImageFromImage());
    EXPECT_TRUE(imageY->isImageFromImage());

    auto &commandStreamReceiver = pDevice->getUltCommandStreamReceiver<FamilyType>();
    EXPECT_EQ(CommandStreamReceiver::SamplerCacheFlushState::samplerCacheFlushBefore, commandStreamReceiver.samplerCacheFlushRequired);
}

struct KernelExecutionEnvironmentTest : public Test<ClDeviceFixture> {
    void SetUp() override {
        ClDeviceFixture::SetUp();

        program = std::make_unique<MockProgram>(toClDeviceVector(*pClDevice));
        pKernelInfo = std::make_unique<KernelInfo>();
        pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;

        pKernel = new MockKernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
        ASSERT_EQ(CL_SUCCESS, pKernel->initialize());
    }

    void TearDown() override {
        delete pKernel;

        ClDeviceFixture::TearDown();
    }

    MockKernel *pKernel;
    std::unique_ptr<MockProgram> program;
    std::unique_ptr<KernelInfo> pKernelInfo;
    SPatchExecutionEnvironment executionEnvironment = {};
};

TEST_F(KernelExecutionEnvironmentTest, GivenCompiledWorkGroupSizeIsZeroWhenGettingMaxRequiredWorkGroupSizeThenMaxWorkGroupSizeIsCorrect) {
    auto maxWorkGroupSize = static_cast<size_t>(pDevice->getDeviceInfo().maxWorkGroupSize);
    auto oldRequiredWorkGroupSizeX = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0];
    auto oldRequiredWorkGroupSizeY = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1];
    auto oldRequiredWorkGroupSizeZ = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2];

    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = 0;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = 0;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = 0;

    EXPECT_EQ(maxWorkGroupSize, this->pKernelInfo->getMaxRequiredWorkGroupSize(maxWorkGroupSize));

    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = oldRequiredWorkGroupSizeX;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = oldRequiredWorkGroupSizeY;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = oldRequiredWorkGroupSizeZ;
}

TEST_F(KernelExecutionEnvironmentTest, GivenCompiledWorkGroupSizeLowerThanMaxWorkGroupSizeWhenGettingMaxRequiredWorkGroupSizeThenMaxWorkGroupSizeIsCorrect) {
    auto maxWorkGroupSize = static_cast<size_t>(pDevice->getDeviceInfo().maxWorkGroupSize);
    auto oldRequiredWorkGroupSizeX = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0];
    auto oldRequiredWorkGroupSizeY = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1];
    auto oldRequiredWorkGroupSizeZ = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2];

    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = static_cast<uint16_t>(maxWorkGroupSize / 2);
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = 1;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = 1;

    EXPECT_EQ(maxWorkGroupSize / 2, this->pKernelInfo->getMaxRequiredWorkGroupSize(maxWorkGroupSize));

    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = oldRequiredWorkGroupSizeX;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = oldRequiredWorkGroupSizeY;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = oldRequiredWorkGroupSizeZ;
}

TEST_F(KernelExecutionEnvironmentTest, GivenCompiledWorkGroupSizeIsGreaterThanMaxWorkGroupSizeWhenGettingMaxRequiredWorkGroupSizeThenMaxWorkGroupSizeIsCorrect) {
    auto maxWorkGroupSize = static_cast<size_t>(pDevice->getDeviceInfo().maxWorkGroupSize);
    auto oldRequiredWorkGroupSizeX = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0];
    auto oldRequiredWorkGroupSizeY = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1];
    auto oldRequiredWorkGroupSizeZ = this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2];

    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = static_cast<uint16_t>(maxWorkGroupSize);
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = static_cast<uint16_t>(maxWorkGroupSize);
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = static_cast<uint16_t>(maxWorkGroupSize);

    EXPECT_EQ(maxWorkGroupSize, this->pKernelInfo->getMaxRequiredWorkGroupSize(maxWorkGroupSize));

    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = oldRequiredWorkGroupSizeX;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = oldRequiredWorkGroupSizeY;
    this->pKernelInfo->kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = oldRequiredWorkGroupSizeZ;
}

struct KernelCrossThreadTests : Test<ClDeviceFixture> {
    KernelCrossThreadTests() {
    }

    void SetUp() override {
        ClDeviceFixture::SetUp();
        program = std::make_unique<MockProgram>(toClDeviceVector(*pClDevice));
        patchDataParameterStream.DataParameterStreamSize = 64 * sizeof(uint8_t);

        pKernelInfo = std::make_unique<KernelInfo>();
        ASSERT_NE(nullptr, pKernelInfo);
        populateKernelDescriptor(pKernelInfo->kernelDescriptor, patchDataParameterStream);
        pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 32;
    }

    void TearDown() override {

        ClDeviceFixture::TearDown();
    }

    std::unique_ptr<MockProgram> program;
    std::unique_ptr<KernelInfo> pKernelInfo;
    SPatchDataParameterStream patchDataParameterStream;
    SPatchExecutionEnvironment executionEnvironment = {};
};

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenGlobalWorkOffsetIsCorrect) {

    pKernelInfo->workloadInfo.globalWorkOffsetOffsets[1] = 4;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkOffsetX);
    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkOffsetY);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkOffsetY);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkOffsetZ);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenLocalWorkSizeIsCorrect) {

    pKernelInfo->workloadInfo.localWorkSizeOffsets[0] = 0xc;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeX);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeX);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeY);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeZ);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenLocalWorkSize2IsCorrect) {

    pKernelInfo->workloadInfo.localWorkSizeOffsets2[1] = 0xd;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeX2);
    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeY2);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeY2);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].localWorkSizeZ2);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenGlobalWorkSizeIsCorrect) {

    pKernelInfo->workloadInfo.globalWorkSizeOffsets[2] = 8;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkSizeX);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkSizeY);
    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkSizeZ);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].globalWorkSizeZ);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenLocalWorkDimIsCorrect) {

    pKernelInfo->workloadInfo.workDimOffset = 12;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].workDim);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].workDim);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenNumWorkGroupsIsCorrect) {

    pKernelInfo->workloadInfo.numWorkGroupsOffset[0] = 0 * sizeof(uint32_t);
    pKernelInfo->workloadInfo.numWorkGroupsOffset[1] = 1 * sizeof(uint32_t);
    pKernelInfo->workloadInfo.numWorkGroupsOffset[2] = 2 * sizeof(uint32_t);

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].numWorkGroupsX);
    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].numWorkGroupsY);
    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].numWorkGroupsZ);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].numWorkGroupsX);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].numWorkGroupsY);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].numWorkGroupsZ);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenEnqueuedLocalWorkSizeIsCorrect) {

    pKernelInfo->workloadInfo.enqueuedLocalWorkSizeOffsets[0] = 0;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].enqueuedLocalWorkSizeX);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].enqueuedLocalWorkSizeX);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].enqueuedLocalWorkSizeY);
    EXPECT_EQ(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].enqueuedLocalWorkSizeZ);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenEnqueuedMaxWorkGroupSizeIsCorrect) {
    pKernelInfo->workloadInfo.maxWorkGroupSizeOffset = 12;

    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].maxWorkGroupSizeForCrossThreadData);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].maxWorkGroupSizeForCrossThreadData);
    EXPECT_EQ(static_cast<void *>(kernel.getCrossThreadData(rootDeviceIndex) + pKernelInfo->workloadInfo.maxWorkGroupSizeOffset), static_cast<void *>(kernel.kernelDeviceInfos[rootDeviceIndex].maxWorkGroupSizeForCrossThreadData));
    EXPECT_EQ(pDevice->getDeviceInfo().maxWorkGroupSize, *kernel.kernelDeviceInfos[rootDeviceIndex].maxWorkGroupSizeForCrossThreadData);
    EXPECT_EQ(pDevice->getDeviceInfo().maxWorkGroupSize, kernel.kernelDeviceInfos[rootDeviceIndex].maxKernelWorkGroupSize);
}

TEST_F(KernelCrossThreadTests, WhenKernelIsInitializedThenDataParameterSimdSizeIsCorrect) {
    pKernelInfo->workloadInfo.simdSizeOffset = 16;
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 16;
    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].dataParameterSimdSize);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].dataParameterSimdSize);
    EXPECT_EQ(static_cast<void *>(kernel.getCrossThreadData(rootDeviceIndex) + pKernelInfo->workloadInfo.simdSizeOffset), static_cast<void *>(kernel.kernelDeviceInfos[rootDeviceIndex].dataParameterSimdSize));
    EXPECT_EQ_VAL(pKernelInfo->getMaxSimdSize(), *kernel.kernelDeviceInfos[rootDeviceIndex].dataParameterSimdSize);
}

TEST_F(KernelCrossThreadTests, GivenParentEventOffsetWhenKernelIsInitializedThenParentEventIsInitiatedWithInvalid) {
    pKernelInfo->workloadInfo.parentEventOffset = 16;
    MockKernel kernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    ASSERT_EQ(CL_SUCCESS, kernel.initialize());

    EXPECT_NE(nullptr, kernel.kernelDeviceInfos[rootDeviceIndex].parentEventOffset);
    EXPECT_NE(&Kernel::dummyPatchLocation, kernel.kernelDeviceInfos[rootDeviceIndex].parentEventOffset);
    EXPECT_EQ(static_cast<void *>(kernel.getCrossThreadData(rootDeviceIndex) + pKernelInfo->workloadInfo.parentEventOffset), static_cast<void *>(kernel.kernelDeviceInfos[rootDeviceIndex].parentEventOffset));
    EXPECT_EQ(WorkloadInfo::invalidParentEvent, *kernel.kernelDeviceInfos[rootDeviceIndex].parentEventOffset);
}

TEST_F(KernelCrossThreadTests, WhenAddingKernelThenProgramRefCountIsIncremented) {

    auto refCount = program->getReference();
    MockKernel *kernel = new MockKernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);
    auto refCount2 = program->getReference();
    EXPECT_EQ(refCount2, refCount + 1);

    delete kernel;
    auto refCount3 = program->getReference();
    EXPECT_EQ(refCount, refCount3);
}

TEST_F(KernelCrossThreadTests, GivenSlmStatisSizeWhenCreatingKernelThenSlmTotalSizeIsSet) {

    pKernelInfo->workloadInfo.slmStaticSize = 1024;

    MockKernel *kernel = new MockKernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    EXPECT_EQ(1024u, kernel->kernelDeviceInfos[rootDeviceIndex].slmTotalSize);

    delete kernel;
}
TEST_F(KernelCrossThreadTests, givenKernelWithPrivateMemoryWhenItIsCreatedThenCurbeIsPatchedProperly) {
    SPatchAllocateStatelessPrivateSurface allocatePrivate;
    allocatePrivate.DataParamSize = 8;
    allocatePrivate.DataParamOffset = 0;
    allocatePrivate.PerThreadPrivateMemorySize = 1;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, allocatePrivate);

    MockKernel *kernel = new MockKernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    kernel->initialize();

    auto privateSurface = kernel->kernelDeviceInfos[pDevice->getRootDeviceIndex()].privateSurface;

    auto constantBuffer = kernel->getCrossThreadData(rootDeviceIndex);
    auto privateAddress = (uintptr_t)privateSurface->getGpuAddressToPatch();
    auto ptrCurbe = (uint64_t *)constantBuffer;
    auto privateAddressFromCurbe = (uintptr_t)*ptrCurbe;

    EXPECT_EQ(privateAddressFromCurbe, privateAddress);

    delete kernel;
}

TEST_F(KernelCrossThreadTests, givenKernelWithPreferredWkgMultipleWhenItIsCreatedThenCurbeIsPatchedProperly) {

    pKernelInfo->workloadInfo.preferredWkgMultipleOffset = 8;
    MockKernel *kernel = new MockKernel(program.get(), MockKernel::toKernelInfoContainer(*pKernelInfo, rootDeviceIndex), *pClDevice);

    kernel->initialize();

    auto *crossThread = kernel->getCrossThreadData(rootDeviceIndex);

    uint32_t *preferredWkgMultipleOffset = (uint32_t *)ptrOffset(crossThread, 8);

    EXPECT_EQ(pKernelInfo->getMaxSimdSize(), *preferredWkgMultipleOffset);

    delete kernel;
}

TEST_F(KernelCrossThreadTests, WhenPatchingBlocksSimdSizeThenSimdSizeIsPatchedCorrectly) {
    MockKernelWithInternals *kernel = new MockKernelWithInternals(*pClDevice);

    // store offset to child's simd size in kernel info
    uint32_t crossThreadOffset = 0; //offset of simd size
    kernel->kernelInfo.childrenKernelsIdOffset.push_back({0, crossThreadOffset});

    // add a new block kernel to program
    auto infoBlock = new KernelInfo();
    infoBlock->kernelDescriptor.kernelAttributes.simdSize = 16;
    kernel->mockProgram->blockKernelManager->addBlockKernelInfo(infoBlock);

    // patch block's simd size
    kernel->mockKernel->patchBlocksSimdSize(rootDeviceIndex);

    // obtain block's simd size from cross thread data
    void *blockSimdSize = ptrOffset(kernel->mockKernel->getCrossThreadData(rootDeviceIndex), kernel->kernelInfo.childrenKernelsIdOffset[0].second);
    uint32_t *simdSize = reinterpret_cast<uint32_t *>(blockSimdSize);

    // check of block's simd size has been patched correctly
    EXPECT_EQ(kernel->mockProgram->blockKernelManager->getBlockKernelInfo(0)->getMaxSimdSize(), *simdSize);

    delete kernel;
}

TEST(KernelInfoTest, WhenPatchingBorderColorOffsetThenPatchIsAppliedCorrectly) {
    KernelInfo info;
    EXPECT_EQ(0u, info.getBorderColorOffset());

    SPatchSamplerStateArray samplerState = {};
    samplerState.BorderColorOffset = 3;
    samplerState.Count = 1;
    populateKernelDescriptor(info.kernelDescriptor, samplerState);
    EXPECT_EQ(3u, info.getBorderColorOffset());
}

TEST(KernelInfoTest, GivenArgNameWhenGettingArgNumberByNameThenCorrectValueIsReturned) {
    KernelInfo info;
    EXPECT_EQ(-1, info.getArgNumByName(""));

    KernelArgInfo kai;
    kai.metadataExtended = std::make_unique<ArgTypeMetadataExtended>();
    kai.metadataExtended->argName = "arg1";
    info.kernelArgInfo.push_back(std::move(kai));

    EXPECT_EQ(-1, info.getArgNumByName(""));
    EXPECT_EQ(-1, info.getArgNumByName("arg2"));

    EXPECT_EQ(0, info.getArgNumByName("arg1"));

    kai = {};
    kai.metadataExtended = std::make_unique<ArgTypeMetadataExtended>();
    kai.metadataExtended->argName = "arg2";
    info.kernelArgInfo.push_back(std::move(kai));

    EXPECT_EQ(0, info.getArgNumByName("arg1"));
    EXPECT_EQ(1, info.getArgNumByName("arg2"));

    info.kernelArgInfo[0].metadataExtended.reset();
    EXPECT_EQ(-1, info.getArgNumByName("arg1"));
}

TEST(KernelTest, GivenNormalKernelWhenGettingInstructionHeapSizeForExecutionModelThenZeroIsReturned) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);

    EXPECT_EQ(0u, kernel.mockKernel->getInstructionHeapSizeForExecutionModel());
}

TEST(KernelTest, WhenSettingKernelArgThenBuiltinDispatchInfoBuilderIsUsed) {
    struct MockBuiltinDispatchBuilder : BuiltinDispatchInfoBuilder {
        using BuiltinDispatchInfoBuilder::BuiltinDispatchInfoBuilder;

        bool setExplicitArg(uint32_t argIndex, size_t argSize, const void *argVal, cl_int &err) const override {
            receivedArgs.push_back(std::make_tuple(argIndex, argSize, argVal));
            err = errToReturn;
            return valueToReturn;
        }

        bool valueToReturn = false;
        cl_int errToReturn = CL_SUCCESS;
        mutable std::vector<std::tuple<uint32_t, size_t, const void *>> receivedArgs;
    };

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.kernelInfo.resizeKernelArgInfoAndRegisterParameter(1);
    kernel.mockKernel->initialize();

    MockBuiltinDispatchBuilder mockBuilder(*device->getBuiltIns(), *device);
    kernel.kernelInfo.builtinDispatchBuilder = &mockBuilder;

    mockBuilder.valueToReturn = false;
    mockBuilder.errToReturn = CL_SUCCESS;
    EXPECT_EQ(0u, kernel.mockKernel->getPatchedArgumentsNum());
    auto ret = kernel.mockKernel->setArg(1, 3, reinterpret_cast<const void *>(5));
    EXPECT_EQ(CL_SUCCESS, ret);
    EXPECT_EQ(1u, kernel.mockKernel->getPatchedArgumentsNum());

    mockBuilder.valueToReturn = false;
    mockBuilder.errToReturn = CL_INVALID_ARG_SIZE;
    ret = kernel.mockKernel->setArg(7, 11, reinterpret_cast<const void *>(13));
    EXPECT_EQ(CL_INVALID_ARG_SIZE, ret);
    EXPECT_EQ(1u, kernel.mockKernel->getPatchedArgumentsNum());

    mockBuilder.valueToReturn = true;
    mockBuilder.errToReturn = CL_SUCCESS;
    ret = kernel.mockKernel->setArg(17, 19, reinterpret_cast<const void *>(23));
    EXPECT_EQ(CL_INVALID_ARG_INDEX, ret);
    EXPECT_EQ(1u, kernel.mockKernel->getPatchedArgumentsNum());

    mockBuilder.valueToReturn = true;
    mockBuilder.errToReturn = CL_INVALID_ARG_SIZE;
    ret = kernel.mockKernel->setArg(29, 31, reinterpret_cast<const void *>(37));
    EXPECT_EQ(CL_INVALID_ARG_INDEX, ret);
    EXPECT_EQ(1u, kernel.mockKernel->getPatchedArgumentsNum());

    ASSERT_EQ(4U, mockBuilder.receivedArgs.size());

    EXPECT_EQ(1U, std::get<0>(mockBuilder.receivedArgs[0]));
    EXPECT_EQ(3U, std::get<1>(mockBuilder.receivedArgs[0]));
    EXPECT_EQ(reinterpret_cast<const void *>(5), std::get<2>(mockBuilder.receivedArgs[0]));

    EXPECT_EQ(7U, std::get<0>(mockBuilder.receivedArgs[1]));
    EXPECT_EQ(11U, std::get<1>(mockBuilder.receivedArgs[1]));
    EXPECT_EQ(reinterpret_cast<const void *>(13), std::get<2>(mockBuilder.receivedArgs[1]));

    EXPECT_EQ(17U, std::get<0>(mockBuilder.receivedArgs[2]));
    EXPECT_EQ(19U, std::get<1>(mockBuilder.receivedArgs[2]));
    EXPECT_EQ(reinterpret_cast<const void *>(23), std::get<2>(mockBuilder.receivedArgs[2]));

    EXPECT_EQ(29U, std::get<0>(mockBuilder.receivedArgs[3]));
    EXPECT_EQ(31U, std::get<1>(mockBuilder.receivedArgs[3]));
    EXPECT_EQ(reinterpret_cast<const void *>(37), std::get<2>(mockBuilder.receivedArgs[3]));
}

HWTEST_F(KernelTest, givenKernelWhenDebugFlagToUseMaxSimdForCalculationsIsUsedThenMaxWorkgroupSizeIsSimdSizeDependant) {
    DebugManagerStateRestore dbgStateRestore;
    DebugManager.flags.UseMaxSimdSizeToDeduceMaxWorkgroupSize.set(true);

    HardwareInfo myHwInfo = *defaultHwInfo;
    GT_SYSTEM_INFO &mySysInfo = myHwInfo.gtSystemInfo;

    mySysInfo.EUCount = 24;
    mySysInfo.SubSliceCount = 3;
    mySysInfo.ThreadCount = 24 * 7;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(&myHwInfo));

    MockKernelWithInternals kernel(*device);

    size_t maxKernelWkgSize;

    kernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 32;
    kernel.mockKernel->getWorkGroupInfo(*device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), &maxKernelWkgSize, nullptr);
    EXPECT_EQ(1024u, maxKernelWkgSize);

    kernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 16;
    kernel.mockKernel->getWorkGroupInfo(*device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), &maxKernelWkgSize, nullptr);
    EXPECT_EQ(512u, maxKernelWkgSize);

    kernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 8;
    kernel.mockKernel->getWorkGroupInfo(*device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), &maxKernelWkgSize, nullptr);
    EXPECT_EQ(256u, maxKernelWkgSize);
}

TEST(KernelTest, givenKernelWithKernelInfoWith32bitPointerSizeThenReport32bit) {
    KernelInfo info;
    info.gpuPointerSize = 4;

    const auto rootDeviceIndex = 0u;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr, rootDeviceIndex));
    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*device));
    std::unique_ptr<MockKernel> kernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(info, rootDeviceIndex), *device));

    EXPECT_TRUE(kernel->is32Bit(rootDeviceIndex));
}

TEST(KernelTest, givenKernelWithKernelInfoWith64bitPointerSizeThenReport64bit) {
    KernelInfo info;
    info.gpuPointerSize = 8;

    const auto rootDeviceIndex = 0u;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr, rootDeviceIndex));
    MockContext context;
    MockProgram program(&context, false, toClDeviceVector(*device));
    std::unique_ptr<MockKernel> kernel(new MockKernel(&program, MockKernel::toKernelInfoContainer(info, rootDeviceIndex), *device));

    EXPECT_FALSE(kernel->is32Bit(rootDeviceIndex));
}

TEST(KernelTest, givenFtrRenderCompressedBuffersWhenInitializingArgsWithNonStatefulAccessThenMarkKernelForAuxTranslation) {
    DebugManagerStateRestore restore;
    DebugManager.flags.ForceAuxTranslationEnabled.set(1);
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));
    auto hwInfo = device->getRootDeviceEnvironment().getMutableHardwareInfo();
    auto &capabilityTable = hwInfo->capabilityTable;
    auto context = clUniquePtr(new MockContext(device.get()));
    context->contextType = ContextType::CONTEXT_TYPE_UNRESTRICTIVE;
    MockKernelWithInternals kernel(*device, context.get());
    kernel.kernelInfo.kernelArgInfo.resize(1);
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended = std::make_unique<NEO::ArgTypeMetadataExtended>();
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended->type = "char *";
    kernel.kernelInfo.kernelArgInfo[0].isBuffer = true;

    capabilityTable.ftrRenderCompressedBuffers = false;
    kernel.kernelInfo.kernelArgInfo[0].pureStatefulBufferAccess = true;
    kernel.mockKernel->initialize();
    EXPECT_FALSE(kernel.mockKernel->isAuxTranslationRequired());

    kernel.kernelInfo.kernelArgInfo[0].pureStatefulBufferAccess = false;
    kernel.mockKernel->initialize();
    EXPECT_FALSE(kernel.mockKernel->isAuxTranslationRequired());

    capabilityTable.ftrRenderCompressedBuffers = true;
    kernel.mockKernel->initialize();
    EXPECT_EQ(ClHwHelper::get(hwInfo->platform.eRenderCoreFamily).requiresAuxResolves(kernel.kernelInfo), kernel.mockKernel->isAuxTranslationRequired());

    DebugManager.flags.ForceAuxTranslationEnabled.set(-1);
    kernel.mockKernel->initialize();
    EXPECT_EQ(ClHwHelper::get(hwInfo->platform.eRenderCoreFamily).requiresAuxResolves(kernel.kernelInfo), kernel.mockKernel->isAuxTranslationRequired());

    DebugManager.flags.ForceAuxTranslationEnabled.set(0);
    kernel.mockKernel->initialize();
    EXPECT_FALSE(kernel.mockKernel->isAuxTranslationRequired());
}

TEST(KernelTest, WhenAuxTranslationIsRequiredThenKernelSetsRequiredResolvesInContext) {
    DebugManagerStateRestore restore;
    DebugManager.flags.ForceAuxTranslationEnabled.set(1);
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));
    auto hwInfo = device->getRootDeviceEnvironment().getMutableHardwareInfo();
    hwInfo->capabilityTable.ftrRenderCompressedBuffers = true;

    auto context = clUniquePtr(new MockContext(device.get()));
    context->contextType = ContextType::CONTEXT_TYPE_UNRESTRICTIVE;
    MockKernelWithInternals kernel(*device, context.get());
    kernel.kernelInfo.kernelArgInfo.resize(1);
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended = std::make_unique<NEO::ArgTypeMetadataExtended>();
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended->type = "char *";
    kernel.kernelInfo.kernelArgInfo[0].isBuffer = true;
    kernel.kernelInfo.kernelArgInfo[0].pureStatefulBufferAccess = false;

    kernel.mockKernel->initialize();

    if (ClHwHelper::get(device->getHardwareInfo().platform.eRenderCoreFamily).requiresAuxResolves(kernel.kernelInfo)) {
        EXPECT_TRUE(context->getResolvesRequiredInKernels());
    } else {
        EXPECT_FALSE(context->getResolvesRequiredInKernels());
    }
}

TEST(KernelTest, WhenAuxTranslationIsNotRequiredThenKernelDoesNotSetRequiredResolvesInContext) {
    DebugManagerStateRestore restore;
    DebugManager.flags.ForceAuxTranslationEnabled.set(0);
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(nullptr));
    auto hwInfo = device->getRootDeviceEnvironment().getMutableHardwareInfo();
    hwInfo->capabilityTable.ftrRenderCompressedBuffers = true;

    auto context = clUniquePtr(new MockContext(device.get()));
    context->contextType = ContextType::CONTEXT_TYPE_UNRESTRICTIVE;
    MockKernelWithInternals kernel(*device, context.get());
    kernel.kernelInfo.kernelArgInfo.resize(1);
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended = std::make_unique<NEO::ArgTypeMetadataExtended>();
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended->type = "char *";
    kernel.kernelInfo.kernelArgInfo[0].isBuffer = true;
    kernel.kernelInfo.kernelArgInfo[0].pureStatefulBufferAccess = true;

    kernel.mockKernel->initialize();
    EXPECT_FALSE(context->getResolvesRequiredInKernels());
}

TEST(KernelTest, givenDebugVariableSetWhenKernelHasStatefulBufferAccessThenMarkKernelForAuxTranslation) {
    DebugManagerStateRestore restore;
    DebugManager.flags.RenderCompressedBuffersEnabled.set(1);

    HardwareInfo localHwInfo = *defaultHwInfo;

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(&localHwInfo));
    auto context = clUniquePtr(new MockContext(device.get()));
    MockKernelWithInternals kernel(*device, context.get());
    kernel.kernelInfo.kernelArgInfo.resize(1);
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended = std::make_unique<NEO::ArgTypeMetadataExtended>();
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended->type = "char *";
    kernel.kernelInfo.kernelArgInfo[0].isBuffer = true;

    kernel.kernelInfo.kernelArgInfo[0].pureStatefulBufferAccess = false;
    localHwInfo.capabilityTable.ftrRenderCompressedBuffers = false;

    kernel.mockKernel->initialize();

    if (ClHwHelper::get(localHwInfo.platform.eRenderCoreFamily).requiresAuxResolves(kernel.kernelInfo)) {
        EXPECT_TRUE(kernel.mockKernel->isAuxTranslationRequired());
    } else {
        EXPECT_FALSE(kernel.mockKernel->isAuxTranslationRequired());
    }
}

TEST(KernelTest, givenKernelWithPairArgumentWhenItIsInitializedThenPatchImmediateIsUsedAsArgHandler) {
    HardwareInfo localHwInfo = *defaultHwInfo;

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(&localHwInfo));
    auto context = clUniquePtr(new MockContext(device.get()));
    MockKernelWithInternals kernel(*device, context.get());
    kernel.kernelInfo.kernelArgInfo.resize(1);
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended = std::make_unique<NEO::ArgTypeMetadataExtended>();
    kernel.kernelInfo.kernelArgInfo[0].metadataExtended->type = "pair<char*, int>";

    kernel.mockKernel->initialize();
    EXPECT_EQ(&Kernel::setArgImmediate, kernel.mockKernel->kernelArgHandlers[0]);
}

TEST(KernelTest, whenNullAllocationThenAssignNullPointerToCacheFlushVector) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.mockKernel->kernelArgRequiresCacheFlush.resize(1);
    kernel.mockKernel->kernelArgRequiresCacheFlush[0] = reinterpret_cast<GraphicsAllocation *>(0x1);

    kernel.mockKernel->addAllocationToCacheFlushVector(0, nullptr);
    EXPECT_EQ(nullptr, kernel.mockKernel->kernelArgRequiresCacheFlush[0]);
}

TEST(KernelTest, givenKernelCompiledWithSimdSizeLowerThanExpectedWhenInitializingThenReturnError) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    auto minSimd = HwHelper::get(device->getHardwareInfo().platform.eRenderCoreFamily).getMinimalSIMDSize();
    MockKernelWithInternals kernel(*device);
    kernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 8;

    cl_int retVal = kernel.mockKernel->initialize();

    if (minSimd > 8) {
        EXPECT_EQ(CL_INVALID_KERNEL, retVal);
    } else {
        EXPECT_EQ(CL_SUCCESS, retVal);
    }
}

TEST(KernelTest, givenKernelCompiledWithSimdOneWhenInitializingThenReturnError) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 1;

    cl_int retVal = kernel.mockKernel->initialize();

    EXPECT_EQ(CL_SUCCESS, retVal);
}

TEST(KernelTest, whenAllocationRequiringCacheFlushThenAssignAllocationPointerToCacheFlushVector) {
    MockGraphicsAllocation mockAllocation;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.mockKernel->kernelArgRequiresCacheFlush.resize(1);

    mockAllocation.setMemObjectsAllocationWithWritableFlags(false);
    mockAllocation.setFlushL3Required(true);

    kernel.mockKernel->addAllocationToCacheFlushVector(0, &mockAllocation);
    EXPECT_EQ(&mockAllocation, kernel.mockKernel->kernelArgRequiresCacheFlush[0]);
}

TEST(KernelTest, whenKernelRequireCacheFlushAfterWalkerThenRequireCacheFlushAfterWalker) {
    MockGraphicsAllocation mockAllocation;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.mockKernel->svmAllocationsRequireCacheFlush = true;

    MockCommandQueue queue;

    DebugManagerStateRestore debugRestore;
    DebugManager.flags.EnableCacheFlushAfterWalker.set(true);

    queue.requiresCacheFlushAfterWalker = true;
    EXPECT_TRUE(kernel.mockKernel->requiresCacheFlushCommand(queue));

    queue.requiresCacheFlushAfterWalker = false;
    EXPECT_TRUE(kernel.mockKernel->requiresCacheFlushCommand(queue));
}

TEST(KernelTest, whenAllocationWriteableThenDoNotAssignAllocationPointerToCacheFlushVector) {
    MockGraphicsAllocation mockAllocation;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.mockKernel->kernelArgRequiresCacheFlush.resize(1);

    mockAllocation.setMemObjectsAllocationWithWritableFlags(true);
    mockAllocation.setFlushL3Required(false);

    kernel.mockKernel->addAllocationToCacheFlushVector(0, &mockAllocation);
    EXPECT_EQ(nullptr, kernel.mockKernel->kernelArgRequiresCacheFlush[0]);
}

TEST(KernelTest, whenAllocationReadOnlyNonFlushRequiredThenAssignNullPointerToCacheFlushVector) {
    MockGraphicsAllocation mockAllocation;
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    kernel.mockKernel->kernelArgRequiresCacheFlush.resize(1);
    kernel.mockKernel->kernelArgRequiresCacheFlush[0] = reinterpret_cast<GraphicsAllocation *>(0x1);

    mockAllocation.setMemObjectsAllocationWithWritableFlags(false);
    mockAllocation.setFlushL3Required(false);

    kernel.mockKernel->addAllocationToCacheFlushVector(0, &mockAllocation);
    EXPECT_EQ(nullptr, kernel.mockKernel->kernelArgRequiresCacheFlush[0]);
}

TEST(KernelTest, givenKernelUsesPrivateMemoryWhenDeviceReleasedBeforeKernelThenKernelUsesMemoryManagerFromEnvironment) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    auto executionEnvironment = device->getExecutionEnvironment();

    auto mockKernel = std::make_unique<MockKernelWithInternals>(*device);
    GraphicsAllocation *privateSurface = device->getExecutionEnvironment()->memoryManager->allocateGraphicsMemoryWithProperties(MockAllocationProperties{device->getRootDeviceIndex(), MemoryConstants::pageSize});
    mockKernel->mockKernel->setPrivateSurface(privateSurface, 10);

    executionEnvironment->incRefInternal();
    device.reset(nullptr);
    mockKernel.reset(nullptr);
    executionEnvironment->decRefInternal();
}

TEST(KernelTest, givenAllArgumentsAreStatefulBuffersWhenInitializingThenAllBufferArgsStatefulIsTrue) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));

    std::vector<KernelArgInfo> kernelArgInfo(2);
    kernelArgInfo[0].isBuffer = true;
    kernelArgInfo[1].isBuffer = true;
    kernelArgInfo[0].pureStatefulBufferAccess = true;
    kernelArgInfo[1].pureStatefulBufferAccess = true;

    MockKernelWithInternals kernel{*device};
    kernel.kernelInfo.kernelArgInfo.swap(kernelArgInfo);

    kernel.mockKernel->initialize();
    EXPECT_TRUE(kernel.mockKernel->allBufferArgsStateful);
}

TEST(KernelTest, givenAllArgumentsAreBuffersButNotAllAreStatefulWhenInitializingThenAllBufferArgsStatefulIsFalse) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));

    std::vector<KernelArgInfo> kernelArgInfo(2);
    kernelArgInfo[0].isBuffer = true;
    kernelArgInfo[1].isBuffer = true;
    kernelArgInfo[0].pureStatefulBufferAccess = true;
    kernelArgInfo[1].pureStatefulBufferAccess = false;

    MockKernelWithInternals kernel{*device};
    kernel.kernelInfo.kernelArgInfo.swap(kernelArgInfo);

    kernel.mockKernel->initialize();
    EXPECT_FALSE(kernel.mockKernel->allBufferArgsStateful);
}

TEST(KernelTest, givenNotAllArgumentsAreBuffersButAllBuffersAreStatefulWhenInitializingThenAllBufferArgsStatefulIsTrue) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));

    std::vector<KernelArgInfo> kernelArgInfo(2);
    kernelArgInfo[0].isBuffer = true;
    kernelArgInfo[1].isBuffer = false;
    kernelArgInfo[0].pureStatefulBufferAccess = true;
    kernelArgInfo[1].pureStatefulBufferAccess = false;

    MockKernelWithInternals kernel{*device};
    kernel.kernelInfo.kernelArgInfo.swap(kernelArgInfo);

    kernel.mockKernel->initialize();
    EXPECT_TRUE(kernel.mockKernel->allBufferArgsStateful);
}

TEST(KernelTest, givenKernelRequiringPrivateScratchSpaceWhenGettingSizeForPrivateScratchSpaceThenCorrectSizeIsReturned) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));

    MockKernelWithInternals mockKernel(*device);

    SPatchMediaVFEState mediaVFEstate;
    mediaVFEstate.PerThreadScratchSpace = 512u;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, mediaVFEstate, 0);

    SPatchMediaVFEState mediaVFEstateSlot1;
    mediaVFEstateSlot1.PerThreadScratchSpace = 1024u;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, mediaVFEstateSlot1, 1);

    EXPECT_EQ(1024u, mockKernel.mockKernel->getPrivateScratchSize(device->getRootDeviceIndex()));
}

TEST(KernelTest, givenKernelWithoutMediaVfeStateSlot1WhenGettingSizeForPrivateScratchSpaceThenCorrectSizeIsReturned) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));

    MockKernelWithInternals mockKernel(*device);

    EXPECT_EQ(0u, mockKernel.mockKernel->getPrivateScratchSize(device->getRootDeviceIndex()));
}

TEST(KernelTest, givenKernelWithPatchInfoCollectionEnabledWhenPatchWithImplicitSurfaceCalledThenPatchInfoDataIsCollected) {
    DebugManagerStateRestore restore;
    DebugManager.flags.AddPatchInfoCommentsForAUBDump.set(true);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    MockGraphicsAllocation mockAllocation;
    SPatchAllocateStatelessGlobalMemorySurfaceWithInitialization patchToken{};
    uint64_t crossThreadData = 0;
    EXPECT_EQ(0u, kernel.mockKernel->getPatchInfoDataList().size());
    kernel.mockKernel->patchWithImplicitSurface(&crossThreadData, mockAllocation, device->getDevice(), patchToken);
    EXPECT_EQ(1u, kernel.mockKernel->getPatchInfoDataList().size());
}

TEST(KernelTest, givenKernelWithPatchInfoCollecitonEnabledAndArgumentWithInvalidCrossThreadDataOffsetWhenPatchWithImplicitSurfaceCalledThenPatchInfoDataIsNotCollected) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    MockGraphicsAllocation mockAllocation;
    ArgDescPointer arg;
    uint64_t ptr = 0;
    kernel.mockKernel->patchWithImplicitSurface(&ptr, mockAllocation, device->getDevice(), arg);
    EXPECT_EQ(0u, kernel.mockKernel->getPatchInfoDataList().size());
}

TEST(KernelTest, givenKernelWithPatchInfoCollectionEnabledAndValidArgumentWhenPatchWithImplicitSurfaceCalledThenPatchInfoDataIsCollected) {
    DebugManagerStateRestore restore;
    DebugManager.flags.AddPatchInfoCommentsForAUBDump.set(true);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    MockGraphicsAllocation mockAllocation;
    ArgDescPointer arg;
    arg.stateless = 0;
    uint64_t crossThreadData = 0;
    EXPECT_EQ(0u, kernel.mockKernel->getPatchInfoDataList().size());
    kernel.mockKernel->patchWithImplicitSurface(&crossThreadData, mockAllocation, device->getDevice(), arg);
    EXPECT_EQ(1u, kernel.mockKernel->getPatchInfoDataList().size());
}

TEST(KernelTest, givenKernelWithPatchInfoCollectionDisabledWhenPatchWithImplicitSurfaceCalledThenPatchInfoDataIsNotCollected) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    MockGraphicsAllocation mockAllocation;
    SPatchAllocateStatelessGlobalMemorySurfaceWithInitialization patchToken{};
    uint64_t crossThreadData = 0;
    EXPECT_EQ(0u, kernel.mockKernel->getPatchInfoDataList().size());
    kernel.mockKernel->patchWithImplicitSurface(&crossThreadData, mockAllocation, device->getDevice(), patchToken);
    EXPECT_EQ(0u, kernel.mockKernel->getPatchInfoDataList().size());
}

TEST(KernelTest, givenDefaultKernelWhenItIsCreatedThenItReportsStatelessWrites) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    EXPECT_TRUE(kernel.mockKernel->areStatelessWritesUsed());
}

TEST(KernelTest, givenPolicyWhensetKernelThreadArbitrationPolicyThenExpectedClValueIsReturned) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);
    EXPECT_EQ(CL_SUCCESS, kernel.mockKernel->setKernelThreadArbitrationPolicy(CL_KERNEL_EXEC_INFO_THREAD_ARBITRATION_POLICY_ROUND_ROBIN_INTEL));
    EXPECT_EQ(CL_SUCCESS, kernel.mockKernel->setKernelThreadArbitrationPolicy(CL_KERNEL_EXEC_INFO_THREAD_ARBITRATION_POLICY_OLDEST_FIRST_INTEL));
    EXPECT_EQ(CL_SUCCESS, kernel.mockKernel->setKernelThreadArbitrationPolicy(CL_KERNEL_EXEC_INFO_THREAD_ARBITRATION_POLICY_AFTER_DEPENDENCY_ROUND_ROBIN_INTEL));
    uint32_t notExistPolicy = 0;
    EXPECT_EQ(CL_INVALID_VALUE, kernel.mockKernel->setKernelThreadArbitrationPolicy(notExistPolicy));
}

TEST(KernelTest, GivenDifferentValuesWhenSetKernelExecutionTypeIsCalledThenCorrectValueIsSet) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals mockKernelWithInternals(*device);
    auto &kernel = *mockKernelWithInternals.mockKernel;
    cl_int retVal;

    EXPECT_EQ(KernelExecutionType::Default, kernel.executionType);

    retVal = kernel.setKernelExecutionType(-1);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(KernelExecutionType::Default, kernel.executionType);

    retVal = kernel.setKernelExecutionType(CL_KERNEL_EXEC_INFO_CONCURRENT_TYPE_INTEL);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(KernelExecutionType::Concurrent, kernel.executionType);

    retVal = kernel.setKernelExecutionType(-1);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(KernelExecutionType::Concurrent, kernel.executionType);

    retVal = kernel.setKernelExecutionType(CL_KERNEL_EXEC_INFO_DEFAULT_TYPE_INTEL);
    EXPECT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(KernelExecutionType::Default, kernel.executionType);
}

TEST(KernelTest, givenKernelLocalIdGenerationByRuntimeFalseWhenGettingStartOffsetThenOffsetToSkipPerThreadDataLoadIsAdded) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    auto rootDeviceIndex = device->getRootDeviceIndex();

    MockKernelWithInternals mockKernel(*device);
    SPatchThreadPayload threadPayload = {};

    threadPayload.OffsetToSkipPerThreadDataLoad = 128u;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, threadPayload);

    mockKernel.kernelInfo.createKernelAllocation(device->getDevice(), false);
    auto allocationOffset = mockKernel.kernelInfo.getGraphicsAllocation()->getGpuAddressToPatch();

    mockKernel.mockKernel->setStartOffset(128);
    auto offset = mockKernel.mockKernel->getKernelStartOffset(false, true, false, rootDeviceIndex);
    EXPECT_EQ(allocationOffset + 256u, offset);
    device->getMemoryManager()->freeGraphicsMemory(mockKernel.kernelInfo.getGraphicsAllocation());
}

TEST(KernelTest, givenKernelLocalIdGenerationByRuntimeTrueAndLocalIdsUsedWhenGettingStartOffsetThenOffsetToSkipPerThreadDataLoadIsNotAdded) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    auto rootDeviceIndex = device->getRootDeviceIndex();

    MockKernelWithInternals mockKernel(*device);
    SPatchThreadPayload threadPayload = {};

    threadPayload.OffsetToSkipPerThreadDataLoad = 128u;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, threadPayload);

    mockKernel.kernelInfo.createKernelAllocation(device->getDevice(), false);
    auto allocationOffset = mockKernel.kernelInfo.getGraphicsAllocation()->getGpuAddressToPatch();

    mockKernel.mockKernel->setStartOffset(128);
    auto offset = mockKernel.mockKernel->getKernelStartOffset(true, true, false, rootDeviceIndex);
    EXPECT_EQ(allocationOffset + 128u, offset);
    device->getMemoryManager()->freeGraphicsMemory(mockKernel.kernelInfo.getGraphicsAllocation());
}

TEST(KernelTest, givenKernelLocalIdGenerationByRuntimeFalseAndLocalIdsNotUsedWhenGettingStartOffsetThenOffsetToSkipPerThreadDataLoadIsNotAdded) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    auto rootDeviceIndex = device->getRootDeviceIndex();

    MockKernelWithInternals mockKernel(*device);
    SPatchThreadPayload threadPayload = {};

    threadPayload.OffsetToSkipPerThreadDataLoad = 128u;
    populateKernelDescriptor(mockKernel.kernelInfo.kernelDescriptor, threadPayload);

    mockKernel.kernelInfo.createKernelAllocation(device->getDevice(), false);
    auto allocationOffset = mockKernel.kernelInfo.getGraphicsAllocation()->getGpuAddressToPatch();

    mockKernel.mockKernel->setStartOffset(128);
    auto offset = mockKernel.mockKernel->getKernelStartOffset(false, false, false, rootDeviceIndex);
    EXPECT_EQ(allocationOffset + 128u, offset);
    device->getMemoryManager()->freeGraphicsMemory(mockKernel.kernelInfo.getGraphicsAllocation());
}

TEST(KernelTest, givenKernelWhenForcePerDssBackedBufferProgrammingIsSetThenKernelRequiresPerDssBackedBuffer) {
    DebugManagerStateRestore restore;
    DebugManager.flags.ForcePerDssBackedBufferProgramming.set(true);

    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);

    EXPECT_TRUE(kernel.mockKernel->requiresPerDssBackedBuffer(device->getRootDeviceIndex()));
}

TEST(KernelTest, givenKernelWhenForcePerDssBackedBufferProgrammingIsNotSetThenKernelDoesntRequirePerDssBackedBuffer) {
    auto device = std::make_unique<MockClDevice>(MockDevice::createWithNewExecutionEnvironment<MockDevice>(defaultHwInfo.get()));
    MockKernelWithInternals kernel(*device);

    EXPECT_FALSE(kernel.mockKernel->requiresPerDssBackedBuffer(device->getRootDeviceIndex()));
}

TEST(KernelTest, whenKernelIsInitializedThenThreadArbitrationPolicyIsSetToDefaultValue) {
    UltClDeviceFactory deviceFactory{1, 0};

    SPatchExecutionEnvironment sPatchExecEnv = {};
    sPatchExecEnv.SubgroupIndependentForwardProgressRequired = true;
    MockKernelWithInternals mockKernelWithInternals{*deviceFactory.rootDevices[0], sPatchExecEnv};

    auto &mockKernel = *mockKernelWithInternals.mockKernel;
    auto &hwHelper = HwHelper::get(deviceFactory.rootDevices[0]->getHardwareInfo().platform.eRenderCoreFamily);
    EXPECT_EQ(hwHelper.getDefaultThreadArbitrationPolicy(), mockKernel.threadArbitrationPolicy);
}

TEST(KernelTest, givenKernelWhenSettingAdditinalKernelExecInfoThenCorrectValueIsSet) {
    UltClDeviceFactory deviceFactory{1, 0};
    MockKernelWithInternals mockKernelWithInternals{*deviceFactory.rootDevices[0]};
    mockKernelWithInternals.kernelInfo.kernelDescriptor.kernelAttributes.flags.requiresSubgroupIndependentForwardProgress = true;

    auto &mockKernel = *mockKernelWithInternals.mockKernel;

    mockKernel.setAdditionalKernelExecInfo(123u);
    EXPECT_EQ(123u, mockKernel.getAdditionalKernelExecInfo());
    mockKernel.setAdditionalKernelExecInfo(AdditionalKernelExecInfo::NotApplicable);
    EXPECT_EQ(AdditionalKernelExecInfo::NotApplicable, mockKernel.getAdditionalKernelExecInfo());
}

namespace NEO {

template <typename GfxFamily>
class DeviceQueueHwMock : public DeviceQueueHw<GfxFamily> {
    using BaseClass = DeviceQueueHw<GfxFamily>;

  public:
    using BaseClass::buildSlbDummyCommands;
    using BaseClass::getCSPrefetchSize;
    using BaseClass::getExecutionModelCleanupSectionSize;
    using BaseClass::getMediaStateClearCmdsSize;
    using BaseClass::getMinimumSlbSize;
    using BaseClass::getProfilingEndCmdsSize;
    using BaseClass::getSlbCS;
    using BaseClass::getWaCommandsSize;
    using BaseClass::offsetDsh;

    DeviceQueueHwMock(Context *context, ClDevice *device, cl_queue_properties &properties) : BaseClass(context, device, properties) {
        auto slb = this->getSlbBuffer();
        LinearStream *slbCS = getSlbCS();
        slbCS->replaceBuffer(slb->getUnderlyingBuffer(), slb->getUnderlyingBufferSize()); // reset
    };
};
} // namespace NEO

HWCMDTEST_F(IGFX_GEN8_CORE, DeviceQueueHwTest, whenSlbEndOffsetGreaterThanZeroThenOverwriteOneEnqueue) {
    std::unique_ptr<DeviceQueueHwMock<FamilyType>> mockDeviceQueueHw(new DeviceQueueHwMock<FamilyType>(pContext, device, deviceQueueProperties::minimumProperties[0]));

    auto slb = mockDeviceQueueHw->getSlbBuffer();
    auto commandsSize = mockDeviceQueueHw->getMinimumSlbSize() + mockDeviceQueueHw->getWaCommandsSize();
    auto slbCopy = malloc(slb->getUnderlyingBufferSize());
    memset(slb->getUnderlyingBuffer(), 0xFE, slb->getUnderlyingBufferSize());
    memcpy(slbCopy, slb->getUnderlyingBuffer(), slb->getUnderlyingBufferSize());

    auto igilCmdQueue = reinterpret_cast<IGIL_CommandQueue *>(mockDeviceQueueHw->getQueueBuffer()->getUnderlyingBuffer());

    // slbEndOffset < commandsSize * 128
    // always fill only 1 enqueue (after offset)
    auto offset = static_cast<int>(commandsSize) * 50;
    igilCmdQueue->m_controls.m_SLBENDoffsetInBytes = offset;
    mockDeviceQueueHw->resetDeviceQueue();
    EXPECT_EQ(0, memcmp(slb->getUnderlyingBuffer(), slbCopy, offset)); // dont touch memory before offset
    EXPECT_NE(0, memcmp(ptrOffset(slb->getUnderlyingBuffer(), offset),
                        slbCopy, commandsSize)); // change 1 enqueue
    EXPECT_EQ(0, memcmp(ptrOffset(slb->getUnderlyingBuffer(), offset + commandsSize),
                        slbCopy, offset)); // dont touch memory after (offset + 1 enqueue)

    // slbEndOffset == commandsSize * 128
    // dont fill commands
    memset(slb->getUnderlyingBuffer(), 0xFEFEFEFE, slb->getUnderlyingBufferSize());
    offset = static_cast<int>(commandsSize) * 128;
    igilCmdQueue->m_controls.m_SLBENDoffsetInBytes = static_cast<int>(commandsSize);
    mockDeviceQueueHw->resetDeviceQueue();
    EXPECT_EQ(0, memcmp(slb->getUnderlyingBuffer(), slbCopy, commandsSize * 128)); // dont touch memory for enqueues

    free(slbCopy);
}

using KernelMultiRootDeviceTest = MultiRootDeviceFixture;

TEST_F(KernelMultiRootDeviceTest, givenKernelWithPrivateSurfaceWhenInitializeThenPrivateSurfacesHaveCorrectRootDeviceIndex) {
    auto pKernelInfo = std::make_unique<KernelInfo>();
    pKernelInfo->kernelDescriptor.kernelAttributes.simdSize = 1;

    // setup private memory
    SPatchAllocateStatelessPrivateSurface tokenSPS;
    tokenSPS.SurfaceStateHeapOffset = 64;
    tokenSPS.DataParamOffset = 40;
    tokenSPS.DataParamSize = 8;
    tokenSPS.PerThreadPrivateMemorySize = 112;
    populateKernelDescriptor(pKernelInfo->kernelDescriptor, tokenSPS);

    KernelInfoContainer kernelInfos;
    kernelInfos.resize(deviceFactory->rootDevices.size());
    for (auto &rootDeviceIndex : context->getRootDeviceIndices()) {
        kernelInfos[rootDeviceIndex] = pKernelInfo.get();
    }

    MockProgram program(context.get(), false, context->getDevices());

    int32_t retVal = CL_INVALID_VALUE;
    auto pMultiDeviceKernel = std::unique_ptr<MultiDeviceKernel>(MultiDeviceKernel::create<MockKernel>(&program, kernelInfos, &retVal));

    EXPECT_EQ(CL_SUCCESS, retVal);

    for (auto &rootDeviceIndex : context->getRootDeviceIndices()) {
        auto kernel = static_cast<MockKernel *>(pMultiDeviceKernel->getKernel(rootDeviceIndex));
        auto privateSurface = kernel->kernelDeviceInfos[rootDeviceIndex].privateSurface;
        ASSERT_NE(nullptr, privateSurface);
        EXPECT_EQ(rootDeviceIndex, privateSurface->getRootDeviceIndex());
    }
}

TEST(KernelCreateTest, whenInitFailedThenReturnNull) {
    struct MockProgram {
        ClDeviceVector getDevices() {
            ClDeviceVector deviceVector;
            deviceVector.push_back(&mDevice);
            return deviceVector;
        }
        void getSource(std::string &) {}
        MockClDevice mDevice{new MockDevice};
    } mockProgram;
    struct MockKernel {
        MockKernel(MockProgram *, const KernelInfoContainer &, ClDevice &) {}
        int initialize() { return -1; };
    };

    KernelInfoContainer kernelInfos;
    KernelInfo info;
    info.gpuPointerSize = 8;
    kernelInfos.push_back(&info);

    auto ret = Kernel::create<MockKernel>(&mockProgram, kernelInfos, mockProgram.mDevice, nullptr);
    EXPECT_EQ(nullptr, ret);
}

TEST(ArgTypeTraits, GivenDefaultInitializedArgTypeMetadataThenAddressSpaceIsGlobal) {
    ArgTypeTraits metadata;
    EXPECT_EQ(NEO::KernelArgMetadata::AddrGlobal, metadata.addressQualifier);
}
