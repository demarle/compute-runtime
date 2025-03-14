/*
 * Copyright (C) 2020-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/command_stream/preemption_mode.h"
#include "shared/source/helpers/common_types.h"
#include "shared/source/helpers/engine_node_helper.h"
#include "shared/source/utilities/reference_tracked_object.h"

#include "opencl/source/api/cl_types.h"
#include "opencl/source/cl_device/cl_device_info.h"
#include "opencl/source/helpers/base_object.h"

#include "engine_node.h"
#include "igfxfmid.h"

#include <vector>

namespace NEO {
class Debugger;
class Device;
class DriverInfo;
class ExecutionEnvironment;
class GmmHelper;
class GmmClientContext;
class MemoryManager;
class PerformanceCounters;
class Platform;
class SourceLevelDebugger;
class SyncBufferHandler;
struct DeviceInfo;
struct EngineControl;
struct HardwareCapabilities;
struct HardwareInfo;
struct RootDeviceEnvironment;

template <>
struct OpenCLObjectMapper<_cl_device_id> {
    typedef class ClDevice DerivedType;
};

class ClDevice : public BaseObject<_cl_device_id> {
  public:
    static const cl_ulong objectMagic = 0x8055832341AC8D08LL;

    ClDevice &operator=(const ClDevice &) = delete;
    ClDevice(const ClDevice &) = delete;

    explicit ClDevice(Device &device, Platform *platformId);
    ~ClDevice() override;

    void incRefInternal();
    unique_ptr_if_unused<ClDevice> decRefInternal();

    unsigned int getEnabledClVersion() const { return enabledClVersion; };
    bool areOcl21FeaturesEnabled() const { return ocl21FeaturesEnabled; };
    bool isOcl21Conformant() const;

    void retainApi();
    unique_ptr_if_unused<ClDevice> releaseApi();

    bool getDeviceAndHostTimer(uint64_t *deviceTimestamp, uint64_t *hostTimestamp) const;
    bool getHostTimer(uint64_t *hostTimestamp) const;
    const HardwareInfo &getHardwareInfo() const;
    EngineControl &getEngine(aub_stream::EngineType engineType, EngineUsage engineUsage);
    EngineControl &getDefaultEngine();
    EngineControl &getInternalEngine();
    std::atomic<uint32_t> &getSelectorCopyEngine();
    MemoryManager *getMemoryManager() const;
    GmmHelper *getGmmHelper() const;
    GmmClientContext *getGmmClientContext() const;
    double getProfilingTimerResolution();
    double getPlatformHostTimerResolution() const;
    bool isSimulation() const;
    GFXCORE_FAMILY getRenderCoreFamily() const;
    void allocateSyncBufferHandler();
    PerformanceCounters *getPerformanceCounters();
    PreemptionMode getPreemptionMode() const;
    bool isDebuggerActive() const;
    Debugger *getDebugger();
    SourceLevelDebugger *getSourceLevelDebugger();
    ExecutionEnvironment *getExecutionEnvironment() const;
    const RootDeviceEnvironment &getRootDeviceEnvironment() const;
    const HardwareCapabilities &getHardwareCapabilities() const;
    bool isFullRangeSvm() const;
    bool areSharedSystemAllocationsAllowed() const;
    uint32_t getRootDeviceIndex() const;
    uint32_t getNumAvailableDevices() const;

    // API entry points
    cl_int getDeviceInfo(cl_device_info paramName,
                         size_t paramValueSize,
                         void *paramValue,
                         size_t *paramValueSizeRet);

    bool getDeviceInfoForImage(cl_device_info paramName,
                               const void *&src,
                               size_t &srcSize,
                               size_t &retSize);

    // This helper template is meant to simplify getDeviceInfo
    template <cl_device_info Param>
    void getCap(const void *&src,
                size_t &size,
                size_t &retSize);

    template <cl_device_info Param>
    void getStr(const void *&src,
                size_t &size,
                size_t &retSize);

    Device &getDevice() const noexcept { return device; }
    const ClDeviceInfo &getDeviceInfo() const { return deviceInfo; }
    const DeviceInfo &getSharedDeviceInfo() const;
    ClDevice *getDeviceById(uint32_t deviceId);
    const std::string &peekCompilerExtensions() const;
    const std::string &peekCompilerExtensionsWithFeatures() const;
    std::unique_ptr<SyncBufferHandler> syncBufferHandler;
    DeviceBitfield getDeviceBitfield() const;
    bool isDeviceEnqueueSupported() const;
    bool arePipesSupported() const;

    static cl_command_queue_capabilities_intel getQueueFamilyCapabilitiesAll();
    MOCKABLE_VIRTUAL cl_command_queue_capabilities_intel getQueueFamilyCapabilities(EngineGroupType type);
    void getQueueFamilyName(char *outputName, size_t maxOutputNameLength, EngineGroupType type);

  protected:
    void initializeCaps();
    void initializeExtensionsWithVersion();
    void initializeOpenclCAllVersions();
    void initializeOsSpecificCaps();
    void setupFp64Flags();
    const std::string getClDeviceName(const HardwareInfo &hwInfo) const;

    Device &device;
    std::vector<std::unique_ptr<ClDevice>> subDevices;
    cl_platform_id platformId;

    std::string name;
    std::unique_ptr<DriverInfo> driverInfo;
    unsigned int enabledClVersion = 0u;
    bool ocl21FeaturesEnabled = false;
    std::string deviceExtensions;
    std::string exposedBuiltinKernels = "";

    ClDeviceInfo deviceInfo = {};
    std::once_flag initializeExtensionsWithVersionOnce;

    std::vector<unsigned int> simultaneousInterops = {0};
    std::string compilerExtensions;
    std::string compilerExtensionsWithFeatures;
};

} // namespace NEO
