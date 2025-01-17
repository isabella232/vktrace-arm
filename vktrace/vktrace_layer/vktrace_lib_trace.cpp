/*
 *
 * Copyright (C) 2015-2017 Valve Corporation
 * Copyright (C) 2015-2017 LunarG, Inc.
 * Copyright (C) 2019 ARM Limited
 * All Rights Reserved.
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
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Tobin Ehlis <tobin@lunarg.com>
 * Author: Peter Lohrmann <peterl@valvesoftware.com>
 * Author: Mark Lobodzinski <mark@lunarg.com>
 * Author: David Pinedo <david@lunarg.com>
 */
#include <stdbool.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>

#if defined(ANDROID)
#include <android/hardware_buffer_jni.h>
#endif

#if defined(PLATFORM_LINUX) && !defined(ANDROID)
#include "../vktrace_common/arm_headless_ext.h"
#endif // #if defined(PLATFORM_LINUX) && !defined(ANDROID)

#include "vktrace_vk_vk.h"
#include "vulkan/vulkan.h"
#include "vulkan/vk_layer.h"
#include "vktrace_platform.h"
#include "vk_dispatch_table_helper.h"
#include "vktrace_common.h"
#include "vktrace_lib_helpers.h"
#include "vktrace_lib_trim.h"

#include "vktrace_interconnect.h"
#include "vktrace_filelike.h"
#include "vktrace_trace_packet_utils.h"
#include "vktrace_vk_exts.h"
#include <stdio.h>

#include "vktrace_pageguard_memorycopy.h"
#include "vktrace_lib_pagestatusarray.h"
#include "vktrace_lib_pageguardmappedmemory.h"
#include "vktrace_lib_pageguardcapture.h"
#include "vktrace_lib_pageguard.h"

#include "vk_struct_size_helper.h"

// This mutex is used to protect API calls sequence when trim starting process.
std::mutex g_mutex_trace;
bool g_is_vkreplay_proc = false;

VKTRACER_LEAVE _Unload(void) {
    // only do the hooking and networking if the tracer is NOT loaded by vktrace
    if (vktrace_is_loaded_into_vktrace() == FALSE) {
        if (vktrace_trace_get_trace_file() != NULL) {
            vktrace_trace_packet_header *pHeader =
                vktrace_create_trace_packet(VKTRACE_TID_VULKAN, VKTRACE_TPI_MARKER_TERMINATE_PROCESS, 0, 0);
            vktrace_finalize_trace_packet(pHeader);
            vktrace_write_trace_packet(pHeader, vktrace_trace_get_trace_file());
            vktrace_delete_trace_packet(&pHeader);
            vktrace_free(vktrace_trace_get_trace_file());
            vktrace_trace_set_trace_file(NULL);
            vktrace_deinitialize_trace_packet_utils();
            trim::deinitialize();
        }
        if (gMessageStream != NULL) {
            vktrace_MessageStream_destroy(&gMessageStream);
        }
        vktrace_LogVerbose("vktrace_lib library unloaded from PID %d", vktrace_get_pid());
    }
}

PFN_vkVoidFunction layer_intercept_instance_proc(const char* name);
PFN_vkVoidFunction layer_intercept_proc(const char* name);

#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
std::unordered_map<VkDevice, VkPhysicalDevice> g_deviceToPhysicalDevice;
std::unordered_map<VkCommandBuffer, std::list<VkCommandBuffer>> g_commandBufferToCommandBuffers;
std::unordered_map<VkCommandBuffer, std::list<VkBuffer>> g_cmdBufferToBuffers;
std::unordered_map<VkBuffer, DeviceMemory> g_bufferToDeviceMemory;
std::unordered_map<VkCommandBuffer, std::list<VkImage>> g_cmdBufferToImages;
std::unordered_map<VkImage, DeviceMemory> g_imageToDeviceMemory;
std::unordered_map<VkCommandBuffer, std::list<VkDescriptorSet>> g_cmdbufferToDescriptorSets;
std::unordered_map<VkDescriptorSet, DeviceMemory> g_descsetToMemory;
std::unordered_map<VkCommandBuffer, std::list<VkAccelerationStructureKHR>> g_cmdBufferToAS;
std::unordered_map<VkImageView, VkImage> g_imageViewToImage;
std::unordered_map<VkAccelerationStructureKHR, VkBuffer> g_AStoBuffer;
#endif
std::unordered_map<void*, memoryMapParam> g_memoryMapInfo;
std::unordered_map<VkDevice, deviceFeatureSupport> g_deviceToFeatureSupport;
std::unordered_map<VkDeviceAddress, VkBuffer> g_deviceAddressToBuffer;
std::vector<VkBuffer> g_bufferInCmdBuildAS;
std::vector<VkCommandBuffer> g_commandBufferInCmdBuildAS;
extern std::map<uint64_t, cmdBuildASPacketInfo> g_cmdBuildASPacket;

// declared as extern in vktrace_lib_helpers.h
VKTRACE_CRITICAL_SECTION g_memInfoLock;
VKMemInfo g_memInfo = {0, NULL, NULL, 0};

std::unordered_map<void*, layer_device_data*> g_deviceDataMap;
std::unordered_map<void*, layer_instance_data*> g_instanceDataMap;
std::unordered_map<VkCommandBuffer, VkDevice> g_commandBufferToDevice;

std::unordered_map<VkBuffer, VkDeviceMemory> g_shaderDeviceAddrBufferToMem;
std::unordered_map<VkDeviceMemory, VkBuffer> g_shaderDeviceAddrBufferToMemRev;

std::unordered_map<VkBuffer, VkDeviceAddress> g_BuftoDeviceAddr;
std::unordered_map<VkDeviceAddress, VkBuffer> g_BuftoDeviceAddrRev;

std::unordered_map<VkAccelerationStructureKHR, VkDeviceAddress> g_AStoDeviceAddr;
std::unordered_map<VkDeviceAddress, VkAccelerationStructureKHR> g_AStoDeviceAddrRev;

layer_instance_data* mid(void* object) {
    dispatch_key key = get_dispatch_key(object);
    std::unordered_map<void*, layer_instance_data*>::const_iterator got;
    got = g_instanceDataMap.find(key);
    assert(got != g_instanceDataMap.end());
    return got->second;
}

layer_device_data* mdd(void* object) {
    dispatch_key key = get_dispatch_key(object);
    std::unordered_map<void*, layer_device_data*>::const_iterator got;
    got = g_deviceDataMap.find(key);
    assert(got != g_deviceDataMap.end());
    return got->second;
}

static layer_instance_data* initInstanceData(VkInstance instance, const PFN_vkGetInstanceProcAddr gpa,
                                             std::unordered_map<void*, layer_instance_data*>& map) {
    layer_instance_data* pTable;
    assert(instance);
    dispatch_key key = get_dispatch_key(instance);

    std::unordered_map<void*, layer_instance_data*>::const_iterator it = map.find(key);
    if (it == map.end()) {
        pTable = new layer_instance_data();
        map[key] = pTable;
    } else {
        return it->second;
    }

    // TODO: Convert to new init method
    layer_init_instance_dispatch_table(instance, &pTable->instTable, gpa);

    return pTable;
}

static layer_device_data* initDeviceData(VkDevice device, const PFN_vkGetDeviceProcAddr gpa,
                                         std::unordered_map<void*, layer_device_data*>& map) {
    layer_device_data* pTable;
    dispatch_key key = get_dispatch_key(device);

    std::unordered_map<void*, layer_device_data*>::const_iterator it = map.find(key);
    if (it == map.end()) {
        pTable = new layer_device_data();
        map[key] = pTable;
    } else {
        return it->second;
    }

    layer_init_device_dispatch_table(device, &pTable->devTable, gpa);

    return pTable;
}

static const bool FIFO_ENABLE_DEFAULT = true;

bool getBoolOption(const char* name, bool def) {
    bool ret = def;
    const char* env_value_str = vktrace_get_global_var(name);
    if (env_value_str) {
        int envvalue;
        if (sscanf(env_value_str, "%d", &envvalue) == 1) {
            if (envvalue == 1) {
                ret = true;
            } else {
                ret = false;
            }
        }
    }
    return ret;
}

uint32_t getUintOption(const char* name, uint32_t def) {
    uint32_t ret = def;
    const char* env_value_str = vktrace_get_global_var(name);
    if (env_value_str) {
        sscanf(env_value_str, "%d", &ret);
    }
    return ret;
}

bool getForceFifoEnableFlag() {
    static bool EnableForceFifo = FIFO_ENABLE_DEFAULT;
    static bool FirstTimeRun = true;
    if (FirstTimeRun) {
        FirstTimeRun = false;
        EnableForceFifo = getBoolOption(VKTRACE_FORCE_FIFO_ENV, FIFO_ENABLE_DEFAULT);
    }
    return EnableForceFifo;
}

bool getPMBSyncGPUDataBackEnableFlag() {
    static bool EnableSyncBack = false;
    static bool FirstTimeRun = true;
    if (FirstTimeRun) {
        FirstTimeRun = false;
        EnableSyncBack = getBoolOption(VKTRACE_PAGEGUARD_ENABLE_SYNC_GPU_DATA_BACK_ENV, false);
    }
    return EnableSyncBack;
}

std::unordered_map<VkFence, uint64_t> g_submittedFenceOnFrame;

uint32_t getDelaySignalFenceFrames() {
    static uint32_t DelayFrames = 0;
    static bool FirstTimeRun = true;
    if (FirstTimeRun) {
        FirstTimeRun = false;
        DelayFrames = getUintOption(VKTRACE_DELAY_SIGNAL_FENCE_FRAMES_ENV, 0);
    }
    return DelayFrames;
}

/*
 * This function will return the pNext pointer of any
 * CreateInfo extensions that are not loader extensions.
 * This is used to skip past the loader extensions prepended
 * to the list during CreateInstance and CreateDevice.
 */
void* strip_create_extensions(const void* pNext) {
    VkLayerInstanceCreateInfo* create_info = (VkLayerInstanceCreateInfo*)pNext;

    while (create_info && (create_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                           create_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)) {
        create_info = (VkLayerInstanceCreateInfo*)create_info->pNext;
    }

    return create_info;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdSetCheckpointNV(
    VkCommandBuffer commandBuffer,
    const void* pCheckpointMarker) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdSetCheckpointNV* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCmdSetCheckpointNV, sizeof(pCheckpointMarker));
    mdd(commandBuffer)->devTable.CmdSetCheckpointNV(commandBuffer, pCheckpointMarker);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdSetCheckpointNV(pHeader);
    pPacket->commandBuffer = commandBuffer;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCheckpointMarker), sizeof(void*), pCheckpointMarker);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCheckpointMarker));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                                                         const VkAllocationCallbacks* pAllocator,
                                                                         VkDeviceMemory* pMemory) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    size_t additional_size = 0;
#if defined(ANDROID)
    vulkan_struct_header* pvkst_header = (vulkan_struct_header*)pAllocateInfo;
    uint32_t import_buf_size           = 0;
    void*    hwbuf_cpu_ptr             = NULL;
    VkImportAndroidHardwareBufferInfoANDROID* pImportAHWBuf = NULL;
    AHardwareBuffer_Desc ahwbuf_desc;
    pvkst_header = (vulkan_struct_header*)find_ext_struct(pvkst_header, VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
    if (pvkst_header) {
        pImportAHWBuf = (VkImportAndroidHardwareBufferInfoANDROID*)pvkst_header;
        AHardwareBuffer_describe(pImportAHWBuf->buffer, &ahwbuf_desc);
        int ret = AHardwareBuffer_lock(pImportAHWBuf->buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &hwbuf_cpu_ptr);
        if (ret) {
            vktrace_LogError("AM: Lock the hardware buffer failed !");
        } else {
            if (ahwbuf_desc.width > ahwbuf_desc.stride) {
                vktrace_LogError("NOTE: AHardwareBuffer_lock function is abnormal(stride = %u, width = %u)!", ahwbuf_desc.stride, ahwbuf_desc.width);
            }
            uint32_t stride = (ahwbuf_desc.width > ahwbuf_desc.stride) ? ahwbuf_desc.width : ahwbuf_desc.stride;
            import_buf_size = stride * ahwbuf_desc.height * getAHardwareBufBPP(ahwbuf_desc.format);
            additional_size = sizeof(AHardwareBuffer_Desc) + import_buf_size;
        }
    }
#endif
    vktrace_trace_packet_header* pHeader;
    packet_vkAllocateMemory* pPacket = NULL;

    // If user disable using shadow memory, we'll use host memory through
    // VK_EXT_external_memory_host extension. If user enable using shadow
    // memory, there's no need to handle vkAllocateMemory because we don't
    // need to decide using the extension or not through memory property
    // flag.
    if (UseMappedExternalHostMemoryExtension()) {
        pageguardEnter();
    }

    size_t packetSize = get_struct_chain_size((void*)pAllocateInfo) + ROUNDUP_TO_4(sizeof(VkMemoryOpaqueCaptureAddressAllocateInfo)) + sizeof(VkAllocationCallbacks) + sizeof(VkDeviceMemory) * 2;
    CREATE_TRACE_PACKET(vkAllocateMemory, (packetSize + additional_size));

    VkImportMemoryHostPointerInfoEXT importMemoryHostPointerInfo = {};
    void* pNextOriginal = const_cast<void*>(pAllocateInfo->pNext);
    void* pHostPointer = nullptr;
    VkDeviceSize original_allocation_size = pAllocateInfo->allocationSize;
    if (UseMappedExternalHostMemoryExtension()) {
        VkMemoryPropertyFlags propertyFlags =
            getPageGuardControlInstance().getMemoryPropertyFlags(device, pAllocateInfo->memoryTypeIndex);
        if ((propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            // host visible memory, enable extension
            if (pAllocateInfo->pNext != nullptr) {
                // Todo: add checking process to detect if the extension we
                //      use here is compatible with the existing pNext chain,
                //      and output warning message if not compatible.
                assert(false);
            }

            // we insert our extension struct into the pNext chain.
            void** ppNext = const_cast<void**>(&(pAllocateInfo->pNext));
            *ppNext = &importMemoryHostPointerInfo;
            reinterpret_cast<VkImportMemoryHostPointerInfoEXT*>(*ppNext)->sType =
                VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
            reinterpret_cast<VkImportMemoryHostPointerInfoEXT*>(*ppNext)->handleType =
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            reinterpret_cast<VkImportMemoryHostPointerInfoEXT*>(*ppNext)->pNext = pNextOriginal;
            // provide the title host memory pointer which is already added
            // memory write watch.
            size_t page_size = pageguardGetSystemPageSize();
            if ((original_allocation_size % page_size) != 0) {
                const_cast<VkMemoryAllocateInfo*>(pAllocateInfo)->allocationSize =
                    pAllocateInfo->allocationSize - (pAllocateInfo->allocationSize % page_size) + page_size;
            }
            reinterpret_cast<VkImportMemoryHostPointerInfoEXT*>(*ppNext)->pHostPointer =
                pageguardAllocateMemory(pAllocateInfo->allocationSize);
            pHostPointer = reinterpret_cast<VkImportMemoryHostPointerInfoEXT*>(*ppNext)->pHostPointer;
        }
    }

    VkMemoryAllocateFlagsInfo *allocateFlagInfo = (VkMemoryAllocateFlagsInfo*)find_ext_struct((const vulkan_struct_header*)pAllocateInfo->pNext, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
    if (allocateFlagInfo == nullptr) {
        allocateFlagInfo = (VkMemoryAllocateFlagsInfo*)find_ext_struct((const vulkan_struct_header*)pAllocateInfo->pNext, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR);
    }
    if (allocateFlagInfo != nullptr && (allocateFlagInfo->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR)) {
        auto it = g_deviceToFeatureSupport.find(device);
        if (it != g_deviceToFeatureSupport.end()) {
            if (it->second.bufferDeviceAddressCaptureReplay) {
                allocateFlagInfo->flags = allocateFlagInfo->flags | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR;
            } else {
                vktrace_LogDebug("The device doesn't support bufferDeviceAddressCaptureReplay feature.");
            }
        }
    }

    result = mdd(device)->devTable.AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    const_cast<VkMemoryAllocateInfo*>(pAllocateInfo)->allocationSize = original_allocation_size;
    if (UseMappedExternalHostMemoryExtension()) {
        if (result == VK_SUCCESS) {
            getPageGuardControlInstance().vkAllocateMemoryPageGuardHandle(device, pAllocateInfo, pAllocator, pMemory, pHostPointer);
            void** ppNext = const_cast<void**>(&(pAllocateInfo->pNext));
            // after allocation, we restore the original pNext. the extension
            // we insert here should not appear during playback.
            *ppNext = pNextOriginal;
        } else {
            if (pHostPointer) {
                pageguardFreeMemory(pHostPointer);
                pHostPointer = nullptr;
                assert(false);
            }
        }
    }

    vktrace_set_packet_entrypoint_end_time(pHeader);

    VkMemoryOpaqueCaptureAddressAllocateInfo captureAddressAllocateInfo = {};
    if (allocateFlagInfo != nullptr && (allocateFlagInfo->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR)) {
        VkDeviceMemoryOpaqueCaptureAddressInfo memoryAddressInfo = {VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO, nullptr, *pMemory};
        uint64_t captureAddress = mdd(device)->devTable.GetDeviceMemoryOpaqueCaptureAddressKHR(device,&memoryAddressInfo);
        VkMemoryOpaqueCaptureAddressAllocateInfo *pCaptureAddressAllocateInfo = (VkMemoryOpaqueCaptureAddressAllocateInfo*)find_ext_struct((const vulkan_struct_header*)pAllocateInfo->pNext, VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO);
        if (pCaptureAddressAllocateInfo == nullptr) {
            pCaptureAddressAllocateInfo = (VkMemoryOpaqueCaptureAddressAllocateInfo*)find_ext_struct((const vulkan_struct_header*)pAllocateInfo->pNext, VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO_KHR);
        }
        if (pCaptureAddressAllocateInfo != nullptr) {
            pCaptureAddressAllocateInfo->opaqueCaptureAddress = captureAddress;
        } else {
            captureAddressAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO;
            captureAddressAllocateInfo.opaqueCaptureAddress = captureAddress;
            const void* temp = pAllocateInfo->pNext;
            const_cast<VkMemoryAllocateInfo*>(pAllocateInfo)->pNext = (const void*)&captureAddressAllocateInfo;
            captureAddressAllocateInfo.pNext = temp;
        }
    }

#if defined(ANDROID)
    if (import_buf_size > 0 && hwbuf_cpu_ptr) {
        PBYTE pdest_addr = (PBYTE)pHeader->pBody + ROUNDUP_TO_8(sizeof(packet_vkAllocateMemory)) + packetSize;
        memcpy(pdest_addr, &ahwbuf_desc, sizeof(AHardwareBuffer_Desc));
        memcpy(pdest_addr + sizeof(AHardwareBuffer_Desc), hwbuf_cpu_ptr, import_buf_size);
        AHardwareBuffer_unlock(pImportAHWBuf->buffer, NULL);
        pImportAHWBuf->buffer = (struct AHardwareBuffer*)(pdest_addr - (uint8_t*)pHeader->pBody);
    }
#endif
    pPacket = interpret_body_as_vkAllocateMemory(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocateInfo), sizeof(VkMemoryAllocateInfo), pAllocateInfo);
    if (pAllocateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pAllocateInfo, pAllocateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMemory), sizeof(VkDeviceMemory), pMemory);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMemory));

    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_DeviceMemory_object(*pMemory);
        info.belongsToDevice = device;
        info.ObjectInfo.DeviceMemory.pCreatePacket = trim::copy_packet(pHeader);
        info.ObjectInfo.DeviceMemory.memoryTypeIndex = pAllocateInfo->memoryTypeIndex;
        info.ObjectInfo.DeviceMemory.propertyFlags = trim::LookUpMemoryProperties(device, pAllocateInfo->memoryTypeIndex);
        info.ObjectInfo.DeviceMemory.size = pAllocateInfo->allocationSize;
        VkMemoryDedicatedAllocateInfo *dedicatedAllocateInfo = (VkMemoryDedicatedAllocateInfo*)find_ext_struct((const vulkan_struct_header*)pAllocateInfo->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
        if (dedicatedAllocateInfo != nullptr) {
            info.ObjectInfo.DeviceMemory.boundToBuffer = dedicatedAllocateInfo->buffer;
            info.ObjectInfo.DeviceMemory.boundToImage = dedicatedAllocateInfo->image;
        }
        if (pAllocator != NULL) {
            info.ObjectInfo.DeviceMemory.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    // begin custom code
    add_new_handle_to_mem_info(*pMemory, pAllocateInfo->memoryTypeIndex, pAllocateInfo->allocationSize, NULL);
    // end custom code
    if (UseMappedExternalHostMemoryExtension()) {
        // only needed if user enable using VK_EXT_external_memory_host.
        pageguardExit();
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                                                    VkDeviceSize size, VkFlags flags, void** ppData) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkMapMemory* pPacket = NULL;
    VKAllocInfo* entry;
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    // Handle the situation that the same memory being mapped twice before a unmap.
    LPPageGuardMappedMemory tmpMappedMemory = getPageGuardControlInstance().findMappedMemoryObject(device, memory);
    if (tmpMappedMemory && (tmpMappedMemory->getMappedOffset() == offset) && (tmpMappedMemory->getMappedSize() == size)) {
        *ppData = tmpMappedMemory->getMappedDataPointer();
        pageguardExit();
        return VK_SUCCESS;
    }
#endif
    CREATE_TRACE_PACKET(vkMapMemory, sizeof(void*));
    result = mdd(device)->devTable.MapMemory(device, memory, offset, size, flags, ppData);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    entry = find_mem_info_entry(memory);
    memoryMapParam param = {0};
    // For vktrace usage, clamp the memory size to the total size less offset if VK_WHOLE_SIZE is specified.
    if (size == VK_WHOLE_SIZE) {
        size = entry->totalSize - offset;
    }
    if (result == VK_SUCCESS) {
        param.memory = memory; param.offset = offset; param.size = size;
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    // Pageguard handling will change real mapped memory pointer to a pointer
    // of shadow memory, but trim need to use real mapped pointer to keep
    // the pageguard status no change.
    //
    // So here, we save the real mapped memory pointer before page guard
    // handling replace it with shadow memory pointer.
    getPageGuardControlInstance().vkMapMemoryPageGuardHandle(device, memory, offset, size, flags, ppData);
#endif
    g_memoryMapInfo[*ppData] = param;
    pPacket = interpret_body_as_vkMapMemory(pHeader);
    pPacket->device = device;
    pPacket->memory = memory;
    pPacket->offset = offset;
    pPacket->size = size;
    pPacket->flags = flags;
    if (ppData != NULL) {
        // here we add data(type is void*) pointed by ppData to trace_packet, and put its address to pPacket->ppData
        // after adding to trace_packet.
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData), sizeof(void*), ppData);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppData));
        add_data_to_mem_info(memory, size, offset, *ppData);
    }
    pPacket->result = result;
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_DeviceMemory_objectInfo(memory);
        if (pInfo != NULL) {
            pInfo->ObjectInfo.DeviceMemory.mappedOffset = offset;
            pInfo->ObjectInfo.DeviceMemory.mappedSize = size;

            // Page guard handling create a shadow memory for every mapped
            // memory object and add page guard to capture the access of
            // write and read, so the page guard handling can keep dual
            // direction of sync between real mapped memory and the shadow
            // memory.
            //
            // When starting trim, trim process need to read and save all image
            // and buffer data to trace file, that will trigger all page guard
            // on those memory if trim access shadow memory, and the status of
            // page guard for those memory will be different with capture
            // without trim. It causes corruption for some title if trim at
            // some locations.
            //
            // So here we make trim to use real memory pointer to avoid change
            // the pageguard status when PMB enabled.
            // Note:we'll have to change here if adding pageguard on real
            //      mapped memory in future, because the pointer here may
            //      already have page guard protection before trim dump its
            //      content.
            pInfo->ObjectInfo.DeviceMemory.mappedAddress = *ppData;
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkUnmapMemory* pPacket;
    VKAllocInfo* entry;
    size_t siz = 0;
#if defined(USE_PAGEGUARD_SPEEDUP)
    void* PageGuardMappedData = NULL;
    pageguardEnter();
    getPageGuardControlInstance().vkUnmapMemoryPageGuardHandle(device, memory, &PageGuardMappedData,
                                                               &vkFlushMappedMemoryRangesWithoutAPICall);
#endif
    uint64_t trace_begin_time = vktrace_get_time();

    // insert into packet the data that was written by CPU between the vkMapMemory call and here
    // Note must do this prior to the real vkUnMap() or else may get a FAULT
    vktrace_enter_critical_section(&g_memInfoLock);
    entry = find_mem_info_entry(memory);
    if (entry && entry->pData != NULL) {
        if (entry->didFlush == NoFlush) {
            // no FlushMapped Memory
            siz = (size_t)entry->rangeSize;
        }
    }
    // some title is not 4 byte aligned when call vkMapMemory, so we need
    // ROUNDUP_TO_4 to avoid access invalid memory
    CREATE_TRACE_PACKET(vkUnmapMemory, ROUNDUP_TO_4(siz));
    pHeader->vktrace_begin_time = trace_begin_time;
    pPacket = interpret_body_as_vkUnmapMemory(pHeader);
    if (siz) {
        assert(entry->handle == memory);
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pData), siz, entry->pData);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pData));
    }
    entry->pData = NULL;
    vktrace_leave_critical_section(&g_memInfoLock);
    pHeader->entrypoint_begin_time = vktrace_get_time();
    mdd(device)->devTable.UnmapMemory(device, memory);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket->device = device;
    pPacket->memory = memory;
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_DeviceMemory_objectInfo(memory);
        if (pInfo != NULL) {
            pInfo->ObjectInfo.DeviceMemory.mappedOffset = 0;
            pInfo->ObjectInfo.DeviceMemory.mappedSize = 0;
            pInfo->ObjectInfo.DeviceMemory.mappedAddress = NULL;
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    if (PageGuardMappedData != nullptr) {
        pageguardFreeMemory(PageGuardMappedData);
    }
    pageguardExit();
#endif
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                                                 const VkAllocationCallbacks* pAllocator) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkFreeMemory* pPacket = NULL;
    bool bFind = false;
    if (g_trimEnabled && !g_trimIsInTrim) {
        for (auto& e : g_bufferToDeviceMemory) {
            if (e.second.memory == memory) {
                auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(), e.first);
                if (it0 != g_bufferInCmdBuildAS.end()) {
                    bFind = true;
                    break;
                }
            }
        }
    }
    if (bFind) {
        return ;
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    // There are some apps call vkFreeMemory without call vkUnmapMemory on
    // same memory object. in that situation, capture/playback run into error.
    // so add process here for that situation.
    pageguardEnter();
    if (getPageGuardControlInstance().findMappedMemoryObject(device, memory) != nullptr) {
        void* PageGuardMappedData = nullptr;
        getPageGuardControlInstance().vkUnmapMemoryPageGuardHandle(device, memory, &PageGuardMappedData,
                                                                   &vkFlushMappedMemoryRangesWithoutAPICall);
        if (PageGuardMappedData != nullptr) {
            pageguardFreeMemory(PageGuardMappedData);
        }
    }
    getPageGuardControlInstance().vkFreeMemoryPageGuardHandle(device, memory, pAllocator);
    pageguardExit();
#endif
    CREATE_TRACE_PACKET(vkFreeMemory, sizeof(VkAllocationCallbacks));
    mdd(device)->devTable.FreeMemory(device, memory, pAllocator);
    if (UseMappedExternalHostMemoryExtension()) {
        pageguardEnter();
        getPageGuardControlInstance().vkFreeMemoryPageGuardHandle(device, memory, pAllocator);
        pageguardExit();
    }
    auto it = g_memoryMapInfo.begin();
    while (it != g_memoryMapInfo.end()) {
        if (it->second.memory == memory) {
            it = g_memoryMapInfo.erase(it);
        } else {
            it++;
        }
    }
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkFreeMemory(pHeader);
    pPacket->device = device;
    pPacket->memory = memory;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::remove_DeviceMemory_object(memory);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    // begin custom code
    rm_handle_from_mem_info(memory);
    // end custom code
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                                                                       const VkMappedMemoryRange* pMemoryRanges) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    size_t rangesSize = 0;
    size_t dataSize = 0;
    uint32_t iter;
    packet_vkInvalidateMappedMemoryRanges* pPacket = NULL;
    uint64_t trace_begin_time = vktrace_get_time();

#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    if (!UseMappedExternalHostMemoryExtension()) {
        // If enable external host memory extension, there will be no shadow
        // memory, so we don't need any read pageguard handling.
        resetAllReadFlagAndPageGuard();
    }
#endif

    // determine sum of sizes of memory ranges and pNext structures
    for (iter = 0; iter < memoryRangeCount; iter++) {
        VkMappedMemoryRange* pRange = (VkMappedMemoryRange*)&pMemoryRanges[iter];
        VKAllocInfo* pEntry = find_mem_info_entry(pRange->memory);
        rangesSize += vk_size_vkmappedmemoryrange(pRange);
        uint64_t range_size = pRange->size;
        if (pRange->size == VK_WHOLE_SIZE) {
            assert(pRange->offset >= pEntry->rangeOffset);
            range_size = pEntry->totalSize - pRange->offset;
            assert(range_size <= pEntry->rangeSize);
        }
        dataSize += ROUNDUP_TO_4((size_t)range_size);
        dataSize += get_struct_chain_size((void*)pRange);
    }

    CREATE_TRACE_PACKET(vkInvalidateMappedMemoryRanges, rangesSize + sizeof(void*) * memoryRangeCount + dataSize);
    pHeader->vktrace_begin_time = trace_begin_time;
    pPacket = interpret_body_as_vkInvalidateMappedMemoryRanges(pHeader);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMemoryRanges), rangesSize, pMemoryRanges);

    // add the pnext structures to the packet
    for (iter = 0; iter < memoryRangeCount; iter++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)&pPacket->pMemoryRanges[iter], (void*)&pMemoryRanges[iter]);

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMemoryRanges));

    // insert into packet the data that was written by CPU between the vkMapMemory call and here
    // create a temporary local ppData array and add it to the packet (to reserve the space for the array)
    void** ppTmpData = (void**)malloc(memoryRangeCount * sizeof(void*));
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData), sizeof(void*) * memoryRangeCount, ppTmpData);
    free(ppTmpData);

    // now the actual memory
    vktrace_enter_critical_section(&g_memInfoLock);
    for (iter = 0; iter < memoryRangeCount; iter++) {
        VkMappedMemoryRange* pRange = (VkMappedMemoryRange*)&pMemoryRanges[iter];
        VKAllocInfo* pEntry = find_mem_info_entry(pRange->memory);

        if (pEntry != NULL) {
            assert(pEntry->handle == pRange->memory);
            uint64_t range_size = 0;
            if (pRange->size != VK_WHOLE_SIZE) {
                assert(pEntry->totalSize >= (pRange->size + pRange->offset));
                assert(pEntry->totalSize >= pRange->size);
                assert(pRange->offset >= pEntry->rangeOffset &&
                       (pRange->offset + pRange->size) <= (pEntry->rangeOffset + pEntry->rangeSize));
                range_size = pRange->size;
            } else {
                range_size = pEntry->totalSize - pRange->offset;
            }
            vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData[iter]), range_size,
                                               pEntry->pData + pRange->offset);
            vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppData[iter]));
        } else {
            vktrace_LogError("Failed to copy app memory into trace packet (idx = %u) on vkInvalidateMappedMemoryRanges",
                             pHeader->global_packet_index);
        }
    }
    vktrace_leave_critical_section(&g_memInfoLock);

    // now finalize the ppData array since it is done being updated
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppData));

    pHeader->entrypoint_begin_time = vktrace_get_time();
    result = mdd(device)->devTable.InvalidateMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket->device = device;
    pPacket->memoryRangeCount = memoryRangeCount;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                                                                  const VkMappedMemoryRange* pMemoryRanges) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    uint64_t rangesSize = 0;
    uint64_t dataSize = 0;
    uint64_t pnextSize = 0;
    uint32_t iter;
    packet_vkFlushMappedMemoryRanges* pPacket = NULL;
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    PBYTE* ppPackageData = new PBYTE[memoryRangeCount];
    getPageGuardControlInstance().vkFlushMappedMemoryRangesPageGuardHandle(
        device, memoryRangeCount, pMemoryRanges, ppPackageData);  // the packet is not needed if no any change on data of all ranges
#endif

    uint64_t trace_begin_time = vktrace_get_time();

// find out how much memory is in the ranges
#if !defined(USE_PAGEGUARD_SPEEDUP)
    for (iter = 0; iter < memoryRangeCount; iter++) {
        VkMappedMemoryRange* pRange = (VkMappedMemoryRange*)&pMemoryRanges[iter];
        dataSize += ROUNDUP_TO_4((size_t)(getPageGuardControlInstance().getMappedMemorySize(device, pRange->memory)));
    }
#else
    dataSize = getPageGuardControlInstance().getALLChangedPackageSizeInMappedMemory(device, memoryRangeCount, pMemoryRanges,
                                                                                    ppPackageData);
