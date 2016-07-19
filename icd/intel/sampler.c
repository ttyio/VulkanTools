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
 * Author: Tony Barbour <tony@LunarG.com>
 *
 */

#include "genhw/genhw.h"
#include "dev.h"
#include "sampler.h"

/**
 * Translate a pipe texture filter to the matching hardware mapfilter.
 */
static int translate_tex_filter(VkFilter filter)
{
   switch (filter) {
   case VK_FILTER_NEAREST: return GEN6_MAPFILTER_NEAREST;
   case VK_FILTER_LINEAR:  return GEN6_MAPFILTER_LINEAR;
   default:
      assert(!"unknown tex filter");
      return GEN6_MAPFILTER_NEAREST;
   }
}

static int translate_tex_mipmap_mode(VkSamplerMipmapMode mode)
{
   switch (mode) {
   case VK_SAMPLER_MIPMAP_MODE_NEAREST: return GEN6_MIPFILTER_NEAREST;
   case VK_SAMPLER_MIPMAP_MODE_LINEAR:  return GEN6_MIPFILTER_LINEAR;
   default:
      assert(!"unknown tex mipmap mode");
      return GEN6_MIPFILTER_NONE;
   }
}

static int translate_tex_addr(VkSamplerAddressMode addr)
{
   switch (addr) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT:                 return GEN6_TEXCOORDMODE_WRAP;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:        return GEN6_TEXCOORDMODE_MIRROR;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:          return GEN6_TEXCOORDMODE_CLAMP;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:        return GEN6_TEXCOORDMODE_CLAMP_BORDER;
   case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:   return GEN6_TEXCOORDMODE_MIRROR_ONCE;
   default:
      assert(!"unknown tex address");
      return GEN6_TEXCOORDMODE_WRAP;
   }
}

static int translate_compare_func(VkCompareOp func)
{
    switch (func) {
    case VK_COMPARE_OP_NEVER:         return GEN6_COMPAREFUNCTION_NEVER;
    case VK_COMPARE_OP_LESS:          return GEN6_COMPAREFUNCTION_LESS;
    case VK_COMPARE_OP_EQUAL:         return GEN6_COMPAREFUNCTION_EQUAL;
    case VK_COMPARE_OP_LESS_OR_EQUAL:    return GEN6_COMPAREFUNCTION_LEQUAL;
    case VK_COMPARE_OP_GREATER:       return GEN6_COMPAREFUNCTION_GREATER;
    case VK_COMPARE_OP_NOT_EQUAL:     return GEN6_COMPAREFUNCTION_NOTEQUAL;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return GEN6_COMPAREFUNCTION_GEQUAL;
    case VK_COMPARE_OP_ALWAYS:        return GEN6_COMPAREFUNCTION_ALWAYS;
    default:
      assert(!"unknown compare_func");
      return GEN6_COMPAREFUNCTION_NEVER;
    }
}

static void translate_border_color(VkBorderColor type, float rgba[4])
{
    switch (type) {
    case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
    case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
        rgba[0] = 1.0;
        rgba[1] = 1.0;
        rgba[2] = 1.0;
        rgba[3] = 1.0;
        break;
    case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
    case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
    default:
        rgba[0] = 0.0;
        rgba[1] = 0.0;
        rgba[2] = 0.0;
        rgba[3] = 0.0;
        break;
    case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
    case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
        rgba[0] = 0.0;
        rgba[1] = 0.0;
        rgba[2] = 0.0;
        rgba[3] = 1.0;
        break;
    }
}

static void
sampler_border_color_state_gen6(const struct intel_gpu *gpu,
                                const float color[4],
                                uint32_t dw[12])
{
   float rgba[4] = { color[0], color[1], color[2], color[3] };

   INTEL_GPU_ASSERT(gpu, 6, 6);

   /*
    * This state is not documented in the Sandy Bridge PRM, but in the
    * Ironlake PRM.  SNORM8 seems to be in DW11 instead of DW1.
    */

   /* IEEE_FP */
   dw[1] = u_fui(rgba[0]);
   dw[2] = u_fui(rgba[1]);
   dw[3] = u_fui(rgba[2]);
   dw[4] = u_fui(rgba[3]);

   /* FLOAT_16 */
   dw[5] = u_float_to_half(rgba[0]) |
           u_float_to_half(rgba[1]) << 16;
   dw[6] = u_float_to_half(rgba[2]) |
           u_float_to_half(rgba[3]) << 16;

   /* clamp to [-1.0f, 1.0f] */
   rgba[0] = U_CLAMP(rgba[0], -1.0f, 1.0f);
   rgba[1] = U_CLAMP(rgba[1], -1.0f, 1.0f);
   rgba[2] = U_CLAMP(rgba[2], -1.0f, 1.0f);
   rgba[3] = U_CLAMP(rgba[3], -1.0f, 1.0f);

   /* SNORM16 */
   dw[9] =  (int16_t) u_iround(rgba[0] * 32767.0f) |
            (int16_t) u_iround(rgba[1] * 32767.0f) << 16;
   dw[10] = (int16_t) u_iround(rgba[2] * 32767.0f) |
            (int16_t) u_iround(rgba[3] * 32767.0f) << 16;

   /* SNORM8 */
   dw[11] = (int8_t) u_iround(rgba[0] * 127.0f) |
            (int8_t) u_iround(rgba[1] * 127.0f) << 8 |
            (int8_t) u_iround(rgba[2] * 127.0f) << 16 |
            (int8_t) u_iround(rgba[3] * 127.0f) << 24;

   /* clamp to [0.0f, 1.0f] */
   rgba[0] = U_CLAMP(rgba[0], 0.0f, 1.0f);
   rgba[1] = U_CLAMP(rgba[1], 0.0f, 1.0f);
   rgba[2] = U_CLAMP(rgba[2], 0.0f, 1.0f);
   rgba[3] = U_CLAMP(rgba[3], 0.0f, 1.0f);

   /* UNORM8 */
   dw[0] = (uint8_t) u_iround(rgba[0] * 255.0f) |
           (uint8_t) u_iround(rgba[1] * 255.0f) << 8 |
           (uint8_t) u_iround(rgba[2] * 255.0f) << 16 |
           (uint8_t) u_iround(rgba[3] * 255.0f) << 24;

   /* UNORM16 */
   dw[7] = (uint16_t) u_iround(rgba[0] * 65535.0f) |
           (uint16_t) u_iround(rgba[1] * 65535.0f) << 16;
   dw[8] = (uint16_t) u_iround(rgba[2] * 65535.0f) |
           (uint16_t) u_iround(rgba[3] * 65535.0f) << 16;
}

