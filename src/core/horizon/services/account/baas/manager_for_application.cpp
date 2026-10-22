#include "core/horizon/services/account/baas/manager_for_application.hpp"

#include <algorithm>

#include "core/horizon/kernel/process.hpp"
#include "core/horizon/online/online.hpp"
#include "core/horizon/services/account/async_context.hpp"

namespace hydra::horizon::services::account::baas {

DEFINE_SERVICE_COMMAND_TABLE(IManagerForApplication, 0, checkAvailability, 1,
                             getAccountId, 2, ensureIdTokenCacheAsync, 3,
                             loadIdTokenCache)

result_t IManagerForApplication::getAccountId(kernel::Process* process,
                                              u64* out_id) {
    if (const auto* config = online::getConfig(process->getTitleId())) {
        *out_id = online::getAccountId(*config);
        return RESULT_SUCCESS;
    }

    LOG_FUNC_STUBBED(Services);

    // HACK
    *out_id = "net_id"_u64;
    return RESULT_SUCCESS;
}

// Online, the ID token is the nickname: OCTR gives RyuNET the nickname as the
// PSN ID token. Offline, there is no token (as before, an error).
result_t IManagerForApplication::ensureIdTokenCacheAsync(RequestContext* ctx) {
    if (!online::getConfig(ctx->process->getTitleId())) {
        LOG_WARN(Services, "No ID token offline");
        return MAKE_RESULT(Svc, 0);
    }

    addService(*ctx, new IAsyncContext());
    return RESULT_SUCCESS;
}

result_t IManagerForApplication::loadIdTokenCache(
    kernel::Process* process, u32* out_size,
    OutBuffer<BufferAttr::MapAlias> out_buffer) {
    *out_size = 0;
    const auto* config = online::getConfig(process->getTitleId());
    if (!config) {
        LOG_WARN(Services, "No ID token offline");
        return MAKE_RESULT(Svc, 0);
    }

    const auto& token = config->nickname;
    if (!out_buffer.stream ||
        out_buffer.stream->getSize() < token.size() + 1)
        return MAKE_RESULT(Account, 0);
    auto* data = out_buffer.stream->getPtr();
    std::copy(token.begin(), token.end(), data);
    data[token.size()] = 0;
    *out_size = static_cast<u32>(token.size());

    LOG_INFO(Services, "Online: ID token \"{}\" loaded", token);
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::account::baas
