/**************************************************************************
 *
 * Copyright 2015-2018 Valve Corporation
 * Copyright (C) 2015-2018 LunarG, Inc.
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
 * Author: David Pinedo <david@lunarg.com>
 **************************************************************************/

#include <stdio.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <inttypes.h>

#if defined(ANDROID)
#include <sstream>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/window.h>
#endif
#include "vktrace_common.h"
#include "vktrace_tracelog.h"
#include "vktrace_filelike.h"
#include "vktrace_trace_packet_utils.h"
#include "vkreplay_main.h"
#include "vkreplay_factory.h"
#include "vkreplay_seq.h"
#include "vkreplay_vkdisplay.h"
#include "vkreplay_preload.h"
#include "screenshot_parsing.h"
#include "vktrace_vk_packet_id.h"
#include "vkreplay_vkreplay.h"
#include "decompressor.h"
#include <json/json.h>

extern vkReplay* g_replay;
static decompressor* g_decompressor = nullptr;

#if defined(ANDROID)
const char* env_var_screenshot_frames = "debug.vulkan.screenshot";
const char* env_var_screenshot_format = "debug.vulkan.screenshot.format";
const char* env_var_screenshot_prefix = "debug.vulkan.screenshot.prefix";
#else
const char* env_var_screenshot_frames = "VK_SCREENSHOT_FRAMES";
const char* env_var_screenshot_format = "VK_SCREENSHOT_FORMAT";
const char* env_var_screenshot_prefix = "VK_SCREENSHOT_PREFIX";
#endif

vktrace_SettingInfo g_settings_info[] = {
    {"o",
     "Open",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pTraceFilePath},
     {&replaySettings.pTraceFilePath},
     TRUE,
     "The trace file to open and replay."},
    {"t",
     "TraceFile",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pTraceFilePath},
     {&replaySettings.pTraceFilePath},
     TRUE,
     "The trace file to open and replay. (Deprecated)"},
    {"pltf",
     "PreloadTraceFile",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.preloadTraceFile},
     {&replaySettings.preloadTraceFile},
     TRUE,
     "Preload tracefile to memory before replay. (NumLoops need to be 1.)"},
#if !defined(ANDROID) && defined(PLATFORM_LINUX)
    {"headless",
     "Headless",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.headless},
     {&replaySettings.headless},
     TRUE,
     "Replay in headless mode via VK_EXT_headless_surface or VK_ARMX_headless_surface."},
#else
    {"vsyncoff",
     "VsyncOff",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.vsyncOff},
     {&replaySettings.vsyncOff},
     TRUE,
     "Turn off vsync to avoid replay being vsync-limited."},
#endif
    {"l",
     "NumLoops",
     VKTRACE_SETTING_UINT,
     {&replaySettings.numLoops},
     {&replaySettings.numLoops},
     TRUE,
     "The number of times to replay the trace file or loop range."},
    {"lsf",
     "LoopStartFrame",
     VKTRACE_SETTING_UINT,
     {&replaySettings.loopStartFrame},
     {&replaySettings.loopStartFrame},
     TRUE,
     "The start frame number of the loop range."},
    {"lef",
     "LoopEndFrame",
     VKTRACE_SETTING_UINT,
     {&replaySettings.loopEndFrame},
     {&replaySettings.loopEndFrame},
     TRUE,
     "The end frame number of the loop range."},
    {"c",
     "CompatibilityMode",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.compatibilityMode},
     {&replaySettings.compatibilityMode},
     TRUE,
     "Use compatibiltiy mode, i.e. convert memory indices to replay device indices, default is TRUE."},
    {"s",
     "Screenshot",
     VKTRACE_SETTING_STRING,
     {&replaySettings.screenshotList},
     {&replaySettings.screenshotList},
     TRUE,
     "Generate screenshots. <string> is one of:\n\
                                         comma separated list of frames\n\
                                         <start>-<count>-<interval>\n\
                                         \"all\""},
    {"sf",
     "ScreenshotFormat",
     VKTRACE_SETTING_STRING,
     {&replaySettings.screenshotColorFormat},
     {&replaySettings.screenshotColorFormat},
     TRUE,
     "Color Space format of screenshot files. Formats are UNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB"},
    {"x",
     "ExitOnAnyError",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.exitOnAnyError},
     {&replaySettings.exitOnAnyError},
     TRUE,
     "Exit if an error occurs during replay, default is FALSE"},
#if defined(PLATFORM_LINUX) && !defined(ANDROID) && (defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR))
    {"ds",
     "DisplayServer",
     VKTRACE_SETTING_STRING,
     {&replaySettings.displayServer},
     {&replaySettings.displayServer},
     TRUE,
     "Display server used for replay. Options are \"xcb\", \"wayland\", \"none\"."},
#endif
    {"sp",
     "ScreenshotPrefix",
     VKTRACE_SETTING_STRING,
     {&replaySettings.screenshotPrefix},
     {&replaySettings.screenshotPrefix},
     TRUE,
     "/path/to/snapshots/prefix- Must contain full path and a prefix, resulting screenshots will be named prefix-framenumber.ppm"},
    {"pt",
     "EnablePortabilityTableSupport",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enablePortabilityTable},
     {&replaySettings.enablePortabilityTable},
     TRUE,
     "Read portability table if it exists."},
    {"mma",
     "SelfManageMemoryAllocation",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.selfManageMemAllocation},
     {&replaySettings.selfManageMemAllocation},
     TRUE,
     "Manage OPTIMAL image's memory allocation by vkreplay. (Deprecated)"},
    {"fsw",
     "ForceSingleWindow",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceSingleWindow},
     {&replaySettings.forceSingleWindow},
     TRUE,
     "Force single window rendering."},
#if defined(_DEBUG)
    {"v",
     "Verbosity",
     VKTRACE_SETTING_STRING,
     {&replaySettings.verbosity},
     {&replaySettings.verbosity},
     TRUE,
     "Verbosity mode. Modes are \"quiet\", \"errors\", \"warnings\", \"full\", "
     "\"debug\"."},
#else
    {"v",
     "Verbosity",
     VKTRACE_SETTING_STRING,
     {&replaySettings.verbosity},
     {&replaySettings.verbosity},
     TRUE,
     "Verbosity mode. Modes are \"quiet\", \"errors\", \"warnings\", "
     "\"full\"."},