#endif
    rangesSize = sizeof(VkMappedMemoryRange) * memoryRangeCount;

    // determine size of pnext chains
    for (iter = 0; iter < memoryRangeCount; iter++) {
        VkMappedMemoryRange* pRange = (VkMappedMemoryRange*)&pMemoryRanges[iter];
        pnextSize += get_struct_chain_size((void*)pRange);
    }

    CREATE_TRACE_PACKET(vkFlushMappedMemoryRanges, rangesSize + sizeof(void*) * memoryRangeCount + dataSize + pnextSize);
    pHeader->vktrace_begin_time = trace_begin_time;
    pPacket = interpret_body_as_vkFlushMappedMemoryRanges(pHeader);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMemoryRanges), rangesSize, pMemoryRanges);

    // add the pnext structures to the packet
    for (iter = 0; iter < memoryRangeCount; iter++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)&pPacket->pMemoryRanges[iter], (void*)&pMemoryRanges[iter]);

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMemoryRanges));

    // insert into packet the data that was written by CPU between the vkMapMemory call and here
    // create a temporary local ppData array and add it to the packet (to reserve the space for the array)
    void** ppTmpData = (void**)malloc(memoryRangeCount * sizeof(void*));
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData), sizeof(void*) * memoryRangeCount, ppTmpData);
    free(ppTmpData);

    // now the actual memory
    vktrace_enter_critical_section(&g_memInfoLock);
    std::set<VkDeviceMemory> flushed_mem;
    for (iter = 0; iter < memoryRangeCount; iter++) {
        VkMappedMemoryRange* pRange = (VkMappedMemoryRange*)&pMemoryRanges[iter];

        if (flushed_mem.find(pRange->memory) != flushed_mem.end()) continue;
        flushed_mem.insert(pRange->memory);


        VKAllocInfo* pEntry = find_mem_info_entry(pRange->memory);

        if (pEntry != NULL) {
#if defined(PLATFORM_LINUX)
            VkDeviceSize rangeSize __attribute__((unused));
#else
            VkDeviceSize rangeSize;
#endif
            if (pRange->size == VK_WHOLE_SIZE) {
                LPPageGuardMappedMemory pOPTMemoryTemp = getPageGuardControlInstance().findMappedMemoryObject(device, pRange);
                rangeSize = getPageGuardControlInstance().getMappedMemorySize(device, pRange->memory) -
                            (pRange->offset - pOPTMemoryTemp->getMappedOffset());
            } else
                rangeSize = pRange->size;
            assert(pEntry->handle == pRange->memory);
            assert(pEntry->totalSize >= (rangeSize + pRange->offset));
            assert(pEntry->totalSize >= rangeSize);
            assert(pRange->offset >= pEntry->rangeOffset &&
                   (pRange->offset + rangeSize) <= (pEntry->rangeOffset + pEntry->rangeSize));
#if defined(USE_PAGEGUARD_SPEEDUP)
            LPPageGuardMappedMemory pOPTMemoryTemp = getPageGuardControlInstance().findMappedMemoryObject(device, pRange);
            VkDeviceSize OPTPackageSizeTemp = 0;
            if (pOPTMemoryTemp && !pOPTMemoryTemp->noGuard()) {
                PBYTE pOPTDataTemp = pOPTMemoryTemp->getChangedDataPackage(&OPTPackageSizeTemp);
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData[iter]), OPTPackageSizeTemp, pOPTDataTemp);
                pOPTMemoryTemp->clearChangedDataPackage();
                pOPTMemoryTemp->resetMemoryObjectAllChangedFlagAndPageGuard();
            } else {
                PBYTE pOPTDataTemp =
                    getPageGuardControlInstance().getChangedDataPackageOutOfMap(ppPackageData, iter, &OPTPackageSizeTemp);
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData[iter]), OPTPackageSizeTemp, pOPTDataTemp);
                getPageGuardControlInstance().clearChangedDataPackageOutOfMap(ppPackageData, iter);
            }
#else
            vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppData[iter]), rangeSize,
                                               pEntry->pData + pRange->offset);
#endif
            vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppData[iter]));
            pEntry->didFlush = ApiFlush;
        } else {
            vktrace_LogError("Failed to copy app memory into trace packet (idx = %u) on vkFlushedMappedMemoryRanges",
                             pHeader->global_packet_index);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    delete[] ppPackageData;
#endif
    vktrace_leave_critical_section(&g_memInfoLock);

    // now finalize the ppData array since it is done being updated
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppData));

    pHeader->entrypoint_begin_time = vktrace_get_time();
    result = mdd(device)->devTable.FlushMappedMemoryRanges(device, memoryRangeCount, pMemoryRanges);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket->device = device;
    pPacket->memoryRangeCount = memoryRangeCount;
    pPacket->result = result;
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            // Currently tracing the frame, so need to track references & store packet to write post-tracing.
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkAllocateCommandBuffers(VkDevice device,
                                                                                 const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                                                 VkCommandBuffer* pCommandBuffers) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkAllocateCommandBuffers* pPacket = NULL;
    CREATE_TRACE_PACKET(vkAllocateCommandBuffers,
                        get_struct_chain_size((void*)pAllocateInfo) + sizeof(VkCommandBuffer) * pAllocateInfo->commandBufferCount);
    result = mdd(device)->devTable.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkAllocateCommandBuffers(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocateInfo), sizeof(VkCommandBufferAllocateInfo),
                                       pAllocateInfo);
    if (pAllocateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pAllocateInfo, (void*)pAllocateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCommandBuffers),
                                       sizeof(VkCommandBuffer) * pAllocateInfo->commandBufferCount, pCommandBuffers);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCommandBuffers));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        trim::ObjectInfo* pPoolInfo = trim::get_CommandPool_objectInfo(pAllocateInfo->commandPool);
        if (pPoolInfo != NULL) {
            pPoolInfo->ObjectInfo.CommandPool.numCommandBuffersAllocated[pAllocateInfo->level] += pAllocateInfo->commandBufferCount;
        }

        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
            trim::ObjectInfo& info = trim::add_CommandBuffer_object(pCommandBuffers[i]);
            info.belongsToDevice = device;
            info.ObjectInfo.CommandBuffer.commandPool = pAllocateInfo->commandPool;
            info.ObjectInfo.CommandBuffer.level = pAllocateInfo->level;
        }

        if (g_trimIsInTrim) {
            trim::mark_CommandPool_reference(pAllocateInfo->commandPool);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
            g_commandBufferToDevice[pCommandBuffers[i]] = device;
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkResetCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolResetFlags flags) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkResetCommandPool* pPacket = NULL;
    CREATE_TRACE_PACKET(vkResetCommandPool, 0);
    result = mdd(device)->devTable.ResetCommandPool(device, commandPool, flags);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkResetCommandPool(pHeader);
    pPacket->device = device;
    pPacket->commandPool = commandPool;
    pPacket->flags = flags;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        trim::ObjectInfo* pPoolInfo = trim::get_CommandPool_objectInfo(commandPool);
        if (pPoolInfo != NULL) {
            trim::reset_CommandPool_object(commandPool);
        }

        if (g_trimIsInTrim) {
            trim::mark_CommandPool_reference(commandPool);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                                             const VkCommandBufferBeginInfo* pBeginInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkBeginCommandBuffer* pPacket = NULL;
    CREATE_TRACE_PACKET(vkBeginCommandBuffer, get_struct_chain_size((void*)pBeginInfo));
    result = mdd(commandBuffer)->devTable.BeginCommandBuffer(commandBuffer, pBeginInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkBeginCommandBuffer(pHeader);
    pPacket->commandBuffer = commandBuffer;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBeginInfo), sizeof(VkCommandBufferBeginInfo), pBeginInfo);
    if (pBeginInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pBeginInfo, (void*)pBeginInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBeginInfo->pInheritanceInfo),
                                       sizeof(VkCommandBufferInheritanceInfo), pBeginInfo->pInheritanceInfo);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBeginInfo->pInheritanceInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBeginInfo));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        trim::remove_CommandBuffer_calls(commandBuffer);
        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        trim::ClearImageTransitions(commandBuffer);
        trim::ClearBufferTransitions(commandBuffer);
        trim::clear_binding_Pipelines_from_CommandBuffer(commandBuffer);

        if (g_trimIsInTrim) {
            auto queryCmd = g_queryCmdStatus.find(commandBuffer);
            if (queryCmd != g_queryCmdStatus.end()) {
                queryCmd->second.clear();
            }

            if (pBeginInfo->pInheritanceInfo) {
                trim::mark_Framebuffer_reference(pBeginInfo->pInheritanceInfo->framebuffer);
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkEnumeratePhysicalDeviceGroups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkEnumeratePhysicalDeviceGroups* pPacket = NULL;
    result = mid(instance)->instTable.EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if (pPhysicalDeviceGroupProperties) {
        for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; ++i) {
            pPhysicalDeviceGroupProperties[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
        }
    }
    CREATE_TRACE_PACKET(vkEnumeratePhysicalDeviceGroups, sizeof(uint32_t) + get_struct_chain_size((void*)pPhysicalDeviceGroupProperties));
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkEnumeratePhysicalDeviceGroups(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPhysicalDeviceGroupCount), sizeof(uint32_t), pPhysicalDeviceGroupCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPhysicalDeviceGroupProperties), (*pPhysicalDeviceGroupCount) * sizeof(VkPhysicalDeviceGroupProperties), pPhysicalDeviceGroupProperties);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPhysicalDeviceGroupCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPhysicalDeviceGroupProperties));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (result == VK_SUCCESS) {
            trim::ObjectInfo* pInfo = trim::get_Instance_objectInfo(instance);
            if (pInfo != NULL && pPhysicalDeviceGroupCount != NULL && pPhysicalDeviceGroupProperties == NULL) {
                pInfo->ObjectInfo.Instance.pEnumeratePhysicalDeviceGroupsCountPacket = trim::copy_packet(pHeader);
            }

            if (pPhysicalDeviceGroupProperties != NULL && pPhysicalDeviceGroupCount != NULL) {
                if (pInfo != NULL) {
                    pInfo->ObjectInfo.Instance.pEnumeratePhysicalDeviceGroupsPacket = trim::copy_packet(pHeader);
                }

                for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
                    for (uint32_t j = 0; j < pPhysicalDeviceGroupProperties[i].physicalDeviceCount; j++) {
                        trim::ObjectInfo& PDInfo = trim::add_PhysicalDevice_object(pPhysicalDeviceGroupProperties[i].physicalDevices[j]);
                        PDInfo.belongsToInstance = instance;
                        // Get the memory properties of the device
                        mid(instance)->instTable.GetPhysicalDeviceMemoryProperties(
                            pPhysicalDeviceGroupProperties[i].physicalDevices[j], &PDInfo.ObjectInfo.PhysicalDevice.physicalDeviceMemoryProperties);
                    }
                }
            }
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateDescriptorPool(VkDevice device,
                                                                               const VkDescriptorPoolCreateInfo* pCreateInfo,
                                                                               const VkAllocationCallbacks* pAllocator,
                                                                               VkDescriptorPool* pDescriptorPool) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateDescriptorPool* pPacket = NULL;
    // begin custom code (needs to use get_struct_chain_size)
    CREATE_TRACE_PACKET(vkCreateDescriptorPool,
                        get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkDescriptorPool));
    // end custom code
    result = mdd(device)->devTable.CreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateDescriptorPool(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkDescriptorPoolCreateInfo), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, (void*)pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pPoolSizes),
                                       pCreateInfo->poolSizeCount * sizeof(VkDescriptorPoolSize), pCreateInfo->pPoolSizes);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorPool), sizeof(VkDescriptorPool), pDescriptorPool);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pPoolSizes));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorPool));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_DescriptorPool_object(*pDescriptorPool);
        info.belongsToDevice = device;
        info.ObjectInfo.DescriptorPool.pCreatePacket = trim::copy_packet(pHeader);
        info.ObjectInfo.DescriptorPool.createFlags = pCreateInfo->flags;
        info.ObjectInfo.DescriptorPool.maxSets = pCreateInfo->maxSets;
        info.ObjectInfo.DescriptorPool.numSets = 0;

        if (pAllocator != NULL) {
            info.ObjectInfo.DescriptorPool.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VkLayerDeviceCreateInfo* get_chain_info(const VkDeviceCreateInfo* pCreateInfo, VkLayerFunction func) {
    VkLayerDeviceCreateInfo* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain_info->function == func)) {
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
    }
    assert(chain_info != NULL);
    return chain_info;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                                                  VkPhysicalDeviceProperties* pProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceProperties* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceProperties, sizeof(VkPhysicalDeviceProperties));
    mid(physicalDevice)->instTable.GetPhysicalDeviceProperties(physicalDevice, pProperties);
    // Munge the pipeline cache UUID so app won't use the pipeline cache. This increases portability of the trace file.
    memset(pProperties->pipelineCacheUUID, 0xff, sizeof(pProperties->pipelineCacheUUID));
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetPhysicalDeviceProperties(pHeader);
    pPacket->physicalDevice = physicalDevice;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pProperties), sizeof(VkPhysicalDeviceProperties), pProperties);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pProperties));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            trim::ObjectInfo* pInfo = trim::get_PhysicalDevice_objectInfo(physicalDevice);

            // We need to record this packet for vktrace portabilitytable handling. The table
            // is used to determine what memory index should be used in vkAllocateMemory
            // during playback.
            //
            // Here we record it to make sure the trimmed trace file include this packet.
            // During playback, vkreplay use the packet to get hardware and driver
            // infomation of capturing runtime, then vkreplay handle portabilitytable
            // with these infomation in vkAllocateMemory to decide if capture/playback
            // runtime are same platform or not, if it's different platform, some process
            // will apply to make it can playback on the different platform.
            //
            // Without this packet, the portability process will be wrong and it cause some
            // title crash when playback on same platform.
            if (pInfo != nullptr) {
                pInfo->ObjectInfo.PhysicalDevice.pGetPhysicalDevicePropertiesPacket = trim::copy_packet(pHeader);
            }
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice,
                                                                                      VkPhysicalDeviceProperties2KHR* pProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceProperties2KHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceProperties2KHR, get_struct_chain_size((void*)pProperties));
    // Munge the pipeline cache UUID so app won't use the pipeline cache. This increases portability of the trace file.
    memset(pProperties->properties.pipelineCacheUUID, 0xff, sizeof(pProperties->properties.pipelineCacheUUID));
    mid(physicalDevice)->instTable.GetPhysicalDeviceProperties2KHR(physicalDevice, pProperties);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetPhysicalDeviceProperties2KHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pProperties), sizeof(VkPhysicalDeviceProperties2KHR),
                                       pProperties);
    vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pProperties, pProperties);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pProperties));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            trim::ObjectInfo* pInfo = trim::get_PhysicalDevice_objectInfo(physicalDevice);
            // See comment above in __HOOKED_vkGetPhysicalDeviceProperties
            if (pInfo != nullptr) {
                pInfo->ObjectInfo.PhysicalDevice.pGetPhysicalDeviceProperties2KHRPacket = trim::copy_packet(pHeader);
            }
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                                       const VkDeviceCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateDevice* pPacket = NULL;

    VkLayerDeviceCreateInfo* chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    assert(fpGetInstanceProcAddr);
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    assert(fpGetDeviceProcAddr);
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(NULL, "vkCreateDevice");
    if (fpCreateDevice == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    result = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }
#ifdef USE_PAGEGUARD_SPEEDUP
    pageguardEnter();
    getPageGuardControlInstance().vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    pageguardExit();
#endif

    initDeviceData(*pDevice, fpGetDeviceProcAddr, g_deviceDataMap);
    // Setup device dispatch table for extensions
    ext_init_create_device(mdd(*pDevice), *pDevice, fpGetDeviceProcAddr, pCreateInfo->enabledExtensionCount,
                           pCreateInfo->ppEnabledExtensionNames);

    // remove the loader extended createInfo structure
    VkDeviceCreateInfo localCreateInfo;
    memcpy(&localCreateInfo, pCreateInfo, sizeof(localCreateInfo));
    localCreateInfo.pNext = strip_create_extensions(pCreateInfo->pNext);

    // determine size of pnext chains
    size_t pnextSize = 0;
    {
        if (pCreateInfo) pnextSize = get_struct_chain_size((void*)&localCreateInfo);
        for (uint32_t iter = 0; iter < localCreateInfo.queueCreateInfoCount; iter++) {
            pnextSize += get_struct_chain_size((void*)&localCreateInfo.pQueueCreateInfos[iter]);
        }
    }

    CREATE_TRACE_PACKET(vkCreateDevice, pnextSize + sizeof(VkAllocationCallbacks) + sizeof(VkDevice));
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateDevice(pHeader);
    pPacket->physicalDevice = physicalDevice;
    add_VkDeviceCreateInfo_to_packet(pHeader, (VkDeviceCreateInfo**)&(pPacket->pCreateInfo), &localCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDevice), sizeof(VkDevice), pDevice);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDevice));
    PFN_vkGetPhysicalDeviceFeatures2KHR func = mid(physicalDevice)->instTable.GetPhysicalDeviceFeatures2KHR;
    if (func == nullptr) {
        func = mid(physicalDevice)->instTable.GetPhysicalDeviceFeatures2;
    }
    deviceFeatureSupport dfs = query_device_feature(func, physicalDevice, pCreateInfo);
    g_deviceToFeatureSupport[*pDevice] = dfs;
    uint32_t features = 0;
    features = features | (PACKET_TAG_ASCAPTUREREPLAY * dfs.accelerationStructureCaptureReplay);
    features = features | (PACKET_TAG_BUFFERCAPTUREREPLAY * dfs.bufferDeviceAddressCaptureReplay);
    vktrace_tag_trace_packet(pHeader, features);
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_Device_object(*pDevice);
        info.belongsToPhysicalDevice = physicalDevice;
        info.ObjectInfo.Device.pCreatePacket = trim::copy_packet(pHeader);

        trim::ObjectInfo* pPhysDevInfo = trim::get_PhysicalDevice_objectInfo(physicalDevice);
        if (pPhysDevInfo != nullptr) {
            info.ObjectInfo.Device.queueFamilyCount = pPhysDevInfo->ObjectInfo.PhysicalDevice.queueFamilyCount;
            info.ObjectInfo.Device.pQueueFamilies = VKTRACE_NEW_ARRAY(trim::QueueFamily, info.ObjectInfo.Device.queueFamilyCount);
            for (uint32_t family = 0; family < info.ObjectInfo.Device.queueFamilyCount; family++) {
                info.ObjectInfo.Device.pQueueFamilies[family].count = 0;
                info.ObjectInfo.Device.pQueueFamilies[family].queues = nullptr;
            }

            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
                uint32_t queueFamilyIndex = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
                uint32_t count = pCreateInfo->pQueueCreateInfos[i].queueCount;

                info.ObjectInfo.Device.pQueueFamilies[queueFamilyIndex].count = count;
                info.ObjectInfo.Device.pQueueFamilies[queueFamilyIndex].queues = VKTRACE_NEW_ARRAY(VkQueue, count);

                for (uint32_t q = 0; q < count; q++) {
                    VkQueue queue = VK_NULL_HANDLE;
                    mdd(*pDevice)->devTable.GetDeviceQueue(*pDevice, queueFamilyIndex, q, &queue);
                    info.ObjectInfo.Device.pQueueFamilies[queueFamilyIndex].queues[q] = queue;

                    // Because this queue was not retrieved through the loader's
                    // trampoile function, we need to assign the dispatch table here
                    *(void**)queue = *(void**)*pDevice;
                }
            }
        }
        if (pAllocator != NULL) {
            info.ObjectInfo.Device.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    g_deviceToPhysicalDevice[*pDevice] = physicalDevice;
    return result;
}

bool isMapMemoryAddress(const void* hostAddress) {
    bool isMapAddress = false;
    uint64_t address = (uint64_t)hostAddress;
    for(auto&it : g_memoryMapInfo) {
        uint64_t start = (uint64_t)(it.first);
        if (address >= start && address < start + it.second.size ) {
            isMapAddress = true;
            break;
        }
    }
    return isMapAddress;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateAccelerationStructureKHR(
    VkDevice device,
    const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkAccelerationStructureKHR* pAccelerationStructure) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateAccelerationStructureKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCreateAccelerationStructureKHR, get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkAccelerationStructureKHR));
    result = mdd(device)->devTable.CreateAccelerationStructureKHR(device, pCreateInfo, pAllocator, pAccelerationStructure);
    if (result == VK_SUCCESS) {
        auto it = g_deviceToFeatureSupport.find(device);
        if (it != g_deviceToFeatureSupport.end() && it->second.accelerationStructureCaptureReplay) {
            VkAccelerationStructureDeviceAddressInfoKHR asda = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, nullptr, *pAccelerationStructure};
            const_cast<VkAccelerationStructureCreateInfoKHR*>(pCreateInfo)->deviceAddress = mdd(device)->devTable.GetAccelerationStructureDeviceAddressKHR(device, &asda);
        } else {
            const_cast<VkAccelerationStructureCreateInfoKHR*>(pCreateInfo)->deviceAddress = 0;
        }
    }
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateAccelerationStructureKHR(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkAccelerationStructureCreateInfoKHR), pCreateInfo);
    vktrace_add_pnext_structs_to_trace_packet(pHeader, (void *)pPacket->pCreateInfo, (void *)pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAccelerationStructure), sizeof(VkAccelerationStructureKHR), pAccelerationStructure);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAccelerationStructure));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_Buffer_objectInfo(pCreateInfo->buffer);
        if (pInfo != NULL) {
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
            g_AStoBuffer[*pAccelerationStructure] = pCreateInfo->buffer;
#endif
            trim::ObjectInfo& info = trim::add_AccelerationStructure_object(*pAccelerationStructure);
            info.belongsToDevice = device;
            vktrace_trace_packet_header* pCreatePacket = trim::copy_packet(pHeader);
            info.ObjectInfo.AccelerationStructure.pCreatePacket = pCreatePacket;
            if (pAllocator != NULL) {
                info.ObjectInfo.AccelerationStructure.pAllocator = pAllocator;
                trim::add_Allocator(pAllocator);
            }
        }
        if (g_trimIsInTrim) {
            trim::mark_Buffer_reference(pCreateInfo->buffer);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}


VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkGetAccelerationStructureBuildSizesKHR(
    VkDevice device,
    VkAccelerationStructureBuildTypeKHR buildType,
    const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
    const uint32_t* pMaxPrimitiveCounts,
    VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkGetAccelerationStructureBuildSizesKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetAccelerationStructureBuildSizesKHR, sizeof(VkAccelerationStructureBuildGeometryInfoKHR) +
                                                                 get_struct_chain_size((void*)pBuildInfo) +
                                                                 (pBuildInfo->geometryCount) * sizeof(uint32_t) +
                                                                 get_struct_chain_size(pSizeInfo));
    mdd(device)->devTable.GetAccelerationStructureBuildSizesKHR(device, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    if (pSizeInfo->accelerationStructureSize < ref_target_range_size())
        pSizeInfo->accelerationStructureSize = ref_target_range_size();
    pPacket = interpret_body_as_vkGetAccelerationStructureBuildSizesKHR(pHeader);
    pPacket->device = device;
    pPacket->buildType = buildType;
    add_VkAccelerationStructureBuildGeometryInfoKHR_to_packet(pHeader, (VkAccelerationStructureBuildGeometryInfoKHR**)&(pPacket->pBuildInfo), (VkAccelerationStructureBuildGeometryInfoKHR*)pBuildInfo, true, 0, false, NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMaxPrimitiveCounts), (pBuildInfo->geometryCount) * sizeof(uint32_t), pMaxPrimitiveCounts);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSizeInfo), sizeof(VkAccelerationStructureBuildSizesInfoKHR), pSizeInfo);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMaxPrimitiveCounts));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSizeInfo));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkBuildAccelerationStructuresKHR(
    VkDevice device,
    VkDeferredOperationKHR deferredOperation,
    uint32_t infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkBuildAccelerationStructuresKHR* pPacket = NULL;
    int pInfosSize = 0;
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    flushAllChangedMappedMemory(&vkFlushMappedMemoryRangesWithoutAPICall);
    if (!UseMappedExternalHostMemoryExtension()) {
        // If enable external host memory extension, there will be no shadow
        // memory, so we don't need any read pageguard handling.
        resetAllReadFlagAndPageGuard();
    }
#endif
    char** hostAddressBit = (char**)malloc(infoCount * sizeof(void*));
    for (uint32_t i = 0; i < infoCount; ++i) {
        hostAddressBit[i] = (char*)malloc(pInfos[i].geometryCount * sizeof(char));
        memset(hostAddressBit[i], 0, pInfos[i].geometryCount * sizeof(char));
    }
    for (uint32_t i = 0; i < infoCount; ++i) {
        pInfosSize += get_struct_chain_size((void*)&pInfos[i]);
        for (uint32_t j = 0; j < pInfos[i].geometryCount; ++j) {
            if (pInfos[i].pGeometries[j].geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
                int size = ppBuildRangeInfos[i][j].primitiveCount;
                pInfosSize += sizeof(VkAccelerationStructureInstanceKHR) * size;
            }
            else if (pInfos[i].pGeometries[j].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
                if (pInfos[i].pGeometries[j].geometry.triangles.vertexData.hostAddress != NULL && !isMapMemoryAddress(pInfos[i].pGeometries[j].geometry.triangles.vertexData.hostAddress)) {
                    //int vbOffset = ppBuildRangeInfos[i][j].primitiveOffset + pInfos[i].pGeometries[j].geometry.triangles.vertexStride * ppBuildRangeInfos[i][j].firstVertex;
                    int vbUsedSize = pInfos[i].pGeometries[j].geometry.triangles.vertexStride * pInfos[i].pGeometries[j].geometry.triangles.maxVertex;
                    //int vbTotalSize = vbOffset + vbUsedSize;
                    pInfosSize += vbUsedSize;
                    hostAddressBit[i][j] = hostAddressBit[i][j] | AS_GEOMETRY_TRIANGLES_VERTEXDATA_BIT;
                }
                if (pInfos[i].pGeometries[j].geometry.triangles.indexData.hostAddress != NULL && !isMapMemoryAddress(pInfos[i].pGeometries[j].geometry.triangles.indexData.hostAddress)) {
                    //int ibOffset = ppBuildRangeInfos[i][j].primitiveOffset;
                    int ibUsedSize = ppBuildRangeInfos[i][j].primitiveCount * getVertexIndexStride(pInfos[i].pGeometries[j].geometry.triangles.indexType) * 3;
                    //int ibTotalSize = ibOffset + ibUsedSize;
                    pInfosSize += ibUsedSize;
                    hostAddressBit[i][j] = hostAddressBit[i][j] | AS_GEOMETRY_TRIANGLES_INDEXDATA_BIT;
                }
                if (pInfos[i].pGeometries[j].geometry.triangles.transformData.hostAddress && !isMapMemoryAddress(pInfos[i].pGeometries[j].geometry.triangles.transformData.hostAddress)) {
                    //int transOffset = ppBuildRangeInfos[i][j].transformOffset;
                    int transUsedSize = sizeof(VkTransformMatrixKHR);
                    //int transTotalSize = transOffset + transUsedSize;
                    pInfosSize += transUsedSize;
                    hostAddressBit[i][j] = hostAddressBit[i][j] | AS_GEOMETRY_TRIANGLES_TRANSFORMDATA_BIT;
                }
            }
            else if (pInfos[i].pGeometries[j].geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
                if (pInfos[i].pGeometries[j].geometry.aabbs.data.hostAddress != NULL && !isMapMemoryAddress(pInfos[i].pGeometries[j].geometry.aabbs.data.hostAddress)) {
                    //int aabbOffset = ppBuildRangeInfos[i][j].primitiveOffset;
                    int aabbUsedSize = ppBuildRangeInfos[i][j].primitiveCount * pInfos[i].pGeometries[j].geometry.aabbs.stride;
                    //int aabbTotalSize = aabbOffset + aabbUsedSize;
                    pInfosSize += aabbUsedSize;
                    hostAddressBit[i][j] = hostAddressBit[i][j] | AS_GEOMETRY_AABB_DATA_BIT;
                }
            }
        }
    }

    int ppBuildRangeInfosSize = 0;
    for (uint32_t i = 0; i < infoCount; ++i) {
        ppBuildRangeInfosSize += sizeof(void*);
        ppBuildRangeInfosSize += pInfos[i].geometryCount * sizeof(VkAccelerationStructureBuildRangeInfoKHR);
    }
    CREATE_TRACE_PACKET(vkBuildAccelerationStructuresKHR, pInfosSize + ppBuildRangeInfosSize);
    result = mdd(device)->devTable.BuildAccelerationStructuresKHR(device, deferredOperation, infoCount, pInfos, ppBuildRangeInfos);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkBuildAccelerationStructuresKHR(pHeader);
    pPacket->device = device;
    pPacket->deferredOperation = deferredOperation;
    pPacket->infoCount = infoCount;
    // add pInfos
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pInfos), infoCount * sizeof(VkAccelerationStructureBuildGeometryInfoKHR), pInfos);
    for (uint32_t i = 0; i < infoCount; ++i) {
        VkAccelerationStructureBuildGeometryInfoKHR* temp = (VkAccelerationStructureBuildGeometryInfoKHR* )&(pPacket->pInfos[i]);
        add_VkAccelerationStructureBuildGeometryInfoKHR_to_packet(pHeader, (VkAccelerationStructureBuildGeometryInfoKHR**)&temp, (VkAccelerationStructureBuildGeometryInfoKHR*)&pInfos[i], false, ppBuildRangeInfos[i], true, hostAddressBit[i]);
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pInfos));

    // add ppBuildRangeInfos
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppBuildRangeInfos), infoCount * sizeof(VkAccelerationStructureBuildRangeInfoKHR*), ppBuildRangeInfos);
    for (uint32_t i = 0; i < infoCount; ++i) {
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppBuildRangeInfos[i]), pInfos[i].geometryCount * sizeof(VkAccelerationStructureBuildRangeInfoKHR), ppBuildRangeInfos[i]);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppBuildRangeInfos[i]));
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppBuildRangeInfos));
    pPacket->result = result;

    for (uint32_t i = 0; i < infoCount; ++i) {
        free(hostAddressBit[i]);
    }
    free(hostAddressBit);

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        // Always save all the vkBuildAccelerationStructuresKHR() when fast forward.
        vktrace_finalize_trace_packet(pHeader);
        bool bASLiving = true;
        for (uint32_t i = 0; i < infoCount; ++i) {
            if (pInfos[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR) {
                trim::ObjectInfo* pDstInfo = trim::get_AccelerationStructure_objectInfo(pInfos[i].dstAccelerationStructure);
                if (pDstInfo == nullptr) {
                    bASLiving = false;
                    break;
                }
            } else if (pInfos[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR){
                trim::ObjectInfo* pSrcInfo = trim::get_AccelerationStructure_objectInfo(pInfos[i].srcAccelerationStructure);
                trim::ObjectInfo* pDstInfo = trim::get_AccelerationStructure_objectInfo(pInfos[i].dstAccelerationStructure);
                if (pSrcInfo == nullptr || pDstInfo == nullptr) {
                    bASLiving = false;
                    break;
                }
            }
        }
        if (g_trimIsInTrim) {
             trim::write_packet(pHeader);
        } else {
            if (bASLiving) {
                trim::add_BuildAccelerationStructure_object(trim::copy_packet(pHeader));
            }
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdBuildAccelerationStructuresKHR(
    VkCommandBuffer commandBuffer,
    uint32_t infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdBuildAccelerationStructuresKHR* pPacket = NULL;
    int pInfosSize = 0;
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    flushAllChangedMappedMemory(&vkFlushMappedMemoryRangesWithoutAPICall);
    if (!UseMappedExternalHostMemoryExtension()) {
        // If enable external host memory extension, there will be no shadow
        // memory, so we don't need any read pageguard handling.
        resetAllReadFlagAndPageGuard();
    }
#endif

    for (uint32_t i = 0; i < infoCount; ++i) {
        pInfosSize += get_struct_chain_size((void*)&pInfos[i]);
        for (uint32_t j = 0; j < pInfos[i].geometryCount; ++j) {
            if (pInfos[i].pGeometries[j].geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
                int size = ppBuildRangeInfos[i][j].primitiveCount;
                pInfosSize += sizeof(VkAccelerationStructureInstanceKHR) * size;
            }
        }
    }
    int ppBuildRangeInfosSize = 0;
    for (uint32_t i = 0; i < infoCount; ++i) {
        ppBuildRangeInfosSize += sizeof(void*);
        ppBuildRangeInfosSize += pInfos[i].geometryCount * sizeof(VkAccelerationStructureBuildRangeInfoKHR);
    }
    CREATE_TRACE_PACKET(vkCmdBuildAccelerationStructuresKHR, pInfosSize + ppBuildRangeInfosSize);
    mdd(commandBuffer)->devTable.CmdBuildAccelerationStructuresKHR(commandBuffer, infoCount, pInfos, ppBuildRangeInfos);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdBuildAccelerationStructuresKHR(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->infoCount = infoCount;
    // add pInfos
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pInfos), infoCount * sizeof(VkAccelerationStructureBuildGeometryInfoKHR), pInfos);
    for (uint32_t i = 0; i < infoCount; ++i) {
        VkAccelerationStructureBuildGeometryInfoKHR* temp = (VkAccelerationStructureBuildGeometryInfoKHR* )&(pPacket->pInfos[i]);
        add_VkAccelerationStructureBuildGeometryInfoKHR_to_packet(pHeader, (VkAccelerationStructureBuildGeometryInfoKHR**)&temp, (VkAccelerationStructureBuildGeometryInfoKHR*)&pInfos[i], false, ppBuildRangeInfos[i], false, NULL);
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pInfos));

    // add ppBuildRangeInfos
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppBuildRangeInfos), infoCount * sizeof(VkAccelerationStructureBuildRangeInfoKHR*), ppBuildRangeInfos);
    for (uint32_t i = 0; i < infoCount; ++i) {
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->ppBuildRangeInfos[i]), pInfos[i].geometryCount * sizeof(VkAccelerationStructureBuildRangeInfoKHR), ppBuildRangeInfos[i]);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppBuildRangeInfos[i]));
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->ppBuildRangeInfos));

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            bool bASLiving = true;
            for (uint32_t i = 0; i < infoCount; i++) {
                if (pInfos[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR) {
                    trim::ObjectInfo* pDstInfo = trim::get_AccelerationStructure_objectInfo(pInfos[i].dstAccelerationStructure);
                    if (pDstInfo == nullptr) {
                       bASLiving = false;
                       break;
                    }
                    trim::mark_AccelerationStructure_reference(pInfos[i].dstAccelerationStructure);
                } else if (pInfos[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) {
                    trim::ObjectInfo* pSrcInfo = trim::get_AccelerationStructure_objectInfo(pInfos[i].srcAccelerationStructure);
                    trim::ObjectInfo* pDstInfo = trim::get_AccelerationStructure_objectInfo(pInfos[i].dstAccelerationStructure);
                    if (pSrcInfo == nullptr || pDstInfo == nullptr) {
                       bASLiving = false;
                       break;
                    }
                    trim::mark_AccelerationStructure_reference(pInfos[i].srcAccelerationStructure);
                    trim::mark_AccelerationStructure_reference(pInfos[i].dstAccelerationStructure);
                }
            }
            if (bASLiving) {
                auto it = std::find(g_commandBufferInCmdBuildAS.begin(), g_commandBufferInCmdBuildAS.end(), commandBuffer);
                if (it == g_commandBufferInCmdBuildAS.end()) {
                    g_commandBufferInCmdBuildAS.push_back(commandBuffer);
                }
                trim::ObjectInfo* pCBInfo = trim::get_CommandBuffer_objectInfo(commandBuffer);
                if (pCBInfo == nullptr) {
                    vktrace_LogError("Can't find the command buffer object.");
                } else {
                    cmdBuildASPacketInfo packetInfo = {pCBInfo->ObjectInfo.CommandBuffer.commandPool, trim::copy_packet(pHeader)};
                    g_cmdBuildASPacket[pHeader->global_packet_index] = packetInfo;
                }
                for (uint32_t i = 0; i < infoCount; ++i) {
                    for (uint32_t j = 0; j < pInfos[i].geometryCount; ++j) {
                        switch (pInfos[i].pGeometries[j].geometryType) {
                            case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
                            if (pInfos[i].pGeometries[j].geometry.triangles.vertexData.deviceAddress != 0) {
                                auto it = g_deviceAddressToBuffer.find(pInfos[i].pGeometries[j].geometry.triangles.vertexData.deviceAddress);
                                if (it == g_deviceAddressToBuffer.end()) {
                                    vktrace_LogError("vertexData device address can't find.");
                                } else {
                                    auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(),it->second);
                                    if (it0 == g_bufferInCmdBuildAS.end()) {
                                        g_bufferInCmdBuildAS.push_back(it->second);
                                    }
                                }
                            }
                            if (pInfos[i].pGeometries[j].geometry.triangles.indexData.deviceAddress != 0) {
                                auto it = g_deviceAddressToBuffer.find(pInfos[i].pGeometries[j].geometry.triangles.indexData.deviceAddress);
                                if (it == g_deviceAddressToBuffer.end()) {
                                    vktrace_LogError("indexData device address can't find.");
                                } else {
                                    auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(),it->second);
                                    if (it0 == g_bufferInCmdBuildAS.end()) {
                                        g_bufferInCmdBuildAS.push_back(it->second);
                                    }
                                }
                            }
                            if (pInfos[i].pGeometries[j].geometry.triangles.transformData.deviceAddress != 0) {
                                auto it = g_deviceAddressToBuffer.find(pInfos[i].pGeometries[j].geometry.triangles.transformData.deviceAddress);
                                if (it == g_deviceAddressToBuffer.end()) {
                                    vktrace_LogError("transformData device address can't find.");
                                } else {
                                    auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(),it->second);
                                    if (it0 == g_bufferInCmdBuildAS.end()) {
                                        g_bufferInCmdBuildAS.push_back(it->second);
                                    }
                                }
                            }
                            break;
                            case VK_GEOMETRY_TYPE_AABBS_KHR:
                            if (pInfos[i].pGeometries[j].geometry.aabbs.data.deviceAddress != 0) {
                                auto it = g_deviceAddressToBuffer.find(pInfos[i].pGeometries[j].geometry.aabbs.data.deviceAddress);
                                if (it == g_deviceAddressToBuffer.end()) {
                                    vktrace_LogError("aabbsData device address can't find.");
                                } else {
                                    auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(),it->second);
                                    if (it0 == g_bufferInCmdBuildAS.end()) {
                                        g_bufferInCmdBuildAS.push_back(it->second);
                                    }
                                }
                            }
                            break;
                            case VK_GEOMETRY_TYPE_INSTANCES_KHR:
                            if (pInfos[i].pGeometries[j].geometry.instances.data.deviceAddress != 0) {
                                auto it = g_deviceAddressToBuffer.find(pInfos[i].pGeometries[j].geometry.instances.data.deviceAddress);
                                if (it == g_deviceAddressToBuffer.end()) {
                                    vktrace_LogError("instancesData device address can't find.");
                                } else {
                                    auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(),it->second);
                                    if (it0 == g_bufferInCmdBuildAS.end()) {
                                        g_bufferInCmdBuildAS.push_back(it->second);
                                    }
                                }
                            }
                            break;
                            default:
                                vktrace_LogError("Can't support the geometry type.");
                        }
                    }
                    if (pInfos[i].scratchData.deviceAddress != 0) {
                        auto it = g_deviceAddressToBuffer.find(pInfos[i].scratchData.deviceAddress);
                        if (it == g_deviceAddressToBuffer.end()) {
                            vktrace_LogError("scratchData device address can't find.");
                        } else {
                            auto it0 = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(), it->second);
                            if (it0 == g_bufferInCmdBuildAS.end()) {
                                g_bufferInCmdBuildAS.push_back(it->second);
                            }
                        }
                    }
                }
            }
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkDestroyAccelerationStructureKHR(
    VkDevice device,
    VkAccelerationStructureKHR accelerationStructure,
    const VkAllocationCallbacks* pAllocator) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkDestroyAccelerationStructureKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkDestroyAccelerationStructureKHR, sizeof(VkAllocationCallbacks));
    mdd(device)->devTable.DestroyAccelerationStructureKHR(device, accelerationStructure, pAllocator);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkDestroyAccelerationStructureKHR(pHeader);
    pPacket->device = device;
    pPacket->accelerationStructure = accelerationStructure;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_AccelerationStructure_objectInfo(accelerationStructure);
        if (pInfo != nullptr) {
            vktrace_delete_trace_packet(&(pInfo->ObjectInfo.AccelerationStructure.pCreatePacket));
        }
        trim::remove_AccelerationStructure_object(accelerationStructure);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
        auto it = g_AStoBuffer.find(accelerationStructure);
        if (it != g_AStoBuffer.end()) {
            g_AStoBuffer.erase(it);
        }
