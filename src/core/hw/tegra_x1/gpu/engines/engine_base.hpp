#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"

#define METHOD_CASE(method_begin, method_count, func, arg_type)                \
    case method_begin ...(method_begin + method_count - 1):                    \
        func(method - method_begin, std::bit_cast<arg_type>(arg));             \
        break;

#define DEFINE_METHOD_TABLE(type, ...)                                         \
    void type::methodHot(u32 method, u32 arg) {                                   \
        if (method >= MACRO_METHODS_REGION) {                                  \
            macro(method, arg);                                                \
            return;                                                            \
        }                                                                      \
        /* Macros read the registers of command methods too */              \
        writeReg(method, arg);                                                 \
        switch (method) {                                                      \
            ZTD_FOR_EACH_0_4(METHOD_CASE, __VA_ARGS__)                         \
        default:                                                               \
            break;                                                             \
        }                                                                      \
    }

namespace hydra::hw::tegra_x1::gpu {
class GMmu;
}

namespace hydra::hw::tegra_x1::gpu::engines {

class EngineBase {
  public:
    virtual ~EngineBase() = default;

    virtual void method(u32 method, u32 arg) = 0;

    virtual void flushMacro() {
        LOG_FATAL(Engines, "This engine does not support macros");
    }

  protected:
    virtual void macro(u32 method, u32 arg) {
        LOG_FATAL(Engines,
                  "This engine does not support macros (method: 0x{:08x}, arg: "
                  "0x{:08x})",
                  method, arg);
    }
};

template <typename RegsT>
class EngineWithRegsBase : public EngineBase {
  public:
#define REG_COUNT (sizeof(RegsT) / sizeof(u32))

    u32 getReg(u32 reg) const {
        if (reg >= REG_COUNT)
            return 0;
        return regs_raw[reg];
    }

  protected:
    union {
        RegsT regs{};
        u32 regs_raw[REG_COUNT];
    };

    void writeReg(u32 reg, u32 value) {
        // Methods past the emulated register file are dropped: the engines
        // are adjacent in memory, writing past the array corrupts the next
        if (reg >= REG_COUNT) {
            ONCE(LOG_WARN(Engines,
                          "Ignoring write to unemulated register 0x{:03x} "
                          "(value: 0x{:08x})",
                          reg, value));
            return;
        }
        LOG_DEBUG(Engines, "Writing to reg 0x{:03x} (value: 0x{:08x})", reg,
                  value);
        regs_raw[reg] = value;
    }

#undef REG_COUNT
};

} // namespace hydra::hw::tegra_x1::gpu::engines
