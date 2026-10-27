#pragma once

// Minimal subset of Hydra's common precompiled header for standalone tests.
// Use the production range and integer types, without the app/Metal headers.
#include "ztd/range.hpp"
namespace hydra {
using ztd::u64;
using ztd::uptr;
using ztd::usize;
}