#endif
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateRayTracingPipelinesKHR(
    VkDevice device,
    VkDeferredOperationKHR deferredOperation,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateRayTracingPipelinesKHR* pPacket = NULL;
    size_t additionSize = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        additionSize += get_struct_chain_size((void*)&pCreateInfos[i]);
    }
    CREATE_TRACE_PACKET(vkCreateRayTracingPipelinesKHR, additionSize + sizeof(VkAllocationCallbacks) + createInfoCount * sizeof(VkPipeline));
    result = mdd(device)->devTable.CreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateRayTracingPipelinesKHR(pHeader);
    pPacket->device = device;
    pPacket->deferredOperation = deferredOperation;
    pPacket->pipelineCache = pipelineCache;
    pPacket->createInfoCount = createInfoCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfos), createInfoCount * sizeof(VkRayTracingPipelineCreateInfoKHR), pCreateInfos);
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        VkRayTracingPipelineCreateInfoKHR* temp = (VkRayTracingPipelineCreateInfoKHR* )&(pPacket->pCreateInfos[i]);
        add_VkRayTracingPipelineCreateInfoKHR_to_packet(pHeader, (VkRayTracingPipelineCreateInfoKHR**)&temp, (VkRayTracingPipelineCreateInfoKHR*)&pCreateInfos[i]);
    }

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPipelines), createInfoCount * sizeof(VkPipeline), pPipelines);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfos));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPipelines));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateFramebuffer(VkDevice device,
                                                                            const VkFramebufferCreateInfo* pCreateInfo,
                                                                            const VkAllocationCallbacks* pAllocator,
                                                                            VkFramebuffer* pFramebuffer) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateFramebuffer* pPacket = NULL;
    // begin custom code
    uint32_t attachmentCount = (pCreateInfo != NULL && pCreateInfo->pAttachments != NULL) ? pCreateInfo->attachmentCount : 0;
    CREATE_TRACE_PACKET(vkCreateFramebuffer,
                        get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkFramebuffer));
    // end custom code
    result = mdd(device)->devTable.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateFramebuffer(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkFramebufferCreateInfo), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, (void*)pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pAttachments),
                                       attachmentCount * sizeof(VkImageView), pCreateInfo->pAttachments);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pFramebuffer), sizeof(VkFramebuffer), pFramebuffer);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pAttachments));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pFramebuffer));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_Framebuffer_object(*pFramebuffer);
        info.belongsToDevice = device;
        info.ObjectInfo.Framebuffer.pCreatePacket = trim::copy_packet(pHeader);
        info.ObjectInfo.Framebuffer.attachmentCount = pCreateInfo->attachmentCount;
        if (pCreateInfo->attachmentCount == 0) {
            info.ObjectInfo.Framebuffer.pAttachments = nullptr;
        } else {
            info.ObjectInfo.Framebuffer.pAttachments = new VkImageView[pCreateInfo->attachmentCount];
            memcpy(info.ObjectInfo.Framebuffer.pAttachments, pCreateInfo->pAttachments,
                   sizeof(VkImageView) * pCreateInfo->attachmentCount);
        }
        if (pAllocator != NULL) {
            info.ObjectInfo.Framebuffer.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                trim::mark_ImageView_reference(pCreateInfo->pAttachments[i]);
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VkLayerInstanceCreateInfo* get_chain_info(const VkInstanceCreateInfo* pCreateInfo, VkLayerFunction func) {
    VkLayerInstanceCreateInfo* chain_info = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && ((chain_info->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) || (chain_info->function != func))) {
        chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext;
    }
    assert(chain_info != NULL);
    return chain_info;
}

#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_MEMCPY_USE_PPL_LIB)
extern "C" BOOL vktrace_pageguard_init_multi_threads_memcpy();
extern "C" void vktrace_pageguard_done_multi_threads_memcpy();
#endif

static bool send_vk_trace_file_header(VkInstance instance) {
    bool rval = false;
    uint64_t packet_size;
    vktrace_trace_file_header* pHeader;
    uint32_t physDevCount;
    size_t header_size;
    VkPhysicalDevice* pPhysDevice;
    VkPhysicalDeviceProperties devProperties;
    struct_gpuinfo* pGpuinfo;

    // Find out how many physical devices we have
    if (VK_SUCCESS != mid(instance)->instTable.EnumeratePhysicalDevices(instance, &physDevCount, NULL) || physDevCount < 1) {
        return false;
    }
    header_size = sizeof(vktrace_trace_file_header) + physDevCount * sizeof(struct_gpuinfo);
    packet_size = header_size + sizeof(packet_size);
    if (!(pPhysDevice = (VkPhysicalDevice*)vktrace_malloc(header_size + sizeof(VkPhysicalDevice) * physDevCount))) {
        return false;
    }
    pHeader = (vktrace_trace_file_header*)((PBYTE)pPhysDevice + sizeof(VkPhysicalDevice) * physDevCount);
    pGpuinfo = (struct_gpuinfo*)((PBYTE)pHeader + sizeof(vktrace_trace_file_header));

    // Get information about all physical devices
    if (VK_SUCCESS != mid(instance)->instTable.EnumeratePhysicalDevices(instance, &physDevCount, pPhysDevice)) {
        goto cleanupAndReturn;
    }

    memset(pHeader, 0, header_size);
    pHeader->trace_file_version = VKTRACE_TRACE_FILE_VERSION;
    pHeader->tracer_version = vktrace_version();
    pHeader->magic = VKTRACE_FILE_MAGIC;
    vktrace_gen_uuid(pHeader->uuid);
    pHeader->first_packet_offset = header_size;
    pHeader->tracer_count = 1;
    pHeader->tracer_id_array[0].id = VKTRACE_TID_VULKAN;
    pHeader->tracer_id_array[0].is_64_bit = (sizeof(intptr_t) == 8) ? 1 : 0;
    pHeader->trace_start_time = vktrace_get_time();
    pHeader->endianess = get_endianess();
    pHeader->ptrsize = sizeof(void*);
    pHeader->arch = get_arch();
    pHeader->os = get_os();
    pHeader->n_gpuinfo = physDevCount;
    for (size_t i = 0; i < physDevCount; i++) {
        mid(*(pPhysDevice + i))->instTable.GetPhysicalDeviceProperties(*(pPhysDevice + i), &devProperties);
        pGpuinfo[i].gpu_id = ((uint64_t)devProperties.vendorID << 32) | (uint64_t)devProperties.deviceID;
        pGpuinfo[i].gpu_drv_vers = (uint64_t)devProperties.driverVersion;
    }

    pHeader->enabled_tracer_features = 0;
    if (getForceFifoEnableFlag()) {
        pHeader->enabled_tracer_features |= TRACER_FEAT_FORCE_FIFO;
    }
    if (getPMBSyncGPUDataBackEnableFlag()) {
        pHeader->enabled_tracer_features |= TRACER_FEAT_PG_SYNC_GPU_DATA_BACK;
    }
    if (getDelaySignalFenceFrames() > 0) {
        pHeader->enabled_tracer_features |= TRACER_FEAT_DELAY_SIGNAL_FENCE;
    }

    vktrace_FileLike_WriteRaw(vktrace_trace_get_trace_file(), &packet_size, sizeof(packet_size));
    vktrace_FileLike_WriteRaw(vktrace_trace_get_trace_file(), pHeader, header_size);
    rval = true;

cleanupAndReturn:
    vktrace_free(pPhysDevice);
    return rval;
}

static void send_vk_api_version_packet() {
    packet_vkApiVersion* pPacket;
    vktrace_trace_packet_header* pHeader;
    pHeader = vktrace_create_trace_packet(VKTRACE_TID_VULKAN, VKTRACE_TPI_VK_vkApiVersion, sizeof(packet_vkApiVersion), 0);
    pPacket = interpret_body_as_vkApiVersion(pHeader);
    pPacket->version = VK_MAKE_VERSION(1, 2, VK_HEADER_VERSION);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    FINISH_TRACE_PACKET();
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                                         const VkAllocationCallbacks* pAllocator,
                                                                         VkInstance* pInstance) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateInstance* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint32_t i;
    uint64_t vktraceStartTime = vktrace_get_time();
    SEND_ENTRYPOINT_ID(vkCreateInstance);
    startTime = vktrace_get_time();

#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_MEMCPY_USE_PPL_LIB)
    vktrace_pageguard_init_multi_threads_memcpy();
#endif

    VkLayerInstanceCreateInfo* chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    assert(fpGetInstanceProcAddr);
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (fpCreateInstance == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }
    endTime = vktrace_get_time();

    initInstanceData(*pInstance, fpGetInstanceProcAddr, g_instanceDataMap);
    ext_init_create_instance(mid(*pInstance), *pInstance, pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);

    // remove the loader extended createInfo structure
    VkInstanceCreateInfo localCreateInfo;
    memcpy(&localCreateInfo, pCreateInfo, sizeof(localCreateInfo));

    // Alloc space to copy pointers
    if (localCreateInfo.enabledLayerCount > 0)
        localCreateInfo.ppEnabledLayerNames = (const char* const*)malloc(localCreateInfo.enabledLayerCount * sizeof(char*));
    if (localCreateInfo.enabledExtensionCount > 0)
        localCreateInfo.ppEnabledExtensionNames = (const char* const*)malloc(localCreateInfo.enabledExtensionCount * sizeof(char*));

    for (i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        char** ppName = (char**)&localCreateInfo.ppEnabledExtensionNames[i];
        *ppName = (char*)pCreateInfo->ppEnabledExtensionNames[i];
    }

    // If app requests vktrace layer or device simulation layer, don't record that in the trace
    char** ppName = (char**)&localCreateInfo.ppEnabledLayerNames[0];
    for (i = 0; i < pCreateInfo->enabledLayerCount; i++) {
        if (strcmp("VK_LAYER_LUNARG_vktrace", pCreateInfo->ppEnabledLayerNames[i]) == 0) {
            // Decrement the enabled layer count and skip copying the pointer
            localCreateInfo.enabledLayerCount--;
        } else if (strcmp("VK_LAYER_LUNARG_device_simulation", pCreateInfo->ppEnabledLayerNames[i]) == 0) {
            // Decrement the enabled layer count and skip copying the pointer
            localCreateInfo.enabledLayerCount--;
        } else {
            // Copy pointer and increment write pointer for everything else
            *ppName++ = (char*)pCreateInfo->ppEnabledLayerNames[i];
        }
    }

    // If this is the first vkCreateInstance call, we haven't written the file header
    // packets to the trace file yet because we need the instance to first be be created
    // so we can query information needed for the file header.  So write the headers now.
    // We can do this because vkCreateInstance must always be the first Vulkan API call.
    static bool firstCreateInstance = true;
    if (firstCreateInstance) {
        if (!send_vk_trace_file_header(*pInstance)) vktrace_LogError("Failed to write trace file header");
        send_vk_api_version_packet();
        firstCreateInstance = false;
        vktrace_LogAlways("Tracing with v%s", VKTRACE_VERSION);
    }

    // Remove loader extensions
    localCreateInfo.pNext = strip_create_extensions(pCreateInfo->pNext);

    CREATE_TRACE_PACKET(vkCreateInstance,
                        sizeof(VkInstance) + get_struct_chain_size((void*)&localCreateInfo) + sizeof(VkAllocationCallbacks));
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkCreateInstance(pHeader);
    add_VkInstanceCreateInfo_to_packet(pHeader, (VkInstanceCreateInfo**)&(pPacket->pCreateInfo),
                                       (VkInstanceCreateInfo*)&localCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pInstance), sizeof(VkInstance), pInstance);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pInstance));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_Instance_object(*pInstance);
        info.ObjectInfo.Instance.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.Instance.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    if (localCreateInfo.enabledLayerCount > 0) free((void*)localCreateInfo.ppEnabledLayerNames);
    if (localCreateInfo.enabledExtensionCount > 0) free((void*)localCreateInfo.ppEnabledExtensionNames);

    if (strstr(vktrace_get_process_name(), "vkreplay") != NULL) {
        g_is_vkreplay_proc = true;
    }

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkDestroyInstance(VkInstance instance,
                                                                      const VkAllocationCallbacks* pAllocator) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    if (g_trimEnabled && g_trimIsInTrim) {
        trim::stop();
    }

    vktrace_trace_packet_header* pHeader;
    packet_vkDestroyInstance* pPacket = NULL;
    dispatch_key key = get_dispatch_key(instance);
    CREATE_TRACE_PACKET(vkDestroyInstance, sizeof(VkAllocationCallbacks));
    mid(instance)->instTable.DestroyInstance(instance, pAllocator);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkDestroyInstance(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::remove_Instance_object(instance);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    g_instanceDataMap.erase(key);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_MEMCPY_USE_PPL_LIB)
    vktrace_pageguard_done_multi_threads_memcpy();
#endif
#if defined(ANDROID)
    _Unload();
