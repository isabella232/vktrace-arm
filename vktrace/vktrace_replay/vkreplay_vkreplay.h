/*
 *
 * Copyright (C) 2015-2017 Valve Corporation
 * Copyright (C) 2015-2017 LunarG, Inc.
 * Copyright (C) 2019 ARM Limited.
 * All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Peter Lohrmann <peterl@valvesoftware.com>
 * Author: Courtney Goeltzenleuchter <courtney@LunarG.com>
 * Author: Mark Lobodzinski <mark@lunarg.com>
 * Author: Tobin Ehlis <tobin@lunarg.com>
 */

#pragma once

#include <set>
#include <map>
#include <vector>
#include <stack>
#include <string>
#include <queue>
#if defined(PLATFORM_LINUX)
#if defined(ANDROID)
#include <android_native_app_glue.h>
#else
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#endif
#endif  // ANDROID
#endif
#include "vktrace_multiplatform.h"
#include "vkreplay_window.h"
#include "vkreplay_factory.h"
#include "vktrace_trace_packet_identifiers.h"
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "vktrace_vk_vk_packets.h"

// TODO138 : Need to add packets files for new wsi headers
}

#include "vulkan/vulkan.h"

#if !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#define undef_VK_USE_PLATFORM_WIN32_KHR
#endif

#if !defined(VK_USE_PLATFORM_XLIB_KHR)
#define VK_USE_PLATFORM_XLIB_KHR
#define undef_VK_USE_PLATFORM_XLIB_KHR
#endif

#if !defined(VK_USE_PLATFORM_XCB_KHR)
#define VK_USE_PLATFORM_XCB_KHR
#define undef_VK_USE_PLATFORM_XCB_KHR
#endif

#if !defined(VK_USE_PLATFORM_WAYLAND_KHR)
#define VK_USE_PLATFORM_WAYLAND_KHR
#define undef_VK_USE_PLATFORM_WAYLAND_KHR
#endif

#if !defined(VK_USE_PLATFORM_ANDROID_KHR)
#define VK_USE_PLATFORM_ANDROID_KHR
#define undef_VK_USE_PLATFORM_ANDROID_KHR
#endif

#include "vk_layer_dispatch_table.h"

#if defined(undef_VK_USE_PLATFORM_WIN32_KHR)
#undef VK_USE_PLATFORM_WIN32_KHR
#endif

#if defined(undef_VK_USE_PLATFORM_XLIB_KHR)
#undef VK_USE_PLATFORM_XLIB_KHR
#endif

#if defined(undef_VK_USE_PLATFORM_XCB_KHR)
#undef VK_USE_PLATFORM_XCB_KHR
#endif

#if defined(undef_VK_USE_PLATFORM_WAYLAND_KHR)
#undef VK_USE_PLATFORM_WAYLAND_KHR
#endif

#if defined(undef_VK_USE_PLATFORM_ANDROID_KHR)
#undef VK_USE_PLATFORM_ANDROID_KHR
#endif

#include "vk_dispatch_table_helper.h"

#include "vkreplay_vkdisplay.h"
#include "vkreplay_vk_objmapper.h"
#include "vkreplay_pipelinecache.h"
#if !defined(ANDROID) && defined(PLATFORM_LINUX)
#include "arm_headless_ext.h"
#endif

#define CHECK_RETURN_VALUE(entrypoint) returnValue = handle_replay_errors(#entrypoint, replayResult, pPacket->result, returnValue);
#define PREMAP_SHIFT (0x1 << 15)

extern vkreplayer_settings* g_pReplaySettings;
extern int g_ruiFrames;

class vkReplay {
   public:
    ~vkReplay();
    vkReplay(vkreplayer_settings* pReplaySettings, vktrace_trace_file_header* pFileHeader,
             vktrace_replay::ReplayDisplayImp* display);
    void destroyObjects(const VkDevice &device);

    int init(vktrace_replay::ReplayDisplay& disp);
    vktrace_replay::ReplayDisplayImp* get_display() { return m_display; }
    vktrace_replay::VKTRACE_REPLAY_RESULT replay(vktrace_trace_packet_header* packet);
    vktrace_replay::VKTRACE_REPLAY_RESULT handle_replay_errors(const char* entrypointName, const VkResult resCall,
                                                               const VkResult resTrace,
                                                               const vktrace_replay::VKTRACE_REPLAY_RESULT resIn);

