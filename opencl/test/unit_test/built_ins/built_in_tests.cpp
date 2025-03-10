/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/gmm_helper/gmm.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/helpers/file_io.h"
#include "shared/source/helpers/hash.h"
#include "shared/source/helpers/string.h"
#include "shared/test/common/helpers/debug_manager_state_restore.h"
#include "shared/test/common/mocks/mock_compiler_interface.h"
#include "shared/test/unit_test/utilities/base_object_utils.h"

#include "opencl/source/built_ins/aux_translation_builtin.h"
#include "opencl/source/built_ins/builtins_dispatch_builder.h"
#include "opencl/source/built_ins/vme_builtin.h"
#include "opencl/source/built_ins/vme_dispatch_builder.h"
#include "opencl/source/helpers/dispatch_info_builder.h"
#include "opencl/source/kernel/kernel.h"
#include "opencl/test/unit_test/built_ins/built_ins_file_names.h"
#include "opencl/test/unit_test/fixtures/built_in_fixture.h"
#include "opencl/test/unit_test/fixtures/cl_device_fixture.h"
#include "opencl/test/unit_test/fixtures/context_fixture.h"
#include "opencl/test/unit_test/fixtures/image_fixture.h"
#include "opencl/test/unit_test/fixtures/run_kernel_fixture.h"
#include "opencl/test/unit_test/global_environment.h"
#include "opencl/test/unit_test/mocks/mock_buffer.h"
#include "opencl/test/unit_test/mocks/mock_builtins.h"
#include "opencl/test/unit_test/mocks/mock_command_queue.h"
#include "opencl/test/unit_test/mocks/mock_compilers.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_memory_manager.h"
#include "opencl/test/unit_test/test_macros/test_checks_ocl.h"
#include "test.h"

#include "compiler_options.h"
#include "gtest/gtest.h"
#include "os_inc.h"
#include "test_traits_common.h"

#include <string>

using namespace NEO;

class BuiltInTests
    : public BuiltInFixture,
      public ClDeviceFixture,
      public ContextFixture,
      public ::testing::Test {

    using BuiltInFixture::SetUp;
    using ContextFixture::SetUp;

  public:
    BuiltInTests() {
        // reserving space here to avoid the appearance of a memory management
        // leak being reported
        allBuiltIns.reserve(5000);
    }

    void SetUp() override {
        DebugManager.flags.ForceAuxTranslationMode.set(static_cast<int32_t>(AuxTranslationMode::Builtin));
        ClDeviceFixture::SetUp();
        cl_device_id device = pClDevice;
        ContextFixture::SetUp(1, &device);
        BuiltInFixture::SetUp(pDevice);
    }

    void TearDown() override {
        allBuiltIns.clear();
        BuiltInFixture::TearDown();
        ContextFixture::TearDown();
        ClDeviceFixture::TearDown();
    }

    void AppendBuiltInStringFromFile(std::string builtInFile, size_t &size) {
        std::string src;
        auto pData = loadDataFromFile(
            builtInFile.c_str(),
            size);

        ASSERT_NE(nullptr, pData);

        src = (const char *)pData.get();
        size_t start = src.find("R\"===(");
        size_t stop = src.find(")===\"");

        // assert that pattern was found
        ASSERT_NE(std::string::npos, start);
        ASSERT_NE(std::string::npos, stop);

        start += strlen("R\"===(");
        size = stop - start;
        allBuiltIns.append(src, start, size);
    }

    bool compareBuiltinOpParams(const BuiltinOpParams &left,
                                const BuiltinOpParams &right) {
        return left.srcPtr == right.srcPtr &&
               left.dstPtr == right.dstPtr &&
               left.size == right.size &&
               left.srcOffset == right.srcOffset &&
               left.dstOffset == right.dstOffset &&
               left.dstMemObj == right.dstMemObj &&
               left.srcMemObj == right.srcMemObj;
    }

    DebugManagerStateRestore restore;
    std::string allBuiltIns;
};

struct VmeBuiltInTests : BuiltInTests {
    void SetUp() override {
        BuiltInTests::SetUp();
        if (!pDevice->getHardwareInfo().capabilityTable.supportsVme) {
            GTEST_SKIP();
        }
    }
};

struct AuxBuiltInTests : BuiltInTests, public ::testing::WithParamInterface<KernelObjForAuxTranslation::Type> {
    void SetUp() override {
        BuiltInTests::SetUp();
        kernelObjType = GetParam();
    }
    KernelObjForAuxTranslation::Type kernelObjType;
};

struct AuxBuiltinsMatcher {
    template <PRODUCT_FAMILY productFamily>
    static constexpr bool isMatched() {
        return TestTraits<NEO::ToGfxCoreFamily<productFamily>::get()>::auxBuiltinsSupported;
    }
};

INSTANTIATE_TEST_CASE_P(,
                        AuxBuiltInTests,
                        testing::ValuesIn({KernelObjForAuxTranslation::Type::MEM_OBJ, KernelObjForAuxTranslation::Type::GFX_ALLOC}));

TEST_F(BuiltInTests, WhenBuildingListOfBuiltinsThenBuiltinsHaveBeenGenerated) {
    for (auto supportsImages : ::testing::Bool()) {
        allBuiltIns.clear();
        size_t size = 0;

        for (auto &fileName : getBuiltInFileNames(supportsImages)) {
            AppendBuiltInStringFromFile(fileName, size);
            ASSERT_NE(0u, size);
        }

        // convert /r/n to /n
        size_t start_pos = 0;
        while ((start_pos = allBuiltIns.find("\r\n", start_pos)) != std::string::npos) {
            allBuiltIns.replace(start_pos, 2, "\n");
        }

        // convert /r to /n
        start_pos = 0;
        while ((start_pos = allBuiltIns.find("\r", start_pos)) != std::string::npos) {
            allBuiltIns.replace(start_pos, 1, "\n");
        }

        uint64_t hash = Hash::hash(allBuiltIns.c_str(), allBuiltIns.length());
        auto hashName = getBuiltInHashFileName(hash, supportsImages);

        //First fail, if we are inconsistent
        EXPECT_EQ(true, fileExists(hashName)) << "**********\nBuilt in kernels need to be regenerated for the mock compilers!\n**********";

        //then write to file if needed
#define GENERATE_NEW_HASH_FOR_BUILT_INS 0
#if GENERATE_NEW_HASH_FOR_BUILT_INS
        std::cout << "writing builtins to file: " << hashName << std::endl;
        const char *pData = allBuiltIns.c_str();
        writeDataToFile(hashName.c_str(), pData, allBuiltIns.length());
#endif
    }
}

TEST_F(BuiltInTests, GivenCopyBufferToBufferWhenDispatchInfoIsCreatedThenParamsAreCorrect) {
    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);

    MockBuffer *srcPtr = new MockBuffer();
    MockBuffer *dstPtr = new MockBuffer();

    MockBuffer &src = *srcPtr;
    MockBuffer &dst = *dstPtr;

    BuiltinOpParams builtinOpsParams;

    builtinOpsParams.srcMemObj = &src;
    builtinOpsParams.dstMemObj = &dst;
    builtinOpsParams.srcPtr = src.getCpuAddress();
    builtinOpsParams.dstPtr = dst.getCpuAddress();
    builtinOpsParams.size = {dst.getSize(), 0, 0};

    MultiDispatchInfo multiDispatchInfo(builtinOpsParams);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));

    size_t leftSize = reinterpret_cast<uintptr_t>(dst.getCpuAddress()) % MemoryConstants::cacheLineSize;
    if (leftSize > 0) {
        leftSize = MemoryConstants::cacheLineSize - leftSize;
    }
    size_t rightSize = (reinterpret_cast<uintptr_t>(dst.getCpuAddress()) + dst.getSize()) % MemoryConstants::cacheLineSize;
    size_t middleSize = (dst.getSize() - leftSize - rightSize) / (sizeof(uint32_t) * 4);

    int i = 0;
    int leftKernel = 0;
    int middleKernel = 0;
    int rightKernel = 0;

    if (leftSize > 0) {
        middleKernel++;
        rightKernel++;
    } else {
        leftKernel = -1;
    }
    if (middleSize > 0) {
        rightKernel++;
    } else {
        middleKernel = -1;
    }
    if (rightSize == 0) {
        rightKernel = -1;
    }

    for (auto &dispatchInfo : multiDispatchInfo) {
        EXPECT_EQ(1u, dispatchInfo.getDim());
        if (i == leftKernel) {
            EXPECT_EQ(Vec3<size_t>(leftSize, 1, 1), dispatchInfo.getGWS());
        } else if (i == middleKernel) {
            EXPECT_EQ(Vec3<size_t>(middleSize, 1, 1), dispatchInfo.getGWS());
        } else if (i == rightKernel) {
            EXPECT_EQ(Vec3<size_t>(rightSize, 1, 1), dispatchInfo.getGWS());
        }
        i++;
    }
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
    delete srcPtr;
    delete dstPtr;
}

HWTEST2_P(AuxBuiltInTests, givenInputBufferWhenBuildingNonAuxDispatchInfoForAuxTranslationThenPickAndSetupCorrectKernels, AuxBuiltinsMatcher) {
    BuiltinDispatchInfoBuilder &baseBuilder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::AuxTranslation, *pClDevice);
    auto &builder = static_cast<BuiltInOp<EBuiltInOps::AuxTranslation> &>(baseBuilder);

    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    MultiDispatchInfo multiDispatchInfo;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);
    std::vector<Kernel *> builtinKernels;
    std::vector<MockKernelObjForAuxTranslation> mockKernelObjForAuxTranslation;
    mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType, 0x1000));
    mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType, 0x20000));
    mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType, 0x30000));

    BuiltinOpParams builtinOpsParams;
    builtinOpsParams.auxTranslationDirection = AuxTranslationDirection::AuxToNonAux;

    for (auto &kernelObj : mockKernelObjForAuxTranslation) {
        kernelObjsForAuxTranslation.insert(kernelObj);
    }

    EXPECT_TRUE(builder.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpsParams));
    EXPECT_EQ(3u, multiDispatchInfo.size());

    for (auto &dispatchInfo : multiDispatchInfo) {
        auto kernel = dispatchInfo.getKernel();
        builtinKernels.push_back(kernel);

        if (kernelObjType == KernelObjForAuxTranslation::Type::MEM_OBJ) {
            auto buffer = castToObject<Buffer>(kernel->getKernelArguments().at(0).object);
            auto kernelObj = *kernelObjsForAuxTranslation.find({KernelObjForAuxTranslation::Type::MEM_OBJ, buffer});
            EXPECT_NE(nullptr, kernelObj.object);
            EXPECT_EQ(KernelObjForAuxTranslation::Type::MEM_OBJ, kernelObj.type);
            kernelObjsForAuxTranslation.erase(kernelObj);

            cl_mem clMem = buffer;
            EXPECT_EQ(clMem, kernel->getKernelArguments().at(0).object);
            EXPECT_EQ(clMem, kernel->getKernelArguments().at(1).object);

            EXPECT_EQ(1u, dispatchInfo.getDim());
            size_t xGws = alignUp(buffer->getSize(), 512) / 16;
            Vec3<size_t> gws = {xGws, 1, 1};
            EXPECT_EQ(gws, dispatchInfo.getGWS());
        } else {
            auto gfxAllocation = static_cast<GraphicsAllocation *>(kernel->getKernelArguments().at(0).object);
            auto kernelObj = *kernelObjsForAuxTranslation.find({KernelObjForAuxTranslation::Type::GFX_ALLOC, gfxAllocation});
            EXPECT_NE(nullptr, kernelObj.object);
            EXPECT_EQ(KernelObjForAuxTranslation::Type::GFX_ALLOC, kernelObj.type);
            kernelObjsForAuxTranslation.erase(kernelObj);

            EXPECT_EQ(gfxAllocation, kernel->getKernelArguments().at(0).object);
            EXPECT_EQ(gfxAllocation, kernel->getKernelArguments().at(1).object);

            EXPECT_EQ(1u, dispatchInfo.getDim());
            size_t xGws = alignUp(gfxAllocation->getUnderlyingBufferSize(), 512) / 16;
            Vec3<size_t> gws = {xGws, 1, 1};
            EXPECT_EQ(gws, dispatchInfo.getGWS());
        }
    }
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
    // always pick different kernel
    EXPECT_EQ(3u, builtinKernels.size());
    EXPECT_NE(builtinKernels[0], builtinKernels[1]);
    EXPECT_NE(builtinKernels[0], builtinKernels[2]);
    EXPECT_NE(builtinKernels[1], builtinKernels[2]);
}