#endif
    {"fdaf",
     "forceDisableAF",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceDisableAF},
     {&replaySettings.forceDisableAF},
     TRUE,
     "Force to disable anisotropic filter, default is FALSE"},
    {"pmp",
     "memoryPercentage",
     VKTRACE_SETTING_UINT,
     {&replaySettings.memoryPercentage},
     {&replaySettings.memoryPercentage},
     TRUE,
     "Preload vktrace file block occupancy system memory percentage,the default is 50%"},
    {"prm",
     "premapping",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.premapping},
     {&replaySettings.premapping},
     TRUE,
     "Premap resources in several vulkan APIs when preloading."},
     {"epc",
     "enablePipelineCache",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enablePipelineCache},
     {&replaySettings.enablePipelineCache},
     TRUE,
     "Write pipeline cache to the disk and use the cache data for the next replay."},
    {"pcp",
     "pipelineCachePath",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pipelineCachePath},
     {&replaySettings.pipelineCachePath},
     TRUE,
     "Set the path for saving the pipeline cache data for the replay."},
    {"fsii",
     "forceSyncImgIdx",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceSyncImgIdx},
     {&replaySettings.forceSyncImgIdx},
     TRUE,
     "Force sync the acquire next image index."},
    {"dascr",
     "disableAsCaptureReplay",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.disableAsCaptureReplay},
     {&replaySettings.disableAsCaptureReplay},
     TRUE,
     "Disable acceleration structure capture replay feature."},
    {"dbcr",
     "disableBufferCaptureReplay",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.disableBufferCaptureReplay},
     {&replaySettings.disableBufferCaptureReplay},
     TRUE,
     "Disable buffer capture replay feature."},
    {"frq",
     "forceRayQuery",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceRayQuery},
     {&replaySettings.forceRayQuery},
     TRUE,
     "Force to replay this trace file as a ray-query one."},
    {"tsf",
     "TriggerScriptOnFrame",
     VKTRACE_SETTING_UINT,
     {&replaySettings.triggerScript},
     {&replaySettings.triggerScript},
     TRUE,
     "Trigger script on a specific frame."},
    {"tsp",
     "scriptPath",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pScriptPath},
     {&replaySettings.pScriptPath},
     TRUE,
     "Trigger script path."},
    {"pmm",
     "perfMeasuringMode",
     VKTRACE_SETTING_UINT,
     {&replaySettings.perfMeasuringMode},
     {&replaySettings.perfMeasuringMode},
     TRUE,
     "Set the performance measuring mode, 0 - off, 1 - on."},
#if defined(_DEBUG)
    {"pcgpi",
     "printCurrentGPI",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.printCurrentGPI},
     {&replaySettings.printCurrentGPI},
     TRUE,
     "Print current GPI that is about to be replayed."},
#endif
    {"esv",
     "enableSyncValidation",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enableSyncValidation},
     {&replaySettings.enableSyncValidation},
     TRUE,
     "Enable the synchronization validation feature of the validation layer."},
    {"ocdf",
     "overrideCreateDeviceFeatures",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.overrideCreateDeviceFeatures},
     {&replaySettings.overrideCreateDeviceFeatures},
     TRUE,
     "If some features in vkCreateDevice in trace file don't support by replaying device, disable them."},
    {"scic",
     "swapChainMinImageCount",
     VKTRACE_SETTING_UINT,
     {&replaySettings.swapChainMinImageCount},
     {&replaySettings.swapChainMinImageCount},
     FALSE,
     "Change the swapchain min image count."},
    {"intd",
     "instrumentationDelay",
     VKTRACE_SETTING_UINT,
     {&replaySettings.instrumentationDelay},
     {&replaySettings.instrumentationDelay},
     TRUE,
     "Delay in microseconds that the retracer should sleep for after each present call in the measurement range."},
    {"sgfs",
     "skipGetFenceStatus",
     VKTRACE_SETTING_UINT,
     {&replaySettings.skipGetFenceStatus},
     {&replaySettings.skipGetFenceStatus},
     TRUE,
     "Skip the GetFenceStatus() calls, 0 - Not skip; 1 - Skip all the unsuccess calls; 2 - Skip all calls."}
};

vktrace_SettingGroup g_replaySettingGroup = {"vkreplay", sizeof(g_settings_info) / sizeof(g_settings_info[0]), &g_settings_info[0], nullptr};


static vktrace_replay::vktrace_trace_packet_replay_library* g_replayer_interface = NULL;

void TerminateHandler(int) {
    if (NULL != g_replayer_interface) {
        g_replayer_interface->OnTerminate();
    }
}

