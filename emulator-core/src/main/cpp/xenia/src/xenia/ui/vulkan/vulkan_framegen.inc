// XenDroid host-side Win-FG integration. Included in namespace xe::ui::vulkan.
// Off-by-default. Only the scheduled presenter thread records interpolation;
// producer threads publish notifications and never sleep for synthetic frames.
static int64_t FrameGenerationNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

static VulkanDevice* winfg_native_device = nullptr;
static VkResult VKAPI_CALL WinfgNativeWaitIdle(VkDevice device) {
  if (!winfg_native_device) return VK_ERROR_DEVICE_LOST;
  std::vector<std::unique_ptr<VulkanDevice::Queue::Acquisition>> locks;
  for (const auto& family : winfg_native_device->queue_families()) {
    for (const auto& queue : family.queues) locks.push_back(std::make_unique<VulkanDevice::Queue::Acquisition>(*queue));
  }
  return winfg_native_device->functions().vkDeviceWaitIdle(device);
}

struct VulkanPresenter::FrameGenContext {
  winfg::DeviceDispatch dd;
  winfg::InstanceDispatch id;
  winfg::FrameGen engine;
  std::unique_ptr<lsfg::Engine> lsfg_engine;
  // ZeroFG runs in place of Win-FG (kind 0) when the device can; null means Win-FG proper.
  std::unique_ptr<zerofg::Interpolator> zerofg;
  VkExtent2D zerofg_extent = {};
  uint64_t zerofg_counter = 0;
  uint64_t zerofg_current_sequence = 0;
  uint64_t zerofg_previous_sequence = 0;
  int kind = 0;
  uint64_t source_frames = 0;
  uint64_t epoch = 0;
  uint32_t planned_generations = 0;
  std::chrono::steady_clock::time_point last_input;
  std::shared_ptr<GuestOutputImage> previous, current, generated;
  GuestOutputProperties properties;
  bool have_previous = false;
  bool current_initialized = false;
  bool previous_initialized = false;
  bool generated_initialized = false;
  bool query_pending = false;
  bool query_engine_pending = false;
  // 0/1: the whole generation pass, barriers included (as before). 2/3: only the
  // engine call itself (ZeroFG Interpolate / Win-FG record / LSFG generateInto),
  // so the gap between the two pairs is what the barriers either side cost.
  VkQueryPool timestamps = VK_NULL_HANDLE;
  uint32_t timestamp_valid_bits = 0;  // of the graphics/compute queue family
  const VulkanDevice* device;
  int preset = -1;
  // last_refresher_submission of the guest output last taken in as an input.
  uint64_t ingested_sequence = UINT64_MAX;
  // This cycle's first synthetic output generated from the newest input (LSFG's later
  // outputs are generations of it).
  bool cycle_generated = false;
  // debug.xendroid.winfg_stage when frame generation was switched on.
  uint32_t stage = 0;
  std::vector<std::shared_ptr<GuestOutputImage>> source_references;

  explicit FrameGenContext(VulkanDevice* d) : device(d) {
    const auto& f = d->functions();
#define WFG_BIND(name) dd.name = f.vk##name
    WFG_BIND(CreateImage); WFG_BIND(DestroyImage); WFG_BIND(CreateImageView); WFG_BIND(DestroyImageView);
    WFG_BIND(AllocateMemory); WFG_BIND(FreeMemory); WFG_BIND(BindImageMemory); WFG_BIND(GetImageMemoryRequirements);
    WFG_BIND(CreateBuffer); WFG_BIND(DestroyBuffer); WFG_BIND(GetBufferMemoryRequirements); WFG_BIND(BindBufferMemory);
    WFG_BIND(MapMemory); WFG_BIND(UnmapMemory); WFG_BIND(CreateSampler); WFG_BIND(DestroySampler);
    WFG_BIND(CreateShaderModule); WFG_BIND(DestroyShaderModule); WFG_BIND(CreateDescriptorSetLayout); WFG_BIND(DestroyDescriptorSetLayout);
    WFG_BIND(CreatePipelineLayout); WFG_BIND(DestroyPipelineLayout); WFG_BIND(CreateComputePipelines); WFG_BIND(DestroyPipeline);
    WFG_BIND(CreateDescriptorPool); WFG_BIND(DestroyDescriptorPool); WFG_BIND(ResetDescriptorPool); WFG_BIND(AllocateDescriptorSets); WFG_BIND(UpdateDescriptorSets);
    WFG_BIND(CmdBindPipeline); WFG_BIND(CmdBindDescriptorSets); WFG_BIND(CmdDispatch); WFG_BIND(CmdPipelineBarrier);
    WFG_BIND(CmdCopyImage); WFG_BIND(CmdBlitImage); WFG_BIND(CmdClearColorImage);
#undef WFG_BIND
    winfg_native_device = d;
    dd.DeviceWaitIdle = &WinfgNativeWaitIdle;
    id.GetPhysicalDeviceMemoryProperties = d->vulkan_instance()->functions().vkGetPhysicalDeviceMemoryProperties;
  }
  ~FrameGenContext() {
    engine.destroy();
    // destroy() above is the last user of the wait-idle hook for this device.
    if (winfg_native_device == device) winfg_native_device = nullptr;
    if (timestamps) device->functions().vkDestroyQueryPool(device->device(), timestamps, nullptr);
  }
};