HWTEST2_P(AuxBuiltInTests, givenInputBufferWhenBuildingAuxDispatchInfoForAuxTranslationThenPickAndSetupCorrectKernels, AuxBuiltinsMatcher) {
    BuiltinDispatchInfoBuilder &baseBuilder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::AuxTranslation, *pClDevice);
    auto &builder = static_cast<BuiltInOp<EBuiltInOps::AuxTranslation> &>(baseBuilder);

    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    MultiDispatchInfo multiDispatchInfo;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);
    std::vector<Kernel *> builtinKernels;
    std::vector<MockKernelObjForAuxTranslation> mockKernelObjForAuxTranslation;
    mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType, 0x1000));
    mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType, 0x20000));
    mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType, 0x30000));

    BuiltinOpParams builtinOpsParams;
    builtinOpsParams.auxTranslationDirection = AuxTranslationDirection::NonAuxToAux;

    for (auto &kernelObj : mockKernelObjForAuxTranslation) {
        kernelObjsForAuxTranslation.insert(kernelObj);
    }

    EXPECT_TRUE(builder.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpsParams));
    EXPECT_EQ(3u, multiDispatchInfo.size());

    for (auto &dispatchInfo : multiDispatchInfo) {
        auto kernel = dispatchInfo.getKernel();
        builtinKernels.push_back(kernel);

        if (kernelObjType == KernelObjForAuxTranslation::Type::MEM_OBJ) {
            auto buffer = castToObject<Buffer>(kernel->getKernelArguments().at(0).object);
            auto kernelObj = *kernelObjsForAuxTranslation.find({KernelObjForAuxTranslation::Type::MEM_OBJ, buffer});
            EXPECT_NE(nullptr, kernelObj.object);
            EXPECT_EQ(KernelObjForAuxTranslation::Type::MEM_OBJ, kernelObj.type);
            kernelObjsForAuxTranslation.erase(kernelObj);

            cl_mem clMem = buffer;
            EXPECT_EQ(clMem, kernel->getKernelArguments().at(0).object);
            EXPECT_EQ(clMem, kernel->getKernelArguments().at(1).object);

            EXPECT_EQ(1u, dispatchInfo.getDim());
            size_t xGws = alignUp(buffer->getSize(), 4) / 4;
            Vec3<size_t> gws = {xGws, 1, 1};
            EXPECT_EQ(gws, dispatchInfo.getGWS());
        } else {
            auto gfxAllocation = static_cast<GraphicsAllocation *>(kernel->getKernelArguments().at(0).object);
            auto kernelObj = *kernelObjsForAuxTranslation.find({KernelObjForAuxTranslation::Type::GFX_ALLOC, gfxAllocation});
            EXPECT_NE(nullptr, kernelObj.object);
            EXPECT_EQ(KernelObjForAuxTranslation::Type::GFX_ALLOC, kernelObj.type);
            kernelObjsForAuxTranslation.erase(kernelObj);

            EXPECT_EQ(gfxAllocation, kernel->getKernelArguments().at(0).object);
            EXPECT_EQ(gfxAllocation, kernel->getKernelArguments().at(1).object);

            EXPECT_EQ(1u, dispatchInfo.getDim());
            size_t xGws = alignUp(gfxAllocation->getUnderlyingBufferSize(), 512) / 16;
            Vec3<size_t> gws = {xGws, 1, 1};
            EXPECT_EQ(gws, dispatchInfo.getGWS());
        }
    }
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
    // always pick different kernel
    EXPECT_EQ(3u, builtinKernels.size());
    EXPECT_NE(builtinKernels[0], builtinKernels[1]);
    EXPECT_NE(builtinKernels[0], builtinKernels[2]);
    EXPECT_NE(builtinKernels[1], builtinKernels[2]);
}

HWTEST2_P(AuxBuiltInTests, givenInputBufferWhenBuildingAuxTranslationDispatchThenPickDifferentKernelsDependingOnRequest, AuxBuiltinsMatcher) {
    BuiltinDispatchInfoBuilder &baseBuilder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::AuxTranslation, *pClDevice);
    auto &builder = static_cast<BuiltInOp<EBuiltInOps::AuxTranslation> &>(baseBuilder);

    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    std::vector<MockKernelObjForAuxTranslation> mockKernelObjForAuxTranslation;
    for (int i = 0; i < 3; i++) {
        mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType));
    }
    std::vector<Kernel *> builtinKernels;

    MultiDispatchInfo multiDispatchInfo;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);
    BuiltinOpParams builtinOpsParams;

    for (auto &kernelObj : mockKernelObjForAuxTranslation) {
        kernelObjsForAuxTranslation.insert(kernelObj);
    }

    builtinOpsParams.auxTranslationDirection = AuxTranslationDirection::AuxToNonAux;
    EXPECT_TRUE(builder.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpsParams));

    builtinOpsParams.auxTranslationDirection = AuxTranslationDirection::NonAuxToAux;
    EXPECT_TRUE(builder.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpsParams));

    EXPECT_EQ(6u, multiDispatchInfo.size());

    for (auto &dispatchInfo : multiDispatchInfo) {
        builtinKernels.push_back(dispatchInfo.getKernel());
    }
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
    // nonAux vs Aux instance
    EXPECT_EQ(6u, builtinKernels.size());
    EXPECT_NE(builtinKernels[0], builtinKernels[3]);
    EXPECT_NE(builtinKernels[1], builtinKernels[4]);
    EXPECT_NE(builtinKernels[2], builtinKernels[5]);
}

HWTEST2_P(AuxBuiltInTests, givenInvalidAuxTranslationDirectionWhenBuildingDispatchInfosThenAbort, AuxBuiltinsMatcher) {
    BuiltinDispatchInfoBuilder &baseBuilder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::AuxTranslation, *pClDevice);
    auto &builder = static_cast<BuiltInOp<EBuiltInOps::AuxTranslation> &>(baseBuilder);

    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    MockKernelObjForAuxTranslation mockKernelObjForAuxTranslation(kernelObjType);

    MultiDispatchInfo multiDispatchInfo;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);
    BuiltinOpParams builtinOpsParams;

    kernelObjsForAuxTranslation.insert(mockKernelObjForAuxTranslation);

    builtinOpsParams.auxTranslationDirection = AuxTranslationDirection::None;
    EXPECT_THROW(builder.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpsParams), std::exception);
}

class MockAuxBuilInOp : public BuiltInOp<EBuiltInOps::AuxTranslation> {
  public:
    using BuiltinDispatchInfoBuilder::populate;
    using BaseClass = BuiltInOp<EBuiltInOps::AuxTranslation>;
    using BaseClass::baseKernel;
    using BaseClass::convertToAuxKernel;
    using BaseClass::convertToNonAuxKernel;
    using BaseClass::resizeKernelInstances;
    using BaseClass::usedKernels;

    using BaseClass::BuiltInOp;
};

TEST_F(BuiltInTests, whenAuxBuiltInIsConstructedThenResizeKernelInstancedTo5) {
    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    EXPECT_EQ(5u, mockAuxBuiltInOp.convertToAuxKernel.size());
    EXPECT_EQ(5u, mockAuxBuiltInOp.convertToNonAuxKernel.size());
}

HWTEST2_P(AuxBuiltInTests, givenMoreKernelObjectsForAuxTranslationThanKernelInstancesWhenDispatchingThenResize, AuxBuiltinsMatcher) {
    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    EXPECT_EQ(5u, mockAuxBuiltInOp.convertToAuxKernel.size());
    EXPECT_EQ(5u, mockAuxBuiltInOp.convertToNonAuxKernel.size());

    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    BuiltinOpParams builtinOpsParams;
    MultiDispatchInfo multiDispatchInfo;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);
    std::vector<MockKernelObjForAuxTranslation> mockKernelObjForAuxTranslation;
    for (int i = 0; i < 7; i++) {
        mockKernelObjForAuxTranslation.push_back(MockKernelObjForAuxTranslation(kernelObjType));
    }

    builtinOpsParams.auxTranslationDirection = AuxTranslationDirection::AuxToNonAux;

    for (auto &kernelObj : mockKernelObjForAuxTranslation) {
        kernelObjsForAuxTranslation.insert(kernelObj);
    }

    EXPECT_TRUE(mockAuxBuiltInOp.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpsParams));
    EXPECT_EQ(7u, mockAuxBuiltInOp.convertToAuxKernel.size());
    EXPECT_EQ(7u, mockAuxBuiltInOp.convertToNonAuxKernel.size());
}

TEST_F(BuiltInTests, givenkAuxBuiltInWhenResizeIsCalledThenCloneAllNewInstancesFromBaseKernel) {
    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    size_t newSize = mockAuxBuiltInOp.convertToAuxKernel.size() + 3;
    mockAuxBuiltInOp.resizeKernelInstances(newSize);

    EXPECT_EQ(newSize, mockAuxBuiltInOp.convertToAuxKernel.size());
    for (auto &convertToAuxKernel : mockAuxBuiltInOp.convertToAuxKernel) {
        EXPECT_EQ(&mockAuxBuiltInOp.baseKernel->getKernelInfo(rootDeviceIndex), &convertToAuxKernel->getKernelInfo(rootDeviceIndex));
    }

    EXPECT_EQ(newSize, mockAuxBuiltInOp.convertToNonAuxKernel.size());
    for (auto &convertToNonAuxKernel : mockAuxBuiltInOp.convertToNonAuxKernel) {
        EXPECT_EQ(&mockAuxBuiltInOp.baseKernel->getKernelInfo(rootDeviceIndex), &convertToNonAuxKernel->getKernelInfo(rootDeviceIndex));
    }
}

HWTEST2_P(AuxBuiltInTests, givenKernelWithAuxTranslationRequiredWhenEnqueueCalledThenLockOnBuiltin, AuxBuiltinsMatcher) {
    BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::AuxTranslation, *pClDevice);
    auto mockAuxBuiltInOp = new MockAuxBuilInOp(*pBuiltIns, *pClDevice);
    pBuiltIns->BuiltinOpsBuilders[static_cast<uint32_t>(EBuiltInOps::AuxTranslation)].first.reset(mockAuxBuiltInOp);

    auto mockProgram = clUniquePtr(new MockProgram(toClDeviceVector(*pClDevice)));
    auto mockBuiltinKernel = MockKernel::create(*pDevice, mockProgram.get());
    auto pMultiDeviceKernel = new MockMultiDeviceKernel(MockMultiDeviceKernel::toKernelVector(mockBuiltinKernel));
    mockAuxBuiltInOp->usedKernels.at(0).reset(pMultiDeviceKernel);

    MockKernelWithInternals mockKernel(*pClDevice, pContext);
    MockCommandQueueHw<FamilyType> cmdQ(pContext, pClDevice, nullptr);
    size_t gws[3] = {1, 0, 0};

    mockKernel.kernelInfo.kernelArgInfo.resize(1);
    mockKernel.kernelInfo.kernelArgInfo.at(0).kernelArgPatchInfoVector.resize(1);
    mockKernel.kernelInfo.kernelArgInfo.at(0).pureStatefulBufferAccess = false;
    mockKernel.mockKernel->initialize();

    MockKernelObjForAuxTranslation mockKernelObjForAuxTranslation(kernelObjType);
    if (kernelObjType == KernelObjForAuxTranslation::Type::MEM_OBJ) {
        cl_mem clMem = mockKernelObjForAuxTranslation.mockBuffer.get();
        mockKernel.mockKernel->setArgBuffer(0, sizeof(cl_mem *), &clMem);
    } else {
        auto gfxAllocation = mockKernelObjForAuxTranslation.mockGraphicsAllocation.get();
        gfxAllocation->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
        auto ptr = reinterpret_cast<void *>(gfxAllocation->getGpuAddressToPatch());
        mockKernel.mockKernel->setArgSvmAlloc(0, ptr, gfxAllocation);
    }

    mockKernel.mockKernel->auxTranslationRequired = false;
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(0u, pMultiDeviceKernel->takeOwnershipCalls);
    EXPECT_EQ(0u, pMultiDeviceKernel->releaseOwnershipCalls);

    mockKernel.mockKernel->auxTranslationRequired = true;
    cmdQ.enqueueKernel(mockKernel.mockKernel, 1, nullptr, gws, nullptr, 0, nullptr, nullptr);

    EXPECT_EQ(1u, pMultiDeviceKernel->takeOwnershipCalls);
    EXPECT_EQ(1u, pMultiDeviceKernel->releaseOwnershipCalls);
}

HWCMDTEST_P(IGFX_GEN8_CORE, AuxBuiltInTests, givenAuxTranslationKernelWhenSettingKernelArgsThenSetValidMocs) {
    if (this->pDevice->areSharedSystemAllocationsAllowed()) {
        GTEST_SKIP();
    }

    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;

    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    MultiDispatchInfo multiDispatchInfo;
    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);

    BuiltinOpParams builtinOpParamsToAux;
    builtinOpParamsToAux.auxTranslationDirection = AuxTranslationDirection::NonAuxToAux;

    BuiltinOpParams builtinOpParamsToNonAux;
    builtinOpParamsToNonAux.auxTranslationDirection = AuxTranslationDirection::AuxToNonAux;

    std::unique_ptr<Buffer> buffer = nullptr;
    std::unique_ptr<GraphicsAllocation> gfxAllocation = nullptr;

    if (kernelObjType == MockKernelObjForAuxTranslation::Type::MEM_OBJ) {
        cl_int retVal = CL_SUCCESS;
        buffer.reset(Buffer::create(pContext, 0, MemoryConstants::pageSize, nullptr, retVal));
        kernelObjsForAuxTranslation.insert({KernelObjForAuxTranslation::Type::MEM_OBJ, buffer.get()});
    } else {
        gfxAllocation.reset(new MockGraphicsAllocation(nullptr, MemoryConstants::pageSize));
        kernelObjsForAuxTranslation.insert({KernelObjForAuxTranslation::Type::GFX_ALLOC, gfxAllocation.get()});
    }

    mockAuxBuiltInOp.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpParamsToAux);
    mockAuxBuiltInOp.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpParamsToNonAux);

    {
        // read args
        auto argNum = 0;
        auto expectedMocs = pDevice->getGmmHelper()->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER_CACHELINE_MISALIGNED);
        auto sshBase = mockAuxBuiltInOp.convertToAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        auto sshOffset = mockAuxBuiltInOp.convertToAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        auto surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(expectedMocs, surfaceState->getMemoryObjectControlState());

        sshBase = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        sshOffset = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(expectedMocs, surfaceState->getMemoryObjectControlState());
    }

    {
        // write args
        auto argNum = 1;
        auto expectedMocs = pDevice->getGmmHelper()->getMOCS(GMM_RESOURCE_USAGE_OCL_BUFFER);
        auto sshBase = mockAuxBuiltInOp.convertToAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        auto sshOffset = mockAuxBuiltInOp.convertToAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        auto surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(expectedMocs, surfaceState->getMemoryObjectControlState());

        sshBase = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        sshOffset = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(expectedMocs, surfaceState->getMemoryObjectControlState());
    }
}

