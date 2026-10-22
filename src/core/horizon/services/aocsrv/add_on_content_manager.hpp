#pragma once

#include "core/horizon/kernel/event.hpp"
#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::aocsrv {

class IAddOnContentManager : public IService {
  public:
    IAddOnContentManager()
        : list_changed_event{
              new kernel::Event(false, "Add-on content list changed event")} {}
    ~IAddOnContentManager() override { list_changed_event->release(); }

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    kernel::Event* list_changed_event;

    // Commands
    result_t countAddOnContent(System* system, kernel::Process* process,
                               u64 process_id, u32* out_count);
    result_t listAddOnContent(System* system, kernel::Process* process,
                              u32 start_index, u32 buffer_size, u64 process_id,
                              u32* out_count,
                              OutBuffer<BufferAttr::MapAlias> out_buffer);
    result_t getAddOnContentBaseId(kernel::Process* process, u64 process_id,
                                   u64* out_id);
    result_t prepareAddOnContent(System* system, kernel::Process* process,
                                 u32 index, u64 process_id);
    result_t
    getAddOnContentListChangedEvent(kernel::Process* process,
                                    OutHandle<HandleAttr::Copy> out_handle);
    STUB_REQUEST_COMMAND(checkAddOnContentMountStatus);
};

} // namespace hydra::horizon::services::aocsrv