namespace vktrace_replay {

void triggerScript() {
    char command[512] = {0};
    memset(command, 0, sizeof(command));
    #if defined(PLATFORM_LINUX) && !defined(ANDROID)
        sprintf(command, "/bin/sh %s", g_pReplaySettings->pScriptPath);
    #else
        sprintf(command, "/system/bin/sh %s", g_pReplaySettings->pScriptPath);
    #endif
    int result = system(command);
    vktrace_LogAlways("Script %s run result: %d", command, result);
}

unsigned int replay(vktrace_trace_packet_replay_library* replayer, vktrace_trace_packet_header* packet)
{
    if (replaySettings.preloadTraceFile && timerStarted()) {    // the packet has already been interpreted during the preloading
        return replayer->Replay(packet);
    }
    else {
        return replayer->Replay(replayer->Interpret(packet));
    }
}

int main_loop(vktrace_replay::ReplayDisplay display, Sequencer& seq, vktrace_trace_packet_replay_library* replayerArray[]) {
    int err = 0;
    vktrace_trace_packet_header* packet;
#if defined(ANDROID) || !defined(ARM_ARCH)
    vktrace_trace_packet_message* msgPacket;
#endif
    struct seqBookmark startingPacket;

    bool trace_running = true;

    // record the location of looping start packet
    seq.record_bookmark();
    seq.get_bookmark(startingPacket);
    uint64_t totalLoops = replaySettings.numLoops;
    uint64_t totalLoopFrames = 0;
    uint64_t end_time;
    uint64_t start_frame = replaySettings.loopStartFrame == UINT_MAX ? 0 : replaySettings.loopStartFrame;
    uint64_t end_frame = UINT_MAX;
    if (start_frame == 0) {
        if (replaySettings.preloadTraceFile) {
            vktrace_LogAlways("Preloading trace file...");
            bool success = seq.start_preload(replayerArray, g_decompressor);
            if (!success) {
               vktrace_LogAlways("The chunk count is 0, won't use preloading to replay.");
               replaySettings.preloadTraceFile =  FALSE;
            }
        }
        timer_started = true;
        vktrace_LogAlways("================== Start timer (Frame: %llu) ==================", start_frame);
    }
    uint64_t start_time = vktrace_get_time();
    const char* screenshot_list = replaySettings.screenshotList;
    if (g_pReplaySettings->triggerScript == 0 && g_pReplaySettings->pScriptPath != NULL) {
        triggerScript();
    }

    while (replaySettings.numLoops > 0) {
        if (replaySettings.numLoops > 1 && replaySettings.screenshotList != NULL) {
            // Don't take screenshots until the last loop
            replaySettings.screenshotList = NULL;
        } else if (replaySettings.numLoops == 1 && replaySettings.screenshotList != screenshot_list) {
            // Re-enable screenshots on last loop if they have been disabled
            replaySettings.screenshotList = screenshot_list;
        }

        while (trace_running) {
            packet = seq.get_next_packet();
            if (!packet) break;

            if (replaySettings.printCurrentGPI)
            {
                vktrace_LogDebug("Replaying GPI %lu", packet->global_packet_index);
            }

            switch (packet->packet_id) {
                case VKTRACE_TPI_MESSAGE:
#if defined(ANDROID) || !defined(ARM_ARCH)
                    if (replaySettings.preloadTraceFile && timerStarted()) {
                        msgPacket = (vktrace_trace_packet_message*)packet->pBody;
                    } else {
                        msgPacket = vktrace_interpret_body_as_trace_packet_message(packet);
                    }
                    vktrace_LogAlways("Packet %lu: Traced Message (%s): %s", packet->global_packet_index,
                                      vktrace_LogLevelToShortString(msgPacket->type), msgPacket->message);
#endif
                    break;
                case VKTRACE_TPI_MARKER_CHECKPOINT:
                    break;
                case VKTRACE_TPI_MARKER_API_BOUNDARY:
                    break;
                case VKTRACE_TPI_MARKER_API_GROUP_BEGIN:
                    break;
                case VKTRACE_TPI_MARKER_API_GROUP_END:
                    break;
                case VKTRACE_TPI_MARKER_TERMINATE_PROCESS:
                    break;
                case VKTRACE_TPI_PORTABILITY_TABLE:
                case VKTRACE_TPI_META_DATA:
                    break;
                case VKTRACE_TPI_VK_vkQueuePresentKHR: {
                    if (replay(g_replayer_interface, packet) != VKTRACE_REPLAY_SUCCESS) {
                        vktrace_LogError("Failed to replay QueuePresent().");
                        if (replaySettings.exitOnAnyError) {
                            err = -1;
                            goto out;
                        }
                    }
                    // frame control logic
                    unsigned int frameNumber = g_replayer_interface->GetFrameNumber();

                    if (frameNumber > start_frame && replaySettings.instrumentationDelay > 0) {
                        usleep(replaySettings.instrumentationDelay);
                    }

                    if (g_pReplaySettings->triggerScript != UINT_MAX && g_pReplaySettings->pScriptPath != NULL && (frameNumber + 1) == g_pReplaySettings->triggerScript) {
                        triggerScript();
                    }

                    // Only set the loop start location and start_time in the first loop when loopStartFrame is not 0
                    if (frameNumber == start_frame && start_frame > 0 && replaySettings.numLoops == totalLoops) {
                        // record the location of looping start packet
                        seq.record_bookmark();
                        seq.get_bookmark(startingPacket);
                        if (replaySettings.preloadTraceFile) {
                            vktrace_LogAlways("Preloading trace file...");
                            bool success = seq.start_preload(replayerArray, g_decompressor);
                            if (!success) {
                                vktrace_LogAlways("The chunk count is 0, won't use preloading to replay.");
                                replaySettings.preloadTraceFile =  FALSE;
                            }
                        }
                        timer_started = true;
                        start_time = vktrace_get_time();
                        vktrace_LogAlways("================== Start timer (Frame: %llu) ==================", start_frame);
                        g_replayer_interface->SetInFrameRange(true);
                    }

                    if (frameNumber == replaySettings.loopEndFrame) {
                        trace_running = false;
                    }

                    display.process_event();
                    while(display.get_pause_status()) {
                        display.process_event();
                    }
                    if (display.get_quit_status()) {
                        goto out;
                    }
                    break;
                }
                // TODO processing code for all the above cases
                default: {
                    if (packet->tracer_id >= VKTRACE_MAX_TRACER_ID_ARRAY_SIZE || packet->tracer_id == VKTRACE_TID_RESERVED) {
                        vktrace_LogError("Tracer_id from packet num packet %d invalid.", packet->packet_id);
                        continue;
                    }
                    g_replayer_interface = replayerArray[packet->tracer_id];
                    if (packet->tracer_id == VKTRACE_TID_VULKAN_COMPRESSED)
                        g_replayer_interface = replayerArray[VKTRACE_TID_VULKAN];
                    if (g_replayer_interface == NULL) {
                        vktrace_LogWarning("Tracer_id %d has no valid replayer.", packet->tracer_id);
                        continue;
                    } else if (timer_started) {
                        g_replayer_interface->SetInFrameRange(true);
                    }
                    if (packet->packet_id >= VKTRACE_TPI_VK_vkApiVersion && packet->packet_id < VKTRACE_TPI_META_DATA) {
                        // replay the API packet
                        if (replay(g_replayer_interface, packet) != VKTRACE_REPLAY_SUCCESS) {
                            vktrace_LogError("Failed to replay packet_id %d, with global_packet_index %d.", packet->packet_id,
                                             packet->global_packet_index);
                            if (replaySettings.exitOnAnyError || packet->packet_id == VKTRACE_TPI_VK_vkCreateInstance ||
                                packet->packet_id == VKTRACE_TPI_VK_vkCreateDevice ||
                                packet->packet_id == VKTRACE_TPI_VK_vkCreateSwapchainKHR) {
                                err = -1;
                                goto out;
                            }
                        }
                    } else {
                        vktrace_LogError("Bad packet type id=%d, index=%d.", packet->packet_id, packet->global_packet_index);
                        err = -1;
                        goto out;
                    }
                }
            }
        }
        replaySettings.numLoops--;
        vktrace_LogVerbose("Loop number %d completed. Remaining loops:%d", replaySettings.numLoops + 1, replaySettings.numLoops);

        if (end_frame == UINT_MAX)
            end_frame = replaySettings.loopEndFrame == UINT_MAX
                            ? g_replayer_interface->GetFrameNumber()
                            : std::min((unsigned int)g_replayer_interface->GetFrameNumber(), replaySettings.loopEndFrame);
        totalLoopFrames += end_frame - start_frame;

        seq.set_bookmark(startingPacket);
        trace_running = true;
        if (g_replayer_interface != NULL) {
            g_replayer_interface->ResetFrameNumber(replaySettings.loopStartFrame);
        }
    }
    if (g_replay != nullptr) {
        g_replay->deviceWaitIdle();
    }
    end_time = vktrace_get_time();
    timer_started = false;
    g_replayer_interface->SetInFrameRange(false);
    g_replayer_interface->OnTerminate();
    vktrace_LogAlways("================== End timer (Frame: %llu) ==================", end_frame);
    if (end_time > start_time) {
        double fps = static_cast<double>(totalLoopFrames) / (end_time - start_time) * NANOSEC_IN_ONE_SEC;
        if (g_ruiFrames) {
            vktrace_LogAlways("NOTE: The number of frames is determined by g_ruiFrames");
        }
        vktrace_LogAlways("%f fps, %f seconds, %" PRIu64 " frame%s, %" PRIu64 " loop%s, framerange %" PRId64 "-%" PRId64, fps,
                          static_cast<double>(end_time - start_time) / NANOSEC_IN_ONE_SEC, totalLoopFrames, totalLoopFrames > 1 ? "s" : "",
                          totalLoops, totalLoops > 1 ? "s" : "", start_frame, end_frame);
        vktrace_LogAlways("start frame at %.6f, end frame at %.6f [ perf arg: --time %.6f,%.6f ]",
                          static_cast<double>(start_time) / NANOSEC_IN_ONE_SEC,
                          static_cast<double>(end_time) / NANOSEC_IN_ONE_SEC,
                          static_cast<double>(start_time) / NANOSEC_IN_ONE_SEC,
                          static_cast<double>(end_time) / NANOSEC_IN_ONE_SEC);
        if (replaySettings.preloadTraceFile) {
            uint64_t preload_waiting_time_when_replaying = get_preload_waiting_time_when_replaying();
            vktrace_LogAlways("waiting time when replaying: %.6fs", static_cast<double>(preload_waiting_time_when_replaying) / NANOSEC_IN_ONE_SEC);
            if (preloaded_whole())
                vktrace_LogAlways("The frame range can be preloaded completely!");
            else
                vktrace_LogAlways("The frame range can't be preloaded completely!");
        }
    } else {
        vktrace_LogError("fps error!");
    }

out:
    seq.clean_up();
    if (g_decompressor != nullptr) {
        delete g_decompressor;
        g_decompressor = nullptr;
    }
    if (replaySettings.screenshotList != NULL) {
        vktrace_free((char*)replaySettings.screenshotList);
        replaySettings.screenshotList = NULL;
    }
    return err;
}
}  // namespace vktrace_replay

