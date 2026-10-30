#include "session/EventQueue.h"

namespace packetmux {

void EventQueue::push(const Event& i_event) { m_events.push_back(i_event); }

bool EventQueue::pop(Event* const o_event) {
  if (m_events.empty()) {
    return false;
  }
  *o_event = m_events.front();
  m_events.pop_front();
  return true;
}

bool EventQueue::empty() const { return m_events.empty(); }

}  // namespace packetmux