HWTEST2_P(AuxBuiltInTests, givenAuxToNonAuxTranslationWhenSettingSurfaceStateThenSetValidAuxMode, AuxBuiltinsMatcher) {
    if (this->pDevice->areSharedSystemAllocationsAllowed()) {
        GTEST_SKIP();
    }

    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    using AUXILIARY_SURFACE_MODE = typename RENDER_SURFACE_STATE::AUXILIARY_SURFACE_MODE;

    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    MultiDispatchInfo multiDispatchInfo;
    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);

    BuiltinOpParams builtinOpParams;
    builtinOpParams.auxTranslationDirection = AuxTranslationDirection::AuxToNonAux;

    std::unique_ptr<Buffer> buffer = nullptr;
    std::unique_ptr<GraphicsAllocation> gfxAllocation = nullptr;

    auto gmm = std::unique_ptr<Gmm>(new Gmm(pDevice->getGmmClientContext(), nullptr, 1, false));
    gmm->isRenderCompressed = true;

    if (kernelObjType == MockKernelObjForAuxTranslation::Type::MEM_OBJ) {
        cl_int retVal = CL_SUCCESS;
        buffer.reset(Buffer::create(pContext, 0, MemoryConstants::pageSize, nullptr, retVal));
        buffer->getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
        buffer->getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setDefaultGmm(gmm.release());

        kernelObjsForAuxTranslation.insert({KernelObjForAuxTranslation::Type::MEM_OBJ, buffer.get()});
    } else {
        gfxAllocation.reset(new MockGraphicsAllocation(nullptr, MemoryConstants::pageSize));
        gfxAllocation->setAllocationType(GraphicsAllocation::AllocationType::BUFFER_COMPRESSED);
        gfxAllocation->setDefaultGmm(gmm.get());

        kernelObjsForAuxTranslation.insert({KernelObjForAuxTranslation::Type::GFX_ALLOC, gfxAllocation.get()});
    }

    mockAuxBuiltInOp.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpParams);

    {
        // read arg
        auto argNum = 0;
        auto sshBase = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        auto sshOffset = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        auto surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_CCS_E, surfaceState->getAuxiliarySurfaceMode());
    }

    {
        // write arg
        auto argNum = 1;
        auto sshBase = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        auto sshOffset = mockAuxBuiltInOp.convertToNonAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        auto surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_NONE, surfaceState->getAuxiliarySurfaceMode());
    }
}

HWTEST2_P(AuxBuiltInTests, givenNonAuxToAuxTranslationWhenSettingSurfaceStateThenSetValidAuxMode, AuxBuiltinsMatcher) {
    if (this->pDevice->areSharedSystemAllocationsAllowed()) {
        GTEST_SKIP();
    }

    using RENDER_SURFACE_STATE = typename FamilyType::RENDER_SURFACE_STATE;
    using AUXILIARY_SURFACE_MODE = typename RENDER_SURFACE_STATE::AUXILIARY_SURFACE_MODE;

    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    MultiDispatchInfo multiDispatchInfo;
    KernelObjsForAuxTranslation kernelObjsForAuxTranslation;
    multiDispatchInfo.setKernelObjsForAuxTranslation(kernelObjsForAuxTranslation);

    BuiltinOpParams builtinOpParams;
    builtinOpParams.auxTranslationDirection = AuxTranslationDirection::NonAuxToAux;

    MockKernelObjForAuxTranslation mockKernelObjForAuxTranslation(kernelObjType);
    auto gmm = std::unique_ptr<Gmm>(new Gmm(pDevice->getGmmClientContext(), nullptr, 1, false));
    gmm->isRenderCompressed = true;
    if (kernelObjType == MockKernelObjForAuxTranslation::Type::MEM_OBJ) {
        mockKernelObjForAuxTranslation.mockBuffer->getGraphicsAllocation(pClDevice->getRootDeviceIndex())->setDefaultGmm(gmm.get());
    } else {
        mockKernelObjForAuxTranslation.mockGraphicsAllocation->setDefaultGmm(gmm.get());
    }
    kernelObjsForAuxTranslation.insert(mockKernelObjForAuxTranslation);

    mockAuxBuiltInOp.buildDispatchInfosForAuxTranslation<FamilyType>(multiDispatchInfo, builtinOpParams);

    {
        // read arg
        auto argNum = 0;
        auto sshBase = mockAuxBuiltInOp.convertToAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        auto sshOffset = mockAuxBuiltInOp.convertToAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        auto surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_NONE, surfaceState->getAuxiliarySurfaceMode());
    }

    {
        // write arg
        auto argNum = 1;
        auto sshBase = mockAuxBuiltInOp.convertToAuxKernel[0]->getSurfaceStateHeap(rootDeviceIndex);
        auto sshOffset = mockAuxBuiltInOp.convertToAuxKernel[0]->getKernelInfo(rootDeviceIndex).kernelArgInfo[argNum].offsetHeap;
        auto surfaceState = reinterpret_cast<RENDER_SURFACE_STATE *>(ptrOffset(sshBase, sshOffset));
        EXPECT_EQ(AUXILIARY_SURFACE_MODE::AUXILIARY_SURFACE_MODE_AUX_CCS_E, surfaceState->getAuxiliarySurfaceMode());
    }
}

TEST_F(BuiltInTests, GivenCopyBufferToBufferWhenDispatchInfoIsCreatedThenSizeIsAlignedToCachLineSize) {
    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);

    AlignedBuffer src;
    AlignedBuffer dst;

    BuiltinOpParams builtinOpsParams;

    builtinOpsParams.srcMemObj = &src;
    builtinOpsParams.dstMemObj = &dst;
    builtinOpsParams.size = {src.getSize(), 0, 0};

    MultiDispatchInfo multiDispatchInfo(builtinOpsParams);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));

    EXPECT_EQ(1u, multiDispatchInfo.size());

    const DispatchInfo *dispatchInfo = multiDispatchInfo.begin();

    EXPECT_EQ(1u, dispatchInfo->getDim());

    size_t leftSize = reinterpret_cast<uintptr_t>(dst.getCpuAddress()) % MemoryConstants::cacheLineSize;
    EXPECT_EQ(0u, leftSize);

    size_t rightSize = (reinterpret_cast<uintptr_t>(dst.getCpuAddress()) + dst.getSize()) % MemoryConstants::cacheLineSize;
    EXPECT_EQ(0u, rightSize);

    size_t middleElSize = sizeof(uint32_t) * 4;
    size_t middleSize = dst.getSize() / middleElSize;
    EXPECT_EQ(Vec3<size_t>(middleSize, 1, 1), dispatchInfo->getGWS());

    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
}

TEST_F(BuiltInTests, givenBigOffsetAndSizeWhenBuilderCopyBufferToBufferStatelessIsUsedThenParamsAreCorrect) {

    if (is32bit) {
        GTEST_SKIP();
    }

    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBufferStateless, *pClDevice);

    uint64_t bigSize = 10ull * MemoryConstants::gigaByte;
    uint64_t bigOffset = 4ull * MemoryConstants::gigaByte;
    uint64_t size = 4ull * MemoryConstants::gigaByte;

    MockBuffer srcBuffer;
    srcBuffer.size = static_cast<size_t>(bigSize);
    MockBuffer dstBuffer;
    dstBuffer.size = static_cast<size_t>(bigSize);

    BuiltinOpParams builtinOpsParams;

    builtinOpsParams.srcMemObj = &srcBuffer;
    builtinOpsParams.srcOffset = {static_cast<size_t>(bigOffset), 0, 0};
    builtinOpsParams.dstMemObj = &dstBuffer;
    builtinOpsParams.dstOffset = {0, 0, 0};
    builtinOpsParams.size = {static_cast<size_t>(size), 0, 0};

    MultiDispatchInfo multiDispatchInfo(builtinOpsParams);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));
    EXPECT_EQ(1u, multiDispatchInfo.size());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
}

TEST_F(BuiltInTests, givenBigOffsetAndSizeWhenBuilderCopyBufferToBufferRectStatelessIsUsedThenParamsAreCorrect) {

    if (is32bit) {
        GTEST_SKIP();
    }

    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferRectStateless, *pClDevice);

    uint64_t bigSize = 10ull * MemoryConstants::gigaByte;
    uint64_t bigOffset = 4ull * MemoryConstants::gigaByte;
    uint64_t size = 4ull * MemoryConstants::gigaByte;

    MockBuffer srcBuffer;
    srcBuffer.size = static_cast<size_t>(bigSize);
    MockBuffer dstBuffer;
    dstBuffer.size = static_cast<size_t>(bigSize);

    BuiltinOpParams dc;
    dc.srcMemObj = &srcBuffer;
    dc.dstMemObj = &dstBuffer;
    dc.srcOffset = {static_cast<size_t>(bigOffset), 0, 0};
    dc.dstOffset = {0, 0, 0};
    dc.size = {static_cast<size_t>(size), 1, 1};
    dc.srcRowPitch = static_cast<size_t>(size);
    dc.srcSlicePitch = 0;
    dc.dstRowPitch = static_cast<size_t>(size);
    dc.dstSlicePitch = 0;

    MultiDispatchInfo multiDispatchInfo(dc);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));
    EXPECT_EQ(1u, multiDispatchInfo.size());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), dc));
}

TEST_F(BuiltInTests, givenBigOffsetAndSizeWhenBuilderFillBufferStatelessIsUsedThenParamsAreCorrect) {

    if (is32bit) {
        GTEST_SKIP();
    }

    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::FillBufferStateless, *pClDevice);

    uint64_t bigSize = 10ull * MemoryConstants::gigaByte;
    uint64_t bigOffset = 4ull * MemoryConstants::gigaByte;
    uint64_t size = 4ull * MemoryConstants::gigaByte;

    MockBuffer srcBuffer;
    srcBuffer.size = static_cast<size_t>(bigSize);
    MockBuffer dstBuffer;
    dstBuffer.size = static_cast<size_t>(bigSize);

    BuiltinOpParams dc;
    dc.srcMemObj = &srcBuffer;
    dc.dstMemObj = &dstBuffer;
    dc.dstOffset = {static_cast<size_t>(bigOffset), 0, 0};
    dc.size = {static_cast<size_t>(size), 0, 0};

    MultiDispatchInfo multiDispatchInfo(dc);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));
    EXPECT_EQ(1u, multiDispatchInfo.size());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), dc));
}

HWTEST_F(BuiltInTests, givenBigOffsetAndSizeWhenBuilderCopyBufferToImageStatelessIsUsedThenParamsAreCorrect) {
    REQUIRE_64BIT_OR_SKIP();
    REQUIRE_IMAGES_OR_SKIP(defaultHwInfo);

    uint64_t bigSize = 10ull * MemoryConstants::gigaByte;
    uint64_t bigOffset = 4ull * MemoryConstants::gigaByte;

    MockBuffer srcBuffer;
    srcBuffer.size = static_cast<size_t>(bigSize);
    std ::unique_ptr<Image> pDstImage(Image2dHelper<>::create(pContext));
    ASSERT_NE(nullptr, pDstImage.get());

    auto &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToImage3dStateless, *pClDevice);

    BuiltinOpParams dc;
    dc.srcPtr = &srcBuffer;
    dc.dstMemObj = pDstImage.get();
    dc.srcOffset = {static_cast<size_t>(bigOffset), 0, 0};
    dc.dstOffset = {0, 0, 0};
    dc.size = {1, 1, 1};
    dc.dstRowPitch = 0;
    dc.dstSlicePitch = 0;

    MultiDispatchInfo multiDispatchInfo(dc);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));
    EXPECT_EQ(1u, multiDispatchInfo.size());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), dc));

    auto kernel = multiDispatchInfo.begin()->getKernel();
    ASSERT_NE(nullptr, kernel);
    EXPECT_TRUE(kernel->getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelAttributes.supportsBuffersBiggerThan4Gb());
    EXPECT_FALSE(kernel->getKernelInfo(rootDeviceIndex).kernelArgInfo[0].pureStatefulBufferAccess);
}

HWTEST_F(BuiltInTests, givenBigOffsetAndSizeWhenBuilderCopyImageToBufferStatelessIsUsedThenParamsAreCorrect) {

    if (is32bit) {
        GTEST_SKIP();
    }

    uint64_t bigSize = 10ull * MemoryConstants::gigaByte;
    uint64_t bigOffset = 4ull * MemoryConstants::gigaByte;

    MockBuffer dstBuffer;
    dstBuffer.size = static_cast<size_t>(bigSize);
    std ::unique_ptr<Image> pSrcImage(Image2dHelper<>::create(pContext));
    ASSERT_NE(nullptr, pSrcImage.get());

    auto &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyImage3dToBufferStateless, *pClDevice);

    BuiltinOpParams dc;
    dc.srcMemObj = pSrcImage.get();
    dc.dstMemObj = &dstBuffer;
    dc.srcOffset = {0, 0, 0};
    dc.dstOffset = {static_cast<size_t>(bigOffset), 0, 0};
    dc.size = {1, 1, 1};

    MultiDispatchInfo multiDispatchInfo(dc);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));
    EXPECT_EQ(1u, multiDispatchInfo.size());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), dc));

    auto kernel = multiDispatchInfo.begin()->getKernel();
    ASSERT_NE(nullptr, kernel);
    EXPECT_TRUE(kernel->getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelAttributes.supportsBuffersBiggerThan4Gb());
    EXPECT_FALSE(kernel->getKernelInfo(rootDeviceIndex).kernelArgInfo[0].pureStatefulBufferAccess);
}