using namespace vktrace_replay;

void loggingCallback(VktraceLogLevel level, const char* pMessage) {
    if (level == VKTRACE_LOG_NONE) return;

#if defined(ANDROID)
    switch (level) {
        case VKTRACE_LOG_DEBUG:
            __android_log_print(ANDROID_LOG_DEBUG, "vkreplay", "%s", pMessage);
            break;
        case VKTRACE_LOG_ERROR:
            __android_log_print(ANDROID_LOG_ERROR, "vkreplay", "%s", pMessage);
            break;
        case VKTRACE_LOG_WARNING:
            __android_log_print(ANDROID_LOG_WARN, "vkreplay", "%s", pMessage);
            break;
        case VKTRACE_LOG_VERBOSE:
            __android_log_print(ANDROID_LOG_VERBOSE, "vkreplay", "%s", pMessage);
            break;
        default:
            __android_log_print(ANDROID_LOG_INFO, "vkreplay", "%s", pMessage);
            break;
    }
#else
    switch (level) {
        case VKTRACE_LOG_DEBUG:
            printf("vkreplay debug: %s\n", pMessage);
            break;
        case VKTRACE_LOG_ERROR:
            printf("vkreplay error: %s\n", pMessage);
            break;
        case VKTRACE_LOG_WARNING:
            printf("vkreplay warning: %s\n", pMessage);
            break;
        case VKTRACE_LOG_VERBOSE:
            printf("vkreplay info: %s\n", pMessage);
            break;
        default:
            printf("%s\n", pMessage);
            break;
    }
    fflush(stdout);

#if defined(_DEBUG)
#if defined(WIN32)
    OutputDebugString(pMessage);
#endif
#endif
#endif  // ANDROID
}

static void freePortabilityTablePackets() {
    for (size_t i = 0; i < portabilityTablePackets.size(); i++) {
        vktrace_trace_packet_header* pPacket = (vktrace_trace_packet_header*)portabilityTablePackets[i];
        if (pPacket) {
            vktrace_free(pPacket);
        }
    }
}

std::vector<uint64_t> portabilityTable;
static bool preloadPortabilityTablePackets() {
    uint64_t originalFilePos = vktrace_FileLike_GetCurrentPosition(traceFile);
    uint64_t portabilityTableTotalPacketSize = 0;

    for (size_t i = 0; i < portabilityTable.size(); i++) {
        if (!vktrace_FileLike_SetCurrentPosition(traceFile, portabilityTable[i])) {
            return false;
        }
        vktrace_trace_packet_header* pPacket = vktrace_read_trace_packet(traceFile);
        if (!pPacket) {
            return false;
        }
        if (pPacket->tracer_id == VKTRACE_TID_VULKAN_COMPRESSED) {
            int ret = decompress_packet(g_decompressor, pPacket);
            if (ret != 0) {
                vktrace_LogError("Decompress packet error.");
                break;
            }
        }
        pPacket = interpret_trace_packet_vk(pPacket);
        portabilityTablePackets[i] = (uintptr_t)pPacket;
        portabilityTableTotalPacketSize += pPacket->size;
    }

    vktrace_LogVerbose("Total packet size preloaded for portability table: %" PRIu64 " bytes", portabilityTableTotalPacketSize);

    if (!vktrace_FileLike_SetCurrentPosition(traceFile, originalFilePos)) {
        freePortabilityTablePackets();
        return false;
    }
    return true;
}

