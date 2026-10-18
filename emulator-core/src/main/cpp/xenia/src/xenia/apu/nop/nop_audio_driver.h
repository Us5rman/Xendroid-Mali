/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_NOP_NOP_AUDIO_DRIVER_H_
#define XENIA_APU_NOP_NOP_AUDIO_DRIVER_H_

#include <cstdint>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace nop {

// Accepts frames and discards them. The audio worker paces the guest's
// callbacks itself and uses the semaphore only as back-pressure, so a frame
// is released as soon as it is submitted.
class NopAudioDriver : public AudioDriver {
 public:
  explicit NopAudioDriver(xe::threading::Semaphore* semaphore);
  ~NopAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override {}
  void Shutdown() override {}
  // Reads as a full queue. The frames go nowhere and the semaphore is released
  // on submit, so with any lower depth apu_pump_topup would run the guest's
  // callback a second time every pump, twice real time.
  size_t GetQueuedFrameCount() override { return SIZE_MAX; }

 private:
  xe::threading::Semaphore* semaphore_ = nullptr;
};

}  // namespace nop
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_NOP_NOP_AUDIO_DRIVER_H_