bool VulkanPresenter::ScheduleGuestOutput() {
  if (!RuntimePresentation().frame_generation_requested.load(std::memory_order_relaxed)) return false;
  if (!CanScheduleGuestOutput()) {
    RuntimePresentation().frame_generation_state = int(FrameGenerationState::kUnsupported);
    RuntimePresentation().frame_generation_error = 9;
    return false;
  }
  const int64_t now = FrameGenerationNowNs();
  {
    // The thread starts under the queue's lock, so two producers never start two.
    std::lock_guard<std::mutex> lock(fg_queue_.mutex);
    if (!fg_thread_.joinable()) fg_thread_ = std::thread(&VulkanPresenter::FrameGenerationThread, this);
    ++fg_queue_.notification;
    fg_queue_.arrival_ns = now;
  }
  fg_queue_.condition.notify_one();
  return true;
}

// The loop and its timing live in presentation_runtime.h (RunFrameGenerationLoop and
// FrameGenerationSchedule, covered by the host tests with a fake painter); here it only
// paints. Lock order: the queue's mutex is never held while painting, and painting takes
// paint_mode_mutex_ (PaintGuestFromScheduledThread) - so the producer
// (ScheduleGuestOutput, which takes only the queue's mutex) can never wait on a paint.
void VulkanPresenter::FrameGenerationThread() {
  RunFrameGenerationLoop(fg_queue_, RuntimePresentation(),
                         [this](int phase) { PaintGuestFromScheduledThread(phase); });
}

void VulkanPresenter::StopFrameGenerationThread() {
  fg_queue_.Shutdown();
  if (fg_thread_.joinable()) fg_thread_.join();
}

void VulkanPresenter::ResetFrameGeneration() {
  if (!fg_context_) return;
  paint_context_.completion_timeline.AwaitAllSubmissions();
  fg_context_.reset();
}

// debug.xendroid.winfg_stage: on-device bisection of frame generation, read when it is
// switched on. Bits: 1 = Win-FG without its global-motion pre-warp (C1), 2 = without its
// flow regularization (C2), 4 = no generation (the synthetic outputs show the newest real
// frame: only the thread, the copies and the FIFO swapchain run), 8 = no GPU timestamps.
constexpr uint32_t kFrameGenerationStageNoGlobalMotion = 1;
constexpr uint32_t kFrameGenerationStageNoFlowRegularization = 2;
constexpr uint32_t kFrameGenerationStageNoGeneration = 4;
constexpr uint32_t kFrameGenerationStageNoTimestamps = 8;
static uint32_t FrameGenerationStage() {
#if XE_PLATFORM_ANDROID || XE_PLATFORM_xendroid
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.winfg_stage", value) > 0) {
    return uint32_t(std::strtoul(value, nullptr, 0));
  }
#endif
  return 0;
}

// The Win-FG preset for an input size: the chosen one, or a coarser one when its finest
// flow level would pass 640x360 (+10%). The flow search costs per pixel of that level, so
// at 2x and 3x resolution scales Quality searched 4 and 9 times the pixels it does at 1x.
static int WinfgPresetForExtent(int preset, VkExtent2D extent) {
  const uint64_t pixels = uint64_t(extent.width) * extent.height;
  for (int p = std::clamp(preset, 0, 2); p < 2; ++p) {
    if ((pixels >> (2 * winfg::FrameGen::flowFinestForPreset(p))) <= 640 * 360 * 11 / 10) {
      return p;
    }
  }
  return 2;
}

