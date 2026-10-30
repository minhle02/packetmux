#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "packetmux/Frames.h"

namespace packetmux {

/**
 * Stateless encoder and decoder for one frame per message. Neither method
 * throws.
 */
class Codec {
 public:
  Codec() = delete;

  /**
   * Decode one message into o_frame. Never allocates. o_frame borrows from
   * i_message, so it stays valid only while i_message does. Checks run in a
   * fixed order, so the status is deterministic. On failure, type and streamId
   * are meaningful only if the header was read, and version only for
   * CODEC_STATUS_UNSUPPORTED_VERSION.
   */
  static CodecStatus decode(const std::span<const uint8_t> i_message,
                            const FrameLimits& i_limits,
                            FrameView* const o_frame);

  /**
   * Append one complete message to o_message. On failure o_message is
   * unchanged. The views in i_frame must not point into o_message: appending
   * can reallocate it.
   */
  static CodecStatus encode(const FrameView& i_frame,
                            const FrameLimits& i_limits,
                            std::vector<uint8_t>* const o_message);
};

}  // namespace packetmux
