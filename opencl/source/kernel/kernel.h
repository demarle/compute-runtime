/*
 * Copyright (C) 2017-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/command_stream/command_stream_receiver_hw.h"
#include "shared/source/command_stream/thread_arbitration_policy.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/device/device.h"
#include "shared/source/helpers/address_patch.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/helpers/timestamp_packet.h"
#include "shared/source/unified_memory/unified_memory.h"
#include "shared/source/utilities/stackvec.h"

#include "opencl/extensions/public/cl_ext_private.h"
#include "opencl/source/api/cl_types.h"
#include "opencl/source/cl_device/cl_device.h"
#include "opencl/source/device_queue/device_queue.h"
#include "opencl/source/helpers/base_object.h"
#include "opencl/source/helpers/properties_helper.h"
#include "opencl/source/kernel/kernel_execution_type.h"
#include "opencl/source/kernel/kernel_objects_for_aux_translation.h"
#include "opencl/source/program/kernel_info.h"
#include "opencl/source/program/program.h"

#include "csr_properties_flags.h"

#include <vector>

namespace NEO {
struct CompletionStamp;
class Buffer;
class CommandStreamReceiver;
class GraphicsAllocation;
class ImageTransformer;
class Surface;
class PrintfHandler;
class MultiDeviceKernel;

using KernelInfoContainer = StackVec<const KernelInfo *, 1>;

class Kernel : public ReferenceTrackedObject<Kernel> {
  public:
    static const uint32_t kernelBinaryAlignement = 64;

    enum kernelArgType {
        NONE_OBJ,
        IMAGE_OBJ,
        BUFFER_OBJ,
        PIPE_OBJ,
        SVM_OBJ,
        SVM_ALLOC_OBJ,
        SAMPLER_OBJ,
        ACCELERATOR_OBJ,
        DEVICE_QUEUE_OBJ,
        SLM_OBJ
    };

    struct SimpleKernelArgInfo {
        kernelArgType type;
        void *object;
        const void *value;
        size_t size;
        GraphicsAllocation *pSvmAlloc;
        cl_mem_flags svmFlags;
        bool isPatched = false;
        bool isStatelessUncacheable = false;
    };

    enum class TunningStatus {
        STANDARD_TUNNING_IN_PROGRESS,
        SUBDEVICE_TUNNING_IN_PROGRESS,
        TUNNING_DONE
    };

    enum class TunningType {
        DISABLED,
        SIMPLE,
        FULL
    };

    typedef int32_t (Kernel::*KernelArgHandler)(uint32_t argIndex,
                                                size_t argSize,
                                                const void *argVal);

    template <typename kernel_t = Kernel, typename program_t = Program>
    static kernel_t *create(program_t *program, const KernelInfoContainer &kernelInfos, ClDevice &clDevice, cl_int *errcodeRet) {
        cl_int retVal;
        kernel_t *pKernel = nullptr;

        pKernel = new kernel_t(program, kernelInfos, clDevice);
        retVal = pKernel->initialize();

        if (retVal != CL_SUCCESS) {
            delete pKernel;
            pKernel = nullptr;
        }

        if (errcodeRet) {
            *errcodeRet = retVal;
        }

        if (FileLoggerInstance().enabled()) {
            std::string source;
            program->getSource(source);
            FileLoggerInstance().dumpKernel(kernelInfos[program->getDevices()[0]->getRootDeviceIndex()]->kernelDescriptor.kernelMetadata.kernelName, source);
        }

        return pKernel;
    }

    Kernel &operator=(const Kernel &) = delete;
    Kernel(const Kernel &) = delete;

    virtual ~Kernel();

    static bool isMemObj(kernelArgType kernelArg) {
        return kernelArg == BUFFER_OBJ || kernelArg == IMAGE_OBJ || kernelArg == PIPE_OBJ;
    }

    bool isAuxTranslationRequired() const { return auxTranslationRequired; }
    void setAuxTranslationRequired(bool onOff) { auxTranslationRequired = onOff; }
    void updateAuxTranslationRequired();

    char *getCrossThreadData(uint32_t rootDeviceIndex) const {
        return kernelDeviceInfos[rootDeviceIndex].crossThreadData;
    }

    uint32_t getCrossThreadDataSize(uint32_t rootDeviceIndex) const {
        return kernelDeviceInfos[rootDeviceIndex].crossThreadDataSize;
    }

    cl_int initialize();

    MOCKABLE_VIRTUAL cl_int cloneKernel(Kernel *pSourceKernel);

    MOCKABLE_VIRTUAL bool canTransformImages() const;
    MOCKABLE_VIRTUAL bool isPatched() const;

    // API entry points
    cl_int setArgument(uint32_t argIndex, size_t argSize, const void *argVal) { return setArg(argIndex, argSize, argVal); }
    cl_int setArgSvm(uint32_t argIndex, size_t svmAllocSize, void *svmPtr, GraphicsAllocation *svmAlloc, cl_mem_flags svmFlags);
    cl_int setArgSvmAlloc(uint32_t argIndex, void *svmPtr, GraphicsAllocation *svmAlloc);

    void setSvmKernelExecInfo(GraphicsAllocation *argValue);
    void clearSvmKernelExecInfo();

    cl_int getInfo(cl_kernel_info paramName, size_t paramValueSize,
                   void *paramValue, size_t *paramValueSizeRet) const;
    void getAdditionalInfo(cl_kernel_info paramName, const void *&paramValue, size_t &paramValueSizeRet) const;
    void getAdditionalWorkGroupInfo(cl_kernel_work_group_info paramName, const void *&paramValue, size_t &paramValueSizeRet, uint32_t rootDeviceIndex) const;

    cl_int getArgInfo(cl_uint argIndx, cl_kernel_arg_info paramName,
                      size_t paramValueSize, void *paramValue, size_t *paramValueSizeRet) const;

    cl_int getWorkGroupInfo(ClDevice &clDevice, cl_kernel_work_group_info paramName,
                            size_t paramValueSize, void *paramValue, size_t *paramValueSizeRet) const;

    cl_int getSubGroupInfo(ClDevice &device, cl_kernel_sub_group_info paramName,
                           size_t inputValueSize, const void *inputValue,
                           size_t paramValueSize, void *paramValue,
                           size_t *paramValueSizeRet) const;

    const void *getKernelHeap(uint32_t rootDeviceIndex) const;
    void *getSurfaceStateHeap(uint32_t rootDeviceIndex) const;
    const void *getDynamicStateHeap(uint32_t rootDeviceIndex) const;

    size_t getKernelHeapSize(uint32_t rootDeviceIndex) const;
    size_t getSurfaceStateHeapSize(uint32_t rootDeviceIndex) const;
    size_t getDynamicStateHeapSize(uint32_t rootDeviceIndex) const;
    size_t getNumberOfBindingTableStates(uint32_t rootDeviceIndex) const;
    size_t getBindingTableOffset(uint32_t rootDeviceIndex) const {
        return kernelDeviceInfos[rootDeviceIndex].localBindingTableOffset;
    }

    void resizeSurfaceStateHeap(uint32_t rootDeviceIndex, void *pNewSsh, size_t newSshSize, size_t newBindingTableCount, size_t newBindingTableOffset);

    void substituteKernelHeap(const Device &device, void *newKernelHeap, size_t newKernelHeapSize);
    bool isKernelHeapSubstituted(uint32_t rootDeviceIndex) const;
    uint64_t getKernelId(uint32_t rootDeviceIndex) const;
    void setKernelId(uint32_t rootDeviceIndex, uint64_t newKernelId);
    uint32_t getStartOffset() const;
    void setStartOffset(uint32_t offset);

    const std::vector<SimpleKernelArgInfo> &getKernelArguments() const {
        return kernelArguments;
    }

    size_t getKernelArgsNumber() const {
        return kernelArguments.size();
    }

    bool requiresSshForBuffers(uint32_t rootDeviceIndex) const {
        return getKernelInfo(rootDeviceIndex).requiresSshForBuffers;
    }

    const KernelInfo &getKernelInfo(uint32_t rootDeviceIndex) const {
        return *kernelInfos[rootDeviceIndex];
    }
    const KernelInfoContainer &getKernelInfos() const {
        return kernelInfos;
    }

    Context &getContext() const {
        return program->getContext();
    }

    Program *getProgram() const { return program; }

    uint32_t getScratchSize(uint32_t rootDeviceIndex) {
        return getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelAttributes.perThreadScratchSize[0];
    }

    uint32_t getPrivateScratchSize(uint32_t rootDeviceIndex) {
        return getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelAttributes.perThreadScratchSize[1];
    }

    void createReflectionSurface();
    template <bool mockable = false>
    void patchReflectionSurface(DeviceQueue *devQueue, PrintfHandler *printfHandler);

    void patchDefaultDeviceQueue(DeviceQueue *devQueue);
    void patchEventPool(DeviceQueue *devQueue);
    void patchBlocksSimdSize(uint32_t rootDeviceIndex);
    bool usesSyncBuffer(uint32_t rootDeviceIndex);
    void patchSyncBuffer(Device &device, GraphicsAllocation *gfxAllocation, size_t bufferOffset);
    void patchBindlessSurfaceStateOffsets(const Device &device, const size_t sshOffset);

    GraphicsAllocation *getKernelReflectionSurface() const {
        return kernelReflectionSurface;
    }

    size_t getInstructionHeapSizeForExecutionModel() const;

    // Helpers
    cl_int setArg(uint32_t argIndex, uint32_t argValue);
    cl_int setArg(uint32_t argIndex, uint64_t argValue);
    cl_int setArg(uint32_t argIndex, cl_mem argValue);
    cl_int setArg(uint32_t argIndex, cl_mem argValue, uint32_t mipLevel);
    cl_int setArg(uint32_t argIndex, size_t argSize, const void *argVal);

    // Handlers
    void setKernelArgHandler(uint32_t argIndex, KernelArgHandler handler);

    void unsetArg(uint32_t argIndex);

    cl_int setArgImmediate(uint32_t argIndex,
                           size_t argSize,
                           const void *argVal);

    cl_int setArgBuffer(uint32_t argIndex,
                        size_t argSize,
                        const void *argVal);

    cl_int setArgPipe(uint32_t argIndex,
                      size_t argSize,
                      const void *argVal);

    cl_int setArgImage(uint32_t argIndex,
                       size_t argSize,
                       const void *argVal);

    cl_int setArgImageWithMipLevel(uint32_t argIndex,
                                   size_t argSize,
                                   const void *argVal, uint32_t mipLevel);

    cl_int setArgLocal(uint32_t argIndex,
                       size_t argSize,
                       const void *argVal);

    cl_int setArgSampler(uint32_t argIndex,
                         size_t argSize,
                         const void *argVal);

    cl_int setArgAccelerator(uint32_t argIndex,
                             size_t argSize,
                             const void *argVal);

    cl_int setArgDevQueue(uint32_t argIndex,
                          size_t argSize,
                          const void *argVal);

    void storeKernelArg(uint32_t argIndex,
                        kernelArgType argType,
                        void *argObject,
                        const void *argValue,
                        size_t argSize,
                        GraphicsAllocation *argSvmAlloc = nullptr,
                        cl_mem_flags argSvmFlags = 0);
    const void *getKernelArg(uint32_t argIndex) const;
    const SimpleKernelArgInfo &getKernelArgInfo(uint32_t argIndex) const;

    bool getAllowNonUniform() const { return program->getAllowNonUniform(); }
    bool isVmeKernel() const { return getDefaultKernelInfo().isVmeWorkload; }
    bool requiresSpecialPipelineSelectMode() const { return specialPipelineSelectMode; }

    void performKernelTunning(CommandStreamReceiver &commandStreamReceiver, const Vec3<size_t> &lws, const Vec3<size_t> &gws, const Vec3<size_t> &offsets, TimestampPacketContainer *timestampContainer);
    MOCKABLE_VIRTUAL bool isSingleSubdevicePreferred() const;

    //residency for kernel surfaces
    MOCKABLE_VIRTUAL void makeResident(CommandStreamReceiver &commandStreamReceiver);
    MOCKABLE_VIRTUAL void getResidency(std::vector<Surface *> &dst, uint32_t rootDeviceIndex);
    bool requiresCoherency();
    void resetSharedObjectsPatchAddresses();
    bool isUsingSharedObjArgs() const { return usingSharedObjArgs; }
    bool hasUncacheableStatelessArgs() const { return statelessUncacheableArgsCount > 0; }

    bool hasPrintfOutput(uint32_t rootDeviceIndex) const;

    void setReflectionSurfaceBlockBtOffset(uint32_t blockID, uint32_t offset);

    cl_int checkCorrectImageAccessQualifier(cl_uint argIndex,
                                            size_t argSize,
                                            const void *argValue) const;

    static uint32_t dummyPatchLocation;

    uint32_t allBufferArgsStateful = CL_TRUE;

    bool isBuiltIn = false;
    const bool isParentKernel;
    const bool isSchedulerKernel;

    uint32_t getThreadArbitrationPolicy() const {
        return threadArbitrationPolicy;
    }
    KernelExecutionType getExecutionType() const {
        return executionType;
    }

    bool checkIfIsParentKernelAndBlocksUsesPrintf();

    bool is32Bit(uint32_t rootDeviceIndex) const {
        return getKernelInfo(rootDeviceIndex).gpuPointerSize == 4;
    }

    size_t getPerThreadSystemThreadSurfaceSize(uint32_t rootDeviceIndex) const {
        return getKernelInfo(rootDeviceIndex).kernelDescriptor.kernelAttributes.perThreadSystemThreadSurfaceSize;
    }

    std::vector<PatchInfoData> &getPatchInfoDataList() { return patchInfoDataList; };
    bool usesOnlyImages() const {
        return usingImagesOnly;
    }

    void fillWithKernelObjsForAuxTranslation(KernelObjsForAuxTranslation &kernelObjsForAuxTranslation, uint32_t rootDeviceIndex);

    MOCKABLE_VIRTUAL bool requiresCacheFlushCommand(const CommandQueue &commandQueue) const;

    using CacheFlushAllocationsVec = StackVec<GraphicsAllocation *, 32>;
    void getAllocationsForCacheFlush(CacheFlushAllocationsVec &out, uint32_t rootDeviceIndex) const;

    void setAuxTranslationDirection(AuxTranslationDirection auxTranslationDirection) {
        this->auxTranslationDirection = auxTranslationDirection;
    }
    void setUnifiedMemorySyncRequirement(bool isUnifiedMemorySyncRequired) {
        this->isUnifiedMemorySyncRequired = isUnifiedMemorySyncRequired;
    }
    void setUnifiedMemoryProperty(cl_kernel_exec_info infoType, bool infoValue);
    void setUnifiedMemoryExecInfo(GraphicsAllocation *argValue);
    void clearUnifiedMemoryExecInfo();

    bool areStatelessWritesUsed() { return containsStatelessWrites; }
    int setKernelThreadArbitrationPolicy(uint32_t propertyValue);
    cl_int setKernelExecutionType(cl_execution_info_kernel_type_intel executionType);
    void setThreadArbitrationPolicy(uint32_t policy) {
        this->threadArbitrationPolicy = policy;
    }
    void getSuggestedLocalWorkSize(const cl_uint workDim, const size_t *globalWorkSize, const size_t *globalWorkOffset,
                                   size_t *localWorkSize, ClDevice &clDevice);
    uint32_t getMaxWorkGroupCount(const cl_uint workDim, const size_t *localWorkSize, const CommandQueue *commandQueue) const;

    uint64_t getKernelStartOffset(
        const bool localIdsGenerationByRuntime,
        const bool kernelUsesLocalIds,
        const bool isCssUsed,
        uint32_t rootDeviceIndex) const;

    bool requiresPerDssBackedBuffer(uint32_t rootDeviceIndex) const;
    bool requiresLimitedWorkgroupSize(uint32_t rootDeviceIndex) const;
    bool isKernelDebugEnabled() const { return debugEnabled; }
    int32_t setAdditionalKernelExecInfoWithParam(uint32_t paramName, size_t paramValueSize, const void *paramValue);
    void setAdditionalKernelExecInfo(uint32_t additionalKernelExecInfo);
    uint32_t getAdditionalKernelExecInfo() const;
    MOCKABLE_VIRTUAL bool requiresWaDisableRccRhwoOptimization(uint32_t rootDeviceIndex) const;
    const ClDeviceVector &getDevices() const {
        return program->getDevices();
    }
    const KernelInfo &getDefaultKernelInfo() const;

    void setGlobalWorkOffsetValues(uint32_t rootDeviceIndex, uint32_t globalWorkOffsetX, uint32_t globalWorkOffsetY, uint32_t globalWorkOffsetZ);
    void setGlobalWorkSizeValues(uint32_t rootDeviceIndex, uint32_t globalWorkSizeX, uint32_t globalWorkSizeY, uint32_t globalWorkSizeZ);
    void setLocalWorkSizeValues(uint32_t rootDeviceIndex, uint32_t localWorkSizeX, uint32_t localWorkSizeY, uint32_t localWorkSizeZ);
    void setLocalWorkSize2Values(uint32_t rootDeviceIndex, uint32_t localWorkSizeX, uint32_t localWorkSizeY, uint32_t localWorkSizeZ);
    void setEnqueuedLocalWorkSizeValues(uint32_t rootDeviceIndex, uint32_t localWorkSizeX, uint32_t localWorkSizeY, uint32_t localWorkSizeZ);
    bool isLocalWorkSize2Patched(uint32_t rootDeviceIndex);
    void setNumWorkGroupsValues(uint32_t rootDeviceIndex, uint32_t numWorkGroupsX, uint32_t numWorkGroupsY, uint32_t numWorkGroupsZ);
    void setWorkDim(uint32_t rootDeviceIndex, uint32_t workDim);
    uint32_t getMaxKernelWorkGroupSize(uint32_t rootDeviceIndex) const;
    uint32_t getSlmTotalSize(uint32_t rootDeviceIndex) const;
    bool getHasIndirectAccess() const {
        return this->kernelHasIndirectAccess;
    }

    MultiDeviceKernel *getMultiDeviceKernel() const { return pMultiDeviceKernel; }
    void setMultiDeviceKernel(MultiDeviceKernel *pMultiDeviceKernelToSet) { pMultiDeviceKernel = pMultiDeviceKernelToSet; }

    size_t getTotalNumDevicesInContext() const;

  protected:
    struct ObjectCounts {
        uint32_t imageCount;
        uint32_t samplerCount;
    };

    class ReflectionSurfaceHelper {
      public:
        static const uint64_t undefinedOffset = (uint64_t)-1;

        static void setKernelDataHeader(void *reflectionSurface, uint32_t numberOfBlocks,
                                        uint32_t parentImages, uint32_t parentSamplers,
                                        uint32_t imageOffset, uint32_t samplerOffset) {
            IGIL_KernelDataHeader *kernelDataHeader = reinterpret_cast<IGIL_KernelDataHeader *>(reflectionSurface);
            kernelDataHeader->m_numberOfKernels = numberOfBlocks;
            kernelDataHeader->m_ParentKernelImageCount = parentImages;
            kernelDataHeader->m_ParentSamplerCount = parentSamplers;
            kernelDataHeader->m_ParentImageDataOffset = imageOffset;
            kernelDataHeader->m_ParentSamplerParamsOffset = samplerOffset;
        }

        static uint32_t setKernelData(void *reflectionSurface, uint32_t offset,
                                      std::vector<IGIL_KernelCurbeParams> &curbeParamsIn,
                                      uint64_t tokenMaskIn, size_t maxConstantBufferSize,
                                      size_t samplerCount, const KernelInfo &kernelInfo,
                                      const HardwareInfo &hwInfo);

        static void setKernelAddressData(void *reflectionSurface, uint32_t offset,
                                         uint32_t kernelDataOffset, uint32_t samplerHeapOffset,
                                         uint32_t constantBufferOffset, uint32_t samplerParamsOffset,
                                         uint32_t sshTokensOffset, uint32_t btOffset,
                                         const KernelInfo &kernelInfo, const HardwareInfo &hwInfo);

        static void getCurbeParams(std::vector<IGIL_KernelCurbeParams> &curbeParamsOut,
                                   uint64_t &tokenMaskOut, uint32_t &firstSSHTokenIndex,
                                   const KernelInfo &kernelInfo, const HardwareInfo &hwInfo);

        static bool compareFunction(IGIL_KernelCurbeParams argFirst, IGIL_KernelCurbeParams argSecond) {
            if (argFirst.m_parameterType == argSecond.m_parameterType) {
                if (argFirst.m_parameterType == iOpenCL::DATA_PARAMETER_LOCAL_WORK_SIZE) {
                    return argFirst.m_patchOffset < argSecond.m_patchOffset;
                } else {
                    return argFirst.m_sourceOffset < argSecond.m_sourceOffset;
                }
            } else {
                return argFirst.m_parameterType < argSecond.m_parameterType;
            }
        }

        static void setKernelAddressDataBtOffset(void *reflectionSurface, uint32_t blockID, uint32_t btOffset);

        static void setParentImageParams(void *reflectionSurface, std::vector<Kernel::SimpleKernelArgInfo> &parentArguments, const KernelInfo &parentKernelInfo);
        static void setParentSamplerParams(void *reflectionSurface, std::vector<Kernel::SimpleKernelArgInfo> &parentArguments, const KernelInfo &parentKernelInfo);

        template <bool mockable = false>
        static void patchBlocksCurbe(void *reflectionSurface, uint32_t blockID,
                                     uint64_t defaultDeviceQueueCurbeOffset, uint32_t patchSizeDefaultQueue, uint64_t defaultDeviceQueueGpuAddress,
                                     uint64_t eventPoolCurbeOffset, uint32_t patchSizeEventPool, uint64_t eventPoolGpuAddress,
                                     uint64_t deviceQueueCurbeOffset, uint32_t patchSizeDeviceQueue, uint64_t deviceQueueGpuAddress,
                                     uint64_t printfBufferOffset, uint32_t printfBufferSize, uint64_t printfBufferGpuAddress,
                                     uint64_t privateSurfaceOffset, uint32_t privateSurfaceSize, uint64_t privateSurfaceGpuAddress);

        static void patchBlocksCurbeWithConstantValues(void *reflectionSurface, uint32_t blockID,
                                                       uint64_t globalMemoryCurbeOffset, uint32_t globalMemoryPatchSize, uint64_t globalMemoryGpuAddress,
                                                       uint64_t constantMemoryCurbeOffset, uint32_t constantMemoryPatchSize, uint64_t constantMemoryGpuAddress,
                                                       uint64_t privateMemoryCurbeOffset, uint32_t privateMemoryPatchSize, uint64_t privateMemoryGpuAddress);
    };

    void
    makeArgsResident(CommandStreamReceiver &commandStreamReceiver);

    void *patchBufferOffset(const KernelArgInfo &argInfo, void *svmPtr, GraphicsAllocation *svmAlloc, uint32_t rootDeviceIndex);

    void patchWithImplicitSurface(void *ptrToPatchInCrossThreadData, GraphicsAllocation &allocation, const Device &device, const ArgDescPointer &arg);
    // Sets-up both crossThreadData and ssh for given implicit (private/constant, etc.) allocation
    template <typename PatchTokenT>
    void patchWithImplicitSurface(void *ptrToPatchInCrossThreadData, GraphicsAllocation &allocation, const Device &device, const PatchTokenT &patch);

    void getParentObjectCounts(ObjectCounts &objectCount);
    Kernel(Program *programArg, const KernelInfoContainer &kernelInfsoArg, ClDevice &clDevice, bool schedulerKernel = false);
    void provideInitializationHints();

    void patchBlocksCurbeWithConstantValues();

    void resolveArgs();

    void reconfigureKernel(uint32_t rootDeviceIndex);
    bool hasDirectStatelessAccessToHostMemory() const;
    bool hasIndirectStatelessAccessToHostMemory() const;

    void addAllocationToCacheFlushVector(uint32_t argIndex, GraphicsAllocation *argAllocation);
    bool allocationForCacheFlush(GraphicsAllocation *argAllocation) const;

    const HardwareInfo &getHardwareInfo(uint32_t rootDeviceIndex) const;

    const ClDevice &getDevice() const {
        return clDevice;
    }

    const ExecutionEnvironment &executionEnvironment;
    Program *program;
    ClDevice &clDevice;
    const ClDeviceVector &deviceVector;
    const KernelInfoContainer kernelInfos;

    std::vector<SimpleKernelArgInfo> kernelArguments;
    std::vector<KernelArgHandler> kernelArgHandlers;
    std::vector<GraphicsAllocation *> kernelSvmGfxAllocations;
    std::vector<GraphicsAllocation *> kernelUnifiedMemoryGfxAllocations;

    AuxTranslationDirection auxTranslationDirection = AuxTranslationDirection::None;

    GraphicsAllocation *kernelReflectionSurface = nullptr;

    bool usingSharedObjArgs = false;
    bool usingImagesOnly = false;
    bool auxTranslationRequired = false;
    bool containsStatelessWrites = true;
    uint32_t patchedArgumentsNum = 0;
    uint32_t startOffset = 0;
    uint32_t statelessUncacheableArgsCount = 0;
    uint32_t threadArbitrationPolicy = ThreadArbitrationPolicy::NotPresent;
    KernelExecutionType executionType = KernelExecutionType::Default;

    std::vector<PatchInfoData> patchInfoDataList;
    std::unique_ptr<ImageTransformer> imageTransformer;

    bool specialPipelineSelectMode = false;
    bool svmAllocationsRequireCacheFlush = false;
    std::vector<GraphicsAllocation *> kernelArgRequiresCacheFlush;
    UnifiedMemoryControls unifiedMemoryControls{};
    bool isUnifiedMemorySyncRequired = true;
    bool debugEnabled = false;
    uint32_t additionalKernelExecInfo = AdditionalKernelExecInfo::NotSet;

    struct KernelDeviceInfo : public NonCopyableClass {
        uint32_t *globalWorkOffsetX = &Kernel::dummyPatchLocation;
        uint32_t *globalWorkOffsetY = &Kernel::dummyPatchLocation;
        uint32_t *globalWorkOffsetZ = &Kernel::dummyPatchLocation;

        uint32_t *localWorkSizeX = &Kernel::dummyPatchLocation;
        uint32_t *localWorkSizeY = &Kernel::dummyPatchLocation;
        uint32_t *localWorkSizeZ = &Kernel::dummyPatchLocation;

        uint32_t *localWorkSizeX2 = &Kernel::dummyPatchLocation;
        uint32_t *localWorkSizeY2 = &Kernel::dummyPatchLocation;
        uint32_t *localWorkSizeZ2 = &Kernel::dummyPatchLocation;

        uint32_t *globalWorkSizeX = &Kernel::dummyPatchLocation;
        uint32_t *globalWorkSizeY = &Kernel::dummyPatchLocation;
        uint32_t *globalWorkSizeZ = &Kernel::dummyPatchLocation;

        uint32_t *enqueuedLocalWorkSizeX = &Kernel::dummyPatchLocation;
        uint32_t *enqueuedLocalWorkSizeY = &Kernel::dummyPatchLocation;
        uint32_t *enqueuedLocalWorkSizeZ = &Kernel::dummyPatchLocation;

        uint32_t *numWorkGroupsX = &Kernel::dummyPatchLocation;
        uint32_t *numWorkGroupsY = &Kernel::dummyPatchLocation;
        uint32_t *numWorkGroupsZ = &Kernel::dummyPatchLocation;

        uint32_t *maxWorkGroupSizeForCrossThreadData = &Kernel::dummyPatchLocation;
        uint32_t maxKernelWorkGroupSize = 0;
        uint32_t *workDim = &Kernel::dummyPatchLocation;
        uint32_t *dataParameterSimdSize = &Kernel::dummyPatchLocation;
        uint32_t *parentEventOffset = &Kernel::dummyPatchLocation;
        uint32_t *preferredWkgMultipleOffset = &Kernel::dummyPatchLocation;

        size_t numberOfBindingTableStates = 0u;
        size_t localBindingTableOffset = 0u;

        std::vector<size_t> slmSizes;
        uint32_t slmTotalSize = 0u;

        std::unique_ptr<char[]> pSshLocal;
        uint32_t sshLocalSize = 0u;
        char *crossThreadData = nullptr;
        uint32_t crossThreadDataSize = 0u;

        GraphicsAllocation *privateSurface = nullptr;
        uint64_t privateSurfaceSize = 0u;
    };
    std::vector<KernelDeviceInfo> kernelDeviceInfos;
    const uint32_t defaultRootDeviceIndex;

    struct KernelConfig {
        Vec3<size_t> gws;
        Vec3<size_t> lws;
        Vec3<size_t> offsets;
        bool operator==(const KernelConfig &other) const { return this->gws == other.gws && this->lws == other.lws && this->offsets == other.offsets; }
    };
    struct KernelConfigHash {
        size_t operator()(KernelConfig const &config) const {
            auto hash = std::hash<size_t>{};
            size_t gwsHashX = hash(config.gws.x);
            size_t gwsHashY = hash(config.gws.y);
            size_t gwsHashZ = hash(config.gws.z);
            size_t gwsHash = hashCombine(gwsHashX, gwsHashY, gwsHashZ);
            size_t lwsHashX = hash(config.lws.x);
            size_t lwsHashY = hash(config.lws.y);
            size_t lwsHashZ = hash(config.lws.z);
            size_t lwsHash = hashCombine(lwsHashX, lwsHashY, lwsHashZ);
            size_t offsetsHashX = hash(config.offsets.x);
            size_t offsetsHashY = hash(config.offsets.y);
            size_t offsetsHashZ = hash(config.offsets.z);
            size_t offsetsHash = hashCombine(offsetsHashX, offsetsHashY, offsetsHashZ);
            return hashCombine(gwsHash, lwsHash, offsetsHash);
        }

        size_t hashCombine(size_t hash1, size_t hash2, size_t hash3) const {
            return (hash1 ^ (hash2 << 1u)) ^ (hash3 << 2u);
        }
    };
    struct KernelSubmissionData {
        std::unique_ptr<TimestampPacketContainer> kernelStandardTimestamps;
        std::unique_ptr<TimestampPacketContainer> kernelSubdeviceTimestamps;
        TunningStatus status;
        bool singleSubdevicePrefered = false;
    };

    bool hasTunningFinished(KernelSubmissionData &submissionData);
    bool hasRunFinished(TimestampPacketContainer *timestampContainer);

    std::unordered_map<KernelConfig, KernelSubmissionData, KernelConfigHash> kernelSubmissionMap;
    bool singleSubdevicePreferedInCurrentEnqueue = false;

    bool kernelHasIndirectAccess = true;
    MultiDeviceKernel *pMultiDeviceKernel = nullptr;
};

} // namespace NEO
