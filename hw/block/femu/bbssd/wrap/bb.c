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
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);
    int64_t cdw11 = le64_to_cpu(cmd->cdw11);
    int64_t cdw12 = le64_to_cpu(cmd->cdw12);

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
        long long unsigned int tmp_vpc = 0;
        struct line *line;
        struct line_mgmt *lm = &(n->ssd->lm);
        for (int i = 0; i < (n->ssd->sp).tt_lines; i++) {
            line = &lm->lines[i];
            tmp_vpc += line->vpc;
        }
        femu_log("Current device util:%.2f (valid pages:%llu)!\n", tmp_vpc*1.0/(n->ssd->sp).pgs_per_line/(n->ssd->sp).tt_lines,tmp_vpc);
        if (false) { // state transition
//                int ret = system("python3 FF.py");
                int ret = 1;
                if (ret != -1) {
                    FILE *fp= fopen("/media/tmp_sdc/femu/build-femu/endurance_log.txt", "r");
                    if (fp == NULL) {
                        ftl_log("Update EC: Endurance log file open error!\n");
                    } else {
                        int chs = (n->ssd->sp).nchs;
                        int luns = (n->ssd->sp).luns_per_ch;
                        int pls = (n->ssd->sp).pls_per_lun;
                        int blks = (n->ssd->sp).blks_per_pl;
                        int i,j,k,m = 0;
                        char * line = g_malloc0(sizeof(char) *chs*luns*pls*blks*10+1);
                        int record[chs*luns*pls*blks+1];
                        while (fgets(line, chs*luns*pls*blks*10+1, fp) != NULL) {
//                            printf("show:%c%c%c\n", line[0], line[1], line[2]);
                        }
//                        printf("end:%c%c%c\n", line[0], line[1], line[2]);
                        char *token;
                        token = strtok(line, " ");
                        int cnt = 0;
                        while (token != NULL && cnt < chs*luns*pls*blks) {
                            record[cnt] = atoi(token);
                            cnt++;
                            token = strtok(NULL, " ");
                        }
                        for ( i = 0; i < chs; i++) {
                            struct ssd_channel *c = &(n->ssd->ch[i]);
                            for ( j = 0; j < luns; j++) {
                                struct nand_lun *l = &(c->lun[j]);
                                for ( k = 0; k < pls; k++) {
                                    struct nand_plane *p = &(l->pl[k]);
                                    for ( m = 0; m < blks; m++) {
                                        struct nand_block *b = &(p->blk[m]);
                                        b->erase_cnt = record[b->id];
                                    }
                                }
                            }
                        }
                        ftl_log("Update EC: Completed!\n");
                        free(line);
                        fclose(fp);
                    }
                }
            }
//        femu_log("Rest OP = %d (sectors), WL = %d\n", (n->ssd->sp).op, (n->ssd->sp).wl); // %"PRIu64"
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

