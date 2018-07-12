/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/edram_store.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"

namespace xe {
namespace gpu {
namespace vulkan {

using xe::ui::vulkan::CheckResult;

// Generated with `xb genspirv`.
#include "xenia/gpu/vulkan/shaders/bin/edram_store_32bpp_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_load_32bpp_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_store_64bpp_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_load_64bpp_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_store_7e3_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_load_7e3_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_store_d24_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_load_d24_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_store_d24f_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_load_d24f_comp.h"
#include "xenia/gpu/vulkan/shaders/bin/edram_clear_color_comp.h"

const EDRAMStore::ModeInfo EDRAMStore::mode_info_[] = {
    {false, false, edram_store_32bpp_comp, sizeof(edram_store_32bpp_comp),
     "S(c): EDRAM Store 32bpp", edram_load_32bpp_comp,
     sizeof(edram_load_32bpp_comp), "S(c): EDRAM Load 32bpp"},

    {false, true, edram_store_64bpp_comp, sizeof(edram_store_64bpp_comp),
     "S(c): EDRAM Store 64bpp", edram_load_64bpp_comp,
     sizeof(edram_load_64bpp_comp), "S(c): EDRAM Load 64bpp"},

    {false, false, edram_store_7e3_comp, sizeof(edram_store_7e3_comp),
     "S(c): EDRAM Store 7e3", edram_load_7e3_comp, sizeof(edram_load_7e3_comp),
     "S(c): EDRAM Load 7e3"},

    {true, false, edram_store_d24_comp, sizeof(edram_store_d24_comp),
     "S(c): EDRAM Store D24", edram_load_d24_comp, sizeof(edram_load_d24_comp),
     "S(c): EDRAM Load D24"},

    {true, false, edram_store_d24f_comp, sizeof(edram_store_d24f_comp),
     "S(c): EDRAM Store D24F", edram_load_d24f_comp, sizeof(edram_load_d24f_comp),
     "S(c): EDRAM Load D24F"}
};

EDRAMStore::EDRAMStore(ui::vulkan::VulkanDevice* device) : device_(device) {}

EDRAMStore::~EDRAMStore() { Shutdown(); }

VkResult EDRAMStore::Initialize() {
  VkResult status = VK_SUCCESS;
  VkMemoryRequirements memory_requirements;

  // Create the 1280x2048 image to store raw EDRAM tile data.
  VkImageCreateInfo image_info;
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.pNext = nullptr;
  // Not really needed if the format is R32_UINT, and may hurt performance, but
  // being able to debug the EDRAM contents somehow is nice.
  image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  // For easier debugging in RenderDoc.
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
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  status = vkCreateImage(*device_, &image_info, nullptr, &edram_image_);
  CheckResult(status, "vkCreateImage");
  if (status != VK_SUCCESS) {
    return status;
  }
  device_->DbgSetObjectName(uint64_t(edram_image_),
                            VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT, "EDRAM");
  vkGetImageMemoryRequirements(*device_, edram_image_, &memory_requirements);
  edram_memory_ = device_->AllocateMemory(memory_requirements, 0);
  if (edram_memory_ == nullptr) {
    assert_always();
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  status = vkBindImageMemory(*device_, edram_image_, edram_memory_, 0);
  CheckResult(status, "vkBindImageMemory");
  if (status != VK_SUCCESS) {
    return status;
  }
  edram_image_state_ = EDRAMImageState::kUntransitioned;

  // Create the view of the EDRAM image.
  VkImageViewCreateInfo image_view_info;
  image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_info.pNext = nullptr;
  image_view_info.flags = 0;
  image_view_info.image = edram_image_;
  image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_info.format = VK_FORMAT_R32_UINT;
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

  // Create the buffer for host depth buffer value copying.
  VkBufferCreateInfo buffer_info;
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.pNext = nullptr;
  buffer_info.flags = 0;
  buffer_info.size = (sizeof(float) + sizeof(uint8_t)) * kTotalTexelCount;
  buffer_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_info.queueFamilyIndexCount = 0;
  buffer_info.pQueueFamilyIndices = nullptr;
  status = vkCreateBuffer(*device_, &buffer_info, nullptr, &depth_copy_buffer_);
  CheckResult(status, "vkCreateBuffer");
  if (status != VK_SUCCESS) {
    return status;
  }
  device_->DbgSetObjectName(uint64_t(depth_copy_buffer_),
                            VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
                            "EDRAM Depth Copy Buffer");
  vkGetBufferMemoryRequirements(*device_, depth_copy_buffer_,
                                &memory_requirements);
  depth_copy_memory_ = device_->AllocateMemory(memory_requirements, 0);
  if (depth_copy_memory_ == nullptr) {
    assert_always();
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  status =
      vkBindBufferMemory(*device_, depth_copy_buffer_, depth_copy_memory_, 0);
  CheckResult(status, "vkBindBufferMemory");
  if (status != VK_SUCCESS) {
    return status;
  }
  VkBufferViewCreateInfo buffer_view_info;
  buffer_view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  buffer_view_info.pNext = nullptr;
  buffer_view_info.flags = 0;
  buffer_view_info.buffer = depth_copy_buffer_;
  buffer_view_info.format = VK_FORMAT_R32_UINT;
  buffer_view_info.offset = 0;
  buffer_view_info.range = kTotalTexelCount * sizeof(float);
  status = vkCreateBufferView(*device_, &buffer_view_info, nullptr,
                              &depth_copy_buffer_view_depth_);
  CheckResult(status, "vkCreateBufferView");
  if (status != VK_SUCCESS) {
    return status;
  }
  buffer_view_info.format = VK_FORMAT_R8_UINT;
  buffer_view_info.offset = kTotalTexelCount * sizeof(float);
  buffer_view_info.range = kTotalTexelCount * sizeof(uint8_t);
  status = vkCreateBufferView(*device_, &buffer_view_info, nullptr,
                              &depth_copy_buffer_view_stencil_);
  CheckResult(status, "vkCreateBufferView");
  if (status != VK_SUCCESS) {
    return status;
  }
  depth_copy_buffer_state_ = DepthCopyBufferState::kUntransitioned;

  // Create the descriptor set layouts for the store and load pipelines.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info;
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[3];
  descriptor_set_layout_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_info.pNext = nullptr;
  descriptor_set_layout_info.flags = 0;
  descriptor_set_layout_info.bindingCount = 2;
  descriptor_set_layout_info.pBindings = descriptor_set_layout_bindings;
  // Binding 0 is the EDRAM.
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  // For color store/load, binding 1 is the render target.
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  status = vkCreateDescriptorSetLayout(*device_, &descriptor_set_layout_info,
                                       nullptr, &descriptor_set_layout_color_);
  CheckResult(status, "vkCreateDescriptorSetLayout");
  if (status != VK_SUCCESS) {
    return status;
  }
  // For depth store/load, bindings 1 and 2 are linear depth and stencil.
  descriptor_set_layout_info.bindingCount = 3;
  descriptor_set_layout_bindings[1].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
  descriptor_set_layout_bindings[2].binding = 2;
  descriptor_set_layout_bindings[2].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
  descriptor_set_layout_bindings[2].descriptorCount = 1;
  descriptor_set_layout_bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[2].pImmutableSamplers = nullptr;
  status = vkCreateDescriptorSetLayout(*device_, &descriptor_set_layout_info,
                                       nullptr, &descriptor_set_layout_depth_);
  CheckResult(status, "vkCreateDescriptorSetLayout");
  if (status != VK_SUCCESS) {
    return status;
  }
  // Clear only needs the EDRAM.
  descriptor_set_layout_info.bindingCount = 1;
  status = vkCreateDescriptorSetLayout(*device_, &descriptor_set_layout_info,
                                       nullptr,
                                       &descriptor_set_layout_clear_color_);
  CheckResult(status, "vkCreateDescriptorSetLayout");
  if (status != VK_SUCCESS) {
    return status;
  }

  // Create the layouts for the pipelines.
  VkPipelineLayoutCreateInfo pipeline_layout_info;
  VkDescriptorSetLayout descriptor_set_layouts[1];
  VkPushConstantRange push_constant_range;
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.pNext = nullptr;
  pipeline_layout_info.flags = 0;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  // Color store/load layout.
  descriptor_set_layouts[0] = descriptor_set_layout_color_;
  push_constant_range.size = sizeof(PushConstantsColor);
  status = vkCreatePipelineLayout(*device_, &pipeline_layout_info, nullptr,
                                  &pipeline_layout_color_);
  CheckResult(status, "vkCreatePipelineLayout");
  if (status != VK_SUCCESS) {
    return status;
  }
  // Depth store/load layout.
  descriptor_set_layouts[0] = descriptor_set_layout_depth_;
  push_constant_range.size = sizeof(PushConstantsDepth);
  status = vkCreatePipelineLayout(*device_, &pipeline_layout_info, nullptr,
                                  &pipeline_layout_depth_);
  CheckResult(status, "vkCreatePipelineLayout");
  if (status != VK_SUCCESS) {
    return status;
  }
  // Color clear layout.
  descriptor_set_layouts[0] = descriptor_set_layout_clear_color_;
  push_constant_range.size = sizeof(PushConstantsClearColor);
  status = vkCreatePipelineLayout(*device_, &pipeline_layout_info, nullptr,
                                  &pipeline_layout_clear_color_);
  CheckResult(status, "vkCreatePipelineLayout");
  if (status != VK_SUCCESS) {
    return status;
  }

  // Create the pool for storage images used during loading and storing.
  VkDescriptorPoolSize pool_sizes[2];
  pool_sizes[0].descriptorCount = 2048;
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  pool_sizes[1].descriptorCount = 2048;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
  descriptor_pool_ = std::make_unique<ui::vulkan::DescriptorPool>(
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
  pipeline_info.basePipelineHandle = nullptr;
  pipeline_info.basePipelineIndex = -1;
  for (int mode_index = 0; mode_index < int(Mode::k_ModeCount); ++mode_index) {
    const ModeInfo& mode_info = mode_info_[mode_index];
    pipeline_info.layout =
        mode_info.is_depth ? pipeline_layout_depth_ : pipeline_layout_color_;

    // Store pipeline.
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

    // Load pipeline.
    shader_module_info.codeSize = mode_info.load_shader_code_size;
    shader_module_info.pCode =
        reinterpret_cast<const uint32_t*>(mode_info.load_shader_code);
    status = vkCreateShaderModule(*device_, &shader_module_info, nullptr,
                                  &mode_data.load_shader_module);
    CheckResult(status, "vkCreateShaderModule");
    if (status != VK_SUCCESS) {
      return status;
    }
    device_->DbgSetObjectName(uint64_t(mode_data.load_shader_module),
                              VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT,
                              mode_info.load_shader_debug_name);
    pipeline_info.stage.module = mode_data.load_shader_module;
    status = vkCreateComputePipelines(*device_, nullptr, 1, &pipeline_info,
                                      nullptr, &mode_data.load_pipeline);
    CheckResult(status, "vkCreateComputePipelines");
    if (status != VK_SUCCESS) {
      return status;
    }
  }

  // Color clear pipeline.
  shader_module_info.codeSize = sizeof(edram_clear_color_comp);
  shader_module_info.pCode =
      reinterpret_cast<const uint32_t*>(edram_clear_color_comp);
  status = vkCreateShaderModule(*device_, &shader_module_info, nullptr,
                                &clear_color_shader_module_);
  CheckResult(status, "vkCreateShaderModule");
  if (status != VK_SUCCESS) {
    return status;
  }
  device_->DbgSetObjectName(uint64_t(clear_color_shader_module_),
                            VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT,
                            "S(c): EDRAM Clear Color");
  pipeline_info.stage.module = clear_color_shader_module_;
  pipeline_info.layout = pipeline_layout_clear_color_;
  status = vkCreateComputePipelines(*device_, nullptr, 1, &pipeline_info,
                                    nullptr, &clear_color_pipeline_);
  CheckResult(status, "vkCreateComputePipelines");
  if (status != VK_SUCCESS) {
    return status;
  }

  return VK_SUCCESS;
}

void EDRAMStore::Shutdown() {
  // TODO(Triang3l): Wait for idle.

  VK_SAFE_DESTROY(vkDestroyPipeline, *device_, clear_color_pipeline_, nullptr);
  VK_SAFE_DESTROY(vkDestroyShaderModule, *device_, clear_color_shader_module_,
                  nullptr);

  for (int mode_index = 0; mode_index < int(Mode::k_ModeCount); ++mode_index) {
    ModeData& mode_data = mode_data_[mode_index];
    VK_SAFE_DESTROY(vkDestroyPipeline, *device_, mode_data.load_pipeline,
                    nullptr);
    VK_SAFE_DESTROY(vkDestroyShaderModule, *device_,
                    mode_data.load_shader_module, nullptr);
    VK_SAFE_DESTROY(vkDestroyPipeline, *device_, mode_data.store_pipeline,
                    nullptr);
    VK_SAFE_DESTROY(vkDestroyShaderModule, *device_,
                    mode_data.store_shader_module, nullptr);
  }

  VK_SAFE_DESTROY(vkDestroyPipelineLayout, *device_,
                  pipeline_layout_clear_color_, nullptr);
  VK_SAFE_DESTROY(vkDestroyDescriptorSetLayout, *device_,
                  descriptor_set_layout_clear_color_, nullptr);
  VK_SAFE_DESTROY(vkDestroyPipelineLayout, *device_, pipeline_layout_depth_,
                  nullptr);
  VK_SAFE_DESTROY(vkDestroyDescriptorSetLayout, *device_,
                  descriptor_set_layout_depth_, nullptr);
  VK_SAFE_DESTROY(vkDestroyPipelineLayout, *device_, pipeline_layout_color_,
                  nullptr);
  VK_SAFE_DESTROY(vkDestroyDescriptorSetLayout, *device_,
                  descriptor_set_layout_color_, nullptr);

  VK_SAFE_DESTROY(vkDestroyBufferView, *device_,
                  depth_copy_buffer_view_stencil_, nullptr);
  VK_SAFE_DESTROY(vkDestroyBufferView, *device_, depth_copy_buffer_view_depth_,
                  nullptr);
  VK_SAFE_DESTROY(vkDestroyBuffer, *device_, depth_copy_buffer_, nullptr);
  VK_SAFE_DESTROY(vkFreeMemory, *device_, depth_copy_memory_, nullptr);

  VK_SAFE_DESTROY(vkDestroyImage, *device_, edram_image_, nullptr);
  VK_SAFE_DESTROY(vkFreeMemory, *device_, edram_memory_, nullptr);
}

void EDRAMStore::TransitionEDRAMImage(VkCommandBuffer command_buffer,
                                      bool load) {
  EDRAMImageState new_state =
      load ? EDRAMImageState::kLoad : EDRAMImageState::kStore;
  if (edram_image_state_ == new_state) {
    return;
  }
  VkImageMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.dstAccessMask =
      load ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = edram_image_;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  VkPipelineStageFlags stage_mask_src;
  if (edram_image_state_ == EDRAMImageState::kUntransitioned) {
    barrier.srcAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    stage_mask_src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  } else {
    barrier.srcAccessMask =
        load ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    stage_mask_src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }
  vkCmdPipelineBarrier(command_buffer, stage_mask_src,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
  edram_image_state_ = new_state;
}

void EDRAMStore::TransitionDepthCopyBuffer(VkCommandBuffer command_buffer,
                                           DepthCopyBufferState new_state) {
  if (depth_copy_buffer_state_ == new_state) {
    return;
  }
  VkBufferMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = depth_copy_buffer_;
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;
  VkPipelineStageFlags stage_mask_src, stage_mask_dst;
  switch (depth_copy_buffer_state_) {
  case DepthCopyBufferState::kUntransitioned:
    barrier.srcAccessMask = 0;
    stage_mask_src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    break;
  case DepthCopyBufferState::kRenderTargetToBuffer:
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    stage_mask_src = VK_PIPELINE_STAGE_TRANSFER_BIT;
    break;
  case DepthCopyBufferState::kBufferToEDRAM:
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    stage_mask_src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    break;
  case DepthCopyBufferState::kEDRAMToBuffer:
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    stage_mask_src = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    break;
  case DepthCopyBufferState::kBufferToRenderTarget:
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    stage_mask_src = VK_PIPELINE_STAGE_TRANSFER_BIT;
    break;
  default:
    assert_unhandled_case(depth_copy_buffer_state_);
    return;
  }
  switch (new_state) {
  case DepthCopyBufferState::kRenderTargetToBuffer:
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    stage_mask_dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
    break;
  case DepthCopyBufferState::kBufferToEDRAM:
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    stage_mask_dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    break;
  case DepthCopyBufferState::kEDRAMToBuffer:
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    stage_mask_dst = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    break;
  case DepthCopyBufferState::kBufferToRenderTarget:
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    stage_mask_dst = VK_PIPELINE_STAGE_TRANSFER_BIT;
    break;
  default:
    assert_unhandled_case(new_state);
    return;
  }
  vkCmdPipelineBarrier(command_buffer, stage_mask_src, stage_mask_dst, 0, 0,
                       nullptr, 1, &barrier, 0, nullptr);
  depth_copy_buffer_state_ = new_state;
}

EDRAMStore::Mode EDRAMStore::GetColorMode(ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case ColorRenderTargetFormat::k_2_10_10_10:
    case ColorRenderTargetFormat::k_16_16:
    case ColorRenderTargetFormat::k_16_16_FLOAT:
    case ColorRenderTargetFormat::k_2_10_10_10_AS_16_16_16_16:
    case ColorRenderTargetFormat::k_32_FLOAT:
      return Mode::k_32bpp;
    case ColorRenderTargetFormat::k_16_16_16_16:
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return Mode::k_64bpp;
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return Mode::k_7e3;
    default:
      break;
  }
  return Mode::k_ModeUnsupported;
}

EDRAMStore::Mode EDRAMStore::GetDepthMode(DepthRenderTargetFormat format) {
  switch (format) {
    case DepthRenderTargetFormat::kD24S8:
      return Mode::k_D24;
    case DepthRenderTargetFormat::kD24FS8:
      return Mode::k_D24F;
    default:
      break;
  }
  return Mode::k_ModeUnsupported;
}

VkFormat EDRAMStore::GetStoreColorImageViewFormat(
    ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case ColorRenderTargetFormat::k_16_16_16_16:
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_UINT;
    default:
      break;
  }
  return VK_FORMAT_R32_UINT;
}

void EDRAMStore::GetPixelEDRAMSizePower(bool format_64bpp, MsaaSamples samples,
                                        uint32_t& width_power, uint32_t& height_power) {
  // For 64bpp images, each pixel is split into 2 EDRAM texels horizontally.
  // For 2X MSAA, two samples of each pixels are placed vertically.
  // For 4X MSAA, the additional samples are also placed horizontally.
  uint32_t pixel_width_power = 0, pixel_height_power = 0;
  if (format_64bpp) {
    ++pixel_width_power;
  }
  if (samples >= MsaaSamples::k2X) {
    ++pixel_height_power;
    if (samples >= MsaaSamples::k4X) {
      ++pixel_width_power;
    }
  }
  width_power = pixel_width_power;
  height_power = pixel_height_power;
}

bool EDRAMStore::GetDimensions(
    bool format_64bpp, MsaaSamples samples, uint32_t edram_base_offset_tiles,
    uint32_t edram_pitch_px, VkRect2D rt_rect, VkRect2D& rt_rect_adjusted,
    uint32_t& edram_add_offset_tiles, VkExtent2D& edram_extent_tiles,
    uint32_t& edram_pitch_tiles) {
  // Check if the area is not empty or outside the bounds.
  if (edram_base_offset_tiles >= 2048 || edram_pitch_px == 0 ||
      rt_rect.extent.width == 0 || rt_rect.extent.height == 0 ||
      uint32_t(rt_rect.offset.x) >= edram_pitch_px) {
    return false;
  }

  // Scales applied to framebuffer dimensions to get tile counts.
  // Tiles are always 5120 bytes long, and at 32bpp without MSAA they're 80x16.
  // The EDRAM storage image is split into 80x16 tiles, but one framebuffer
  // pixel can take multiple texels in the EDRAM image with a 64bpp format or
  // with multisampling.
  uint32_t pixel_width_power, pixel_height_power;
  GetPixelEDRAMSizePower(format_64bpp, samples, pixel_width_power,
                         pixel_height_power);

  // Scale the framebuffer area relatively to EDRAM image texels.
  rt_rect.offset.x <<= pixel_width_power;
  rt_rect.offset.y <<= pixel_height_power;
  rt_rect.extent.width <<= pixel_width_power;
  rt_rect.extent.height <<= pixel_height_power;
  edram_pitch_px <<= pixel_width_power;

  // Snap dimensions to whole tiles.
  uint32_t rt_rect_tiles_left = uint32_t(rt_rect.offset.x) / 80;
  uint32_t rt_rect_tiles_right =
      xe::round_up(uint32_t(rt_rect.offset.x) + rt_rect.extent.width, 80) / 80;
  uint32_t rt_rect_tiles_top = uint32_t(rt_rect.offset.y) >> 4;
  uint32_t rt_rect_tiles_bottom =
      xe::round_up(uint32_t(rt_rect.offset.y) + rt_rect.extent.height, 16) >> 4;
  uint32_t edram_pitch = xe::round_up(edram_pitch_px, 80) / 80;

  // Check if a framebuffer area wider than the surface pitch was requested.
  // Shouldn't happen actually, but just in case.
  uint32_t rt_rect_tiles_width = rt_rect_tiles_right - rt_rect_tiles_left;
  rt_rect_tiles_width = std::min(rt_rect_tiles_width, edram_pitch);

  // Calculate additional offset to the region being stored.
  uint32_t edram_add_offset =
      rt_rect_tiles_top * edram_pitch + rt_rect_tiles_left;
  uint32_t edram_offset = edram_base_offset_tiles + edram_add_offset;

  // Clamp the height in case the framebuffer size was highly overestimated.
  // This, on the other hand, may happen.
  uint32_t rt_rect_tiles_height = rt_rect_tiles_bottom - rt_rect_tiles_top;
  if (edram_offset + rt_rect_tiles_height * edram_pitch > 2048) {
    rt_rect_tiles_height = (2048 - edram_offset) / edram_pitch;
    if (rt_rect_tiles_height == 0) {
      return false;
    }
  }

  // Return the new dimensions. Keep SSAA, but revert 64bpp width scale.
  rt_rect_adjusted.offset.x = rt_rect_tiles_left * 80;
  rt_rect_adjusted.offset.y = rt_rect_tiles_top << 4;
  rt_rect_adjusted.extent.width = rt_rect_tiles_width * 80;
  rt_rect_adjusted.extent.height = rt_rect_tiles_height << 4;
  if (format_64bpp) {
    rt_rect_adjusted.offset.x >>= 1;
    rt_rect_adjusted.extent.width >>= 1;
  }
  edram_add_offset_tiles = edram_add_offset;
  edram_extent_tiles.width = rt_rect_tiles_width;
  edram_extent_tiles.height = rt_rect_tiles_height;
  edram_pitch_tiles = edram_pitch;
  return true;
}

uint32_t EDRAMStore::GetMaxHeight(bool format_64bpp, MsaaSamples samples,
                                  uint32_t offset_tiles, uint32_t pitch_px) {
  if (pitch_px == 0) {
    return 0;
  }
  uint32_t pixel_width_power, pixel_height_power;
  GetPixelEDRAMSizePower(format_64bpp, samples, pixel_width_power,
                         pixel_height_power);
  uint32_t edram_pitch_tiles =
      xe::round_up(pitch_px << pixel_width_power, 80) / 80;
  uint32_t height = ((2048 - offset_tiles) / edram_pitch_tiles) <<
                    (4 - pixel_height_power);
  return std::min(height, 2560u);
}

void EDRAMStore::CopyColor(VkCommandBuffer command_buffer, VkFence fence,
                           bool load, VkImageView rt_image_view_u32,
                           ColorRenderTargetFormat rt_format,
                           MsaaSamples rt_samples, VkRect2D rt_rect,
                           uint32_t edram_offset_tiles,
                           uint32_t edram_pitch_px) {
  XELOGGPU("EDRAM StoreColor (%s): offset %u, pitch %u, height %u.\n",
           load ? "load" : "store", edram_offset_tiles, edram_pitch_px,
           rt_rect.extent.height);

  Mode mode = GetColorMode(rt_format);
  if (mode == Mode::k_ModeUnsupported) {
    return;
  }
  const ModeInfo& mode_info = mode_info_[size_t(mode)];

  // Get the dimensions for the copying.
  VkRect2D rt_rect_adjusted;
  uint32_t edram_add_offset_tiles, edram_pitch_tiles;
  VkExtent2D edram_extent_tiles;
  if (!GetDimensions(mode_info.is_64bpp, rt_samples, edram_offset_tiles,
                     edram_pitch_px, rt_rect, rt_rect_adjusted,
                     edram_add_offset_tiles, edram_extent_tiles,
                     edram_pitch_tiles)) {
    return;
  }

  // Allocate space for the descriptors.
  if (!descriptor_pool_->has_open_batch()) {
    descriptor_pool_->BeginBatch(fence);
  }
  VkDescriptorSet descriptor_set =
      descriptor_pool_->AcquireEntry(descriptor_set_layout_color_);
  if (!descriptor_set) {
    assert_always();
    descriptor_pool_->CancelBatch();
    return;
  }

  // Switch the EDRAM image to the requested state.
  TransitionEDRAMImage(command_buffer, load);

  // Write the descriptors.
  VkWriteDescriptorSet descriptors[2];
  // EDRAM.
  descriptors[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[0].pNext = nullptr;
  descriptors[0].dstSet = descriptor_set;
  descriptors[0].dstBinding = 0;
  descriptors[0].dstArrayElement = 0;
  descriptors[0].descriptorCount = 1;
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorImageInfo image_info_edram;
  image_info_edram.sampler = nullptr;
  image_info_edram.imageView = edram_image_view_;
  image_info_edram.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  descriptors[0].pImageInfo = &image_info_edram;
  descriptors[0].pBufferInfo = nullptr;
  descriptors[0].pTexelBufferView = nullptr;
  // Render target.
  descriptors[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[1].pNext = nullptr;
  descriptors[1].dstSet = descriptor_set;
  descriptors[1].dstBinding = 1;
  descriptors[1].dstArrayElement = 0;
  descriptors[1].descriptorCount = 1;
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorImageInfo image_info_rt;
  image_info_rt.sampler = nullptr;
  image_info_rt.imageView = rt_image_view_u32;
  image_info_rt.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  descriptors[1].pImageInfo = &image_info_rt;
  descriptors[1].pBufferInfo = nullptr;
  descriptors[1].pTexelBufferView = nullptr;
  vkUpdateDescriptorSets(*device_, 2, descriptors, 0, nullptr);

  // Dispatch the computation.
  ModeData& mode_data = mode_data_[size_t(mode)];
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    load ? mode_data.load_pipeline : mode_data.store_pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_layout_color_, 0, 1, &descriptor_set,
                          0, nullptr);
  PushConstantsColor push_constants;
  push_constants.edram_offset_tiles = edram_offset_tiles +
                                      edram_add_offset_tiles;
  push_constants.edram_pitch_tiles = edram_pitch_tiles;
  push_constants.rt_offset_px[0] = uint32_t(rt_rect_adjusted.offset.x);
  push_constants.rt_offset_px[1] = uint32_t(rt_rect_adjusted.offset.y);
  vkCmdPushConstants(command_buffer, pipeline_layout_color_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                     &push_constants);
  uint32_t group_count_y = edram_extent_tiles.height;
  if (!mode_info.is_64bpp) {
    // For 32bpp modes, tiles are split into 2 groups because 1280 threads
    // may be over the limit.
    group_count_y *= 2;
  }
  vkCmdDispatch(command_buffer, edram_extent_tiles.width, group_count_y, 1);

  if (!load) {
    // Commit the write so loads or overlapping writes won't conflict.
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 0, nullptr);
  }
}

void EDRAMStore::CopyDepth(VkCommandBuffer command_buffer, VkFence fence,
                           bool load, VkImage rt_image,
                           DepthRenderTargetFormat rt_format,
                           MsaaSamples rt_samples, VkRect2D rt_rect,
                           uint32_t edram_offset_tiles,
                           uint32_t edram_pitch_px) {
  Mode mode = GetDepthMode(rt_format);
  if (mode == Mode::k_ModeUnsupported) {
    return;
  }
  const ModeInfo& mode_info = mode_info_[size_t(mode)];

  // Get the dimensions for the copying.
  VkRect2D rt_rect_adjusted;
  uint32_t edram_add_offset_tiles, edram_pitch_tiles;
  VkExtent2D edram_extent_tiles;
  if (!GetDimensions(false, rt_samples, edram_offset_tiles, edram_pitch_px,
                     rt_rect, rt_rect_adjusted, edram_add_offset_tiles,
                     edram_extent_tiles, edram_pitch_tiles)) {
    return;
  }

  // Allocate space for the descriptors.
  if (!descriptor_pool_->has_open_batch()) {
    descriptor_pool_->BeginBatch(fence);
  }
  VkDescriptorSet descriptor_set =
      descriptor_pool_->AcquireEntry(descriptor_set_layout_depth_);
  if (!descriptor_set) {
    assert_always();
    descriptor_pool_->CancelBatch();
    return;
  }

  // Switch the image to the needed state.
  TransitionEDRAMImage(command_buffer, load);

  // Prepare for copying to or from the linear buffer.
  // TODO(Triang3l): Copy entirely if granularity is 0 rather than 1.
  // Desktop GPUs have the granularity of 1, but PowerVR has 0.
  VkBufferImageCopy regions[2];
  regions[0].bufferOffset = 0;
  regions[0].bufferRowLength = rt_rect_adjusted.extent.width;
  regions[0].bufferImageHeight = rt_rect_adjusted.extent.height;
  regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  regions[0].imageSubresource.mipLevel = 0;
  regions[0].imageSubresource.baseArrayLayer = 0;
  regions[0].imageSubresource.layerCount = 1;
  regions[0].imageOffset.x = rt_rect_adjusted.offset.x;
  regions[0].imageOffset.y = rt_rect_adjusted.offset.y;
  regions[0].imageOffset.z = 0;
  regions[0].imageExtent.width = rt_rect_adjusted.extent.width;
  regions[0].imageExtent.height = rt_rect_adjusted.extent.height;
  regions[0].imageExtent.depth = 1;
  regions[1] = regions[0];
  regions[1].bufferOffset = kTotalTexelCount * sizeof(float);
  regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;

  // Copy the depth to the linear buffer if we're storing, and transition.
  if (load) {
    TransitionDepthCopyBuffer(command_buffer,
                              DepthCopyBufferState::kEDRAMToBuffer);
  } else {
    TransitionDepthCopyBuffer(command_buffer,
                              DepthCopyBufferState::kRenderTargetToBuffer);
    vkCmdCopyImageToBuffer(command_buffer, rt_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           depth_copy_buffer_, 2, regions);
    TransitionDepthCopyBuffer(command_buffer,
                              DepthCopyBufferState::kBufferToEDRAM);
  }

  // Write the descriptors.
  VkWriteDescriptorSet descriptors[3];
  // EDRAM.
  descriptors[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[0].pNext = nullptr;
  descriptors[0].dstSet = descriptor_set;
  descriptors[0].dstBinding = 0;
  descriptors[0].dstArrayElement = 0;
  descriptors[0].descriptorCount = 1;
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorImageInfo image_info_edram;
  image_info_edram.sampler = nullptr;
  image_info_edram.imageView = edram_image_view_;
  image_info_edram.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  descriptors[0].pImageInfo = &image_info_edram;
  descriptors[0].pBufferInfo = nullptr;
  descriptors[0].pTexelBufferView = nullptr;
  // Depth.
  descriptors[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[1].pNext = nullptr;
  descriptors[1].dstSet = descriptor_set;
  descriptors[1].dstBinding = 1;
  descriptors[1].dstArrayElement = 0;
  descriptors[1].descriptorCount = 1;
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
  descriptors[1].pImageInfo = nullptr;
  descriptors[1].pBufferInfo = nullptr;
  descriptors[1].pTexelBufferView = &depth_copy_buffer_view_depth_;
  // Stencil.
  descriptors[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptors[2].pNext = nullptr;
  descriptors[2].dstSet = descriptor_set;
  descriptors[2].dstBinding = 2;
  descriptors[2].dstArrayElement = 0;
  descriptors[2].descriptorCount = 1;
  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
  descriptors[2].pImageInfo = nullptr;
  descriptors[2].pBufferInfo = nullptr;
  descriptors[2].pTexelBufferView = &depth_copy_buffer_view_stencil_;
  vkUpdateDescriptorSets(*device_, 3, descriptors, 0, nullptr);

  // Dispatch the computation.
  ModeData& mode_data = mode_data_[size_t(mode)];
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    load ? mode_data.load_pipeline : mode_data.store_pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_layout_depth_, 0, 1, &descriptor_set,
                          0, nullptr);
  PushConstantsDepth push_constants;
  push_constants.edram_offset_tiles = edram_offset_tiles +
                                      edram_add_offset_tiles;
  push_constants.edram_pitch_tiles = edram_pitch_tiles;
  push_constants.buffer_pitch_px = rt_rect_adjusted.extent.width;
  vkCmdPushConstants(command_buffer, pipeline_layout_depth_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                     &push_constants);
  // 2 groups per tile because 1280 threads may be over the limit.
  vkCmdDispatch(command_buffer, edram_extent_tiles.width,
                edram_extent_tiles.height * 2, 1);

  if (load) {
    // Copy the loaded depth to the render target.
    TransitionDepthCopyBuffer(command_buffer,
                              DepthCopyBufferState::kBufferToRenderTarget);
    vkCmdCopyBufferToImage(command_buffer, depth_copy_buffer_, rt_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
  } else {
    // Commit the write so loads or overlapping writes won't conflict.
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 0, nullptr);
  }
}

void EDRAMStore::ClearColor(VkCommandBuffer command_buffer, VkFence fence,
                            bool format_64bpp, MsaaSamples samples,
                            uint32_t offset_tiles, uint32_t pitch_px,
                            uint32_t height_px, uint32_t color_high,
                            uint32_t color_low) {
  XELOGGPU("EDRAM ClearColor: pitch %u, height %u.\n", pitch_px, height_px);

  // Get the clear region size.
  VkRect2D rect;
  rect.offset.x = 0;
  rect.offset.y = 0;
  rect.extent.width = pitch_px;
  rect.extent.height = height_px;
  VkRect2D rect_adjusted;
  uint32_t offset_tiles_add;
  VkExtent2D extent_tiles;
  uint32_t pitch_tiles;
  if (!GetDimensions(format_64bpp, samples, offset_tiles, pitch_px, rect,
                     rect_adjusted, offset_tiles_add, extent_tiles,
                     pitch_tiles)) {
    return;
  }

  // Allocate space for the descriptors.
  if (!descriptor_pool_->has_open_batch()) {
    descriptor_pool_->BeginBatch(fence);
  }
  VkDescriptorSet descriptor_set =
      descriptor_pool_->AcquireEntry(descriptor_set_layout_clear_color_);
  if (!descriptor_set) {
    assert_always();
    descriptor_pool_->CancelBatch();
    return;
  }

  // Switch the EDRAM image to storing.
  TransitionEDRAMImage(command_buffer, false);

  // Write the EDRAM image descriptor.
  VkWriteDescriptorSet descriptor;
  descriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor.pNext = nullptr;
  descriptor.dstSet = descriptor_set;
  descriptor.dstBinding = 0;
  descriptor.dstArrayElement = 0;
  descriptor.descriptorCount = 1;
  descriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  VkDescriptorImageInfo image_info_edram;
  image_info_edram.sampler = nullptr;
  image_info_edram.imageView = edram_image_view_;
  image_info_edram.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  descriptor.pImageInfo = &image_info_edram;
  descriptor.pBufferInfo = nullptr;
  descriptor.pTexelBufferView = nullptr;
  vkUpdateDescriptorSets(*device_, 1, &descriptor, 0, nullptr);

  // Dispatch the computation.
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    clear_color_pipeline_);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline_layout_clear_color_, 0, 1, &descriptor_set,
                          0, nullptr);
  PushConstantsClearColor push_constants;
  push_constants.offset_tiles = offset_tiles + offset_tiles_add;
  push_constants.pitch_tiles = pitch_tiles;
  // TODO(Triang3l): Verify component order.
  push_constants.color_high = color_high;
  push_constants.color_low = format_64bpp ? color_low : color_high;
  vkCmdPushConstants(command_buffer, pipeline_layout_clear_color_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                     &push_constants);
  vkCmdDispatch(command_buffer, extent_tiles.width, extent_tiles.height, 1);
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 0, nullptr);
}

void EDRAMStore::ClearDepth(VkCommandBuffer command_buffer, VkFence fence,
                            MsaaSamples samples, uint32_t offset_tiles,
                            uint32_t pitch_px, uint32_t height_px,
                            uint32_t stencil_depth) {
  // TODO(Triang3l): Clear the host depth when it's added.
  ClearColor(command_buffer, fence, false, samples, offset_tiles, pitch_px,
             height_px, stencil_depth, stencil_depth);
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
