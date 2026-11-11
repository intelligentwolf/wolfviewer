/**
 * @file wolfvkdevice.h
 * @brief Vulkan instance, physical device choice, logical device, queues, per-frame command pools and sync.
 *
 * [VULKAN BACKEND 2026-11-08] stage 4 of ~/wolfviewer-releases/vulkan-plan.md.
 *
 * One WolfVkDevice exists for the viewer (gWolfVkDevice). Life cycle:
 *   1. createInstance(config)      - loader library, instance, validation layer + debug messenger when asked
 *   2. (caller creates the window surface with WolfVkSurface::create)
 *   3. createDevice(surface)       - physical device choice, one graphics queue that can present to the surface,
 *                                    allocator, frames in flight, timeline semaphore when available
 *   4. beginFrame()/endFrame()     - per frame, by the swapchain owner
 *   5. destroy()
 *
 * Physical device choice (in this order, first match wins):
 *   a. WolfVulkanDevice setting: a case-insensitive part of the device name, e.g. "radeon" or "nvidia"
 *   b. the device whose VkPhysicalDeviceIDProperties::deviceUUID (vulkan_core.h:5941) equals the OpenGL context's
 *      GL_DEVICE_UUID_EXT (GL_EXT_memory_object, glext.h:7736): the very card OpenGL is drawing with, which the
 *      staged hand-over (GL and Vulkan sharing images) needs. Proven on NVIDIA and radeonsi by the vkglshare probe.
 *      [W3 2026-11-11] When OpenGL gave its UUID and no device has it, c. decides (logged).
 *   c. the device whose vendor matches the OpenGL renderer's vendor (gGLManager.mIsNVIDIA/mIsAMD/mIsIntel); [V7] when the
 *      vendor is known and no device matches, the choice fails likewise
 *   d. the first VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU (vulkan_core.h:2235)
 *   e. the first device
 * Devices without VK_KHR_swapchain, without a graphics queue that can present, or of VK_PHYSICAL_DEVICE_TYPE_CPU
 * (vulkan_core.h:2237, llvmpipe/lavapipe/SwiftShader; [V7]) are skipped.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_VK_DEVICE_H
#define WOLF_VK_DEVICE_H

#include "wolfvkloader.h"
#include "wolfvkallocator.h"
#include <functional>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

struct WolfVkPhysicalDeviceInfo
{
    VkPhysicalDevice mHandle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties mProperties{};           // vulkan_core.h:3708
    VkPhysicalDeviceIDProperties mIds{};                // vulkan_core.h:5941 (Vulkan 1.1; zero on a 1.0 device)
    VkPhysicalDeviceDriverProperties mDriver{};         // vulkan_core.h:6475 (Vulkan 1.2 or VK_KHR_driver_properties; zero otherwise)
    VkPhysicalDeviceMemoryProperties mMemory{};         // vulkan_core.h:3693
    std::vector<VkQueueFamilyProperties> mQueueFamilies; // vulkan_core.h:3720
    bool mHasSwapchain = false;                          // VK_KHR_swapchain present
    bool mHasTimelineSemaphore = false;                  // Vulkan 1.2 feature or VK_KHR_timeline_semaphore
    bool mHasPortabilitySubset = false;                  // MoltenVK: VK_KHR_portability_subset must be enabled when present
    bool mHasDriverProperties = false;
    bool mHasDedicatedAllocation = false;                // Vulkan 1.1 or VK_KHR_dedicated_allocation + get_memory_requirements2
    // [SHARED IMAGES 2026-11-08] images and semaphores shared with the OpenGL context: VK_KHR_external_memory_fd +
    // VK_KHR_external_semaphore_fd (Linux, vulkan_core.h:10579/:10664) or VK_KHR_external_memory_win32 +
    // VK_KHR_external_semaphore_win32 (Windows, vulkan_win32.h:58/:130). The capability structures are Vulkan 1.1 core.
    bool mHasExternalSharing = false;
    // [PRESENT FENCES 2026-11-08] VK_KHR_swapchain_maintenance1 (vulkan_core.h:13440) or its EXT original (:19608), with
    // the swapchainMaintenance1 feature (:13441) and the instance's matching surface_maintenance1 enabled: presents can
    // carry a fence (VkSwapchainPresentFenceInfoKHR :13447). The extension name to enable, or nullptr.
    const char* mSwapchainMaintenance1 = nullptr;
    // [GL DEPTH 2026-11-08] VK_EXT_depth_clip_control (vulkan_core.h:21208) with its depthClipControl feature (:21209):
    // pipelines can take GL's clip-space z range [-w, w] as is (VkPipelineViewportDepthClipControlCreateInfoEXT :21215)
    bool mHasDepthClipControl = false;
    // [AMD PERF 2026-11-09] VK_EXT_image_compression_control (vulkan_core.h:20917; vk.xml: depends
    // "VK_KHR_get_physical_device_properties2,VK_VERSION_1_1") is offered, with its imageCompressionControl feature (:20957)
    bool mHasImageCompressionControl = false;
    bool mImageCompressionControlFeature = false;
    uint32_t mGraphicsPresentFamily = UINT32_MAX;        // chosen queue family, set by createDevice

    std::string name() const { return mProperties.deviceName; }
    std::string driverString() const;      // "NVIDIA 615.78.08" / "Mesa 26.2.5-arch1.1 (radv)" / vendor's packed version
    std::string apiVersionString() const;  // "1.4.351"
    const char* typeString() const;        // "discrete", "integrated", ...
};

class WolfVkDevice
{
public:
    struct Config
    {
        bool mValidation = false;              // VK_LAYER_KHRONOS_validation + VK_EXT_debug_utils
        std::string mDeviceOverride;           // WolfVulkanDevice setting (part of a device name), empty = automatic
        bool mHaveGLDeviceUUID = false;        // glGetUnsignedBytevEXT(GL_DEVICE_UUID_EXT) was available
        uint8_t mGLDeviceUUID[VK_UUID_SIZE] = {};
        uint32_t mGLVendorID = 0;              // PCI vendor of the GL renderer when known: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
        uint32_t mFramesInFlight = 2;          // command pools / fences rotated per frame
        std::string mApplicationName = "WolfViewer";
        uint32_t mApplicationVersion = 0;
    };

    // PCI vendor ids as reported in VkPhysicalDeviceProperties::vendorID (vulkan_core.h:3711); the values are the
    // PCI-SIG assignments (https://pcisig.com/membership/member-companies): NVIDIA 0x10DE, AMD/ATI 0x1002, Intel 0x8086.
    static constexpr uint32_t VENDOR_NVIDIA = 0x10DE;
    static constexpr uint32_t VENDOR_AMD = 0x1002;
    static constexpr uint32_t VENDOR_INTEL = 0x8086;
    static constexpr uint32_t VENDOR_APPLE = 0x106B;

    struct Frame
    {
        VkCommandPool mPool = VK_NULL_HANDLE;
        VkCommandBuffer mCmd = VK_NULL_HANDLE;
        VkFence mFence = VK_NULL_HANDLE;            // signalled when this frame's submit finished
        VkSemaphore mAcquire = VK_NULL_HANDLE;      // swapchain image acquired (binary)
        uint64_t mTimelineValue = 0;                // value the timeline reaches when this frame finished
        std::vector<std::function<void()>> mDeferred; // destruction queue, run when the slot is reused
        bool mSubmitted = false;
        // [VULKAN G-BUFFER 2026-11-08] mid-frame command buffers (one per submit before the frame's own), from mPool
        std::vector<VkCommandBuffer> mMidCmds;
        uint32_t mMidUsed = 0;
        // [HANG GUARD 2026-11-08 8.2k] mFence was handed to a vkQueueSubmit since the slot's last beginFrame. mSubmitted
        // alone is not enough: submitMidFrame sets it with no fence, and waiting a reset fence nobody submitted can only
        // time out (the 8 s guard would then report a hang that is not one).
        bool mFenceSubmitted = false;
        // Devices without a timeline semaphore: one fence per mid-frame submit, so beginFrame and waitIdle can wait for
        // those submits with a timeout instead of relying on the frame fence alone.
        std::vector<VkFence> mMidFences;
        uint32_t mMidFencesUsed = 0;
    };

    WolfVkDevice() = default;
    ~WolfVkDevice();

    bool createInstance(const Config& config, std::string& error);
    // Enumerates devices with their properties (valid after createInstance), for the floater and the log.
    const std::vector<WolfVkPhysicalDeviceInfo>& physicalDevices() const { return mPhysicalDevices; }
    // [VULKAN WAYLAND 2026-11-09] the configuration createInstance was given (the OpenGL device UUID among it)
    const Config& config() const { return mConfig; }
    // Runs the physical device choice (a.-e. above) without creating a device, so About and the renderer floater
    // can name the card that would be used; chosen()/aboutLine() are valid afterwards.
    bool chooseOnly(VkSurfaceKHR surface, std::string& error) { return choosePhysicalDevice(surface, error); }
    // [W3 2026-11-11] the last choice failed because no device can be used at all (no GPU with a presenting graphics
    // queue and VK_KHR_swapchain): a driver change only can mend it; a vendor mismatch may be retried
    bool noCandidates() const { return mNoCandidates; }
    // surface: the window surface to present to (VK_NULL_HANDLE allowed for an off-screen device, e.g. tests)
    bool createDevice(VkSurfaceKHR surface, std::string& error);
    void destroy();
    // [V4 2026-11-11] The owner could not destroy a swapchain (its presents did not finish within the bounded wait), so
    // the device (VUID-vkDestroyDevice-device-05137) and the instance under its surface must not be destroyed either:
    // both are dropped without destruction, as for a hung device, and later destroy() calls leave the instance alone.
    void abandon(const std::string& why);

    bool hasInstance() const { return mInstance != VK_NULL_HANDLE; }
    bool hasDevice() const { return mDevice != VK_NULL_HANDLE; }

    VkInstance instance() const { return mInstance; }
    VkPhysicalDevice physical() const { return mChosen.mHandle; }
    VkDevice device() const { return mDevice; }
    VkQueue queue() const { return mQueue; }
    uint32_t queueFamily() const { return mChosen.mGraphicsPresentFamily; }
    const WolfVkPhysicalDeviceInfo& chosen() const { return mChosen; }
    const VkPhysicalDeviceLimits& limits() const { return mChosen.mProperties.limits; }
    uint32_t apiVersion() const { return mChosen.mProperties.apiVersion; }
    bool hasTimeline() const { return mTimeline != VK_NULL_HANDLE; }
    bool validationEnabled() const { return mValidationEnabled; }
    WolfVkAllocator& allocator() { return mAllocator; }
    uint32_t framesInFlight() const { return static_cast<uint32_t>(mFrames.size()); }

    // Frames. beginFrame waits until this slot's previous use has finished (fence or timeline), runs its deferred
    // destructions, resets its command pool and begins the command buffer. endFrame ends the command buffer and
    // submits it, waiting on the acquire semaphore if wait_acquire is set (colour attachment output stage) and
    // signalling render_done (the swapchain image's semaphore, WolfVkSwapchain::renderDone; may be VK_NULL_HANDLE for
    // an off-screen frame) and the timeline. Returns false on a lost device.
    Frame& beginFrame();
    // Runs the current slot's deferred destructions. Called by the owner AFTER it has reset whatever else of its own
    // still names the objects (descriptor pools: a descriptor set that names an image view keeps the view "in use" for
    // the validation layer until its pool is reset, VUID-vkDestroyImageView-imageView-01026, seen 2026-11-08).
    void flushDeferred();
    bool endFrame(bool wait_acquire, VkSemaphore render_done, std::string& error);
    Frame& currentFrame() { return mFrames[mFrameIndex]; }
    uint32_t frameIndex() const { return mFrameIndex; }   // [AMD PERF] the slot being recorded / next to be begun
    uint64_t frameCounter() const { return mFrameCounter; }

    // Destroys something frames-in-flight later, when no submitted command buffer can still use it.
    void deferDestroy(std::function<void()> destroy);

    // One-time command buffer on the graphics queue, submitted and waited for (uploads at start-up, read-backs).
    VkCommandBuffer beginOneTime();
    bool endOneTimeAndWait(VkCommandBuffer cmd, std::string& error);
    // [AMD PERF 2026-11-08] one-time submits since the last call and the CPU time spent in endOneTimeAndWait (submit + fence
    // wait), for the per-second frame breakdown (WolfLogFrameRate)
    void takeOneTimeStats(uint32_t& count, double& ms) { count = mOneTimeCount; ms = mOneTimeMs; mOneTimeCount = 0; mOneTimeMs = 0.0; }

    // [SHARED IMAGES 2026-11-08] Sharing with the OpenGL context (wolfvkshare.cpp). externalSharing(): the chosen device
    // has the export extensions and they were enabled. externalHandleType(): the VkExternalMemoryHandleTypeFlagBits
    // of this platform (OPAQUE_FD vulkan_core.h:5579 / OPAQUE_WIN32 :5580), 0 when sharing is off.
    bool externalSharing() const { return mChosen.mHasExternalSharing && mDevice != VK_NULL_HANDLE; }
    // [PRESENT FENCES 2026-11-08] VK_*_swapchain_maintenance1 is enabled on this device: vkQueuePresentKHR may take a
    // VkSwapchainPresentFenceInfoKHR (WolfVkSwapchain::present)
    bool presentFences() const { return mPresentFences && mDevice != VK_NULL_HANDLE; }
    // [GL DEPTH 2026-11-08] VK_EXT_depth_clip_control is enabled: every pipeline uses negativeOneToOne (GL's z range) and
    // the shader mirrors pass GL's projection unchanged (WolfVkShaderMirror::applyProjectionFix does nothing)
    bool glDepthRange() const { return mGlDepthRange && mDevice != VK_NULL_HANDLE; }
    // [AMD PERF 2026-11-09] Debug only (WolfVulkanDebugCompressionQuery, set before createDevice): enable
    // VK_EXT_image_compression_control when offered, so WolfVkImage can log each attachment image's
    // VkImageCompressionPropertiesEXT through vkGetImageSubresourceLayout2. Off, the device is created as before.
    static void setCompressionQueryWanted(bool on) { sCompressionQueryWanted = on; }
    bool compressionQuery() const { return mCompressionQuery && mDevice != VK_NULL_HANDLE && gVk.vkGetImageSubresourceLayout2 != nullptr; }
    VkExternalMemoryHandleTypeFlags externalHandleType() const;
    // One handle: an fd (Linux) or a HANDLE (Windows). [G3 2026-11-11] A successful GL import takes over an fd
    // (EXT_external_objects_fd 4.2.1/6.1) but never a Windows NT handle (EXT_external_objects_win32 rev 10 4.2.1/6.1: "the
    // application must release the handle"), which the importer closes with closeExternalHandle after the import.
    struct ExternalHandle { int mFd = -1; void* mWin32 = nullptr; bool valid() const { return mFd >= 0 || mWin32 != nullptr; } };
    // vkGetMemoryFdKHR (vulkan_core.h:10600) / vkGetMemoryWin32HandleKHR (vulkan_win32.h:88) on a dedicated, exportable allocation.
    bool exportMemory(VkDeviceMemory memory, ExternalHandle& out, std::string& error);
    // A binary semaphore created with VkExportSemaphoreCreateInfo (vulkan_core.h:5989) and its handle
    // (vkGetSemaphoreFdKHR :10682 / vkGetSemaphoreWin32HandleKHR vulkan_win32.h:166).
    bool createExportedSemaphore(VkSemaphore& out, ExternalHandle& handle, std::string& error);
    void closeExternalHandle(ExternalHandle& handle);
    // Extra synchronisation for endFrame: semaphores to wait on (with their stages) and to signal besides the
    // swapchain's and the timeline (the GL hand-over semaphores).
    struct ExtraSync
    {
        std::vector<VkSemaphore> mWaits;
        std::vector<VkPipelineStageFlags> mWaitStages;
        std::vector<VkSemaphore> mSignals;
    };
    bool endFrame(bool wait_acquire, VkSemaphore render_done, const ExtraSync& extra, std::string& error);
    // [VULKAN G-BUFFER 2026-11-08] A submit in the middle of the frame (a pass GL waits for on its queue): the command
    // buffer comes from the slot's pool and is reset with it; the submit signals the timeline with the next value, so
    // beginFrame's wait on the slot's last value covers it. Between beginFrame and endFrame only.
    VkCommandBuffer beginMidFrame();
    bool submitMidFrame(VkCommandBuffer cmd, const ExtraSync& extra, std::string& error);
    // A mid-frame command buffer that will not be submitted (the GL side could not hand its images over): ended and left
    // to the slot's pool, which beginFrame resets. Nothing recorded in it reaches the queue.
    void discardMidFrame(VkCommandBuffer cmd);
    // A submit with no command buffer (or one one-time command buffer, ended by this call and freed after it ran),
    // for the semaphore protocol alone: waits, signals, then returns (waits for completion when wait is set).
    bool submitSync(VkCommandBuffer cmd, const ExtraSync& extra, bool wait, std::string& error);

    // Waits for the device to go idle and runs every deferred destruction (shutdown, swapchain recreate).
    void waitIdleAndFlushDeferred();
    // Device idle only (no destructions): before resetting a descriptor pool whose sets the GPU may still read.
    // [HANG GUARD 2026-11-08 8.2k] Bounded: every batch this device submitted is tracked (timeline value or fence) and
    // waited for at most FRAME_WAIT_LIMIT_NS; false = the device is (now) hung and nothing may be destroyed or waited on.
    bool waitIdle();
    // [W2 2026-11-11] vkQueueWaitIdle (vulkan_core.h:4549), which takes no timeout, on a helper thread, waited at most
    // FRAME_WAIT_LIMIT_NS. On a timeout the device is declared hung at once - the helper still holds the queue, which
    // must be externally synchronised, so nothing may touch it again - and the helper is left to finish or not.
    bool queueWaitIdleBounded(const char* what);

    // [HANG GUARD 2026-11-08] beginFrame waits at most FRAME_WAIT_LIMIT_NS for the slot's previous frame; when the card
    // has not finished by then the device is marked hung: no further waits anywhere (they would never return), the owner
    // falls back to OpenGL and leaks the Vulkan objects rather than freezing the viewer (seen 2026-11-08: a frame that
    // never completed left the viewer in vkWaitSemaphores for good). Every other CPU wait of the Vulkan path (one-time
    // submits, waitIdle, swapchain acquire, the GL-side finish of the shared images) uses the same limit and the same flag.
    static constexpr uint64_t FRAME_WAIT_LIMIT_NS = 8ull * 1000000000ull;
    bool hung() const { return mHung; }
    std::string hungReason() const { return mHungReason; }
    // Marks the device hung from a wait outside this class that ran out of time (swapchain acquire, the GL finish).
    void declareHung(const std::string& reason);
    // [DEVICE LOST 2026-11-08] true (and the device marked hung) when res is VK_ERROR_DEVICE_LOST; what names the call
    bool lost(VkResult res, const char* what);

    // Debug messenger statistics: how many validation errors/warnings and the last message (for the floater).
    uint32_t validationMessageCount() const { return mValidationMessages; }
    const std::string& lastValidationMessage() const { return mLastValidationMessage; }

    // Lines for Help > About and the renderer floater.
    // "NVIDIA GeForce RTX 5090 Laptop GPU, driver NVIDIA 615.78.08, Vulkan 1.4.351"
    std::string aboutLine() const;

    // Called from the debug messenger (public for the C callback).
    void onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const char* id, const char* message);

private:
    bool queryPhysicalDevices(std::string& error);
    bool choosePhysicalDevice(VkSurfaceKHR surface, std::string& error);
    bool createFrames(std::string& error);
    void destroyDeviceObjects();   // [V11 2026-11-11] createDevice failed: device-level objects only, the instance stays
    void destroyFrames();
    void runDeferred(Frame& frame);
    // [HANG GUARD 2026-11-08 8.2k] every submitted batch finished (timeline value mTimelineNext, submitted frame fences,
    // mid-frame fences), each wait at most FRAME_WAIT_LIMIT_NS; false + mHung on a timeout.
    bool waitAllSubmitted(const char* what);
    // a fence for one mid-frame submit on a device without a timeline semaphore (unsignalled, from the slot's list)
    VkFence midFrameFence(Frame& frame);

    Config mConfig;
    VkInstance mInstance = VK_NULL_HANDLE;
    uint32_t mInstanceVersion = 0;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    bool mValidationEnabled = false;
    bool mPortabilityEnumeration = false;
    // [PRESENT FENCES 2026-11-08] which surface_maintenance1 the instance enabled (the KHR and EXT swapchain extensions
    // each depend on their own: vk.xml VK_KHR_swapchain_maintenance1 depends "VK_KHR_swapchain+VK_KHR_surface_maintenance1+
    // (VK_KHR_get_physical_device_properties2,VK_VERSION_1_1)", VK_EXT_swapchain_maintenance1 likewise on the EXT one)
    bool mSurfaceMaintenanceKHR = false, mSurfaceMaintenanceEXT = false;
    bool mPresentFences = false;   // the device was created with a swapchain_maintenance1 and its feature
    bool mGlDepthRange = false;    // the device was created with VK_EXT_depth_clip_control and its feature
    bool mCompressionQuery = false;   // the device was created with VK_EXT_image_compression_control (debug query only)
    static bool sCompressionQueryWanted;
    std::vector<WolfVkPhysicalDeviceInfo> mPhysicalDevices;
    WolfVkPhysicalDeviceInfo mChosen;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    WolfVkAllocator mAllocator;

    std::vector<Frame> mFrames;
    uint32_t mFrameIndex = 0;
    uint64_t mFrameCounter = 0;
    bool mRecording = false;   // between beginFrame and endFrame
    uint32_t mOneTimeOpen = 0; // one-time command buffers begun and not yet submitted
    uint32_t mOneTimeCount = 0;   // [AMD PERF] takeOneTimeStats
    double mOneTimeMs = 0.0;
    bool mHung = false;
    bool mAbandoned = false;
    bool mNoCandidates = false;   // [W3 2026-11-11] noCandidates()   // [V4 2026-11-11] the device was dropped undestroyed: the instance stays for the process
    std::string mHungReason;
    VkSemaphore mTimeline = VK_NULL_HANDLE;   // timeline semaphore: value = number of frames completed
    uint64_t mTimelineNext = 0;
    VkCommandPool mOneTimePool = VK_NULL_HANDLE;
    std::mutex mDeferredMutex;

    uint32_t mValidationMessages = 0;
    std::string mLastValidationMessage;
};

extern WolfVkDevice gWolfVkDevice;

#endif // WOLF_VK_DEVICE_H
