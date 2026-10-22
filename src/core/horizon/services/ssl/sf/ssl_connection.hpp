#pragma once

#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::socket {
class HostSockets;
}

namespace hydra::horizon::services::ssl::sf {

enum class IoMode : u32 {
    Blocking = 1,
    NonBlocking = 2,
};

// A TLS connection over one of the title's sockets. TLS itself is not
// implemented: on a socket connected to the online server's HTTP port the
// bytes pass through as they are (the server speaks plain HTTP there, as
// OCTR's sceHttp does with RyuNET); with any other host the handshake fails.
class ISslConnection : public IService {
  public:
    explicit ISslConnection(socket::HostSockets& sockets_)
        : sockets{sockets_} {}
    ~ISslConnection() override;

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    socket::HostSockets& sockets;
    i32 fd{-1};
    std::string host_name;
    u32 verify_option{0};
    IoMode io_mode{IoMode::Blocking};
    u32 session_cache_mode{0};
    u32 renegotiation_mode{0};
    bool do_not_close_socket{false};
    bool get_server_cert_chain{false};
    bool handshake_done{false};

    result_t passThroughHandshake();

    // Commands
    result_t setSocketDescriptor(i32 in_fd, i32* out_fd);
    result_t setHostName(InBuffer<BufferAttr::MapAlias> buffer);
    result_t setVerifyOption(u32 option);
    result_t setIoMode(IoMode mode);
    result_t getSocketDescriptor(i32* out_fd);
    result_t getHostName(u32* out_size,
                         OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t getVerifyOption(u32* out_option);
    result_t getIoMode(IoMode* out_mode);
    result_t doHandshake();
    result_t doHandshakeGetServerCert(u32* out_size, u32* out_count,
                                      OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t read(u32* out_size, OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t write(InBuffer<BufferAttr::MapAlias> buffer, u32* out_size);
    result_t pending(i32* out_size);
    result_t peek(u32* out_size, OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t poll(u32 events, u32 timeout_ms, u32* out_events);
    STUB_REQUEST_COMMAND(getVerifyCertError);
    result_t getNeededServerCertBufferSize(u32* out_size);
    result_t setSessionCacheMode(u32 mode);
    result_t getSessionCacheMode(u32* out_mode);
    STUB_REQUEST_COMMAND(flushSessionCache);
    result_t setRenegotiationMode(u32 mode);
    result_t getRenegotiationMode(u32* out_mode);
    result_t setOption(u32 word0, u32 word1);
    result_t getOption(u32 option, bool* out_value);
};

} // namespace hydra::horizon::services::ssl::sf
