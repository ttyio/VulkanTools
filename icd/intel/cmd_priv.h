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
 * Author: Chia-I Wu <olv@lunarg.com>
 * Author: Chris Forbes <chrisf@ijw.co.nz>
 *
 */

#ifndef CMD_PRIV_H
#define CMD_PRIV_H

#include "genhw/genhw.h"
#include "dev.h"
#include "fb.h"
#include "gpu.h"
#include "cmd.h"

#define CMD_ASSERT(cmd, min_gen, max_gen) \
    INTEL_GPU_ASSERT((cmd)->dev->gpu, (min_gen), (max_gen))

enum intel_cmd_item_type {
    /* for state buffer */
    INTEL_CMD_ITEM_BLOB,
    INTEL_CMD_ITEM_CLIP_VIEWPORT,
    INTEL_CMD_ITEM_SF_VIEWPORT,
    INTEL_CMD_ITEM_SCISSOR_RECT,
    INTEL_CMD_ITEM_CC_VIEWPORT,
    INTEL_CMD_ITEM_COLOR_CALC,
    INTEL_CMD_ITEM_DEPTH_STENCIL,
    INTEL_CMD_ITEM_BLEND,
    INTEL_CMD_ITEM_SAMPLER,

    /* for surface buffer */
    INTEL_CMD_ITEM_SURFACE,
    INTEL_CMD_ITEM_BINDING_TABLE,

    /* for instruction buffer */
    INTEL_CMD_ITEM_KERNEL,

    INTEL_CMD_ITEM_COUNT,
};

struct intel_cmd_item {
    enum intel_cmd_item_type type;
    size_t offset;
    size_t size;
};

#define INTEL_CMD_RELOC_TARGET_IS_WRITER (1u << 31)
struct intel_cmd_reloc {
    enum intel_cmd_writer_type which;
    size_t offset;

    intptr_t target;
    uint32_t target_offset;

    uint32_t flags;
};

struct intel_att_view;

enum intel_cmd_meta_mode {
    /*
     * Draw POINTLIST of (width * height) vertices with only VS enabled.  The
     * vertex id is from 0 to (width * height - 1).
     */
    INTEL_CMD_META_VS_POINTS,

    /*
     * Draw a RECTLIST from (dst.x, dst.y) to (dst.x + width, dst.y + height)
     * with only FS enabled.
     */
    INTEL_CMD_META_FS_RECT,

    /*
     * Draw a RECTLIST from (dst.x, dst.y) to (dst.x + width, dst.y + height)
     * with only depth/stencil enabled.
     */
    INTEL_CMD_META_DEPTH_STENCIL_RECT,
};

enum intel_cmd_meta_ds_op {
    INTEL_CMD_META_DS_NOP,
    INTEL_CMD_META_DS_HIZ_CLEAR,
    INTEL_CMD_META_DS_HIZ_RESOLVE,
    INTEL_CMD_META_DS_RESOLVE,
};

struct intel_cmd_meta {
    enum intel_cmd_meta_mode mode;
    enum intel_dev_meta_shader shader_id;

    struct {
        bool valid;

        uint32_t surface[8];
        uint32_t surface_len;

        intptr_t reloc_target;
        uint32_t reloc_offset;
        uint32_t reloc_flags;

        uint32_t lod, layer;
        uint32_t x, y;
    } src, dst;

    struct {
        struct intel_att_view view;
        uint32_t stencil_ref;
        /* Using VkImageAspectFlagBits as that means we
         * are expecting only one bit to be set at a time */
        VkImageAspectFlagBits aspect;

        enum intel_cmd_meta_ds_op op;
        bool optimal;
    } ds;

    uint32_t clear_val[4];

    uint32_t width, height;
    uint32_t sample_count;
};

static inline int cmd_gen(const struct intel_cmd *cmd)
{
    return intel_gpu_gen(cmd->dev->gpu);
}

static inline void cmd_fail(struct intel_cmd *cmd, VkResult result)
{
    intel_dev_log(cmd->dev, VK_DEBUG_REPORT_ERROR_BIT_EXT,
                  &cmd->obj.base, 0, 0,
                  "command building error");

    cmd->result = result;
}

