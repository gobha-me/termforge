#pragma once

// TermForge — private forwarding shim for the width helpers.
//
// The implementation now lives in the PUBLIC header
// termforge/widgets/detail/width.hpp so the public dropdown skeleton
// (detail/dropdown.hpp) can use truncate_to_width() without reaching into the
// library's PRIVATE include dir (#54). In-tree sources keep including
// "detail/width.hpp" exactly as before; this header just re-exports it.

#include "termforge/widgets/detail/width.hpp"
