/*
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "opencl/source/kernel/kernel.h"

namespace NEO {
template <>
struct OpenCLObjectMapper<_cl_kernel> {
    typedef class MultiDeviceKernel DerivedType;
};

using KernelVectorType = StackVec<Kernel *, 4>;

class MultiDeviceKernel : public BaseObject<_cl_kernel> {
  public:
    static const cl_ulong objectMagic = 0x3284ADC8EA0AFE25LL;

    ~MultiDeviceKernel() override;
    MultiDeviceKernel(KernelVectorType kernelVector);

    Kernel *getKernel(uint32_t rootDeviceIndex) const { return kernels[rootDeviceIndex]; }
    Kernel *getDefaultKernel() const { return defaultKernel; }

    template <typename kernel_t = Kernel, typename program_t = Program, typename multi_device_kernel_t = MultiDeviceKernel>
    static multi_device_kernel_t *create(program_t *program, const KernelInfoContainer &kernelInfos, cl_int *errcodeRet) {
        KernelVectorType kernels{};
        kernels.resize(program->getMaxRootDeviceIndex() + 1);

        for (auto &pDevice : program->getDevices()) {
            auto rootDeviceIndex = pDevice->getRootDeviceIndex();
            if (kernels[rootDeviceIndex]) {
                continue;
            }
            kernels[rootDeviceIndex] = Kernel::create<kernel_t, program_t>(program, kernelInfos, *pDevice, errcodeRet);
        }
        auto pMultiDeviceKernel = new multi_device_kernel_t(std::move(kernels));

        return pMultiDeviceKernel;
    }

    cl_int cloneKernel(MultiDeviceKernel *pSourceMultiDeviceKernel);
    const std::vector<Kernel::SimpleKernelArgInfo> &getKernelArguments() const;
    cl_int checkCorrectImageAccessQualifier(cl_uint argIndex, size_t argSize, const void *argValue) const;
    void unsetArg(uint32_t argIndex);
    cl_int setArg(uint32_t argIndex, size_t argSize, const void *argVal);
    cl_int getInfo(cl_kernel_info paramName, size_t paramValueSize, void *paramValue, size_t *paramValueSizeRet) const;
    cl_int getArgInfo(cl_uint argIndx, cl_kernel_arg_info paramName, size_t paramValueSize, void *paramValue, size_t *paramValueSizeRet) const;
    const ClDeviceVector &getDevices() const;
    size_t getKernelArgsNumber() const;
    Context &getContext() const;
    cl_int setArgSvmAlloc(uint32_t argIndex, void *svmPtr, GraphicsAllocation *svmAlloc);
    bool getHasIndirectAccess() const;
    void setUnifiedMemoryProperty(cl_kernel_exec_info infoType, bool infoValue);
    void setSvmKernelExecInfo(GraphicsAllocation *argValue);
    void clearSvmKernelExecInfo();
    void setUnifiedMemoryExecInfo(GraphicsAllocation *argValue);
    void clearUnifiedMemoryExecInfo();
    int setKernelThreadArbitrationPolicy(uint32_t propertyValue);
    cl_int setKernelExecutionType(cl_execution_info_kernel_type_intel executionType);
    int32_t setAdditionalKernelExecInfoWithParam(uint32_t paramName, size_t paramValueSize, const void *paramValue);
    Program *getProgram() const { return program; }
    const KernelInfoContainer &getKernelInfos() const { return kernelInfos; }

  protected:
    template <typename FuncType, typename... Args>
    cl_int getResultFromEachKernel(FuncType function, Args &&...args) const {
        cl_int retVal = CL_INVALID_VALUE;

        for (auto &pKernel : kernels) {
            if (pKernel) {
                retVal = (pKernel->*function)(std::forward<Args>(args)...);
                if (CL_SUCCESS != retVal) {
                    break;
                }
            }
        }
        return retVal;
    }
    template <typename FuncType, typename... Args>
    void callOnEachKernel(FuncType function, Args &&...args) {
        for (auto &pKernel : kernels) {
            if (pKernel) {
                (pKernel->*function)(std::forward<Args>(args)...);
            }
        }
    }
    static Kernel *determineDefaultKernel(KernelVectorType &kernelVector);
    KernelVectorType kernels;
    Kernel *defaultKernel = nullptr;
    Program *program = nullptr;
    const KernelInfoContainer &kernelInfos;
};

} // namespace NEO