static void
sampler_init(struct intel_sampler *sampler,
             const struct intel_gpu *gpu,
             const VkSamplerCreateInfo *info)
{
   int mip_filter, min_filter, mag_filter, max_aniso;
   int lod_bias, max_lod, min_lod;
   int wrap_s, wrap_t, wrap_r;
   uint32_t dw0, dw1, dw3;
   float border_color[4];

   INTEL_GPU_ASSERT(gpu, 6, 7.5);
   STATIC_ASSERT(ARRAY_SIZE(sampler->cmd) >= 15);

   mip_filter = translate_tex_mipmap_mode(info->mipmapMode);
   min_filter = translate_tex_filter(info->minFilter);
   mag_filter = translate_tex_filter(info->magFilter);

   if (info->anisotropyEnable == VK_FALSE) {
        max_aniso = 1;
   } else {
        if (info->maxAnisotropy >= 2 && info->maxAnisotropy <= 16)
           max_aniso = info->maxAnisotropy / 2 - 1;
        else if (info->maxAnisotropy > 16)
           max_aniso = GEN6_ANISORATIO_16;
        else
           max_aniso = GEN6_ANISORATIO_2;
   }

   /*
    * Here is how the hardware calculate per-pixel LOD, from my reading of the
    * PRMs:
    *
    *  1) LOD is set to log2(ratio of texels to pixels) if not specified in
    *     other ways.  The number of texels is measured using level
    *     SurfMinLod.
    *  2) Bias is added to LOD.
    *  3) LOD is clamped to [MinLod, MaxLod], and the clamped value is
    *     compared with Base to determine whether magnification or
    *     minification is needed.  (if preclamp is disabled, LOD is compared
    *     with Base before clamping)
    *  4) If magnification is needed, or no mipmapping is requested, LOD is
    *     set to floor(MinLod).
    *  5) LOD is clamped to [0, MIPCnt], and SurfMinLod is added to LOD.
    *
    * With Gallium interface, Base is always zero and
    * pipe_sampler_view::u.tex.first_level specifies SurfMinLod.
    */
   if (intel_gpu_gen(gpu) >= INTEL_GEN(7)) {
      const float scale = 256.0f;

      /* [-16.0, 16.0) in S4.8 */
      lod_bias = (int)
         (U_CLAMP(info->mipLodBias, -16.0f, 15.9f) * scale);
      lod_bias &= 0x1fff;

      /* [0.0, 14.0] in U4.8 */
      max_lod = (int) (U_CLAMP(info->maxLod, 0.0f, 14.0f) * scale);
      min_lod = (int) (U_CLAMP(info->minLod, 0.0f, 14.0f) * scale);
   }
   else {
      const float scale = 64.0f;

      /* [-16.0, 16.0) in S4.6 */
      lod_bias = (int)
         (U_CLAMP(info->mipLodBias, -16.0f, 15.9f) * scale);
      lod_bias &= 0x7ff;

      /* [0.0, 13.0] in U4.6 */
      max_lod = (int) (U_CLAMP(info->maxLod, 0.0f, 13.0f) * scale);
      min_lod = (int) (U_CLAMP(info->minLod, 0.0f, 13.0f) * scale);
   }

   /*
    * We want LOD to be clamped to determine magnification/minification, and
    * get set to zero when it is magnification or when mipmapping is disabled.
    * The hardware would set LOD to floor(MinLod) and that is a problem when
    * MinLod is greater than or equal to 1.0f.
    *
    * With Base being zero, it is always minification when MinLod is non-zero.
    * To achieve our goal, we just need to set MinLod to zero and set
    * MagFilter to MinFilter when mipmapping is disabled.
    */

   /* determine wrap s/t/r */
   wrap_s = translate_tex_addr(info->addressModeU);
   wrap_t = translate_tex_addr(info->addressModeV);
   wrap_r = translate_tex_addr(info->addressModeW);

   translate_border_color(info->borderColor, border_color);

   if (intel_gpu_gen(gpu) >= INTEL_GEN(7)) {
      dw0 = 1 << 28 |
            mip_filter << 20 |
            lod_bias << 1;

      if (info->maxAnisotropy > 1 && info->anisotropyEnable == VK_TRUE) {
         dw0 |= GEN6_MAPFILTER_ANISOTROPIC << 17 |
                GEN6_MAPFILTER_ANISOTROPIC << 14 |
                1;
      } else {
         dw0 |= mag_filter << 17 |
                min_filter << 14;
      }

      dw1 = min_lod << 20 |
            max_lod << 8;

      dw1 |= translate_compare_func(info->compareOp) << 1;

      dw3 = max_aniso << 19;

      /* round the coordinates for linear filtering */
      if (min_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MIN_ROUND |
                 GEN6_SAMPLER_DW3_V_MIN_ROUND |
                 GEN6_SAMPLER_DW3_R_MIN_ROUND);
      }
      if (mag_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MAG_ROUND |
                 GEN6_SAMPLER_DW3_V_MAG_ROUND |
                 GEN6_SAMPLER_DW3_R_MAG_ROUND);
      }

      dw3 |= wrap_s << 6 |
             wrap_t << 3 |
             wrap_r;

      if (info->unnormalizedCoordinates)
          dw3 |= GEN7_SAMPLER_DW3_NON_NORMALIZED_COORD;

      sampler->cmd[0] = dw0;
      sampler->cmd[1] = dw1;
      sampler->cmd[2] = dw3;

      memcpy(&sampler->cmd[3], &border_color, sizeof(border_color));
   }
   else {
      dw0 = 1 << 28 |
            mip_filter << 20 |
            lod_bias << 3;

      dw0 |= translate_compare_func(info->compareOp);

      if (info->maxAnisotropy > 1 && info->anisotropyEnable == VK_TRUE) {
         dw0 |= GEN6_MAPFILTER_ANISOTROPIC << 17 |
                GEN6_MAPFILTER_ANISOTROPIC << 14;
      }
      else {
         dw0 |= (min_filter != mag_filter) << 27 |
                mag_filter << 17 |
                min_filter << 14;
      }

      dw1 = min_lod << 22 |
            max_lod << 12;

      dw1 |= wrap_s << 6 |
             wrap_t << 3 |
             wrap_r;

      dw3 = max_aniso << 19;

      /* round the coordinates for linear filtering */
      if (min_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MIN_ROUND |
                 GEN6_SAMPLER_DW3_V_MIN_ROUND |
                 GEN6_SAMPLER_DW3_R_MIN_ROUND);
      }
      if (mag_filter != GEN6_MAPFILTER_NEAREST) {
         dw3 |= (GEN6_SAMPLER_DW3_U_MAG_ROUND |
                 GEN6_SAMPLER_DW3_V_MAG_ROUND |
                 GEN6_SAMPLER_DW3_R_MAG_ROUND);
      }

      if (info->unnormalizedCoordinates)
          dw3 |= GEN6_SAMPLER_DW3_NON_NORMALIZED_COORD;

      sampler->cmd[0] = dw0;
      sampler->cmd[1] = dw1;
      sampler->cmd[2] = dw3;

      sampler_border_color_state_gen6(gpu, border_color, &sampler->cmd[3]);
   }
}

