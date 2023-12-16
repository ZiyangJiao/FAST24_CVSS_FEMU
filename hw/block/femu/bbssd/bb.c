#include "../nvme.h"
#include "./ftl.h"

static void bb_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vbb = 0;
    const char *vbbssd_mn = "FEMU BlackBox-SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

/* bb <=> black-box */
static void bb_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    bb_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in Blackbox-SSD mode ...\n");
    ssd_init(n);
}

static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    char buffer[64] = "";
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);
    int64_t cdw11 = le64_to_cpu(cmd->cdw11);
    int64_t cdw12 = le64_to_cpu(cmd->cdw12);
    int64_t cdw13 = le64_to_cpu(cmd->cdw13);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_STATE:
        (n->ssd->sp).wl = (int)cdw11; // 0 - CV enabled; 1 - conventional SSD mode
        (n->ssd->sp).acceleration = (int)cdw12; // acceleration factor control for FF-SSD
        if ((int)cdw13 > 0) {
            (n->ssd->sp).retired_ec = (int)cdw13; // set the host requirement for mapping-out threshold
            printf("FEMU:%s,The block retirement threshold is updated to (%lu)\n", n->devname, cdw13);
        }
        struct line_mgmt *lm = &(n->ssd->lm);
        struct ssdparams *spp = &(n->ssd->sp);
        int64_t bad = lm->bad_line_cnt*spp->blks_per_line/(1024*1024*1024/spp->secsz/spp->secs_per_blk);
        sprintf(buffer, "{'BadGib': %"PRIu64"}", bad);
        dma_read_prp(n, (uint8_t *)buffer, sizeof(buffer), prp1, prp2);
        break;
    case FEMU_FAST_AGING:
        if ((int)cdw11 == 1) {
            (n->ssd->sp).pg_rd_lat = 1;
            (n->ssd->sp).pg_wr_lat = 1;
            (n->ssd->sp).blk_er_lat = 1;
        } else {
            (n->ssd->sp).pg_rd_lat = NAND_READ_LATENCY;
            (n->ssd->sp).pg_wr_lat = NAND_PROG_LATENCY;
            (n->ssd->sp).blk_er_lat = NAND_ERASE_LATENCY;
        }
        break;
    case FEMU_SET_DEGRADE:
        if ((int)cdw11 == 1) {
            n->ssd->cv_moderate = 1;
            printf("FEMU:%s,CV_degraded mode is set.\n", n->devname);
        } else {
            n->ssd->cv_moderate = 0;
            printf("FEMU:%s,CV_degraded mode is unset.\n", n->devname);
        }
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

static uint16_t bb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
    case NVME_CMD_REMAP:
        return bb_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t bb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(n, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

int nvme_register_bbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = bb_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = bb_admin_cmd,
        .io_cmd           = bb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}