#endif
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateRenderPass(VkDevice device,
                                                                           const VkRenderPassCreateInfo* pCreateInfo,
                                                                           const VkAllocationCallbacks* pAllocator,
                                                                           VkRenderPass* pRenderPass) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateRenderPass* pPacket = NULL;
    // begin custom code (get_struct_chain_size)
    uint32_t attachmentCount = (pCreateInfo != NULL && (pCreateInfo->pAttachments != NULL)) ? pCreateInfo->attachmentCount : 0;
    uint32_t dependencyCount = (pCreateInfo != NULL && (pCreateInfo->pDependencies != NULL)) ? pCreateInfo->dependencyCount : 0;
    uint32_t subpassCount = (pCreateInfo != NULL && (pCreateInfo->pSubpasses != NULL)) ? pCreateInfo->subpassCount : 0;
    CREATE_TRACE_PACKET(vkCreateRenderPass,
                        get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkRenderPass));
    // end custom code
    result = mdd(device)->devTable.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateRenderPass(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkRenderPassCreateInfo), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pAttachments),
                                       attachmentCount * sizeof(VkAttachmentDescription), pCreateInfo->pAttachments);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pDependencies),
                                       dependencyCount * sizeof(VkSubpassDependency), pCreateInfo->pDependencies);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pSubpasses),
                                       subpassCount * sizeof(VkSubpassDescription), pCreateInfo->pSubpasses);
    for (uint32_t i = 0; i < pPacket->pCreateInfo->subpassCount; i++) {
        VkSubpassDescription* pSubpass = (VkSubpassDescription*)&pPacket->pCreateInfo->pSubpasses[i];
        const VkSubpassDescription* pSp = &pCreateInfo->pSubpasses[i];
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pInputAttachments),
                                           pSubpass->inputAttachmentCount * sizeof(VkAttachmentReference), pSp->pInputAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pInputAttachments));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pColorAttachments),
                                           pSubpass->colorAttachmentCount * sizeof(VkAttachmentReference), pSp->pColorAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pColorAttachments));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pResolveAttachments),
                                           pSubpass->colorAttachmentCount * sizeof(VkAttachmentReference),
                                           pSp->pResolveAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pResolveAttachments));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pDepthStencilAttachment), 1 * sizeof(VkAttachmentReference),
                                           pSp->pDepthStencilAttachment);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pDepthStencilAttachment));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pPreserveAttachments),
                                           pSubpass->preserveAttachmentCount * sizeof(VkAttachmentReference),
                                           pSp->pPreserveAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pPreserveAttachments));
    }
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderPass), sizeof(VkRenderPass), pRenderPass);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pAttachments));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pDependencies));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pSubpasses));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderPass));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_RenderPass_object(*pRenderPass);
        trim::add_RenderPassCreateInfo(*pRenderPass, (VkApplicationInfo*)pCreateInfo);
        info.belongsToDevice = device;
        info.ObjectInfo.RenderPass.pCreatePacket = trim::copy_packet(pHeader);
        if (pCreateInfo == nullptr || pCreateInfo->attachmentCount == 0) {
            info.ObjectInfo.RenderPass.attachmentCount = 0;
            info.ObjectInfo.RenderPass.pAttachments = nullptr;
        } else {
            info.ObjectInfo.RenderPass.attachmentCount = pCreateInfo->attachmentCount;
            info.ObjectInfo.RenderPass.pAttachments = new trim::ImageTransition[pCreateInfo->attachmentCount];
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                info.ObjectInfo.RenderPass.pAttachments[i].initialLayout = pCreateInfo->pAttachments[i].initialLayout;
                info.ObjectInfo.RenderPass.pAttachments[i].finalLayout = pCreateInfo->pAttachments[i].finalLayout;

                // We don't know which object it is at this time, but we'll find out in VkBindDescriptorSets().
                info.ObjectInfo.RenderPass.pAttachments[i].image = VK_NULL_HANDLE;
            }
        }
        if (pAllocator != NULL) {
            info.ObjectInfo.RenderPass.pAllocator = pAllocator;
        }

        if (pAllocator != NULL) {
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VkResult post_vkCreateRenderPass2(
    vktrace_trace_packet_header* pHeader,
    VkDevice device,
    const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass,
    VkResult res) {
    uint32_t attachmentCount = (pCreateInfo != NULL && (pCreateInfo->pAttachments != NULL)) ? pCreateInfo->attachmentCount : 0;
    uint32_t dependencyCount = (pCreateInfo != NULL && (pCreateInfo->pDependencies != NULL)) ? pCreateInfo->dependencyCount : 0;
    uint32_t subpassCount = (pCreateInfo != NULL && (pCreateInfo->pSubpasses != NULL)) ? pCreateInfo->subpassCount : 0;
    uint32_t correlatedViewMaskCount =
        (pCreateInfo != NULL && (pCreateInfo->pCorrelatedViewMasks != NULL)) ? pCreateInfo->correlatedViewMaskCount : 0;
    packet_vkCreateRenderPass2* pPacket = (packet_vkCreateRenderPass2*)pHeader->pBody;

    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkRenderPassCreateInfo2), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pAttachments),
                                       attachmentCount * sizeof(VkAttachmentDescription2), pCreateInfo->pAttachments);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pDependencies),
                                       dependencyCount * sizeof(VkSubpassDependency2), pCreateInfo->pDependencies);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pSubpasses),
                                       subpassCount * sizeof(VkSubpassDescription2), pCreateInfo->pSubpasses);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pCorrelatedViewMasks),
                                       correlatedViewMaskCount * sizeof(uint32_t), pCreateInfo->pCorrelatedViewMasks);

    for (uint32_t i = 0; i < pPacket->pCreateInfo->attachmentCount; i++) {
        VkAttachmentDescription2* pAttachment = (VkAttachmentDescription2*)&(pPacket->pCreateInfo->pAttachments[i]);
        if (pAttachment) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pAttachment, &pCreateInfo->pAttachments[i]);
    }
    for (uint32_t i = 0; i < pPacket->pCreateInfo->dependencyCount; i++) {
        VkSubpassDependency2* pDependencies = (VkSubpassDependency2*)&(pPacket->pCreateInfo->pDependencies[i]);
        if (pDependencies) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pDependencies, &pCreateInfo->pDependencies[i]);
    }
    for (uint32_t i = 0; i < pPacket->pCreateInfo->subpassCount; i++) {
        VkSubpassDescription2* pSubpass = (VkSubpassDescription2*)&pPacket->pCreateInfo->pSubpasses[i];
        const VkSubpassDescription2* pSp = &pCreateInfo->pSubpasses[i];
        if (pSubpass) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pSubpass, pSp);
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pInputAttachments),
                                           pSubpass->inputAttachmentCount * sizeof(VkAttachmentReference2), pSp->pInputAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pInputAttachments));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pColorAttachments),
                                           pSubpass->colorAttachmentCount * sizeof(VkAttachmentReference2), pSp->pColorAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pColorAttachments));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pResolveAttachments),
                                           pSubpass->colorAttachmentCount * sizeof(VkAttachmentReference2),
                                           pSp->pResolveAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pResolveAttachments));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pDepthStencilAttachment),
                                           1 * sizeof(VkAttachmentReference2), pSp->pDepthStencilAttachment);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pDepthStencilAttachment));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSubpass->pPreserveAttachments),
                                           pSubpass->preserveAttachmentCount * sizeof(VkAttachmentReference2),
                                           pSp->pPreserveAttachments);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pSubpass->pPreserveAttachments));
    }
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderPass), sizeof(VkRenderPass), pRenderPass);
    pPacket->result = res;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pAttachments));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pDependencies));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pSubpasses));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pCorrelatedViewMasks));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderPass));
    return VK_SUCCESS;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateRenderPass2(VkDevice device,
                                                                            const VkRenderPassCreateInfo2* pCreateInfo,
                                                                            const VkAllocationCallbacks* pAllocator,
                                                                            VkRenderPass* pRenderPass) {
    return __HOOKED_vkCreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateRenderPass2KHR(VkDevice device,
                                                                               const VkRenderPassCreateInfo2* pCreateInfo,
                                                                               const VkAllocationCallbacks* pAllocator,
                                                                               VkRenderPass* pRenderPass) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateRenderPass2KHR* pPacket = NULL;

    // begin custom code (get_struct_chain_size)
    CREATE_TRACE_PACKET(vkCreateRenderPass2KHR,
                        get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkRenderPass));
    // end custom code
    result = mdd(device)->devTable.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateRenderPass2KHR(pHeader);

    post_vkCreateRenderPass2(pPacket->header, device, pCreateInfo, pAllocator, pRenderPass, result);
     if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_RenderPass_object(*pRenderPass);
        trim::add_RenderPassCreateInfo(*pRenderPass, (VkApplicationInfo*)(pCreateInfo));
        info.belongsToDevice = device;
        info.ObjectInfo.RenderPass.pCreatePacket = trim::copy_packet(pHeader);
        if (pCreateInfo == nullptr || pCreateInfo->attachmentCount == 0) {
            info.ObjectInfo.RenderPass.attachmentCount = 0;
            info.ObjectInfo.RenderPass.pAttachments = nullptr;
        } else {
            info.ObjectInfo.RenderPass.attachmentCount = pCreateInfo->attachmentCount;
            info.ObjectInfo.RenderPass.pAttachments = new trim::ImageTransition[pCreateInfo->attachmentCount];
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                info.ObjectInfo.RenderPass.pAttachments[i].initialLayout = pCreateInfo->pAttachments[i].initialLayout;
                info.ObjectInfo.RenderPass.pAttachments[i].finalLayout = pCreateInfo->pAttachments[i].finalLayout;

                // We don't know which object it is at this time, but we'll find out in VkBindDescriptorSets().
                info.ObjectInfo.RenderPass.pAttachments[i].image = VK_NULL_HANDLE;
            }
        }
        if (pAllocator != NULL) {
            info.ObjectInfo.RenderPass.pAllocator = pAllocator;
        }

        if (pAllocator != NULL) {
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                             const char* pLayerName,
                                                                                             uint32_t* pPropertyCount,
                                                                                             VkExtensionProperties* pProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkEnumerateDeviceExtensionProperties* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    // Only call down chain if querying ICD rather than layer device extensions
    if (pLayerName == NULL)
        result =
            mid(physicalDevice)->instTable.EnumerateDeviceExtensionProperties(physicalDevice, NULL, pPropertyCount, pProperties);
    else {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    endTime = vktrace_get_time();
    CREATE_TRACE_PACKET(vkEnumerateDeviceExtensionProperties, ((pLayerName != NULL) ? ROUNDUP_TO_4(strlen(pLayerName) + 1) : 0) +
                                                                  sizeof(uint32_t) +
                                                                  (*pPropertyCount * sizeof(VkExtensionProperties)));
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkEnumerateDeviceExtensionProperties(pHeader);
    pPacket->physicalDevice = physicalDevice;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pLayerName), ((pLayerName != NULL) ? strlen(pLayerName) + 1 : 0),
                                       pLayerName);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPropertyCount), sizeof(uint32_t), pPropertyCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pProperties), *pPropertyCount * sizeof(VkExtensionProperties),
                                       pProperties);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pLayerName));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPropertyCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pProperties));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                                                         uint32_t* pPropertyCount,
                                                                                         VkLayerProperties* pProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkEnumerateDeviceLayerProperties* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    result = mid(physicalDevice)->instTable.EnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
    endTime = vktrace_get_time();
    CREATE_TRACE_PACKET(vkEnumerateDeviceLayerProperties, sizeof(uint32_t) + (*pPropertyCount * sizeof(VkLayerProperties)));
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkEnumerateDeviceLayerProperties(pHeader);
    pPacket->physicalDevice = physicalDevice;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPropertyCount), sizeof(uint32_t), pPropertyCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pProperties), *pPropertyCount * sizeof(VkLayerProperties),
                                       pProperties);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPropertyCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pProperties));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceQueueFamilyProperties* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    mid(physicalDevice)
        ->instTable.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
    endTime = vktrace_get_time();
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceQueueFamilyProperties,
                        sizeof(uint32_t) + *pQueueFamilyPropertyCount * sizeof(VkQueueFamilyProperties));
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkGetPhysicalDeviceQueueFamilyProperties(pHeader);
    pPacket->physicalDevice = physicalDevice;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pQueueFamilyPropertyCount), sizeof(uint32_t),
                                       pQueueFamilyPropertyCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pQueueFamilyProperties),
                                       *pQueueFamilyPropertyCount * sizeof(VkQueueFamilyProperties), pQueueFamilyProperties);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pQueueFamilyPropertyCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pQueueFamilyProperties));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        trim::ObjectInfo* pInfo = trim::get_PhysicalDevice_objectInfo(physicalDevice);
        if (pInfo != NULL) {
            if (pQueueFamilyProperties == nullptr) {
                pInfo->ObjectInfo.PhysicalDevice.pGetPhysicalDeviceQueueFamilyPropertiesCountPacket = trim::copy_packet(pHeader);
                pInfo->ObjectInfo.PhysicalDevice.queueFamilyCount = *pQueueFamilyPropertyCount;
            } else {
                pInfo->ObjectInfo.PhysicalDevice.pGetPhysicalDeviceQueueFamilyPropertiesPacket = trim::copy_packet(pHeader);
            }
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkGetPhysicalDeviceQueueFamilyProperties2KHR(
    VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2KHR* pQueueFamilyProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceQueueFamilyProperties2KHR* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    mid(physicalDevice)
        ->instTable.GetPhysicalDeviceQueueFamilyProperties2KHR(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
    endTime = vktrace_get_time();
    int resultArraySize = 0;
    if (pQueueFamilyProperties != nullptr) {
        for (uint32_t i = 0; i < *pQueueFamilyPropertyCount; ++i) {
            resultArraySize += get_struct_chain_size((void*)&pQueueFamilyProperties[i]);
        }
    }
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceQueueFamilyProperties2KHR, sizeof(uint32_t) + resultArraySize);
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkGetPhysicalDeviceQueueFamilyProperties2KHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pQueueFamilyPropertyCount), sizeof(uint32_t),
                                       pQueueFamilyPropertyCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pQueueFamilyProperties), resultArraySize,
                                       pQueueFamilyProperties);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pQueueFamilyPropertyCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pQueueFamilyProperties));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_PhysicalDevice_objectInfo(physicalDevice);
        if (pInfo != NULL) {
            if (pQueueFamilyProperties == nullptr) {
                pInfo->ObjectInfo.PhysicalDevice.pGetPhysicalDeviceQueueFamilyPropertiesCountPacket = trim::copy_packet(pHeader);
                pInfo->ObjectInfo.PhysicalDevice.queueFamilyCount = *pQueueFamilyPropertyCount;
            } else {
                pInfo->ObjectInfo.PhysicalDevice.pGetPhysicalDeviceQueueFamilyPropertiesPacket = trim::copy_packet(pHeader);
            }
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkEnumeratePhysicalDevices(VkInstance instance,
                                                                                   uint32_t* pPhysicalDeviceCount,
                                                                                   VkPhysicalDevice* pPhysicalDevices) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkEnumeratePhysicalDevices* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    // TODO make sure can handle being called twice with pPD == 0
    SEND_ENTRYPOINT_ID(vkEnumeratePhysicalDevices);
    startTime = vktrace_get_time();
    result = mid(instance)->instTable.EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    endTime = vktrace_get_time();
    CREATE_TRACE_PACKET(
        vkEnumeratePhysicalDevices,
        sizeof(uint32_t) + ((pPhysicalDevices && pPhysicalDeviceCount) ? *pPhysicalDeviceCount * sizeof(VkPhysicalDevice) : 0));
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkEnumeratePhysicalDevices(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPhysicalDeviceCount), sizeof(uint32_t), pPhysicalDeviceCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPhysicalDevices),
                                       *pPhysicalDeviceCount * sizeof(VkPhysicalDevice), pPhysicalDevices);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPhysicalDeviceCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPhysicalDevices));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (result == VK_SUCCESS) {
            trim::ObjectInfo* pInfo = trim::get_Instance_objectInfo(instance);
            if (pInfo != NULL && pPhysicalDeviceCount != NULL && pPhysicalDevices == NULL) {
                pInfo->ObjectInfo.Instance.pEnumeratePhysicalDevicesCountPacket = trim::copy_packet(pHeader);
            }

            if (pPhysicalDevices != NULL && pPhysicalDeviceCount != NULL) {
                if (pInfo != NULL) {
                    pInfo->ObjectInfo.Instance.pEnumeratePhysicalDevicesPacket = trim::copy_packet(pHeader);
                }

                for (uint32_t iter = 0; iter < *pPhysicalDeviceCount; iter++) {
                    trim::ObjectInfo& PDInfo = trim::add_PhysicalDevice_object(pPhysicalDevices[iter]);
                    PDInfo.belongsToInstance = instance;
                    // Get the memory properties of the device
                    mid(instance)->instTable.GetPhysicalDeviceMemoryProperties(
                        pPhysicalDevices[iter], &PDInfo.ObjectInfo.PhysicalDevice.physicalDeviceMemoryProperties);
                }
            }
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool,
                                                                              uint32_t firstQuery, uint32_t queryCount,
                                                                              size_t dataSize, void* pData, VkDeviceSize stride,
                                                                              VkQueryResultFlags flags) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetQueryPoolResults* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    result = mdd(device)->devTable.GetQueryPoolResults(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);
    endTime = vktrace_get_time();
    CREATE_TRACE_PACKET(vkGetQueryPoolResults, dataSize);
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkGetQueryPoolResults(pHeader);
    pPacket->device = device;
    pPacket->queryPool = queryPool;
    pPacket->firstQuery = firstQuery;
    pPacket->queryCount = queryCount;
    pPacket->dataSize = dataSize;
    pPacket->stride = stride;
    pPacket->flags = flags;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pData), dataSize, pData);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pData));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        auto it = g_queryPoolStatus.find(queryPool);
        if (g_trimIsInTrim && it != g_queryPoolStatus.end() && it->second) {
            trim::mark_QueryPool_reference(queryPool);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkAllocateDescriptorSets(VkDevice device,
                                                                                 const VkDescriptorSetAllocateInfo* pAllocateInfo,
                                                                                 VkDescriptorSet* pDescriptorSets) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkAllocateDescriptorSets* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    SEND_ENTRYPOINT_ID(vkAllocateDescriptorSets);
    startTime = vktrace_get_time();
    result = mdd(device)->devTable.AllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
    endTime = vktrace_get_time();
    CREATE_TRACE_PACKET(vkAllocateDescriptorSets, get_struct_chain_size(pAllocateInfo) +
                                                      (pAllocateInfo->descriptorSetCount * sizeof(VkDescriptorSetLayout)) +
                                                      (pAllocateInfo->descriptorSetCount * sizeof(VkDescriptorSet)));
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkAllocateDescriptorSets(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocateInfo), sizeof(VkDescriptorSetAllocateInfo),
                                       pAllocateInfo);
    if (pAllocateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pAllocateInfo, pAllocateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocateInfo->pSetLayouts),
                                       pPacket->pAllocateInfo->descriptorSetCount * sizeof(VkDescriptorSetLayout),
                                       pAllocateInfo->pSetLayouts);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorSets),
                                       pPacket->pAllocateInfo->descriptorSetCount * sizeof(VkDescriptorSet), pDescriptorSets);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocateInfo->pSetLayouts));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorSets));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocateInfo));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pPoolInfo = trim::get_DescriptorPool_objectInfo(pAllocateInfo->descriptorPool);
        if (VK_SUCCESS == result) {
            // If the call fail, DescriptorPool.numSets of trim trackinfo
            // should not be changed because no new descriptorSet be allocated
            // from the pool. Otherwise, for some title it cause  numSets
            // beyond the pool maxSets in VkDescriptorPoolCreateInfo, and make
            // trim generate wrong vkAllocateDescriptorSets for that pool
            // which cause the call failed with VK_ERROR_OUT_OF_POOL_MEMORY
            // during playback the trimmed trace file. So here we only track
            // successful vkAllocateDescriptorSets.

            if (pPoolInfo != NULL) {
                pPoolInfo->ObjectInfo.DescriptorPool.numSets += pAllocateInfo->descriptorSetCount;
            }
            for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
                trim::ObjectInfo& setInfo = trim::add_DescriptorSet_object(pDescriptorSets[i]);
                setInfo.belongsToDevice = device;
                setInfo.ObjectInfo.DescriptorSet.descriptorPool = pAllocateInfo->descriptorPool;
                setInfo.ObjectInfo.DescriptorSet.layout = pAllocateInfo->pSetLayouts[i];

                // need to allocate for a potential write & copy to update the descriptor; one for each binding based on the layout
                trim::ObjectInfo* pLayoutInfo = trim::get_DescriptorSetLayout_objectInfo(pAllocateInfo->pSetLayouts[i]);
                if (pLayoutInfo != NULL) {
                    uint32_t numImages = pLayoutInfo->ObjectInfo.DescriptorSetLayout.numImages;
                    uint32_t numBuffers = pLayoutInfo->ObjectInfo.DescriptorSetLayout.numBuffers;
                    uint32_t numTexelBufferViews = pLayoutInfo->ObjectInfo.DescriptorSetLayout.numTexelBufferViews;
                    uint32_t numAS = pLayoutInfo->ObjectInfo.DescriptorSetLayout.numAS;
                    uint32_t numBindings = numImages + numBuffers + numTexelBufferViews + numAS;

                    setInfo.ObjectInfo.DescriptorSet.numBindings = numBindings;
                    setInfo.ObjectInfo.DescriptorSet.writeDescriptorCount = 0;
                    setInfo.ObjectInfo.DescriptorSet.copyDescriptorCount = 0;
                    setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets = new VkWriteDescriptorSet[numBindings];
                    setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets = new VkCopyDescriptorSet[numBindings];

                    // setup these WriteDescriptorSets to be specific to each binding of the associated layout
                    for (uint32_t b = 0; b < pLayoutInfo->ObjectInfo.DescriptorSetLayout.bindingCount; b++) {
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pNext = NULL;
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].dstArrayElement =
                            0;  // defaulting to 0, no way to know for sure at this time
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].dstSet = pDescriptorSets[i];
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].dstBinding =
                            pLayoutInfo->ObjectInfo.DescriptorSetLayout.pBindings[b].binding;
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].descriptorCount =
                            pLayoutInfo->ObjectInfo.DescriptorSetLayout.pBindings[b].descriptorCount;
                        setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].descriptorType =
                            pLayoutInfo->ObjectInfo.DescriptorSetLayout.pBindings[b].descriptorType;

                        switch (setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].descriptorType) {
                            case VK_DESCRIPTOR_TYPE_SAMPLER:
                            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pImageInfo =
                                    new VkDescriptorImageInfo[setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b]
                                                                  .descriptorCount];
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pBufferInfo = NULL;
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pTexelBufferView = NULL;
                            } break;
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pImageInfo = NULL;
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pBufferInfo =
                                    new VkDescriptorBufferInfo[setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b]
                                                                   .descriptorCount];
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pTexelBufferView = NULL;
                            } break;
                            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pImageInfo = NULL;
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pBufferInfo = NULL;
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pTexelBufferView =
                                    new VkBufferView[setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].descriptorCount];
                            } break;
                            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pImageInfo = NULL;
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pBufferInfo = NULL;
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pTexelBufferView = NULL;
                                int size = sizeof(VkWriteDescriptorSetAccelerationStructureKHR) + setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].descriptorCount * sizeof(VkAccelerationStructureKHR);
                                VkWriteDescriptorSetAccelerationStructureKHR *pWriteDSAS = (VkWriteDescriptorSetAccelerationStructureKHR*)malloc(size);
                                memset(pWriteDSAS, 0 ,size);
                                pWriteDSAS->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                                pWriteDSAS->pNext = nullptr;
                                pWriteDSAS->accelerationStructureCount = setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].descriptorCount;
                                pWriteDSAS->pAccelerationStructures = (VkAccelerationStructureKHR*)((char*)pWriteDSAS + sizeof(VkWriteDescriptorSetAccelerationStructureKHR));
                                setInfo.ObjectInfo.DescriptorSet.pWriteDescriptorSets[b].pNext = (void*)pWriteDSAS;
                            } break;
                            default:
                                break;
                        }
                    }

                    // setup the CopyDescriptorSets similar to above
                    for (uint32_t b = 0; b < pLayoutInfo->ObjectInfo.DescriptorSetLayout.bindingCount; b++) {
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].pNext = NULL;
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].descriptorCount =
                            pLayoutInfo->ObjectInfo.DescriptorSetLayout.pBindings[b].descriptorCount;
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].dstArrayElement =
                            0;  // defaulting to 0, no way to know for sure at this time
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].dstSet = pDescriptorSets[i];
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].dstBinding =
                            pLayoutInfo->ObjectInfo.DescriptorSetLayout.pBindings[b].binding;
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].srcArrayElement =
                            0;  // defaulting to 0, no way to know for sure at this time
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].srcSet =
                            0;  // defaulting to 0, no way to know for sure at this time
                        setInfo.ObjectInfo.DescriptorSet.pCopyDescriptorSets[b].srcBinding =
                            0;  // defaulting to 0, no way to know for sure at this time
                    }
                }
            }
        }

        if (g_trimIsInTrim) {
            trim::mark_DescriptorPool_reference(pAllocateInfo->descriptorPool);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

// When define DescriptorSet Layout, the binding number is also defined. by Doc,
// the descriptor bindings can be specified sparsely so that not all binding
// numbers between 0 and the maximum binding number. the function is used to
// convert the binding number to binding index starting from 0.
uint32_t get_binding_index(VkDescriptorSet dstSet, uint32_t binding) {
    uint32_t binding_index = INVALID_BINDING_INDEX;
    trim::ObjectInfo* pInfo = trim::get_DescriptorSet_objectInfo(dstSet);
    for (uint32_t i = 0; i < pInfo->ObjectInfo.DescriptorSet.numBindings; i++) {
        if (binding == pInfo->ObjectInfo.DescriptorSet.pWriteDescriptorSets[i].dstBinding) {
            binding_index = i;
            break;
        }
    }
    if (binding_index == INVALID_BINDING_INDEX) {
        vktrace_LogWarning(
            "The binding is invalid when the app tries to update the bindings of the DescriptorSet using "
            "vkUpdateDescriptorSets.");
        assert(false);
    }
    return binding_index;
}

// The method is supposed to be used in __HOOKED_vkUpdateDescriptorSets
// to handle a VkCopyDescriptorSet struct, the struct is one element of
// VkCopyDescriptorSet array which is specified as an input parameter in
// vkUpdateDescriptorSets call.
//
// Compare with VkWriteDescriptorSet handling which is also an input parameter
// of vkUpdateDescriptorSets, the handling here is quite different. for
// VkWriteDescriptorSet, the caller specify a descriptor array, use it to
// update one or more bindings of a descriptor set. In trim process,
// coresponding to every binding, we use an descriptor info array which is
// the same length with number of descriptors in that binding, trim track
// these input VkWriteDescriptorSet and update to correspond trim descriptor
// info array. we can think this as we track every command that change the
// binding state through VkWriteDescriptorSet and combine these commands
// into one and record it.
//
// But for VkCopyDescriptorSet, that's different. In a VkCopyDescriptorSet,
// the caller specify the copy is from a location in one src bindings
// of a source descriptorset to another location in the dst bindings of a dst
// descriptorset, the copy should continue until it reach specified count.
// so it only specify the "location" not "value" of related
// descriptor. but the tracking time and the time of starting to trim is
// (always) different, so it's possible for same binding and same location,
// the descriptor there is changed to other value or already invalid when
// starting to trim.
//
// So we need to track/store the "value" at same time. In the following
// function, we directly copy corresponding trim descriptor array of
// binding according to the VkCopyDescriptorSet struct. for example,
// if the struct specify the copy from one location in binding A of
// descriptorset B to another location in binding C of descriptorset D,
// we do not record the location, we directly copy corresponding trim
// descriptor info from source to target at once, and when starting to
// trim, we generate vkUpdateDescriptorSets which only based on
// VkWriteDescriptorSet.
//
// const VkCopyDescriptorSet* pDescriptorCopies, point to a VkCopyDescriptorSet
// struct.
void handleCopyDescriptorSet(const VkCopyDescriptorSet* pDescriptorCopies) {
    trim::ObjectInfo* pDstInfo = trim::get_DescriptorSet_objectInfo(pDescriptorCopies->dstSet);
    trim::ObjectInfo* pSrcInfo = trim::get_DescriptorSet_objectInfo(pDescriptorCopies->srcSet);
    if ((pDstInfo == nullptr) || (pSrcInfo == nullptr)) {
        assert(false);
        return;
    }

    // In pDescriptorCopies, the caller specified the initial src and dst
    // bindings, we need to convert to binding index.

    uint32_t current_dst_binding_index = get_binding_index(pDescriptorCopies->dstSet, pDescriptorCopies->dstBinding);
    uint32_t current_src_binding_index = get_binding_index(pDescriptorCopies->srcSet, pDescriptorCopies->srcBinding);

    uint32_t j = 0;
    for (trim::DescriptorIterator descriptor_iterator_dst(pDstInfo, current_dst_binding_index, pDescriptorCopies->dstArrayElement,
                                                          pDescriptorCopies->descriptorCount),
         descriptor_iterator_src(pSrcInfo, current_src_binding_index, pDescriptorCopies->srcArrayElement,
                                 pDescriptorCopies->descriptorCount);
         (false == descriptor_iterator_dst.IsEnd()) && (false == descriptor_iterator_src.IsEnd());
         ++descriptor_iterator_dst, ++descriptor_iterator_src, ++j) {
        switch (descriptor_iterator_dst.GetCurrentDescriptorType()) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator_dst)), reinterpret_cast<void*>(&(*descriptor_iterator_src)),
                       sizeof(VkDescriptorImageInfo));
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator_dst)), reinterpret_cast<void*>(&(*descriptor_iterator_src)),
                       sizeof(VkDescriptorBufferInfo));
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator_dst)), reinterpret_cast<void*>(&(*descriptor_iterator_src)),
                       sizeof(VkBufferView));
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator_dst)), reinterpret_cast<void*>(&(*descriptor_iterator_src)),
                       sizeof(VkAccelerationStructureKHR));
                break;
            default:
                assert(0);
                break;
        }
    }
}

void handleWriteDescriptorSet(const VkWriteDescriptorSet* pDescriptorUpdateEntry) {
    // The following variables are used to locate the descriptor
    // track info in the target descriptor set, so we can use the
    // input descriptor data to update it.
    // For every input descriptor data, we need to find
    // corresponding binding number and array element index.
    VkWriteDescriptorSet* pWriteDescriptorSet;  // This is the pointer to trim tracking info of binding in target descriptorset.
    uint32_t bindingDescriptorInfoArrayWriteIndex = 0;   // This is the array index of current descriptor
                                                         // (within the binding array) that we'll update.
    uint32_t bindingDescriptorInfoArrayWriteLength = 0;  // The descriptor amount in the binding array.

    trim::ObjectInfo* pInfo = trim::get_DescriptorSet_objectInfo(pDescriptorUpdateEntry->dstSet);

    // Get the binding index from the binding number which is the
    // updating target. this is the array index of the current
    // binding in the descriptorset track info.
    //
    // Note: by Doc, Vulkan allows the descriptor bindings to be
    // specified sparsely so we cannot assume the binding index
    // is the binding number.
    uint32_t bindingIndex = get_binding_index(pDescriptorUpdateEntry->dstSet, pDescriptorUpdateEntry->dstBinding);
    assert(bindingIndex != INVALID_BINDING_INDEX);

    pWriteDescriptorSet = &pInfo->ObjectInfo.DescriptorSet.pWriteDescriptorSets[bindingIndex];
    bindingDescriptorInfoArrayWriteIndex = pDescriptorUpdateEntry->dstArrayElement;
    bindingDescriptorInfoArrayWriteLength = pWriteDescriptorSet->descriptorCount;
    if (!(bindingDescriptorInfoArrayWriteIndex < bindingDescriptorInfoArrayWriteLength))
        assert(bindingDescriptorInfoArrayWriteIndex < bindingDescriptorInfoArrayWriteLength);

    uint32_t j = 0;
    for (trim::DescriptorIterator descriptor_iterator(pInfo, bindingIndex, bindingDescriptorInfoArrayWriteIndex,
                                                      pDescriptorUpdateEntry->descriptorCount);
         !descriptor_iterator.IsEnd(); ++descriptor_iterator, ++j) {
        // The following code update the descriptorset track info
        // with the descriptor data.

        if (descriptor_iterator.GetCurrentBindingIndex() >= pInfo->ObjectInfo.DescriptorSet.writeDescriptorCount) {
            // this is to track the latest data in this call and also cover previous calls.
            // writeDescriptorCount is used to indicate so far how many bindings of this
            // descriptorset has been updated, this include this call and all previous
            // calls, from all these calls, we record the max bindingindex. its value must
            // be <= numBindings.
            pInfo->ObjectInfo.DescriptorSet.writeDescriptorCount = descriptor_iterator.GetCurrentBindingIndex() + 1;
        }

        switch (pDescriptorUpdateEntry->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator)),
                       reinterpret_cast<void*>(const_cast<VkDescriptorImageInfo*>(pDescriptorUpdateEntry->pImageInfo + j)),
                       sizeof(VkDescriptorImageInfo));
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator)),
                       reinterpret_cast<void*>(const_cast<VkDescriptorBufferInfo*>(pDescriptorUpdateEntry->pBufferInfo + j)),
                       sizeof(VkDescriptorBufferInfo));
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                memcpy(reinterpret_cast<void*>(&(*descriptor_iterator)),
                       reinterpret_cast<void*>(const_cast<VkBufferView*>(pDescriptorUpdateEntry->pTexelBufferView + j)),
                       sizeof(VkBufferView));
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                VkWriteDescriptorSetAccelerationStructureKHR* pdst = ( VkWriteDescriptorSetAccelerationStructureKHR*)&(*descriptor_iterator);
                VkWriteDescriptorSetAccelerationStructureKHR* psrc = (VkWriteDescriptorSetAccelerationStructureKHR*)(pDescriptorUpdateEntry->pNext);
                memcpy((void*)pdst->pAccelerationStructures, (void*)psrc->pAccelerationStructures, sizeof(VkAccelerationStructureKHR) * psrc->accelerationStructureCount);
            }
                break;
            default:
                assert(0);
                break;
        }
    }
}

// Manually written because it needs to use get_struct_chain_size and allocate some extra pointers (why?)
// Also since it needs to app the array of pointers and sub-buffers (see comments in function)
VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                                                                           const VkWriteDescriptorSet* pDescriptorWrites,
                                                                           uint32_t descriptorCopyCount,
                                                                           const VkCopyDescriptorSet* pDescriptorCopies) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkUpdateDescriptorSets* pPacket = NULL;
    // begin custom code
    size_t arrayByteCount = 0;

    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        arrayByteCount += get_struct_chain_size(&pDescriptorWrites[i]);
    }

    for (uint32_t i = 0; i < descriptorCopyCount; i++) {
        arrayByteCount += get_struct_chain_size(&pDescriptorCopies[i]);
    }

    CREATE_TRACE_PACKET(vkUpdateDescriptorSets, arrayByteCount);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            if (pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                g_descsetToMemory[pDescriptorWrites[i].dstSet] = g_bufferToDeviceMemory[pDescriptorWrites[i].pBufferInfo->buffer];
            } else if (pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                auto it = g_imageViewToImage.find(pDescriptorWrites[i].pImageInfo->imageView);
                if (it != g_imageViewToImage.end()) {
                    g_descsetToMemory[pDescriptorWrites[i].dstSet] = g_imageToDeviceMemory[it->second];
                }
            }
        }
    }
#endif
    // end custom code

    mdd(device)->devTable.UpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount,
                                               pDescriptorCopies);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkUpdateDescriptorSets(pHeader);
    pPacket->device = device;
    pPacket->descriptorWriteCount = descriptorWriteCount;
    // begin custom code
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites),
                                       descriptorWriteCount * sizeof(VkWriteDescriptorSet), pDescriptorWrites);
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        switch (pPacket->pDescriptorWrites[i].descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pImageInfo),
                                                   pDescriptorWrites[i].descriptorCount * sizeof(VkDescriptorImageInfo),
                                                   pDescriptorWrites[i].pImageInfo);
                vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pImageInfo));
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pTexelBufferView),
                                                   pDescriptorWrites[i].descriptorCount * sizeof(VkBufferView),
                                                   pDescriptorWrites[i].pTexelBufferView);
                vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pTexelBufferView));
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pBufferInfo),
                                                   pDescriptorWrites[i].descriptorCount * sizeof(VkDescriptorBufferInfo),
                                                   pDescriptorWrites[i].pBufferInfo);
                vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pBufferInfo));
            } break;
            default:
                break;
        }
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pDescriptorWrites + i), pDescriptorWrites + i);
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites));

    pPacket->descriptorCopyCount = descriptorCopyCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorCopies),
                                       descriptorCopyCount * sizeof(VkCopyDescriptorSet), pDescriptorCopies);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorCopies));
    // end custom code

    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            trim::ObjectInfo* pInfo = trim::get_DescriptorSet_objectInfo(pDescriptorWrites[i].dstSet);
            if (pInfo != NULL) {
                if (g_trimIsInTrim) {
                    trim::mark_DescriptorSet_reference(pDescriptorWrites[i].dstSet);
                    for (uint32_t j = 0; j < pDescriptorWrites[i].descriptorCount; j++) {
                        switch (pDescriptorWrites[i].descriptorType) {
                            case VK_DESCRIPTOR_TYPE_SAMPLER:
                            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                                trim::mark_ImageView_reference(pDescriptorWrites[i].pImageInfo[j].imageView);
                            } break;
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                                trim::mark_Buffer_reference(pDescriptorWrites[i].pBufferInfo[j].buffer);
                            } break;
                            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                                trim::mark_BufferView_reference(pDescriptorWrites[i].pTexelBufferView[j]);
                            } break;
                            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                                assert(pDescriptorWrites[i].pNext != nullptr);
                                VkWriteDescriptorSetAccelerationStructureKHR *pWriteDSAS = (VkWriteDescriptorSetAccelerationStructureKHR*)(pDescriptorWrites[i].pNext);
                                for(int asi = 0; asi < pWriteDSAS->accelerationStructureCount; asi++) {
                                    trim::mark_AccelerationStructure_reference(pWriteDSAS->pAccelerationStructures[asi]);
                                }
                            } break;
                            default:
                                break;
                        }
                    }
                }
            }
            handleWriteDescriptorSet(&pDescriptorWrites[i]);
        }

        for (uint32_t i = 0; i < descriptorCopyCount; i++) {
            trim::ObjectInfo* pInfo = trim::get_DescriptorSet_objectInfo(pDescriptorCopies[i].dstSet);
            if (pInfo != NULL) {
                if (g_trimIsInTrim) {
                    trim::mark_DescriptorSet_reference(pDescriptorCopies[i].srcSet);
                    trim::mark_DescriptorSet_reference(pDescriptorCopies[i].dstSet);
                }
                // handle all elements of the pDescriptorCopies array one by one, note: in
                // handleCopyDescriptorSet function, we do not record these CopyDescriptorSet
                // directly, but update WriteDescriptorSet of related bindings instead.
                handleCopyDescriptorSet(&pDescriptorCopies[i]);
            }
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetFenceStatus(
    VkDevice device,
    VkFence fence) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetFenceStatus* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetFenceStatus, 0);
    result = mdd(device)->devTable.GetFenceStatus(device, fence);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    if (g_submittedFenceOnFrame.find(fence) != g_submittedFenceOnFrame.end() &&
        getDelaySignalFenceFrames() > 0 &&
        result == VK_SUCCESS) {
        if (getDelaySignalFenceFrames() > (g_trimFrameCounter - g_submittedFenceOnFrame[fence]) ) {
            result = VK_NOT_READY;
        }
    }
    pPacket = interpret_body_as_vkGetFenceStatus(pHeader);
    pPacket->device = device;
    pPacket->fence = fence;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                                      const VkSubmitInfo* pSubmits, VkFence fence) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    if ((g_trimEnabled) && (pSubmits != NULL)) {
        vktrace_enter_critical_section(&trim::trimTransitionMapLock);
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    flushAllChangedMappedMemory(&vkFlushMappedMemoryRangesWithoutAPICall);
    if (!UseMappedExternalHostMemoryExtension()) {
        // If enable external host memory extension, there will be no shadow
        // memory, so we don't need any read pageguard handling.
        resetAllReadFlagAndPageGuard();
    }
#endif
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkQueueSubmit* pPacket = NULL;
    size_t arrayByteCount = 0;
    for (uint32_t i = 0; i < submitCount; ++i) {
        arrayByteCount += vk_size_vksubmitinfo(&pSubmits[i]);
        arrayByteCount += get_struct_chain_size(&pSubmits[i]);
    }
    CREATE_TRACE_PACKET(vkQueueSubmit, arrayByteCount);
    result = mdd(queue)->devTable.QueueSubmit(queue, submitCount, pSubmits, fence);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension() && getPMBSyncGPUDataBackEnableFlag()) {
        std::list<const std::list<VkBuffer>*> dstBuffers;
        std::list<const std::list<VkImage>*> dstImages;
        std::list<DeviceMemory> descritporsetMemory = {};
        for (uint32_t i = 0; i < submitCount; ++i) {
            for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) {
                if (g_commandBufferToCommandBuffers.find(pSubmits[i].pCommandBuffers[j]) != g_commandBufferToCommandBuffers.end()) {
                    std::list<VkCommandBuffer>& secondaryCmdBufs = g_commandBufferToCommandBuffers[pSubmits[i].pCommandBuffers[j]];
                    for (auto it = secondaryCmdBufs.begin(); it != secondaryCmdBufs.end(); it++) {
                        if (g_cmdBufferToBuffers.find(*it) != g_cmdBufferToBuffers.end()) {
                            dstBuffers.push_front(&g_cmdBufferToBuffers[*it]);
                        }
                        if (g_cmdBufferToImages.find(*it) != g_cmdBufferToImages.end()) {
                            dstImages.push_front(&g_cmdBufferToImages[*it]);
                        }
                        auto ittemp = g_cmdbufferToDescriptorSets.find(*it);
                        if (ittemp != g_cmdbufferToDescriptorSets.end()) {
                            for (auto dsit = ittemp->second.begin(); dsit != ittemp->second.end(); dsit++) {
                                auto memit = g_descsetToMemory.find(*dsit);
                                if (memit != g_descsetToMemory.end()) {
                                    descritporsetMemory.push_front(memit->second);
                                }
                            }
                        }

                    }
                } else {
                    if (g_cmdBufferToBuffers.find(pSubmits[i].pCommandBuffers[j]) != g_cmdBufferToBuffers.end()) {
                        dstBuffers.push_front(&g_cmdBufferToBuffers[pSubmits[i].pCommandBuffers[j]]);
                    }
                    if (g_cmdBufferToImages.find(pSubmits[i].pCommandBuffers[j]) != g_cmdBufferToImages.end()) {
                        dstImages.push_front(&g_cmdBufferToImages[pSubmits[i].pCommandBuffers[j]]);
                    }
                    auto it = g_cmdbufferToDescriptorSets.find(pSubmits[i].pCommandBuffers[j]);
                    if (it != g_cmdbufferToDescriptorSets.end()) {
                        for (auto dsit = it->second.begin(); dsit != it->second.end(); dsit++) {
                            auto memit = g_descsetToMemory.find(*dsit);
                            if (memit != g_descsetToMemory.end()) {
                                descritporsetMemory.push_front(memit->second);
                            }
                        }
                    }
                }
            }
        }
        if (!dstBuffers.empty() || !dstImages.empty() || !descritporsetMemory.empty()) {
            std::unordered_set<VkDeviceMemory> copiedDevMem;
            vktrace_LogWarning("Copy image/buffer to dest buffer occurs !");
            result = mdd(queue)->devTable.QueueWaitIdle(queue);
            for (auto iterBufferList = dstBuffers.begin();
                    iterBufferList != dstBuffers.end(); ++iterBufferList) {
                for (auto iterBuffer = (*iterBufferList)->begin();
                        iterBuffer != (*iterBufferList)->end(); iterBuffer++) {
                    VkBuffer buffer = *iterBuffer;
                    if (g_bufferToDeviceMemory.find(buffer) != g_bufferToDeviceMemory.end()
                        && copiedDevMem.find(g_bufferToDeviceMemory[buffer].memory) == copiedDevMem.end()) {
                        // Sync real mapped memory (recorded in __HOOKED_vkBindBufferMemory) for the dest buffer
                        // (recorded in __HOOKED_vkCmdCopyImageToBuffer) back to the copy of that memory
                        VkDevice device = g_bufferToDeviceMemory[buffer].device;
                        VkDeviceMemory memory = g_bufferToDeviceMemory[buffer].memory;
                        getPageGuardControlInstance().SyncRealMappedMemoryToMemoryCopyHandle(device, memory);
                        copiedDevMem.insert(memory);
                    }
                }
            }
            for (auto iterImageList = dstImages.begin(); iterImageList != dstImages.end(); ++iterImageList) {
                for (auto iterImage = (*iterImageList)->begin(); iterImage != (*iterImageList)->end(); iterImage++) {
                    VkImage image = *iterImage;
                    if (g_imageToDeviceMemory.find(image) != g_imageToDeviceMemory.end()
                        && copiedDevMem.find(g_imageToDeviceMemory[image].memory) == copiedDevMem.end()) {
                        // Sync real mapped memory (recorded in __HOOKED_vkBindBufferMemory) for the dest buffer
                        // (recorded in __HOOKED_vkCmdCopyImageToBuffer) back to the copy of that memory
                        VkDevice device = g_imageToDeviceMemory[image].device;
                        VkDeviceMemory memory = g_imageToDeviceMemory[image].memory;
                        getPageGuardControlInstance().SyncRealMappedMemoryToMemoryCopyHandle(device, memory);
                        copiedDevMem.insert(memory);
                    }
                }
            }
            for (auto iterDsmemory = descritporsetMemory.begin(); iterDsmemory != descritporsetMemory.end(); ++iterDsmemory) {
                if (copiedDevMem.find(iterDsmemory->memory) == copiedDevMem.end()) {
                    // Sync real mapped memory (recorded in __HOOKED_vkBindBufferMemory) for the dest buffer
                    // (recorded in __HOOKED_vkCmdCopyImageToBuffer) back to the copy of that memory
                    VkDevice device = iterDsmemory->device;
                    VkDeviceMemory memory = iterDsmemory->memory;
                    getPageGuardControlInstance().SyncRealMappedMemoryToMemoryCopyHandle(device, memory);
                    copiedDevMem.insert(iterDsmemory->memory);
                }
            }
        }
    }