static inline void cmd_reserve_reloc(struct intel_cmd *cmd,
                                     uint32_t reloc_len)
{
    /* fail silently */
    if (cmd->reloc_used + reloc_len > cmd->reloc_count) {
        cmd->reloc_used = 0;
        cmd_fail(cmd, VK_ERROR_VALIDATION_FAILED_EXT);
    }
    assert(cmd->reloc_used + reloc_len <= cmd->reloc_count);
}

void cmd_writer_grow(struct intel_cmd *cmd,
                     enum intel_cmd_writer_type which,
                     size_t new_size);

void cmd_writer_record(struct intel_cmd *cmd,
                       enum intel_cmd_writer_type which,
                       enum intel_cmd_item_type type,
                       size_t offset, size_t size);

/**
 * Return an offset to a region that is aligned to \p alignment and has at
 * least \p size bytes.
 */
static inline size_t cmd_writer_reserve(struct intel_cmd *cmd,
                                        enum intel_cmd_writer_type which,
                                        size_t alignment, size_t size)
{
    struct intel_cmd_writer *writer = &cmd->writers[which];
    size_t offset;

    assert(alignment && u_is_pow2(alignment));
    offset = u_align(writer->used, alignment);

    if (offset + size > writer->size) {
        cmd_writer_grow(cmd, which, offset + size);
        /* align again in case of errors */
        offset = u_align(writer->used, alignment);

        assert(offset + size <= writer->size);
    }

    return offset;
}

/**
 * Add a reloc at \p pos.  No error checking.
 */
static inline void cmd_writer_reloc(struct intel_cmd *cmd,
                                    enum intel_cmd_writer_type which,
                                    size_t offset, intptr_t target,
                                    uint32_t target_offset, uint32_t flags)
{
    struct intel_cmd_reloc *reloc = &cmd->relocs[cmd->reloc_used];

    assert(cmd->reloc_used < cmd->reloc_count);

    reloc->which = which;
    reloc->offset = offset;
    reloc->target = target;
    reloc->target_offset = target_offset;
    reloc->flags = flags;

    cmd->reloc_used++;
}

/**
 * Reserve a region from the state buffer.  The offset, in bytes, to the
 * reserved region is returned.
 *
 * Note that \p alignment is in bytes and \p len is in DWords.
 */
static inline uint32_t cmd_state_reserve(struct intel_cmd *cmd,
                                         enum intel_cmd_item_type item,
                                         size_t alignment, uint32_t len)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_STATE;
    const size_t size = len << 2;
    const size_t offset = cmd_writer_reserve(cmd, which, alignment, size);
    struct intel_cmd_writer *writer = &cmd->writers[which];

    /* all states are at least aligned to 32-bytes */
    assert(alignment % 32 == 0);

    writer->used = offset + size;

    if (intel_debug & (INTEL_DEBUG_BATCH | INTEL_DEBUG_HANG))
        cmd_writer_record(cmd, which, item, offset, size);

    return offset;
}

/**
 * Get the pointer to a reserved region for updating.  The pointer is only
 * valid until the next reserve call.
 */
static inline void cmd_state_update(struct intel_cmd *cmd,
                                    uint32_t offset, uint32_t len,
                                    uint32_t **dw)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_STATE;
    struct intel_cmd_writer *writer = &cmd->writers[which];

    assert(offset + (len << 2) <= writer->used);

    *dw = (uint32_t *) ((char *) writer->ptr + offset);
}

/**
 * Reserve a region from the state buffer.  Both the offset, in bytes, and the
 * pointer to the reserved region are returned.  The pointer is only valid
 * until the next reserve call.
 *
 * Note that \p alignment is in bytes and \p len is in DWords.
 */
static inline uint32_t cmd_state_pointer(struct intel_cmd *cmd,
                                         enum intel_cmd_item_type item,
                                         size_t alignment, uint32_t len,
                                         uint32_t **dw)
{
    const uint32_t offset = cmd_state_reserve(cmd, item, alignment, len);

    cmd_state_update(cmd, offset, len, dw);

    return offset;
}

/**
 * Write a dynamic state to the state buffer.
 */
static inline uint32_t cmd_state_write(struct intel_cmd *cmd,
                                       enum intel_cmd_item_type item,
                                       size_t alignment, uint32_t len,
                                       const uint32_t *dw)
{
    uint32_t offset, *dst;

    offset = cmd_state_pointer(cmd, item, alignment, len, &dst);
    memcpy(dst, dw, len << 2);

    return offset;
}