static bool readPortabilityTable() {
    uint64_t tableSize;
    uint64_t originalFilePos;

    originalFilePos = vktrace_FileLike_GetCurrentPosition(traceFile);
    if (UINT64_MAX == originalFilePos) return false;
    if (!vktrace_FileLike_SetCurrentPosition(traceFile, traceFile->mFileLen - sizeof(uint64_t))) return false;
    if (!vktrace_FileLike_ReadRaw(traceFile, &tableSize, sizeof(uint64_t))) return false;
    if (tableSize != 0) {
        if (!vktrace_FileLike_SetCurrentPosition(traceFile, traceFile->mFileLen - ((tableSize + 1) * sizeof(uint64_t))))
            return false;
        portabilityTable.resize((size_t)tableSize);
        portabilityTablePackets.resize((size_t)tableSize);
        if (!vktrace_FileLike_ReadRaw(traceFile, &portabilityTable[0], sizeof(uint64_t) * tableSize)) return false;
    }
    if (!vktrace_FileLike_SetCurrentPosition(traceFile, originalFilePos)) return false;
    vktrace_LogDebug("portabilityTable size=%" PRIu64 "\n", tableSize);
    return true;
}

static int vktrace_SettingGroup_init_from_metadata(const Json::Value &replay_options) {
    vktrace_SettingInfo* pSettings = g_replaySettingGroup.pSettings;
    unsigned int num_settings = g_replaySettingGroup.numSettings;

    // update settings based on command line options
    for (Json::Value::const_iterator it = replay_options.begin(); it != replay_options.end(); ++it) {
        unsigned int settingIndex;
        std::string curArg = it.key().asString();
        std::string curValue = (*it).asString();

        for (settingIndex = 0; settingIndex < num_settings; settingIndex++) {
            const char* pSettingName = pSettings[settingIndex].pShortName;

            if (pSettingName != NULL && g_replaySettingGroup.pOptionsOverridedByCmd[settingIndex] == false && curArg == pSettingName) {
                if (vktrace_SettingInfo_parse_value(&pSettings[settingIndex], curValue.c_str())) {
                    const int MAX_NAME_LENGTH = 100;
                    const int MAX_VALUE_LENGTH = 100;
                    char name[MAX_NAME_LENGTH];
                    char value[MAX_VALUE_LENGTH];
                    vktrace_Setting_to_str(&pSettings[settingIndex], name, value);
                    vktrace_LogAlways("Option \"%s\" overridden to \"%s\" by meta data", name, value);
                }
                break;
            }
        }
    }
    return 0;
}

static void readMetaData(vktrace_trace_file_header* pFileHeader) {
    uint64_t originalFilePos;

    originalFilePos = vktrace_FileLike_GetCurrentPosition(traceFile);
    if (!vktrace_FileLike_SetCurrentPosition(traceFile, pFileHeader->meta_data_offset)) {
        vktrace_LogError("readMetaData(): Failed to set file position at %llu", pFileHeader->meta_data_offset);
    } else {
        vktrace_trace_packet_header hdr;
        if (!vktrace_FileLike_ReadRaw(traceFile, &hdr, sizeof(hdr)) || hdr.packet_id != VKTRACE_TPI_META_DATA) {
            vktrace_LogError("readMetaData(): Failed to read the meta data packet header");
        } else {
            uint64_t meta_data_json_str_size = hdr.size - sizeof(hdr);
            char* meta_data_json_str = new char[meta_data_json_str_size];
            if (!vktrace_FileLike_ReadRaw(traceFile, meta_data_json_str, meta_data_json_str_size)) {
                vktrace_LogError("readMetaData(): Failed to read the meta data json string");
            } else {
                vktrace_LogDebug("Meta data: %s", meta_data_json_str);

                Json::Reader reader;
                Json::Value meda_data_json;
                bool parsingSuccessful = reader.parse(meta_data_json_str, meda_data_json);
                if (!parsingSuccessful) {
                    vktrace_LogError("readMetaData(): Failed to parse the meta data json string");
                }
                Json::Value replay_options = meda_data_json["ReplayOptions"];
                vktrace_SettingGroup_init_from_metadata(replay_options);

                if (meda_data_json.isMember("deviceFeatures")) {
                    Json::Value device = meda_data_json["deviceFeatures"];
                    int deviceCount = device["device"].size();
                    extern std::unordered_map<VkDevice, deviceFeatureSupport> g_TraceDeviceToDeviceFeatures;
                    for(unsigned int i = 0; i < deviceCount; i++){
                        VkDevice traceDevice = (VkDevice)std::strtoul(device["device"][i]["deviceHandle"].asCString(), 0, 16);
                        deviceFeatureSupport deviceFeatures = {device["device"][i]["accelerationStructureCaptureReplay"].asUInt(), device["device"][i]["bufferDeviceAddressCaptureReplay"].asUInt()};
                        g_TraceDeviceToDeviceFeatures[traceDevice] = deviceFeatures;
                    }
                }
            }
            delete[] meta_data_json_str;
        }
    }
    vktrace_FileLike_SetCurrentPosition(traceFile, originalFilePos);
}

int vkreplay_main(int argc, char** argv, vktrace_replay::ReplayDisplayImp* pDisp = nullptr) {
    int err = 0;
    vktrace_SettingGroup* pAllSettings = NULL;
    unsigned int numAllSettings = 0;

    // Default verbosity level
    vktrace_LogSetCallback(loggingCallback);
    vktrace_LogSetLevel(VKTRACE_LOG_ERROR);

    // apply settings from cmd-line args
    std::vector<BOOL> optionsOverridedByCmd(g_replaySettingGroup.numSettings, false);
    g_replaySettingGroup.pOptionsOverridedByCmd = optionsOverridedByCmd.data();
    if (vktrace_SettingGroup_init_from_cmdline(&g_replaySettingGroup, argc, argv, &replaySettings.pTraceFilePath) != 0) {
        // invalid options specified
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        return -1;
    }

    if (replaySettings.loopStartFrame >= replaySettings.loopEndFrame) {
        vktrace_LogError("Bad loop frame range, the end frame number must be greater than start frame number");
        return -1;
    }
    if (replaySettings.memoryPercentage > 100 || replaySettings.memoryPercentage == 0) {
        vktrace_LogError("Bad preload memory Percentage");
        return -1;
    }

    // merge settings so that new settings will get written into the settings file
    vktrace_SettingGroup_merge(&g_replaySettingGroup, &pAllSettings, &numAllSettings);

    // Force NumLoops option to 1 if pre-load is enabled, because the trace file loaded into memory may be overwritten during replay
    // which will cause error in the second or later loops.
    if (replaySettings.preloadTraceFile && replaySettings.numLoops != 1) {
        vktrace_LogError("PreloadTraceFile is enabled.  Force NumLoops to 1!");
        vktrace_LogError("Please don't enable PreloadTraceFile if you want to replay the trace file multiple times!");
        replaySettings.numLoops = 1;
    }

    // Set verbosity level
    if (replaySettings.verbosity == NULL || !strcmp(replaySettings.verbosity, "errors"))
        replaySettings.verbosity = "errors";
    else if (!strcmp(replaySettings.verbosity, "quiet"))
        vktrace_LogSetLevel(VKTRACE_LOG_NONE);
    else if (!strcmp(replaySettings.verbosity, "warnings"))
        vktrace_LogSetLevel(VKTRACE_LOG_WARNING);
    else if (!strcmp(replaySettings.verbosity, "full"))
        vktrace_LogSetLevel(VKTRACE_LOG_VERBOSE);
#if defined(_DEBUG)
    else if (!strcmp(replaySettings.verbosity, "debug"))
        vktrace_LogSetLevel(VKTRACE_LOG_DEBUG);
#endif
    else {
        vktrace_SettingGroup_print(&g_replaySettingGroup);
        // invalid options specified
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        return -1;
    }

    // Set up environment for screenshot
    if (replaySettings.screenshotList != NULL) {
        if (!screenshot::checkParsingFrameRange(replaySettings.screenshotList)) {
            vktrace_LogError("Screenshot range error");
            vktrace_SettingGroup_print(&g_replaySettingGroup);
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            return -1;
        } else {
            // Set env var that communicates list to ScreenShot layer
            vktrace_set_global_var(env_var_screenshot_frames, replaySettings.screenshotList);
        }

        // Set up environment for screenshot color space format
        if (replaySettings.screenshotColorFormat != NULL && replaySettings.screenshotList != NULL) {
            vktrace_set_global_var(env_var_screenshot_format, replaySettings.screenshotColorFormat);
        } else if (replaySettings.screenshotColorFormat != NULL && replaySettings.screenshotList == NULL) {
            vktrace_LogWarning("Screenshot format should be used when screenshot enabled!");
            vktrace_set_global_var(env_var_screenshot_format, "");
        } else {
            vktrace_set_global_var(env_var_screenshot_format, "");
        }

        // Set up environment for screenshot prefix
        if (replaySettings.screenshotPrefix != NULL && replaySettings.screenshotList != NULL) {
            vktrace_set_global_var(env_var_screenshot_prefix, replaySettings.screenshotPrefix);
        } else if (replaySettings.screenshotPrefix != NULL && replaySettings.screenshotList == NULL) {
            vktrace_LogWarning("Screenshot prefix should be used when screenshot enabled!");
            vktrace_set_global_var(env_var_screenshot_prefix, "");
        } else {
            vktrace_set_global_var(env_var_screenshot_prefix, "");
        }
    }

    vktrace_LogAlways("Replaying with v%s", VKTRACE_VERSION);

    // open the trace file
    char* pTraceFile = replaySettings.pTraceFilePath;
    vktrace_trace_file_header fileHeader;
    vktrace_trace_file_header* pFileHeader;  // File header, including gpuinfo structs

    FILE* tracefp;

    if (pTraceFile != NULL && strlen(pTraceFile) > 0) {
        tracefp = fopen(pTraceFile, "rb");
        if (tracefp == NULL) {
            vktrace_LogError("Cannot open trace file: '%s'.", pTraceFile);
            // invalid options specified
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }
    } else {
        vktrace_LogError("No trace file specified.");
        vktrace_SettingGroup_print(&g_replaySettingGroup);
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        return -1;
    }

    // Decompress trace file if it is a gz file.
    std::string tmpfilename;
    std::ostringstream sout_tmpname;
    if (vktrace_File_IsCompressed(tracefp)) {
        time_t t = time(NULL);
#if defined(ANDROID)
        sout_tmpname << "/sdcard/tmp_";
#elif defined(PLATFORM_LINUX)
        sout_tmpname << "/tmp/tmp_";
#else
        sout_tmpname << "tmp_";
#endif
        sout_tmpname << std::uppercase << std::hex << t << ".vktrace";
        tmpfilename = sout_tmpname.str();
        // Close the fp for the gz file and open the decompressed temp file.
        fclose(tracefp);
        if (!vktrace_File_Decompress(pTraceFile, tmpfilename.c_str())) {
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }
        tracefp = fopen(tmpfilename.c_str(), "rb");
        if (tracefp == NULL) {
            vktrace_LogError("Cannot open trace file: '%s'.", tmpfilename.c_str());
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }
    }

    // read the header
    traceFile = vktrace_FileLike_create_file(tracefp);
    if (vktrace_FileLike_ReadRaw(traceFile, &fileHeader, sizeof(fileHeader)) == false) {
        vktrace_LogError("Unable to read header from file.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    extern bool g_hasAsApi;
    g_hasAsApi = (fileHeader.bit_flags & VKTRACE_USE_ACCELERATION_STRUCTURE_API_BIT) ? true : false;
    if (fileHeader.trace_file_version == VKTRACE_TRACE_FILE_VERSION_10) {
        g_hasAsApi = true;
    }

    // set global version num
    vktrace_set_trace_version(fileHeader.trace_file_version);

    // Make sure trace file version is supported.
    // We can't play trace files with a version prior to the minimum compatible version.
    // We also won't attempt to play trace files that are newer than this replayer.
    if (fileHeader.trace_file_version < VKTRACE_TRACE_FILE_VERSION_MINIMUM_COMPATIBLE ||
        fileHeader.trace_file_version > VKTRACE_TRACE_FILE_VERSION) {
        vktrace_LogError(
            "Trace file version %u is not compatible with this replayer version (%u).\nYou'll need to make a new trace file, or "
            "use "
            "the appropriate replayer.",
            fileHeader.trace_file_version, VKTRACE_TRACE_FILE_VERSION_MINIMUM_COMPATIBLE);
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Make sure magic number in trace file is valid and we have at least one gpuinfo struct
    if (fileHeader.magic != VKTRACE_FILE_MAGIC || fileHeader.n_gpuinfo < 1) {
        vktrace_LogError("%s does not appear to be a valid Vulkan trace file.", pTraceFile);
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Make sure we replay 64-bit traces with 64-bit replayer, and 32-bit traces with 32-bit replayer
    if (sizeof(void*) != fileHeader.ptrsize) {
        vktrace_LogError("%d-bit trace file is not supported by %d-bit vkreplay.", 8 * fileHeader.ptrsize, 8 * sizeof(void*));
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Make sure replay system endianess matches endianess in trace file
    if (get_endianess() != fileHeader.endianess) {
        vktrace_LogError("System endianess (%s) does not appear match endianess of tracefile (%s).",
                         get_endianess_string(get_endianess()), get_endianess_string(fileHeader.endianess));
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Allocate a new header that includes space for all gpuinfo structs
    if (!(pFileHeader = (vktrace_trace_file_header*)vktrace_malloc(sizeof(vktrace_trace_file_header) +
                                                                   (size_t)(fileHeader.n_gpuinfo * sizeof(struct_gpuinfo))))) {
        vktrace_LogError("Can't allocate space for trace file header.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Copy the file header, and append the gpuinfo array
    *pFileHeader = fileHeader;
    if (vktrace_FileLike_ReadRaw(traceFile, pFileHeader + 1, pFileHeader->n_gpuinfo * sizeof(struct_gpuinfo)) == false) {
        vktrace_LogError("Unable to read header from file.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        vktrace_free(pFileHeader);
        return -1;
    }

    // create decompressor
    if (pFileHeader->compress_type != VKTRACE_COMPRESS_TYPE_NONE) {
        g_decompressor = create_decompressor((VKTRACE_COMPRESS_TYPE)pFileHeader->compress_type);
        if (g_decompressor == nullptr) {
            vktrace_LogError("Create decompressor failed.");
            return -1;
        }
    }

    // read the meta data json string
    if (pFileHeader->trace_file_version > VKTRACE_TRACE_FILE_VERSION_9 && pFileHeader->meta_data_offset > 0) {
        readMetaData(pFileHeader);
    }
    if (replaySettings.forceRayQuery == TRUE) {
        g_hasAsApi = true;
    }

    // read portability table if it exists
    if (pFileHeader->portability_table_valid)
        vktrace_LogAlways("Portability table exists.");
    if (replaySettings.enablePortabilityTable) {
        vktrace_LogDebug("Read portability table if it exists.");
        if (pFileHeader->portability_table_valid) pFileHeader->portability_table_valid = readPortabilityTable();
        if (pFileHeader->portability_table_valid) pFileHeader->portability_table_valid = preloadPortabilityTablePackets();
        if (!pFileHeader->portability_table_valid)
            vktrace_LogAlways(
                "Trace file does not appear to contain portability table. Will not attempt to map memoryType indices.");
    } else {
        vktrace_LogDebug("Do not use portability table no matter it exists or not.");
        pFileHeader->portability_table_valid = 0;
    }

    // load any API specific driver libraries and init replayer objects
    uint8_t tidApi = VKTRACE_TID_RESERVED;
    vktrace_trace_packet_replay_library* replayer[VKTRACE_MAX_TRACER_ID_ARRAY_SIZE];
    ReplayFactory makeReplayer;

#if defined(PLATFORM_LINUX) && !defined(ANDROID)
    // Choose default display server if unset
    if (replaySettings.displayServer == NULL) {
        auto session = getenv("XDG_SESSION_TYPE");
        if (session == NULL) {
            replaySettings.displayServer = "none";
        } else if (strcmp(session, "x11") == 0) {
            replaySettings.displayServer = "xcb";
        } else if (strcmp(session, "wayland") == 0) {
            replaySettings.displayServer = "wayland";
        } else {
            replaySettings.displayServer = "none";
        }
    }

    // -headless option should be used with "-ds none" or without "-ds" option
    if ((strcasecmp(replaySettings.displayServer, "none") != 0) && replaySettings.headless) {
        vktrace_LogError("-headless should not be enabled when display server is not \"none\"");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
        vktrace_free(pFileHeader);
        return -1;
    }
#endif

    // Create window. Initial size is 100x100. It will later get resized to the size
    // used by the traced app. The resize will happen  during playback of swapchain functions.
    vktrace_replay::ReplayDisplay disp(100, 100);

    // Create display
    if (GetDisplayImplementation(replaySettings.displayServer, &pDisp) == -1) {
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            fclose(tracefp);
            vktrace_free(pTraceFile);
            vktrace_free(traceFile);
            if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
            vktrace_free(pFileHeader);
            return -1;
        }

    disp.set_implementation(pDisp);
//**********************************************************
#if defined(_DEBUG)
    static BOOL debugStartup = FALSE;  // TRUE
    while (debugStartup)
        ;
#endif
    //***********************************************************

    for (int i = 0; i < VKTRACE_MAX_TRACER_ID_ARRAY_SIZE; i++) {
        replayer[i] = NULL;
    }

    for (uint64_t i = 0; i < pFileHeader->tracer_count; i++) {
        uint8_t tracerId = pFileHeader->tracer_id_array[i].id;
        tidApi = tracerId;

        const VKTRACE_TRACER_REPLAYER_INFO* pReplayerInfo = &(gs_tracerReplayerInfo[tracerId]);

        if (pReplayerInfo->tracerId != tracerId) {
            vktrace_LogError("Replayer info for TracerId (%d) failed consistency check.", tracerId);
            assert(!"TracerId in VKTRACE_TRACER_REPLAYER_INFO does not match the requested tracerId. The array needs to be corrected.");
        } else if (pReplayerInfo->needsReplayer == TRUE) {
            // Have our factory create the necessary replayer
            replayer[tracerId] = makeReplayer.Create(tracerId);

            if (replayer[tracerId] == NULL) {
                // replayer failed to be created
                if (pAllSettings != NULL) {
                    vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
                }
                fclose(tracefp);
                vktrace_free(pTraceFile);
                vktrace_free(traceFile);
                if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
                vktrace_free(pFileHeader);
                return -1;
            }

            // merge the replayer's settings into the list of all settings so that we can output a comprehensive settings file later
            // on.
            vktrace_SettingGroup_merge(replayer[tracerId]->GetSettings(), &pAllSettings, &numAllSettings);

            // update the replayer with the loaded settings
            replayer[tracerId]->UpdateFromSettings(pAllSettings, numAllSettings);

            // Initialize the replayer
            err = replayer[tracerId]->Initialize(&disp, &replaySettings, pFileHeader);
            if (err) {
                vktrace_LogError("Couldn't Initialize replayer for TracerId %d.", tracerId);
                if (pAllSettings != NULL) {
                    vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
                }
                fclose(tracefp);
                vktrace_free(pTraceFile);
                vktrace_free(traceFile);
                if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
                vktrace_free(pFileHeader);
                return err;
            }
        }
    }

    if (tidApi == VKTRACE_TID_RESERVED) {
        vktrace_LogError("No API specified in tracefile for replaying.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
        vktrace_free(pFileHeader);
        return -1;
    }

    // main loop
    uint64_t filesize = (pFileHeader->compress_type == VKTRACE_COMPRESS_TYPE_NONE) ? traceFile->mFileLen : fileHeader.decompress_file_size;
    Sequencer sequencer(traceFile, g_decompressor, filesize);
    err = vktrace_replay::main_loop(disp, sequencer, replayer);

    for (int i = 0; i < VKTRACE_MAX_TRACER_ID_ARRAY_SIZE; i++) {
        if (replayer[i] != NULL) {
            replayer[i]->Deinitialize();
            makeReplayer.Destroy(&replayer[i]);
        }
    }

    if (pAllSettings != NULL) {
        vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
    }

    fclose(tracefp);
    vktrace_free(pTraceFile);
    vktrace_free(traceFile);
    if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
    vktrace_free(pFileHeader);
    if (!err && tmpfilename.length() > 0) {
        remove(tmpfilename.c_str());
    }

    return err;
}

#if defined(ANDROID)
static bool initialized = false;
static bool active = true;

// Convert Intents to argv
// Ported from Hologram sample, only difference is flexible key
std::vector<std::string> get_args(android_app& app, const char* intent_extra_data_key) {
    std::vector<std::string> args;
    JavaVM& vm = *app.activity->vm;
    JNIEnv* p_env;
    if (vm.AttachCurrentThread(&p_env, nullptr) != JNI_OK) return args;

    JNIEnv& env = *p_env;
    jobject activity = app.activity->clazz;
    jmethodID get_intent_method = env.GetMethodID(env.GetObjectClass(activity), "getIntent", "()Landroid/content/Intent;");
    jobject intent = env.CallObjectMethod(activity, get_intent_method);
    jmethodID get_string_extra_method =
        env.GetMethodID(env.GetObjectClass(intent), "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    jvalue get_string_extra_args;
    get_string_extra_args.l = env.NewStringUTF(intent_extra_data_key);
    jstring extra_str = static_cast<jstring>(env.CallObjectMethodA(intent, get_string_extra_method, &get_string_extra_args));

    std::string args_str;
    if (extra_str) {
        const char* extra_utf = env.GetStringUTFChars(extra_str, nullptr);
        args_str = extra_utf;
        env.ReleaseStringUTFChars(extra_str, extra_utf);
        env.DeleteLocalRef(extra_str);
    }

    env.DeleteLocalRef(get_string_extra_args.l);
    env.DeleteLocalRef(intent);
    vm.DetachCurrentThread();

    // split args_str
    std::stringstream ss(args_str);
    std::string arg;
    while (std::getline(ss, arg, ' ')) {
        if (!arg.empty()) args.push_back(arg);
    }

    return args;
}

static int32_t processInput(struct android_app* app, AInputEvent* event) {
    if ((app->userData != nullptr) && (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION)) {
        vkDisplayAndroid* display = reinterpret_cast<vkDisplayAndroid*>(app->userData);

        // TODO: Distinguish between tap and swipe actions; swipe to advance to next frame when paused.
        int32_t action = AMotionEvent_getAction(event);
        if (action == AMOTION_EVENT_ACTION_UP) {
            display->set_pause_status(!display->get_pause_status());
            return 1;
        }
    }

    return 0;
}

static void processCommand(struct android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: {
            if (app->window) {
                initialized = true;
            }
            break;
        }
        case APP_CMD_GAINED_FOCUS: {
            active = true;
            break;
        }
        case APP_CMD_LOST_FOCUS: {
            active = false;
            break;
        }
    }
}

// Start with carbon copy of main() and convert it to support Android, then diff them and move common code to helpers.
void android_main(struct android_app* app) {
    const char* appTag = "vkreplay";

    // This will be set by the vkDisplay object.
    app->userData = nullptr;

    int vulkanSupport = InitVulkan();
    if (vulkanSupport == 0) {
        __android_log_print(ANDROID_LOG_ERROR, appTag, "No Vulkan support found");
        return;
    }

    app->onAppCmd = processCommand;
    app->onInputEvent = processInput;

    while (1) {
        int events;
        struct android_poll_source* source;
        while (ALooper_pollAll(active ? 0 : -1, NULL, &events, (void**)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }

            if (app->destroyRequested != 0) {
                // anything to clean up?
                return;
            }
        }

        if (initialized && active) {
            // Parse Intents into argc, argv
            // Use the following key to send arguments to gtest, i.e.
            // --es args "-v\ debug\ -t\ /sdcard/cube0.vktrace"
            const char key[] = "args";
            std::vector<std::string> args = get_args(*app, key);

            int argc = args.size() + 1;

            char** argv = (char**)malloc(argc * sizeof(char*));
            argv[0] = (char*)"vkreplay";
            for (int i = 0; i < args.size(); i++) argv[i + 1] = (char*)args[i].c_str();

            __android_log_print(ANDROID_LOG_INFO, appTag, "argc = %i", argc);
            for (int i = 0; i < argc; i++) __android_log_print(ANDROID_LOG_INFO, appTag, "argv[%i] = %s", i, argv[i]);

            // sleep to allow attaching debugger
            // sleep(10);

            auto pDisp = new vkDisplayAndroid(app);

            // Call into common code
            int err = vkreplay_main(argc, argv, pDisp);
            __android_log_print(ANDROID_LOG_DEBUG, appTag, "vkreplay_main returned %i", err);

            ANativeActivity_finish(app->activity);
            free(argv);

            // Kill the process
            // This is not a necessarily good practice.  But it works to make sure the process is killed after replaying a trace
            // file.  So user will not need to run "adb shell am force-stop come.example.vkreplay" afterwards.
            exit(err);
        }
    }
}

#else  // ANDROID

extern "C" int main(int argc, char** argv) { return vkreplay_main(argc, argv); }

#endif