#endif
    pPacket = interpret_body_as_vkQueueSubmit(pHeader);
    pPacket->queue = queue;
    pPacket->submitCount = submitCount;
    pPacket->fence = fence;
    pPacket->result = result;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSubmits), submitCount * sizeof(VkSubmitInfo), pSubmits);
    for (uint32_t i = 0; i < submitCount; ++i) {
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pSubmits + i), pSubmits + i);
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSubmits[i].pCommandBuffers),
                                           pPacket->pSubmits[i].commandBufferCount * sizeof(VkCommandBuffer),
                                           pSubmits[i].pCommandBuffers);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSubmits[i].pCommandBuffers));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSubmits[i].pWaitSemaphores),
                                           pPacket->pSubmits[i].waitSemaphoreCount * sizeof(VkSemaphore),
                                           pSubmits[i].pWaitSemaphores);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSubmits[i].pWaitSemaphores));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSubmits[i].pSignalSemaphores),
                                           pPacket->pSubmits[i].signalSemaphoreCount * sizeof(VkSemaphore),
                                           pSubmits[i].pSignalSemaphores);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSubmits[i].pSignalSemaphores));
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSubmits[i].pWaitDstStageMask),
                                           pPacket->pSubmits[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags),
                                           pSubmits[i].pWaitDstStageMask);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSubmits[i].pWaitDstStageMask));
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSubmits));
    if (fence != VK_NULL_HANDLE && getDelaySignalFenceFrames() > 0) {
        g_submittedFenceOnFrame[fence] = g_trimFrameCounter;
    }
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        // when we're not trimming, we just need to track the queue
        vktrace_finalize_trace_packet(pHeader);

        if (result == VK_SUCCESS) {
            trim::ObjectInfo* pFenceInfo = trim::get_Fence_objectInfo(fence);
            if (pFenceInfo != NULL) {
                pFenceInfo->ObjectInfo.Fence.signaled = true;
            }

            if (pSubmits != NULL) {
                for (uint32_t i = 0; i < submitCount; i++) {
                    // Update attachment objects based on RenderPass transitions
                    for (uint32_t c = 0; c < pSubmits[i].commandBufferCount; c++) {
                        trim::ObjectInfo* pCBInfo = trim::get_CommandBuffer_objectInfo(pSubmits[i].pCommandBuffers[c]);
                        if (pCBInfo != nullptr) {
                            pCBInfo->ObjectInfo.CommandBuffer.submitQueue = queue;
                        }

                        // apply image transitions
                        std::list<trim::ImageTransition> imageTransitions =
                            trim::GetImageTransitions(pSubmits[i].pCommandBuffers[c]);
                        for (std::list<trim::ImageTransition>::iterator transition = imageTransitions.begin();
                             transition != imageTransitions.end(); transition++) {
                            trim::ObjectInfo* pImage = trim::get_Image_objectInfo(transition->image);
                            if (pImage != nullptr) {
                                pImage->ObjectInfo.Image.mostRecentLayout = transition->finalLayout;
                                pImage->ObjectInfo.Image.accessFlags = transition->dstAccessMask;
                            }
                        }

                        // apply buffer transitions
                        std::list<trim::BufferTransition> bufferTransitions =
                            trim::GetBufferTransitions(pSubmits[i].pCommandBuffers[c]);
                        for (std::list<trim::BufferTransition>::iterator transition = bufferTransitions.begin();
                             transition != bufferTransitions.end(); transition++) {
                            trim::ObjectInfo* pBuffer = trim::get_Buffer_objectInfo(transition->buffer);
                            if (pBuffer != nullptr) {
                                pBuffer->ObjectInfo.Buffer.accessFlags = transition->dstAccessMask;
                            }
                        }
                    }

                    if (pSubmits[i].pWaitSemaphores != NULL) {
                        for (uint32_t w = 0; w < pSubmits[i].waitSemaphoreCount; w++) {
                            trim::ObjectInfo* pInfo = trim::get_Semaphore_objectInfo(pSubmits[i].pWaitSemaphores[w]);
                            if (pInfo != NULL) {
                                pInfo->ObjectInfo.Semaphore.signaledOnQueue = VK_NULL_HANDLE;
                                pInfo->ObjectInfo.Semaphore.signaledOnSwapChain = VK_NULL_HANDLE;
                            }
                        }
                    }

                    if (pSubmits[i].pSignalSemaphores != NULL) {
                        for (uint32_t s = 0; s < pSubmits[i].signalSemaphoreCount; s++) {
                            trim::ObjectInfo* pInfo = trim::get_Semaphore_objectInfo(pSubmits[i].pSignalSemaphores[s]);
                            if (pInfo != NULL) {
                                pInfo->ObjectInfo.Semaphore.signaledOnQueue = queue;
                                pInfo->ObjectInfo.Semaphore.signaledOnSwapChain = VK_NULL_HANDLE;
                            }
                        }
                    }
                }
            }
        }

        if (g_trimIsInTrim) {
            if (pSubmits != NULL && pSubmits->pCommandBuffers != NULL) {
                for (uint32_t s = 0; s < submitCount; s++) {
                    for (uint32_t i = 0; i < pSubmits[s].commandBufferCount; i++) {
                        extern std::unordered_map<VkCommandBuffer, trim::ObjectInfo*> cb_delete_packet;
                        auto it = cb_delete_packet.find(pSubmits[s].pCommandBuffers[i]);
                        if (it != cb_delete_packet.end()) {
                            assert(trim::get_CommandBuffer_objectInfo(pSubmits[s].pCommandBuffers[i])->vkObject != (uint64_t)it->first);
                        }
                        trim::mark_CommandBuffer_reference(pSubmits[s].pCommandBuffers[i]);
                        auto queryCmd = g_queryCmdStatus.find(pSubmits[s].pCommandBuffers[i]);
                        if (queryCmd != g_queryCmdStatus.end()) {
                            for (auto it = queryCmd->second.begin(); it != queryCmd->second.end(); it++) {
                                if (g_queryPoolStatus.find(it->first) != g_queryPoolStatus.end()) {
                                    g_queryPoolStatus[it->first] = (it->second == QueryCmd_Begin) ? true : false;
                                }
                            }
                        }
                    }
                }
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    if ((g_trimEnabled) && (pSubmits != NULL)) {
        vktrace_leave_critical_section(&trim::trimTransitionMapLock);
    }

#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkQueueBindSparse(VkQueue queue, uint32_t bindInfoCount,
                                                                          const VkBindSparseInfo* pBindInfo, VkFence fence) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkQueueBindSparse* pPacket = NULL;
    size_t arrayByteCount = 0;
    uint32_t i = 0;

    for (i = 0; i < bindInfoCount; ++i) {
        arrayByteCount += vk_size_vkbindsparseinfo(&pBindInfo[i]);
        arrayByteCount += get_struct_chain_size(&pBindInfo[i]);
    }

    CREATE_TRACE_PACKET(vkQueueBindSparse, arrayByteCount + 2 * sizeof(VkDeviceMemory));
    result = mdd(queue)->devTable.QueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkQueueBindSparse(pHeader);
    pPacket->queue = queue;
    pPacket->bindInfoCount = bindInfoCount;
    pPacket->fence = fence;
    pPacket->result = result;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfo), bindInfoCount * sizeof(VkBindSparseInfo), pBindInfo);

    for (i = 0; i < bindInfoCount; ++i) {
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfo[i].pBufferBinds),
                                           pPacket->pBindInfo[i].bufferBindCount * sizeof(VkSparseBufferMemoryBindInfo),
                                           pBindInfo[i].pBufferBinds);
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pBindInfo + i), pBindInfo + i);
        for (uint32_t j = 0; j < pPacket->pBindInfo[i].bufferBindCount; j++) {
            VkSparseBufferMemoryBindInfo* pSparseBufferMemoryBindInfo =
                (VkSparseBufferMemoryBindInfo*)&pPacket->pBindInfo[i].pBufferBinds[j];
            const VkSparseBufferMemoryBindInfo* pSparseBufMemBndInf = &pBindInfo[i].pBufferBinds[j];
            vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSparseBufferMemoryBindInfo->pBinds),
                                               pSparseBufferMemoryBindInfo->bindCount * sizeof(VkSparseMemoryBind),
                                               pSparseBufMemBndInf->pBinds);
            vktrace_finalize_buffer_address(pHeader, (void**)&(pSparseBufferMemoryBindInfo->pBinds));
        }
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfo[i].pBufferBinds));

        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfo[i].pImageBinds),
                                           pPacket->pBindInfo[i].imageBindCount * sizeof(VkSparseImageMemoryBindInfo),
                                           pBindInfo[i].pImageBinds);
        for (uint32_t j = 0; j < pPacket->pBindInfo[i].imageBindCount; j++) {
            VkSparseImageMemoryBindInfo* pSparseImageMemoryBindInfo =
                (VkSparseImageMemoryBindInfo*)&pPacket->pBindInfo[i].pImageBinds[j];
            const VkSparseImageMemoryBindInfo* pSparseImgMemBndInf = &pBindInfo[i].pImageBinds[j];
            vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSparseImageMemoryBindInfo->pBinds),
                                               pSparseImageMemoryBindInfo->bindCount * sizeof(VkSparseImageMemoryBind),
                                               pSparseImgMemBndInf->pBinds);
            vktrace_finalize_buffer_address(pHeader, (void**)&(pSparseImageMemoryBindInfo->pBinds));
        }
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfo[i].pImageBinds));

        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfo[i].pImageOpaqueBinds),
                                           pPacket->pBindInfo[i].imageOpaqueBindCount * sizeof(VkSparseImageOpaqueMemoryBindInfo),
                                           pBindInfo[i].pImageOpaqueBinds);
        for (uint32_t j = 0; j < pPacket->pBindInfo[i].imageOpaqueBindCount; j++) {
            VkSparseImageOpaqueMemoryBindInfo* pSparseImageOpaqueMemoryBindInfo =
                (VkSparseImageOpaqueMemoryBindInfo*)&pPacket->pBindInfo[i].pImageOpaqueBinds[j];
            const VkSparseImageOpaqueMemoryBindInfo* pSparseImgOpqMemBndInf = &pBindInfo[i].pImageOpaqueBinds[j];
            vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pSparseImageOpaqueMemoryBindInfo->pBinds),
                                               pSparseImageOpaqueMemoryBindInfo->bindCount * sizeof(VkSparseMemoryBind),
                                               pSparseImgOpqMemBndInf->pBinds);
            vktrace_finalize_buffer_address(pHeader, (void**)&(pSparseImageOpaqueMemoryBindInfo->pBinds));
        }
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfo[i].pImageOpaqueBinds));

        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfo[i].pWaitSemaphores),
                                           pPacket->pBindInfo[i].waitSemaphoreCount * sizeof(VkSemaphore),
                                           pBindInfo[i].pWaitSemaphores);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfo[i].pWaitSemaphores));

        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfo[i].pSignalSemaphores),
                                           pPacket->pBindInfo[i].signalSemaphoreCount * sizeof(VkSemaphore),
                                           pBindInfo[i].pSignalSemaphores);
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfo[i].pSignalSemaphores));
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfo));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        if (result == VK_SUCCESS) {
            if (bindInfoCount != 0) {
                for (uint32_t i = 0; i < bindInfoCount; i++) {
                    if (pBindInfo[i].pWaitSemaphores != NULL) {
                        for (uint32_t w = 0; w < pBindInfo[i].waitSemaphoreCount; w++) {
                            trim::ObjectInfo* pInfo = trim::get_Semaphore_objectInfo(pBindInfo[i].pWaitSemaphores[w]);
                            if (pInfo != NULL) {
                                pInfo->ObjectInfo.Semaphore.signaledOnQueue = VK_NULL_HANDLE;
                                pInfo->ObjectInfo.Semaphore.signaledOnSwapChain = VK_NULL_HANDLE;
                            }
                        }
                    }

                    if (pBindInfo[i].pSignalSemaphores != NULL) {
                        for (uint32_t s = 0; s < pBindInfo[i].signalSemaphoreCount; s++) {
                            trim::ObjectInfo* pInfo = trim::get_Semaphore_objectInfo(pBindInfo[i].pSignalSemaphores[s]);
                            if (pInfo != NULL) {
                                pInfo->ObjectInfo.Semaphore.signaledOnQueue = queue;
                                pInfo->ObjectInfo.Semaphore.signaledOnSwapChain = VK_NULL_HANDLE;
                            }
                        }
                    }
                }
            }
        }

        if (g_trimIsInTrim) {
            for (uint32_t i = 0; i < pBindInfo->bufferBindCount; i++) {
                trim::mark_Buffer_reference(pBindInfo->pBufferBinds[i].buffer);
            }
            for (uint32_t i = 0; i < pBindInfo->imageBindCount; i++) {
                trim::mark_Image_reference(pBindInfo->pImageBinds[i].image);
            }
            for (uint32_t i = 0; i < pBindInfo->imageOpaqueBindCount; i++) {
                trim::mark_Image_reference(pBindInfo->pImageOpaqueBinds[i].image);
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdWaitEvents(
    VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdWaitEvents* pPacket = NULL;
    size_t customSize;
    customSize = (eventCount * sizeof(VkEvent)) + (memoryBarrierCount * sizeof(VkMemoryBarrier)) +
                 (bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier)) +
                 (imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier));
    for (uint32_t i = 0; i < memoryBarrierCount; i++) customSize += get_struct_chain_size(&pMemoryBarriers[i]);
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) customSize += get_struct_chain_size(&pBufferMemoryBarriers[i]);
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) customSize += get_struct_chain_size(&pImageMemoryBarriers[i]);
    CREATE_TRACE_PACKET(vkCmdWaitEvents, customSize);
    mdd(commandBuffer)
        ->devTable.CmdWaitEvents(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, memoryBarrierCount,
                                 pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount,
                                 pImageMemoryBarriers);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdWaitEvents(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->eventCount = eventCount;
    pPacket->srcStageMask = srcStageMask;
    pPacket->dstStageMask = dstStageMask;
    pPacket->memoryBarrierCount = memoryBarrierCount;
    pPacket->bufferMemoryBarrierCount = bufferMemoryBarrierCount;
    pPacket->imageMemoryBarrierCount = imageMemoryBarrierCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pEvents), eventCount * sizeof(VkEvent), pEvents);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pEvents));

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMemoryBarriers), memoryBarrierCount * sizeof(VkMemoryBarrier),
                                       pMemoryBarriers);
    for (uint32_t i = 0; i < memoryBarrierCount; i++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pMemoryBarriers + i), pMemoryBarriers + i);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBufferMemoryBarriers),
                                       bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier), pBufferMemoryBarriers);
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pBufferMemoryBarriers + i), pBufferMemoryBarriers + i);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pImageMemoryBarriers),
                                       imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier), pImageMemoryBarriers);
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pImageMemoryBarriers + i), pImageMemoryBarriers + i);

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMemoryBarriers));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBufferMemoryBarriers));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pImageMemoryBarriers));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
            trim::ObjectInfo* pImageInfo = trim::get_Image_objectInfo(pImageMemoryBarriers[i].image);
            if (pImageInfo != nullptr) {
                pImageInfo->ObjectInfo.Image.mostRecentLayout = pImageMemoryBarriers[i].newLayout;
            }
        }

        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));

        if (g_trimIsInTrim) {
            for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
                trim::mark_Buffer_reference(pBufferMemoryBarriers[i].buffer);
            }
            for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
                trim::mark_Image_reference(pImageMemoryBarriers[i].image);
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdPipelineBarrier* pPacket = NULL;
    size_t customSize;
    customSize = (memoryBarrierCount * sizeof(VkMemoryBarrier)) + (bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier)) +
                 (imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier));
    for (uint32_t i = 0; i < memoryBarrierCount; i++) customSize += get_struct_chain_size(&pMemoryBarriers[i]);
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) customSize += get_struct_chain_size(&pBufferMemoryBarriers[i]);
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) customSize += get_struct_chain_size(&pImageMemoryBarriers[i]);
    CREATE_TRACE_PACKET(vkCmdPipelineBarrier, customSize);
    mdd(commandBuffer)
        ->devTable.CmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount,
                                      pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount,
                                      pImageMemoryBarriers);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdPipelineBarrier(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->srcStageMask = srcStageMask;
    pPacket->dstStageMask = dstStageMask;
    pPacket->dependencyFlags = dependencyFlags;
    pPacket->memoryBarrierCount = memoryBarrierCount;
    pPacket->bufferMemoryBarrierCount = bufferMemoryBarrierCount;
    pPacket->imageMemoryBarrierCount = imageMemoryBarrierCount;

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMemoryBarriers), memoryBarrierCount * sizeof(VkMemoryBarrier),
                                       pMemoryBarriers);
    for (uint32_t i = 0; i < memoryBarrierCount; i++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pMemoryBarriers + i), pMemoryBarriers + i);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBufferMemoryBarriers),
                                       bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier), pBufferMemoryBarriers);
    for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pBufferMemoryBarriers + i), pBufferMemoryBarriers + i);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pImageMemoryBarriers),
                                       imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier), pImageMemoryBarriers);
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pImageMemoryBarriers + i), pImageMemoryBarriers + i);

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMemoryBarriers));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBufferMemoryBarriers));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pImageMemoryBarriers));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
            trim::ObjectInfo* pImageInfo = trim::get_Image_objectInfo(pImageMemoryBarriers[i].image);
            if (pImageInfo != nullptr) {
                trim::ImageTransition transition;
                transition.image = pImageMemoryBarriers[i].image;
                transition.initialLayout = pImageMemoryBarriers[i].oldLayout;
                transition.finalLayout = pImageMemoryBarriers[i].newLayout;
                transition.srcAccessMask = pImageMemoryBarriers[i].srcAccessMask;
                transition.dstAccessMask = pImageMemoryBarriers[i].dstAccessMask;

                trim::AddImageTransition(commandBuffer, transition);
                if (g_trimIsInTrim) {
                    trim::mark_Image_reference(pImageMemoryBarriers[i].image);
                }
            }
        }

        for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
            trim::ObjectInfo* pBufferInfo = trim::get_Buffer_objectInfo(pBufferMemoryBarriers[i].buffer);
            if (pBufferInfo != nullptr) {
                trim::BufferTransition transition;
                transition.buffer = pBufferMemoryBarriers[i].buffer;
                transition.srcAccessMask = pBufferMemoryBarriers[i].srcAccessMask;
                transition.dstAccessMask = pBufferMemoryBarriers[i].dstAccessMask;

                trim::AddBufferTransition(commandBuffer, transition);
                if (g_trimIsInTrim) {
                    trim::mark_Buffer_reference(pBufferMemoryBarriers[i].buffer);
                }
            }
        }

        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

bool getCmdPushConstantsRemapEnableFlag() {
    static bool EnableBuffer = false;
    static bool FirstTimeRun = true;
    if (FirstTimeRun) {
        FirstTimeRun = false;
        const char* env_remap_buffer_enable = vktrace_get_global_var(VKTRACE_ENABLE_VKCMDPUSHCONSTANTS_REMAP_ENV);
        if (env_remap_buffer_enable) {
            int envvalue;
            if (sscanf(env_remap_buffer_enable, "%d", &envvalue) == 1) {
                if (envvalue != 0) {
                    EnableBuffer = true;
                }
            }
        }
    }
    return EnableBuffer;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                                                                       VkShaderStageFlags stageFlags, uint32_t offset,
                                                                       uint32_t size, const void* pValues) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdPushConstants* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCmdPushConstants, size);
    mdd(commandBuffer)->devTable.CmdPushConstants(commandBuffer, layout, stageFlags, offset, size, pValues);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdPushConstants(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->layout = layout;
    pPacket->stageFlags = stageFlags;
    pPacket->offset = offset;
    pPacket->size = size;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pValues), size, pValues);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pValues));
    if (getCmdPushConstantsRemapEnableFlag()) {
        bool deviceAddrFound = false;
        VkDeviceAddress *ptr = (VkDeviceAddress *)pValues;
        for (int i = 0; i < size / (sizeof(VkDeviceAddress)); ++i) {
            auto it = g_BuftoDeviceAddrRev.find(ptr[i]);
            if (it != g_BuftoDeviceAddrRev.end()) {
                deviceAddrFound = true;
                break;
            }
        }
        if (deviceAddrFound) {
            vktrace_LogDebug("Created a VKTRACE_TPI_VK_vkCmdPushConstantsRemap packet");
            pHeader->packet_id = VKTRACE_TPI_VK_vkCmdPushConstantsRemap;
        }
    }
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                                                                         const VkCommandBuffer* pCommandBuffers) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdExecuteCommands* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCmdExecuteCommands, commandBufferCount * sizeof(VkCommandBuffer));
    mdd(commandBuffer)->devTable.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        if (g_commandBufferToCommandBuffers.find(commandBuffer) != g_commandBufferToCommandBuffers.end()) {
            g_commandBufferToCommandBuffers[commandBuffer].clear();
        }
        g_commandBufferToCommandBuffers[commandBuffer].push_back(commandBuffer);
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            g_commandBufferToCommandBuffers[commandBuffer].push_back(pCommandBuffers[i]);
        }
    }
#endif
    pPacket = interpret_body_as_vkCmdExecuteCommands(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->commandBufferCount = commandBufferCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCommandBuffers), commandBufferCount * sizeof(VkCommandBuffer),
                                       pCommandBuffers);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCommandBuffers));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
            trim::mark_CommandBuffer_reference(commandBuffer);
            if (pCommandBuffers != nullptr && commandBufferCount > 0) {
                for (uint32_t i = 0; i < commandBufferCount; i++) {
                    trim::mark_CommandBuffer_reference(pCommandBuffers[i]);
                }
            }
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache,
                                                                               size_t* pDataSize, void* pData) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkGetPipelineCacheData* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    result = mdd(device)->devTable.GetPipelineCacheData(device, pipelineCache, pDataSize, pData);
    endTime = vktrace_get_time();
    assert(pDataSize);
    // Currently do not capture payload in vkGetPipelineCacheData() API.
    // So we won't reserve spaces for pDataSize and pData in the buffer for now.
    CREATE_TRACE_PACKET(vkGetPipelineCacheData, 0);
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetPipelineCacheData(pHeader);
    pPacket->device = device;
    pPacket->pipelineCache = pipelineCache;
    // Currently do not capture payload in vkGetPipelineCacheData() API.
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDataSize), 0, NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pData), 0, NULL);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDataSize));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pData));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        if (g_trimIsInTrim) {
            trim::mark_PipelineCache_reference(pipelineCache);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

// This function returns the size of VkGraphicsPipelineCreateInfo struct to be used when creating a packet.
static size_t get_VkGraphicsPipelineCreateInfo_size(const VkGraphicsPipelineCreateInfo* pCreateInfos) {
    size_t entryPointNameLength = 0;
    size_t struct_size = get_struct_chain_size(pCreateInfos);

    if ((pCreateInfos->stageCount) && (pCreateInfos->pStages != nullptr)) {
        VkPipelineShaderStageCreateInfo* pStage = const_cast<VkPipelineShaderStageCreateInfo*>(pCreateInfos->pStages);
        for (uint32_t i = 0; i < pCreateInfos->stageCount; i++) {
            if (pStage->pName) {
                entryPointNameLength = strlen(pStage->pName) + 1;
                struct_size += ROUNDUP_TO_4(entryPointNameLength) - entryPointNameLength;
            }
            struct_size += get_struct_chain_size(pStage);
            ++pStage;
        }
    }
    struct_size += get_struct_chain_size(pCreateInfos->pVertexInputState);
    struct_size += get_struct_chain_size(pCreateInfos->pInputAssemblyState);
    struct_size += get_struct_chain_size(pCreateInfos->pTessellationState);
    struct_size += get_struct_chain_size(pCreateInfos->pViewportState);
    struct_size += get_struct_chain_size(pCreateInfos->pRasterizationState);
    struct_size += get_struct_chain_size(pCreateInfos->pMultisampleState);
    struct_size += get_struct_chain_size(pCreateInfos->pDepthStencilState);
    struct_size += get_struct_chain_size(pCreateInfos->pColorBlendState);
    struct_size += get_struct_chain_size(pCreateInfos->pDynamicState);
    return struct_size;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                                                  uint32_t createInfoCount,
                                                                                  const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                                                  const VkAllocationCallbacks* pAllocator,
                                                                                  VkPipeline* pPipelines) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateGraphicsPipelines* pPacket = NULL;
    size_t total_size = 0;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        total_size += get_VkGraphicsPipelineCreateInfo_size(&pCreateInfos[i]);
    }
    CREATE_TRACE_PACKET(vkCreateGraphicsPipelines,
                        total_size + sizeof(VkAllocationCallbacks) + createInfoCount * sizeof(VkPipeline));
    result =
        mdd(device)->devTable.CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateGraphicsPipelines(pHeader);
    pPacket->device = device;
    pPacket->pipelineCache = pipelineCache;
    pPacket->createInfoCount = createInfoCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfos),
                                       createInfoCount * sizeof(VkGraphicsPipelineCreateInfo), pCreateInfos);
    add_VkGraphicsPipelineCreateInfos_to_trace_packet(pHeader, (VkGraphicsPipelineCreateInfo*)pPacket->pCreateInfos, pCreateInfos,
                                                      createInfoCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPipelines), createInfoCount * sizeof(VkPipeline), pPipelines);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfos));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPipelines));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        for (uint32_t i = 0; i < createInfoCount; i++) {
            trim::ObjectInfo& info = trim::add_Pipeline_object(pPipelines[i]);
            info.belongsToDevice = device;
            info.ObjectInfo.Pipeline.isGraphicsPipeline = true;
            info.ObjectInfo.Pipeline.pipelineCache = pipelineCache;
            info.ObjectInfo.Pipeline.renderPassVersion = trim::get_RenderPassVersion(pCreateInfos[i].renderPass);
            info.ObjectInfo.Pipeline.shaderModuleCreateInfoCount = pCreateInfos[i].stageCount;
            info.ObjectInfo.Pipeline.pShaderModuleCreateInfos =
                VKTRACE_NEW_ARRAY(VkShaderModuleCreateInfo, pCreateInfos[i].stageCount);

            for (uint32_t stageIndex = 0; stageIndex < info.ObjectInfo.Pipeline.shaderModuleCreateInfoCount; stageIndex++) {
                trim::ObjectInfo* pShaderModuleInfo = trim::get_ShaderModule_objectInfo(pCreateInfos[i].pStages[stageIndex].module);
                if (pShaderModuleInfo != nullptr) {
                    trim::StateTracker::copy_VkShaderModuleCreateInfo(
                        &info.ObjectInfo.Pipeline.pShaderModuleCreateInfos[stageIndex],
                        pShaderModuleInfo->ObjectInfo.ShaderModule.createInfo);
                } else {
                    memset(&info.ObjectInfo.Pipeline.pShaderModuleCreateInfos[stageIndex], 0, sizeof(VkShaderModuleCreateInfo));
                }

                if (g_trimIsInTrim) {
                    trim::mark_ShaderModule_reference(pCreateInfos[i].pStages[stageIndex].module);
                }
            }

            trim::StateTracker::copy_VkGraphicsPipelineCreateInfo(&info.ObjectInfo.Pipeline.graphicsPipelineCreateInfo,
                                                                  pCreateInfos[i]);
            if (pAllocator != NULL) {
                info.ObjectInfo.Pipeline.pAllocator = pAllocator;
                trim::add_Allocator(pAllocator);
            }
        }

        if (g_trimIsInTrim) {
            trim::mark_PipelineCache_reference(pipelineCache);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

uint64_t getVkComputePipelineCreateInfosAdditionalSize(uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos) {
    uint64_t uiRet = 0;
    VkPipelineShaderStageCreateInfo* packetShader;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        uiRet += sizeof(VkPipelineShaderStageCreateInfo);
        packetShader = (VkPipelineShaderStageCreateInfo*)&pCreateInfos[i].stage;
        uiRet += strlen(packetShader->pName) + 1;
        uiRet += sizeof(VkSpecializationInfo);
        if (packetShader->pSpecializationInfo != NULL) {
            uiRet += sizeof(VkSpecializationMapEntry) * packetShader->pSpecializationInfo->mapEntryCount;
            uiRet += packetShader->pSpecializationInfo->dataSize;
        }
    }
    return uiRet;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                                                 uint32_t createInfoCount,
                                                                                 const VkComputePipelineCreateInfo* pCreateInfos,
                                                                                 const VkAllocationCallbacks* pAllocator,
                                                                                 VkPipeline* pPipelines) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateComputePipelines* pPacket = NULL;
    size_t pnextSize = 0;

    // Determine size of pNext chain
    for (uint32_t iter = 0; iter < createInfoCount; iter++) pnextSize += get_struct_chain_size((void*)&pCreateInfos[iter]);

    CREATE_TRACE_PACKET(vkCreateComputePipelines, pnextSize + createInfoCount * sizeof(VkComputePipelineCreateInfo) +
                                                      getVkComputePipelineCreateInfosAdditionalSize(createInfoCount, pCreateInfos) +
                                                      sizeof(VkAllocationCallbacks) + createInfoCount * sizeof(VkPipeline));

    result =
        mdd(device)->devTable.CreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateComputePipelines(pHeader);
    pPacket->device = device;
    pPacket->pipelineCache = pipelineCache;
    pPacket->createInfoCount = createInfoCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfos),
                                       createInfoCount * sizeof(VkComputePipelineCreateInfo), pCreateInfos);
    add_VkComputePipelineCreateInfos_to_trace_packet(pHeader, (VkComputePipelineCreateInfo*)pPacket->pCreateInfos, pCreateInfos,
                                                     createInfoCount);
    for (uint32_t iter = 0; iter < createInfoCount; iter++)
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)&pPacket->pCreateInfos[iter], (void*)&pCreateInfos[iter]);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPipelines), createInfoCount * sizeof(VkPipeline), pPipelines);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfos));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPipelines));
    if (!g_trimEnabled) {
        // trim not enabled, send packet as usual
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        for (uint32_t i = 0; i < createInfoCount; i++) {
            trim::ObjectInfo& info = trim::add_Pipeline_object(pPipelines[i]);
            info.belongsToDevice = device;
            info.ObjectInfo.Pipeline.isGraphicsPipeline = false;
            info.ObjectInfo.Pipeline.pipelineCache = pipelineCache;
            info.ObjectInfo.Pipeline.shaderModuleCreateInfoCount = 1;
            info.ObjectInfo.Pipeline.pShaderModuleCreateInfos = VKTRACE_NEW(VkShaderModuleCreateInfo);

            trim::ObjectInfo* pShaderModuleInfo = trim::get_ShaderModule_objectInfo(pCreateInfos[i].stage.module);
            if (pShaderModuleInfo != nullptr) {
                trim::StateTracker::copy_VkShaderModuleCreateInfo(&info.ObjectInfo.Pipeline.pShaderModuleCreateInfos[0],
                                                                  pShaderModuleInfo->ObjectInfo.ShaderModule.createInfo);
            } else {
                memset(info.ObjectInfo.Pipeline.pShaderModuleCreateInfos, 0, sizeof(VkShaderModuleCreateInfo));
            }

            trim::StateTracker::copy_VkComputePipelineCreateInfo(&info.ObjectInfo.Pipeline.computePipelineCreateInfo,
                                                                 pCreateInfos[i]);
            if (pAllocator != NULL) {
                info.ObjectInfo.Pipeline.pAllocator = pAllocator;
                trim::add_Allocator(pAllocator);
            }

            if (g_trimIsInTrim) {
                trim::mark_ShaderModule_reference(pCreateInfos[i].stage.module);
            }
        }
        if (g_trimIsInTrim) {
            trim::mark_PipelineCache_reference(pipelineCache);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreatePipelineCache(VkDevice device,
                                                                              const VkPipelineCacheCreateInfo* pCreateInfo,
                                                                              const VkAllocationCallbacks* pAllocator,
                                                                              VkPipelineCache* pPipelineCache) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreatePipelineCache* pPacket = NULL;
    // Currently do not capture payload in vkCreatePipelineCache() API.
    // So we won't reserve spaces for pCreateInfo->pInitialData in the buffer for now.
    CREATE_TRACE_PACKET(vkCreatePipelineCache, get_struct_chain_size((void*)pCreateInfo) +
                                                   sizeof(VkAllocationCallbacks) +
                                                   sizeof(VkPipelineCache));
    result = mdd(device)->devTable.CreatePipelineCache(device, pCreateInfo, pAllocator, pPipelineCache);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreatePipelineCache(pHeader);
    pPacket->device = device;

    // Currently do not capture payload in vkCreatePipelineCache() API.
    VkPipelineCacheCreateInfo createInfoCopy = *pCreateInfo;
    createInfoCopy.pInitialData = NULL;
    createInfoCopy.initialDataSize = 0;

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkPipelineCacheCreateInfo), &createInfoCopy);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPipelineCache), sizeof(VkPipelineCache), pPipelineCache);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pInitialData));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPipelineCache));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_PipelineCache_object(*pPipelineCache);
        info.belongsToDevice = device;
        info.ObjectInfo.PipelineCache.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.PipelineCache.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                                         const VkRenderPassBeginInfo* pRenderPassBegin,
                                                                         VkSubpassContents contents) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdBeginRenderPass* pPacket = NULL;
    size_t clearValueSize = sizeof(VkClearValue) * pRenderPassBegin->clearValueCount;
    CREATE_TRACE_PACKET(vkCmdBeginRenderPass, get_struct_chain_size((void*)pRenderPassBegin) + clearValueSize);
    mdd(commandBuffer)->devTable.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdBeginRenderPass(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->contents = contents;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderPassBegin), sizeof(VkRenderPassBeginInfo), pRenderPassBegin);
    if (pRenderPassBegin) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pRenderPassBegin), pRenderPassBegin);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderPassBegin->pClearValues), clearValueSize,
                                       pRenderPassBegin->pClearValues);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderPassBegin->pClearValues));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderPassBegin));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        trim::ObjectInfo* pCommandBuffer = trim::get_CommandBuffer_objectInfo(commandBuffer);
        if (pCommandBuffer != nullptr) {
            pCommandBuffer->ObjectInfo.CommandBuffer.activeRenderPass = pRenderPassBegin->renderPass;

            trim::ObjectInfo* pFramebuffer = trim::get_Framebuffer_objectInfo(pRenderPassBegin->framebuffer);
            trim::ObjectInfo* pRenderPass = trim::get_RenderPass_objectInfo(pRenderPassBegin->renderPass);
            if (pRenderPass != nullptr && pFramebuffer != nullptr) {
                assert(pRenderPass->ObjectInfo.RenderPass.attachmentCount <= pFramebuffer->ObjectInfo.Framebuffer.attachmentCount);
                uint32_t minAttachmentCount = std::min<uint32_t>(pRenderPass->ObjectInfo.RenderPass.attachmentCount,
                                                                 pFramebuffer->ObjectInfo.Framebuffer.attachmentCount);
                for (uint32_t i = 0; i < minAttachmentCount; i++) {
                    trim::ObjectInfo* pImageView =
                        trim::get_ImageView_objectInfo(pFramebuffer->ObjectInfo.Framebuffer.pAttachments[i]);
                    if (pImageView != nullptr) {
                        pRenderPass->ObjectInfo.RenderPass.pAttachments[i].image = pImageView->ObjectInfo.ImageView.image;
                    } else {
                        pRenderPass->ObjectInfo.RenderPass.pAttachments[i].image = VK_NULL_HANDLE;
                    }
                }
            }
        }

        if (g_trimIsInTrim) {
            trim::mark_Framebuffer_reference(pRenderPassBegin->framebuffer);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdBeginRenderPass2(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo) {
    __HOOKED_vkCmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdBeginRenderPass2KHR(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdBeginRenderPass2KHR* pPacket = NULL;
    size_t clearValueSize = sizeof(VkClearValue) * pRenderPassBegin->clearValueCount;
    CREATE_TRACE_PACKET(vkCmdBeginRenderPass2KHR, get_struct_chain_size((void*)pRenderPassBegin) + get_struct_chain_size((void*)pSubpassBeginInfo) + sizeof(VkSubpassBeginInfo) + clearValueSize);
    mdd(commandBuffer)->devTable.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdBeginRenderPass2KHR(pHeader);
    pPacket->commandBuffer = commandBuffer;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderPassBegin), sizeof(VkRenderPassBeginInfo), pRenderPassBegin);
    if (pRenderPassBegin) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pRenderPassBegin), pRenderPassBegin);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderPassBegin->pClearValues), clearValueSize, pRenderPassBegin->pClearValues);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSubpassBeginInfo), sizeof(VkSubpassBeginInfo), pSubpassBeginInfo);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderPassBegin->pClearValues));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderPassBegin));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSubpassBeginInfo));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        trim::ObjectInfo* pCommandBuffer = trim::get_CommandBuffer_objectInfo(commandBuffer);
        if (pCommandBuffer != nullptr) {
            pCommandBuffer->ObjectInfo.CommandBuffer.activeRenderPass = pRenderPassBegin->renderPass;

            trim::ObjectInfo* pFramebuffer = trim::get_Framebuffer_objectInfo(pRenderPassBegin->framebuffer);
            trim::ObjectInfo* pRenderPass = trim::get_RenderPass_objectInfo(pRenderPassBegin->renderPass);
            if (pRenderPass != nullptr && pFramebuffer != nullptr) {
                assert(pRenderPass->ObjectInfo.RenderPass.attachmentCount <= pFramebuffer->ObjectInfo.Framebuffer.attachmentCount);
                uint32_t minAttachmentCount = std::min<uint32_t>(pRenderPass->ObjectInfo.RenderPass.attachmentCount,
                                                                 pFramebuffer->ObjectInfo.Framebuffer.attachmentCount);
                for (uint32_t i = 0; i < minAttachmentCount; i++) {
                    trim::ObjectInfo* pImageView =
                        trim::get_ImageView_objectInfo(pFramebuffer->ObjectInfo.Framebuffer.pAttachments[i]);
                    if (pImageView != nullptr) {
                        pRenderPass->ObjectInfo.RenderPass.pAttachments[i].image = pImageView->ObjectInfo.ImageView.image;
                    } else {
                        pRenderPass->ObjectInfo.RenderPass.pAttachments[i].image = VK_NULL_HANDLE;
                    }
                }
            }
        }

        if (g_trimIsInTrim) {
            trim::mark_Framebuffer_reference(pRenderPassBegin->framebuffer);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdBeginRenderingKHR(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfoKHR* pRenderingInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdBeginRenderingKHR* pPacket = NULL;

    size_t customSize = 0;
    customSize += get_struct_chain_size((void*)pRenderingInfo);
    for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i) {
        customSize += get_struct_chain_size((void*)&pRenderingInfo->pColorAttachments[i]);
    }
    customSize += get_struct_chain_size((void*)(pRenderingInfo->pDepthAttachment));
    customSize += get_struct_chain_size((void*)(pRenderingInfo->pStencilAttachment));
    CREATE_TRACE_PACKET(vkCmdBeginRenderingKHR, customSize);

    mdd(commandBuffer)->devTable.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdBeginRenderingKHR(pHeader);

    pPacket->commandBuffer = commandBuffer;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderingInfo), sizeof(VkRenderingInfoKHR), pRenderingInfo);
    if (pRenderingInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pRenderingInfo, pRenderingInfo);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderingInfo->pColorAttachments),
                                       (pRenderingInfo->colorAttachmentCount) * sizeof(VkRenderingAttachmentInfoKHR), pRenderingInfo->pColorAttachments);
    for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i) {
        VkRenderingAttachmentInfoKHR* pColorAttachment = (VkRenderingAttachmentInfoKHR*)&(pPacket->pRenderingInfo->pColorAttachments[i]);
        if (pColorAttachment) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pColorAttachment, &pRenderingInfo->pColorAttachments[i]);
    }

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderingInfo->pDepthAttachment), sizeof(VkRenderingAttachmentInfoKHR), pRenderingInfo->pDepthAttachment);
    if(pRenderingInfo->pDepthAttachment) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pRenderingInfo->pDepthAttachment, pRenderingInfo->pDepthAttachment);

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRenderingInfo->pStencilAttachment), sizeof(VkRenderingAttachmentInfoKHR), pRenderingInfo->pStencilAttachment);
    if(pRenderingInfo->pStencilAttachment) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pRenderingInfo->pStencilAttachment, pRenderingInfo->pStencilAttachment);

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderingInfo->pColorAttachments));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderingInfo->pDepthAttachment));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderingInfo->pStencilAttachment));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRenderingInfo));

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        // NOT TEST
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool,
                                                                             uint32_t descriptorSetCount,
                                                                             const VkDescriptorSet* pDescriptorSets) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkFreeDescriptorSets* pPacket = NULL;
    CREATE_TRACE_PACKET(vkFreeDescriptorSets, descriptorSetCount * sizeof(VkDescriptorSet));
    result = mdd(device)->devTable.FreeDescriptorSets(device, descriptorPool, descriptorSetCount, pDescriptorSets);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        for (uint32_t i = 0; i < descriptorSetCount; i++) {
            auto it = g_descsetToMemory.find(pDescriptorSets[i]);
            if (it != g_descsetToMemory.end()) {
                g_descsetToMemory.erase(it);
            }
        }
    }
