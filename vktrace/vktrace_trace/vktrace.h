/**************************************************************************
 *
 * Copyright 2014-2016 Valve Corporation
 * Copyright (C) 2014-2016 LunarG, Inc.
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
 **************************************************************************/
#pragma once

extern "C" {
#include "vktrace_settings.h"
}

#include <vector>
#include <unordered_map>
#include "vktrace_trace_packet_identifiers.h"

#if defined(WIN32)
#define VKTRACE_WM_COMPLETE (WM_USER + 0)
#endif

//----------------------------------------------------------------------------------------------------------------------
// globals
//----------------------------------------------------------------------------------------------------------------------
typedef struct vktrace_settings {
    const char* program;
    const char* arguments;
    const char* working_dir;
    char* output_trace;
    BOOL print_trace_messages;
    const char* screenshotList;
    const char* screenshotColorFormat;
    BOOL enable_pmb;
    const char* verbosity;
    const char* traceTrigger;
    BOOL enable_trim_post_processing;
    BOOL enable_trace_lock;
    const char* trimCmdBatchSizeStr;
    const char* compressType;
    unsigned int compressThreshold;
} vktrace_settings;

extern vktrace_settings g_settings;
extern uint32_t lastPacketThreadId;
extern uint64_t lastPacketIndex;
extern uint64_t lastPacketEndTime;

void vktrace_appendPortabilityPacket(FILE* pTraceFile, std::vector<uint64_t>& portabilityTable);
uint32_t vktrace_appendMetaData(FILE* pTraceFile, const std::vector<uint64_t>& injectedData, uint64_t &meta_data_offset);
uint32_t vktrace_appendDeviceFeatures(FILE* pTraceFile, const std::unordered_map<VkDevice, uint32_t>& deviceToFeatures, uint64_t meta_data_offset);
void vktrace_resetFilesize(FILE* pTraceFile, uint64_t decompressFilesize);