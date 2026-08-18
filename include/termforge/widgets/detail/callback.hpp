#pragma once

// TermForge — callback invocation helpers.
//
// Widget callbacks (on_activate, on_change, on_select, menu item actions, …)
// are allowed to do anything, including mutating the widget that fired them:
// reassigning the very std::function being run, or destroying the object that
// owns it. Invoking such a callback through a reference into the mutated
// object is a use-after-free — the bug class behind issues #5 and #32, and
// the reason every widget used to hand-roll
//
//   auto cb = m_on_x;
//   if (cb) cb(args...);
//
// at ~19 sites across 10 files. Hand-rolling relies on the next widget author
// remembering the idiom; this header makes the safe form the only form.
//
//   detail::invoke_copy(m_on_activate);
//   detail::invoke_copy(m_on_change, index, text);
//
// The slot is COPIED out of the object before it runs, so a callback that
// reassigns or clears the member (or frees its owner) cannot invalidate the
// call in flight. A null slot is a no-op, matching the old `if (cb)` guard.

#include <functional>
#include <utility>

namespace termforge::detail {

// Deduces the slot's declared signature from the std::function type, then
// converts the arguments to it. The deduction has to come from the slot (not
// the call arguments) because a signature like void(int, const std::string&)
// cannot be deduced from by-value template parameters — an rvalue std::string
// would deduce Args = std::string, not const std::string&.
//
// Void-only on purpose (#56 item 5): every widget callback in the codebase
// returns void (the one non-void callback, FilePickerDialog's error-overlay
// query, is invoked directly, not through here). A non-void R would force a
// what-to-return-on-null design question no caller asks; add it back when a
// real caller does.
template <typename... Args, typename... CallArgs>
auto invoke_copy(const std::function<void(Args...)>& slot, CallArgs&&... args)
    -> void {
  // Decay-copied: any lambda capture by reference still refers to whatever
  // the app captured; only the std::function holder itself is detached from
  // the widget.
  auto cb = slot;
  if (cb) cb(std::forward<CallArgs>(args)...);
}

} // namespace termforge::detail
