#pragma once

#include <deque>

#include "packetmux/SessionTypes.h"

namespace packetmux {

/**
 * The queue behind Session::pollEvent.
 */
class EventQueue {
 public:
  void push(const Event& i_event);
  bool pop(Event* const o_event);
  bool empty() const;

 private:
  std::deque<Event> m_events;
};

}  // namespace packetmux
