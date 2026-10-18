/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_TEXTURE_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_TEXTURE_CACHE_H_

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/hash.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/ui/vulkan/vulkan_mem_alloc.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanTextureCache final : public TextureCache {
 public:
  // Recorded per resolve so texture uploads can be tested against it.
  struct ResolveDestDescriptor {
    uint32_t base;          // copy_dest_base_unadjusted & 0x1FFFFFFF
    uint32_t pitch_div_32;  // copy_dest_coordinate_info.pitch_aligned_div_32
    uint32_t x0, y0, width, height;
    uint32_t format;    // xenos::ColorFormat
    uint32_t endian;    // copy_dest_info.copy_dest_endian
    uint32_t is_array;  // copy_dest_info.copy_dest_array
    // Whether the resolve also stored into the promoted texture. Every resolve
    // is recorded, because promotion decides from this history and cannot store
    // before it has happened, but only a resolve that actually wrote the image
    // may count towards covering it - one refused for pitch, format or bounds
    // updates shared memory alone, and serving over it shows the previous
    // frame's texels.
    uint32_t wrote_texture;
    // Memory is reused across scenes, so coverage must only trust resolves
    // from the frame being served.
    uint64_t frame;
  };
  void NoteResolveDestination(const ResolveDestDescriptor& desc);
  // Storage view of the promoted texture an in-pass resolve to `base` should
  // fill. Large surfaces are resolved in strips, each with its own advancing
  // base, so `base_delta_out` reports how far into the texture this strip
  // starts - the caller converts it to a row offset.
  struct ResolveDestTextureInfo {
    uint32_t width = 0, height = 0, pitch = 0, format = 0;
    // The texture itself, for the calls below (opaque to the caller).
    void* texture = nullptr;
  };
  VkImageView GetResolveDestStorageView(
      uint32_t base, uint32_t* base_delta_out,
      ResolveDestTextureInfo* info_out) const;
  // The same for a resolve done by a compute dispatch outside render passes,
  // whatever the texture's usage - BeginResolveDestComputeStore transitions it.
  // Memory reused across passes holds textures of several formats at once, so
  // only a texture whose pitch, format and endianness are the ones the resolve
  // writes qualifies (for depth, only k_24_8 and k_24_8_FLOAT, which the store
  // decodes like the upload does, and with depth_into_8888 also k_8_8_8_8,
  // which gets the packed words).
  VkImageView GetResolveDestStorageViewForCompute(
      uint32_t base, uint32_t pitch_div_32, xenos::TextureFormat format,
      bool is_depth, uint32_t endian, uint32_t* base_delta_out,
      ResolveDestTextureInfo* info_out, bool depth_into_8888 = false) const;
  // Makes the texture writable by the compute resolve about to be recorded
  // (the barrier is pushed, not submitted).
  void BeginResolveDestComputeStore(const ResolveDestTextureInfo& info);
  // Stamps the promoted texture at `base` as filled by a resolve this frame.
  void MarkResolveDestWritten(uint32_t base, uint64_t frame);
  void MarkResolveDestWritten(const ResolveDestTextureInfo& info,
                              uint64_t frame);
  // Around the resolve's MarkRangeAsResolved when it also stored the texels
  // into the texture: its write then keeps the texture's image valid instead
  // of invalidating it. The rectangle is where the texels landed, in texels of
  // the texture - stores that together cover every row revalidate an image
  // that other writes had invalidated.
  void BeginResolveStoreRangeWrite(const ResolveDestTextureInfo& info,
                                   int32_t x0, int32_t y0, uint32_t width,
                                   uint32_t height);
  void EndResolveStoreRangeWrite();
  // With the whole guest memory invalidated (no watch fires then): no image
  // may be taken as matching the memory any more.
  void InvalidateResolveStoreTracking();

  // Diagnostics: the textures waiting to be recreated for promotion at the
  // last submission, and since the previous call, the attempts that recreated
  // one, found no texture with the key, or found it still in use.
  struct PromotionQueueStats {
    uint64_t pending = 0;
    uint64_t recreated = 0;
    uint64_t not_found = 0;
    uint64_t in_flight = 0;
  };
  PromotionQueueStats TakePromotionQueueStats() {
    PromotionQueueStats stats = promotion_queue_stats_;
    promotion_queue_stats_ = PromotionQueueStats();
    stats.pending = resolve_dest_promotion_queue_.size();
    return stats;
  }

 private:
  class VulkanTexture;
  // Textures that already existed when a resolve first targeted them: they
  // were created without STORAGE usage, so they must be recreated once before
  // they can be promoted. Without this only textures that happen to be created
  // while a matching resolve is live ever get promoted.
  std::vector<TextureKey> resolve_dest_promotion_queue_;
  PromotionQueueStats promotion_queue_stats_;
  // Keys whose promoted image the driver refused, created unpromoted.
  std::unordered_set<TextureKey, TextureKey::Hasher>
      resolve_dest_promotion_refused_;
  // Promoted textures by guest base address, so an in-pass resolve can find
  // the image it is about to fill. Several textures of different formats can
  // share a base. Entries are removed by ~VulkanTexture.
  std::unordered_multimap<uint32_t, VulkanTexture*> resolve_dest_textures_;

  // Store tracking of the images of promoted textures (see VulkanTexture's
  // store_tracked_valid): the texture a direct host resolve stored into, while
  // its write to the memory is being marked, and the rectangle it stored.
  VulkanTexture* resolve_store_in_flight_ = nullptr;
  int32_t resolve_store_x0_ = 0, resolve_store_y0_ = 0;
  uint32_t resolve_store_width_ = 0, resolve_store_height_ = 0;
  // Watches the base level memory of a texture for writes other than its own
  // stores (with the global critical region held).
  void ArmResolveStoreWatch(VulkanTexture& texture);
  static void ResolveStoreWatchCallback(
      const global_unique_lock_type& global_lock, void* context, void* data,
      uint64_t argument, bool invalidated_by_gpu);

  // Sized well above the per-frame resolve count so a frame stays visible.
  static constexpr size_t kResolveDestHistory = 256;
  std::array<ResolveDestDescriptor, kResolveDestHistory> resolve_dests_{};
  size_t resolve_dests_next_ = 0;
  // Whether a resolve wrote everything the texture would be uploaded from.
  bool IsResolveDestEligible(const Texture& texture) const;
  // Whether full-width resolves that STORED INTO THE IMAGE have covered every
  // row of the surface. Format and endian are matched per span: a same-base
  // resolve in another format was refused the store, so it covers nothing.
  bool ResolveDestsCoverSurface(uint32_t base, uint32_t size_bytes,
                                uint32_t pitch_div_32, uint32_t width,
                                uint32_t height, xenos::TextureFormat format,
                                uint32_t endian, uint64_t frame) const;
  VulkanTexture* FindResolveDestTexture(
      uint32_t base, uint32_t* base_delta_out = nullptr) const;
  bool ShouldPromoteToResolveDest(const TextureKey& key) const;
  // The format of the image of a texture with the key, and with mutable
  // formats the other one of its unsigned and signed versions.
  void GetTextureImageFormats(const TextureKey& key,
                              VkFormat formats_out[2]) const;
  // The uint format a resolve stores into the image of a texture with the key
  // through, VK_FORMAT_UNDEFINED if it can't be a resolve destination - the
  // structural match of ShouldPromoteToResolveDest, then the host format.
  VkFormat GetResolveDestUintFormat(const TextureKey& key,
                                    VkFormat host_format) const;
  // Whether a texture with the key created now would be promoted (the
  // recreation queue takes only those - one that wouldn't be was recreated
  // unpromoted at every load).
  bool CanPromoteToResolveDest(const TextureKey& key) const;
  // Whether the texture can be served from its resolve instead of uploaded.
  bool TryServeFromResolveDest(const VulkanTexture& texture, bool load_base,
                               bool load_mips) const;
  // The upload itself (LoadTextureDataFromResidentMemoryImpl minus serving).
  bool LoadTextureDataFromResidentMemoryUpload(VulkanTexture& vulkan_texture,
                                               bool load_base, bool load_mips);
  // After an upload or a serve of the base level of a promoted texture: the
  // image matches the memory, and store tracking watches it from now on.
  void NoteResolveDestImageUpToDate(VulkanTexture& texture, bool load_base);
  // vulkan_resolve_dest_diag: why an upload of GPU-written memory was not
  // served - the reason and the misses for it, per texture.
  void LogResolveDestMiss(const VulkanTexture& texture, bool load_mips);
  std::unordered_map<TextureKey, std::pair<uint32_t, uint32_t>,
                     TextureKey::Hasher>
      resolve_dest_diag_reasons_;
  // The load shader of the host format a texture with the key is loaded to.
  LoadShaderIndex GetLoadShaderForKey(const TextureKey& key) const;
  // Whether textures with the key are created with the R32_UINT storage alias
  // for loads straight into the image (vulkan_texture_load_to_image) - the
  // host format is checked by the caller.
  bool IsLoadToImageCandidate(const TextureKey& key) const;

 public:
  // Sampler parameters that can be directly converted to a host sampler or used
  // for checking whether samplers bindings are up to date.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Force the border color alpha to 1.0 (only meaningful with a border
      // clamp mode).
      uint32_t force_bc_w_to_max : 1;  // 23
      // Maximum mip level is in the texture resource itself, but mip_base_map
      // limits fetching to mip_min_level (level 0 when the base is available).
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<uint32_t>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& parameters) const {
      return value == parameters.value;
    }
    bool operator!=(const SamplerParameters& parameters) const {
      return value != parameters.value;
    }
  };

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  static std::unique_ptr<VulkanTextureCache> Create(
      const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages) {
    std::unique_ptr<VulkanTextureCache> texture_cache(new VulkanTextureCache(
        register_file, shared_memory, draw_resolution_scale_x,
        draw_resolution_scale_y, command_processor,
        guest_shader_pipeline_stages));
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return std::move(texture_cache);
  }

  ~VulkanTextureCache();

  void BeginSubmission(uint64_t new_submission_index) override;

  // Must be called within a frame - creates and untiles textures needed by
  // shaders, and enqueues transitioning them into the sampled usage. This may
  // bind compute pipelines (notifying the command processor about that), and
  // also since it may insert deferred barriers, before flushing the barriers
  // preceding host GPU work.
  void RequestTextures(uint32_t used_texture_mask) override;

  // Layout the active binding's image currently resides in: GENERAL for
  // promoted resolve destinations, SHADER_READ_ONLY_OPTIMAL otherwise (and for
  // the null views). Descriptor writes must match it or sampling is undefined.
  VkImageLayout GetActiveBindingImageLayout(uint32_t fetch_constant_index,
                                            xenos::FetchOpDimension dimension,
                                            bool is_signed) const;
  VkImageView GetActiveBindingOrNullImageView(uint32_t fetch_constant_index,
                                              xenos::FetchOpDimension dimension,
                                              bool is_signed);
  // The null image view for the dimension, with the components the host
  // swizzle makes constant 1 mapped to 1.
  VkImageView GetNullImageView(xenos::FetchOpDimension dimension,
                               uint32_t host_swizzle);

  // Descriptor set (kStorageBufferCompute layout) binding the whole shared
  // memory buffer for compute load/store, or VK_NULL_HANDLE if the buffer
  // doesn't fit in maxStorageBufferRange. When valid, the byte offset into the
  // buffer must be supplied via push constants. Shared with resolve.
  VkDescriptorSet shared_memory_persistent_descriptor_set() const {
    return shared_memory_persistent_descriptor_set_;
  }

  // Fetch constants whose host image views have changed since the bits were
  // last reset, for reusing texture descriptor sets across draws when bindings
  // are unchanged. Accumulated whenever UpdateTextureBindingsImpl rewrites
  // bindings.
  uint32_t texture_bindings_changed() const {
    return texture_bindings_changed_;
  }
  void ResetTextureBindingsChanged(uint32_t mask) {
    texture_bindings_changed_ &= ~mask;
  }

  SamplerParameters GetSamplerParameters(
      const VulkanShader::SamplerBinding& binding) const;

  // Must be called for every used sampler at least once in a single submission,
  // and a submission must be open for this to be callable.
  // Returns:
  // - The sampler, if obtained successfully - and increases its last usage
  //   submission index - and has_overflown_out = false.
  // - VK_NULL_HANDLE and has_overflown_out = true if there's a total sampler
  //   count overflow in a submission that potentially hasn't completed yet.
  // - VK_NULL_HANDLE and has_overflown_out = false in case of a general failure
  //   to create a sampler.
  VkSampler UseSampler(SamplerParameters parameters, bool& has_overflown_out);
  // Returns the submission index to await (may be the current submission in
  // case of an overflow within a single submission - in this case, it must be
  // ended, and a new one must be started) in case of sampler count overflow, so
  // samplers may be freed, and UseSamplers may take their slots.
  uint64_t GetSubmissionToAwaitOnSamplerOverflow(
      uint32_t overflowed_sampler_count) const;

  // Incremented whenever any VkSampler is destroyed (LRU eviction in
  // UseSampler, cache teardown). Lets the command processor's cross-draw
  // sampler cache detect that cached handles may no longer be valid.
  uint64_t sampler_destroy_generation() const {
    return sampler_destroy_generation_;
  }

  // Returns the 2D view of the front buffer texture (for fragment shader
  // reading - the barrier will be pushed in the command processor if needed),
  // or VK_NULL_HANDLE in case of failure. May call LoadTextureData.
  VkImageView RequestSwapTexture(uint32_t& width_scaled_out,
                                 uint32_t& height_scaled_out,
                                 xenos::TextureFormat& format_out);

  // Scaled resolve buffer management (for use by VulkanRenderTargetCache)
  // Simple non-overlapping buffer (fallback when sparse binding unavailable)
  struct ScaledResolveBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint64_t size = 0;
    uint64_t range_start_scaled = 0;
    uint64_t range_length_scaled = 0;
  };

  // Sparse buffer wrapper for overlapping 2GB windows
  class ScaledResolveSparseBuffer {
   public:
    explicit ScaledResolveSparseBuffer(VkBuffer buffer) : buffer_(buffer) {}

    VkBuffer buffer() const { return buffer_; }

   private:
    VkBuffer buffer_ = VK_NULL_HANDLE;
  };

  // Constants for sparse scaled resolve
  static constexpr uint32_t kScaledResolveHeapSizeLog2 = 24;  // 16MB heaps
  static constexpr uint32_t kScaledResolveHeapSize =
      uint32_t(1) << kScaledResolveHeapSizeLog2;
  static constexpr uint64_t kScaledResolveSparseBufferSize =
      uint64_t(2) << 30;  // 2GB per buffer

  // Public scaled resolve buffer methods for use by VulkanRenderTargetCache
  bool EnsureScaledResolveMemoryCommittedPublic(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) {
    return EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                              length_scaled_alignment_log2);
  }

  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                     uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);

  VkBuffer GetCurrentScaledResolveBuffer() const;

  // Returns the base scaled address that the current buffer starts at.
  // For sparse buffers: buffer N starts at N GB (N << 30)
  // For simple buffers: returns the buffer's range_start_scaled
  uint64_t GetCurrentScaledResolveBufferBaseOffset() const {
    if (sparse_scaled_resolve_supported_) {
      return uint64_t(scaled_resolve_current_buffer_index_) << 30;
    }
    if (scaled_resolve_current_buffer_index_ < scaled_resolve_buffers_.size()) {
      return scaled_resolve_buffers_[scaled_resolve_current_buffer_index_]
          .range_start_scaled;
    }
    return 0;
  }

  size_t GetScaledResolveCurrentBufferIndex() const {
    return scaled_resolve_current_buffer_index_;
  }

  const ScaledResolveBuffer* GetScaledResolveBufferInfo(size_t index) const {
    if (index < scaled_resolve_buffers_.size()) {
      return &scaled_resolve_buffers_[index];
    }
    return nullptr;
  }

  // Get the current scaled resolve range (set by MakeScaledResolveRangeCurrent)
  uint64_t GetCurrentScaledResolveRangeStartScaled() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  uint64_t GetCurrentScaledResolveRangeLengthScaled() const {
    return scaled_resolve_current_range_length_scaled_;
  }

 protected:
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

  bool EnsureScaledResolveMemoryCommitted(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) override;

  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  enum LoadDescriptorSetIndex {
    kLoadDescriptorSetIndexDestination,
    kLoadDescriptorSetIndexSource,
    kLoadDescriptorSetCount,
  };

  struct HostFormat {
    LoadShaderIndex load_shader;
    // Do NOT add integer formats to this - they are not filterable, can only be
    // read with ImageFetch, not ImageSample! Games that fetch fixed-point
    // formats are handled after sampling by scaling the normalized host value
    // back to the guest integer range (see GetIntegerScaleBits). Keep these as
    // sampled float/normalized views.
    VkFormat format;
    // Whether the format is block-compressed on the host (the host block size
    // matches the guest format block size in this case), and isn't decompressed
    // on load.
    bool block_compressed;

    // Set up dynamically based on what's supported by the device.
    bool linear_filterable;
  };

  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Mapping of Xenos swizzle components to Vulkan format components.
    uint32_t swizzle;
    // Whether the unsigned and the signed formats are compatible for one image
    // and the same image data (on a portability subset device, this should also
    // take imageViewFormatReinterpretation into account).
    bool unsigned_signed_compatible;
  };

  class VulkanTexture final : public Texture {
   public:
    enum class Usage {
      kUndefined,
      kTransferDestination,
      kGuestShaderSampled,
      kSwapSampled,
      // Promoted resolve destinations park here: GENERAL layout, sampleable
      // AND storable, so an in-pass resolve never needs a mid-pass layout
      // transition. STORAGE usage already forfeits UBWC on Adreno, so
      // sampling from GENERAL costs these images nothing extra.
      kResolveDestStorage,
      // Written by a texture load compute shader through load_storage_view()
      // (GENERAL).
      kLoadStorageWrite,
    };

   private:
    VkImageView resolve_dest_storage_view_ = VK_NULL_HANDLE;
    // R32_UINT alias of the base level for texture loads straight into the
    // image (vulkan_texture_load_to_image); may be resolve_dest_storage_view_.
    VkImageView load_storage_view_ = VK_NULL_HANDLE;
    // Its uint format, for the views of the other levels and layers below.
    VkFormat load_storage_format_ = VK_FORMAT_UNDEFINED;
    // Single-level, single-layer uint views of the levels and layers past the
    // base layer of level 0, which a load of a texture with mips or layers
    // stores through ([level * layers + layer]), created on first use.
    std::vector<VkImageView> load_storage_level_views_;
    uint64_t resolve_dest_written_frame_ = 0;
    bool pending_storage_write_ = false;
    // Whether the image still holds what the guest memory of the base level
    // holds, as this key reads it - so a reload can be skipped. Set by an
    // upload, kept by the direct host resolves that store the same texels into
    // the image, cleared by any other write to the memory (seen through
    // store_watch_handle_, as the base watch is gone once outdated). Accessed
    // with the global critical region held.
    bool store_tracked_valid_ = false;
    // Rows [0, store_covered_rows_) rewritten by stores since the image became
    // invalid; revalidates it once they reach the height.
    uint32_t store_covered_rows_ = 0;
    SharedMemory::WatchHandle store_watch_handle_ = nullptr;
    // vulkan_resolve_dest_diag: what last made the image invalid
    // (StoreInvalidation), and with a resolve storing into another texture
    // over the memory, that texture's base and size.
    uint32_t store_invalidation_ = 0;
    uint32_t store_invalidation_other_base_ = 0;
    uint32_t store_invalidation_other_width_ = 0;
    uint32_t store_invalidation_other_height_ = 0;

   public:
    // Takes ownership of the image and its memory.
    // track_usage: if false, texture won't participate in LRU cache eviction.
    explicit VulkanTexture(VulkanTextureCache& texture_cache,
                           const TextureKey& key, VkImage image,
                           VmaAllocation allocation, bool track_usage = true);
    ~VulkanTexture();

    VkImage image() const { return image_; }

    // Uint-aliased storage view an in-pass resolve writes through. Null unless
    // the texture was promoted at creation.
    VkImageView resolve_dest_storage_view() const {
      return resolve_dest_storage_view_;
    }
    void SetResolveDestStorageView(VkImageView view) {
      resolve_dest_storage_view_ = view;
    }
    VkImageView load_storage_view() const { return load_storage_view_; }
    void SetLoadStorageView(VkImageView view, VkFormat format) {
      load_storage_view_ = view;
      load_storage_format_ = format;
    }
    // The uint storage view of one level and layer (load_storage_view() for
    // the base layer of level 0), or null if it can't be created.
    VkImageView GetLoadStorageLevelView(uint32_t level, uint32_t layer);
    uint64_t resolve_dest_written_frame() const {
      return resolve_dest_written_frame_;
    }
    void SetResolveDestWrittenFrame(uint64_t frame) {
      resolve_dest_written_frame_ = frame;
    }
    bool store_tracked_valid() const { return store_tracked_valid_; }
    void SetStoreTrackedValid(bool valid) {
      store_tracked_valid_ = valid;
      if (!valid) {
        store_covered_rows_ = 0;
      }
    }
    uint32_t store_covered_rows() const { return store_covered_rows_; }
    void SetStoreCoveredRows(uint32_t rows) { store_covered_rows_ = rows; }
    uint32_t store_invalidation() const { return store_invalidation_; }
    uint32_t store_invalidation_other_base() const {
      return store_invalidation_other_base_;
    }
    uint32_t store_invalidation_other_width() const {
      return store_invalidation_other_width_;
    }
    uint32_t store_invalidation_other_height() const {
      return store_invalidation_other_height_;
    }
    void SetStoreInvalidation(uint32_t cause, uint32_t other_base = 0,
                              uint32_t other_width = 0,
                              uint32_t other_height = 0) {
      store_invalidation_ = cause;
      store_invalidation_other_base_ = other_base;
      store_invalidation_other_width_ = other_width;
      store_invalidation_other_height_ = other_height;
    }
    SharedMemory::WatchHandle store_watch_handle() const {
      return store_watch_handle_;
    }
    void SetStoreWatchHandle(SharedMemory::WatchHandle handle) {
      store_watch_handle_ = handle;
    }
    Usage usage() const { return usage_; }
    // An in-pass store happened and no barrier has covered it yet; the next
    // bind must emit one even though the usage does not change.
    bool ConsumePendingStorageWrite() {
      bool pending = pending_storage_write_;
      pending_storage_write_ = false;
      return pending;
    }
    void SetPendingStorageWrite() { pending_storage_write_ = true; }

    // Doesn't transition (the caller must insert the barrier).
    Usage SetUsage(Usage new_usage) {
      Usage old_usage = usage_;
      usage_ = new_usage;
      return old_usage;
    }

    VkImageView GetView(bool is_signed, uint32_t host_swizzle,
                        bool is_array = true);

    // For 3D textures sampled as 2D - creates a 2D copy of slice 0.
    VkImageView GetOrCreate3DAs2DImageView(bool is_signed,
                                           uint32_t host_swizzle);

   private:
    union ViewKey {
      uint32_t key;
      struct {
        uint32_t is_signed_separate_view : 1;
        uint32_t host_swizzle : 12;
        uint32_t is_array : 1;
      };

      ViewKey() : key(0) { static_assert_size(*this, sizeof(key)); }

      struct Hasher {
        size_t operator()(const ViewKey& key) const {
          return std::hash<decltype(key.key)>{}(key.key);
        }
      };
      bool operator==(const ViewKey& other_key) const {
        return key == other_key.key;
      }
      bool operator!=(const ViewKey& other_key) const {
        return !(*this == other_key);
      }
    };

    static constexpr VkComponentSwizzle GetComponentSwizzle(
        uint32_t texture_swizzle, uint32_t component_index) {
      xenos::XE_GPU_TEXTURE_SWIZZLE texture_component_swizzle =
          xenos::XE_GPU_TEXTURE_SWIZZLE(
              (texture_swizzle >> (3 * component_index)) & 0b111);
      if (texture_component_swizzle ==
          xenos::XE_GPU_TEXTURE_SWIZZLE(component_index)) {
        // The portability subset requires all swizzles to be IDENTITY, return
        // IDENTITY specifically, not R, G, B, A.
        return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
      switch (texture_component_swizzle) {
        case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
          return VK_COMPONENT_SWIZZLE_R;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
          return VK_COMPONENT_SWIZZLE_G;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
          return VK_COMPONENT_SWIZZLE_B;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
          return VK_COMPONENT_SWIZZLE_A;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
          return VK_COMPONENT_SWIZZLE_ZERO;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
          return VK_COMPONENT_SWIZZLE_ONE;
        default:
          // An invalid value.
          return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
    }

    VkImage image_;
    VmaAllocation allocation_;

    Usage usage_ = Usage::kUndefined;

    std::unordered_map<ViewKey, VkImageView, ViewKey::Hasher> views_;
    // GetView's memo (texture_lookup_memo): the arguments of the last call
    // that returned a view, and the view - most textures are always sampled
    // the same way.
    VkImageView last_view_ = VK_NULL_HANDLE;
    uint32_t last_view_arguments_ = 0;

    // For 3D textures sampled as 2D - cached 2D texture loaded from slice 0.
    // Uses a modified key (depth=1) with 3D tiling to read from guest memory.
    std::unique_ptr<VulkanTexture> texture_3d_as_2d_;
    VkImageView image_view_3d_as_2d_unsigned_ = VK_NULL_HANDLE;
    VkImageView image_view_3d_as_2d_signed_ = VK_NULL_HANDLE;
  };

  struct VulkanTextureBinding {
    VkImageView image_view_unsigned;
    VkImageView image_view_signed;

    VulkanTextureBinding() { Reset(); }

    void Reset() {
      image_view_unsigned = VK_NULL_HANDLE;
      image_view_signed = VK_NULL_HANDLE;
    }
  };

  struct Sampler {
    VkSampler sampler;
    uint64_t last_usage_submission;
    std::pair<const SamplerParameters, Sampler>* used_previous;
    std::pair<const SamplerParameters, Sampler>* used_next;
  };

  uint64_t sampler_destroy_generation_ = 0;

  static constexpr bool AreDimensionsCompatible(
      xenos::FetchOpDimension binding_dimension,
      xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  explicit VulkanTextureCache(
      const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages);

  bool Initialize();

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;

  void GetTextureUsageMasks(VulkanTexture::Usage usage,
                            VkPipelineStageFlags& stage_mask,
                            VkAccessFlags& access_mask, VkImageLayout& layout);
  // Transitions a texture for guest-shader use: kResolveDestStorage (GENERAL)
  // for promoted resolve destinations, kGuestShaderSampled otherwise, with a
  // barrier when the usage changes or an in-pass store is pending.
  void TransitionTextureForGuestShader(VulkanTexture& texture);

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  // Sparse scaled resolve helper functions
  bool InitializeSparseScaledResolve();
  void ShutdownSparseScaledResolve();
  size_t GetScaledResolveSparseBufferCount() const;
  std::array<size_t, 2> GetPossibleScaledResolveBufferIndices(
      uint64_t address_scaled) const;
  bool EnsureScaledResolveMemoryCommittedSparse(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2);
  bool MakeScaledResolveRangeCurrentSparse(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2);
  void BindHeapToOverlappingBuffers(uint32_t heap_index, VkDeviceMemory heap);

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Using the Vulkan Memory Allocator because texture count in games is
  // naturally pretty much unbounded, while Vulkan implementations, especially
  // on Windows versions before 10, may have an allocation count limit as low as
  // 4096.
  VmaAllocator vma_allocator_ = VK_NULL_HANDLE;

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  HostFormatPair host_formats_[64];

  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_scaled_{};
  // Unscaled variants with whole-cache-line accesses per instruction, for the
  // load shaders that have one (vulkan_texture_load_coalesced).
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_coalesced_{};
  // Variants storing straight into the texture's R32_UINT alias instead of a
  // buffer to copy to the image (vulkan_texture_load_to_image), with a storage
  // image destination descriptor set.
  VkPipelineLayout load_pipeline_layout_image_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_image_{};
  // vulkan_texture_load_to_image at startup: eligible textures are created
  // with the R32_UINT storage alias (the cvar itself can be switched later to
  // compare the load paths on the same images).
  bool load_to_image_storage_ = false;

  // Persistent descriptor binding the whole shared memory buffer
  // (kStorageBufferCompute layout) for compute load/store, so per-operation
  // transient descriptors don't need to be allocated and written. Only created
  // when the buffer fits in maxStorageBufferRange; the byte offset into the
  // buffer is passed via push constants instead. Used as the source of texture
  // loads here, and shared as the destination of resolves in the render target
  // cache.
  VkDescriptorPool shared_memory_persistent_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_persistent_descriptor_set_ = VK_NULL_HANDLE;

  // Accumulated mask of fetch constants whose host image views were rewritten,
  // consumed by the command processor to decide texture descriptor set reuse.
  uint32_t texture_bindings_changed_ = 0;

  // If both images can be placed in the same allocation, it's one allocation,
  // otherwise it's two separate.
  std::array<VkDeviceMemory, 2> null_images_memory_{};
  VkImage null_image_2d_array_cube_ = VK_NULL_HANDLE;
  VkImage null_image_3d_ = VK_NULL_HANDLE;
  VkImageView null_image_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView null_image_view_cube_ = VK_NULL_HANDLE;
  VkImageView null_image_view_3d_ = VK_NULL_HANDLE;
  // Null views per dimension (2D array, cube, 3D) and per mask of components
  // that are constant 1, created on first use (GetNullImageView).
  std::array<std::array<VkImageView, 16>, 3> null_image_views_ones_{};
  bool null_images_cleared_ = false;

  std::array<VulkanTextureBinding, xenos::kTextureFetchConstantCount>
      vulkan_texture_bindings_;

  uint32_t sampler_max_count_;

  xenos::AnisoFilter max_anisotropy_;

  std::unordered_map<SamplerParameters, Sampler, SamplerParameters::Hasher>
      samplers_;
  std::pair<const SamplerParameters, Sampler>* sampler_used_first_ = nullptr;
  std::pair<const SamplerParameters, Sampler>* sampler_used_last_ = nullptr;

  // Scaled resolve buffer storage (simple non-overlapping, fallback path)
  std::vector<ScaledResolveBuffer> scaled_resolve_buffers_;
  // Current scaled resolve range tracking
  uint64_t scaled_resolve_current_range_start_scaled_ = 0;
  uint64_t scaled_resolve_current_range_length_scaled_ = 0;
  size_t scaled_resolve_current_buffer_index_ = SIZE_MAX;

  // Sparse scaled resolve (overlapping 2GB windows)
  bool sparse_scaled_resolve_supported_ = false;
  // 2GB overlapping sparse buffers - buffer N covers [N GB ... (N+2) GB)
  // For 3x3 scale (4.5GB), need 4 buffers: 0:[0-2GB), 1:[1-3GB), 2:[2-4GB),
  // 3:[3-4.5GB)
  static constexpr size_t kMaxScaledResolveSparseBuffers =
      (uint64_t(SharedMemory::kBufferSize) * kMaxDrawResolutionScaleAlongAxis *
           kMaxDrawResolutionScaleAlongAxis -
       1) >>
      30;
  std::array<std::unique_ptr<ScaledResolveSparseBuffer>,
             kMaxScaledResolveSparseBuffers>
      scaled_resolve_sparse_buffers_;
  // 16MB heaps that can be mapped to multiple buffer regions
  std::vector<VkDeviceMemory> scaled_resolve_heaps_;
  uint32_t scaled_resolve_heap_count_ = 0;
  // Memory type for sparse allocations
  uint32_t scaled_resolve_memory_type_ = UINT32_MAX;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_TEXTURE_CACHE_H_
