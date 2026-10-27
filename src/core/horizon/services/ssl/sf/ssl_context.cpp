#include "core/horizon/services/ssl/sf/ssl_context.hpp"

#include "core/horizon/os.hpp"
#include "core/horizon/services/ssl/sf/ssl_connection.hpp"
#include "core/system.hpp"

namespace hydra::horizon::services::ssl::sf {

DEFINE_SERVICE_COMMAND_TABLE(ISslContext, 0, setOption, 2, createConnection, 4,
                             importServerPki, 5, importClientPki, 8,
                             registerInternalPki)

result_t ISslContext::setOption(u32 option, i32 value) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "option: {}, value: {}", option,
                               value);
    return RESULT_SUCCESS;
}

result_t ISslContext::createConnection(RequestContext* ctx) {
    addService(*ctx,
               new ISslConnection(ctx->system.getOs().getHostSockets()));
    return RESULT_SUCCESS;
}

result_t ISslContext::importServerPki(CertificateFormat cert_format,
                                      InBuffer<BufferAttr::MapAlias> in_buffer,
                                      u64* out_id) {
    (void)in_buffer;
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "certificate format: {}", cert_format);

    // HACK
    *out_id = 0;
    return RESULT_SUCCESS;
}

result_t
ISslContext::importClientPki(InBuffer<BufferAttr::MapAlias> in_cert_buffer,
                             InBuffer<BufferAttr::MapAlias> in_password_buffer,
                             u64* out_id) {
    (void)in_cert_buffer;
    (void)in_password_buffer;
    LOG_FUNC_STUBBED(Services);

    // HACK
    *out_id = 0;
    return RESULT_SUCCESS;
}

result_t ISslContext::registerInternalPki(InternalPki pki, u64* out_id) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "PKI: {}", pki);

    // HACK
    *out_id = 0;
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::ssl::sf