// The loader's entry point, for ZeroFG: it loads everything else through it.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* name);

// What the guest output images are created with (GuestOutputImage::Create adds
// the transfer-source, sampled and color attachment usage to the storage usage).
constexpr VkImageUsageFlags kZeroFgSourceUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// An output that isn't A2B10G10R10 is written through ZeroFG's own image and blitted,
// which needs the transfer destination usage.
constexpr VkImageUsageFlags kZeroFgOutputUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// ZeroFG (Apache-2.0) in place of Win-FG. Returns null where it can't run (Vulkan 1.3
// with synchronization2 is needed, and a picture size its grid accepts), so the caller
// falls back to Win-FG.
static std::unique_ptr<zerofg::Interpolator> CreateZeroFgEngine(
    const VulkanDevice* vulkan_device, VkExtent2D extent) {
  if (vulkan_device->properties().apiVersion < VK_API_VERSION_1_3) {
    XELOGW("ZeroFG: needs Vulkan 1.3, using Win-FG");
    return nullptr;
  }
  const auto& ifn = vulkan_device->vulkan_instance()->functions();
  VkPhysicalDeviceVulkan13Features features_13 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features features_12 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  features_12.pNext = &features_13;
  VkPhysicalDeviceFeatures2 features_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features_2.pNext = &features_12;
  ifn.vkGetPhysicalDeviceFeatures2(vulkan_device->physical_device(), &features_2);
  if (!features_13.synchronization2) {
    XELOGW("ZeroFG: synchronization2 isn't supported, using Win-FG");
    return nullptr;
  }
  VkPhysicalDeviceSubgroupProperties subgroup = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceProperties2 properties_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties_2.pNext = &subgroup;
  ifn.vkGetPhysicalDeviceProperties2(vulkan_device->physical_device(), &properties_2);

  zerofg::CreateInfo info;
  info.vulkan.instance = vulkan_device->vulkan_instance()->instance();
  info.vulkan.physical_device = vulkan_device->physical_device();
  info.vulkan.device = vulkan_device->device();
  info.vulkan.get_instance_proc_addr = vkGetInstanceProcAddr;
  info.mode = zerofg::Mode::kReallyZero;
  info.backend = zerofg::Backend::kModern;
  info.capabilities.effective_api_version = VK_API_VERSION_1_3;
  info.capabilities.synchronization2_enabled = true;
  info.capabilities.shader_storage_image_extended_formats_enabled = true;
  info.capabilities.shader_float16_supported = features_12.shaderFloat16;
  info.capabilities.shader_float16_enabled = features_12.shaderFloat16;
  info.capabilities.subgroup_size = subgroup.subgroupSize;
  info.capabilities.subgroup_min_size = subgroup.subgroupSize;
  info.capabilities.subgroup_max_size = subgroup.subgroupSize;
  info.capabilities.subgroup_supported_operations = subgroup.supportedOperations;
  info.capabilities.subgroup_supported_stages = subgroup.supportedStages;
  info.frame_context_count = 1;
  zerofg::Status status;
  std::unique_ptr<zerofg::Interpolator> engine =
      zerofg::Interpolator::Create(info, &status);
  if (!engine) {
    XELOGW("ZeroFG: engine creation failed ({}), using Win-FG", int(status));
    return nullptr;
  }
  status = engine->Resize(extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_FORMAT_R8G8B8A8_UNORM);
  if (status != zerofg::Status::kSuccess) {
    XELOGW("ZeroFG: {}x{} was refused ({}), using Win-FG", extent.width,
           extent.height, int(status));
    return nullptr;
  }
  XELOGI("Frame generation: ZeroFG ready at {}x{}", extent.width, extent.height);
  return engine;
}