TEST_F(BuiltInTests, GivenUnalignedCopyBufferToBufferWhenDispatchInfoIsCreatedThenParamsAreCorrect) {
    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);

    AlignedBuffer src;
    AlignedBuffer dst;

    BuiltinOpParams builtinOpsParams;

    builtinOpsParams.srcMemObj = &src;
    builtinOpsParams.srcOffset.x = 5; // causes misalignment from 4-byte boundary by 1 byte (8 bits)
    builtinOpsParams.dstMemObj = &dst;
    builtinOpsParams.size = {src.getSize(), 0, 0};

    MultiDispatchInfo multiDispatchInfo(builtinOpsParams);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));

    EXPECT_EQ(1u, multiDispatchInfo.size());

    const Kernel *kernel = multiDispatchInfo.begin()->getKernel();

    EXPECT_EQ(kernel->getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelMetadata.kernelName, "CopyBufferToBufferMiddleMisaligned");

    const auto crossThreadData = kernel->getCrossThreadData(rootDeviceIndex);
    const auto crossThreadOffset = kernel->getKernelInfo(rootDeviceIndex).kernelArgInfo[4].kernelArgPatchInfoVector[0].crossthreadOffset;
    EXPECT_EQ(8u, *reinterpret_cast<uint32_t *>(ptrOffset(crossThreadData, crossThreadOffset)));

    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
}

TEST_F(BuiltInTests, GivenReadBufferAlignedWhenDispatchInfoIsCreatedThenParamsAreCorrect) {
    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);

    AlignedBuffer srcMemObj;
    auto size = 10 * MemoryConstants::cacheLineSize;
    auto dstPtr = alignedMalloc(size, MemoryConstants::cacheLineSize);

    BuiltinOpParams builtinOpsParams;

    builtinOpsParams.srcMemObj = &srcMemObj;
    builtinOpsParams.dstPtr = dstPtr;
    builtinOpsParams.size = {size, 0, 0};

    MultiDispatchInfo multiDispatchInfo(builtinOpsParams);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));

    EXPECT_EQ(1u, multiDispatchInfo.size());

    const DispatchInfo *dispatchInfo = multiDispatchInfo.begin();

    EXPECT_EQ(1u, dispatchInfo->getDim());

    size_t leftSize = reinterpret_cast<uintptr_t>(dstPtr) % MemoryConstants::cacheLineSize;
    EXPECT_EQ(0u, leftSize);

    size_t rightSize = (reinterpret_cast<uintptr_t>(dstPtr) + size) % MemoryConstants::cacheLineSize;
    EXPECT_EQ(0u, rightSize);

    size_t middleElSize = sizeof(uint32_t) * 4;
    size_t middleSize = size / middleElSize;
    EXPECT_EQ(Vec3<size_t>(middleSize, 1, 1), dispatchInfo->getGWS());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
    alignedFree(dstPtr);
}

TEST_F(BuiltInTests, GivenWriteBufferAlignedWhenDispatchInfoIsCreatedThenParamsAreCorrect) {
    BuiltinDispatchInfoBuilder &builder = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);

    auto size = 10 * MemoryConstants::cacheLineSize;
    auto srcPtr = alignedMalloc(size, MemoryConstants::cacheLineSize);
    AlignedBuffer dstMemObj;

    BuiltinOpParams builtinOpsParams;

    builtinOpsParams.srcPtr = srcPtr;
    builtinOpsParams.dstMemObj = &dstMemObj;
    builtinOpsParams.size = {size, 0, 0};

    MultiDispatchInfo multiDispatchInfo(builtinOpsParams);
    ASSERT_TRUE(builder.buildDispatchInfos(multiDispatchInfo));

    EXPECT_EQ(1u, multiDispatchInfo.size());

    const DispatchInfo *dispatchInfo = multiDispatchInfo.begin();

    EXPECT_EQ(1u, dispatchInfo->getDim());

    size_t leftSize = reinterpret_cast<uintptr_t>(srcPtr) % MemoryConstants::cacheLineSize;
    EXPECT_EQ(0u, leftSize);

    size_t rightSize = (reinterpret_cast<uintptr_t>(srcPtr) + size) % MemoryConstants::cacheLineSize;
    EXPECT_EQ(0u, rightSize);

    size_t middleElSize = sizeof(uint32_t) * 4;
    size_t middleSize = size / middleElSize;
    EXPECT_EQ(Vec3<size_t>(middleSize, 1, 1), dispatchInfo->getGWS());
    EXPECT_TRUE(compareBuiltinOpParams(multiDispatchInfo.peekBuiltinOpParams(), builtinOpsParams));
    alignedFree(srcPtr);
}

TEST_F(BuiltInTests, WhenGettingBuilderInfoTwiceThenPointerIsSame) {
    BuiltinDispatchInfoBuilder &builder1 = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);
    BuiltinDispatchInfoBuilder &builder2 = BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::CopyBufferToBuffer, *pClDevice);

    EXPECT_EQ(&builder1, &builder2);
}

TEST_F(BuiltInTests, GivenUnknownBuiltInOpWhenGettingBuilderInfoThenExceptionThrown) {
    bool caughtException = false;
    try {
        BuiltInDispatchBuilderOp::getBuiltinDispatchInfoBuilder(EBuiltInOps::COUNT, *pClDevice);
    } catch (const std::runtime_error &) {
        caughtException = true;
    }
    EXPECT_TRUE(caughtException);
}

HWCMDTEST_F(IGFX_GEN8_CORE, BuiltInTests, WhenGettingSchedulerKernelThenCorrectKernelReturned) {
    REQUIRE_OCL_21_OR_SKIP(defaultHwInfo);
    SchedulerKernel &schedulerKernel = pContext->getSchedulerKernel();
    std::string name = SchedulerKernel::schedulerName;
    EXPECT_EQ(name, schedulerKernel.getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelMetadata.kernelName);
}

HWCMDTEST_F(IGFX_GEN8_CORE, BuiltInTests, WhenGetttingSchedulerKernelForSecondTimeThenReuseKernel) {
    REQUIRE_OCL_21_OR_SKIP(defaultHwInfo);

    SchedulerKernel &schedulerKernel = pContext->getSchedulerKernel();

    Program *program = schedulerKernel.getProgram();
    EXPECT_NE(nullptr, program);

    SchedulerKernel &schedulerKernelSecond = pContext->getSchedulerKernel();

    Program *program2 = schedulerKernelSecond.getProgram();

    EXPECT_EQ(&schedulerKernel, &schedulerKernelSecond);
    EXPECT_EQ(program, program2);
}

TEST_F(BuiltInTests, GivenUnsupportedBuildTypeWhenBuildingDispatchInfoThenFalseIsReturned) {
    auto &builtIns = *pDevice->getBuiltIns();
    BuiltinDispatchInfoBuilder dispatchInfoBuilder{builtIns, *pClDevice};
    BuiltinOpParams params;

    MultiDispatchInfo multiDispatchInfo(params);
    auto ret = dispatchInfoBuilder.buildDispatchInfos(multiDispatchInfo);
    EXPECT_FALSE(ret);
    ASSERT_EQ(0U, multiDispatchInfo.size());

    ret = dispatchInfoBuilder.buildDispatchInfos(multiDispatchInfo, nullptr, 0, Vec3<size_t>{0, 0, 0}, Vec3<size_t>{0, 0, 0}, Vec3<size_t>{0, 0, 0});
    EXPECT_FALSE(ret);
    EXPECT_EQ(0U, multiDispatchInfo.size());
}

TEST_F(BuiltInTests, GivenDefaultBuiltinDispatchInfoBuilderWhenValidateDispatchIsCalledThenClSuccessIsReturned) {
    auto &builtIns = *pDevice->getBuiltIns();
    BuiltinDispatchInfoBuilder dispatchInfoBuilder{builtIns, *pClDevice};
    auto ret = dispatchInfoBuilder.validateDispatch(nullptr, 1, Vec3<size_t>{0, 0, 0}, Vec3<size_t>{0, 0, 0}, Vec3<size_t>{0, 0, 0});
    EXPECT_EQ(CL_SUCCESS, ret);
}

TEST_F(BuiltInTests, WhenSettingExplictArgThenTrueIsReturned) {
    auto &builtIns = *pDevice->getBuiltIns();
    BuiltinDispatchInfoBuilder dispatchInfoBuilder{builtIns, *pClDevice};
    MultiDispatchInfo multiDispatchInfo;
    BuiltinOpParams params;

    cl_int err;
    auto ret = dispatchInfoBuilder.setExplicitArg(1, 5, nullptr, err);
    EXPECT_TRUE(ret);
}

TEST_F(VmeBuiltInTests, GivenVmeBuilderWhenGettingDispatchInfoThenValidPointerIsReturned) {
    overwriteBuiltInBinaryName("media_kernels_backend");

    EBuiltInOps::Type vmeOps[] = {EBuiltInOps::VmeBlockMotionEstimateIntel, EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel, EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel};
    for (auto op : vmeOps) {
        BuiltinDispatchInfoBuilder &builder = Vme::getBuiltinDispatchInfoBuilder(op, *pClDevice);
        EXPECT_NE(nullptr, &builder);
    }

    restoreBuiltInBinaryName();
}

TEST_F(VmeBuiltInTests, givenInvalidBuiltInOpWhenGetVmeBuilderInfoThenExceptionIsThrown) {
    EXPECT_THROW(Vme::getBuiltinDispatchInfoBuilder(EBuiltInOps::COUNT, *pClDevice), std::exception);
}

TEST_F(VmeBuiltInTests, GivenVmeBuilderAndInvalidParamsWhenGettingDispatchInfoThenEmptyKernelIsReturned) {
    overwriteBuiltInBinaryName("media_kernels_backend");
    EBuiltInOps::Type vmeOps[] = {EBuiltInOps::VmeBlockMotionEstimateIntel, EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel, EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel};
    for (auto op : vmeOps) {
        BuiltinDispatchInfoBuilder &builder = Vme::getBuiltinDispatchInfoBuilder(op, *pClDevice);

        MultiDispatchInfo outMdi;
        Vec3<size_t> gws{352, 288, 0};
        Vec3<size_t> elws{0, 0, 0};
        Vec3<size_t> offset{0, 0, 0};
        auto ret = builder.buildDispatchInfos(outMdi, nullptr, 0, gws, elws, offset);
        EXPECT_FALSE(ret);
        EXPECT_EQ(0U, outMdi.size());
    }
    restoreBuiltInBinaryName();
}

TEST_F(VmeBuiltInTests, GivenVmeBuilderWhenGettingDispatchInfoThenParamsAreCorrect) {
    MockKernelWithInternals mockKernel{*pClDevice};
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 16;
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = 16;
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = 0;
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = 0;

    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltinDispatchInfoBuilder &builder = Vme::getBuiltinDispatchInfoBuilder(EBuiltInOps::VmeBlockMotionEstimateIntel, *pClDevice);
    restoreBuiltInBinaryName();

    MultiDispatchInfo outMdi;
    Vec3<size_t> gws{352, 288, 0};
    Vec3<size_t> elws{0, 0, 0};
    Vec3<size_t> offset{16, 0, 0};

    MockBuffer mb;
    cl_mem bufferArg = static_cast<cl_mem>(&mb);

    cl_int err;
    constexpr uint32_t bufferArgNum = 3;
    bool ret = builder.setExplicitArg(bufferArgNum, sizeof(cl_mem), &bufferArg, err);
    EXPECT_FALSE(ret);
    EXPECT_EQ(CL_SUCCESS, err);

    ret = builder.buildDispatchInfos(outMdi, mockKernel.mockKernel, 0, gws, elws, offset);
    EXPECT_TRUE(ret);
    EXPECT_EQ(1U, outMdi.size());

    auto outDi = outMdi.begin();
    EXPECT_EQ(Vec3<size_t>(352, 1, 1), outDi->getGWS());
    EXPECT_EQ(Vec3<size_t>(16, 1, 1), outDi->getEnqueuedWorkgroupSize());
    EXPECT_EQ(Vec3<size_t>(16, 0, 0), outDi->getOffset());
    EXPECT_NE(mockKernel.mockKernel, outDi->getKernel());

    EXPECT_EQ(bufferArg, outDi->getKernel()->getKernelArg(bufferArgNum));

    constexpr uint32_t vmeImplicitArgsBase = 6;
    constexpr uint32_t vmeImplicitArgs = 3;
    ASSERT_EQ(vmeImplicitArgsBase + vmeImplicitArgs, outDi->getKernel()->getKernelInfo(rootDeviceIndex).kernelArgInfo.size());
    uint32_t vmeExtraArgsExpectedVals[] = {18, 22, 18}; // height, width, stride
    for (uint32_t i = 0; i < vmeImplicitArgs; ++i) {
        auto &argInfo = outDi->getKernel()->getKernelInfo(rootDeviceIndex).kernelArgInfo[vmeImplicitArgsBase + i];
        ASSERT_EQ(1U, argInfo.kernelArgPatchInfoVector.size());
        auto off = argInfo.kernelArgPatchInfoVector[0].crossthreadOffset;
        EXPECT_EQ(vmeExtraArgsExpectedVals[i], *((uint32_t *)(outDi->getKernel()->getCrossThreadData(rootDeviceIndex) + off)));
    }
}