static void sampler_destroy(struct intel_obj *obj)
{
    struct intel_sampler *sampler = intel_sampler_from_obj(obj);

    intel_sampler_destroy(sampler);
}

VkResult intel_sampler_create(struct intel_dev *dev,
                                const VkSamplerCreateInfo *info,
                                struct intel_sampler **sampler_ret)
{
    struct intel_sampler *sampler;

    sampler = (struct intel_sampler *) intel_base_create(&dev->base.handle,
            sizeof(*sampler), dev->base.dbg, VK_DEBUG_REPORT_OBJECT_TYPE_SAMPLER_EXT, info, 0);
    if (!sampler)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    sampler->obj.destroy = sampler_destroy;

    sampler_init(sampler, dev->gpu, info);

    *sampler_ret = sampler;

    return VK_SUCCESS;
}

void intel_sampler_destroy(struct intel_sampler *sampler)
{
    intel_base_destroy(&sampler->obj.base);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(
    VkDevice                                  device,
    const VkSamplerCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                     pAllocator,
    VkSampler*                                pSampler)
{
    struct intel_dev *dev = intel_dev(device);

    return intel_sampler_create(dev, pCreateInfo,
            (struct intel_sampler **) pSampler);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(
    VkDevice                                device,
    VkSampler                                 sampler,
    const VkAllocationCallbacks*                     pAllocator)

 {
    struct intel_obj *obj = intel_obj(sampler);

    obj->destroy(obj);
 }
