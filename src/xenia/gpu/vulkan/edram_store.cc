/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/edram_store.h"
#include "xenia/base/math.h"

namespace xe {
namespace gpu {
namespace vulkan {

using xe::ui::vulkan::CheckResult;

// Generated with `xb genspirv`.
#include "xenia/gpu/vulkan/shaders/bin/edram_store_32bpp1x_comp.h"

const ModeInfo EDRAMStore::mode_info_[Mode::k_ModeCount] = {
    {edram_store_32bpp1x_comp, sizeof(edram_store_32bpp1x_comp),
     "S(c): EDRAM Store 32bpp 1x"}
};

EDRAMStore::EDRAMStore(ui::vulkan::VulkanDevice* device) : device_(device) {}

EDRAMStore::~EDRAMStore() { Shutdown(); }

VkResult EDRAMStore::Initialize() {
  VkResult status = VK_SUCCESS;

  // Create the 1280x2048 image to store raw EDRAM tile data.
  VkImageCreateInfo image_info;
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.pNext = nullptr;
  image_info.flags = 0;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent.width = 1280;
  image_info.extent.height = 2048;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.queueFamilyIndexCount = 0;
  image_info.pQueueFamilyIndices = nullptr;
  image_info.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
  status = vkCreateImage(*device_, &image_info, nullptr, &edram_image_);
  CheckResult(status, "vkCreateImage");
  if (status != VK_SUCCESS) {
    return status;
  }
  device_->DbgSetObjectName(uint64_t(edram_image_),
                            VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT, "EDRAM");
  VkMemoryRequirements image_requirements;
  vkGetImageMemoryRequirements(*device_, edram_image_, &image_requirements);
  edram_memory_ = device_->AllocateMemory(image_requirements, 0);
  assert_not_null(edram_memory_);
  if (!edram_memory_) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  status = vkBindImageMemory(*device_, edram_image_, edram_memory_, 0);
  CheckResult(status, "vkBindImageMemory");
  if (status != VK_SUCCESS) {
    return status;
  }
  edram_image_status_ = EDRAMImageStatus::kUntransitioned;

  // Create the view of the EDRAM image.
  VkImageViewCreateInfo image_view_info;
  image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_info.pNext = nullptr;
  image_view_info.flags = 0;
  image_view_info.image = edram_image_;
  image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_view_info.components.r = VK_COMPONENT_SWIZZLE_R;
  image_view_info.components.g = VK_COMPONENT_SWIZZLE_G;
  image_view_info.components.b = VK_COMPONENT_SWIZZLE_B;
  image_view_info.components.a = VK_COMPONENT_SWIZZLE_A;
  image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  image_view_info.subresourceRange.baseMipLevel = 0;
  image_view_info.subresourceRange.levelCount = 1;
  image_view_info.subresourceRange.baseArrayLayer = 0;
  image_view_info.subresourceRange.layerCount = 1;
  status = vkCreateImageView(*device_, &image_view_info, nullptr,
                             &edram_image_view_);
  CheckResult(status, "vkCreateImageView");
  if (status != VK_SUCCESS) {
    return status;
  }

  // Create the descriptor set layout for the load and store pipelines.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info;
  descriptor_set_layout_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_info.pNext = nullptr;
  descriptor_set_layout_info.flags = 0;
  descriptor_set_layout_info.bindingCount = 2;
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[2];
  // EDRAM.
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  // Render target.
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  descriptor_set_layout_info.pBindings = descriptor_set_layout_bindings;
  status = vkCreateDescriptorSetLayout(*device_, &descriptor_set_layout_info,
                                       nullptr, &descriptor_set_layout_);
  CheckResult(status, "vkCreateDescriptorSetLayout");
  if (status != VK_SUCCESS) {
    return status;
  }

  // Create the layout for the load and store pipelines.
  VkPipelineLayoutCreateInfo pipeline_layout_info;
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.pNext = nullptr;
  pipeline_layout_info.flags = 0;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &descriptor_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  VkPushConstantRange push_constant_range;
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(PushConstants);
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  status = vkCreatePipelineLayout(*device_, &pipeline_layout_info, nullptr,
                                  &pipeline_layout_);
  CheckResult(status, "vkCreatePipelineLayout");
  if (status != VK_SUCCESS) {
    return status;
  }

  // Create the pool for storage images used during loading and storing.
  VkDescriptorPoolSize pool_sizes[1];
  pool_sizes[0].descriptorCount = 4096;
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptor_pool_ = std::make_unique<DescriptorPool>(
      *device_, 4096,
      std::vector<VkDescriptorPoolSize>(pool_sizes, std::end(pool_sizes)));

  // Initialize all modes.
  VkShaderModuleCreateInfo shader_module_info;
  shader_module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_info.pNext = nullptr;
  shader_module_info.flags = 0;
  VkComputePipelineCreateInfo pipeline_info;
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = nullptr;
  pipeline_info.flags = 0;
  pipeline_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.pNext = nullptr;
  pipeline_info.stage.flags = 0;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.pName = "main";
  pipeline_info.stage.pSpecializationInfo = nullptr;
  pipeline_info.layout = pipeline_layout_;
  pipeline_info.basePipelineHandle = nullptr;
  pipeline_info.basePipelineIndex = -1;
  for (int mode_index = 0; mode_index < int(Mode::k_ModeCount); ++mode_index) {
    const ModeInfo& mode_info = mode_info_[mode_index];
    ModeData& mode_data = mode_data_[mode_index];
    shader_module_info.codeSize = mode_info.store_shader_code_size;
    shader_module_info.pCode =
        reinterpret_cast<const uint32_t*>(mode_info.store_shader_code);
    status = vkCreateShaderModule(*device_, &shader_module_info, nullptr,
                                  &mode_data.store_shader_module);
    CheckResult(status, "vkCreateShaderModule");
    if (status != VK_SUCCESS) {
      return status;
    }
    device_->DbgSetObjectName(uint64_t(mode_data.store_shader_module),
                              VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT,
                              mode_info.store_shader_debug_name);
    pipeline_info.stage.module = mode_data.store_shader_module;
    status = vkCreateComputePipelines(*device_, nullptr, 1, &pipeline_info,
                                      nullptr, &mode_data.store_pipeline);
    CheckResult(status, "vkCreateComputePipelines");
    if (status != VK_SUCCESS) {
      return status;
    }
  }

  return VK_SUCCESS;
}

void EDRAMStore::Shutdown() {
  // TODO(Triang3l): Wait for idle.
  for (int mode_index = 0; mode_index < int(Mode::k_ModeCount); ++mode_index) {
    ModeData& mode_data = mode_data_[mode_index];
    if (mode_data.store_pipeline) {
      vkDestroyPipeline(*device_, mode_data.store_pipeline, nullptr);
      mode_data.store_pipeline = nullptr;
    }
    if (mode_data.store_shader_module) {
      vkDestroyShaderModule(*device_, mode_data.store_shader_module, nullptr);
      mode_data.store_shader_module = nullptr;
    }
  }
  if (pipeline_layout_) {
    vkDestroyPipelineLayout(*device_, pipeline_layout_, nullptr);
    pipeline_layout_ = nullptr;
  }
  if (descriptor_set_layout_) {
    vkDestroyDescriptorSetLayout(*device_, descriptor_set_layout_, nullptr);
    descriptor_set_layout_ = nullptr;
  }
  if (edram_image_) {
    vkDestroyImage(*device_, edram_image_, nullptr);
    edram_image_ = nullptr;
  }
  if (edram_memory_) {
    vkFreeMemory(*device_, edram_memory_, nullptr);
    edram_memory_ = nullptr;
  }
}

void EDRAMStore::TransitionEDRAMImage(VkCommandBuffer command_buffer,
                                      bool load) {
  EDRAMImageStatus new_status =
      load ? EDRAMImageStatus::kLoad : EDRAMImageStatus::kStore;
  if (edram_image_status_ == new_status) {
    return;
  }
  VkImageMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  if (edram_image_status_ == EDRAMImageStatus::kUntransitioned) {
    barrier.srcAccessMask = 0;
  } else {
    barrier.srcAccessMask =
        load ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
  }
  barrier.dstAccessMask =
      load ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = edram_image_;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  VkPipelineStageFlags src_stage_mask;
  if (edram_image_status_ == EDRAMImageStatus::kUntransitioned) {
    src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  } else {
    src_stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }
  vkCmdPipelineBarrier(command_buffer, src_stage_mask,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
  edram_image_status_ = new_status;
}

Mode GetModeForRT(ColorRenderTargetFormat format, MsaaSamples samples) {
  if (rt_samples != MsaaSamples::k1X) {
    // MSAA storing not supported yet.
    return k_ModeUnsupported;
  }
  switch (rt_format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case ColorRenderTargetFormat::k_2_10_10_10:
    case ColorRenderTargetFormat::k_16_16:
    case ColorRenderTargetFormat::k_16_16_FLOAT:
    case ColorRenderTargetFormat::k_2_10_10_10_AS_16_16_16_16:
    case ColorRenderTargetFormat::k_32_FLOAT:
      return Mode::k_32bpp_1X;
    default:
      // 64-bit not supported yet.
      break;
  }
  return k_ModeUnsupported;
}

bool EDRAMStore::GetDimensions(Mode mode, VkExtent2D rt_extent,
                               uint32_t edram_offset_tiles,
                               uint32_t edram_pitch_px,
                               uint32_t& rt_pitch_tiles,
                               uint32_t& edram_pitch_tiles,
                               uint32_t& edram_tile_rows) {
  if (rt_extent.width == 0 || rt_extent.height == 0 || edram_pitch_px == 0) {
    return false;
  }

  // All this is for 80x16 - other modes are not supported yet!
  if (edram_offset_tiles >= 2048) {
    return false;
  }
  uint32_t edram_pitch = xe::round_up(edram_pitch_px, 80) / 80;
  uint32_t rt_pitch = std::min(xe::round_up(rt_extent.width, 80) / 80,
                               edram_pitch);
  uint32_t edram_rows = xe::round_up(rt_extent.height, 16) / 16;
  uint32_t edram_size = edram_pitch * edram_rows;
  if (edram_offset_tiles + edram_size > 2048) {
    // Ignore excess rows.
    edram_rows = (2048 - edram_size) / edram_pitch;
    if (edram_rows == 0) {
      return false;
    }
  }

  rt_pitch_tiles = rt_pitch;
  edram_pitch_tiles = edram_pitch;
  edram_pitch_rows = edram_rows;
  return true;
}

void EDRAMStore::StoreColor(VkCommandBuffer command_buffer, VkFence fence,
                            VkImageView rt_image_view,
                            ColorRenderTargetFormat rt_format,
                            MsaaSamples rt_samples, VkRect2D rt_rect,
                            uint32_t edram_offset_tiles,
                            uint32_t edram_pitch_px) {
  if (edram_pitch_px == 0 || rt_rect.extent.width == 0 ||
      rt_rect.extent.height == 0) {
    return;
  }

  Mode mode = GetModeForRT(rt_format, rt_samples);
  if (mode == k_ModeUnsupported) {
    return;
  }
  uint32_t rt_pitch_tiles, edram_pitch_tiles, edram_tile_rows;
  if (!GetDimensions(mode, rt_rect.extent, edram_offset_tiles, edram_pitch_px,
                     rt_pitch_tiles, edram_pitch_tiles, edram_tile_rows)) {
    return;
  }
  ModeData& mode_data = mode_data_[mode];

  // Allocate space for the descriptors.
  if (!descriptor_pool_->has_open_batch()) {
    descriptor_pool_->BeginBatch(fence);
  }
  auto set = descriptor_pool_->AcquireEntry(descriptor_set_layout_);
  if (!set) {
    assert_always();
    descriptor_pool_->CancelBatch();
    return;
  }

  // Switch the EDRAM image to storing.
  TransitionEDRAMImage(command_buffer, false);

  // Write the descriptors.
  VkWriteDescriptorSet descriptors[2];
  descriptors[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[0].pNext = nullptr;
  descriptors[0].dstSet = set;
  descriptors[0].dstBinding = 0;
  descriptors[0].dstArrayElement = 0;
  descriptors[0].descriptorCount = 0;
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorImageInfo image_info_edram;
  image_info_edram.sampler = nullptr;
  image_info_edram.imageView = edram_image_view_;
  image_info_edram.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  descriptors[0].pImageInfo = &image_info_edram;
  descriptors[0].pBufferInfo = nullptr;
  descriptors[0].pTexelBufferView = nullptr;
  descriptors[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[1].pNext = nullptr;
  descriptors[1].dstSet = set;
  descriptors[1].dstBinding = 1;
  descriptors[1].dstArrayElement = 0;
  descriptors[1].descriptorCount = 0;
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorImageInfo image_info_rt;
  image_info_rt.sampler = nullptr;
  image_info_rt.imageView = rt_image_view;
  image_info_rt.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  descriptors[1].pImageInfo = &image_info_rt;
  descriptors[1].pBufferInfo = nullptr;
  descriptors[1].pTexelBufferView = nullptr;
  vkUpdateDescriptorSets(*device_, 2, descriptors, 0, nullptr);

  // Dispatch the computation descriptors.
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    mode_data.store_pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_layout_, 0, 1, &set, 0, nullptr);
  StorePushContants push_constants;
  push_constants.edram_offset = edram_offset_tiles;
  push_constants.edram_pitch = edram_pitch_tiles;
  push_constants.rt_offset[0] = uint32_t(rt_rect.offset.x);
  push_constants.rt_offset[1] = uint32_t(rt_rect.offset.y);
  vkCmdPushConstants(command_buffer, pipeline_layout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                     &push_constants);
  uint32_t group_count_x = rt_pitch_tiles;
  if (mode == Mode::k_32bpp_1X) {
    // For the 80x16 mode, tiles are split into 2 groups because 1280 threads
    // may be over the limit.
    group_count_x *= 2;
  }
  vkCmdDispatch(command_buffer, group_count_x, edram_tile_rows, 1);

  // Commit the write so loads or overlapping writes won't conflict.
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 0, nullptr);
}

void EDRAMStore::Scavenge() {
  if (descriptor_pool_->has_open_batch()) {
    descriptor_pool_->EndBatch();
  }
  descriptor_pool_->Scavenge();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe