/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "opencl/source/kernel/kernel.h"
#include "opencl/test/unit_test/fixtures/cl_device_fixture.h"
#include "opencl/test/unit_test/fixtures/memory_management_fixture.h"
#include "opencl/test/unit_test/helpers/kernel_binary_helper.h"
#include "opencl/test/unit_test/mocks/mock_kernel.h"
#include "opencl/test/unit_test/mocks/mock_program.h"
#include "opencl/test/unit_test/program/program_tests.h"
#include "opencl/test/unit_test/program/program_with_source.h"
#include "test.h"

using namespace NEO;

class KernelArgInfoTest : public ProgramFromSourceTest {
  public:
    KernelArgInfoTest() {
    }

    ~KernelArgInfoTest() override = default;

  protected:
    void SetUp() override {
        kbHelper = new KernelBinaryHelper("copybuffer", true);
        ProgramFromSourceTest::SetUp();
        ASSERT_NE(nullptr, pProgram);
        ASSERT_EQ(CL_SUCCESS, retVal);

        retVal = pProgram->build(
            pProgram->getDevices(),
            nullptr,
            false);
        ASSERT_EQ(CL_SUCCESS, retVal);

        // create a kernel
        pKernel = Kernel::create(
            pProgram,
            pProgram->getKernelInfosForKernel(kernelName),
            *pPlatform->getClDevice(0),
            &retVal);

        ASSERT_EQ(CL_SUCCESS, retVal);
        ASSERT_NE(nullptr, pKernel);
    }

    void TearDown() override {
        delete pKernel;
        pKernel = nullptr;
        ProgramFromSourceTest::TearDown();
        delete kbHelper;
    }

    template <typename T>
    void queryArgInfo(cl_kernel_arg_info paramName, T &paramValue) {
        size_t paramValueSize = 0;
        size_t param_value_size_ret = 0;

        // get size
        retVal = pKernel->getArgInfo(
            0,
            paramName,
            paramValueSize,
            nullptr,
            &param_value_size_ret);
        EXPECT_NE(0u, param_value_size_ret);
        ASSERT_EQ(CL_SUCCESS, retVal);

        // get the name
        paramValueSize = param_value_size_ret;

        retVal = pKernel->getArgInfo(
            0,
            paramName,
            paramValueSize,
            &paramValue,
            nullptr);
        ASSERT_EQ(CL_SUCCESS, retVal);
    }

    Kernel *pKernel = nullptr;
    cl_int retVal = CL_SUCCESS;
    KernelBinaryHelper *kbHelper = nullptr;
};

TEST_F(KernelArgInfoTest, GivenNullWhenGettingKernelInfoThenNullIsReturned) {
    auto kernelInfo = this->pProgram->getKernelInfo(nullptr, 0);
    EXPECT_EQ(nullptr, kernelInfo);
}

TEST_F(KernelArgInfoTest, GivenInvalidParametersWhenGettingKernelArgInfoThenValueSizeRetIsNotUpdated) {
    size_t paramValueSizeRet = 0x1234;

    retVal = pKernel->getArgInfo(
        0,
        0,
        0,
        nullptr,
        &paramValueSizeRet);
    EXPECT_EQ(CL_INVALID_VALUE, retVal);
    EXPECT_EQ(0x1234u, paramValueSizeRet);
}

TEST_F(KernelArgInfoTest, GivenKernelArgAccessQualifierWhenQueryingArgInfoThenKernelArgAcessNoneIsReturned) {
    cl_kernel_arg_access_qualifier param_value = 0;
    queryArgInfo<cl_kernel_arg_access_qualifier>(CL_KERNEL_ARG_ACCESS_QUALIFIER, param_value);
    EXPECT_EQ(static_cast<cl_kernel_arg_access_qualifier>(CL_KERNEL_ARG_ACCESS_NONE), param_value);
}

TEST_F(KernelArgInfoTest, GivenKernelArgAddressQualifierWhenQueryingArgInfoThenKernelArgAddressGlobalIsReturned) {
    cl_kernel_arg_address_qualifier param_value = 0;
    queryArgInfo<cl_kernel_arg_address_qualifier>(CL_KERNEL_ARG_ADDRESS_QUALIFIER, param_value);
    EXPECT_EQ(static_cast<cl_kernel_arg_address_qualifier>(CL_KERNEL_ARG_ADDRESS_GLOBAL), param_value);
}

TEST_F(KernelArgInfoTest, GivenKernelArgTypeQualifierWhenQueryingArgInfoThenKernelArgTypeNoneIsReturned) {
    cl_kernel_arg_type_qualifier param_value = 0;
    queryArgInfo<cl_kernel_arg_type_qualifier>(CL_KERNEL_ARG_TYPE_QUALIFIER, param_value);
    EXPECT_EQ(static_cast<cl_kernel_arg_type_qualifier>(CL_KERNEL_ARG_TYPE_NONE), param_value);
}

TEST_F(KernelArgInfoTest, GivenParamWhenGettingKernelTypeNameThenCorrectValueIsReturned) {
    cl_kernel_arg_info paramName = CL_KERNEL_ARG_TYPE_NAME;
    char *paramValue = nullptr;
    size_t paramValueSize = 0;
    size_t paramValueSizeRet = 0;

    // get size
    retVal = pKernel->getArgInfo(
        0,
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

    retVal = pKernel->getArgInfo(
        0,
        paramName,
        paramValueSize,
        paramValue,
        nullptr);
    ASSERT_EQ(CL_SUCCESS, retVal);
    const char expectedString[] = "uint*";
    auto result = strncmp(paramValue, expectedString, sizeof(expectedString));
    EXPECT_EQ(0, result);
    delete[] paramValue;
}

TEST_F(KernelArgInfoTest, GivenParamWhenGettingKernelArgNameThenCorrectValueIsReturned) {
    cl_kernel_arg_info paramName = CL_KERNEL_ARG_NAME;
    char *paramValue = nullptr;
    size_t paramValueSize = 0;
    size_t paramValueSizeRet = 0;

    // get size
    retVal = pKernel->getArgInfo(
        0,
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

    retVal = pKernel->getArgInfo(
        0,
        paramName,
        paramValueSize,
        paramValue,
        nullptr);
    ASSERT_EQ(CL_SUCCESS, retVal);
    EXPECT_EQ(0, strcmp(paramValue, "src"));
    delete[] paramValue;
}