TEST_F(VmeBuiltInTests, GivenAdvancedVmeBuilderWhenGettingDispatchInfoThenParamsAreCorrect) {
    MockKernelWithInternals mockKernel{*pClDevice};
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.simdSize = 16;
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.requiredWorkgroupSize[0] = 16;
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.requiredWorkgroupSize[1] = 0;
    mockKernel.kernelInfo.kernelDescriptor.kernelAttributes.requiredWorkgroupSize[2] = 0;

    Vec3<size_t> gws{352, 288, 0};
    Vec3<size_t> elws{0, 0, 0};
    Vec3<size_t> offset{0, 0, 0};

    cl_int err;

    constexpr uint32_t bufferArgNum = 7;
    MockBuffer mb;
    cl_mem bufferArg = static_cast<cl_mem>(&mb);

    constexpr uint32_t srcImageArgNum = 1;
    auto image = std::unique_ptr<Image>(Image2dHelper<>::create(pContext));
    cl_mem srcImageArg = static_cast<cl_mem>(image.get());

    EBuiltInOps::Type vmeOps[] = {EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel, EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel};
    for (auto op : vmeOps) {
        MultiDispatchInfo outMdi;
        overwriteBuiltInBinaryName("media_kernels_backend");
        BuiltinDispatchInfoBuilder &builder = Vme::getBuiltinDispatchInfoBuilder(op, *pClDevice);
        restoreBuiltInBinaryName();

        bool ret = builder.setExplicitArg(srcImageArgNum, sizeof(cl_mem), &srcImageArg, err);
        EXPECT_FALSE(ret);
        EXPECT_EQ(CL_SUCCESS, err);

        ret = builder.setExplicitArg(bufferArgNum, sizeof(cl_mem), &bufferArg, err);
        EXPECT_FALSE(ret);
        EXPECT_EQ(CL_SUCCESS, err);

        ret = builder.buildDispatchInfos(outMdi, mockKernel.mockKernel, 0, gws, elws, offset);
        EXPECT_TRUE(ret);
        EXPECT_EQ(1U, outMdi.size());

        auto outDi = outMdi.begin();
        EXPECT_EQ(Vec3<size_t>(352, 1, 1), outDi->getGWS());
        EXPECT_EQ(Vec3<size_t>(16, 1, 1), outDi->getEnqueuedWorkgroupSize());
        EXPECT_NE(mockKernel.mockKernel, outDi->getKernel());

        EXPECT_EQ(srcImageArg, outDi->getKernel()->getKernelArg(srcImageArgNum));

        uint32_t vmeImplicitArgsBase = outDi->getKernel()->getKernelInfo(rootDeviceIndex).getArgNumByName("intraSrcImg");
        uint32_t vmeImplicitArgs = 4;
        ASSERT_EQ(vmeImplicitArgsBase + vmeImplicitArgs, outDi->getKernel()->getKernelInfo(rootDeviceIndex).kernelArgInfo.size());
        EXPECT_EQ(srcImageArg, outDi->getKernel()->getKernelArg(vmeImplicitArgsBase));
        ++vmeImplicitArgsBase;
        --vmeImplicitArgs;
        uint32_t vmeExtraArgsExpectedVals[] = {18, 22, 18}; // height, width, stride
        for (uint32_t i = 0; i < vmeImplicitArgs; ++i) {
            auto &argInfo = outDi->getKernel()->getKernelInfo(rootDeviceIndex).kernelArgInfo[vmeImplicitArgsBase + i];
            ASSERT_EQ(1U, argInfo.kernelArgPatchInfoVector.size());
            auto off = argInfo.kernelArgPatchInfoVector[0].crossthreadOffset;
            EXPECT_EQ(vmeExtraArgsExpectedVals[i], *((uint32_t *)(outDi->getKernel()->getCrossThreadData(rootDeviceIndex) + off)));
        }
    }
}