/**
 * Write a surface state to the surface buffer.  The offset, in bytes, of the
 * state is returned.
 *
 * Note that \p alignment is in bytes and \p len is in DWords.
 */
static inline uint32_t cmd_surface_write(struct intel_cmd *cmd,
                                         enum intel_cmd_item_type item,
                                         size_t alignment, uint32_t len,
                                         const uint32_t *dw)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_SURFACE;
    const size_t size = len << 2;
    const uint32_t offset = cmd_writer_reserve(cmd, which, alignment, size);
    struct intel_cmd_writer *writer = &cmd->writers[which];
    uint32_t *dst;

    assert(item == INTEL_CMD_ITEM_SURFACE ||
           item == INTEL_CMD_ITEM_BINDING_TABLE);

    /* all states are at least aligned to 32-bytes */
    assert(alignment % 32 == 0);

    writer->used = offset + size;

    if (intel_debug & INTEL_DEBUG_BATCH)
        cmd_writer_record(cmd, which, item, offset, size);

    dst = (uint32_t *) ((char *) writer->ptr + offset);
    memcpy(dst, dw, size);

    return offset;
}

/**
 * Add a relocation entry for a DWord of a surface state.
 */
static inline void cmd_surface_reloc(struct intel_cmd *cmd,
                                     uint32_t offset, uint32_t dw_index,
                                     struct intel_bo *bo,
                                     uint32_t bo_offset, uint32_t reloc_flags)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_SURFACE;

    cmd_writer_reloc(cmd, which, offset + (dw_index << 2),
            (intptr_t) bo, bo_offset, reloc_flags);
}

static inline void cmd_surface_reloc_writer(struct intel_cmd *cmd,
                                            uint32_t offset, uint32_t dw_index,
                                            enum intel_cmd_writer_type writer,
                                            uint32_t writer_offset)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_SURFACE;

    cmd_writer_reloc(cmd, which, offset + (dw_index << 2),
            (intptr_t) writer, writer_offset,
            INTEL_CMD_RELOC_TARGET_IS_WRITER);
}

/**
 * Write a kernel to the instruction buffer.  The offset, in bytes, of the
 * kernel is returned.
 */
static inline uint32_t cmd_instruction_write(struct intel_cmd *cmd,
                                             size_t size,
                                             const void *kernel)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_INSTRUCTION;
    /*
     * From the Sandy Bridge PRM, volume 4 part 2, page 112:
     *
     *     "Due to prefetch of the instruction stream, the EUs may attempt to
     *      access up to 8 instructions (128 bytes) beyond the end of the
     *      kernel program - possibly into the next memory page.  Although
     *      these instructions will not be executed, software must account for
     *      the prefetch in order to avoid invalid page access faults."
     */
    const size_t reserved_size = size + 128;
    /* kernels are aligned to 64 bytes */
    const size_t alignment = 64;
    const size_t offset = cmd_writer_reserve(cmd,
            which, alignment, reserved_size);
    struct intel_cmd_writer *writer = &cmd->writers[which];

    memcpy((char *) writer->ptr + offset, kernel, size);

    writer->used = offset + size;

    if (intel_debug & (INTEL_DEBUG_BATCH | INTEL_DEBUG_HANG))
        cmd_writer_record(cmd, which, INTEL_CMD_ITEM_KERNEL, offset, size);

    return offset;
}

/**
 * Reserve a region from the batch buffer.  Both the offset, in DWords, and
 * the pointer to the reserved region are returned.  The pointer is only valid
 * until the next reserve call.
 *
 * Note that \p len is in DWords.
 */
static inline uint32_t cmd_batch_pointer(struct intel_cmd *cmd,
                                         uint32_t len, uint32_t **dw)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_BATCH;
    /*
     * We know the batch bo is always aligned.  Using 1 here should allow the
     * compiler to optimize away aligning.
     */
    const size_t alignment = 1;
    const size_t size = len << 2;
    const size_t offset = cmd_writer_reserve(cmd, which, alignment, size);
    struct intel_cmd_writer *writer = &cmd->writers[which];

    assert(offset % 4 == 0);
    *dw = (uint32_t *) ((char *) writer->ptr + offset);

    writer->used = offset + size;

    return offset >> 2;
}

/**
 * Write a command to the batch buffer.
 */