    void push_validation_msg(VkFlags msgFlags, VkDebugReportObjectTypeEXT objType, uint64_t srcObjectHandle, size_t location,
                             int32_t msgCode, const char* pLayerPrefix, const char* pMsg, const void* pUserData);
    vktrace_replay::VKTRACE_REPLAY_RESULT pop_validation_msgs();
    int dump_validation_data();
    int get_frame_number() { return m_frameNumber; }
    void reset_frame_number(int frameNumber) { m_frameNumber = frameNumber > 0 ? frameNumber : 0; }
    void interpret_pnext_handles(void* struct_ptr);
    void deviceWaitIdle();
    void on_terminate();
    void set_in_frame_range(bool inrange) { m_inFrameRange = inrange; }
    vktrace_replay::PipelineCacheAccessor::Ptr get_pipelinecache_accessor() const;

    bool premap_FlushMappedMemoryRanges(vktrace_trace_packet_header* pHeader);
    bool premap_UpdateDescriptorSets(vktrace_trace_packet_header* pHeader);
    bool premap_CmdBindDescriptorSets(vktrace_trace_packet_header* pHeader);
    bool premap_CmdDrawIndexed(vktrace_trace_packet_header* pHeader);

    void post_interpret(vktrace_trace_packet_header* pHeader);
    vkReplayObjMapper* get_ReplayObjMapper() { return &m_objMapper; };
    std::unordered_map<VkDevice, VkPhysicalDevice> get_ReplayPhysicalDevices () { return replayPhysicalDevices; };
    VkLayerInstanceDispatchTable* get_VkLayerInstanceDispatchTable() { return &m_vkFuncs; };

   private:
    void init_funcs(void* handle);
    void* m_libHandle;
    VkLayerInstanceDispatchTable m_vkFuncs;
#if !defined(ANDROID) && defined(PLATFORM_LINUX)
    PFN_vkCreateHeadlessSurfaceARM
        m_PFN_vkCreateHeadlessSurfaceARM;  // To be removed after vkCreateHeadlessSurfaceARM being published
    int m_headlessExtensionChoice;
    VkResult create_headless_surface_ext(VkInstance instance, VkSurfaceKHR* pSurface);
#endif
    VkLayerDispatchTable m_vkDeviceFuncs;
    vkReplayObjMapper m_objMapper;
    void (*m_pDSDump)(char*);
    void (*m_pCBDump)(char*);
    // VKTRACESNAPSHOT_PRINT_OBJECTS m_pVktraceSnapshotPrint;
    vktrace_replay::ReplayDisplayImp* m_display;

    int m_frameNumber;
    vktrace_trace_file_header* m_pFileHeader;
    struct_gpuinfo* m_pGpuinfo;
    uint32_t m_gpu_count = 0;
    uint32_t m_gpu_group_count = 0;
    uint32_t m_imageIndex = UINT32_MAX;
    uint32_t m_pktImgIndex = UINT32_MAX;

    VkDisplayType m_displayServer = VK_DISPLAY_NONE;
    const char* initialized_screenshot_list;

    // Replay platform description
    uint64_t m_replay_endianess;
    uint64_t m_replay_ptrsize;
    uint64_t m_replay_arch;
    uint64_t m_replay_os;
    uint64_t m_replay_gpu;
    uint64_t m_replay_drv_vers;
    uint8_t  m_replay_pipelinecache_uuid[VK_UUID_SIZE];
    TripleValue m_ASCaptureReplaySupport;

    // Result of comparing trace platform with replay platform
    // -1: Not initialized. 0: No match. 1: Match.
    int m_platformMatch;

    bool platformMatch() {
        if (m_platformMatch == -1 && m_replay_gpu != 0 && m_replay_drv_vers != 0) {
            // Compare trace file platform to replay platform.
            // If a value is null/zero, it is unknown, and we'll consider the platform to not match.
            // If m_replay_gpu and/or m_replay_drv_vers is 0, we don't yet have replay device gpu and driver version info because
            // vkGetPhysicalDeviceProperties has not yet been called. We'll consider it a non-match, but will keep checking for
            // non-zero on subsequent calls.
            // We save the result of the compare so we don't have to keep doing this complex compare.
            m_platformMatch = (m_replay_endianess == m_pFileHeader->endianess) & (m_replay_ptrsize == m_pFileHeader->ptrsize) &
                              (m_replay_arch == m_pFileHeader->arch) & (m_replay_os == m_pFileHeader->os) &
                              (m_replay_gpu == m_pGpuinfo->gpu_id) & (m_replay_drv_vers == m_pGpuinfo->gpu_drv_vers) &
                              (m_pGpuinfo->gpu_id != 0) & (m_pGpuinfo->gpu_drv_vers != 0) & (strlen((char*)&m_replay_arch) != 0) &
                              (strlen((char*)&m_replay_os) != 0) & (strlen((char*)&m_pFileHeader->arch) != 0) &
                              (strlen((char*)&m_pFileHeader->os) != 0);
        }
        return (m_platformMatch == 1);
    }

    bool callFailedDuringTrace(VkResult result, uint16_t packet_id);