TEST_F(VmeBuiltInTests, WhenGettingBuiltinAsStringThenCorrectStringIsReturned) {
    EXPECT_EQ(0, strcmp("aux_translation.builtin_kernel", getBuiltinAsString(EBuiltInOps::AuxTranslation)));
    EXPECT_EQ(0, strcmp("copy_buffer_to_buffer.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyBufferToBuffer)));
    EXPECT_EQ(0, strcmp("copy_buffer_rect.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyBufferRect)));
    EXPECT_EQ(0, strcmp("fill_buffer.builtin_kernel", getBuiltinAsString(EBuiltInOps::FillBuffer)));
    EXPECT_EQ(0, strcmp("copy_buffer_to_image3d.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyBufferToImage3d)));
    EXPECT_EQ(0, strcmp("copy_image3d_to_buffer.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyImage3dToBuffer)));
    EXPECT_EQ(0, strcmp("copy_image_to_image1d.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyImageToImage1d)));
    EXPECT_EQ(0, strcmp("copy_image_to_image2d.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyImageToImage2d)));
    EXPECT_EQ(0, strcmp("copy_image_to_image3d.builtin_kernel", getBuiltinAsString(EBuiltInOps::CopyImageToImage3d)));
    EXPECT_EQ(0, strcmp("copy_kernel_timestamps.builtin_kernel", getBuiltinAsString(EBuiltInOps::QueryKernelTimestamps)));
    EXPECT_EQ(0, strcmp("fill_image1d.builtin_kernel", getBuiltinAsString(EBuiltInOps::FillImage1d)));
    EXPECT_EQ(0, strcmp("fill_image2d.builtin_kernel", getBuiltinAsString(EBuiltInOps::FillImage2d)));
    EXPECT_EQ(0, strcmp("fill_image3d.builtin_kernel", getBuiltinAsString(EBuiltInOps::FillImage3d)));
    EXPECT_EQ(0, strcmp("vme_block_motion_estimate_intel.builtin_kernel", getBuiltinAsString(EBuiltInOps::VmeBlockMotionEstimateIntel)));
    EXPECT_EQ(0, strcmp("vme_block_advanced_motion_estimate_check_intel.builtin_kernel", getBuiltinAsString(EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel)));
    EXPECT_EQ(0, strcmp("vme_block_advanced_motion_estimate_bidirectional_check_intel", getBuiltinAsString(EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel)));
    EXPECT_EQ(0, strcmp("unknown", getBuiltinAsString(EBuiltInOps::COUNT)));
}

TEST_F(BuiltInTests, WhenUnknownOperationIsSpecifiedThenUnknownNameIsReturned) {
    EXPECT_EQ(0, strcmp("unknown", getUnknownBuiltinAsString(EBuiltInOps::CopyImage3dToBuffer)));
    EXPECT_EQ(0, strcmp("unknown", getUnknownBuiltinAsString(EBuiltInOps::COUNT)));
}

TEST_F(BuiltInTests, GivenEncodeTypeWhenGettingExtensionThenCorrectStringIsReturned) {
    EXPECT_EQ(0, strcmp("", BuiltinCode::getExtension(BuiltinCode::ECodeType::Any)));
    EXPECT_EQ(0, strcmp(".bin", BuiltinCode::getExtension(BuiltinCode::ECodeType::Binary)));
    EXPECT_EQ(0, strcmp(".bc", BuiltinCode::getExtension(BuiltinCode::ECodeType::Intermediate)));
    EXPECT_EQ(0, strcmp(".cl", BuiltinCode::getExtension(BuiltinCode::ECodeType::Source)));
    EXPECT_EQ(0, strcmp("", BuiltinCode::getExtension(BuiltinCode::ECodeType::COUNT)));
    EXPECT_EQ(0, strcmp("", BuiltinCode::getExtension(BuiltinCode::ECodeType::INVALID)));
}

TEST_F(BuiltInTests, GivenBuiltinResourceWhenCreatingBuiltinResourceThenSizesAreEqual) {
    std::string resource = "__kernel";

    auto br1 = createBuiltinResource(resource.data(), resource.size());
    EXPECT_NE(0u, br1.size());
    auto br2 = createBuiltinResource(br1);
    EXPECT_NE(0u, br2.size());

    EXPECT_EQ(br1, br2);
}

TEST_F(BuiltInTests, WhenCreatingBuiltinResourceNameThenCorrectStringIsReturned) {
    EBuiltInOps::Type builtin = EBuiltInOps::CopyBufferToBuffer;
    const std::string extension = ".cl";
    const std::string platformName = "skl";
    const uint32_t deviceRevId = 9;

    std::string resourceNameGeneric = createBuiltinResourceName(builtin, extension);
    std::string resourceNameForPlatform = createBuiltinResourceName(builtin, extension, platformName);
    std::string resourceNameForPlatformAndStepping = createBuiltinResourceName(builtin, extension, platformName, deviceRevId);

    EXPECT_EQ(0, strcmp("copy_buffer_to_buffer.builtin_kernel.cl", resourceNameGeneric.c_str()));
    EXPECT_EQ(0, strcmp("skl_0_copy_buffer_to_buffer.builtin_kernel.cl", resourceNameForPlatform.c_str()));
    EXPECT_EQ(0, strcmp("skl_9_copy_buffer_to_buffer.builtin_kernel.cl", resourceNameForPlatformAndStepping.c_str()));
}

TEST_F(BuiltInTests, WhenJoiningPathThenPathsAreJoinedWithCorrectSeparator) {
    std::string resourceName = "copy_buffer_to_buffer.builtin_kernel.cl";
    std::string resourcePath = "path";

    EXPECT_EQ(0, strcmp(resourceName.c_str(), joinPath("", resourceName).c_str()));
    EXPECT_EQ(0, strcmp(resourcePath.c_str(), joinPath(resourcePath, "").c_str()));
    EXPECT_EQ(0, strcmp((resourcePath + PATH_SEPARATOR + resourceName).c_str(), joinPath(resourcePath + PATH_SEPARATOR, resourceName).c_str()));
    EXPECT_EQ(0, strcmp((resourcePath + PATH_SEPARATOR + resourceName).c_str(), joinPath(resourcePath, resourceName).c_str()));
}

TEST_F(BuiltInTests, GivenFileNameWhenGettingKernelFromEmbeddedStorageRegistryThenValidPtrIsReturnedForExisitngKernels) {
    EmbeddedStorageRegistry storageRegistry;

    std::string resource = "__kernel";
    storageRegistry.store("kernel.cl", createBuiltinResource(resource.data(), resource.size() + 1));

    const BuiltinResourceT *br = storageRegistry.get("kernel.cl");
    EXPECT_NE(nullptr, br);
    EXPECT_EQ(0, strcmp(resource.data(), br->data()));

    const BuiltinResourceT *bnr = storageRegistry.get("unknown.cl");
    EXPECT_EQ(nullptr, bnr);
}

TEST_F(BuiltInTests, WhenStoringRootPathThenPathIsSavedCorrectly) {
    class MockStorage : Storage {
      public:
        MockStorage(const std::string &rootPath) : Storage(rootPath){};
        std::string &getRootPath() {
            return Storage::rootPath;
        }

      protected:
        BuiltinResourceT loadImpl(const std::string &fullResourceName) override {
            BuiltinResourceT ret;
            return ret;
        }
    };
    const std::string rootPath("root");
    MockStorage mockStorage(rootPath);
    EXPECT_EQ(0, strcmp(rootPath.data(), mockStorage.getRootPath().data()));
}

TEST_F(BuiltInTests, GivenFiledNameWhenLoadingImplKernelFromEmbeddedStorageRegistryThenValidPtrIsReturnedForExisitngKernels) {
    class MockEmbeddedStorage : EmbeddedStorage {
      public:
        MockEmbeddedStorage(const std::string &rootPath) : EmbeddedStorage(rootPath){};
        BuiltinResourceT loadImpl(const std::string &fullResourceName) override {
            return EmbeddedStorage::loadImpl(fullResourceName);
        }
    };
    MockEmbeddedStorage mockEmbeddedStorage("root");

    BuiltinResourceT br = mockEmbeddedStorage.loadImpl("copy_buffer_to_buffer.builtin_kernel.cl");
    EXPECT_NE(0u, br.size());

    BuiltinResourceT bnr = mockEmbeddedStorage.loadImpl("unknown.cl");
    EXPECT_EQ(0u, bnr.size());
}

TEST_F(BuiltInTests, GivenFiledNameWhenLoadingImplKernelFromFileStorageThenValidPtrIsReturnedForExisitngKernels) {
    class MockFileStorage : FileStorage {
      public:
        MockFileStorage(const std::string &rootPath) : FileStorage(rootPath){};
        BuiltinResourceT loadImpl(const std::string &fullResourceName) override {
            return FileStorage::loadImpl(fullResourceName);
        }
    };
    MockFileStorage mockEmbeddedStorage("root");

    BuiltinResourceT br = mockEmbeddedStorage.loadImpl("test_files/copybuffer.cl");
    EXPECT_NE(0u, br.size());

    BuiltinResourceT bnr = mockEmbeddedStorage.loadImpl("unknown.cl");
    EXPECT_EQ(0u, bnr.size());
}

TEST_F(BuiltInTests, WhenBuiltinsLibIsCreatedThenAllStoragesSizeIsTwo) {
    class MockBuiltinsLib : BuiltinsLib {
      public:
        StoragesContainerT &getAllStorages() {
            return BuiltinsLib::allStorages;
        }
    };
    auto mockBuiltinsLib = std::unique_ptr<MockBuiltinsLib>(new MockBuiltinsLib());
    EXPECT_EQ(2u, mockBuiltinsLib->getAllStorages().size());
}

TEST_F(BuiltInTests, GivenTypeAnyWhenGettingBuiltinCodeThenCorrectBuiltinReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Any, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::Binary, code.type);
    EXPECT_NE(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

TEST_F(BuiltInTests, GivenTypeBinaryWhenGettingBuiltinCodeThenCorrectBuiltinReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Binary, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::Binary, code.type);
    EXPECT_NE(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

TEST_F(BuiltInTests, GivenTypeIntermediateWhenGettingBuiltinCodeThenCorrectBuiltinReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Intermediate, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::Intermediate, code.type);
    EXPECT_EQ(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

TEST_F(BuiltInTests, GivenTypeSourceWhenGettingBuiltinCodeThenCorrectBuiltinReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Source, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::Source, code.type);
    EXPECT_NE(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

TEST_F(BuiltInTests, GivenTypeInvalidWhenGettingBuiltinCodeThenKernelIsEmpty) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::INVALID, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::INVALID, code.type);
    EXPECT_EQ(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

TEST_F(BuiltInTests, GivenBuiltinTypeSourceWhenGettingBuiltinResourceThenResourceSizeIsNonZero) {
    class MockBuiltinsLib : BuiltinsLib {
      public:
        BuiltinResourceT getBuiltinResource(EBuiltInOps::Type builtin, BuiltinCode::ECodeType requestedCodeType, Device &device) {
            return BuiltinsLib::getBuiltinResource(builtin, requestedCodeType, device);
        }
    };
    auto mockBuiltinsLib = std::unique_ptr<MockBuiltinsLib>(new MockBuiltinsLib());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::AuxTranslation, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyBufferRect, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillBuffer, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyBufferToImage3d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImage3dToBuffer, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImageToImage1d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImageToImage2d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImageToImage3d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillImage1d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillImage2d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillImage3d, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::VmeBlockMotionEstimateIntel, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel, BuiltinCode::ECodeType::Source, *pDevice).size());
    EXPECT_EQ(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::COUNT, BuiltinCode::ECodeType::Source, *pDevice).size());
}

HWCMDTEST_F(IGFX_GEN8_CORE, BuiltInTests, GivenBuiltinTypeBinaryWhenGettingBuiltinResourceThenResourceSizeIsNonZero) {
    class MockBuiltinsLib : BuiltinsLib {
      public:
        BuiltinResourceT getBuiltinResource(EBuiltInOps::Type builtin, BuiltinCode::ECodeType requestedCodeType, Device &device) {
            return BuiltinsLib::getBuiltinResource(builtin, requestedCodeType, device);
        }
    };
    auto mockBuiltinsLib = std::unique_ptr<MockBuiltinsLib>(new MockBuiltinsLib());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::AuxTranslation, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyBufferRect, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillBuffer, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyBufferToImage3d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImage3dToBuffer, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImageToImage1d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImageToImage2d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::CopyImageToImage3d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillImage1d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillImage2d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_NE(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::FillImage3d, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_EQ(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::VmeBlockMotionEstimateIntel, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_EQ(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_EQ(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel, BuiltinCode::ECodeType::Binary, *pDevice).size());
    EXPECT_EQ(0u, mockBuiltinsLib->getBuiltinResource(EBuiltInOps::COUNT, BuiltinCode::ECodeType::Binary, *pDevice).size());
}

TEST_F(BuiltInTests, GivenTypeAnyWhenCreatingProgramFromCodeThenValidPointerIsReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Any, *pDevice);
    EXPECT_NE(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_NE(nullptr, program.get());
}

TEST_F(BuiltInTests, GivenTypeSourceWhenCreatingProgramFromCodeThenValidPointerIsReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Source, *pDevice);
    EXPECT_NE(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_NE(nullptr, program.get());
}

TEST_F(BuiltInTests, givenCreateProgramFromSourceWhenDeviceSupportSharedSystemAllocationThenInternalOptionsDisableStosoFlag) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    pClDevice->deviceInfo.sharedSystemMemCapabilities = CL_UNIFIED_SHARED_MEMORY_ACCESS_INTEL | CL_UNIFIED_SHARED_MEMORY_ATOMIC_ACCESS_INTEL | CL_UNIFIED_SHARED_MEMORY_CONCURRENT_ACCESS_INTEL | CL_UNIFIED_SHARED_MEMORY_CONCURRENT_ATOMIC_ACCESS_INTEL;
    pDevice->deviceInfo.sharedSystemAllocationsSupport = true;

    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Source, *pDevice);
    EXPECT_NE(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_NE(nullptr, program.get());
    std::string builtinInternalOptions;
    program->initInternalOptions(builtinInternalOptions);
    EXPECT_THAT(builtinInternalOptions, testing::HasSubstr(std::string(CompilerOptions::greaterThan4gbBuffersRequired)));
}

TEST_F(BuiltInTests, GivenTypeIntermediateWhenCreatingProgramFromCodeThenNullPointerIsReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Intermediate, *pDevice);
    EXPECT_EQ(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_EQ(nullptr, program.get());
}

TEST_F(BuiltInTests, GivenTypeBinaryWhenCreatingProgramFromCodeThenValidPointerIsReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Binary, *pDevice);
    EXPECT_NE(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_NE(nullptr, program.get());
}

TEST_F(BuiltInTests, GivenTypeInvalidWhenCreatingProgramFromCodeThenNullPointerIsReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::INVALID, *pDevice);
    EXPECT_EQ(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_EQ(nullptr, program.get());
}

TEST_F(BuiltInTests, GivenInvalidBuiltinWhenCreatingProgramFromCodeThenNullPointerIsReturned) {
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::COUNT, BuiltinCode::ECodeType::Any, *pDevice);
    EXPECT_EQ(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    EXPECT_EQ(nullptr, program.get());
}

TEST_F(BuiltInTests, GivenForce32bitWhenCreatingProgramThenCorrectKernelIsCreated) {
    bool force32BitAddressess = pDevice->getDeviceInfo().force32BitAddressess;
    const_cast<DeviceInfo *>(&pDevice->getDeviceInfo())->force32BitAddressess = true;

    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    const BuiltinCode bc = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Source, *pDevice);
    ASSERT_NE(0u, bc.resource.size());
    auto program = std::unique_ptr<Program>(BuiltinDispatchInfoBuilder::createProgramFromCode(bc, toClDeviceVector(*pClDevice)));
    ASSERT_NE(nullptr, program.get());

    std::string builtinInternalOptions;
    program->initInternalOptions(builtinInternalOptions);
    auto it = builtinInternalOptions.find(NEO::CompilerOptions::arch32bit.data());
    EXPECT_EQ(std::string::npos, it);

    it = builtinInternalOptions.find(NEO::CompilerOptions::greaterThan4gbBuffersRequired.data());
    if (is32bit || pDevice->areSharedSystemAllocationsAllowed()) {
        EXPECT_NE(std::string::npos, it);
    } else {
        EXPECT_EQ(std::string::npos, it);
    }

    const_cast<DeviceInfo *>(&pDevice->getDeviceInfo())->force32BitAddressess = force32BitAddressess;
}

TEST_F(BuiltInTests, GivenVmeKernelWhenGettingDeviceInfoThenCorrectVmeVersionIsReturned) {
    if (!pDevice->getHardwareInfo().capabilityTable.supportsVme) {
        GTEST_SKIP();
    }
    cl_uint param;
    auto ret = pClDevice->getDeviceInfo(CL_DEVICE_ME_VERSION_INTEL, sizeof(param), &param, nullptr);
    EXPECT_EQ(CL_SUCCESS, ret);
    EXPECT_EQ(static_cast<cl_uint>(CL_ME_VERSION_ADVANCED_VER_2_INTEL), param);
}

TEST_F(VmeBuiltInTests, WhenVmeKernelIsCreatedThenParamsAreCorrect) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int err;

    {
        int32_t bufArgNum = 7;

        cl_mem mem = 0;
        vmeBuilder.setExplicitArg(bufArgNum, sizeof(cl_mem), &mem, err);
        EXPECT_TRUE(vmeBuilder.validateBufferSize(-1, 16));
        EXPECT_TRUE(vmeBuilder.validateBufferSize(bufArgNum, 16));

        MockBuffer mb;
        mem = &mb;
        vmeBuilder.setExplicitArg(bufArgNum, sizeof(cl_mem), &mem, err);
        EXPECT_TRUE(vmeBuilder.validateBufferSize(bufArgNum, mb.getSize()));
        EXPECT_TRUE(vmeBuilder.validateBufferSize(bufArgNum, mb.getSize() / 2));
        EXPECT_FALSE(vmeBuilder.validateBufferSize(bufArgNum, mb.getSize() * 2));

        mem = 0;
        vmeBuilder.setExplicitArg(bufArgNum, sizeof(cl_mem), &mem, err);
    }

    {
        EXPECT_TRUE(vmeBuilder.validateEnumVal(1, 1, 2, 3, 4));
        EXPECT_TRUE(vmeBuilder.validateEnumVal(1, 1));
        EXPECT_TRUE(vmeBuilder.validateEnumVal(3, 1, 2, 3));

        EXPECT_FALSE(vmeBuilder.validateEnumVal(1, 3, 4));
        EXPECT_FALSE(vmeBuilder.validateEnumVal(1));
        EXPECT_FALSE(vmeBuilder.validateEnumVal(1, 2));

        int32_t valArgNum = 3;
        uint32_t val = 7;
        vmeBuilder.setExplicitArg(valArgNum, sizeof(val), &val, err);
        EXPECT_FALSE(vmeBuilder.validateEnumArg<uint32_t>(valArgNum, 3));
        EXPECT_TRUE(vmeBuilder.validateEnumArg<uint32_t>(valArgNum, 7));

        val = 0;
        vmeBuilder.setExplicitArg(valArgNum, sizeof(val), &val, err);
    }

    {
        int32_t valArgNum = 3;
        uint32_t val = 7;
        vmeBuilder.setExplicitArg(valArgNum, sizeof(val), &val, err);
        EXPECT_EQ(val, vmeBuilder.getKernelArgByValValue<uint32_t>(valArgNum));
        val = 11;
        vmeBuilder.setExplicitArg(valArgNum, sizeof(val), &val, err);
        EXPECT_EQ(val, vmeBuilder.getKernelArgByValValue<uint32_t>(valArgNum));

        val = 0;
        vmeBuilder.setExplicitArg(valArgNum, sizeof(val), &val, err);
    }
}

TEST_F(VmeBuiltInTests, WhenVmeKernelIsCreatedThenDispatchIsBidirectional) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> avmeBuilder(*this->pBuiltIns, *pClDevice);
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel> avmeBidirBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    EXPECT_FALSE(avmeBuilder.isBidirKernel());
    EXPECT_TRUE(avmeBidirBuilder.isBidirKernel());
}

struct ImageVmeValidFormat : Image2dDefaults {
    static const cl_image_format imageFormat;
    static const cl_image_desc iamgeDesc;
};

const cl_image_format ImageVmeValidFormat::imageFormat = {
    CL_R,
    CL_UNORM_INT8};

const cl_image_desc ImageVmeValidFormat::iamgeDesc = {
    CL_MEM_OBJECT_IMAGE1D,
    8192,
    16,
    1,
    1,
    0,
    0,
    0,
    0,
    {nullptr}};

struct ImageVmeInvalidDataType : Image2dDefaults {
    static const cl_image_format imageFormat;
};

const cl_image_format ImageVmeInvalidDataType::imageFormat = {
    CL_R,
    CL_FLOAT};

struct ImageVmeInvalidChannelOrder : Image2dDefaults {
    static const cl_image_format imageFormat;
};

const cl_image_format ImageVmeInvalidChannelOrder::imageFormat = {
    CL_RGBA,
    CL_UNORM_INT8};

TEST_F(VmeBuiltInTests, WhenValidatingImagesThenCorrectResponses) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockMotionEstimateIntel> vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    uint32_t srcImgArgNum = 1;
    uint32_t refImgArgNum = 2;

    cl_int err;
    { // validate images are not null
        std::unique_ptr<Image> image1(ImageHelper<ImageVmeValidFormat>::create(pContext));

        cl_mem srcImgMem = 0;
        cl_mem refImgMem = 0;
        EXPECT_EQ(CL_INVALID_KERNEL_ARGS, vmeBuilder.validateImages(Vec3<size_t>{3, 3, 0}, Vec3<size_t>{0, 0, 0}));

        srcImgMem = image1.get();
        refImgMem = 0;
        vmeBuilder.setExplicitArg(srcImgArgNum, sizeof(srcImgMem), &srcImgMem, err);
        EXPECT_EQ(CL_INVALID_KERNEL_ARGS, vmeBuilder.validateImages(Vec3<size_t>{3, 3, 0}, Vec3<size_t>{0, 0, 0}));
    }

    { // validate image formats
        std::unique_ptr<Image> imageValid(ImageHelper<ImageVmeValidFormat>::create(pContext));
        std::unique_ptr<Image> imageInvalidDataType(ImageHelper<ImageVmeInvalidDataType>::create(pContext));
        std::unique_ptr<Image> imageChannelOrder(ImageHelper<ImageVmeInvalidChannelOrder>::create(pContext));

        Image *images[] = {imageValid.get(), imageInvalidDataType.get(), imageChannelOrder.get()};
        for (Image *srcImg : images) {
            for (Image *dstImg : images) {
                cl_mem srcImgMem = srcImg;
                cl_mem refImgMem = dstImg;
                vmeBuilder.setExplicitArg(srcImgArgNum, sizeof(srcImgMem), &srcImgMem, err);
                vmeBuilder.setExplicitArg(refImgArgNum, sizeof(refImgMem), &refImgMem, err);
                bool shouldSucceed = (srcImg == imageValid.get()) && (dstImg == imageValid.get());
                if (shouldSucceed) {
                    EXPECT_EQ(CL_SUCCESS, vmeBuilder.validateImages(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}));
                } else {
                    EXPECT_EQ(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR, vmeBuilder.validateImages(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}));
                }
            }
        }
    }

    { // validate image tiling
        std::unique_ptr<Image> imageValid(ImageHelper<ImageVmeValidFormat>::create(pContext));
        pContext->isSharedContext = true;
        std::unique_ptr<Image> imageLinear(ImageHelper<ImageVmeValidFormat>::create(pContext));
        pContext->isSharedContext = false;
        Image *images[] = {imageValid.get(), imageLinear.get()};
        for (Image *srcImg : images) {
            for (Image *dstImg : images) {
                cl_mem srcImgMem = srcImg;
                cl_mem refImgMem = dstImg;
                vmeBuilder.setExplicitArg(srcImgArgNum, sizeof(srcImgMem), &srcImgMem, err);
                vmeBuilder.setExplicitArg(refImgArgNum, sizeof(refImgMem), &refImgMem, err);
                bool shouldSucceed = (srcImg == imageValid.get()) && (dstImg == imageValid.get());
                if (shouldSucceed) {
                    EXPECT_EQ(CL_SUCCESS, vmeBuilder.validateImages(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}));
                } else {
                    EXPECT_EQ(CL_OUT_OF_RESOURCES, vmeBuilder.validateImages(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}));
                }
            }
        }
    }

    { // validate region size
        std::unique_ptr<Image> imageValid(ImageHelper<ImageVmeValidFormat>::create(pContext));
        cl_mem imgValidMem = imageValid.get();
        vmeBuilder.setExplicitArg(srcImgArgNum, sizeof(imgValidMem), &imgValidMem, err);
        vmeBuilder.setExplicitArg(refImgArgNum, sizeof(imgValidMem), &imgValidMem, err);

        EXPECT_EQ(CL_INVALID_IMAGE_SIZE, vmeBuilder.validateImages(Vec3<size_t>{imageValid->getImageDesc().image_width + 1, 1, 0}, Vec3<size_t>{0, 0, 0}));
        EXPECT_EQ(CL_INVALID_IMAGE_SIZE, vmeBuilder.validateImages(Vec3<size_t>{1, imageValid->getImageDesc().image_height + 1, 0}, Vec3<size_t>{0, 0, 0}));
    }
}

TEST_F(VmeBuiltInTests, WhenValidatingFlagsThenValidFlagCombinationsReturnTrue) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    uint32_t defaultSkipBlockVal = 8192;
    uint32_t flagsArgNum = 3;

    std::tuple<uint32_t, bool, uint32_t> flagsToTest[] = {
        std::make_tuple(CL_ME_CHROMA_INTRA_PREDICT_ENABLED_INTEL, false, defaultSkipBlockVal),
        std::make_tuple(CL_ME_SKIP_BLOCK_TYPE_16x16_INTEL, true, CL_ME_MB_TYPE_16x16_INTEL),
        std::make_tuple(CL_ME_SKIP_BLOCK_TYPE_8x8_INTEL, true, CL_ME_MB_TYPE_8x8_INTEL),
        std::make_tuple(defaultSkipBlockVal, true, defaultSkipBlockVal),
    };

    cl_int err;
    for (auto &conf : flagsToTest) {
        uint32_t skipBlock = defaultSkipBlockVal;
        vmeBuilder.setExplicitArg(flagsArgNum, sizeof(uint32_t), &std::get<0>(conf), err);
        bool validationResult = vmeBuilder.validateFlags(skipBlock);
        if (std::get<1>(conf)) {
            EXPECT_TRUE(validationResult);
        } else {
            EXPECT_FALSE(validationResult);
        }
        EXPECT_EQ(std::get<2>(conf), skipBlock);
    }
}

TEST_F(VmeBuiltInTests, WhenValidatingSkipBlockTypeThenCorrectResponses) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel> avmeBidirectionalBuilder(*this->pBuiltIns, *pClDevice);
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> avmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int err;
    uint32_t skipBlockTypeArgNum = 4;

    uint32_t skipBlockType = 8192;
    bool ret = avmeBidirectionalBuilder.validateSkipBlockTypeArg(skipBlockType);
    EXPECT_TRUE(ret);
    EXPECT_EQ(8192U, skipBlockType);

    skipBlockType = 8192U;
    avmeBuilder.setExplicitArg(skipBlockTypeArgNum, sizeof(uint32_t), &skipBlockType, err);
    ret = avmeBuilder.validateSkipBlockTypeArg(skipBlockType);
    EXPECT_FALSE(ret);

    skipBlockType = CL_ME_MB_TYPE_16x16_INTEL;
    avmeBuilder.setExplicitArg(skipBlockTypeArgNum, sizeof(uint32_t), &skipBlockType, err);
    skipBlockType = 8192U;
    ret = avmeBuilder.validateSkipBlockTypeArg(skipBlockType);
    EXPECT_TRUE(ret);
    EXPECT_EQ(static_cast<uint32_t>(CL_ME_MB_TYPE_16x16_INTEL), skipBlockType);

    skipBlockType = CL_ME_MB_TYPE_8x8_INTEL;
    avmeBuilder.setExplicitArg(skipBlockTypeArgNum, sizeof(uint32_t), &skipBlockType, err);
    skipBlockType = 8192U;
    ret = avmeBuilder.validateSkipBlockTypeArg(skipBlockType);
    EXPECT_TRUE(ret);
    EXPECT_EQ(static_cast<uint32_t>(CL_ME_MB_TYPE_8x8_INTEL), skipBlockType);
}

TEST_F(VmeBuiltInTests, GivenAcceleratorWhenExplicitlySettingArgThenFalseIsReturned) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");

    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int err;
    uint32_t aceleratorArgNum = 0;
    bool ret = vmeBuilder.setExplicitArg(aceleratorArgNum, sizeof(cl_accelerator_intel), nullptr, err);
    EXPECT_FALSE(ret);
    EXPECT_EQ(CL_INVALID_ACCELERATOR_INTEL, err);

    cl_motion_estimation_desc_intel acceleratorDesc;
    acceleratorDesc.subpixel_mode = CL_ME_SUBPIXEL_MODE_INTEGER_INTEL;
    acceleratorDesc.sad_adjust_mode = CL_ME_SAD_ADJUST_MODE_NONE_INTEL;
    acceleratorDesc.search_path_type = CL_ME_SEARCH_PATH_RADIUS_2_2_INTEL;
    acceleratorDesc.mb_block_type = CL_ME_MB_TYPE_16x16_INTEL;
    auto neoAccelerator = std::unique_ptr<VmeAccelerator>(VmeAccelerator::create(pContext, CL_ACCELERATOR_TYPE_MOTION_ESTIMATION_INTEL, sizeof(acceleratorDesc), &acceleratorDesc, err));
    ASSERT_NE(nullptr, neoAccelerator.get());
    cl_accelerator_intel clAccel = neoAccelerator.get();
    ret = vmeBuilder.setExplicitArg(aceleratorArgNum, sizeof(cl_accelerator_intel), &clAccel, err);
    EXPECT_FALSE(ret);
    EXPECT_EQ(CL_SUCCESS, err);
}

TEST_F(VmeBuiltInTests, WhenValidatingDispatchThenCorrectReturns) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    struct MockVmeBuilder : BuiltInOp<EBuiltInOps::VmeBlockMotionEstimateIntel> {
        using BuiltInOp<EBuiltInOps::VmeBlockMotionEstimateIntel>::BuiltInOp;

        cl_int validateVmeDispatch(Vec3<size_t> inputRegion, Vec3<size_t> offset, size_t blkNum, size_t blkMul) const override {
            receivedInputRegion = inputRegion;
            receivedOffset = offset;
            receivedBlkNum = blkNum;
            receivedBlkMul = blkMul;
            wasValidateVmeDispatchCalled = true;
            return valueToReturn;
        }

        mutable bool wasValidateVmeDispatchCalled = false;
        mutable Vec3<size_t> receivedInputRegion = {0, 0, 0};
        mutable Vec3<size_t> receivedOffset = {0, 0, 0};
        mutable size_t receivedBlkNum = 0;
        mutable size_t receivedBlkMul = 0;
        mutable cl_int valueToReturn = CL_SUCCESS;
    };

    uint32_t aaceleratorArgNum = 0;
    MockVmeBuilder vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int ret = vmeBuilder.validateDispatch(nullptr, 1, Vec3<size_t>{16, 16, 0}, Vec3<size_t>{16, 1, 0}, Vec3<size_t>{0, 0, 0});
    EXPECT_EQ(CL_INVALID_WORK_DIMENSION, ret);

    ret = vmeBuilder.validateDispatch(nullptr, 3, Vec3<size_t>{16, 16, 0}, Vec3<size_t>{16, 1, 0}, Vec3<size_t>{0, 0, 0});
    EXPECT_EQ(CL_INVALID_WORK_DIMENSION, ret);

    ret = vmeBuilder.validateDispatch(nullptr, 2, Vec3<size_t>{16, 16, 0}, Vec3<size_t>{16, 1, 0}, Vec3<size_t>{0, 0, 0});
    EXPECT_EQ(CL_INVALID_KERNEL_ARGS, ret); // accelerator not set
    EXPECT_FALSE(vmeBuilder.wasValidateVmeDispatchCalled);

    cl_int err;
    cl_motion_estimation_desc_intel acceleratorDesc;
    acceleratorDesc.subpixel_mode = CL_ME_SUBPIXEL_MODE_INTEGER_INTEL;
    acceleratorDesc.sad_adjust_mode = CL_ME_SAD_ADJUST_MODE_NONE_INTEL;
    acceleratorDesc.search_path_type = CL_ME_SEARCH_PATH_RADIUS_2_2_INTEL;

    Vec3<size_t> gws{16, 16, 0};
    Vec3<size_t> lws{16, 1, 0};
    Vec3<size_t> off{0, 0, 0};
    size_t gwWidthInBlk = 0;
    size_t gwHeightInBlk = 0;
    vmeBuilder.getBlkTraits(gws, gwWidthInBlk, gwHeightInBlk);

    {
        acceleratorDesc.mb_block_type = CL_ME_MB_TYPE_16x16_INTEL;
        auto neoAccelerator = std::unique_ptr<VmeAccelerator>(VmeAccelerator::create(pContext, CL_ACCELERATOR_TYPE_MOTION_ESTIMATION_INTEL, sizeof(acceleratorDesc), &acceleratorDesc, err));
        ASSERT_NE(nullptr, neoAccelerator.get());
        cl_accelerator_intel clAccel = neoAccelerator.get();
        vmeBuilder.setExplicitArg(aaceleratorArgNum, sizeof(clAccel), &clAccel, err);
        vmeBuilder.wasValidateVmeDispatchCalled = false;
        auto ret = vmeBuilder.validateDispatch(nullptr, 2, gws, lws, off);
        EXPECT_EQ(CL_SUCCESS, ret);
        EXPECT_TRUE(vmeBuilder.wasValidateVmeDispatchCalled);
        EXPECT_EQ(gws, vmeBuilder.receivedInputRegion);
        EXPECT_EQ(off, vmeBuilder.receivedOffset);

        EXPECT_EQ(gwWidthInBlk * gwHeightInBlk, vmeBuilder.receivedBlkNum);
        EXPECT_EQ(1U, vmeBuilder.receivedBlkMul);
    }

    {
        acceleratorDesc.mb_block_type = CL_ME_MB_TYPE_4x4_INTEL;
        auto neoAccelerator = std::unique_ptr<VmeAccelerator>(VmeAccelerator::create(pContext, CL_ACCELERATOR_TYPE_MOTION_ESTIMATION_INTEL, sizeof(acceleratorDesc), &acceleratorDesc, err));
        ASSERT_NE(nullptr, neoAccelerator.get());
        cl_accelerator_intel clAccel = neoAccelerator.get();
        vmeBuilder.setExplicitArg(aaceleratorArgNum, sizeof(clAccel), &clAccel, err);
        vmeBuilder.wasValidateVmeDispatchCalled = false;
        auto ret = vmeBuilder.validateDispatch(nullptr, 2, gws, lws, off);
        EXPECT_EQ(CL_SUCCESS, ret);
        EXPECT_TRUE(vmeBuilder.wasValidateVmeDispatchCalled);
        EXPECT_EQ(gws, vmeBuilder.receivedInputRegion);
        EXPECT_EQ(off, vmeBuilder.receivedOffset);

        EXPECT_EQ(gwWidthInBlk * gwHeightInBlk, vmeBuilder.receivedBlkNum);
        EXPECT_EQ(16U, vmeBuilder.receivedBlkMul);
    }

    {
        acceleratorDesc.mb_block_type = CL_ME_MB_TYPE_8x8_INTEL;
        auto neoAccelerator = std::unique_ptr<VmeAccelerator>(VmeAccelerator::create(pContext, CL_ACCELERATOR_TYPE_MOTION_ESTIMATION_INTEL, sizeof(acceleratorDesc), &acceleratorDesc, err));
        ASSERT_NE(nullptr, neoAccelerator.get());
        cl_accelerator_intel clAccel = neoAccelerator.get();
        vmeBuilder.setExplicitArg(aaceleratorArgNum, sizeof(clAccel), &clAccel, err);
        vmeBuilder.wasValidateVmeDispatchCalled = false;
        vmeBuilder.valueToReturn = 37;
        auto ret = vmeBuilder.validateDispatch(nullptr, 2, gws, lws, off);
        EXPECT_EQ(37, ret);
        EXPECT_TRUE(vmeBuilder.wasValidateVmeDispatchCalled);
        EXPECT_EQ(gws, vmeBuilder.receivedInputRegion);
        EXPECT_EQ(off, vmeBuilder.receivedOffset);

        EXPECT_EQ(gwWidthInBlk * gwHeightInBlk, vmeBuilder.receivedBlkNum);
        EXPECT_EQ(4U, vmeBuilder.receivedBlkMul);
    }
}

TEST_F(VmeBuiltInTests, WhenValidatingVmeDispatchThenCorrectReturns) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockMotionEstimateIntel> vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int err;

    // images not set
    EXPECT_EQ(CL_INVALID_KERNEL_ARGS, vmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    uint32_t srcImgArgNum = 1;
    uint32_t refImgArgNum = 2;

    std::unique_ptr<Image> imageValid(ImageHelper<ImageVmeValidFormat>::create(pContext));
    cl_mem srcImgMem = imageValid.get();

    vmeBuilder.setExplicitArg(srcImgArgNum, sizeof(srcImgMem), &srcImgMem, err);
    vmeBuilder.setExplicitArg(refImgArgNum, sizeof(srcImgMem), &srcImgMem, err);

    // null buffers are valid
    EXPECT_EQ(CL_SUCCESS, vmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    // too small buffers should fail
    MockBuffer mb;
    cl_mem mem = &mb;

    uint32_t predictionMotionVectorBufferArgNum = 3;
    uint32_t motionVectorBufferArgNum = 4;
    uint32_t residualsBufferArgNum = 5;
    for (uint32_t argNum : {predictionMotionVectorBufferArgNum, motionVectorBufferArgNum, residualsBufferArgNum}) {
        EXPECT_EQ(CL_SUCCESS, vmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, mb.getSize() * 2, 1));
        vmeBuilder.setExplicitArg(argNum, sizeof(cl_mem), &mem, err);
        EXPECT_EQ(CL_INVALID_BUFFER_SIZE, vmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, mb.getSize() * 2, 1));
        vmeBuilder.setExplicitArg(argNum, sizeof(cl_mem), nullptr, err);
    }
}

TEST_F(VmeBuiltInTests, GivenAdvancedVmeWhenValidatingVmeDispatchThenCorrectReturns) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> avmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int err;
    // images not set
    ASSERT_EQ(CL_INVALID_KERNEL_ARGS, avmeBuilder.VmeBuiltinDispatchInfoBuilder::validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));
    EXPECT_EQ(CL_INVALID_KERNEL_ARGS, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    uint32_t srcImgArgNum = 1;
    uint32_t refImgArgNum = 2;

    std::unique_ptr<Image> imageValid(ImageHelper<ImageVmeValidFormat>::create(pContext));
    cl_mem srcImgMem = imageValid.get();

    avmeBuilder.setExplicitArg(srcImgArgNum, sizeof(srcImgMem), &srcImgMem, err);
    avmeBuilder.setExplicitArg(refImgArgNum, sizeof(srcImgMem), &srcImgMem, err);

    ASSERT_EQ(CL_SUCCESS, avmeBuilder.VmeBuiltinDispatchInfoBuilder::validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    uint32_t flagsArgNum = 3;
    uint32_t val = CL_ME_CHROMA_INTRA_PREDICT_ENABLED_INTEL;
    avmeBuilder.setExplicitArg(flagsArgNum, sizeof(val), &val, err);
    EXPECT_EQ(CL_INVALID_KERNEL_ARGS, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));
    val = CL_ME_SKIP_BLOCK_TYPE_8x8_INTEL;
    avmeBuilder.setExplicitArg(flagsArgNum, sizeof(val), &val, err);

    uint32_t skipBlockTypeArgNum = 4;
    val = 8192;
    avmeBuilder.setExplicitArg(skipBlockTypeArgNum, sizeof(uint32_t), &val, err);
    EXPECT_EQ(CL_OUT_OF_RESOURCES, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));
    val = CL_ME_MB_TYPE_16x16_INTEL;
    avmeBuilder.setExplicitArg(skipBlockTypeArgNum, sizeof(uint32_t), &val, err);

    uint32_t searchCostPenaltyArgNum = 5;
    val = 8192;
    avmeBuilder.setExplicitArg(searchCostPenaltyArgNum, sizeof(uint32_t), &val, err);
    EXPECT_EQ(CL_OUT_OF_RESOURCES, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));
    val = CL_ME_COST_PENALTY_NONE_INTEL;
    avmeBuilder.setExplicitArg(searchCostPenaltyArgNum, sizeof(uint32_t), &val, err);

    uint32_t searchCostPrecisionArgNum = 6;
    val = 8192;
    avmeBuilder.setExplicitArg(searchCostPrecisionArgNum, sizeof(uint32_t), &val, err);
    EXPECT_EQ(CL_OUT_OF_RESOURCES, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));
    val = CL_ME_COST_PRECISION_QPEL_INTEL;
    avmeBuilder.setExplicitArg(searchCostPrecisionArgNum, sizeof(uint32_t), &val, err);

    // for non-bidirectional avme kernel, countMotionVectorBuffer must be set
    uint32_t countMotionVectorBufferArgNum = 7;
    EXPECT_EQ(CL_INVALID_BUFFER_SIZE, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    MockBuffer mb;
    cl_mem mem = &mb;
    avmeBuilder.setExplicitArg(countMotionVectorBufferArgNum, sizeof(cl_mem), &mem, err);
    EXPECT_EQ(CL_SUCCESS, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 1, 1));
}

