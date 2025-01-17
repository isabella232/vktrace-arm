/**************************************************************************
 *
 * Copyright 2014-2016 Valve Corporation
 * Copyright (C) 2014-2016 LunarG, Inc.
 * Copyright (C) 2019 ARM Limited.
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
#include "vktrace.h"

#include "vktrace_process.h"

extern "C" {
#include "vktrace_common.h"
#include "vktrace_filelike.h"
#include "vktrace_interconnect.h"
#include "vktrace_trace_packet_identifiers.h"
#include "vktrace_trace_packet_utils.h"
}

#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <json/json.h>

#include "screenshot_parsing.h"

vktrace_settings g_settings;
vktrace_settings g_default_settings;

vktrace_SettingInfo g_settings_info[] = {
    // common command options
    {"p", "Program", VKTRACE_SETTING_STRING, {&g_settings.program}, {&g_default_settings.program}, TRUE, "The program to trace."},
    {"a",
     "Arguments",
     VKTRACE_SETTING_STRING,
     {&g_settings.arguments},
     {&g_default_settings.arguments},
     TRUE,
     "Command line arguments to pass to trace program."},
    {"w",
     "WorkingDir",
     VKTRACE_SETTING_STRING,
     {&g_settings.working_dir},
     {&g_default_settings.working_dir},
     TRUE,
     "The program's working directory."},
    {"o",
     "OutputTrace",
     VKTRACE_SETTING_STRING,
     {&g_settings.output_trace},
     {&g_default_settings.output_trace},
     TRUE,
     "Path to the generated output trace file."},
    {"s",
     "ScreenShot",
     VKTRACE_SETTING_STRING,
     {&g_settings.screenshotList},
     {&g_default_settings.screenshotList},
     TRUE,
     "Generate screenshots. <string> is one of:\n\
                                         comma separated list of frames\n\
                                         <start>-<count>-<interval>\n\
                                         \"all\""},
    {"sf",
     "ScreenshotFormat",
     VKTRACE_SETTING_STRING,
     {&g_settings.screenshotColorFormat},
     {&g_default_settings.screenshotColorFormat},
     TRUE,
     "Color Space format of screenshot files. Formats are UNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB"},
    {"ptm",
     "PrintTraceMessages",
     VKTRACE_SETTING_BOOL,
     {&g_settings.print_trace_messages},
     {&g_default_settings.print_trace_messages},
     TRUE,
     "Print trace messages to vktrace console."},
    {"P",
     "PMB",
     VKTRACE_SETTING_BOOL,
     {&g_settings.enable_pmb},
     {&g_default_settings.enable_pmb},
     TRUE,
     "Enable tracking of persistently mapped buffers, default is TRUE."},
#if defined(_DEBUG)
    {"v",
     "Verbosity",
     VKTRACE_SETTING_STRING,
     {&g_settings.verbosity},
     {&g_default_settings.verbosity},
     TRUE,
     "Verbosity mode. Modes are \"quiet\", \"errors\", \"warnings\", \"full\", "
     "\"max\", \"debug\"."},
#else
    {"v",
     "Verbosity",
     VKTRACE_SETTING_STRING,
     {&g_settings.verbosity},
     {&g_default_settings.verbosity},
     TRUE,
     "Verbosity mode. Modes are \"quiet\", \"errors\", \"warnings\", "
     "\"full\", \"max\"."},
#endif

    {"tr",
     "TraceTrigger",
     VKTRACE_SETTING_STRING,
     {&g_settings.traceTrigger},
     {&g_default_settings.traceTrigger},
     TRUE,
     "Start/stop trim by hotkey or frame range:\n\
                                         hotkey-[F1-F12|TAB|CONTROL]\n\
                                         hotkey-[F1-F12|TAB|CONTROL]-<frameCount>\n\
                                         frames-<startFrame>-<endFrame>"},
    {"tpp",
     "TrimPostProcessing",
     VKTRACE_SETTING_BOOL,
     {&g_settings.enable_trim_post_processing},
     {&g_default_settings.enable_trim_post_processing},
     TRUE,
     "Enable trim post processing to make trimmed trace file smaller, default is FALSE."},
    //{ "z", "pauze", VKTRACE_SETTING_BOOL, &g_settings.pause,
    //&g_default_settings.pause, TRUE, "Wait for a key at startup (so a debugger
    // can be attached)" },
    {"tbs",
     "TrimBatchSize",
     VKTRACE_SETTING_STRING,
     {&g_settings.trimCmdBatchSizeStr},
     {&g_default_settings.trimCmdBatchSizeStr},
     TRUE,
     "Set the maximum trim commands batch size, default is device allocation limit count divide by 100."},
    {"tl",
     "TraceLock",
     VKTRACE_SETTING_BOOL,
     {&g_settings.enable_trace_lock},
     {&g_default_settings.enable_trace_lock},
     TRUE,
     "Enable locking of API calls during trace if TraceLock is set to TRUE,\n\
                                       default is FALSE in which it is enabled only when trimming is enabled."},
    {"ct",
     "CompressType",
     VKTRACE_SETTING_STRING,
     {&g_settings.compressType},
     {&g_default_settings.compressType},
     TRUE,
     "The compression library type. no, lz4 and snappy are supported for now.\n\
                                        no for no compression and lz4 is the default value."},
    {"cth",
     "CompressThreshhold",
     VKTRACE_SETTING_UINT,
     {&g_settings.compressThreshold},
     {&g_default_settings.compressThreshold},
     TRUE,
     "The compression threashold size. The package would be compressed only if they are larger than this value.\n\
                                        Default value is 1024(1KB)."},
};

vktrace_SettingGroup g_settingGroup = {"vktrace", sizeof(g_settings_info) / sizeof(g_settings_info[0]), &g_settings_info[0]};

// ------------------------------------------------------------------------------------------------
#if defined(WIN32)
uint64_t MessageLoop() {
    MSG msg = {0};
    bool quit = false;
    while (!quit) {
        if (GetMessage(&msg, NULL, 0, 0) == FALSE) {
            quit = true;
        } else {
            quit = (msg.message == VKTRACE_WM_COMPLETE);
        }
    }
    return msg.wParam;
}
#endif

int PrepareTracers(vktrace_process_capture_trace_thread_info** ppTracerInfo) {
    unsigned int num_tracers = 32;

    assert(ppTracerInfo != NULL && *ppTracerInfo == NULL);
    *ppTracerInfo = VKTRACE_NEW_ARRAY(vktrace_process_capture_trace_thread_info, num_tracers);
    memset(*ppTracerInfo, 0, sizeof(vktrace_process_capture_trace_thread_info) * num_tracers);

    // we only support Vulkan tracer
    (*ppTracerInfo)[0].tracerId = VKTRACE_TID_VULKAN;

    return num_tracers;
}

bool InjectTracersIntoProcess(vktrace_process_info* pInfo) {
    bool bRecordingThreadsCreated = true;
    vktrace_thread tracingThread;
    if (vktrace_platform_remote_load_library(pInfo->hProcess, NULL, &tracingThread, NULL)) {
        // prepare data for capture threads
        pInfo->pCaptureThreads[0].pProcessInfo = pInfo;
        pInfo->pCaptureThreads[0].recordingThread = VKTRACE_NULL_THREAD;

        // create thread to record trace packets from the tracer
        pInfo->pCaptureThreads[0].recordingThread =
            vktrace_platform_create_thread(Process_RunRecordTraceThread, &(pInfo->pCaptureThreads[0]));
        if (pInfo->pCaptureThreads[0].recordingThread == VKTRACE_NULL_THREAD) {
            vktrace_LogError("Failed to create trace recording thread.");
            bRecordingThreadsCreated = false;
        }

    } else {
        // failed to inject a DLL
        bRecordingThreadsCreated = false;
    }
    return bRecordingThreadsCreated;
}

void loggingCallback(VktraceLogLevel level, const char* pMessage) {
    if (level == VKTRACE_LOG_NONE) return;

    switch (level) {
        case VKTRACE_LOG_DEBUG:
            printf("vktrace debug: %s\n", pMessage);
            break;
        case VKTRACE_LOG_ERROR:
            printf("vktrace error: %s\n", pMessage);
            break;
        case VKTRACE_LOG_WARNING:
            printf("vktrace warning: %s\n", pMessage);
            break;
        case VKTRACE_LOG_VERBOSE:
            printf("vktrace info: %s\n", pMessage);
            break;
        default:
            printf("%s\n", pMessage);
            break;
    }
    fflush(stdout);

#if defined(WIN32)
#if defined(_DEBUG)
    OutputDebugString(pMessage);
#endif
#endif
}

uint32_t lastPacketThreadId;
uint64_t lastPacketIndex;
uint64_t lastPacketEndTime;

void vktrace_appendPortabilityPacket(FILE* pTraceFile, std::vector<uint64_t>& portabilityTable) {
    vktrace_trace_packet_header hdr;
    uint64_t one_64 = 1;

    if (pTraceFile == NULL) {
        vktrace_LogError("tracefile was not created");
        return;
    }

    vktrace_LogVerbose("Post processing trace file");

    // Add a word containing the size of the table to the table.
    // This will be the last word in the file.
    portabilityTable.push_back(portabilityTable.size());

    // Append the table packet to the trace file.
    hdr.size = sizeof(hdr) + portabilityTable.size() * sizeof(uint64_t);
    hdr.global_packet_index = lastPacketIndex + 1;
    hdr.tracer_id = VKTRACE_TID_VULKAN;
    hdr.packet_id = VKTRACE_TPI_PORTABILITY_TABLE;
    hdr.thread_id = lastPacketThreadId;
    hdr.vktrace_begin_time = hdr.entrypoint_begin_time = hdr.entrypoint_end_time = hdr.vktrace_end_time = lastPacketEndTime;
    hdr.next_buffers_offset = 0;
    hdr.pBody = (uintptr_t)NULL;
    if (0 == Fseek(pTraceFile, 0, SEEK_END) && 1 == fwrite(&hdr, sizeof(hdr), 1, pTraceFile) &&
        portabilityTable.size() == fwrite(&portabilityTable[0], sizeof(uint64_t), portabilityTable.size(), pTraceFile)) {
        // Set the flag in the file header that indicates the portability table has been written
        if (0 == Fseek(pTraceFile, offsetof(vktrace_trace_file_header, portability_table_valid), SEEK_SET))
            fwrite(&one_64, sizeof(uint64_t), 1, pTraceFile);
    }
    portabilityTable.clear();
    vktrace_LogVerbose("Post processing of trace file completed");
}

void vktrace_resetFilesize(FILE* pTraceFile, uint64_t decompressFilesize) {
    if (0 == Fseek(pTraceFile, offsetof(vktrace_trace_file_header, decompress_file_size), SEEK_SET)) {
        fwrite(&decompressFilesize, sizeof(uint64_t), 1, pTraceFile);
    }
}

uint32_t vktrace_appendMetaData(FILE* pTraceFile, const std::vector<uint64_t>& injectedData, uint64_t &meta_data_offset) {
    Json::Value root;
    Json::Value injectedCallList;
    for (uint32_t i = 0; i < injectedData.size(); i++) {
        injectedCallList.append(injectedData[i]);
    }
    root["injectedCalls"] = injectedCallList;
    auto str = root.toStyledString();
    vktrace_LogVerbose("Meta data string: %s", str.c_str());

    vktrace_trace_packet_header hdr;
    uint64_t meta_data_file_offset = 0;
    uint32_t meta_data_size = str.size() + 1;
    meta_data_size = ROUNDUP_TO_8(meta_data_size);
    char *meta_data_str_json = new char[meta_data_size];
    memset(meta_data_str_json, 0, meta_data_size);
    strcpy(meta_data_str_json, str.c_str());

    hdr.size = sizeof(hdr) + meta_data_size;
    hdr.global_packet_index = lastPacketIndex++;
    hdr.tracer_id = VKTRACE_TID_VULKAN;
    hdr.packet_id = VKTRACE_TPI_META_DATA;
    hdr.thread_id = lastPacketThreadId;
    hdr.vktrace_begin_time = hdr.entrypoint_begin_time = hdr.entrypoint_end_time = hdr.vktrace_end_time = lastPacketEndTime;
    hdr.next_buffers_offset = 0;
    hdr.pBody = (uintptr_t)NULL;

    if (0 == Fseek(pTraceFile, 0, SEEK_END)) {
        meta_data_file_offset = Ftell(pTraceFile);
        if (1 == fwrite(&hdr, sizeof(hdr), 1, pTraceFile) &&
            meta_data_size == fwrite(meta_data_str_json, sizeof(char), meta_data_size, pTraceFile)) {
            if (0 == Fseek(pTraceFile, offsetof(vktrace_trace_file_header, meta_data_offset), SEEK_SET)) {
                fwrite(&meta_data_file_offset, sizeof(uint64_t), 1, pTraceFile);
                vktrace_LogVerbose("Meta data at the file offset %llu", meta_data_file_offset);
            }
        }
        meta_data_offset = meta_data_file_offset;
    } else {
        vktrace_LogError("File operation failed during append the meta data");
    }
    delete[] meta_data_str_json;
    return meta_data_size;
}

uint32_t vktrace_appendDeviceFeatures(FILE* pTraceFile, const std::unordered_map<VkDevice, uint32_t>& deviceToFeatures, uint64_t meta_data_offset) {
    /**************************************************************
     * JSON format:
     * "deviceFeatures" : {
     *      "device" : [
     *          {  // device 0
     *              "accelerationStructureCaptureReplay" : 1,
     *              "bufferDeviceAddressCaptureReplay" : 0,
     *              "deviceHandle" : "0xaaaaaaaa"
     *          }
     *          {  // device 1
     *              "accelerationStructureCaptureReplay" : 1,
     *              "bufferDeviceAddressCaptureReplay" : 1,
     *              "deviceHandle" : "0xbbbbbbbb"
     *          }
     *      ]
     * }
     * ***********************************************************/
    Json::Reader reader;
    Json::Value metaRoot;
    Json::Value featuresRoot;
    char device[16] = {0};
    for (auto e : deviceToFeatures) {
        char deviceHandle[16] = {0};
        sprintf(deviceHandle, "%p", e.first);
        Json::Value features;
        features["deviceHandle"] = Json::Value(deviceHandle);
        features["accelerationStructureCaptureReplay"] = Json::Value((e.second & PACKET_TAG_ASCAPTUREREPLAY) ? 1 : 0);
        features["bufferDeviceAddressCaptureReplay"] = Json::Value((e.second & PACKET_TAG_BUFFERCAPTUREREPLAY) ? 1 : 0);
        featuresRoot["device"].append(features);
    }
    FileLike fileLike = {.mMode = FileLike::File, .mFile = pTraceFile};
    vktrace_trace_packet_header hdr = {0};
    uint32_t device_features_string_size = 0;
    if (vktrace_FileLike_SetCurrentPosition(&fileLike, meta_data_offset)
        && vktrace_FileLike_ReadRaw(&fileLike, &hdr, sizeof(hdr))
        && hdr.packet_id == VKTRACE_TPI_META_DATA) {
        uint64_t meta_data_json_str_size = hdr.size - sizeof(hdr);
        char* meta_data_json_str = new char[meta_data_json_str_size];
        memset(meta_data_json_str, 0, meta_data_json_str_size);
        if (!meta_data_json_str || !vktrace_FileLike_ReadRaw(&fileLike, meta_data_json_str, meta_data_json_str_size)) {
            vktrace_LogError("Reading meta data of the original file failed");
            return 0;
        }
        reader.parse(meta_data_json_str, metaRoot);
        metaRoot["deviceFeatures"] = featuresRoot;
        delete[] meta_data_json_str;
        auto metaStr = metaRoot.toStyledString();
        hdr.size = sizeof(hdr) + metaStr.size();
        device_features_string_size = metaStr.size() - meta_data_json_str_size;
        vktrace_FileLike_SetCurrentPosition(&fileLike, meta_data_offset);
        if (1 == fwrite(&hdr, sizeof(hdr), 1, pTraceFile)) {
            fwrite(metaStr.c_str(), sizeof(char), metaStr.size(), pTraceFile);
        }
        auto featureStr = featuresRoot.toStyledString();
        vktrace_LogVerbose("Device features: %s", featureStr.c_str());
    } else {
        vktrace_LogAlways("Dump device features failed");
        return 0;
    }

    return device_features_string_size;
}