#endif
    pPacket = interpret_body_as_vkFreeDescriptorSets(pHeader);
    pPacket->device = device;
    pPacket->descriptorPool = descriptorPool;
    pPacket->descriptorSetCount = descriptorSetCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorSets), descriptorSetCount * sizeof(VkDescriptorSet),
                                       pDescriptorSets);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorSets));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pPoolInfo = trim::get_DescriptorPool_objectInfo(descriptorPool);
        if (pPoolInfo != NULL &&
            (pPoolInfo->ObjectInfo.DescriptorPool.createFlags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) != 0) {
            pPoolInfo->ObjectInfo.DescriptorPool.numSets -= descriptorSetCount;

            for (uint32_t i = 0; i < descriptorSetCount; i++) {
                // Clean up memory
                trim::remove_DescriptorSet_object(pDescriptorSets[i]);
                if (g_trimIsInTrim) {
                    trim::mark_DescriptorSet_reference(pDescriptorSets[i]);
                }
            }
        }
        if (g_trimIsInTrim) {
            trim::mark_DescriptorPool_reference(descriptorPool);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                                                      const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateImage* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCreateImage, get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkImage));

    if (g_trimEnabled) {
        // need to add TRANSFER_SRC usage to the image so that we can copy out of it.
        // By Doc, image for which we can use vkCmdCopyImageToBuffer and vkCmdCopyImage
        // to copy out must have been created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT usage flag.
        (const_cast<VkImageCreateInfo*>(pCreateInfo))->usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    result = mdd(device)->devTable.CreateImage(device, pCreateInfo, pAllocator, pImage);

    if (g_trimEnabled && (pCreateInfo->tiling == VK_IMAGE_TILING_OPTIMAL) && (result == VK_SUCCESS)) {
        // For all miplevels of the image, here we fisrt get all subresource
        // memory sizes and put them in a vector, then save the vector to a
        // map of which the key is image handle.
        //
        // Such process is part of a solution to fix an image difference
        // problem for some title:
        //
        // For some title, if trim start at some specific locations, trimmed
        // trace file playback show image difference compared with original
        // title running.

        // The reason of the problem is: when trim copy out an optimal tiling
        // image which is bound to device local only memory, it will first
        // copy the image data to a staging buffer. An API
        // vkGetImageSubresourceLayout is used to get every subresource data
        // offset for saving to the buffer. But by Doc, the image must
        // be linear tiling image. In the problem title, trim need to copy
        // out some optimal tiling images to staging buffer, the call
        // vkGetImageSubresourceLayout return wrong offset and size for some
        // miplevel of the image and cause image data overwriting and finally
        // cause trimmed trace file playback show image difference.
        //
        // The fix of the problem use the following process to replace
        // vkGetImageSubresourceLayout call and get the subresource size and
        // then get the offset. The basic process is: create image with
        // specific miplevel, then query its memory requirement to get the
        // size. such process performed when vkCreateImage and save these
        // sizes to a map. When starting to trim and copy out the image, trim
        // will directly use these sizes.

        std::vector<VkDeviceSize> subResourceSizes;
        if (trim::calculateImageAllSubResourceSize(device, *pCreateInfo, pAllocator, subResourceSizes)) {
            trim::addImageSubResourceSizes(*pImage, subResourceSizes);
        }
    }
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateImage(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkImageCreateInfo), pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pQueueFamilyIndices),
                                       sizeof(uint32_t) * pCreateInfo->queueFamilyIndexCount, pCreateInfo->pQueueFamilyIndices);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pImage), sizeof(VkImage), pImage);
    pPacket->result = result;

    if (g_trimEnabled) {
        VkImageCreateInfo* pCreateInfo = const_cast<VkImageCreateInfo*>(pPacket->pCreateInfo);
        pCreateInfo->usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pQueueFamilyIndices));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pImage));

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
#if TRIM_USE_ORDERED_IMAGE_CREATION
        trim::add_Image_call(trim::copy_packet(pHeader));
#endif  // TRIM_USE_ORDERED_IMAGE_CREATION"
        trim::ObjectInfo& info = trim::add_Image_object(*pImage);
        info.belongsToDevice = device;
#if !TRIM_USE_ORDERED_IMAGE_CREATION
        info.ObjectInfo.Image.pCreatePacket = trim::copy_packet(pHeader);
#endif  //! TRIM_USE_ORDERED_IMAGE_CREATION
        info.ObjectInfo.Image.bIsSwapchainImage = false;
        info.ObjectInfo.Image.format = pCreateInfo->format;
        info.ObjectInfo.Image.imageType = pCreateInfo->imageType;
        info.ObjectInfo.Image.aspectMask = trim::getImageAspectFromFormat(pCreateInfo->format);
        info.ObjectInfo.Image.extent = pCreateInfo->extent;
        info.ObjectInfo.Image.mipLevels = pCreateInfo->mipLevels;
        info.ObjectInfo.Image.arrayLayers = pCreateInfo->arrayLayers;
        info.ObjectInfo.Image.sharingMode = pCreateInfo->sharingMode;
        info.ObjectInfo.Image.needsStagingBuffer = (pCreateInfo->tiling == VK_IMAGE_TILING_OPTIMAL);
        info.ObjectInfo.Image.queueFamilyIndex =
            (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT && pCreateInfo->pQueueFamilyIndices != NULL &&
             pCreateInfo->queueFamilyIndexCount != 0)
                ? pCreateInfo->pQueueFamilyIndices[0]
                : 0;
        info.ObjectInfo.Image.initialLayout = pCreateInfo->initialLayout;
        info.ObjectInfo.Image.mostRecentLayout = pCreateInfo->initialLayout;
        if (pAllocator != NULL) {
            info.ObjectInfo.Image.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                                                       const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateBuffer* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCreateBuffer, get_struct_chain_size((void*)pCreateInfo) + ROUNDUP_TO_4(sizeof(VkBufferOpaqueCaptureAddressCreateInfo)) + sizeof(VkAllocationCallbacks) + sizeof(VkBuffer));
    VkBufferCreateInfo replayCreateInfo = *pCreateInfo;
    VkBufferCreateInfo trimCreateInfo = *pCreateInfo;
    if (g_trimEnabled) {
        // need to add TRANSFER_SRC usage to the buffer so that we can copy out of it.
        trimCreateInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        pCreateInfo = &trimCreateInfo;
    }

    if (pCreateInfo->usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR) {
        auto it = g_deviceToFeatureSupport.find(device);
        if (it != g_deviceToFeatureSupport.end() && it->second.accelerationStructureCaptureReplay) {
            const_cast<VkBufferCreateInfo*>(pCreateInfo)->usage = pCreateInfo->usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_EXT;
        }
    }

    if (pCreateInfo->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_EXT ) {
        auto it = g_deviceToFeatureSupport.find(device);
        if (it != g_deviceToFeatureSupport.end() && it->second.bufferDeviceAddressCaptureReplay) {
            const_cast<VkBufferCreateInfo*>(pCreateInfo)->flags = pCreateInfo->flags | VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_EXT;
        } else {
            const_cast<VkBufferCreateInfo*>(pCreateInfo)->flags = pCreateInfo->flags & ~VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_EXT;
            vktrace_LogDebug("The device doesn't support bufferDeviceAddressCaptureReplay feature.");
        }
    }

    result = mdd(device)->devTable.CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    if (pCreateInfo->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        g_shaderDeviceAddrBufferToMem[*pBuffer] = 0;
    }

    VkBufferOpaqueCaptureAddressCreateInfo captureAddressCreateInfo = {};
    if (pCreateInfo->flags & VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_EXT) {
        VkBufferDeviceAddressInfo addressInfo = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, *pBuffer};
        uint64_t captureAddress = mdd(device)->devTable.GetBufferOpaqueCaptureAddress(device,&addressInfo);
        VkBufferOpaqueCaptureAddressCreateInfo *pCaptureAddressCreateInfo = (VkBufferOpaqueCaptureAddressCreateInfo*)find_ext_struct((const vulkan_struct_header*)pCreateInfo->pNext, VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO);
        if (pCaptureAddressCreateInfo == nullptr) {
            pCaptureAddressCreateInfo = (VkBufferOpaqueCaptureAddressCreateInfo*)find_ext_struct((const vulkan_struct_header*)pCreateInfo->pNext, VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO_KHR);
        }
        if (pCaptureAddressCreateInfo != nullptr) {
            pCaptureAddressCreateInfo->opaqueCaptureAddress = captureAddress;
        } else {
            captureAddressCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO;
            captureAddressCreateInfo.opaqueCaptureAddress = captureAddress;
            const void* temp = pCreateInfo->pNext;
            const_cast<VkBufferCreateInfo*>(pCreateInfo)->pNext = (const void*)&captureAddressCreateInfo;
            captureAddressCreateInfo.pNext = temp;
        }
    }

    if (g_trimEnabled) {
        // need to add TRANSFER_DST usage to the buffer so that we can recreate it.
        replayCreateInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        pCreateInfo = &replayCreateInfo;
    }
    pPacket = interpret_body_as_vkCreateBuffer(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkBufferCreateInfo), pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pQueueFamilyIndices), sizeof(uint32_t) * pCreateInfo->queueFamilyIndexCount, pCreateInfo->pQueueFamilyIndices);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBuffer), sizeof(VkBuffer), pBuffer);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pQueueFamilyIndices));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBuffer));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo &info = trim::add_Buffer_object(*pBuffer);
        info.belongsToDevice = device;
        info.ObjectInfo.Buffer.pCreatePacket = trim::copy_packet(pHeader);
        info.ObjectInfo.Buffer.size = pCreateInfo->size;
        if (pCreateInfo->queueFamilyIndexCount > 0) { info.ObjectInfo.Buffer.queueFamilyIndex = pCreateInfo->pQueueFamilyIndices[0]; }
        if (pAllocator != NULL) {
            info.ObjectInfo.Buffer.pAllocator = pAllocator;
        }
        if (pAllocator != NULL) {
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                                                                    const VkAllocationCallbacks* pAllocator) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkDestroyBuffer* pPacket = NULL;
    bool bFind = false;
    if (g_trimEnabled && !g_trimIsInTrim) {
        auto it = std::find(g_bufferInCmdBuildAS.begin(), g_bufferInCmdBuildAS.end(), buffer);
        if (it != g_bufferInCmdBuildAS.end()) {
            bFind = true;
        }
    }
    if (bFind) {
        return ;
    }
    CREATE_TRACE_PACKET(vkDestroyBuffer, sizeof(VkAllocationCallbacks));
    mdd(device)->devTable.DestroyBuffer(device, buffer, pAllocator);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        if (g_bufferToDeviceMemory.find(buffer) != g_bufferToDeviceMemory.end() &&
            g_bufferToDeviceMemory[buffer].device == device) {
            g_bufferToDeviceMemory.erase(buffer);
        }
    }
#endif
    pPacket = interpret_body_as_vkDestroyBuffer(pHeader);
    pPacket->device = device;
    pPacket->buffer = buffer;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    auto it = g_shaderDeviceAddrBufferToMem.find(buffer);
    if (it != g_shaderDeviceAddrBufferToMem.end()) {
        g_shaderDeviceAddrBufferToMemRev.erase(it->second);
        g_shaderDeviceAddrBufferToMem.erase(it);
    }
    auto it2 = g_BuftoDeviceAddr.find(buffer);
    if (it2 != g_BuftoDeviceAddr.end()) {
        g_BuftoDeviceAddrRev.erase(it2->second);
        g_BuftoDeviceAddr.erase(it2);
    }
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::remove_Buffer_object(buffer);
        if (g_trimIsInTrim) {
            trim::mark_Buffer_reference(buffer);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                                           VkDeviceSize memoryOffset) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkBindBufferMemory* pPacket = NULL;
    CREATE_TRACE_PACKET(vkBindBufferMemory, 0);
    result = mdd(device)->devTable.BindBufferMemory(device, buffer, memory, memoryOffset);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        DeviceMemory deviceMemory = {};
        deviceMemory.device = device;
        deviceMemory.memory = memory;
        g_bufferToDeviceMemory[buffer] = deviceMemory;
    }
#endif
    pPacket = interpret_body_as_vkBindBufferMemory(pHeader);
    pPacket->device = device;
    pPacket->buffer = buffer;
    pPacket->memory = memory;
    pPacket->memoryOffset = memoryOffset;
    pPacket->result = result;
    auto it = g_shaderDeviceAddrBufferToMem.find(buffer);
    if (it != g_shaderDeviceAddrBufferToMem.end()) {
        it->second = memory;
        g_shaderDeviceAddrBufferToMemRev[memory] = buffer;
    }
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_Buffer_objectInfo(buffer);
        if (pInfo != NULL) {
            pInfo->ObjectInfo.Buffer.pBindBufferMemoryPacket = trim::copy_packet(pHeader);
            pInfo->ObjectInfo.Buffer.memory = memory;
            pInfo->ObjectInfo.Buffer.memoryOffset = memoryOffset;
            pInfo->ObjectInfo.Buffer.needsStagingBuffer = trim::IsMemoryDeviceOnly(memory);
        }
        if (g_trimIsInTrim) {
            trim::mark_Buffer_reference(buffer);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

static vktrace_trace_packet_header* generateBindBufferMemoryPacket(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                                   VkDeviceSize memoryOffset) {
    vktrace_trace_packet_header* pHeader;
    packet_vkBindBufferMemory* pPacket = NULL;
    CREATE_TRACE_PACKET(vkBindBufferMemory, 0);
    pPacket = interpret_body_as_vkBindBufferMemory(pHeader);
    pPacket->device = device;
    pPacket->buffer = buffer;
    pPacket->memory = memory;
    pPacket->memoryOffset = memoryOffset;
    pPacket->result = VK_SUCCESS;
    return pHeader;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkBindBufferMemory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindBufferMemoryInfo* pBindInfos) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkBindBufferMemory2* pPacket = NULL;
    size_t extra_size = 0;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        extra_size += get_struct_chain_size((void*)&(pBindInfos[i]));
    }
    CREATE_TRACE_PACKET(vkBindBufferMemory2, extra_size);
    result = mdd(device)->devTable.BindBufferMemory2(device, bindInfoCount, pBindInfos);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        for (uint32_t i = 0; i < bindInfoCount; i++) {
            DeviceMemory deviceMemory = {};
            deviceMemory.device = device;
            deviceMemory.memory = pBindInfos[i].memory;
            g_bufferToDeviceMemory[pBindInfos[i].buffer] = deviceMemory;
        }
    }
#endif
    pPacket = interpret_body_as_vkBindBufferMemory2(pHeader);
    pPacket->device = device;
    pPacket->bindInfoCount = bindInfoCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfos), bindInfoCount * sizeof(VkBindBufferMemoryInfo), pBindInfos);
    if (pBindInfos) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pBindInfos, (void*)pBindInfos);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfos));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        for (uint32_t i = 0; i < bindInfoCount; i++) {
            trim::ObjectInfo* pInfo = trim::get_Buffer_objectInfo(pBindInfos[i].buffer);
            if (pInfo != NULL) {
                vktrace_trace_packet_header* pBBMHeader = generateBindBufferMemoryPacket(device, pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
                pInfo->ObjectInfo.Buffer.pBindBufferMemoryPacket = trim::copy_packet(pBBMHeader);
                pInfo->ObjectInfo.Buffer.memory = pBindInfos[i].memory;
                pInfo->ObjectInfo.Buffer.memoryOffset = pBindInfos[i].memoryOffset;
                pInfo->ObjectInfo.Buffer.needsStagingBuffer = trim::IsMemoryDeviceOnly(pBindInfos[i].memory);
                vktrace_delete_trace_packet(&pBBMHeader);
            }
        }

        if (g_trimIsInTrim) {
            for (uint32_t i = 0; i < bindInfoCount; i++) {
                trim::mark_Buffer_reference(pBindInfos[i].buffer);
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkBindImageMemory2(
    VkDevice device,
    uint32_t bindInfoCount,
    const VkBindImageMemoryInfo* pBindInfos) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkBindImageMemory2* pPacket = NULL;
    size_t extra_size = 0;
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        extra_size += get_struct_chain_size((void*)&(pBindInfos[i]));
    }
    CREATE_TRACE_PACKET(vkBindImageMemory2, extra_size);
    result = mdd(device)->devTable.BindImageMemory2(device, bindInfoCount, pBindInfos);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        for (uint32_t i = 0; i < bindInfoCount; i++) {
            DeviceMemory deviceMemory = {};
            deviceMemory.device = device;
            deviceMemory.memory = pBindInfos[i].memory;
            g_imageToDeviceMemory[pBindInfos[i].image] = deviceMemory;
        }
    }
#endif
    pPacket = interpret_body_as_vkBindImageMemory2(pHeader);
    pPacket->device = device;
    pPacket->bindInfoCount = bindInfoCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pBindInfos), bindInfoCount * sizeof(VkBindImageMemoryInfo), pBindInfos);
    if (pBindInfos) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pBindInfos, (void*)pBindInfos);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pBindInfos));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        for (uint32_t i = 0; i < bindInfoCount; i++) {
            trim::ObjectInfo* pInfo = trim::get_Image_objectInfo(pBindInfos[i].image);
            if (pInfo != nullptr) {
                if (pInfo->ObjectInfo.Image.memorySize == 0) {
                    // trim get image memory size through target title call
                    // vkGetImageMemoryRequirements for the image, but so
                    // far the title doesn't call vkGetImageMemoryRequirements,
                    // so here we call it for the image.
                    VkMemoryRequirements MemoryRequirements;
                    mdd(device)->devTable.GetImageMemoryRequirements(device, pBindInfos[i].image, &MemoryRequirements);
                    pInfo->ObjectInfo.Image.memorySize = MemoryRequirements.size;
                }
                pInfo->ObjectInfo.Image.pBindImageMemoryPacket = trim::copy_packet(pHeader);
                pInfo->ObjectInfo.Image.memory = pBindInfos[i].memory;
                pInfo->ObjectInfo.Image.memoryOffset = pBindInfos[i].memoryOffset;
                pInfo->ObjectInfo.Image.needsStagingBuffer = pInfo->ObjectInfo.Image.needsStagingBuffer || trim::IsMemoryDeviceOnly(pBindInfos[i].memory);
            }
        }
        if (g_trimIsInTrim) {
            for (uint32_t i = 0; i < bindInfoCount; i++) {
                trim::mark_Image_reference(pBindInfos[i].image);
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkDebugMarkerSetObjectNameEXT(
    VkDevice device,
    const VkDebugMarkerObjectNameInfoEXT* pNameInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkDebugMarkerSetObjectNameEXT* pPacket = NULL;
    CREATE_TRACE_PACKET(vkDebugMarkerSetObjectNameEXT, ((pNameInfo != NULL) ? ROUNDUP_TO_4(strlen(pNameInfo->pObjectName) + 1): 0) + get_struct_chain_size((void*)pNameInfo));
    result = mdd(device)->devTable.DebugMarkerSetObjectNameEXT(device, pNameInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkDebugMarkerSetObjectNameEXT(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pNameInfo), sizeof(VkDebugMarkerObjectNameInfoEXT), pNameInfo);
    if (pNameInfo)
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pNameInfo->pObjectName), ((pNameInfo->pObjectName != NULL) ? strlen(pNameInfo->pObjectName) + 1 : 0), pNameInfo->pObjectName);
    pPacket->result = result;
    if (pNameInfo)
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pNameInfo->pObjectName));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pNameInfo));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdDebugMarkerBeginEXT(
    VkCommandBuffer commandBuffer,
    const VkDebugMarkerMarkerInfoEXT* pMarkerInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdDebugMarkerBeginEXT* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCmdDebugMarkerBeginEXT, ((pMarkerInfo != NULL) ? ROUNDUP_TO_4(strlen(pMarkerInfo->pMarkerName) + 1): 0) + get_struct_chain_size((void*)pMarkerInfo));
    mdd(commandBuffer)->devTable.CmdDebugMarkerBeginEXT(commandBuffer, pMarkerInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdDebugMarkerBeginEXT(pHeader);
    pPacket->commandBuffer = commandBuffer;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMarkerInfo), sizeof(VkDebugMarkerMarkerInfoEXT), pMarkerInfo);
    if (pMarkerInfo)
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pMarkerInfo->pMarkerName), ((pMarkerInfo->pMarkerName != NULL) ? strlen(pMarkerInfo->pMarkerName) + 1 : 0), pMarkerInfo->pMarkerName);
    if (pMarkerInfo)
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMarkerInfo->pMarkerName));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pMarkerInfo));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceSurfaceCapabilitiesKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, sizeof(VkSurfaceCapabilitiesKHR));
    result = mid(physicalDevice)->instTable.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
    pPacket = interpret_body_as_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->surface = surface;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurfaceCapabilities), sizeof(VkSurfaceCapabilitiesKHR),
                                       pSurfaceCapabilities);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurfaceCapabilities));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                                                                             VkSurfaceKHR surface,
                                                                                             uint32_t* pSurfaceFormatCount,
                                                                                             VkSurfaceFormatKHR* pSurfaceFormats) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    size_t _dataSize;
    packet_vkGetPhysicalDeviceSurfaceFormatsKHR* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    result = mid(physicalDevice)
                 ->instTable.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    endTime = vktrace_get_time();
    _dataSize = (pSurfaceFormatCount == NULL || pSurfaceFormats == NULL) ? 0 : (*pSurfaceFormatCount * sizeof(VkSurfaceFormatKHR));
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceSurfaceFormatsKHR, sizeof(uint32_t) + _dataSize);
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkGetPhysicalDeviceSurfaceFormatsKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->surface = surface;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurfaceFormatCount), sizeof(uint32_t), pSurfaceFormatCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurfaceFormats), _dataSize, pSurfaceFormats);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurfaceFormatCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurfaceFormats));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                                                                                  VkSurfaceKHR surface,
                                                                                                  uint32_t* pPresentModeCount,
                                                                                                  VkPresentModeKHR* pPresentModes) {
    static uint32_t sRealPresentModeCount = 0;
    static std::vector<VkPresentModeKHR> sRealPresentModes;
    uint32_t* pRealPresentModeCount = nullptr;
    VkPresentModeKHR* pRealPresentModes = nullptr;
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    size_t _dataSize;
    packet_vkGetPhysicalDeviceSurfacePresentModesKHR* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    if (getForceFifoEnableFlag()) {
        pRealPresentModeCount = &sRealPresentModeCount;
        if (pPresentModes) {
            sRealPresentModeCount = sRealPresentModeCount > 0? sRealPresentModeCount : *pPresentModeCount;
            sRealPresentModes.resize(sRealPresentModeCount);
            pRealPresentModes = sRealPresentModes.data();
        }
    } else {
        pRealPresentModeCount = pPresentModeCount;
        pRealPresentModes = pPresentModes;
    }
    startTime = vktrace_get_time();
    result = mid(physicalDevice)
                 ->instTable.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pRealPresentModeCount, pRealPresentModes);
    endTime = vktrace_get_time();
    if (getForceFifoEnableFlag()) {
        if (pPresentModeCount != NULL && pPresentModes == NULL) {
            *pPresentModeCount = 1;
        }
        if (pPresentModes != NULL) {
            pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
        }
    }
    _dataSize = (pPresentModeCount == NULL || pPresentModes == NULL) ? 0 : (*pPresentModeCount * sizeof(VkPresentModeKHR));
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceSurfacePresentModesKHR, sizeof(uint32_t) + _dataSize);
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkGetPhysicalDeviceSurfacePresentModesKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->surface = surface;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentModeCount), sizeof(uint32_t), pPresentModeCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentModes), _dataSize, pPresentModes);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentModeCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentModes));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateSwapchainKHR(VkDevice device,
                                                                             const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                                             const VkAllocationCallbacks* pAllocator,
                                                                             VkSwapchainKHR* pSwapchain) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateSwapchainKHR* pPacket = NULL;
    VkSwapchainCreateInfoKHR tmpCreateInfo = *pCreateInfo;
    CREATE_TRACE_PACKET(vkCreateSwapchainKHR, vk_size_vkswapchaincreateinfokhr(pCreateInfo) +
                                                  get_struct_chain_size((void*)pCreateInfo) + sizeof(VkSwapchainKHR) +
                                                  sizeof(VkAllocationCallbacks));
    if (!g_is_vkreplay_proc && tmpCreateInfo.minImageCount < 3) {
        tmpCreateInfo.minImageCount = 3;
        vktrace_LogWarning("Overwritting the minImageCount to 3 !");
    }
    result = mdd(device)->devTable.CreateSwapchainKHR(device, &tmpCreateInfo, pAllocator, pSwapchain);
    pPacket = interpret_body_as_vkCreateSwapchainKHR(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkSwapchainCreateInfoKHR), &tmpCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSwapchain), sizeof(VkSwapchainKHR), pSwapchain);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pQueueFamilyIndices),
                                       pCreateInfo->queueFamilyIndexCount * sizeof(uint32_t), pCreateInfo->pQueueFamilyIndices);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo->pQueueFamilyIndices));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSwapchain));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SwapchainKHR_object(*pSwapchain);
        info.belongsToDevice = device;
        info.ObjectInfo.SwapchainKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SwapchainKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                                uint32_t* pSwapchainImageCount,
                                                                                VkImage* pSwapchainImages) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    size_t _dataSize;
    packet_vkGetSwapchainImagesKHR* pPacket = NULL;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t vktraceStartTime = vktrace_get_time();
    startTime = vktrace_get_time();
    result = mdd(device)->devTable.GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    endTime = vktrace_get_time();
    _dataSize = (pSwapchainImageCount == NULL || pSwapchainImages == NULL) ? 0 : (*pSwapchainImageCount * sizeof(VkImage));
    CREATE_TRACE_PACKET(vkGetSwapchainImagesKHR, sizeof(uint32_t) + _dataSize);
    pHeader->vktrace_begin_time = vktraceStartTime;
    pHeader->entrypoint_begin_time = startTime;
    pHeader->entrypoint_end_time = endTime;
    pPacket = interpret_body_as_vkGetSwapchainImagesKHR(pHeader);
    pPacket->device = device;
    pPacket->swapchain = swapchain;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSwapchainImageCount), sizeof(uint32_t), pSwapchainImageCount);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSwapchainImages), _dataSize, pSwapchainImages);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSwapchainImageCount));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSwapchainImages));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo* pInfo = trim::get_SwapchainKHR_objectInfo(swapchain);
        if (pInfo != NULL) {
            if (pSwapchainImageCount != NULL && pSwapchainImages == NULL) {
                if (g_trimIsPreTrim) {
                    // only want to replay this call if it was made PRE trim frames.
                    pInfo->ObjectInfo.SwapchainKHR.pGetSwapchainImageCountPacket = trim::copy_packet(pHeader);
                }
            } else if (pSwapchainImageCount != NULL && pSwapchainImages != NULL) {
                if (g_trimIsPreTrim) {
                    // only want to replay this call if it was made PRE trim frames.
                    pInfo->ObjectInfo.SwapchainKHR.pGetSwapchainImagesPacket = trim::copy_packet(pHeader);
                }
                for (uint32_t i = 0; i < *pSwapchainImageCount; i++) {
                    trim::ObjectInfo& imageInfo = trim::add_Image_object(pSwapchainImages[i]);
                    imageInfo.ObjectInfo.Image.bIsSwapchainImage = true;
                }
            }
        }

        if (g_trimIsInTrim) {
            if ((pSwapchainImageCount != nullptr) && (pSwapchainImages != nullptr)) {
                for (uint32_t i = 0; i < *pSwapchainImageCount; i++) {
                    trim::mark_Image_reference(pSwapchainImages[i]);
                }
            }
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkQueuePresentKHR* pPacket = NULL;
    size_t swapchainSize = pPresentInfo->swapchainCount * sizeof(VkSwapchainKHR);
    size_t indexSize = pPresentInfo->swapchainCount * sizeof(uint32_t);
    size_t semaSize = pPresentInfo->waitSemaphoreCount * sizeof(VkSemaphore);
    size_t resultsSize = pPresentInfo->swapchainCount * sizeof(VkResult);
    size_t totalSize = sizeof(VkPresentInfoKHR) + get_struct_chain_size((void*)pPresentInfo) + swapchainSize + indexSize + semaSize;
    if (pPresentInfo->pResults != NULL) {
        totalSize += resultsSize;
    }
    CREATE_TRACE_PACKET(vkQueuePresentKHR, totalSize);
    result = mdd(queue)->devTable.QueuePresentKHR(queue, pPresentInfo);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkQueuePresentKHR(pHeader);
    pPacket->queue = queue;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentInfo), sizeof(VkPresentInfoKHR), pPresentInfo);
    if (pPresentInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pPresentInfo, pPresentInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentInfo->pSwapchains), swapchainSize,
                                       pPresentInfo->pSwapchains);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentInfo->pImageIndices), indexSize,
                                       pPresentInfo->pImageIndices);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentInfo->pWaitSemaphores), semaSize,
                                       pPresentInfo->pWaitSemaphores);
    if (pPresentInfo->pResults != NULL) {
        vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pPresentInfo->pResults), resultsSize,
                                           pPresentInfo->pResults);
    }
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentInfo->pImageIndices));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentInfo->pSwapchains));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentInfo->pWaitSemaphores));
    if (pPresentInfo->pResults != NULL) {
        vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentInfo->pResults));
    }
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pPresentInfo));

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        if (result == VK_SUCCESS && pPresentInfo != NULL && pPresentInfo->pWaitSemaphores != NULL) {
            for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
                trim::ObjectInfo* pInfo = trim::get_Semaphore_objectInfo(pPresentInfo->pWaitSemaphores[i]);
                if (pInfo != NULL) {
                    pInfo->ObjectInfo.Semaphore.signaledOnQueue = VK_NULL_HANDLE;
                    pInfo->ObjectInfo.Semaphore.signaledOnSwapChain = VK_NULL_HANDLE;
                }
            }
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    g_trimFrameCounter++;
    if (g_trimEnabled) {
        if (trim::is_trim_trigger_enabled(trim::enum_trim_trigger::hotKey)) {
            if (!g_trimAlreadyFinished)
            {
                if (trim::is_hotkey_trim_triggered()) {
                    if (g_trimIsInTrim) {
                        vktrace_LogAlways("Trim stopping now at frame: %" PRIu64, g_trimFrameCounter - 1);
                        trim::stop();
                    }
                    else {
                        g_trimStartFrame = g_trimFrameCounter;
                        if (g_trimEndFrame < UINT64_MAX)
                        {
                            g_trimEndFrame += g_trimStartFrame;
                        }
                        vktrace_LogAlways("Trim starting now at frame: %" PRIu64, g_trimStartFrame);
                        trim::start();
                    }
                }
                else
                {
                    // when hotkey start the trim capture, now we have two ways to
                    // stop it: press hotkey or captured frames reach user specified
                    // frame count. Here is the process of the latter one.

                    if (g_trimIsInTrim && (g_trimEndFrame < UINT64_MAX))
                    {
                        if (g_trimFrameCounter == g_trimEndFrame)
                        {
                            vktrace_LogAlways("Trim stopping now at frame: %" PRIu64, g_trimEndFrame);
                            trim::stop();
                        }
                    }
                }
            }
        } else if (trim::is_trim_trigger_enabled(trim::enum_trim_trigger::frameCounter)) {
            if (g_trimFrameCounter == g_trimStartFrame) {
                vktrace_LogAlways("Trim starting now at frame: %" PRIu64, g_trimStartFrame);
                trim::start();
            }
            if (g_trimEndFrame < UINT64_MAX && g_trimFrameCounter == g_trimEndFrame + 1) {
                vktrace_LogAlways("Trim stopping now at frame: %" PRIu64, g_trimEndFrame);
                trim::stop();
            }
        }
    }
    if (g_trimFrameCounter > getCheckHandlerFrames()) {
        disableHandlerCheck();
    }
    return result;
}

