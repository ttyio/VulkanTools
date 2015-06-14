/*
 * Vulkan
 *
 * Copyright (C) 2015 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Jon Ashburn <jon@lunarg.com>
 *   Courtney Goeltzenleuchter <courtney@lunarg.com>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <alloca.h>
#include "debug_report.h"
#include "vkLayer.h"

typedef void (VKAPI *PFN_stringCallback)(char *message);

static const struct loader_extension_property debug_report_extension_info = {
    .info =  {
        .sType = VK_STRUCTURE_TYPE_EXTENSION_PROPERTIES,
        .name = DEBUG_REPORT_EXTENSION_NAME,
        .version = VK_DEBUG_REPORT_EXTENSION_VERSION,
        .description = "loader: debug report extension",
        },
    .origin = VK_EXTENSION_ORIGIN_LOADER,
};

void debug_report_add_instance_extensions(
        struct loader_extension_list *ext_list)
{
    loader_add_to_ext_list(ext_list, 1, &debug_report_extension_info);
}

void debug_report_create_instance(
        struct loader_instance *ptr_instance)
{
    ptr_instance->debug_report_enabled = has_vk_extension_property(
                                             &debug_report_extension_info.info,
                                             &ptr_instance->enabled_instance_extensions);
}

static VkResult debug_report_DbgCreateMsgCallback(
        VkInstance instance,
        VkFlags msgFlags,
        const PFN_vkDbgMsgCallback pfnMsgCallback,
        void* pUserData,
        VkDbgMsgCallback* pMsgCallback)
{
    VkLayerDbgFunctionNode *pNewDbgFuncNode = (VkLayerDbgFunctionNode *) malloc(sizeof(VkLayerDbgFunctionNode));
    if (!pNewDbgFuncNode)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    struct loader_instance *inst = loader_instance(instance);
    loader_platform_thread_lock_mutex(&loader_lock);
    VkResult result = inst->disp->DbgCreateMsgCallback(instance, msgFlags, pfnMsgCallback, pUserData, pMsgCallback);
    if (result == VK_SUCCESS) {
        pNewDbgFuncNode->msgCallback = *pMsgCallback;
        pNewDbgFuncNode->pfnMsgCallback = pfnMsgCallback;
        pNewDbgFuncNode->msgFlags = msgFlags;
        pNewDbgFuncNode->pUserData = pUserData;
        pNewDbgFuncNode->pNext = inst->DbgFunctionHead;
        inst->DbgFunctionHead = pNewDbgFuncNode;
    } else {
        free(pNewDbgFuncNode);
    }
    loader_platform_thread_unlock_mutex(&loader_lock);
    return result;
}

static VkResult debug_report_DbgDestroyMsgCallback(
        VkInstance instance,
        VkDbgMsgCallback msg_callback)
{
    struct loader_instance *inst = loader_instance(instance);
    loader_platform_thread_lock_mutex(&loader_lock);
    VkLayerDbgFunctionNode *pTrav = inst->DbgFunctionHead;
    VkLayerDbgFunctionNode *pPrev = pTrav;

    VkResult result = inst->disp->DbgDestroyMsgCallback(instance, msg_callback);

    while (pTrav) {
        if (pTrav->msgCallback == msg_callback) {
            pPrev->pNext = pTrav->pNext;
            if (inst->DbgFunctionHead == pTrav)
                inst->DbgFunctionHead = pTrav->pNext;
            free(pTrav);
            break;
        }
        pPrev = pTrav;
        pTrav = pTrav->pNext;
    }

    loader_platform_thread_unlock_mutex(&loader_lock);
    return result;
}


/*
 * This is the instance chain terminator function
 * for DbgCreateMsgCallback
 */
VkResult loader_DbgCreateMsgCallback(
        VkInstance                          instance,
        VkFlags                             msgFlags,
        const PFN_vkDbgMsgCallback          pfnMsgCallback,
        const void*                         pUserData,
        VkDbgMsgCallback*                   pMsgCallback)
{
    VkDbgMsgCallback *icd_info;
    const struct loader_icd *icd;
    struct loader_instance *inst;
    VkResult res;
    uint32_t storage_idx;

    if (instance == VK_NULL_HANDLE)
        return VK_ERROR_INVALID_HANDLE;

    assert(loader.icds_scanned);

    for (inst = loader.instances; inst; inst = inst->next) {
        if ((VkInstance) inst == instance)
            break;
    }

    if (inst == VK_NULL_HANDLE)
        return VK_ERROR_INVALID_HANDLE;

    icd_info = calloc(sizeof(VkDbgMsgCallback), inst->total_icd_count);
    if (!icd_info) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    storage_idx = 0;
    for (icd = inst->icds; icd; icd = icd->next) {
        if (!icd->DbgCreateMsgCallback) {
            continue;
        }

        res = icd->DbgCreateMsgCallback(
                  icd->instance,
                  msgFlags,
                  pfnMsgCallback,
                  pUserData,
                  &icd_info[storage_idx]);

        if (res != VK_SUCCESS) {
            break;
        }
        storage_idx++;
    }

    /* roll back on errors */
    if (icd) {
        storage_idx = 0;
        for (icd = inst->icds; icd; icd = icd->next) {
            if (icd_info[storage_idx]) {
                icd->DbgDestroyMsgCallback(
                      icd->instance,
                      icd_info[storage_idx]);
            }
            storage_idx++;
        }

        return res;
    }

    *pMsgCallback = (VkDbgMsgCallback) icd_info;

    return VK_SUCCESS;
}