TEST_F(VmeBuiltInTests, GivenAdvancedBidirectionalVmeWhenValidatingVmeDispatchThenCorrectReturns) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateBidirectionalCheckIntel> avmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    cl_int err;
    uint32_t srcImgArgNum = 1;
    uint32_t refImgArgNum = 2;

    std::unique_ptr<Image> imageValid(ImageHelper<ImageVmeValidFormat>::create(pContext));
    cl_mem srcImgMem = imageValid.get();

    avmeBuilder.setExplicitArg(srcImgArgNum, sizeof(srcImgMem), &srcImgMem, err);
    avmeBuilder.setExplicitArg(refImgArgNum, sizeof(srcImgMem), &srcImgMem, err);

    ASSERT_EQ(CL_SUCCESS, avmeBuilder.VmeBuiltinDispatchInfoBuilder::validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    uint32_t flagsArgNum = 6;
    uint32_t val = CL_ME_SKIP_BLOCK_TYPE_8x8_INTEL;
    avmeBuilder.setExplicitArg(flagsArgNum, sizeof(val), &val, err);

    uint32_t searchCostPenaltyArgNum = 7;
    val = CL_ME_COST_PENALTY_NONE_INTEL;
    avmeBuilder.setExplicitArg(searchCostPenaltyArgNum, sizeof(uint32_t), &val, err);

    uint32_t searchCostPrecisionArgNum = 8;
    val = CL_ME_COST_PRECISION_QPEL_INTEL;
    avmeBuilder.setExplicitArg(searchCostPrecisionArgNum, sizeof(uint32_t), &val, err);

    uint32_t bidirWeightArgNum = 10;
    val = 255;
    avmeBuilder.setExplicitArg(bidirWeightArgNum, sizeof(uint8_t), &val, err);
    EXPECT_EQ(CL_INVALID_KERNEL_ARGS, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));
    val = CL_ME_BIDIR_WEIGHT_QUARTER_INTEL;
    avmeBuilder.setExplicitArg(bidirWeightArgNum, sizeof(uint8_t), &val, err);

    EXPECT_EQ(CL_SUCCESS, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 64, 1));

    // test bufferSize checking
    uint32_t countMotionVectorBufferArgNum = 11;
    MockBuffer mb;
    cl_mem mem = &mb;
    avmeBuilder.setExplicitArg(countMotionVectorBufferArgNum, sizeof(cl_mem), &mem, err);
    EXPECT_EQ(CL_SUCCESS, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, 1, 1));
    EXPECT_EQ(CL_INVALID_BUFFER_SIZE, avmeBuilder.validateVmeDispatch(Vec3<size_t>{1, 1, 0}, Vec3<size_t>{0, 0, 0}, mb.getSize() * 2, 1));
}