// ------------------------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    int exitval = 0;
    memset(&g_settings, 0, sizeof(vktrace_settings));

    vktrace_LogSetCallback(loggingCallback);
    vktrace_LogSetLevel(VKTRACE_LOG_ERROR);

    // setup defaults
    memset(&g_default_settings, 0, sizeof(vktrace_settings));
    g_default_settings.output_trace = vktrace_allocate_and_copy("vktrace_out.vktrace");
    g_default_settings.verbosity = "errors";
    g_default_settings.screenshotList = NULL;
    g_default_settings.screenshotColorFormat = NULL;
    g_default_settings.enable_pmb = true;
    g_default_settings.enable_trim_post_processing = false;
    g_default_settings.compressType = "lz4";
    g_default_settings.compressThreshold = 1024;

    // Check to see if the PAGEGUARD_PAGEGUARD_ENABLE_ENV env var is set.
    // If it is set to anything but "1", set the default to false.
    // Note that the command line option will override the env variable.
    char* pmbEnableEnv = vktrace_get_global_var(VKTRACE_PMB_ENABLE_ENV);
    if (pmbEnableEnv && strcmp(pmbEnableEnv, "1")) g_default_settings.enable_pmb = false;

    // Check to see if the VKTRACE_TRIM_POST_PROCESS_ENV env var is set.
    // If it is set to "1", set it to true.
    // If it is set to anything but "1", set it to false.
    // Note that the command line option will override the env variable.
    char* tppEnableEnv = vktrace_get_global_var(VKTRACE_TRIM_POST_PROCESS_ENV);
    if (tppEnableEnv && strcmp(tppEnableEnv, "1")) g_default_settings.enable_trim_post_processing = false;
    if (tppEnableEnv && !strcmp(tppEnableEnv, "1")) g_default_settings.enable_trim_post_processing = true;

    // get the value of VKTRACE_ENABLE_TRACE_LOCK_ENV env variable.
    // if it is set to "1" (true), locking for API calls is enabled.
    // by default it is set to "0" (false), in which locking is enabled only when trimming is enabled.
    // Note that the command line option will override the env variable.
    char* tl_enable_env = vktrace_get_global_var(VKTRACE_ENABLE_TRACE_LOCK_ENV);
    if (tl_enable_env && (strcmp(tl_enable_env, "1") == 0)) g_default_settings.enable_trace_lock = true;

    if (vktrace_SettingGroup_init(&g_settingGroup, NULL, argc, argv, &g_settings.arguments) != 0) {
        // invalid cmd-line parameters
        vktrace_SettingGroup_delete(&g_settingGroup);
        vktrace_free(g_default_settings.output_trace);
        return -1;
    } else {
        // Validate vktrace inputs
        BOOL validArgs = TRUE;

        if (g_settings.output_trace == NULL || strlen(g_settings.output_trace) == 0) {
            validArgs = FALSE;
        }

        if (strcmp(g_settings.verbosity, "quiet") == 0)
            vktrace_LogSetLevel(VKTRACE_LOG_NONE);
        else if (strcmp(g_settings.verbosity, "errors") == 0)
            vktrace_LogSetLevel(VKTRACE_LOG_ERROR);
        else if (strcmp(g_settings.verbosity, "warnings") == 0)
            vktrace_LogSetLevel(VKTRACE_LOG_WARNING);
        else if (strcmp(g_settings.verbosity, "full") == 0)
            vktrace_LogSetLevel(VKTRACE_LOG_VERBOSE);
#if defined(_DEBUG)
        else if (strcmp(g_settings.verbosity, "debug") == 0)
            vktrace_LogSetLevel(VKTRACE_LOG_DEBUG);
#endif
        else {
            vktrace_LogSetLevel(VKTRACE_LOG_ERROR);
            validArgs = FALSE;
        }
        vktrace_set_global_var(_VKTRACE_VERBOSITY_ENV, g_settings.verbosity);

        if (g_settings.screenshotList) {
            if (!screenshot::checkParsingFrameRange(g_settings.screenshotList)) {
                vktrace_LogError("Screenshot range error");
                validArgs = FALSE;
            } else {
                // Export list to screenshot layer
                vktrace_set_global_var("VK_SCREENSHOT_FRAMES", g_settings.screenshotList);
            }
        } else {
            vktrace_set_global_var("VK_SCREENSHOT_FRAMES", "");
        }

        // Set up environment for screenshot color space format
        if (g_settings.screenshotColorFormat != NULL && g_settings.screenshotList != NULL) {
            vktrace_set_global_var("VK_SCREENSHOT_FORMAT", g_settings.screenshotColorFormat);
        }else if (g_settings.screenshotColorFormat != NULL && g_settings.screenshotList == NULL) {
            vktrace_LogWarning("Screenshot format should be used when screenshot enabled!");
            vktrace_set_global_var("VK_SCREENSHOT_FORMAT", "");
        } else {
            vktrace_set_global_var("VK_SCREENSHOT_FORMAT", "");
        }

        if (validArgs == FALSE) {
            vktrace_SettingGroup_print(&g_settingGroup);
            return -1;
        }

        if (g_settings.program == NULL || strlen(g_settings.program) == 0) {
            vktrace_LogWarning("No program (-p) parameter found: Will run vktrace as server.");
            printf("Running vktrace as server...\n");
            fflush(stdout);
            g_settings.arguments = NULL;
        } else {
            if (g_settings.working_dir == NULL || strlen(g_settings.working_dir) == 0) {
                CHAR* buf = VKTRACE_NEW_ARRAY(CHAR, 4096);
                vktrace_LogVerbose("No working directory (-w) parameter found: Assuming executable's path as working directory.");
                vktrace_platform_full_path(g_settings.program, 4096, buf);
                g_settings.working_dir = vktrace_platform_extract_path(buf);
                VKTRACE_DELETE(buf);
            }

            vktrace_LogVerbose("Running vktrace as parent process will spawn child process: %s", g_settings.program);
            if (g_settings.arguments != NULL && strlen(g_settings.arguments) > 0) {
                vktrace_LogVerbose("Args to be passed to child process: '%s'", g_settings.arguments);
            }
        }
    }

    vktrace_set_global_var(VKTRACE_PMB_ENABLE_ENV, g_settings.enable_pmb ? "1" : "0");
    vktrace_set_global_var(VKTRACE_TRIM_POST_PROCESS_ENV, g_settings.enable_trim_post_processing ? "1" : "0");
    vktrace_set_global_var(VKTRACE_ENABLE_TRACE_LOCK_ENV, g_settings.enable_trace_lock ? "1" : "0");

    if (g_settings.traceTrigger) {
        // Export list to screenshot layer
        vktrace_set_global_var(VKTRACE_TRIM_TRIGGER_ENV, g_settings.traceTrigger);
    } else {
        vktrace_set_global_var(VKTRACE_TRIM_TRIGGER_ENV, "");
    }

    // set trim max commands batched size env var that communicates with the layer
    if (g_settings.trimCmdBatchSizeStr != NULL) {
        uint64_t trimMaxCmdBatchSzValue = 0;
        if (sscanf(g_settings.trimCmdBatchSizeStr, "%" PRIu64, &trimMaxCmdBatchSzValue) == 1) {
            if (trimMaxCmdBatchSzValue > 0) {
                vktrace_set_global_var(VKTRACE_TRIM_MAX_COMMAND_BATCH_SIZE_ENV, g_settings.trimCmdBatchSizeStr);
                vktrace_LogVerbose(
                    "Maximum trim commands batched size set by option is: '%s'.\n\
                                    Note: This maximum number will be limited by device max memory allocation \
                                    count determined during trim.",
                    g_settings.trimCmdBatchSizeStr);
            } else {
                vktrace_LogError(
                    "Trim commands batched size range error. Commands batched size should be bigger than \
                                  0 and will be limited by device max memory allocation count determined during trim.");
                return 1;
            }
        } else {
            vktrace_LogError("Trim Max Commands Batched Size option must be formatted as: \"<max batched size>\".");
            return 1;
        }
    } else {
        vktrace_set_global_var(VKTRACE_TRIM_MAX_COMMAND_BATCH_SIZE_ENV, "");
    }

    vktrace_LogAlways("Tracing with v%s", VKTRACE_VERSION);

    unsigned int serverIndex = 0;
    do {
        // Create and start the process or run in server mode

        BOOL procStarted = TRUE;
        vktrace_process_info procInfo;
        memset(&procInfo, 0, sizeof(vktrace_process_info));
        if (g_settings.program != NULL) {
            procInfo.exeName = vktrace_allocate_and_copy(g_settings.program);
            procInfo.processArgs = vktrace_allocate_and_copy(g_settings.arguments);
            procInfo.fullProcessCmdLine = vktrace_copy_and_append(g_settings.program, " ", g_settings.arguments);
            procInfo.workingDirectory = vktrace_allocate_and_copy(g_settings.working_dir);
            procInfo.traceFilename = vktrace_allocate_and_copy(g_settings.output_trace);
        } else {
            procInfo.traceFilename = find_available_filename(g_settings.output_trace, true);
        }

        procInfo.parentThreadId = vktrace_platform_get_thread_id();

        // setup tracer, only Vulkan tracer suppported
        procInfo.maxCaptureThreadsNumber = PrepareTracers(&procInfo.pCaptureThreads);
        procInfo.currentCaptureThreadsCount = 1;
        if (g_settings.program != NULL) {
            char* instEnv = vktrace_get_global_var("VK_INSTANCE_LAYERS");
            // Add ScreenShot layer if enabled
            if (g_settings.screenshotList && (!instEnv || !strstr(instEnv, "VK_LAYER_LUNARG_screenshot"))) {
                if (!instEnv || strlen(instEnv) == 0)
                    vktrace_set_global_var("VK_INSTANCE_LAYERS", "VK_LAYER_LUNARG_screenshot");
                else {
                    char* newEnv = vktrace_copy_and_append(instEnv, VKTRACE_LIST_SEPARATOR, "VK_LAYER_LUNARG_screenshot");
                    vktrace_set_global_var("VK_INSTANCE_LAYERS", newEnv);
                }
                instEnv = vktrace_get_global_var("VK_INSTANCE_LAYERS");
            }
            char* devEnv = vktrace_get_global_var("VK_DEVICE_LAYERS");
            if (g_settings.screenshotList && (!devEnv || !strstr(devEnv, "VK_LAYER_LUNARG_screenshot"))) {
                if (!devEnv || strlen(devEnv) == 0)
                    vktrace_set_global_var("VK_DEVICE_LAYERS", "VK_LAYER_LUNARG_screenshot");
                else {
                    char* newEnv = vktrace_copy_and_append(devEnv, VKTRACE_LIST_SEPARATOR, "VK_LAYER_LUNARG_screenshot");
                    vktrace_set_global_var("VK_DEVICE_LAYERS", newEnv);
                }
                devEnv = vktrace_get_global_var("VK_DEVICE_LAYERS");
            }
            // Add vktrace_layer enable env var if needed
            if (!instEnv || strlen(instEnv) == 0) {
                vktrace_set_global_var("VK_INSTANCE_LAYERS", "VK_LAYER_LUNARG_vktrace");
            } else if (instEnv != strstr(instEnv, "VK_LAYER_LUNARG_vktrace")) {
                char* newEnv = vktrace_copy_and_append("VK_LAYER_LUNARG_vktrace", VKTRACE_LIST_SEPARATOR, instEnv);
                vktrace_set_global_var("VK_INSTANCE_LAYERS", newEnv);
            }
            if (!devEnv || strlen(devEnv) == 0) {
                vktrace_set_global_var("VK_DEVICE_LAYERS", "VK_LAYER_LUNARG_vktrace");
            } else if (devEnv != strstr(devEnv, "VK_LAYER_LUNARG_vktrace")) {
                char* newEnv = vktrace_copy_and_append("VK_LAYER_LUNARG_vktrace", VKTRACE_LIST_SEPARATOR, devEnv);
                vktrace_set_global_var("VK_DEVICE_LAYERS", newEnv);
            }
            // call CreateProcess to launch the application
            procStarted = vktrace_process_spawn(&procInfo);
        }
        if (procStarted == FALSE) {
            vktrace_LogError("Failed to set up remote process.");
            exit(1);
        } else {
            if (InjectTracersIntoProcess(&procInfo) == FALSE) {
                vktrace_LogError("Failed to set up tracer communication threads.");
                exit(1);
            }

            // create watchdog thread to monitor existence of remote process
            if (g_settings.program != NULL)
            {
                procInfo.watchdogThread = vktrace_platform_create_thread(Process_RunWatchdogThread, &procInfo);
            }

#if defined(PLATFORM_LINUX) || defined(PLATFORM_OSX)

            // Sync wait for local threads and remote process to complete.
            if (g_settings.program != NULL) {
                vktrace_linux_sync_wait_for_thread(&procInfo.watchdogThread);
            }

            for (uint32_t i = 0; i < procInfo.currentCaptureThreadsCount; i++) {
                vktrace_linux_sync_wait_for_thread(&(procInfo.pCaptureThreads[i].recordingThread));
            }

#else
            vktrace_platform_resume_thread(&procInfo.hThread);

            // Now into the main message loop, listen for hotkeys to send over.
            exitval = (int)MessageLoop();
#endif
            if (procInfo.messageStream && procInfo.messageStream->mServerListenSocket) {
                closesocket(procInfo.messageStream->mServerListenSocket);
            }
        }
        vktrace_process_info_delete(&procInfo);
        serverIndex++;
    } while (g_settings.program == NULL);

    vktrace_SettingGroup_delete(&g_settingGroup);
    vktrace_free(g_default_settings.output_trace);

    exit(exitval);
}
