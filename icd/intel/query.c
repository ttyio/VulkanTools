/*
 *
 * Copyright (C) 2015-2016 Valve Corporation
 * Copyright (C) 2015-2016 LunarG, Inc.
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
 * Author: Chia-I Wu <olvaffe@gmail.com>
 * Author: Courtney Goeltzenleuchter <courtney@LunarG.com>
 *
 */

#include "dev.h"
#include "mem.h"
#include "query.h"
#include "genhw/genhw.h"

static void query_destroy(struct intel_obj *obj)
{
    struct intel_query *query = intel_query_from_obj(obj);

    intel_mem_free(obj->mem);
    intel_query_destroy(query);
}

static void query_init_pipeline_statistics(
        struct intel_dev *dev,
        const VkQueryPoolCreateInfo *info,
        struct intel_query *query)
{
    /*
     * Note: order defined by Vulkan spec.
     */
    const uint32_t regs[][2] = {
        {VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT, GEN6_REG_IA_PRIMITIVES_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT, GEN6_REG_VS_INVOCATION_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT, GEN6_REG_GS_INVOCATION_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT, GEN6_REG_GS_PRIMITIVES_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT, GEN6_REG_CL_INVOCATION_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT, GEN6_REG_CL_PRIMITIVES_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT, GEN6_REG_PS_INVOCATION_COUNT},
        {VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT, (intel_gpu_gen(dev->gpu) >= INTEL_GEN(7)) ? GEN7_REG_HS_INVOCATION_COUNT : 0},
        {VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT, (intel_gpu_gen(dev->gpu) >= INTEL_GEN(7)) ? GEN7_REG_DS_INVOCATION_COUNT : 0},
        {VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT, 0}
    };
    STATIC_ASSERT(ARRAY_SIZE(regs) < 32);
    uint32_t i;
    uint32_t reg_count = 0;

    /*
     * Only query registers indicated via pipeline statistics flags.
     * If HW does not support a flag, fill value with 0.
     */
    for (i=0; i < ARRAY_SIZE(regs); i++) {
        if ((regs[i][0] & info->pipelineStatistics)) {
            query->regs[reg_count] = regs[i][1];
            reg_count++;
        }
    }

    query->reg_count = reg_count;
    query->slot_stride = u_align(reg_count * sizeof(uint64_t) * 2, 64);
}

VkResult intel_query_create(struct intel_dev *dev,
                            const VkQueryPoolCreateInfo *info,
                            struct intel_query **query_ret)
{
    struct intel_query *query;

    query = (struct intel_query *) intel_base_create(&dev->base.handle,
            sizeof(*query), dev->base.dbg, VK_DEBUG_REPORT_OBJECT_TYPE_QUEUE_EXT,
            info, 0);
    if (!query)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    query->type = info->queryType;
    query->slot_count = info->queryCount;

    /*
     * For each query type, the GPU will be asked to write the values of some
     * registers to a buffer before and after a sequence of commands.  We will
     * compare the differences to get the query results.
     */
    switch (info->queryType) {
    case VK_QUERY_TYPE_OCCLUSION:
        query->slot_stride = u_align(sizeof(uint64_t) * 2, 64);
        break;
    case VK_QUERY_TYPE_PIPELINE_STATISTICS:
        query_init_pipeline_statistics(dev, info, query);
        break;
    case VK_QUERY_TYPE_TIMESTAMP:
        query->slot_stride = u_align(sizeof(uint64_t), 64);
        break;
    default:
        assert(!"unknown query type");
        break;
    }

    VkMemoryAllocateInfo mem_reqs;
    mem_reqs.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem_reqs.allocationSize = query->slot_stride * query->slot_count;
    mem_reqs.pNext = NULL;
    mem_reqs.memoryTypeIndex = 0;
    intel_mem_alloc(dev, &mem_reqs, &query->obj.mem);

    query->obj.destroy = query_destroy;

    *query_ret = query;

    return VK_SUCCESS;
}

void intel_query_destroy(struct intel_query *query)
{
    intel_base_destroy(&query->obj.base);
}

static void
query_process_occlusion(const struct intel_query *query,
                        uint32_t count, const uint8_t *raw,
                        uint64_t *results)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        const uint64_t *pair = (const uint64_t *) raw;

        results[i] = pair[1] - pair[0];
        raw += query->slot_stride;
    }
}

static void
query_process_pipeline_statistics(const struct intel_query *query,
                                  uint32_t count, const uint8_t *raw,
                                  void *results)
{
    const uint32_t num_regs = query->reg_count;
    uint32_t i, j;

    for (i = 0; i < count; i++) {
        const uint64_t *before = (const uint64_t *) raw;
        const uint64_t *after = before + num_regs;
        uint64_t *dst = (uint64_t *) (results + i);

        for (j = 0; j < num_regs; j++)
            dst[j] = after[j] - before[j];

        raw += query->slot_stride;
    }
}

static void
query_process_timestamp(const struct intel_query *query,
                        uint32_t count, const uint8_t *raw,
                        uint64_t *results)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        const uint64_t *ts = (const uint64_t *) raw;

        results[i] = *ts;
        raw += query->slot_stride;
    }
}

VkResult intel_query_get_results(struct intel_query *query,
                                 uint32_t slot_start, uint32_t slot_count,
                                 void *results)
{
    const uint8_t *ptr;

    if (intel_mem_is_busy(query->obj.mem))
        return VK_NOT_READY;

    ptr = (const uint8_t *) intel_mem_map_sync(query->obj.mem, false);
    if (!ptr)
        return VK_ERROR_MEMORY_MAP_FAILED;

    ptr += query->obj.offset + query->slot_stride * slot_start;

    switch (query->type) {
    case VK_QUERY_TYPE_OCCLUSION:
        query_process_occlusion(query, slot_count, ptr, results);
        break;
    case VK_QUERY_TYPE_PIPELINE_STATISTICS:
        query_process_pipeline_statistics(query, slot_count, ptr, results);
        break;
    case VK_QUERY_TYPE_TIMESTAMP:
        query_process_timestamp(query, slot_count, ptr, results);
        break;
    default:
        assert(0);
        break;
    }

    intel_mem_unmap(query->obj.mem);

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(
    VkDevice                                    device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                     pAllocator,
    VkQueryPool*                                pQueryPool)
{
    struct intel_dev *dev = intel_dev(device);

    return intel_query_create(dev, pCreateInfo,
            (struct intel_query **) pQueryPool);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(
    VkDevice                                    device,
    VkQueryPool                                 queryPool,
    const VkAllocationCallbacks*                     pAllocator)

 {
    struct intel_obj *obj = intel_obj(queryPool);

    obj->destroy(obj);
 }

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice                                    device,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    size_t                                      dataSize,
    void*                                       pData,
    size_t                                      stride,
    VkQueryResultFlags                          flags)
{
    struct intel_query *query = intel_query(queryPool);

    if (pData)
        return intel_query_get_results(query, firstQuery, queryCount, pData);
    else
        return VK_SUCCESS;
}