/* TODO these can probably be moved into code gen */
#if defined(VK_USE_PLATFORM_WIN32_KHR)
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                                                const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                                                const VkAllocationCallbacks* pAllocator,
                                                                                VkSurfaceKHR* pSurface) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateWin32SurfaceKHR* pPacket = NULL;
    // don't bother with copying the actual win32 hinstance, hwnd into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkCreateWin32SurfaceKHR, sizeof(VkSurfaceKHR) + sizeof(VkAllocationCallbacks) +
                                                     sizeof(VkWin32SurfaceCreateInfoKHR) +
                                                     get_struct_chain_size((void*)pCreateInfo));
    result = mid(instance)->instTable.CreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    pPacket = interpret_body_as_vkCreateWin32SurfaceKHR(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkWin32SurfaceCreateInfoKHR), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)pPacket->pCreateInfo, pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SurfaceKHR_object(*pSurface);
        info.belongsToInstance = instance;
        info.ObjectInfo.SurfaceKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SurfaceKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkBool32 VKAPI_CALL
__HOOKED_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkBool32 result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceWin32PresentationSupportKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceWin32PresentationSupportKHR, 0);
    result = mid(physicalDevice)->instTable.GetPhysicalDeviceWin32PresentationSupportKHR(physicalDevice, queueFamilyIndex);
    pPacket = interpret_body_as_vkGetPhysicalDeviceWin32PresentationSupportKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->queueFamilyIndex = queueFamilyIndex;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateXcbSurfaceKHR(VkInstance instance,
                                                                              const VkXcbSurfaceCreateInfoKHR* pCreateInfo,
                                                                              const VkAllocationCallbacks* pAllocator,
                                                                              VkSurfaceKHR* pSurface) {
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateXcbSurfaceKHR* pPacket = NULL;
    trim::set_keyboard_connection(pCreateInfo->connection);
    // don't bother with copying the actual xcb window and connection into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkCreateXcbSurfaceKHR,
                        sizeof(VkSurfaceKHR) + sizeof(VkAllocationCallbacks) + get_struct_chain_size((void*)pCreateInfo));
    result = mid(instance)->instTable.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    pPacket = interpret_body_as_vkCreateXcbSurfaceKHR(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkXcbSurfaceCreateInfoKHR), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pNext), pCreateInfo->pNext);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SurfaceKHR_object(*pSurface);
        info.belongsToInstance = instance;
        info.ObjectInfo.SurfaceKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SurfaceKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkBool32 VKAPI_CALL __HOOKED_vkGetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t* connection, xcb_visualid_t visual_id) {
    VkBool32 result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceXcbPresentationSupportKHR* pPacket = NULL;
    // don't bother with copying the actual xcb visual_id and connection into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceXcbPresentationSupportKHR, 0);
    result = mid(physicalDevice)
                 ->instTable.GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, connection, visual_id);
    pPacket = interpret_body_as_vkGetPhysicalDeviceXcbPresentationSupportKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->connection = connection;
    pPacket->queueFamilyIndex = queueFamilyIndex;
    pPacket->visual_id = visual_id;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateXlibSurfaceKHR(VkInstance instance,
                                                                               const VkXlibSurfaceCreateInfoKHR* pCreateInfo,
                                                                               const VkAllocationCallbacks* pAllocator,
                                                                               VkSurfaceKHR* pSurface) {
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateXlibSurfaceKHR* pPacket = NULL;
    // don't bother with copying the actual xlib window and connection into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkCreateXlibSurfaceKHR,
                        sizeof(VkSurfaceKHR) + sizeof(VkAllocationCallbacks) + get_struct_chain_size((void*)pCreateInfo));
    result = mid(instance)->instTable.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    pPacket = interpret_body_as_vkCreateXlibSurfaceKHR(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkXlibSurfaceCreateInfoKHR), pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pNext), pCreateInfo->pNext);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SurfaceKHR_object(*pSurface);
        info.belongsToInstance = instance;
        info.ObjectInfo.SurfaceKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SurfaceKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkBool32 VKAPI_CALL __HOOKED_vkGetPhysicalDeviceXlibPresentationSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, Display* dpy, VisualID visualID) {
    vktrace_trace_packet_header* pHeader;
    VkBool32 result;
    packet_vkGetPhysicalDeviceXlibPresentationSupportKHR* pPacket = NULL;
    // don't bother with copying the actual xlib visual_id and connection into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceXlibPresentationSupportKHR, 0);
    result =
        mid(physicalDevice)->instTable.GetPhysicalDeviceXlibPresentationSupportKHR(physicalDevice, queueFamilyIndex, dpy, visualID);
    pPacket = interpret_body_as_vkGetPhysicalDeviceXlibPresentationSupportKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->dpy = dpy;
    pPacket->queueFamilyIndex = queueFamilyIndex;
    pPacket->visualID = visualID;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

#if defined(VK_USE_PLATFORM_XLIB_XRANDR_EXT)

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkAcquireXlibDisplayEXT(
    VkPhysicalDevice physicalDevice,
    Display* dpy,
    VkDisplayKHR display)
{
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkAcquireXlibDisplayEXT* pPacket = NULL;
    CREATE_TRACE_PACKET(vkAcquireXlibDisplayEXT, sizeof(Display));
    result = mid(physicalDevice)->instTable.AcquireXlibDisplayEXT(physicalDevice, dpy, display);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkAcquireXlibDisplayEXT(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->display = display;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->dpy), sizeof(Display), dpy);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->dpy));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetRandROutputDisplayEXT(
    VkPhysicalDevice physicalDevice,
    Display* dpy,
    RROutput rrOutput,
    VkDisplayKHR* pDisplay)
{
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetRandROutputDisplayEXT* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetRandROutputDisplayEXT, sizeof(Display) + sizeof(VkDisplayKHR));
    result = mid(physicalDevice)->instTable.GetRandROutputDisplayEXT(physicalDevice, dpy, rrOutput, pDisplay);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetRandROutputDisplayEXT(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->rrOutput = rrOutput;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->dpy), sizeof(Display), dpy);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDisplay), sizeof(VkDisplayKHR), pDisplay);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->dpy));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDisplay));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
#endif
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateWaylandSurfaceKHR(VkInstance instance,
                                                                                  const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
                                                                                  const VkAllocationCallbacks* pAllocator,
                                                                                  VkSurfaceKHR* pSurface) {
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateWaylandSurfaceKHR* pPacket = NULL;
    // don't bother with copying the actual wayland window and connection into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkCreateWaylandSurfaceKHR,
                        sizeof(VkSurfaceKHR) + sizeof(VkAllocationCallbacks) + get_struct_chain_size((void*)pCreateInfo));
    result = mid(instance)->instTable.CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    pPacket = interpret_body_as_vkCreateWaylandSurfaceKHR(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkWaylandSurfaceCreateInfoKHR),
                                       pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pNext), pCreateInfo->pNext);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SurfaceKHR_object(*pSurface);
        info.belongsToInstance = instance;
        info.ObjectInfo.SurfaceKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SurfaceKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkBool32 VKAPI_CALL __HOOKED_vkGetPhysicalDeviceWaylandPresentationSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display) {
    vktrace_trace_packet_header* pHeader;
    VkBool32 result;
    packet_vkGetPhysicalDeviceWaylandPresentationSupportKHR* pPacket = NULL;
    // don't bother with copying the actual Wayland visual_id and connection into the trace packet, vkreplay has to use it's own
    // anyway
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceWaylandPresentationSupportKHR, 0);
    result =
        mid(physicalDevice)->instTable.GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, display);
    pPacket = interpret_body_as_vkGetPhysicalDeviceWaylandPresentationSupportKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->display = display;
    pPacket->queueFamilyIndex = queueFamilyIndex;
    pPacket->result = result;
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateAndroidSurfaceKHR(VkInstance instance,
                                                                                  const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
                                                                                  const VkAllocationCallbacks* pAllocator,
                                                                                  VkSurfaceKHR* pSurface) {
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateAndroidSurfaceKHR* pPacket = NULL;
    // don't bother with copying the actual native window into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkCreateAndroidSurfaceKHR,
                        sizeof(VkSurfaceKHR) + sizeof(VkAllocationCallbacks) + get_struct_chain_size((void*)pCreateInfo));
    result = mid(instance)->instTable.CreateAndroidSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    pPacket = interpret_body_as_vkCreateAndroidSurfaceKHR(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkAndroidSurfaceCreateInfoKHR),
                                       pCreateInfo);
    if (pCreateInfo) vktrace_add_pnext_structs_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pNext), pCreateInfo->pNext);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SurfaceKHR_object(*pSurface);
        info.belongsToInstance = instance;
        info.ObjectInfo.SurfaceKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SurfaceKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
#endif

#if defined(VK_USE_PLATFORM_HEADLESS_EXT)
VkResult saveCreateHeadlessSurfAsAndroidSurf(VkInstance instance,
                                             const VkHeadlessSurfaceCreateInfoEXT *pCreateInfo,
                                             const VkAllocationCallbacks *pAllocator,
                                             VkSurfaceKHR* pSurface) {
    vktrace_trace_packet_header* pHeader;
    VkResult result;
    packet_vkCreateAndroidSurfaceKHR* pPacket = NULL;
    VkAndroidSurfaceCreateInfoKHR fakedAndroidCreateInfo;

    fakedAndroidCreateInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    fakedAndroidCreateInfo.pNext = NULL;
    fakedAndroidCreateInfo.flags = 0;
    fakedAndroidCreateInfo.window = (ANativeWindow*)0xFACEFACE;

    // don't bother with copying the actual native window into the trace packet, vkreplay has to use it's own anyway
    CREATE_TRACE_PACKET(vkCreateAndroidSurfaceKHR,
                        sizeof(VkSurfaceKHR) + sizeof(VkAllocationCallbacks) + get_struct_chain_size((void*)&fakedAndroidCreateInfo));
    result = mid(instance)->instTable.CreateHeadlessSurfaceEXT(instance, pCreateInfo, pAllocator, pSurface);
    pPacket = interpret_body_as_vkCreateAndroidSurfaceKHR(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkAndroidSurfaceCreateInfoKHR),
                                       &fakedAndroidCreateInfo);
    vktrace_add_pnext_structs_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo->pNext), fakedAndroidCreateInfo.pNext);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::ObjectInfo& info = trim::add_SurfaceKHR_object(*pSurface);
        info.belongsToInstance = instance;
        info.ObjectInfo.SurfaceKHR.pCreatePacket = trim::copy_packet(pHeader);
        if (pAllocator != NULL) {
            info.ObjectInfo.SurfaceKHR.pAllocator = pAllocator;
            trim::add_Allocator(pAllocator);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateHeadlessSurfaceEXT(VkInstance instance,
                                                                                  const VkHeadlessSurfaceCreateInfoEXT *pCreateInfo,
                                                                                  const VkAllocationCallbacks *pAllocator,
                                                                                  VkSurfaceKHR* pSurface) {
    return saveCreateHeadlessSurfAsAndroidSurf(instance, pCreateInfo, pAllocator, pSurface);
}
#else
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateHeadlessSurfaceEXT(
    VkInstance instance,
    const VkHeadlessSurfaceCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateHeadlessSurfaceEXT* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCreateHeadlessSurfaceEXT, get_struct_chain_size((void*)pCreateInfo) + sizeof(VkAllocationCallbacks) + sizeof(VkSurfaceKHR));
    result = mid(instance)->instTable.CreateHeadlessSurfaceEXT(instance, pCreateInfo, pAllocator, pSurface);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCreateHeadlessSurfaceEXT(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pCreateInfo), sizeof(VkHeadlessSurfaceCreateInfoEXT), pCreateInfo);
    vktrace_add_pnext_structs_to_trace_packet(pHeader, (void *)pPacket->pCreateInfo, (void *)pCreateInfo);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurface), sizeof(VkSurfaceKHR), pSurface);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pCreateInfo));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurface));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
#endif

#if defined(VK_USE_PLATFORM_HEADLESS_ARM)
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateHeadlessSurfaceARM(VkInstance instance,
                                                                                   const VkHeadlessSurfaceCreateInfoARM *pCreateInfo,
                                                                                   const VkAllocationCallbacks *pAllocator,
                                                                                   VkSurfaceKHR *pSurface) {
    return saveCreateHeadlessSurfAsAndroidSurf(instance, (const VkHeadlessSurfaceCreateInfoEXT*)pCreateInfo, pAllocator, pSurface);
}
#endif

static std::unordered_map<VkDescriptorUpdateTemplate, VkDescriptorUpdateTemplateCreateInfo*> descriptorUpdateTemplateCreateInfo;
static vktrace_sem_id descriptorUpdateTemplateCreateInfo_sem_id;
static bool descriptorUpdateTemplateCreateInfo_success = vktrace_sem_create(&descriptorUpdateTemplateCreateInfo_sem_id, 1);

void lockDescriptorUpdateTemplateCreateInfo() {
    if (!descriptorUpdateTemplateCreateInfo_success) {
        vktrace_LogError("Semaphore create failed!");
    }
    vktrace_sem_wait(descriptorUpdateTemplateCreateInfo_sem_id);
}

void unlockDescriptorUpdateTemplateCreateInfo() { vktrace_sem_post(descriptorUpdateTemplateCreateInfo_sem_id); }

void FinalizeTrimCreateDescriptorUpdateTemplate(vktrace_trace_packet_header* pHeader, VkDevice device,
                                                const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    vktrace_finalize_trace_packet(pHeader);
    trim::ObjectInfo& info = trim::add_DescriptorUpdateTemplate_object(*pDescriptorUpdateTemplate);
    info.belongsToDevice = device;
    info.ObjectInfo.DescriptorUpdateTemplate.pCreatePacket = trim::copy_packet(pHeader);
    info.ObjectInfo.DescriptorUpdateTemplate.flags = pCreateInfo->flags;
    info.ObjectInfo.DescriptorUpdateTemplate.descriptorUpdateEntryCount = pCreateInfo->descriptorUpdateEntryCount;
    if ((pCreateInfo->descriptorUpdateEntryCount != 0) && (pCreateInfo->pDescriptorUpdateEntries != nullptr)) {
        info.ObjectInfo.DescriptorUpdateTemplate.pDescriptorUpdateEntries =
            VKTRACE_NEW_ARRAY(VkDescriptorUpdateTemplateEntry, pCreateInfo->descriptorUpdateEntryCount);
        assert(info.ObjectInfo.DescriptorUpdateTemplate.pDescriptorUpdateEntries != nullptr);
        memcpy(reinterpret_cast<void*>(
                   const_cast<VkDescriptorUpdateTemplateEntry*>(info.ObjectInfo.DescriptorUpdateTemplate.pDescriptorUpdateEntries)),
               reinterpret_cast<const void*>(const_cast<VkDescriptorUpdateTemplateEntry*>(pCreateInfo->pDescriptorUpdateEntries)),
               pCreateInfo->descriptorUpdateEntryCount * sizeof(VkDescriptorUpdateTemplateEntry));
    }
    info.ObjectInfo.DescriptorUpdateTemplate.templateType = pCreateInfo->templateType;
    info.ObjectInfo.DescriptorUpdateTemplate.descriptorSetLayout = pCreateInfo->descriptorSetLayout;
    info.ObjectInfo.DescriptorUpdateTemplate.pipelineBindPoint = pCreateInfo->pipelineBindPoint;
    info.ObjectInfo.DescriptorUpdateTemplate.pipelineLayout = pCreateInfo->pipelineLayout;
    info.ObjectInfo.DescriptorUpdateTemplate.set = pCreateInfo->set;

    if (pAllocator != NULL) {
        info.ObjectInfo.DescriptorUpdateTemplate.pAllocator = pAllocator;
        trim::add_Allocator(pAllocator);
    }

    if (g_trimIsInTrim) {
        trim::write_packet(pHeader);
    } else {
        vktrace_delete_trace_packet(&pHeader);
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateDescriptorUpdateTemplate* pPacket = NULL;

    CREATE_TRACE_PACKET(
        vkCreateDescriptorUpdateTemplate,
        get_struct_chain_size(reinterpret_cast<void*>(const_cast<VkDescriptorUpdateTemplateCreateInfo*>(pCreateInfo))) +
            sizeof(VkAllocationCallbacks) + sizeof(VkDescriptorUpdateTemplate) +
            sizeof(VkDescriptorUpdateTemplateEntry) * pCreateInfo->descriptorUpdateEntryCount);
    result = mdd(device)->devTable.CreateDescriptorUpdateTemplate(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    vktrace_set_packet_entrypoint_end_time(pHeader);

    lockDescriptorUpdateTemplateCreateInfo();
    descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate] =
        reinterpret_cast<VkDescriptorUpdateTemplateCreateInfo*>(malloc(sizeof(VkDescriptorUpdateTemplateCreateInfo)));
    memcpy(descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate], pCreateInfo,
           sizeof(VkDescriptorUpdateTemplateCreateInfo));
    descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate]->pDescriptorUpdateEntries =
        reinterpret_cast<VkDescriptorUpdateTemplateEntry*>(
            malloc(sizeof(VkDescriptorUpdateTemplateEntry) * pCreateInfo->descriptorUpdateEntryCount));
    memcpy(reinterpret_cast<void*>(const_cast<VkDescriptorUpdateTemplateEntry*>(
               descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate]->pDescriptorUpdateEntries)),
           pCreateInfo->pDescriptorUpdateEntries,
           sizeof(VkDescriptorUpdateTemplateEntry) * pCreateInfo->descriptorUpdateEntryCount);
    unlockDescriptorUpdateTemplateCreateInfo();

    pPacket = interpret_body_as_vkCreateDescriptorUpdateTemplate(pHeader);
    pPacket->device = device;

    vktrace_add_buffer_to_trace_packet(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateCreateInfo**>(&(pPacket->pCreateInfo))),
        sizeof(VkDescriptorUpdateTemplateCreateInfo), pCreateInfo);

    if (nullptr != pCreateInfo) {
        vktrace_add_pnext_structs_to_trace_packet(
            pHeader, reinterpret_cast<void*>(const_cast<VkDescriptorUpdateTemplateCreateInfo*>(pPacket->pCreateInfo)), pCreateInfo);
    }

    vktrace_add_buffer_to_trace_packet(
        pHeader,
        reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateEntry**>(&(pPacket->pCreateInfo->pDescriptorUpdateEntries))),
        sizeof(VkDescriptorUpdateTemplateEntry) * pCreateInfo->descriptorUpdateEntryCount, pCreateInfo->pDescriptorUpdateEntries);
    vktrace_add_buffer_to_trace_packet(pHeader,
                                       reinterpret_cast<void**>(const_cast<VkAllocationCallbacks**>(&(pPacket->pAllocator))),
                                       sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplate**>(&(pPacket->pDescriptorUpdateTemplate))),
        sizeof(VkDescriptorUpdateTemplate), pDescriptorUpdateTemplate);
    pPacket->result = result;
    vktrace_finalize_buffer_address(
        pHeader,
        reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateEntry**>(&(pPacket->pCreateInfo->pDescriptorUpdateEntries))));
    vktrace_finalize_buffer_address(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateCreateInfo**>(&(pPacket->pCreateInfo))));
    vktrace_finalize_buffer_address(pHeader, reinterpret_cast<void**>(const_cast<VkAllocationCallbacks**>(&(pPacket->pAllocator))));
    vktrace_finalize_buffer_address(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplate**>(&(pPacket->pDescriptorUpdateTemplate))));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        FinalizeTrimCreateDescriptorUpdateTemplate(pHeader, device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    }

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkCreateDescriptorUpdateTemplateKHR(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplateKHR* pDescriptorUpdateTemplate) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkCreateDescriptorUpdateTemplateKHR* pPacket = NULL;

    CREATE_TRACE_PACKET(
        vkCreateDescriptorUpdateTemplateKHR,
        get_struct_chain_size(reinterpret_cast<void*>(const_cast<VkDescriptorUpdateTemplateCreateInfoKHR*>(pCreateInfo))) +
            sizeof(VkAllocationCallbacks) + sizeof(VkDescriptorUpdateTemplateKHR) +
            sizeof(VkDescriptorUpdateTemplateEntryKHR) * pCreateInfo->descriptorUpdateEntryCount);
    result = mdd(device)->devTable.CreateDescriptorUpdateTemplateKHR(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    vktrace_set_packet_entrypoint_end_time(pHeader);

    lockDescriptorUpdateTemplateCreateInfo();
    descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate] =
        reinterpret_cast<VkDescriptorUpdateTemplateCreateInfoKHR*>(malloc(sizeof(VkDescriptorUpdateTemplateCreateInfoKHR)));
    memcpy(descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate], pCreateInfo,
           sizeof(VkDescriptorUpdateTemplateCreateInfoKHR));
    descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate]->pDescriptorUpdateEntries =
        reinterpret_cast<VkDescriptorUpdateTemplateEntryKHR*>(
            malloc(sizeof(VkDescriptorUpdateTemplateEntryKHR) * pCreateInfo->descriptorUpdateEntryCount));
    memcpy(reinterpret_cast<void*>(const_cast<VkDescriptorUpdateTemplateEntry*>(
               descriptorUpdateTemplateCreateInfo[*pDescriptorUpdateTemplate]->pDescriptorUpdateEntries)),
           pCreateInfo->pDescriptorUpdateEntries,
           sizeof(VkDescriptorUpdateTemplateEntryKHR) * pCreateInfo->descriptorUpdateEntryCount);
    unlockDescriptorUpdateTemplateCreateInfo();

    pPacket = interpret_body_as_vkCreateDescriptorUpdateTemplateKHR(pHeader);
    pPacket->device = device;

    vktrace_add_buffer_to_trace_packet(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateCreateInfo**>(&(pPacket->pCreateInfo))),
        sizeof(VkDescriptorUpdateTemplateCreateInfoKHR), pCreateInfo);
    if (nullptr != pCreateInfo) {
        vktrace_add_pnext_structs_to_trace_packet(
            pHeader, reinterpret_cast<void*>(const_cast<VkDescriptorUpdateTemplateCreateInfo*>(pPacket->pCreateInfo)), pCreateInfo);
    }

    vktrace_add_buffer_to_trace_packet(
        pHeader,
        reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateEntry**>(&(pPacket->pCreateInfo->pDescriptorUpdateEntries))),
        sizeof(VkDescriptorUpdateTemplateEntryKHR) * pCreateInfo->descriptorUpdateEntryCount,
        pCreateInfo->pDescriptorUpdateEntries);
    vktrace_add_buffer_to_trace_packet(pHeader,
                                       reinterpret_cast<void**>(const_cast<VkAllocationCallbacks**>(&(pPacket->pAllocator))),
                                       sizeof(VkAllocationCallbacks), NULL);
    vktrace_add_buffer_to_trace_packet(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplate**>(&(pPacket->pDescriptorUpdateTemplate))),
        sizeof(VkDescriptorUpdateTemplateKHR), pDescriptorUpdateTemplate);
    pPacket->result = result;
    vktrace_finalize_buffer_address(
        pHeader,
        reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateEntry**>(&(pPacket->pCreateInfo->pDescriptorUpdateEntries))));
    vktrace_finalize_buffer_address(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplateCreateInfo**>(&(pPacket->pCreateInfo))));
    vktrace_finalize_buffer_address(pHeader, reinterpret_cast<void**>(const_cast<VkAllocationCallbacks**>(&(pPacket->pAllocator))));
    vktrace_finalize_buffer_address(
        pHeader, reinterpret_cast<void**>(const_cast<VkDescriptorUpdateTemplate**>(&(pPacket->pDescriptorUpdateTemplate))));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        FinalizeTrimCreateDescriptorUpdateTemplate(pHeader, device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    }

    return result;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkDestroyDescriptorUpdateTemplate* pPacket = NULL;
    CREATE_TRACE_PACKET(vkDestroyDescriptorUpdateTemplate, sizeof(VkAllocationCallbacks));
    mdd(device)->devTable.DestroyDescriptorUpdateTemplate(device, descriptorUpdateTemplate, pAllocator);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkDestroyDescriptorUpdateTemplate(pHeader);
    pPacket->device = device;
    pPacket->descriptorUpdateTemplate = descriptorUpdateTemplate;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::remove_DescriptorUpdateTemplate_object(descriptorUpdateTemplate);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    lockDescriptorUpdateTemplateCreateInfo();
    if (descriptorUpdateTemplateCreateInfo.find(descriptorUpdateTemplate) != descriptorUpdateTemplateCreateInfo.end()) {
        if (descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]) {
            if (descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries)
                free((void*)descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries);
            free(descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]);
        }
        descriptorUpdateTemplateCreateInfo.erase(descriptorUpdateTemplate);
    }
    unlockDescriptorUpdateTemplateCreateInfo();
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkDestroyDescriptorUpdateTemplateKHR(
    VkDevice device, VkDescriptorUpdateTemplateKHR descriptorUpdateTemplate, const VkAllocationCallbacks* pAllocator) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkDestroyDescriptorUpdateTemplateKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkDestroyDescriptorUpdateTemplateKHR, sizeof(VkAllocationCallbacks));
    mdd(device)->devTable.DestroyDescriptorUpdateTemplateKHR(device, descriptorUpdateTemplate, pAllocator);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkDestroyDescriptorUpdateTemplateKHR(pHeader);
    pPacket->device = device;
    pPacket->descriptorUpdateTemplate = descriptorUpdateTemplate;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pAllocator), sizeof(VkAllocationCallbacks), NULL);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pAllocator));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::remove_DescriptorUpdateTemplate_object(descriptorUpdateTemplate);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    lockDescriptorUpdateTemplateCreateInfo();
    if (descriptorUpdateTemplateCreateInfo.find(descriptorUpdateTemplate) != descriptorUpdateTemplateCreateInfo.end()) {
        if (descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]) {
            if (descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries)
                free((void*)descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries);
            free(descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]);
        }
        descriptorUpdateTemplateCreateInfo.erase(descriptorUpdateTemplate);
    }
    unlockDescriptorUpdateTemplateCreateInfo();
}

