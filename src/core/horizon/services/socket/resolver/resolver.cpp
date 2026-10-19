#include "core/horizon/services/socket/resolver/resolver.hpp"

namespace hydra::horizon::services::socket::resolver {

DEFINE_SERVICE_COMMAND_TABLE(IResolver, 6, getAddrInfoRequest, 12,
                             getAddrInfoRequestWithOptions)

// The socket backend is offline. Return a resolver failure rather than an
// empty successful response or an unknown IPC command. Command 12's output
// order follows Atmosphere's dns_mitm IResolver interface.
result_t IResolver::getAddrInfoRequest(u32* out_errno, i32* out_ret,
                                       u32* out_size) {
    *out_errno = 11; // nn::socket EAGAIN
    *out_ret = 2;    // EAI_AGAIN
    *out_size = 0;
    return RESULT_SUCCESS;
}

result_t IResolver::getAddrInfoRequestWithOptions(u32* out_size, i32* out_ret,
                                                  i32* out_host_error,
                                                  i32* out_errno) {
    *out_size = 0;
    *out_ret = 2;        // EAI_AGAIN
    *out_host_error = 2; // TRY_AGAIN
    *out_errno = 11;     // nn::socket EAGAIN
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::socket::resolver