bool VulkanPresenter::PrepareGeneratedFrame(VkCommandBuffer cmd,
    std::shared_ptr<GuestOutputImage>& source, GuestOutputProperties& properties,
    uint64_t source_sequence) {
  auto& runtime = RuntimePresentation();
  const auto& f = vulkan_device_->functions();
  const auto& ifn = vulkan_device_->vulkan_instance()->functions();
  const auto device = vulkan_device_->device();
  // The upstream engine's descriptor scratch is single-flight. Preserve that
  // contract rather than updating a UBO/descriptor still being read by the GPU.
  if (!paint_context_.completion_timeline.AwaitAllSubmissions()) return false;
  if (fg_context_ && (fg_context_->kind != runtime.frame_generation_engine.load() ||
      fg_context_->epoch != runtime.configuration_epoch.load())) ResetFrameGeneration();
  const auto now = std::chrono::steady_clock::now();
  if (fg_context_ && fg_generated_phase_ && fg_context_->last_input.time_since_epoch().count() &&
      now - fg_context_->last_input > std::chrono::milliseconds(250)) {
    // A pause (loading, a menu): the newest input is from before it, so generation starts
    // over from the next two frames. The engine and its pipelines stay - recreating them
    // (10 compute pipelines for Win-FG, compiled again) cost a hitch after every pause.
    fg_context_->current_initialized = false;
    fg_context_->previous_initialized = false;
    fg_context_->have_previous = false;
    fg_context_->last_input = {};
    if (fg_context_->lsfg_engine) fg_context_->lsfg_engine->reset();
  }
  if (fg_context_) fg_context_->source_references.clear();
  if (!fg_context_) {
    VkFormatProperties source_format, rgba_format, float_format, flow_format;
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(), kGuestOutputFormat, &source_format);
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(), VK_FORMAT_R8G8B8A8_UNORM, &rgba_format);
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(), VK_FORMAT_R32_SFLOAT, &float_format);
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(), VK_FORMAT_R16G16B16A16_SFLOAT, &flow_format);
    if (!(source_format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(rgba_format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !(rgba_format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(float_format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(flow_format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
      runtime.frame_generation_error = 1; return false;
    }
    fg_context_ = std::make_shared<FrameGenContext>(vulkan_device_);
    fg_context_->kind = runtime.frame_generation_engine.load();
    fg_context_->epoch = runtime.configuration_epoch.load();
    fg_context_->stage = FrameGenerationStage();
    XELOGI("Frame generation: {} on, {}x output{}", fg_context_->kind == 1 ? "LSFG" : "Win-FG",
           fg_context_->kind == 1 ? runtime.frame_generation_multiplier.load() : 2,
           fg_context_->stage ? fmt::format(" (winfg_stage {})", fg_context_->stage) : std::string());
    auto queue = vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    if (fg_context_->kind == 0) {
      fg_context_->zerofg_extent = source->extent();
      fg_context_->zerofg = CreateZeroFgEngine(vulkan_device_, fg_context_->zerofg_extent);
    }
    if (fg_context_->kind == 0 && !fg_context_->zerofg && !fg_context_->engine.init(&fg_context_->dd, &fg_context_->id, vulkan_device_->physical_device(), device,
        vulkan_device_->queue_family_graphics_compute(), queue.queue())) {
      runtime.frame_generation_error = 2; return false;
    }
    if (fg_context_->kind == 1) {
      if (!vulkan_device_->properties().nullDescriptor || !vulkan_device_->properties().vulkanMemoryModel) {
        runtime.frame_generation_error = 8; return false;
      }
      LsfgVkDispatch lsfg_dispatch;
#define LSFG_COPY(name) lsfg_dispatch.name = f.vk##name
      LSFG_COPY(AllocateDescriptorSets); LSFG_COPY(AllocateMemory); LSFG_COPY(BindBufferMemory); LSFG_COPY(BindImageMemory);
      LSFG_COPY(CmdBindDescriptorSets); LSFG_COPY(CmdBindPipeline); LSFG_COPY(CmdCopyImage); LSFG_COPY(CmdDispatch); LSFG_COPY(CmdPipelineBarrier);
      LSFG_COPY(CreateBuffer); LSFG_COPY(CreateComputePipelines); LSFG_COPY(CreateDescriptorPool); LSFG_COPY(CreateDescriptorSetLayout);
      LSFG_COPY(CreateImage); LSFG_COPY(CreateImageView); LSFG_COPY(CreatePipelineLayout); LSFG_COPY(CreateSampler); LSFG_COPY(CreateShaderModule);
      LSFG_COPY(DestroyBuffer); LSFG_COPY(DestroyDescriptorPool); LSFG_COPY(DestroyDescriptorSetLayout); LSFG_COPY(DestroyImage); LSFG_COPY(DestroyImageView);
      LSFG_COPY(DestroyPipeline); LSFG_COPY(DestroyPipelineLayout); LSFG_COPY(DestroySampler); LSFG_COPY(DestroyShaderModule); LSFG_COPY(FreeMemory);
      LSFG_COPY(GetBufferMemoryRequirements); LSFG_COPY(GetImageMemoryRequirements); LSFG_COPY(MapMemory); LSFG_COPY(UnmapMemory); LSFG_COPY(UpdateDescriptorSets);
#undef LSFG_COPY
      lsfg_dispatch.GetPhysicalDeviceMemoryProperties = ifn.vkGetPhysicalDeviceMemoryProperties;
      std::string cache;
      { std::lock_guard<std::mutex> configuration(runtime.configuration_mutex); cache = runtime.lsfg_cache; }
      fg_context_->lsfg_engine = std::make_unique<lsfg::Engine>();
      const uint32_t target = vulkan_device_->properties().apiVersion >= VK_API_VERSION_1_3 ? lsfg::kSpirv16 : lsfg::kSpirv15;
      if (!lsfgVkdInit(lsfg_dispatch) || !fg_context_->lsfg_engine->init(device, vulkan_device_->physical_device(), cache, target)) {
        runtime.frame_generation_error = 7; return false;
      }
      fg_context_->lsfg_engine->configure(runtime.frame_generation_multiplier.load(), 0, 1.0f, runtime.display_hz.load());
    }
    uint32_t families = 0;
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> queues(families);
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families, queues.data());
    const uint32_t valid_bits = queues[vulkan_device_->queue_family_graphics_compute()].timestampValidBits;
    if (valid_bits && vulkan_device_->properties().timestampPeriod > 0 &&
        !(fg_context_->stage & kFrameGenerationStageNoTimestamps)) {
      VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = 4;
      if (f.vkCreateQueryPool(device, &info, nullptr, &fg_context_->timestamps) == VK_SUCCESS) {
        fg_context_->timestamp_valid_bits = valid_bits;
      } else {
        fg_context_->timestamps = VK_NULL_HANDLE;
      }
    }
  }
  auto& context = *fg_context_;
  // F08: the previous pass's GPU time (its submission completed above). Masked to the
  // queue's valid bits and wrap-safe; a failed read is "unavailable", never the old value.
  if (context.query_pending && context.timestamps) {
    uint64_t timestamps[4];
    const VkResult read = f.vkGetQueryPoolResults(device, context.timestamps, 0, 4, sizeof(timestamps), timestamps,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    RecordGenerationGpu(runtime, read == VK_SUCCESS
        ? TimestampElapsedNs(timestamps[0], timestamps[1], context.timestamp_valid_bits,
                             vulkan_device_->properties().timestampPeriod)
        : -1);
    // The engine-only pair (2/3) is only meaningful where it was actually written (the
    // LSFG warm-up path and the no-generation early-outs skip it), guarded the same way
    // query_pending itself is: context.query_engine_pending mirrors it per pass.
    if (context.query_engine_pending) {
      RecordGenerationEngineGpu(runtime, read == VK_SUCCESS
          ? TimestampElapsedNs(timestamps[2], timestamps[3], context.timestamp_valid_bits,
                               vulkan_device_->properties().timestampPeriod)
          : -1);
      context.query_engine_pending = false;
    }
    context.query_pending = false;
  }
  const bool fresh = source_sequence != context.ingested_sequence;
  if (!fg_generated_phase_) {
    // The real output, the guest output itself. A guest frame no synthetic output took in
    // (its slot was skipped as late, or it came after it) is taken in here without
    // generating, so it is shown when due and the next cycle starts from it. Before, the
    // copy in context.current was shown, which still held the previous frame.
    context.cycle_generated = false;
    return !fresh || TakeInGuestOutput(cmd, source, properties, source_sequence, false);
  }
  if (fg_generation_index_ > 1) {
    // LSFG's later outputs: further generations of the frame this cycle's first one took in.
    // Without one the newest real frame is shown again.
    if (context.kind != 1 || !context.cycle_generated ||
        uint32_t(fg_generation_index_) > context.planned_generations) {
      return true;
    }
    VkImageMemoryBarrier before{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    before.image = context.generated->image(); before.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    before.newLayout = VK_IMAGE_LAYOUT_GENERAL; before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &before);
    context.lsfg_engine->generateInto(cmd, fg_generation_index_ - 1, 0, context.generated->image(), context.generated->view(),
        context.current->extent().width, context.current->extent().height);
    std::swap(before.oldLayout, before.newLayout); std::swap(before.srcAccessMask, before.dstAccessMask);
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &before);
    properties = context.properties;
    source = context.generated; fg_actual_synthetic_ = true;
    return true;
  }
  // The cycle's first synthetic output. With no guest frame since the last output (the real
  // output took it in), the newest real frame is shown again: a frame between the two before
  // it would step back in time.
  return !fresh || TakeInGuestOutput(cmd, source, properties, source_sequence, true);
}

// The guest output becomes the newest input (the one before it the older); with `generate`,
// the frame between them is made and becomes `source`. Otherwise `source` stays the guest
// output (and LSFG only takes the frame in).
bool VulkanPresenter::TakeInGuestOutput(VkCommandBuffer cmd,
    std::shared_ptr<GuestOutputImage>& source, GuestOutputProperties& properties,
    uint64_t source_sequence, bool generate) {
  auto& runtime = RuntimePresentation();
  const auto& f = vulkan_device_->functions();
  auto& context = *fg_context_;
  const auto extent = source->extent();
  context.last_input = std::chrono::steady_clock::now();
  // Win-FG's preset for the input size and the stage's switches, logged when it changes.
  auto configure_winfg = [&]() {
    const int requested_preset = runtime.frame_generation_preset.load();
    const int preset = WinfgPresetForExtent(requested_preset, extent);
    winfg::Config cfg; cfg.enabled = true; cfg.model = 3; cfg.multiplier = 2; cfg.perfPreset = preset;
    if (context.stage & kFrameGenerationStageNoGlobalMotion) cfg.gmMode = 2;
    if (context.stage & kFrameGenerationStageNoFlowRegularization) cfg.frMode = 2;
    context.engine.configure(cfg); context.preset = preset;
    XELOGI("Frame generation: Win-FG preset {} at {}x{}{}", preset, extent.width, extent.height,
           preset != requested_preset ? fmt::format(" (preset {} chosen, coarser for the size)", requested_preset)
                                      : std::string());
  };
  context.ingested_sequence = source_sequence;
  if (!context.current || context.current->extent().width != extent.width || context.current->extent().height != extent.height) {
    context.previous = GuestOutputImage::Create(vulkan_device_, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    context.current = GuestOutputImage::Create(vulkan_device_, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    context.generated = GuestOutputImage::Create(vulkan_device_, extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    context.have_previous = false; context.current_initialized = false; context.previous_initialized = false; context.generated_initialized = false;
    context.preset = -1;
    if (context.lsfg_engine) context.lsfg_engine->forgetTargets();
    if (!context.current || !context.previous || !context.generated) { runtime.frame_generation_error = 3; return false; }
    if (context.kind == 0 && context.zerofg) {
      if (context.zerofg_extent.width != extent.width || context.zerofg_extent.height != extent.height) {
        const zerofg::Status resize_status = context.zerofg->Resize(
            extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM);
        if (resize_status != zerofg::Status::kSuccess) {
          XELOGW("ZeroFG: {}x{} was refused ({})", extent.width, extent.height, int(resize_status));
          runtime.frame_generation_error = 3; return false;
        }
        context.zerofg_extent = extent;
      }
    } else if (context.kind == 0) {
      // Configured before its images exist, so the preset's finest flow level sizes them
      // at once. Configured after them, a preset other than Balanced rebuilt them in the
      // middle of recording the first generated frame, a full device wait included.
      configure_winfg();
      if (!context.engine.onResize(extent, VK_FORMAT_R8G8B8A8_UNORM)) { runtime.frame_generation_error = 3; return false; }
    }
    if (context.kind == 1 && !context.lsfg_engine->prepare(extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM)) {
      runtime.frame_generation_error = 3; return false;
    }
  }
  if (context.current_initialized) {
    std::swap(context.current, context.previous);
    std::swap(context.current_initialized, context.previous_initialized);
    context.have_previous = context.previous_initialized;
  }
  context.properties = properties;
  context.zerofg_previous_sequence = context.zerofg_current_sequence;
  context.zerofg_current_sequence = ++context.zerofg_counter;
  auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
  };
  barrier(source->image(), kGuestOutputInternalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  barrier(context.current->image(), context.current_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageBlit blit{};
  blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
  f.vkCmdBlitImage(cmd, source->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, context.current->image(),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  barrier(source->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kGuestOutputInternalLayout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
  barrier(context.current->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  context.current_initialized = true;
  context.source_references.push_back(source);
  if (context.kind == 1) {
    // A frame taken in without generating still goes through the pacer and the engine, which
    // primes its history before generating again.
    const auto generations = context.lsfg_engine->plan(
        generate ? runtime.frame_generation_multiplier.load() - 1 : 0, ++context.source_frames);
    context.planned_generations = generations;
    // Timed: this source frame's processing plus its first generated frame. A warm-up
    // pass (no generation yet) is not a generation pass and stays out of the figures.
    if (context.timestamps && generations) {
      f.vkCmdResetQueryPool(cmd, context.timestamps, 0, 2);
      f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 0);
    }
    barrier(context.current->image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
    context.lsfg_engine->process(cmd, context.current->image(), extent.width, extent.height, generations);
    barrier(context.current->image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (!generations) {
      if (generate) runtime.frame_generation_state = int(FrameGenerationState::kWarmingUp);
      return true;
    }
    barrier(context.generated->image(), context.generated_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    if (context.timestamps) {
      f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 2);
    }
    context.lsfg_engine->generateInto(cmd, 0, 0, context.generated->image(), context.generated->view(), extent.width, extent.height);
    if (context.timestamps) {
      f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 3);
      f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 1);
      context.query_pending = true;
      context.query_engine_pending = true;
    }
    barrier(context.generated->image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    context.generated_initialized = true; context.cycle_generated = true;
    source = context.generated;
    fg_actual_synthetic_ = true;
    runtime.frame_generation_state = int(FrameGenerationState::kRunning);
    return true;
  }
  if (!generate) return true;
  if (!context.have_previous) {
    runtime.frame_generation_state = int(FrameGenerationState::kWarmingUp);
    return true;
  }
  if (context.stage & kFrameGenerationStageNoGeneration) {
    runtime.frame_generation_state = int(FrameGenerationState::kRunning);
    return true;
  }
  if (!context.zerofg && context.preset != WinfgPresetForExtent(runtime.frame_generation_preset.load(), extent)) {
    configure_winfg();
  }
  barrier(context.generated->image(), context.generated_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  if (context.timestamps) {
    f.vkCmdResetQueryPool(cmd, context.timestamps, 0, 2);
    f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 0);
  }
  if (context.timestamps) {
    f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 2);
  }
  if (context.zerofg) {
    auto zerofg_image = [&](const std::shared_ptr<GuestOutputImage>& image, VkImageLayout layout,
                            VkImageUsageFlags usage, uint64_t sequence) {
      zerofg::Image z;
      z.image = image->image();
      z.view = image->view();
      z.layout = layout;
      z.format = VK_FORMAT_R8G8B8A8_UNORM;
      z.width = extent.width;
      z.height = extent.height;
      z.usage = usage;
      z.tiling = VK_IMAGE_TILING_OPTIMAL;
      z.creation_facts_known = true;
      z.sequence = sequence;
      return z;
    };
    const zerofg::Status status = context.zerofg->Interpolate(
        cmd, 0,
        zerofg_image(context.previous, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kZeroFgSourceUsage,
                     context.zerofg_previous_sequence),
        zerofg_image(context.current, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kZeroFgSourceUsage,
                     context.zerofg_current_sequence),
        0.5f,
        zerofg_image(context.generated, VK_IMAGE_LAYOUT_GENERAL, kZeroFgOutputUsage,
                     ++context.zerofg_counter));
    if (status != zerofg::Status::kSuccess) {
      XELOGW("ZeroFG: interpolation failed ({})", int(status));
      runtime.frame_generation_error = 6; return false;
    }
  } else if (!context.engine.record(cmd, context.previous->view(), context.current->view(), context.generated->view(), 0.5f, 0)) {
    runtime.frame_generation_error = 6; return false;
  }
  if (context.timestamps) {
    f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 3);
    f.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, context.timestamps, 1);
    context.query_pending = true;
    context.query_engine_pending = true;
  }
  barrier(context.generated->image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  context.generated_initialized = true; context.cycle_generated = true;
  source = context.generated;
  fg_actual_synthetic_ = true;
  runtime.frame_generation_state = int(FrameGenerationState::kRunning);
  return true;
}
