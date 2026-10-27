#include "core/hot/table.hpp"

#include <cstdint>
#include <cstring>

#ifdef HYDRA_HOT_ABI_HEADER
#include "hot_abi.hpp"
#else
#define HYDRA_HOT_ABI ""
#endif

namespace hydra::hot {

const char* getAbi() { return HYDRA_HOT_ABI; }

void fillTable(Table& table) {
#define HYDRA_HOT_FILL(name, function) table.name = function;
    HYDRA_HOT_FUNCTIONS(HYDRA_HOT_FILL)
#undef HYDRA_HOT_FILL
#define HYDRA_HOT_ROUTE(name, method) table.name = method;
#include "core/hot/routes.inc"
#undef HYDRA_HOT_ROUTE
}

} // namespace hydra::hot

// Entry point of the hydra-hot module, also present in the executable
extern "C" __attribute__((visibility("default"))) bool
hydra_hot_register(hydra::hot::Table* table, std::uint64_t size,
                   const char* abi) {
    // Built against other headers, class layouts can differ
    if (size != sizeof(hydra::hot::Table) || std::strcmp(abi, HYDRA_HOT_ABI) != 0)
        return false;
    hydra::hot::fillTable(*table);
    return true;
}
