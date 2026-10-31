#include "session/state/EstablishedState.h"

namespace packetmux {

EstablishedState::EstablishedState(SessionProcessor* const o_processor)
    : SessionStateBase(o_processor) {}

SessionState EstablishedState::id() const { return SESSION_STATE_ESTABLISHED; }

void EstablishedState::onEnter() {
  m_processor->markEstablished();
  m_processor->keepAlive().reset(m_processor->nowMs());
}

SessionStep EstablishedState::onAttach() {
  return stay(SESSION_STATUS_ALREADY_CONNECTED);
}

SessionStep EstablishedState::onMessage(
    const std::span<const uint8_t> i_message) {
  FrameView frame;
  const CodecStatus status = m_processor->decode(i_message, &frame);
  if (status != CODEC_STATUS_OK) {
    return failConnection(status);
  }
  m_processor->keepAlive().onFrameReceived(m_processor->nowMs());

  switch (frame.type) {
    case FRAME_TYPE_HELLO:
      return failConnection(CODEC_STATUS_OK);
    case FRAME_TYPE_GOAWAY:
      m_processor->setCloseCode(frame.code);
      return moveTo(SESSION_STATE_CLOSED);
    case FRAME_TYPE_PING:
      m_processor->queuePong(frame.nonce);
      break;
    case FRAME_TYPE_PONG:
      m_processor->keepAlive().onPong(frame.nonce, m_processor->nowMs());
      break;
    case FRAME_TYPE_OPEN:
      if (!m_processor->streams().onPeerOpen(frame)) {
        return failConnection(CODEC_STATUS_OK);
      }
      break;
    default:
      m_processor->streams().onFrame(frame);
      break;
  }

  // Checked once, here, so no handler can end the connection halfway through.
  if (m_processor->controlQueueOverCap()) {
    m_processor->setDisconnectReason(REASON_CODE_LIMIT_EXCEEDED);
    return moveTo(SESSION_STATE_DISCONNECTED);
  }
  return stay();
}

SessionStep EstablishedState::onConnectionLost() {
  m_processor->setDisconnectReason(REASON_CODE_TRANSPORT_LOST);
  return moveTo(SESSION_STATE_DISCONNECTED);
}

SessionStep EstablishedState::onTick() {
  uint64_t nonce = 0U;
  const KeepAliveAction action =
      m_processor->keepAlive().poll(m_processor->nowMs(), &nonce);
  if (action == KEEP_ALIVE_ACTION_DEAD) {
    m_processor->setDisconnectReason(REASON_CODE_TIMEOUT);
    return moveTo(SESSION_STATE_DISCONNECTED);
  }
  if (action == KEEP_ALIVE_ACTION_SEND_PING) {
    m_processor->queuePing(nonce);
  }
  return stay();
}

SessionStep EstablishedState::onClose(const ReasonCode i_code) {
  m_processor->setCloseCode(i_code);
  m_processor->queueGoaway(i_code);
  return moveTo(SESSION_STATE_CLOSING);
}

SessionStep EstablishedState::onOpenStream(const OpenRequest& i_request,
                                           uint32_t* const o_streamId) {
  return stay(m_processor->streams().open(i_request.kind, i_request.params,
                                          o_streamId));
}

SessionStep EstablishedState::onTransmitDrained() { return stay(); }

uint64_t EstablishedState::nextDeadlineMs() const {
  return m_processor->keepAlive().nextDeadlineMs();
}

SessionStep EstablishedState::failConnection(const CodecStatus i_status) {
  m_processor->setDisconnectReason(REASON_CODE_PROTOCOL_ERROR, i_status);
  return moveTo(SESSION_STATE_DRAINING);
}

}  // namespace packetmux
