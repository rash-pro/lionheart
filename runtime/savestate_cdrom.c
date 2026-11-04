#include <stddef.h>
#if defined(FF8_SAVESTATES)
#include "savestate_internal.h"
void ss_cdrom(ss_io *io, hw_cd_t *d) {
    unsigned i;
    uint32_t sectors = d->sectors, offset = d->offset;
    ss_version(io, 1);
    S32(d->sectors); S32(d->offset); S32(d->img_lba); S32(d->img_count);
    for (i = 0; i < 256; ++i) { S32(d->img[i].lba); S32(d->img[i].size); }
    S8(d->index); S8(d->params); S8(d->response); S8(d->command_params);
    S32(d->param_count); S32(d->response_pos); S32(d->response_count); S32(d->command_count);
    S8(d->command); S8(d->second); S8(d->irq_flags); S8(d->irq_enable); S8(d->mode); S8(d->status);
    S8(d->filter_file); S8(d->filter_channel); S8(d->muted);
    S8(d->sector); S8(d->data); S8(d->header);
    S32(d->data_pos); S32(d->data_size); S32(d->sector_size); S32(d->sector_valid); S32(d->request);
    S32(d->reading); S32(d->playing); S32(d->location_pending); S32(d->location); S32(d->read_lba);
    S32(d->sector_lba); S32(d->data_lba); S64(d->command_due); S64(d->second_due); S64(d->read_due);
    S64(d->commands); S64(d->interrupts); S64(d->sectors_read); S64(d->bytes_dma); S64(d->overruns);
    S8(d->volume); S8(d->pending_volume); S8(d->xa_muted); S8(d->xa_file); S8(d->xa_channel); S8(d->xa_active);
    S16(d->xa_history); S16(d->xa_ring); S32(d->xa_ring_pos); S32(d->xa_sixstep);
    S64(d->xa_sectors); S64(d->xa_frames); S64(d->xa_filtered);
    S8(d->sector_buffer); S32(d->buffer_lba); S32(d->shell_open); S32(d->lid_irq); S64(d->lid_due);
    if ((io->reading && (d->sectors != sectors || d->offset != offset)) || d->img_count > 256 ||
        d->index > 3 || d->param_count > 16 || d->response_pos > 16 || d->response_count > 16 ||
        d->command_count > 16 || d->data_pos > 2352 || d->data_size > 2352 || d->sector_size > 2352 ||
        d->irq_flags > 5 || d->xa_ring_pos >= 32 || d->xa_sixstep > 6) io->failed = 1;
    /* All sector reads seek explicitly (cdrom.c:read_sector); FILE position and
     * optional per-LBA diagnostic allocations are not guest state. */
}
#endif