/*
 * This is the instance chain terminator function
 * for DbgDestroyMsgCallback
 */
VkResult loader_DbgDestroyMsgCallback(
        VkInstance instance,
        VkDbgMsgCallback msgCallback)
{
    uint32_t storage_idx;
    VkDbgMsgCallback *icd_info;
    const struct loader_icd *icd;
    VkResult res = VK_SUCCESS;
    struct loader_instance *inst;

    if (instance == VK_NULL_HANDLE)
        return VK_ERROR_INVALID_HANDLE;

    assert(loader.icds_scanned);

    for (inst = loader.instances; inst; inst = inst->next) {
        if ((VkInstance) inst == instance)
            break;
    }

    if (inst == VK_NULL_HANDLE)
        return VK_ERROR_INVALID_HANDLE;

    icd_info = (VkDbgMsgCallback *) msgCallback;
    storage_idx = 0;
    for (icd = inst->icds; icd; icd = icd->next) {
        if (icd_info[storage_idx]) {
            icd->DbgDestroyMsgCallback(
                  icd->instance,
                  icd_info[storage_idx]);
        }
        storage_idx++;
    }
    return res;
}

static void print_msg_flags(VkFlags msgFlags, char *msg_flags)
{
    bool separator = false;

    msg_flags[0] = 0;
    if (msgFlags & VK_DBG_REPORT_DEBUG_BIT) {
        strcat(msg_flags, "DEBUG");
        separator = true;
    }
    if (msgFlags & VK_DBG_REPORT_INFO_BIT) {
        if (separator) strcat(msg_flags, ",");
        strcat(msg_flags, "INFO");
        separator = true;
    }
    if (msgFlags & VK_DBG_REPORT_WARN_BIT) {
        if (separator) strcat(msg_flags, ",");
        strcat(msg_flags, "WARN");
        separator = true;
    }
    if (msgFlags & VK_DBG_REPORT_PERF_WARN_BIT) {
        if (separator) strcat(msg_flags, ",");
        strcat(msg_flags, "PERF");
        separator = true;
    }
    if (msgFlags & VK_DBG_REPORT_ERROR_BIT) {
        if (separator) strcat(msg_flags, ",");
        strcat(msg_flags, "ERROR");
    }
}

// DebugReport utility callback functions
static void VKAPI StringCallback(
    VkFlags                             msgFlags,
    VkObjectType                        objType,
    VkObject                            srcObject,
    size_t                              location,
    int32_t                             msgCode,
    const char*                         pLayerPrefix,
    const char*                         pMsg,
    void*                               pUserData)
{
    uint32_t buf_size;
    char *buf;
    char msg_flags[30];
    PFN_stringCallback callback = (PFN_stringCallback) pUserData;

    print_msg_flags(msgFlags, msg_flags);

    buf_size = strlen(msg_flags) + /* ReportFlags: i.e. (DEBUG,INFO,WARN,PERF,ERROR) */
               20 +  /* objType */
               20 + /* srcObject */
               20 + /* location */
               20 + /* msgCode */
               strlen(pLayerPrefix) +
               strlen(pMsg) +
               50 /* other / whitespace */;
    buf = alloca(buf_size);

    snprintf(buf, buf_size, "%s (%s): object: 0x%" PRIxLEAST64 " type: %d location: %zu msgCode: %d: %s",
             pLayerPrefix, msg_flags, srcObject, objType, location, msgCode, pMsg);
    callback(buf);
}

static void VKAPI StdioCallback(
    VkFlags                             msgFlags,
    VkObjectType                        objType,
    VkObject                            srcObject,
    size_t                              location,
    int32_t                             msgCode,
    const char*                         pLayerPrefix,
    const char*                         pMsg,
    void*                               pUserData)
{
    char msg_flags[30];

    print_msg_flags(msgFlags, msg_flags);

    fprintf((FILE *) pUserData, "%s(%s): object: 0x%" PRIxLEAST64 " type: %d location: %zu msgCode: %d: %s",
             pLayerPrefix, msg_flags, srcObject, objType, location, msgCode, pMsg);
}

static void VKAPI BreakCallback(
    VkFlags                             msgFlags,
    VkObjectType                        objType,
    VkObject                            srcObject,
    size_t                              location,
    int32_t                             msgCode,
    const char*                         pLayerPrefix,
    const char*                         pMsg,
    void*                               pUserData)
{

}

void *debug_report_instance_gpa(
        struct loader_instance *ptr_instance,
        const char* name)
{
    if (ptr_instance == VK_NULL_HANDLE || !ptr_instance->debug_report_enabled)
        return NULL;

    if (!strcmp("vkDbgCreateMsgCallback", name))
        return (void *) debug_report_DbgCreateMsgCallback;
    else if (!strcmp("vkDbgDestroyMsgCallback", name))
        return (void *) debug_report_DbgDestroyMsgCallback;
    else if (!strcmp("vkDbgStringCallback", name))
        return (void *) StringCallback;
    else if (!strcmp("vkDbgStdioCallback", name))
        return (void *) StdioCallback;
    else if (!strcmp("vkDbgBreakCallback", name))
        return (void *) BreakCallback;

    return NULL;
}