    struct ValidationMsg {
        VkFlags msgFlags;
        VkDebugReportObjectTypeEXT objType;
        uint64_t srcObjectHandle;
        size_t location;
        int32_t msgCode;
        char layerPrefix[256];
        char msg[256];
        void* pUserData;
    };

    struct ApiCallStat {
        uint32_t injectedCallCount = 0;
        uint32_t total = 0;
    };
    std::unordered_map<uint32_t, vkReplay::ApiCallStat> m_CallStats;
    bool m_inFrameRange = false;

    VkDebugReportCallbackEXT m_dbgMsgCallbackObj;

    std::vector<struct ValidationMsg> m_validationMsgs;
    std::vector<int> m_screenshotFrames;
    VkResult manually_replay_vkCreateInstance(packet_vkCreateInstance* pPacket);
    VkResult manually_replay_vkCreateDevice(packet_vkCreateDevice* pPacket);
    VkResult manually_replay_vkCreateBuffer(packet_vkCreateBuffer* pPacket);
    VkResult manually_replay_vkCreateImage(packet_vkCreateImage* pPacket);
    VkResult manually_replay_vkCreateCommandPool(packet_vkCreateCommandPool* pPacket);
    void manually_replay_vkDestroyBuffer(packet_vkDestroyBuffer* pPacket);
    void manually_replay_vkDestroyImage(packet_vkDestroyImage* pPacket);
    VkResult manually_replay_vkEnumeratePhysicalDevices(packet_vkEnumeratePhysicalDevices* pPacket);
    VkResult manually_replay_vkEnumeratePhysicalDeviceGroups(packet_vkEnumeratePhysicalDeviceGroups *pPacket);
    // TODO138 : Many new functions in API now that we need to assess if manual code needed
    // VkResult manually_replay_vkGetPhysicalDeviceInfo(packet_vkGetPhysicalDeviceInfo* pPacket);
    // VkResult manually_replay_vkGetGlobalExtensionInfo(packet_vkGetGlobalExtensionInfo* pPacket);
    // VkResult manually_replay_vkGetPhysicalDeviceExtensionInfo(packet_vkGetPhysicalDeviceExtensionInfo* pPacket);
    VkResult manually_replay_vkQueueSubmit(packet_vkQueueSubmit* pPacket);
    VkResult manually_replay_vkQueueBindSparse(packet_vkQueueBindSparse* pPacket);
    // VkResult manually_replay_vkGetObjectInfo(packet_vkGetObjectInfo* pPacket);
    // VkResult manually_replay_vkGetImageSubresourceInfo(packet_vkGetImageSubresourceInfo* pPacket);
    void manually_replay_vkUpdateDescriptorSets(packet_vkUpdateDescriptorSets* pPacket);
    void manually_replay_vkUpdateDescriptorSetsPremapped(packet_vkUpdateDescriptorSets* pPacket);
    VkResult manually_replay_vkCreateDescriptorSetLayout(packet_vkCreateDescriptorSetLayout* pPacket);
    void manually_replay_vkDestroyDescriptorSetLayout(packet_vkDestroyDescriptorSetLayout* pPacket);
    VkResult manually_replay_vkAllocateDescriptorSets(packet_vkAllocateDescriptorSets* pPacket);
    VkResult manually_replay_vkFreeDescriptorSets(packet_vkFreeDescriptorSets* pPacket);
    void manually_replay_vkCmdBindDescriptorSets(packet_vkCmdBindDescriptorSets* pPacket);
    void manually_replay_vkCmdBindDescriptorSetsPremapped(packet_vkCmdBindDescriptorSets* pPacket);
    void manually_replay_vkCmdBindVertexBuffers(packet_vkCmdBindVertexBuffers* pPacket);
    VkResult manually_replay_vkGetPipelineCacheData(packet_vkGetPipelineCacheData* pPacket);
    VkResult manually_replay_vkCreateGraphicsPipelines(packet_vkCreateGraphicsPipelines* pPacket);
    VkResult manually_replay_vkCreateComputePipelines(packet_vkCreateComputePipelines* pPacket);
    VkResult manually_replay_vkCreatePipelineLayout(packet_vkCreatePipelineLayout* pPacket);
    void manually_replay_vkCmdWaitEvents(packet_vkCmdWaitEvents* pPacket);
    void manually_replay_vkCmdPipelineBarrier(packet_vkCmdPipelineBarrier* pPacket);
    VkResult manually_replay_vkCreateFramebuffer(packet_vkCreateFramebuffer* pPacket);
    VkResult manually_replay_vkCreateRenderPass(packet_vkCreateRenderPass* pPacket);
    VkResult manually_replay_vkCreateRenderPass2(packet_vkCreateRenderPass2* pPacket);
    VkResult manually_replay_vkCreateRenderPass2KHR(packet_vkCreateRenderPass2KHR* pPacket);
    void manually_replay_vkCmdBeginRenderPass(packet_vkCmdBeginRenderPass* pPacket);
    void manually_replay_vkCmdBeginRenderPass2KHR(packet_vkCmdBeginRenderPass2KHR *pPacket);
    void manually_replay_vkCmdBeginRenderPass2(packet_vkCmdBeginRenderPass2 *pPacket);
    void manually_replay_vkCmdBeginRenderingKHR(packet_vkCmdBeginRenderingKHR *pPacket);
    void manually_replay_vkCmdEndRenderingKHR(packet_vkCmdEndRenderingKHR *pPacket);
    VkResult manually_replay_vkBeginCommandBuffer(packet_vkBeginCommandBuffer* pPacket);
    VkResult manually_replay_vkAllocateCommandBuffers(packet_vkAllocateCommandBuffers* pPacket);
    VkResult manually_replay_vkWaitForFences(packet_vkWaitForFences* pPacket);
    VkResult manually_replay_vkAllocateMemory(packet_vkAllocateMemory* pPacket);
    void manually_replay_vkFreeMemory(packet_vkFreeMemory* pPacket);
    VkResult manually_replay_vkMapMemory(packet_vkMapMemory* pPacket);
    void manually_replay_vkUnmapMemory(packet_vkUnmapMemory* pPacket);
    VkResult manually_replay_vkFlushMappedMemoryRanges(packet_vkFlushMappedMemoryRanges* pPacket);
    VkResult manually_replay_vkFlushMappedMemoryRangesPremapped(packet_vkFlushMappedMemoryRanges* pPacket);
    void manually_replay_vkCmdDrawIndexedPremapped(packet_vkCmdDrawIndexed* pPacket);
    VkResult manually_replay_vkInvalidateMappedMemoryRanges(packet_vkInvalidateMappedMemoryRanges* pPacket);
    void manually_replay_vkGetPhysicalDeviceMemoryProperties(packet_vkGetPhysicalDeviceMemoryProperties* pPacket);
    void manually_replay_vkGetPhysicalDeviceMemoryProperties2KHR(packet_vkGetPhysicalDeviceMemoryProperties2KHR* pPacket);
    void manually_replay_vkGetPhysicalDeviceQueueFamilyProperties(packet_vkGetPhysicalDeviceQueueFamilyProperties* pPacket);
    void manually_replay_vkGetPhysicalDeviceQueueFamilyProperties2KHR(packet_vkGetPhysicalDeviceQueueFamilyProperties2KHR* pPacket);
    void manually_replay_vkGetPhysicalDeviceSparseImageFormatProperties(
        packet_vkGetPhysicalDeviceSparseImageFormatProperties* pPacket);
    void manually_replay_vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
        packet_vkGetPhysicalDeviceSparseImageFormatProperties2KHR* pPacket);
    void manually_replay_vkGetImageMemoryRequirements(packet_vkGetImageMemoryRequirements* pPacket);
    void manually_replay_vkGetImageMemoryRequirements2KHR(packet_vkGetImageMemoryRequirements2KHR* pPacket);
    void manually_replay_vkGetBufferMemoryRequirements(packet_vkGetBufferMemoryRequirements* pPacket);
    void manually_replay_vkGetBufferMemoryRequirements2KHR(packet_vkGetBufferMemoryRequirements2KHR* pPacket);
    void manually_replay_vkGetPhysicalDeviceProperties(packet_vkGetPhysicalDeviceProperties* pPacket);
    void manually_replay_vkGetPhysicalDeviceProperties2KHR(packet_vkGetPhysicalDeviceProperties2KHR* pPacket);
    VkResult manually_replay_vkFlushMappedMemoryRangesRemap(packet_vkFlushMappedMemoryRanges *pPacket);
    VkResult manually_replay_vkGetPhysicalDeviceSurfaceSupportKHR(packet_vkGetPhysicalDeviceSurfaceSupportKHR* pPacket);
    VkResult manually_replay_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(packet_vkGetPhysicalDeviceSurfaceCapabilitiesKHR* pPacket);
    VkResult manually_replay_vkGetPhysicalDeviceSurfaceFormatsKHR(packet_vkGetPhysicalDeviceSurfaceFormatsKHR* pPacket);
    VkResult manually_replay_vkGetPhysicalDeviceSurfacePresentModesKHR(packet_vkGetPhysicalDeviceSurfacePresentModesKHR* pPacket);
    VkResult manually_replay_vkCreateSwapchainKHR(packet_vkCreateSwapchainKHR* pPacket);
    void manually_replay_vkDestroySwapchainKHR(packet_vkDestroySwapchainKHR* pPacket);
    VkResult manually_replay_vkGetSwapchainImagesKHR(packet_vkGetSwapchainImagesKHR* pPacket);
    VkResult manually_replay_vkQueuePresentKHR(packet_vkQueuePresentKHR* pPacket);
    VkResult manually_replay_vkAcquireNextImageKHR(packet_vkAcquireNextImageKHR* pPacket);
    VkResult manually_replay_vkCreateXcbSurfaceKHR(packet_vkCreateXcbSurfaceKHR* pPacket);
    VkBool32 manually_replay_vkGetPhysicalDeviceXcbPresentationSupportKHR(
        packet_vkGetPhysicalDeviceXcbPresentationSupportKHR* pPacket);
    VkResult manually_replay_vkCreateXlibSurfaceKHR(packet_vkCreateXlibSurfaceKHR* pPacket);
    VkBool32 manually_replay_vkGetPhysicalDeviceXlibPresentationSupportKHR(
        packet_vkGetPhysicalDeviceXlibPresentationSupportKHR* pPacket);
    VkResult manually_replay_vkCreateWaylandSurfaceKHR(packet_vkCreateWaylandSurfaceKHR* pPacket);
    VkBool32 manually_replay_vkGetPhysicalDeviceWaylandPresentationSupportKHR(
        packet_vkGetPhysicalDeviceWaylandPresentationSupportKHR* pPacket);
    VkResult manually_replay_vkCreateWin32SurfaceKHR(packet_vkCreateWin32SurfaceKHR* pPacket);
    VkBool32 manually_replay_vkGetPhysicalDeviceWin32PresentationSupportKHR(
        packet_vkGetPhysicalDeviceWin32PresentationSupportKHR* pPacket);
    VkResult manually_replay_vkCreateAndroidSurfaceKHR(packet_vkCreateAndroidSurfaceKHR* pPacket);
    VkResult manually_replay_vkCreateDebugReportCallbackEXT(packet_vkCreateDebugReportCallbackEXT* pPacket);
    void manually_replay_vkDestroyDebugReportCallbackEXT(packet_vkDestroyDebugReportCallbackEXT* pPacket);
    VkResult manually_replay_vkCreateDescriptorUpdateTemplate(packet_vkCreateDescriptorUpdateTemplate* pPacket);
    VkResult manually_replay_vkCreateDescriptorUpdateTemplateKHR(packet_vkCreateDescriptorUpdateTemplateKHR* pPacket);
    void manually_replay_vkDestroyDescriptorUpdateTemplate(packet_vkDestroyDescriptorUpdateTemplate* pPacket);
    void manually_replay_vkDestroyDescriptorUpdateTemplateKHR(packet_vkDestroyDescriptorUpdateTemplateKHR* pPacket);
    void manually_replay_vkUpdateDescriptorSetWithTemplate(packet_vkUpdateDescriptorSetWithTemplate* pPacket);
    void manually_replay_vkUpdateDescriptorSetWithTemplateKHR(packet_vkUpdateDescriptorSetWithTemplateKHR* pPacket);
    void manually_replay_vkCmdPushDescriptorSetKHR(packet_vkCmdPushDescriptorSetKHR* pPacket);
    void manually_replay_vkCmdPushDescriptorSetWithTemplateKHR(packet_vkCmdPushDescriptorSetWithTemplateKHR* pPacket);
    VkResult manually_replay_vkBindBufferMemory2KHR(packet_vkBindBufferMemory2KHR* pPacket);
    VkResult manually_replay_vkBindImageMemory2KHR(packet_vkBindImageMemory2KHR* pPacket);
    VkResult manually_replay_vkBindBufferMemory2(packet_vkBindBufferMemory2* pPacket);
    VkResult manually_replay_vkBindImageMemory2(packet_vkBindImageMemory2* pPacket);
    VkResult manually_replay_vkGetDisplayPlaneSupportedDisplaysKHR(packet_vkGetDisplayPlaneSupportedDisplaysKHR* pPacket);
    VkResult manually_replay_vkEnumerateDeviceExtensionProperties(packet_vkEnumerateDeviceExtensionProperties* pPacket);
    VkResult manually_replay_vkRegisterDeviceEventEXT(packet_vkRegisterDeviceEventEXT* pPacket);
    VkResult manually_replay_vkRegisterDisplayEventEXT(packet_vkRegisterDisplayEventEXT* pPacket);
    VkResult manually_replay_vkBindBufferMemory(packet_vkBindBufferMemory* pPacket);
    VkResult manually_replay_vkBindImageMemory(packet_vkBindImageMemory* pPacket);
    void manually_replay_vkGetImageMemoryRequirements2(packet_vkGetImageMemoryRequirements2* pPacket);
    void manually_replay_vkGetBufferMemoryRequirements2(packet_vkGetBufferMemoryRequirements2* pPacket);
    VkResult manually_replay_vkCreateSampler(packet_vkCreateSampler* pPacket);
    VkResult manually_replay_vkCreateDisplayPlaneSurfaceKHR(packet_vkCreateDisplayPlaneSurfaceKHR *pPacket);
    void changeDeviceFeature(VkBool32 *traceFeatures,VkBool32 *deviceFeatures,uint32_t numOfFeatures);
    void manually_replay_vkGetAccelerationStructureBuildSizesKHR(packet_vkGetAccelerationStructureBuildSizesKHR *pPacket);
    void manually_replay_vkCmdPushConstants(packet_vkCmdPushConstants *pPacket);
    void manually_replay_vkCmdPushConstantsRemap(packet_vkCmdPushConstants *pPacket);
    VkResult manually_replay_vkCreateAccelerationStructureKHR(packet_vkCreateAccelerationStructureKHR *pPacket);
    VkResult manually_replay_vkBuildAccelerationStructuresKHR(packet_vkBuildAccelerationStructuresKHR *pPacket);
    void manually_replay_vkCmdBuildAccelerationStructuresKHR(packet_vkCmdBuildAccelerationStructuresKHR* pPacket);
    void manually_replay_vkCmdBuildAccelerationStructuresIndirectKHR(packet_vkCmdBuildAccelerationStructuresIndirectKHR* pPacket);
    void manually_replay_vkCmdCopyAccelerationStructureToMemoryKHR(packet_vkCmdCopyAccelerationStructureToMemoryKHR* pPacket);
    void manually_replay_vkCmdCopyMemoryToAccelerationStructureKHR(packet_vkCmdCopyMemoryToAccelerationStructureKHR* pPacket);
    VkResult manually_replay_vkGetAccelerationStructureDeviceAddressKHR(packet_vkGetAccelerationStructureDeviceAddressKHR *pPacket);
    void manually_replay_vkDestroyAccelerationStructureKHR(packet_vkDestroyAccelerationStructureKHR *pPacket);
    VkResult manually_replay_vkCopyAccelerationStructureToMemoryKHR(packet_vkCopyAccelerationStructureToMemoryKHR *pPacket);
    VkResult manually_replay_vkCopyMemoryToAccelerationStructureKHR(packet_vkCopyMemoryToAccelerationStructureKHR *pPacket);
    VkResult manually_replay_vkCreateRayTracingPipelinesKHR(packet_vkCreateRayTracingPipelinesKHR *pPacket);
    void manually_replay_vkCmdCopyBufferRemap(packet_vkCmdCopyBuffer *pPacket);
    void process_screenshot_list(const char* list) {
        std::string spec(list), word;
        size_t start = 0, comma = 0;

        while (start < spec.size()) {
            comma = spec.find(',', start);

            if (comma == std::string::npos)
                word = std::string(spec, start);
            else
                word = std::string(spec, start, comma - start);

            m_screenshotFrames.push_back(atoi(word.c_str()));
            if (comma == std::string::npos) break;

            start = comma + 1;
        }
    }

    struct QueueFamilyProperties {
        uint32_t count;
        VkQueueFamilyProperties* queueFamilyProperties;
    };

    // Map VkPhysicalDevice to QueueFamilyPropeties (and ultimately queue indices)
    std::unordered_map<VkPhysicalDevice, struct QueueFamilyProperties> traceQueueFamilyProperties;
    std::unordered_map<VkPhysicalDevice, struct QueueFamilyProperties> replayQueueFamilyProperties;

    // Map VkDevice to a VkPhysicalDevice
    std::unordered_map<VkDevice, VkPhysicalDevice> tracePhysicalDevices;
    std::unordered_map<VkDevice, VkPhysicalDevice> replayPhysicalDevices;

    // Map VkBuffer to VkDevice, so we can search for the VkDevice used to create a buffer
    std::unordered_map<VkBuffer, VkDevice> traceBufferToDevice;
    std::unordered_map<VkBuffer, VkDevice> replayBufferToDevice;

    // Map VkImage to VkDevice, so we can search for the VkDevice used to create an image
    std::unordered_map<VkImage, VkDevice> traceImageToDevice;
    std::unordered_map<VkImage, VkDevice> replayImageToDevice;

    // Map Vulkan objects to VkDevice, so we can search for the VkDevice used to create an object
    std::unordered_map<VkQueryPool, VkDevice> replayQueryPoolToDevice;
    std::unordered_map<VkEvent, VkDevice> replayEventToDevice;
    std::unordered_map<VkFence, VkDevice> replayFenceToDevice;
    std::unordered_map<VkSemaphore, VkDevice> replaySemaphoreToDevice;
    std::unordered_map<VkFramebuffer, VkDevice> replayFramebufferToDevice;
    std::unordered_map<VkDescriptorPool, VkDevice> replayDescriptorPoolToDevice;
    std::unordered_map<VkPipeline, VkDevice> replayPipelineToDevice;
    std::unordered_map<VkPipelineCache, VkDevice> replayPipelineCacheToDevice;
    std::unordered_map<VkShaderModule, VkDevice> replayShaderModuleToDevice;
    std::unordered_map<VkRenderPass, VkDevice> replayRenderPassToDevice;
    std::unordered_map<VkPipelineLayout, VkDevice> replayPipelineLayoutToDevice;
    std::unordered_map<VkDescriptorSetLayout, VkDevice> replayDescriptorSetLayoutToDevice;
    std::unordered_map<VkSampler, VkDevice> replaySamplerToDevice;
    std::unordered_map<VkBufferView, VkDevice> replayBufferViewToDevice;
    std::unordered_map<VkImageView, VkDevice> replayImageViewToDevice;
    std::unordered_map<VkDeviceMemory, VkDevice> replayDeviceMemoryToDevice;
    std::unordered_map<VkSwapchainKHR, VkDevice> replaySwapchainKHRToDevice;
    std::unordered_map<VkCommandPool, VkDevice> replayCommandPoolToDevice;
    std::unordered_map<VkImage, VkDevice> replaySwapchainImageToDevice;
    std::unordered_map<VkDeferredOperationKHR, VkDevice> replayDeferredOperationKHRToDevice;
    std::unordered_map<VkAccelerationStructureKHR, VkDevice> replayAccelerationStructureKHRToDevice;
    std::unordered_map<VkPipeline, VkDevice> replayRayTracingPipelinesKHRToDevice;
    std::unordered_map<VkAccelerationStructureNV, VkDevice> replayAccelerationStructureNVToDevice;
    std::unordered_map<VkPipeline, VkDevice> replayRayTracingPipelinesNVToDevice;

    // Map VkSwapchainKHR to vector of VkImage, so we can unmap swapchain images at vkDestroySwapchainKHR
    std::unordered_map<VkSwapchainKHR, std::vector<VkImage>> traceSwapchainToImages;

    // Map VkPhysicalDevice to VkPhysicalDeviceMemoryProperites
    std::unordered_map<VkPhysicalDevice, VkPhysicalDeviceMemoryProperties> traceMemoryProperties;
    std::unordered_map<VkPhysicalDevice, VkPhysicalDeviceMemoryProperties> replayMemoryProperties;

    // Map swapchain image index to VkImage, VkImageView, VkFramebuffer, so we can replace swapchain image and frame buffer with the
    // acquired ones
    struct SwapchainImageState {
        std::unordered_map<uint32_t, VkImage> traceImageIndexToImage;
        std::unordered_map<VkImage, uint32_t> traceImageToImageIndex;
        std::unordered_map<uint32_t, std::unordered_set<VkImageView> > traceImageIndexToImageViews;
        std::unordered_map<VkImageView, uint32_t> traceImageViewToImageIndex;
        std::unordered_map<uint32_t, VkFramebuffer> traceImageIndexToFramebuffer;
        std::unordered_map<VkFramebuffer, uint32_t> traceFramebufferToImageIndex;

        void reset() {
            traceImageIndexToImage.clear();
            traceImageToImageIndex.clear();
            traceImageIndexToImageViews.clear();
            traceImageViewToImageIndex.clear();
            traceImageIndexToFramebuffer.clear();
            traceFramebufferToImageIndex.clear();
        }
    };
    std::unordered_map<VkSwapchainKHR, SwapchainImageState> swapchainImageStates;
    VkSwapchainKHR curSwapchainHandle;

    std::stack<SwapchainImageState> savedSwapchainImgStates;
    std::unordered_map< VkSwapchainKHR, std::vector<bool> > swapchainImageAcquireStatus;
    std::unordered_map< uint32_t, VkSemaphore > swapchainImgIdxToAcquireSemaphore;
    std::unordered_map< VkSemaphore, VkSemaphore > acquireSemaphoreToFSIISemaphore;
    std::queue<VkSemaphore> fsiiSemaphores;

    // Map VkImage to VkMemoryRequirements
    std::unordered_map<VkImage, VkMemoryRequirements> replayGetImageMemoryRequirements;

    // Map VkBuffer to VkMemoryRequirements
    std::unordered_map<VkBuffer, VkMemoryRequirements> replayGetBufferMemoryRequirements;

    // Map device to extension property count, for device extension property queries
    std::unordered_map<VkPhysicalDevice, uint32_t> replayDeviceExtensionPropertyCount;

    bool modifyMemoryTypeIndexInAllocateMemoryPacket(VkDevice remappedDevice, packet_vkAllocateMemory* pPacket);

    std::unordered_map<VkImage, VkImageTiling> replayImageToTiling;
    std::unordered_map<VkImage, VkDeviceMemory> replayOptimalImageToDeviceMemory;
    std::unordered_map<VkDeviceMemory, uint32_t> traceDeviceMemoryToMemoryTypeIndex;
    std::unordered_map<VkDeviceMemory, AHardwareBuffer*> traceDeviceMemoryToAHWBuf;

    std::unordered_set<VkDeviceMemory> traceSkippedDeviceMemories;
    std::vector<VkImage> hardwarebufferImage;

    // Map VkSurfaceKHR to VkSurfaceCapabilitiesKHR
    std::unordered_map<VkSurfaceKHR, VkSurfaceCapabilitiesKHR> replaySurfaceCapabilities;

    // Map VkSurfaceKHR to VkSwapchainKHR
    std::unordered_map<VkSurfaceKHR, VkSwapchainKHR> replaySurfToSwapchain;
    typedef struct _traceMemoryMapInfo{
        VkDeviceMemory traceMemory;
        VkDeviceSize offset;
        VkDeviceSize size;
        VkMemoryMapFlags flags;
    }traceMemoryMapInfo;

    typedef struct _objDeviceAddr{
        VkDeviceAddress replayDeviceAddr = 0;
        uint64_t traceObjHandle = 0;
    }objDeviceAddr;
    std::unordered_map<VkDeviceAddress, objDeviceAddr> traceDeviceAddrToReplayDeviceAddr4Buf;
    std::unordered_map<VkDeviceAddress, objDeviceAddr> traceDeviceAddrToReplayDeviceAddr4AS;
    std::unordered_map<VkBuffer, VkDeviceMemory> traceBufferToReplayMemory;
    std::unordered_map<VkAccelerationStructureKHR, void*> replayASToCopyAddress;
    std::unordered_map<void*, traceMemoryMapInfo> traceAddressToTraceMemoryMapInfo;
    std::unordered_map<VkBuffer, VkDeviceSize> replayBufferToASBuildSizes;
    std::unordered_map<VkAccelerationStructureKHR, VkAccelerationStructureBuildSizesInfoKHR> replayASToASBuildSizes;
    std::unordered_map<VkDeviceSize, VkAccelerationStructureBuildSizesInfoKHR> traceASSizeToReplayASBuildSizes;
    std::unordered_map<VkDeviceSize, VkAccelerationStructureBuildSizesInfoKHR> traceUpdateSizeToReplayASBuildSizes;
    std::unordered_map<VkDeviceSize, VkAccelerationStructureBuildSizesInfoKHR> traceBuildSizeToReplayASBuildSizes;
    std::unordered_map<VkDeviceSize, VkDeviceSize> traceScratchSizeToReplayScratchSize;
    std::unordered_map<VkDeviceMemory, void*> replayMemoryToMapAddress;
    std::unordered_map<VkDevice, deviceFeatureSupport> replayDeviceToFeatureSupport;
    std::unordered_map<VkCommandBuffer, VkDevice> replayCommandBufferToReplayDevice;
    std::unordered_map<VkBuffer, VkDeviceMemory> replayBufferToReplayDeviceMemory;

    uint32_t swapchainRefCount = 0;
    uint32_t surfRefCount = 0;

    uint32_t m_instCount = 0;

    void forceDisableCaptureReplayFeature();
    void* fromTraceAddr2ReplayAddr(VkDevice replayDevice, const void* traceAddress);
    bool isMapMemoryAddress(const void* hostAddress);
    bool getReplayMemoryTypeIdx(VkDevice traceDevice, VkDevice replayDevice, uint32_t traceIdx,
                                VkMemoryRequirements* memRequirements, uint32_t* pReplayIdx);

    void getReplayQueueFamilyIdx(VkPhysicalDevice tracePhysicalDevice, VkPhysicalDevice replayPhysicalDevice, uint32_t* pIdx);
    void getReplayQueueFamilyIdx(VkDevice traceDevice, VkDevice replayDevice, uint32_t* pIdx);

    void remapHandlesInDescriptorSetWithTemplateData(VkDescriptorUpdateTemplateKHR remappedDescriptorUpdateTemplate, char* pData);
    bool findImageFromOtherSwapchain(VkSwapchainKHR swapchain);
    void checkDeviceExtendFeatures(const VkBaseOutStructure *pNext, VkPhysicalDevice physicalDevice);
    vktrace_replay::PipelineCacheAccessor::Ptr    m_pipelinecache_accessor;

    std::unordered_map<VkQueryPool, VkQueryType>  m_querypool_type;
};
