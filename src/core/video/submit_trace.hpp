#pragma once

#include <span>
#include <string_view>

#include "core/horizon/services/nvdrv/ioctl/const.hpp"

namespace hydra::video {
void traceIoctl(std::string_view engine,
                horizon::services::nvdrv::ioctl::IoctlContext& context, u32 code);
// Opt-in, bounded diagnostic capture. Does not emulate execution or completion.
void traceSubmission(std::string_view engine,
                     horizon::services::nvdrv::ioctl::IoctlContext& context);
void traceEngineState(std::string_view engine,
                      horizon::services::nvdrv::ioctl::IoctlContext& context,
                      std::span<const u32> registers);
}
