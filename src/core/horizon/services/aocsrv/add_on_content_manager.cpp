#include "core/horizon/services/aocsrv/add_on_content_manager.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/system.hpp"

namespace hydra::horizon::services::aocsrv {

DEFINE_SERVICE_COMMAND_TABLE(IAddOnContentManager, 2, countAddOnContent, 3,
                             listAddOnContent, 5, getAddOnContentBaseId, 7,
                             prepareAddOnContent, 8,
                             getAddOnContentListChangedEvent, 50,
                             checkAddOnContentMountStatus)

result_t IAddOnContentManager::getAddOnContentListChangedEvent(
    kernel::Process* process, OutHandle<HandleAttr::Copy> out_handle) {
    // The DLC list is currently empty and static, so this event stays
    // unsignalled.
    out_handle = process->addHandle(list_changed_event);
    return RESULT_SUCCESS;
}

namespace {
std::vector<u32> indices(System& system, kernel::Process& process) {
    const u64 base = process.getTitleId() + 0x1000;
    std::vector<u32> result;
    for (const auto& [id, file] : system.getOs().getAddOnContent())
        if (id >= base && id - base <= 0xfff)
            result.push_back(u32(id - base));
    return result;
}
} // namespace

result_t IAddOnContentManager::countAddOnContent(System* system,
                                                 kernel::Process* process,
                                                 u64 process_id,
                                                 u32* out_count) {
    (void)process_id;
    *out_count = static_cast<u32>(indices(*system, *process).size());
    return RESULT_SUCCESS;
}

result_t IAddOnContentManager::listAddOnContent(
    System* system, kernel::Process* process, u32 start_index, u32 buffer_size,
    u64 process_id, u32* out_count,
    OutBuffer<BufferAttr::MapAlias> out_buffer) {
    (void)process_id;
    *out_count = 0;
    const auto ids = indices(*system, *process);
    if (start_index > ids.size() || (buffer_size && !out_buffer.stream))
        return MAKE_RESULT(Fs, filesystem::FsResult::DoesNotExist);
    const usize capacity =
        out_buffer.stream ? out_buffer.stream->getSize() / sizeof(u32) : 0;
    const usize count =
        std::min<usize>({buffer_size, capacity, ids.size() - start_index});
    if (count)
        out_buffer.stream->writeSpan(
            std::span(ids).subspan(start_index, count));
    *out_count = static_cast<u32>(count);
    return RESULT_SUCCESS;
}

result_t IAddOnContentManager::getAddOnContentBaseId(kernel::Process* process,
                                                     u64 process_id,
                                                     u64* out_id) {
    (void)process_id;
    *out_id = process->getTitleId() + 0x1000;
    return RESULT_SUCCESS;
}

result_t IAddOnContentManager::prepareAddOnContent(System* system,
                                                   kernel::Process* process,
                                                   u32 index, u64 process_id) {
    (void)process_id;
    if (index > 0xfff || !system->getOs().getAddOnContent().contains(
                             process->getTitleId() + 0x1000 + index))
        return MAKE_RESULT(Fs, filesystem::FsResult::DoesNotExist);
    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::aocsrv