static inline uint32_t cmd_batch_write(struct intel_cmd *cmd,
                                       uint32_t len, const uint32_t *dw)
{
    uint32_t pos;
    uint32_t *dst;

    pos = cmd_batch_pointer(cmd, len, &dst);
    memcpy(dst, dw, len << 2);

    return pos;
}

/**
 * Add a relocation entry for a DWord of a command.
 */
static inline void cmd_batch_reloc(struct intel_cmd *cmd, uint32_t pos,
                                   struct intel_bo *bo,
                                   uint32_t bo_offset, uint32_t reloc_flags)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_BATCH;

    cmd_writer_reloc(cmd, which, pos << 2, (intptr_t) bo, bo_offset, reloc_flags);
}

static inline void cmd_batch_reloc_writer(struct intel_cmd *cmd, uint32_t pos,
                                          enum intel_cmd_writer_type writer,
                                          uint32_t writer_offset)
{
    const enum intel_cmd_writer_type which = INTEL_CMD_WRITER_BATCH;

    cmd_writer_reloc(cmd, which, pos << 2, (intptr_t) writer, writer_offset,
            INTEL_CMD_RELOC_TARGET_IS_WRITER);
}

void cmd_batch_state_base_address(struct intel_cmd *cmd);
void cmd_batch_push_const_alloc(struct intel_cmd *cmd);

/**
 * Begin the batch buffer.
 */
static inline void cmd_batch_begin(struct intel_cmd *cmd)
{
    cmd_batch_state_base_address(cmd);
    cmd_batch_push_const_alloc(cmd);
}

/**
 * End the batch buffer.
 */
static inline void cmd_batch_end(struct intel_cmd *cmd)
{
    struct intel_cmd_writer *writer = &cmd->writers[INTEL_CMD_WRITER_BATCH];
    uint32_t *dw;

    if (writer->used & 0x7) {
        cmd_batch_pointer(cmd, 1, &dw);
        dw[0] = GEN6_MI_CMD(MI_BATCH_BUFFER_END);
    } else {
        cmd_batch_pointer(cmd, 2, &dw);
        dw[0] = GEN6_MI_CMD(MI_BATCH_BUFFER_END);
        dw[1] = GEN6_MI_CMD(MI_NOOP);
    }
}

static inline void cmd_begin_render_pass(struct intel_cmd *cmd,
                                         const struct intel_render_pass *rp,
                                         const struct intel_fb *fb,
                                         const uint32_t sp,
                                         VkSubpassContents contents)
{
    assert(sp < rp->subpass_count);

    cmd->bind.render_pass_changed = true;
    cmd->bind.render_pass = rp;
    cmd->bind.render_pass_subpass = &rp->subpasses[sp];
    cmd->bind.fb = fb;
    cmd->bind.render_pass_contents = contents;
}

static inline void cmd_end_render_pass(struct intel_cmd *cmd)
{
    //note what to do if rp != bound rp
    cmd->bind.render_pass = 0;
    cmd->bind.fb = 0;
}

void cmd_batch_flush(struct intel_cmd *cmd, uint32_t pipe_control_dw0);
void cmd_batch_flush_all(struct intel_cmd *cmd);

void cmd_batch_depth_count(struct intel_cmd *cmd,
                           struct intel_bo *bo,
                           VkDeviceSize offset);

void cmd_batch_timestamp(struct intel_cmd *cmd,
                         struct intel_bo *bo,
                         VkDeviceSize offset);

void cmd_batch_immediate(struct intel_cmd *cmd,
                         uint32_t pipe_control_flags,
                         struct intel_bo *bo,
                         VkDeviceSize offset,
                         uint64_t val);

void cmd_draw_meta(struct intel_cmd *cmd, const struct intel_cmd_meta *meta);

void cmd_meta_ds_op(struct intel_cmd *cmd,
                    enum intel_cmd_meta_ds_op op,
                    struct intel_img *img,
                    const VkImageSubresourceRange *range);

void cmd_meta_clear_color_image(
    VkCommandBuffer                         commandBuffer,
    struct intel_img                   *img,
    VkImageLayout                       imageLayout,
    const VkClearColorValue            *pClearColor,
    uint32_t                            rangeCount,
    const VkImageSubresourceRange      *pRanges);

void cmd_meta_clear_depth_stencil_image(
    VkCommandBuffer                              commandBuffer,
    struct intel_img*                        img,
    VkImageLayout                            imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*          pRanges);

#endif /* CMD_PRIV_H */