static size_t getDescriptorSetDataSize(VkDescriptorUpdateTemplate descriptorUpdateTemplate) {
    size_t dataSize = 0;
    lockDescriptorUpdateTemplateCreateInfo();
    for (uint32_t i = 0; i < descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->descriptorUpdateEntryCount; i++) {
        for (uint32_t j = 0;
             j < descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].descriptorCount; j++) {
            size_t thisSize = 0;
            switch (descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    thisSize =
                        descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].offset +
                        j * descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].stride +
                        sizeof(VkDescriptorImageInfo);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    thisSize =
                        descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].offset +
                        j * descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].stride +
                        sizeof(VkDescriptorBufferInfo);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    thisSize =
                        descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].offset +
                        j * descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].stride +
                        sizeof(VkBufferView);
                    break;
                case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    thisSize =
                        descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].offset +
                        j * descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i].stride +
                        sizeof(VkWriteDescriptorSetAccelerationStructureKHR);
                    break;
                default:
                    assert(0);
                    break;
            }
            dataSize = std::max(dataSize, thisSize);
        }
    }
    unlockDescriptorUpdateTemplateCreateInfo();
    return dataSize;
}

void FinalizeTrimUpdateDescriptorSetWithTemplate(vktrace_trace_packet_header* pHeader, VkDescriptorSet descriptorSet,
                                                 VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData) {
    vktrace_finalize_trace_packet(pHeader);
    lockDescriptorUpdateTemplateCreateInfo();

    // Trim keep tracking all descriptorsets from beginning, include all
    // descriptors in the descriptorset. If any function change any
    // descriptor of it, the track info of that descriptorset should
    // also be changed. The following source code update the descriptorset
    // trackinfo to make sure the track info reflect current descriptorset
    // state after the function update its descriptors.
    //
    if (VK_NULL_HANDLE != descriptorUpdateTemplate) {
        for (uint32_t i = 0; i < descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->descriptorUpdateEntryCount; i++) {
            const VkDescriptorUpdateTemplateEntry* pDescriptorUpdateEntry =
                &(descriptorUpdateTemplateCreateInfo[descriptorUpdateTemplate]->pDescriptorUpdateEntries[i]);
            // The following variables are used to locate the descriptor
            // track info in the target descriptor set, so we can use the
            // descriptor data in template to update it.
            // For every input descriptor data, we need to find
            // corresponding binding number and array element index.
            VkWriteDescriptorSet* pWriteDescriptorSet =
                nullptr;  // This is the pointer to trim tracking info of binding in target descriptorset.
            uint32_t bindingDescriptorInfoArrayWriteIndex = 0;   // This is the array index of current descriptor
                                                                 // (within the binding array) that we'll update.
            uint32_t bindingDescriptorInfoArrayWriteLength = 0;  // The descriptor amount in the binding array.

            uint32_t bindingIndex = 0;  // the array index of the current binding in the descriptorset track info.

            trim::ObjectInfo* pInfo = trim::get_DescriptorSet_objectInfo(descriptorSet);

            // Get the binding index from the binding number which is the
            // updating target.
            //
            // Note: by Doc, Vulkan allows the descriptor bindings to be
            // specified sparsely so we cannot assume the binding index
            // is the binding number.
            bindingIndex = get_binding_index(descriptorSet, pDescriptorUpdateEntry->dstBinding);
            assert(bindingIndex != INVALID_BINDING_INDEX);

            pWriteDescriptorSet = &pInfo->ObjectInfo.DescriptorSet.pWriteDescriptorSets[bindingIndex];
            bindingDescriptorInfoArrayWriteIndex = pDescriptorUpdateEntry->dstArrayElement;
            bindingDescriptorInfoArrayWriteLength = pWriteDescriptorSet->descriptorCount;
            if (!(bindingDescriptorInfoArrayWriteIndex < bindingDescriptorInfoArrayWriteLength))
                assert(bindingDescriptorInfoArrayWriteIndex < bindingDescriptorInfoArrayWriteLength);

            uint32_t j = 0;
            for (trim::DescriptorIterator descriptor_iterator(pInfo, bindingIndex, bindingDescriptorInfoArrayWriteIndex,
                                                              pDescriptorUpdateEntry->descriptorCount);
                 !descriptor_iterator.IsEnd(); descriptor_iterator++, j++) {
                // update writeDescriptorCount accordingly with the number of binding
                // the descriptorset has been updated by chekcing the binding index.
                if (descriptor_iterator.GetCurrentBindingIndex() >= pInfo->ObjectInfo.DescriptorSet.writeDescriptorCount) {
                    pInfo->ObjectInfo.DescriptorSet.writeDescriptorCount = descriptor_iterator.GetCurrentBindingIndex() + 1;
                }

                // First get the descriptor data pointer, Doc provide the
                // following formula to calculate the pointer for every
                // array element:
                const char* pDescriptorRawData =
                    reinterpret_cast<const char*>(pData) + pDescriptorUpdateEntry->offset + j * pDescriptorUpdateEntry->stride;

                // The following code update the descriptorset track info
                // with the descriptor data.

                switch (pDescriptorUpdateEntry->descriptorType) {
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                        memcpy(reinterpret_cast<void*>(&(*descriptor_iterator)), pDescriptorRawData, sizeof(VkDescriptorImageInfo));
                        break;
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                        memcpy(reinterpret_cast<void*>(&(*descriptor_iterator)), pDescriptorRawData,
                               sizeof(VkDescriptorBufferInfo));
                        break;
                    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                        memcpy(reinterpret_cast<void*>(&(*descriptor_iterator)), pDescriptorRawData, sizeof(VkBufferView));
                        break;
                    default:
                        assert(0);
                        break;
                }
            }
        }
    }

    unlockDescriptorUpdateTemplateCreateInfo();
    if (g_trimIsInTrim) {
        trim::mark_DescriptorSet_reference(descriptorSet);
        trim::write_packet(pHeader);
    } else {
        vktrace_delete_trace_packet(&pHeader);
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void* pData) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkUpdateDescriptorSetWithTemplate* pPacket = NULL;
    size_t dataSize;

    // TODO: We're saving all the data, from pData to the end of the last item, including data before offset and skipped data.
    // This could be optimized to save only the data chunks that are actually needed.
    dataSize = getDescriptorSetDataSize(descriptorUpdateTemplate);

    CREATE_TRACE_PACKET(vkUpdateDescriptorSetWithTemplate, dataSize);
    mdd(device)->devTable.UpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate, pData);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkUpdateDescriptorSetWithTemplate(pHeader);
    pPacket->device = device;
    pPacket->descriptorSet = descriptorSet;
    pPacket->descriptorUpdateTemplate = descriptorUpdateTemplate;
    vktrace_add_buffer_to_trace_packet(pHeader, const_cast<void**>(&(pPacket->pData)), dataSize, pData);
    vktrace_finalize_buffer_address(pHeader, const_cast<void**>(&(pPacket->pData)));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        FinalizeTrimUpdateDescriptorSetWithTemplate(pHeader, descriptorSet, descriptorUpdateTemplate, pData);
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkUpdateDescriptorSetWithTemplateKHR(
    VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplateKHR descriptorUpdateTemplate, const void* pData) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkUpdateDescriptorSetWithTemplateKHR* pPacket = NULL;
    size_t dataSize;

    // TODO: We're saving all the data, from pData to the end of the last item, including data before offset and skipped data.
    // This could be optimized to save only the data chunks that are actually needed.
    dataSize = getDescriptorSetDataSize(descriptorUpdateTemplate);

    CREATE_TRACE_PACKET(vkUpdateDescriptorSetWithTemplateKHR, dataSize);
    mdd(device)->devTable.UpdateDescriptorSetWithTemplateKHR(device, descriptorSet, descriptorUpdateTemplate, pData);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkUpdateDescriptorSetWithTemplateKHR(pHeader);
    pPacket->device = device;
    pPacket->descriptorSet = descriptorSet;
    pPacket->descriptorUpdateTemplate = descriptorUpdateTemplate;
    vktrace_add_buffer_to_trace_packet(pHeader, const_cast<void**>(&(pPacket->pData)), dataSize, pData);
    vktrace_finalize_buffer_address(pHeader, const_cast<void**>(&(pPacket->pData)));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        FinalizeTrimUpdateDescriptorSetWithTemplate(pHeader, descriptorSet, descriptorUpdateTemplate, pData);
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer,
                                                                              VkPipelineBindPoint pipelineBindPoint,
                                                                              VkPipelineLayout layout, uint32_t set,
                                                                              uint32_t descriptorWriteCount,
                                                                              const VkWriteDescriptorSet* pDescriptorWrites) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdPushDescriptorSetKHR* pPacket = NULL;
    size_t arrayByteCount = 0;

    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        arrayByteCount += get_struct_chain_size(&pDescriptorWrites[i]);
    }

    CREATE_TRACE_PACKET(vkCmdPushDescriptorSetKHR, arrayByteCount);
    mdd(commandBuffer)
        ->devTable.CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdPushDescriptorSetKHR(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->pipelineBindPoint = pipelineBindPoint;
    pPacket->layout = layout;
    pPacket->set = set;
    pPacket->descriptorWriteCount = descriptorWriteCount;

    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites),
                                       descriptorWriteCount * sizeof(VkWriteDescriptorSet), pDescriptorWrites);
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        switch (pPacket->pDescriptorWrites[i].descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pImageInfo),
                                                   pDescriptorWrites[i].descriptorCount * sizeof(VkDescriptorImageInfo),
                                                   pDescriptorWrites[i].pImageInfo);
                vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pImageInfo));
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pTexelBufferView),
                                                   pDescriptorWrites[i].descriptorCount * sizeof(VkBufferView),
                                                   pDescriptorWrites[i].pTexelBufferView);
                vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pTexelBufferView));
            } break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pBufferInfo),
                                                   pDescriptorWrites[i].descriptorCount * sizeof(VkDescriptorBufferInfo),
                                                   pDescriptorWrites[i].pBufferInfo);
                vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites[i].pBufferInfo));
            } break;
            default:
                break;
        }
        vktrace_add_pnext_structs_to_trace_packet(pHeader, (void*)(pPacket->pDescriptorWrites + i), pDescriptorWrites + i);
    }

    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pDescriptorWrites));

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            if (g_trimIsInTrim) {
                trim::mark_DescriptorSet_reference(pDescriptorWrites[i].dstSet);
                for (uint32_t j = 0; j < pDescriptorWrites[i].descriptorCount; j++) {
                    switch (pDescriptorWrites[i].descriptorType) {
                        case VK_DESCRIPTOR_TYPE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                            trim::mark_ImageView_reference(pDescriptorWrites[i].pImageInfo[j].imageView);
                        } break;
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                            trim::mark_Buffer_reference(pDescriptorWrites[i].pBufferInfo[j].buffer);
                        } break;
                        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                            trim::mark_BufferView_reference(pDescriptorWrites[i].pTexelBufferView[j]);
                        } break;
                        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
                            assert(pDescriptorWrites[i].pNext != nullptr);
                            VkWriteDescriptorSetAccelerationStructureKHR *pWriteDSAS = (VkWriteDescriptorSetAccelerationStructureKHR*)(pDescriptorWrites[i].pNext);
                            for(int asi = 0; asi < pWriteDSAS->accelerationStructureCount; asi++) {
                                trim::mark_AccelerationStructure_reference(pWriteDSAS->pAccelerationStructures[asi]);
                            }
                        } break;
                        default:
                            break;
                    }
                }
            }
            // TODO: add more process to handle the following case: the title
            //       recording commands cross multiple frames and there are
            //       one or more vkCmdPushDescriptorSetKHR recorded into
            //       the command buffer, and the trim starting frame is one of
            //       them. We need adding more process and move some of
            //       the process into vkQueueSubmit.
            handleWriteDescriptorSet(&pDescriptorWrites[i]);
        }

        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdPushDescriptorSetWithTemplateKHR(
    VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplateKHR descriptorUpdateTemplate, VkPipelineLayout layout, uint32_t set,
    const void* pData) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdPushDescriptorSetWithTemplateKHR* pPacket = NULL;
    size_t dataSize;

    // TODO: We're saving all the data, from pData to the end of the last item, including data before offset and skipped data.
    // This could be optimized to save only the data chunks that are actually needed.
    dataSize = getDescriptorSetDataSize(descriptorUpdateTemplate);

    CREATE_TRACE_PACKET(vkCmdPushDescriptorSetWithTemplateKHR, dataSize);
    mdd(commandBuffer)->devTable.CmdPushDescriptorSetWithTemplateKHR(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkCmdPushDescriptorSetWithTemplateKHR(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->descriptorUpdateTemplate = descriptorUpdateTemplate;
    pPacket->layout = layout;
    pPacket->set = set;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pData), dataSize, pData);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pData));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                                           VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                                                                           uint32_t regionCount,
                                                                           const VkBufferImageCopy* pRegions) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdCopyImageToBuffer* pPacket = NULL;
    CREATE_TRACE_PACKET(vkCmdCopyImageToBuffer, regionCount * sizeof(VkBufferImageCopy));
    mdd(commandBuffer)->devTable.CmdCopyImageToBuffer(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        g_cmdBufferToBuffers[commandBuffer].push_back(dstBuffer);
    }
#endif
    pPacket = interpret_body_as_vkCmdCopyImageToBuffer(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->srcImage = srcImage;
    pPacket->srcImageLayout = srcImageLayout;
    pPacket->dstBuffer = dstBuffer;
    pPacket->regionCount = regionCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRegions), regionCount * sizeof(VkBufferImageCopy), pRegions);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRegions));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        if (g_trimIsInTrim) {
            trim::mark_Buffer_reference(dstBuffer);
            trim::mark_Image_reference(srcImage);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkWaitForFences(VkDevice device, uint32_t fenceCount,
                                                                        const VkFence* pFences, VkBool32 waitAll,
                                                                        uint64_t timeout) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkWaitForFences* pPacket = NULL;
    CREATE_TRACE_PACKET(vkWaitForFences, fenceCount * sizeof(VkFence));
    result = mdd(device)->devTable.WaitForFences(device, fenceCount, pFences, waitAll, timeout);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    if (!UseMappedExternalHostMemoryExtension()) {
    }
    pPacket = interpret_body_as_vkWaitForFences(pHeader);
    pPacket->device = device;
    pPacket->fenceCount = fenceCount;
    pPacket->waitAll = waitAll;
    pPacket->timeout = timeout;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pFences), fenceCount * sizeof(VkFence), pFences);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pFences));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}

// TODO Wayland support

/* TODO: Probably want to make this manual to get the result of the boolean and then check it on replay
VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    const VkSurfaceDescriptionKHR* pSurfaceDescription,
    VkBool32* pSupported)
{
    VkResult result;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetPhysicalDeviceSurfaceSupportKHR* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetPhysicalDeviceSurfaceSupportKHR, sizeof(VkSurfaceDescriptionKHR) + sizeof(VkBool32));
    result = mid(physicalDevice)->instTable.GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex,
pSurfaceDescription, pSupported);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetPhysicalDeviceSurfaceSupportKHR(pHeader);
    pPacket->physicalDevice = physicalDevice;
    pPacket->queueFamilyIndex = queueFamilyIndex;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSurfaceDescription), sizeof(VkSurfaceDescriptionKHR),
pSurfaceDescription);
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pSupported), sizeof(VkBool32), pSupported);
    pPacket->result = result;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSurfaceDescription));
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pSupported));
    if (!g_trimEnabled)
    {
        FINISH_TRACE_PACKET();
    }
    else
    {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim)
        {
            trim::write_packet(pHeader);
        }
        else
        {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return result;
}
*/

/**
 * Want trace packets created for GetDeviceProcAddr that is app initiated
 * but not for loader initiated calls to GDPA. Thus need two versions of GDPA.
 */
VKTRACER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vktraceGetDeviceProcAddr(VkDevice device, const char* funcName) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    PFN_vkVoidFunction addr;

    vktrace_trace_packet_header* pHeader;
    packet_vkGetDeviceProcAddr* pPacket = NULL;
    CREATE_TRACE_PACKET(vkGetDeviceProcAddr, ((funcName != NULL) ? ROUNDUP_TO_4(strlen(funcName) + 1) : 0));
    addr = __HOOKED_vkGetDeviceProcAddr(device, funcName);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetDeviceProcAddr(pHeader);
    pPacket->device = device;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pName), ((funcName != NULL) ? strlen(funcName) + 1 : 0),
                                       funcName);
    pPacket->result = addr;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pName));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
    return addr;
}

/* GDPA with no trace packet creation */
VKTRACER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __HOOKED_vkGetDeviceProcAddr(VkDevice device, const char* funcName) {
    if (!strcmp("vkGetDeviceProcAddr", funcName)) {
        if (gMessageStream != NULL) {
            return (PFN_vkVoidFunction)vktraceGetDeviceProcAddr;
        } else {
            return (PFN_vkVoidFunction)__HOOKED_vkGetDeviceProcAddr;
        }
    }

    layer_device_data* devData = mdd(device);
    if (gMessageStream != NULL) {
        PFN_vkVoidFunction addr;
        addr = layer_intercept_proc(funcName);
        if (addr) return addr;

        if (devData->KHRDeviceSwapchainEnabled) {
            if (!strcmp("vkCreateSwapchainKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateSwapchainKHR;
            if (!strcmp("vkDestroySwapchainKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkDestroySwapchainKHR;
            if (!strcmp("vkGetSwapchainImagesKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkGetSwapchainImagesKHR;
            if (!strcmp("vkAcquireNextImageKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkAcquireNextImageKHR;
            if (!strcmp("vkQueuePresentKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkQueuePresentKHR;
        }
    }

    if (device == VK_NULL_HANDLE) {
        return NULL;
    }
    vktrace_LogError("Function %s is not in Device", funcName);

    VkLayerDispatchTable* pDisp = &devData->devTable;
    if (pDisp->GetDeviceProcAddr == NULL) return NULL;
    return pDisp->GetDeviceProcAddr(device, funcName);
}

/**
 * Want trace packets created for GetInstanceProcAddr that is app initiated
 * but not for loader initiated calls to GIPA. Thus need two versions of GIPA.
 */
VKTRACER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vktraceGetInstanceProcAddr(VkInstance instance, const char* funcName) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    PFN_vkVoidFunction addr;
    vktrace_trace_packet_header* pHeader;
    packet_vkGetInstanceProcAddr* pPacket = NULL;
    // assert(strcmp("vkGetInstanceProcAddr", funcName));
    CREATE_TRACE_PACKET(vkGetInstanceProcAddr, ((funcName != NULL) ? ROUNDUP_TO_4(strlen(funcName) + 1) : 0));
    addr = __HOOKED_vkGetInstanceProcAddr(instance, funcName);
    vktrace_set_packet_entrypoint_end_time(pHeader);
    pPacket = interpret_body_as_vkGetInstanceProcAddr(pHeader);
    pPacket->instance = instance;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pName), ((funcName != NULL) ? strlen(funcName) + 1 : 0),
                                       funcName);
    pPacket->result = addr;
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pName));
    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        if (g_trimIsInTrim) {
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }

    return addr;
}

bool getCmdCopyBufferRemapASEnableFlag() {
    static bool EnableAs = false;
    static bool FirstTimeRun = true;
    if (FirstTimeRun) {
        FirstTimeRun = false;
        const char* env_remap_as_enable = vktrace_get_global_var(VKTRACE_ENABLE_VKCMDCOPYBUFFER_REMAP_AS_ENV);
        if (env_remap_as_enable) {
            int envvalue;
            if (sscanf(env_remap_as_enable, "%d", &envvalue) == 1) {
                if (envvalue != 0) {
                    EnableAs = true;
                }
            }
        }
    }
    return EnableAs;
}

bool getCmdCopyBufferRemapBufferEnableFlag() {
    static bool EnableBuffer = false;
    static bool FirstTimeRun = true;
    if (FirstTimeRun) {
        FirstTimeRun = false;
        const char* env_remap_buffer_enable = vktrace_get_global_var(VKTRACE_ENABLE_VKCMDCOPYBUFFER_REMAP_BUFFER_ENV);
        if (env_remap_buffer_enable) {
            int envvalue;
            if (sscanf(env_remap_buffer_enable, "%d", &envvalue) == 1) {
                if (envvalue != 0) {
                    EnableBuffer = true;
                }
            }
        }
    }
    return EnableBuffer;
}

VKTRACER_EXPORT VKAPI_ATTR void VKAPI_CALL __HOOKED_vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferCopy* pRegions) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    vktrace_trace_packet_header* pHeader;
    packet_vkCmdCopyBuffer* pPacket = NULL;

#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardEnter();
    flushAllChangedMappedMemory(&vkFlushMappedMemoryRangesWithoutAPICall);
    if (!UseMappedExternalHostMemoryExtension()) {
        // If enable external host memory extension, there will be no shadow
        // memory, so we don't need any read pageguard handling.
        resetAllReadFlagAndPageGuard();
    }
#endif

    CREATE_TRACE_PACKET(vkCmdCopyBuffer, regionCount * sizeof(VkBufferCopy));
    mdd(commandBuffer)->devTable.CmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
    vktrace_set_packet_entrypoint_end_time(pHeader);
#if defined(USE_PAGEGUARD_SPEEDUP) && !defined(PAGEGUARD_ADD_PAGEGUARD_ON_REAL_MAPPED_MEMORY)
    if (!UseMappedExternalHostMemoryExtension()) {
        g_cmdBufferToBuffers[commandBuffer].push_back(dstBuffer);
    }
#endif
    pPacket = interpret_body_as_vkCmdCopyBuffer(pHeader);
    pPacket->commandBuffer = commandBuffer;
    pPacket->srcBuffer = srcBuffer;
    pPacket->dstBuffer = dstBuffer;
    pPacket->regionCount = regionCount;
    vktrace_add_buffer_to_trace_packet(pHeader, (void**)&(pPacket->pRegions), regionCount * sizeof(VkBufferCopy), pRegions);
    vktrace_finalize_buffer_address(pHeader, (void**)&(pPacket->pRegions));

    bool enableRemapAs = getCmdCopyBufferRemapASEnableFlag();
    bool enableRemapBuffer = getCmdCopyBufferRemapBufferEnableFlag();
    if (enableRemapAs || enableRemapBuffer) {
        bool asFound = false;
        bool bufferFound = false;
        if (g_shaderDeviceAddrBufferToMem.find(dstBuffer) != g_shaderDeviceAddrBufferToMem.end()) {
            VkDevice device = VK_NULL_HANDLE;
            auto it = g_commandBufferToDevice.find(commandBuffer);
            if (it == g_commandBufferToDevice.end()) {
                vktrace_LogError("Error detected in CmdCopyBuffer(), couldn't be able to find corresponding VkDevice for VkCommandBuffer 0x%llx.", commandBuffer);
                return;
            }
            device = it->second;

            VkDeviceMemory mem = VK_NULL_HANDLE;
            auto it2 = g_bufferToDeviceMemory.find(srcBuffer);
            if (it2 == g_bufferToDeviceMemory.end()) {
                vktrace_LogError("Error detected in __HOOKED_vkCmdCopyBuffer(), couldn't be able to find corresponding VkDeviceMemory for VkBuffer 0x%llx.", srcBuffer);
                return;
            }
            mem = it2->second.memory;

            for (int i = 0; i < regionCount; ++i) {
                VkDeviceSize srcOffset = pRegions[i].srcOffset;
                VkDeviceSize size = pRegions[i].size;
                VkAccelerationStructureInstanceKHR* pAsInstance = nullptr;
                VkResult mapResult = mdd(device)->devTable.MapMemory(device, mem, srcOffset, size, 0, (void**)&pAsInstance);
                if (mapResult == VK_SUCCESS) {
                    // remap all VkAccelerationStructureInstanceKHRs from trace values to replay values
                    if (enableRemapAs) {
                        for (int j = 0; j < size / sizeof(VkAccelerationStructureInstanceKHR); ++j) {
                            auto it = g_AStoDeviceAddrRev.find(pAsInstance[j].accelerationStructureReference);
                            if (it != g_AStoDeviceAddrRev.end()) {
                                asFound = true;
                                break;
                            }
                        }
                    }
                    // remap all VkDeviceAddress from trace values to replay values
                    if (enableRemapBuffer) {
                        VkDeviceAddress *pDeviceAddress = (VkDeviceAddress *)pAsInstance;
                        for (int j = 0; j < size / sizeof(VkDeviceAddress); ++j) {
                            auto it = g_BuftoDeviceAddrRev.find(pDeviceAddress[j]);
                            if (it != g_BuftoDeviceAddrRev.end()) {
                                bufferFound = true;
                                break;
                            }
                        }
                    }
                    mdd(device)->devTable.UnmapMemory(device, mem);
                }
            }
        }

        if (asFound && bufferFound) {
            vktrace_LogDebug("Created a VKTRACE_TPI_VK_vkCmdCopyBufferRemapASandBuffer");
            pPacket->header->packet_id = VKTRACE_TPI_VK_vkCmdCopyBufferRemapASandBuffer;
        }
        else if (asFound) {
            vktrace_LogDebug("Created a VKTRACE_TPI_VK_vkCmdCopyBufferRemapAS");
            pPacket->header->packet_id = VKTRACE_TPI_VK_vkCmdCopyBufferRemapAS;
        }
        else if (bufferFound) {
            vktrace_LogDebug("Created a VKTRACE_TPI_VK_vkCmdCopyBufferRemapBuffer");
            pPacket->header->packet_id = VKTRACE_TPI_VK_vkCmdCopyBufferRemapBuffer;
        }
    }

    if (!g_trimEnabled) {
        FINISH_TRACE_PACKET();
    } else {
        vktrace_finalize_trace_packet(pHeader);
        trim::add_CommandBuffer_call(commandBuffer, trim::copy_packet(pHeader));
        if (g_trimIsInTrim) {
            trim::mark_Buffer_reference(srcBuffer);
            trim::mark_Buffer_reference(dstBuffer);
            trim::write_packet(pHeader);
        } else {
            vktrace_delete_trace_packet(&pHeader);
        }
    }
#if defined(USE_PAGEGUARD_SPEEDUP)
    pageguardExit();
#endif
}

/* GIPA with no trace packet creation */
VKTRACER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __HOOKED_vkGetInstanceProcAddr(VkInstance instance, const char* funcName) {
    PFN_vkVoidFunction addr;
    layer_instance_data* instData;

    vktrace_platform_thread_once((void*)&gInitOnce, InitTracer);
    if (!strcmp("vkGetInstanceProcAddr", funcName)) {
        if (gMessageStream != NULL) {
            return (PFN_vkVoidFunction)vktraceGetInstanceProcAddr;
        } else {
            return (PFN_vkVoidFunction)__HOOKED_vkGetInstanceProcAddr;
        }
    }

    if (gMessageStream != NULL) {
        addr = layer_intercept_instance_proc(funcName);
        if (addr) return addr;

        addr = layer_intercept_proc(funcName);
        if (addr) return addr;

        if (instance == VK_NULL_HANDLE) {
            return NULL;
        }

        instData = mid(instance);
        if (instData->LunargDebugReportEnabled) {
            if (!strcmp("vkCreateDebugReportCallbackEXT", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkCreateDebugReportCallbackEXT;
            if (!strcmp("vkDestroyDebugReportCallbackEXT", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkDestroyDebugReportCallbackEXT;
        }
        if (instData->KHRSurfaceEnabled) {
            if (!strcmp("vkGetPhysicalDeviceSurfaceSupportKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceSurfaceSupportKHR;
            if (!strcmp("vkDestroySurfaceKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkDestroySurfaceKHR;
            if (!strcmp("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
            if (!strcmp("vkGetPhysicalDeviceSurfaceFormatsKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceSurfaceFormatsKHR;
            if (!strcmp("vkGetPhysicalDeviceSurfacePresentModesKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceSurfacePresentModesKHR;
        }
#if defined(VK_USE_PLATFORM_XLIB_KHR)
        if (instData->KHRXlibSurfaceEnabled) {
            if (!strcmp("vkCreateXlibSurfaceKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateXlibSurfaceKHR;
            if (!strcmp("vkGetPhysicalDeviceXlibPresentationSupportKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceXlibPresentationSupportKHR;
        }
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
        if (instData->KHRXcbSurfaceEnabled) {
            if (!strcmp("vkCreateXcbSurfaceKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateXcbSurfaceKHR;
            if (!strcmp("vkGetPhysicalDeviceXcbPresentationSupportKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceXcbPresentationSupportKHR;
        }
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        if (instData->KHRWaylandSurfaceEnabled) {
            if (!strcmp("vkCreateWaylandSurfaceKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateWaylandSurfaceKHR;
            if (!strcmp("vkGetPhysicalDeviceWaylandPresentationSupportKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceWaylandPresentationSupportKHR;
        }
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (instData->KHRWin32SurfaceEnabled) {
            if (!strcmp("vkCreateWin32SurfaceKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateWin32SurfaceKHR;
            if (!strcmp("vkGetPhysicalDeviceWin32PresentationSupportKHR", funcName))
                return (PFN_vkVoidFunction)__HOOKED_vkGetPhysicalDeviceWin32PresentationSupportKHR;
        }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (instData->KHRAndroidSurfaceEnabled) {
            if (!strcmp("vkCreateAndroidSurfaceKHR", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateAndroidSurfaceKHR;
        }
#endif
#if defined(VK_USE_PLATFORM_HEADLESS_ARM)
        if (instData->KHRHeadlessSurfaceEnabled) {
            if (!strcmp("vkCreateHeadlessSurfaceARM", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateHeadlessSurfaceARM;
        }
#endif
#if defined(VK_USE_PLATFORM_HEADLESS_EXT)
        if (instData->KHRHeadlessSurfaceEnabled) {
            if (!strcmp("vkCreateHeadlessSurfaceEXT", funcName)) return (PFN_vkVoidFunction)__HOOKED_vkCreateHeadlessSurfaceEXT;
        }
#endif
    } else {
        if (instance == VK_NULL_HANDLE) {
            return NULL;
        }
        instData = mid(instance);
    }
    vktrace_LogError("Function %s is not in Instance", funcName);

    VkLayerInstanceDispatchTable* pTable = &instData->instTable;
    if (pTable->GetInstanceProcAddr == NULL) return NULL;

    return pTable->GetInstanceProcAddr(instance, funcName);
}

static const VkLayerProperties layerProps = {
    "VK_LAYER_LUNARG_vktrace", VK_MAKE_VERSION(1, 2, VK_HEADER_VERSION), 1, "LunarG tracing layer",
};

template <typename T>
VkResult EnumerateProperties(uint32_t src_count, const T* src_props, uint32_t* dst_count, T* dst_props) {
    if (!dst_props || !src_props) {
        *dst_count = src_count;
        return VK_SUCCESS;
    }

    uint32_t copy_count = (*dst_count < src_count) ? *dst_count : src_count;
    memcpy(dst_props, src_props, sizeof(T) * copy_count);
    *dst_count = copy_count;

    return (copy_count == src_count) ? VK_SUCCESS : VK_INCOMPLETE;
}

// LoaderLayerInterface V0
// https://github.com/KhronosGroup/Vulkan-Loader/blob/master/loader/LoaderAndLayerInterface.md

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                                                  VkLayerProperties* pProperties) {
    return EnumerateProperties(1, &layerProps, pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                                                                      uint32_t* pPropertyCount,
                                                                                      VkExtensionProperties* pProperties) {
    if (pLayerName && !strcmp(pLayerName, layerProps.layerName))
        return EnumerateProperties(0, (VkExtensionProperties*)nullptr, pPropertyCount, pProperties);

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                                                                               uint32_t* pPropertyCount,
                                                                                               VkExtensionProperties* pProperties) {
    return vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

VKTRACER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL __HOOKED_vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                                                           VkLayerProperties* pProperties) {
    return vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                                                uint32_t* pPropertyCount,
                                                                                VkLayerProperties* pProperties) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    return EnumerateProperties(1, &layerProps, pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(const char* pLayerName,
                                                                                    uint32_t* pPropertyCount,
                                                                                    VkExtensionProperties* pProperties) {
    if (pLayerName && !strcmp(pLayerName, layerProps.layerName))
        return EnumerateProperties(0, (VkExtensionProperties*)nullptr, pPropertyCount, pProperties);

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_LUNARG_vktraceGetInstanceProcAddr(VkInstance instance,
                                                                                                    const char* funcName) {
    return __HOOKED_vkGetInstanceProcAddr(instance, funcName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_LUNARG_vktraceGetDeviceProcAddr(VkDevice device,
                                                                                                  const char* funcName) {
    trim::TraceLock<std::mutex> lock(g_mutex_trace);
    return __HOOKED_vkGetDeviceProcAddr(device, funcName);
}
