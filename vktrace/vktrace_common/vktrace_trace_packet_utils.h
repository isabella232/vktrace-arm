/**************************************************************************
 *
 * Copyright 2014-2016 Valve Corporation
 * Copyright (C) 2014-2016 LunarG, Inc.
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
 * Author: Peter Lohrmann <peterl@valvesoftware.com>
 **************************************************************************/
#pragma once

#include "vktrace_multiplatform.h"
#include "vktrace_trace_packet_identifiers.h"
#include "vktrace_filelike.h"
#include "vktrace_memory.h"
#include "vktrace_process.h"

#if defined(__cplusplus)
extern "C" {
#endif

// pUuid is expected to be an array of 4 unsigned ints
void vktrace_gen_uuid(uint32_t* pUuid);

#define NANOSEC_IN_ONE_SEC 1000000000
uint64_t vktrace_get_time();

void vktrace_initialize_trace_packet_utils();
void vktrace_deinitialize_trace_packet_utils();

uint64_t get_endianess();
const char* get_endianess_string(uint64_t endianess);
uint64_t get_arch();
uint64_t get_os();

char* find_available_filename(const char* originalFilename, BOOL bForceOverwrite);
deviceFeatureSupport query_device_feature(PFN_vkGetPhysicalDeviceFeatures2KHR GetPhysicalDeviceFeatures, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo);
static FILE* vktrace_open_trace_file(vktrace_process_capture_trace_thread_info* trace_thread_info) {
    FILE* tracefp = NULL;
    assert(trace_thread_info != NULL);

    // open trace file
    if (trace_thread_info->pProcessInfo->traceFileCriticalSectionCreated)
    {
        char* process_trace_file_name = find_available_filename(trace_thread_info->pProcessInfo->traceFilename, true);
        assert(process_trace_file_name != NULL);
        tracefp = fopen(process_trace_file_name, "w+b");
        vktrace_free(process_trace_file_name);
    } else {
        tracefp = fopen(trace_thread_info->pProcessInfo->traceFilename, "w+b");
    }

    if (tracefp == NULL) {
        vktrace_LogError("Cannot open trace file for writing %s.", trace_thread_info->pProcessInfo->traceFilename);
        return tracefp;
    } else {
        vktrace_LogDebug("Creating trace file: '%s'", trace_thread_info->pProcessInfo->traceFilename);
    }

    // create critical section
    if (!trace_thread_info->pProcessInfo->traceFileCriticalSectionCreated) {
        vktrace_create_critical_section(&trace_thread_info->pProcessInfo->traceFileCriticalSection);
        trace_thread_info->pProcessInfo->traceFileCriticalSectionCreated = TRUE;
    }

    return tracefp;
};

typedef enum packet_tag {
    PACKET_TAG__INJECTED = 0x1,
    PACKET_TAG_ASCAPTUREREPLAY = 0x2,
    PACKET_TAG_BUFFERCAPTUREREPLAY = 0x4
} packet_tag;

enum TripleValue
{
    YES,
    NO,
    NO_VALUE
};

enum ASGeometryData {
    AS_GEOMETRY_TRIANGLES_VERTEXDATA_BIT = 0x1,
    AS_GEOMETRY_TRIANGLES_INDEXDATA_BIT = 0x2,
    AS_GEOMETRY_TRIANGLES_TRANSFORMDATA_BIT = 0x4,
    AS_GEOMETRY_AABB_DATA_BIT = 0x8,
};

int getVertexIndexStride(VkIndexType type);

//=============================================================================
// trace packets
// There is a trace_packet_header before every trace_packet_body.
// Additional buffers will come after the trace_packet_body.

//=============================================================================
// Methods for creating, populating, and writing trace packets

// \param packet_size should include the total bytes for the specific type of packet, and any additional buffers needed by the
// packet.
//        The size of the header will be added automatically within the function.
vktrace_trace_packet_header* vktrace_create_trace_packet(uint8_t tracer_id, uint16_t packet_id, uint64_t packet_size,
                                                         uint64_t additional_buffers_size);

// deletes a trace packet and sets pointer to NULL, this function should be used on a packet created to write to trace file
void vktrace_delete_trace_packet(vktrace_trace_packet_header** ppHeader);

// gets the next address available to write a buffer into the packet
void* vktrace_trace_packet_get_new_buffer_address(vktrace_trace_packet_header* pHeader, uint64_t byteCount);

// copies buffer data into a trace packet at the specified offset (from the end of the header).
// it is up to the caller to ensure that buffers do not overlap.
void vktrace_add_buffer_to_trace_packet(vktrace_trace_packet_header* pHeader, void** ptr_address, uint64_t size,
                                        const void* pBuffer);

// adds pNext structures to a trace packet
void vktrace_add_pnext_structs_to_trace_packet(vktrace_trace_packet_header* pHeader, void* pOut, const void* pIn);

// converts buffer pointers into byte offset so that pointer can be interpretted after being read into memory
void vktrace_finalize_buffer_address(vktrace_trace_packet_header* pHeader, void** ptr_address);

// sets entrypoint end time
void vktrace_set_packet_entrypoint_end_time(vktrace_trace_packet_header* pHeader);

// void initialize_trace_packet_header(vktrace_trace_packet_header* pHeader, uint8_t tracer_id, uint16_t packet_id, uint64_t
// total_packet_size);
void vktrace_finalize_trace_packet(vktrace_trace_packet_header* pHeader);

// Write the trace packet to the filelike thing.
// This has no knowledge of the details of the packet other than its size.
void vktrace_write_trace_packet(const vktrace_trace_packet_header* pHeader, FileLike* pFile);

// Tag the trace packet
void vktrace_tag_trace_packet(vktrace_trace_packet_header* pHeader, uint32_t tag);

//=============================================================================
// Methods for Reading and interpretting trace packets

// Reads in the trace packet header, the body of the packet, and additional buffers
vktrace_trace_packet_header* vktrace_read_trace_packet(FileLike* pFile);

// Get the trace packet tag
uint32_t vktrace_get_trace_packet_tag(const vktrace_trace_packet_header* pHeader);

// deletes a trace packet and sets pointer to NULL, this function should be used on a packet read from trace file
void vktrace_delete_trace_packet_no_lock(vktrace_trace_packet_header** ppHeader);

// converts a pointer variable that is currently byte offset into a pointer to the actual offset location
void* vktrace_trace_packet_interpret_buffer_pointer(vktrace_trace_packet_header* pHeader, intptr_t ptr_variable);

// Adding to packets TODO: Move to codegen
void add_VkApplicationInfo_to_packet(vktrace_trace_packet_header* pHeader, VkApplicationInfo** ppStruct, const VkApplicationInfo* pInStruct);
void add_VkDebugUtilsLabelEXT_to_packet(vktrace_trace_packet_header* pHeader, VkDebugUtilsLabelEXT** ppStruct, const VkDebugUtilsLabelEXT* pInStruct);
void add_VkAccelerationStructureBuildGeometryInfoKHR_to_packet(vktrace_trace_packet_header* pHeader, VkAccelerationStructureBuildGeometryInfoKHR** ppStruct,
                                                               VkAccelerationStructureBuildGeometryInfoKHR* pInStruct, bool addSelf, const VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfos,
                                                               bool hostAddr, char* geometryDataBit);
void add_VkInstanceCreateInfo_to_packet(vktrace_trace_packet_header* pHeader, VkInstanceCreateInfo** ppStruct, VkInstanceCreateInfo* pInStruct);
void add_VkDeviceCreateInfo_to_packet(vktrace_trace_packet_header* pHeader, VkDeviceCreateInfo** ppStruct, const VkDeviceCreateInfo* pInStruct);

void add_VkRayTracingPipelineCreateInfoKHR_to_packet(vktrace_trace_packet_header* pHeader, VkRayTracingPipelineCreateInfoKHR** ppStruct, VkRayTracingPipelineCreateInfoKHR* pInStruct);
VkRayTracingPipelineCreateInfoKHR* interpret_VkRayTracingPipelineCreateInfoKHR(vktrace_trace_packet_header* pHeader, intptr_t ptr_variable);

BOOL vktrace_append_portabilitytable(uint16_t packet_id);
// Interpreting packets TODO: Move to codegen
VkAccelerationStructureBuildGeometryInfoKHR* interpret_VkAccelerationStructureBuildGeometryInfoKHR(vktrace_trace_packet_header* pHeader, intptr_t ptr_variable, bool hostAddress);
VkDebugUtilsLabelEXT* interpret_VkDebugUtilsLabelEXT(vktrace_trace_packet_header* pHeader, intptr_t ptr_variable);
VkInstanceCreateInfo* interpret_VkInstanceCreateInfo(vktrace_trace_packet_header* pHeader, intptr_t ptr_variable);
VkDeviceCreateInfo* interpret_VkDeviceCreateInfo(vktrace_trace_packet_header* pHeader, intptr_t ptr_variable);
void interpret_VkPipelineShaderStageCreateInfo(vktrace_trace_packet_header* pHeader, VkPipelineShaderStageCreateInfo* pShader);
VkDeviceGroupDeviceCreateInfo* interpret_VkDeviceGroupDeviceCreateInfoKHX(vktrace_trace_packet_header* pHeader,
                                                                          intptr_t ptr_variable);
// converts the Vulkan struct pnext chain that is currently byte offsets into pointers
void vkreplay_interpret_pnext_pointers(vktrace_trace_packet_header* pHeader, void* struct_ptr);
//=============================================================================
// trace packet message
// Interpretting a trace_packet_message should be done only when:
// 1) a trace_packet is first created and most of the contents are empty.
// 2) immediately after the packet was read from memory
// All other conversions of the trace packet body from the header should
// be performed using a C-style cast.
static vktrace_trace_packet_message* vktrace_interpret_body_as_trace_packet_message(vktrace_trace_packet_header* pHeader) {
    vktrace_trace_packet_message* pPacket = (vktrace_trace_packet_message*)pHeader->pBody;
    // update pointers
    pPacket->pHeader = pHeader;
    pPacket->message = (char*)vktrace_trace_packet_interpret_buffer_pointer(pHeader, (intptr_t)pPacket->message);
    return pPacket;
}

#if defined(__cplusplus)
}
#endif
