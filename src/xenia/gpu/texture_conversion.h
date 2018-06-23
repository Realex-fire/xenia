/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_TEXTURE_CONVERSION_H_
#define XENIA_GPU_TEXTURE_CONVERSION_H_

#include <cstring>
#include <functional>
#include <memory>

#include "xenia/base/assert.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
namespace texture_conversion {

typedef std::function<void(Endian, void*, const void*, size_t)>
    CopyBlockCallback;

void CopySwapBlock(Endian endian, void* output, const void* input,
                   size_t length);
void ConvertTexelCTX1ToR8G8(Endian endian, void* output, const void* input,
                            size_t length);
void ConvertTexelDXT3AToDXT3(Endian endian, void* output, const void* input,
                             size_t length);

typedef std::function<void(void*, const void*, size_t)> UntileCopyBlockCallback;

typedef struct UntileInfo {
  bool is_3d;
  // offset_z, depth, input_pitch_v, output_pitch_v only used for 3D textures.
  uint32_t offset_x;
  uint32_t offset_y;
  uint32_t offset_z;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t input_pitch_h;
  uint32_t input_pitch_v;
  uint32_t output_pitch_h;
  uint32_t output_pitch_v;
  const FormatInfo* input_format_info;
  const FormatInfo* output_format_info;
  UntileCopyBlockCallback copy_callback;
} UntileInfo;

void Untile(uint8_t* output_buffer, const uint8_t* input_buffer,
            const UntileInfo* untile_info);

}  // namespace texture_conversion
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_TEXTURE_CONVERSION_H_