TEST_F(VmeBuiltInTests, GivenAdvancedVmeWhenGettingSkipResidualsBuffExpSizeThenDefaultSizeIsReturned) {
    this->pBuiltIns->setCacheingEnableState(false);
    overwriteBuiltInBinaryName("media_kernels_backend");
    BuiltInOp<EBuiltInOps::VmeBlockAdvancedMotionEstimateCheckIntel> vmeBuilder(*this->pBuiltIns, *pClDevice);
    restoreBuiltInBinaryName();

    auto size16x16 = vmeBuilder.getSkipResidualsBuffExpSize(CL_ME_MB_TYPE_16x16_INTEL, 4);
    auto sizeDefault = vmeBuilder.getSkipResidualsBuffExpSize(8192, 4);
    EXPECT_EQ(size16x16, sizeDefault);
}

TEST_F(BuiltInTests, GivenInvalidBuiltinKernelNameWhenCreatingBuiltInProgramThenInvalidValueErrorIsReturned) {
    const char *kernelNames = "invalid_kernel";
    cl_int retVal = CL_SUCCESS;

    cl_program program = Vme::createBuiltInProgram(
        *pContext,
        pContext->getDevices(),
        kernelNames,
        retVal);

    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(nullptr, program);
}

TEST_F(BuiltInTests, WhenGettingSipKernelThenReturnProgramCreatedFromIsaAcquiredThroughCompilerInterface) {
    auto mockCompilerInterface = new MockCompilerInterface();
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[rootDeviceIndex]->compilerInterface.reset(mockCompilerInterface);
    auto builtins = new BuiltIns;
    pDevice->getExecutionEnvironment()->rootDeviceEnvironments[rootDeviceIndex]->builtins.reset(builtins);
    mockCompilerInterface->sipKernelBinaryOverride = mockCompilerInterface->getDummyGenBinary();

    const SipKernel &sipKernel = builtins->getSipKernel(SipKernelType::Csr, *pDevice);

    auto expectedMem = mockCompilerInterface->sipKernelBinaryOverride.data();
    EXPECT_EQ(0, memcmp(expectedMem, sipKernel.getSipAllocation()->getUnderlyingBuffer(), mockCompilerInterface->sipKernelBinaryOverride.size()));
    EXPECT_EQ(SipKernelType::Csr, mockCompilerInterface->requestedSipKernel);

    mockCompilerInterface->releaseDummyGenBinary();
}

TEST_F(BuiltInTests, givenSipKernelWhenItIsCreatedThenItHasGraphicsAllocationForKernel) {
    const SipKernel &sipKern = pDevice->getBuiltIns()->getSipKernel(SipKernelType::Csr, pContext->getDevice(0)->getDevice());
    auto sipAllocation = sipKern.getSipAllocation();
    EXPECT_NE(nullptr, sipAllocation);
}

TEST_F(BuiltInTests, givenSipKernelWhenAllocationFailsThenItHasNullptrGraphicsAllocation) {
    auto executionEnvironment = new MockExecutionEnvironment;
    executionEnvironment->prepareRootDeviceEnvironments(1);
    auto memoryManager = new MockMemoryManager(*executionEnvironment);
    executionEnvironment->memoryManager.reset(memoryManager);
    auto device = std::unique_ptr<RootDevice>(Device::create<RootDevice>(executionEnvironment, 0u));
    EXPECT_NE(nullptr, device);

    memoryManager->failAllocate32Bit = true;

    auto builtins = std::make_unique<BuiltIns>();
    const SipKernel &sipKern = builtins->getSipKernel(SipKernelType::Csr, *device);
    auto sipAllocation = sipKern.getSipAllocation();
    EXPECT_EQ(nullptr, sipAllocation);
}

TEST_F(BuiltInTests, givenSameDeviceIsUsedWhenUsingStaticGetterThenExpectRetrieveSameAllocation) {
    const SipKernel &sipKern = pDevice->getBuiltIns()->getSipKernel(SipKernelType::Csr, pContext->getDevice(0)->getDevice());
    auto sipAllocation = sipKern.getSipAllocation();
    EXPECT_NE(nullptr, sipAllocation);
    auto staticSipAllocation = SipKernel::getSipKernelAllocation(*pDevice);
    EXPECT_NE(nullptr, staticSipAllocation);
    EXPECT_EQ(sipAllocation, staticSipAllocation);
}

TEST_F(BuiltInTests, givenDebugFlagForceUseSourceWhenArgIsBinaryThenReturnBuiltinCodeBinary) {
    DebugManager.flags.RebuildPrecompiledKernels.set(true);
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Binary, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::Binary, code.type);
    EXPECT_NE(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

TEST_F(BuiltInTests, givenDebugFlagForceUseSourceWhenArgIsAnyThenReturnBuiltinCodeSource) {
    DebugManager.flags.RebuildPrecompiledKernels.set(true);
    auto builtinsLib = std::unique_ptr<BuiltinsLib>(new BuiltinsLib());
    BuiltinCode code = builtinsLib->getBuiltinCode(EBuiltInOps::CopyBufferToBuffer, BuiltinCode::ECodeType::Any, *pDevice);
    EXPECT_EQ(BuiltinCode::ECodeType::Source, code.type);
    EXPECT_NE(0u, code.resource.size());
    EXPECT_EQ(pDevice, code.targetDevice);
}

using BuiltInOwnershipWrapperTests = BuiltInTests;

TEST_F(BuiltInOwnershipWrapperTests, givenBuiltinWhenConstructedThenLockAndUnlockOnDestruction) {
    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    {
        BuiltInOwnershipWrapper lock(mockAuxBuiltInOp);
        EXPECT_TRUE(mockAuxBuiltInOp.baseKernel->getMultiDeviceKernel()->hasOwnership());
    }
    EXPECT_FALSE(mockAuxBuiltInOp.baseKernel->getMultiDeviceKernel()->hasOwnership());
}

TEST_F(BuiltInOwnershipWrapperTests, givenLockWithoutParametersWhenConstructingThenLockOnlyWhenRequested) {
    MockAuxBuilInOp mockAuxBuiltInOp(*pBuiltIns, *pClDevice);
    {
        BuiltInOwnershipWrapper lock;
        lock.takeOwnership(mockAuxBuiltInOp);
        EXPECT_TRUE(mockAuxBuiltInOp.baseKernel->getMultiDeviceKernel()->hasOwnership());
    }
    EXPECT_FALSE(mockAuxBuiltInOp.baseKernel->getMultiDeviceKernel()->hasOwnership());
}

TEST_F(BuiltInOwnershipWrapperTests, givenLockWithAcquiredOwnershipWhenTakeOwnershipCalledThenAbort) {
    MockAuxBuilInOp mockAuxBuiltInOp1(*pBuiltIns, *pClDevice);
    MockAuxBuilInOp mockAuxBuiltInOp2(*pBuiltIns, *pClDevice);

    BuiltInOwnershipWrapper lock(mockAuxBuiltInOp1);
    EXPECT_THROW(lock.takeOwnership(mockAuxBuiltInOp1), std::exception);
    EXPECT_THROW(lock.takeOwnership(mockAuxBuiltInOp2), std::exception);
}

HWTEST_F(BuiltInOwnershipWrapperTests, givenBuiltInOwnershipWrapperWhenAskedForTypeTraitsThenDisableCopyConstructorAndOperator) {
    EXPECT_FALSE(std::is_copy_constructible<BuiltInOwnershipWrapper>::value);
    EXPECT_FALSE(std::is_copy_assignable<BuiltInOwnershipWrapper>::value);
}
