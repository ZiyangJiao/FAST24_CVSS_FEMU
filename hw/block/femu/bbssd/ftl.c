#include "ftl.h"

// #define FEMU_MYDEBUG_FTL
#ifdef FEMU_MYDEBUG_FTL
FILE * femu_log_file;
#define write_log(fmt, ...) \
    do { if (femu_log_file) fprintf(femu_log_file, fmt, ## __VA_ARGS__); fflush(NULL);} while (0)
#else
#define write_log(fmt, ...) \
    do { } while (0)
#endif

static void *ftl_thread(void *arg);

// static bool is_write_heavy(struct ssd *ssd, uint64_t lpn)
// {
//     // write_heavy = ssd->rwtbl[lpn] || (ssd->lpnwtbl[lpn] > ssd->writes/(ssd->sp).tt_pgs);
//     if (((&ssd->sp)->wl) || (ssd->cv_moderate)) { // even blocks wear
//         return true;
//     }
//     return (ssd->rwtbl[lpn]);
// }

static inline bool should_gc(struct ssd *ssd)
{
    if (ssd->lm.free_line_cnt >= ssd->sp.gc_thres_lines)
        return false;

    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line)
        return false;
    if ((&ssd->sp)->wl) // Tr-SSD
        return ((victim_line->ipc) >= (ssd->sp.pgs_per_line / 8));
    return ((victim_line->vpc) == 0); // CV-SSD
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa);
static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
                                     victim_line_get_pri, victim_line_set_pri,
                                     victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->bad_line_list);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->bad_line_cnt = 0;
    lm->full_line_cnt = 0;

    /* precondition with #i of data lines*/
    /*
    for (int i = 0; i < 400; i++) {
        line = QTAILQ_FIRST(&lm->free_line_list);
        QTAILQ_REMOVE(&lm->free_line_list, line, entry);
        lm->free_line_cnt--;
//        line->vpc = spp->pgs_per_line;
//        QTAILQ_INSERT_TAIL(&lm->full_line_list, line, entry);
        QTAILQ_INSERT_TAIL(&lm->bad_line_list, line, entry);
//        lm->full_line_cnt++;
        lm->bad_line_cnt++;

//        struct ppa ppa;
//        struct nand_block *block_cold;
//        ppa.g.blk = line->id;
//        for (int ch = 0; ch < spp->nchs; ch++) {
//            for (int lun = 0; lun < spp->luns_per_ch; lun++) {
//                ppa.g.ch = ch;
//                ppa.g.lun = lun;
//                ppa.g.pl = 0;
//                block_cold = get_blk(ssd, &ppa);
//                block_cold->vpc = spp->pgs_per_blk;
//            }
//        }
    }
     */
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
    ftl_log("Init write line %d for write!\n", wpp->blk);


    /* initiate write frontier for read dominant data */
    // wpp = &ssd->wp2;

    // curline = QTAILQ_FIRST(&lm->free_line_list);
    // QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    // lm->free_line_cnt--;

    // wpp->curline = curline;
    // wpp->ch = 0;
    // wpp->lun = 0;
    // wpp->pg = 0;
    // wpp->blk = curline->id;
    // wpp->pl = 0;
    // ftl_log("Init write line %d for read!\n", wpp->blk);

}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;
    struct line *tmpline = NULL;
    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    /* CV block allocation policy */
    if ((ssd->sp).wl == 0) {
//        ftl_log("CV Block allocation\n");
        struct nand_block *block_tmp = NULL;
        int hottest = 0;
        struct ppa ppa;
        ppa.g.ch = 0;
        ppa.g.lun = 0;
        ppa.g.pl = 0;
        ppa.g.blk = curline->id;
        block_tmp = get_blk(ssd, &ppa);
        hottest = block_tmp->erase_cnt;
        tmpline = curline;
        if (ssd->cv_moderate == 0) { // oldest block first
            for (int i = 1; i < lm->free_line_cnt; i++) {
                tmpline = QTAILQ_NEXT(tmpline, entry);
                ppa.g.blk = tmpline->id;
                block_tmp = get_blk(ssd, &ppa);
                if (block_tmp->erase_cnt > hottest) {
                    hottest = block_tmp->erase_cnt;
                    curline = tmpline;
                } else if (block_tmp->erase_cnt == hottest && tmpline->id < curline->id) {
                    curline = tmpline;
                }
            }
        } else { // youngest block first
            for (int i = 1; i < lm->free_line_cnt; i++) {
                tmpline = QTAILQ_NEXT(tmpline, entry);
                ppa.g.blk = tmpline->id;
                block_tmp = get_blk(ssd, &ppa);
                if (block_tmp->erase_cnt < hottest) {
                    hottest = block_tmp->erase_cnt;
                    curline = tmpline;
                }
            }
        }
    }


    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    lm->count_line_for_write++;
    return curline;
}


static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}


static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);
//    ppa.write_heavy = true;

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
	spp->secsz = 512;
    spp->secs_per_pg = 8; /* Page = 4KiB*/
    // spp->pgs_per_blk = 256; /* BLK = 1MiB */
    // spp->pgs_per_blk = 256*4*2; /* BLK = 8MiB */
    spp->pgs_per_blk = 256*4; /* BLK = 4MiB */
    spp->blks_per_pl = 256*2; /* 128GiB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    /* 256 lines, 512MiB (1048576 sectors) per line */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

//    spp->gc_thres_pcent = 0.85;
    spp->gc_thres_pcent = 0.95;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
//    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->gc_thres_lines_high = 20;
    spp->enable_gc_delay = true;

    spp->ecc_corr_str = 50; /* ECC correction strength (bits) */
    spp->epsilon = 0.00148;
    spp->alpha = 0.000000516375983;
    spp->k = 2.05;
    spp->gamma = 0.00000000651773564;
    spp->p = 0.435025976;
    spp->q = 1.71;

    spp->endurance = 300;
//    spp->op = 2097152; /* 1 GiB = 2097152 sectors (sector size = 512 bytes) */
    spp->op = 18436096; /* 128GiB physical capacity, 128GB(120GiB) logical capacity, 7.37% OP, 9002MiB = 18436096 sectors*/
    spp->id = 0;
    spp->wl = 1;
    spp->total_ec = 0;
    spp->acceleration = 2;
    spp->read_retry = 0;
    spp->pages_from_host = 0;
    spp->pages_from_gc = 0;
    spp->pages_from_wl = 0;
    spp->retired_ec = 290;

    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->read_cnt = 0;
    blk->wp = 0;
    blk->id = spp->id;
    spp->id++;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

// static void ssd_init_rwtbl(struct ssd *ssd)
// {
//     ssd->reads = 0;
//     ssd->writes = 0;
//     struct ssdparams *spp = &ssd->sp;

//     ssd->rwtbl = g_malloc0(sizeof(bool) * spp->tt_pgs);
//     for (int i = 0; i < spp->tt_pgs; i++) {
//         ssd->rwtbl[i] = true;
//     }

//     ssd->lpnrtbl = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
//     for (int i = 0; i < spp->tt_pgs; i++) {
//         ssd->lpnrtbl[i] = 0;
//     }

//     ssd->lpnwtbl = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
//     for (int i = 0; i < spp->tt_pgs; i++) {
//         ssd->lpnwtbl[i] = 0;
//     }
// }

void ssd_init(FemuCtrl *n)
{
#ifdef FEMU_MYDEBUG_FTL
    char str[80];
    sprintf(str, "/mnt/tmp_sdc/femu/build-femu/femu_debug.log");
    femu_log_file = fopen(str, "w+");
#endif

    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    // ssd_init_rwtbl(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
                                                                           0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
                                                                                                                                               >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static inline bool swap_is_benefit(int ec_from, int ec_to, struct ssdparams* spp)
{
    // int read_retry_from = 0;
    // int read_retry_to = 0;
    // int bits_count = spp->secs_per_pg * spp->secsz * 8;
    // double rber_from = spp->epsilon + spp->alpha*pow(ec_from,spp->k);
    // double rber_to = spp->epsilon + spp->alpha*pow(ec_to,spp->k);
    // rber_from = rber_from > 1.0 ? 1.0 : rber_from;
    // rber_to = rber_to > 1.0 ? 1.0 : rber_to;

    // while ((int)(bits_count * rber_from) > spp->ecc_corr_str) {
    //     rber_from /= 2.0;
    //     read_retry_from += 1;
    // }

    // while ((int)(bits_count * rber_to) > spp->ecc_corr_str) {
    //     rber_to /= 2.0;
    //     read_retry_to += 1;
    // }
    int read_retry_from = ec_from/50;
    int read_retry_to = (ec_to + 1)/50;
    return read_retry_from > read_retry_to;
}

static inline int get_read_retry_cnt(struct nand_block *blk, struct ssdparams *spp) {
    //precomuted results based on formula for lower emulateion overhead
    int read_retry = blk->erase_cnt/50;
    if (blk->erase_cnt > spp->retired_ec) {
        read_retry += 1;
    }
    return read_retry;
}

static uint64_t ssd_gc_read(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    // return 0;
    if ((ssd->lm.lines[ppa->g.blk]).read_cnt < 1000) {
        return 0;
    }
    // printf("ssd_gc_read: enter\n");
    int c = ncmd->cmd;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    
    // read-heavy data check
    int avg_ec = (int)(spp->total_ec/spp->tt_blks);
    // printf("ssd_gc_read: ready\n");
    // check if target is an aged (super)block
    if (blk->erase_cnt > avg_ec && c == NAND_READ && ncmd->type == USER_IO && spp->wl != 1 && ssd->cv_moderate == 0) {
    // if (false) {
        // printf("READ0: is aged\n");
        struct line_mgmt *lm = &ssd->lm;
        struct ppa ppa_tmp;
        uint64_t line_cur_read = 0, line_read = 0, line_max_read = 0;
        int line_read_min_id = -1;
        int line_read_min_ec = -1;
        int i, ch, lun;
        struct nand_block *blk_tmp = NULL;
        uint64_t line_avg_read = spp->total_host_read/(spp->tt_lines - lm->bad_line_cnt);
        // printf("READ0: try to find candidate\n");
        for (i = 0; i < lm->tt_lines; i++) { // calculate avg_read and find a candidate to relocate
            ppa_tmp.g.blk = i;
            ppa_tmp.g.pl = 0;
            ppa_tmp.g.ch = 0;
            ppa_tmp.g.lun = 0;
            struct line *line = &lm->lines[i];
            blk_tmp = get_blk(ssd, &ppa_tmp);
            line_read = line->read_cnt;
            if (line_read > line_max_read) {
            	line_max_read = line_read;
            }
            // printf("READ0: get line read\n");
            // for (ch = 0; ch < spp->nchs; ch++) {
            //     ppa_tmp.g.ch = ch;
            //     for (lun = 0; lun < spp->luns_per_ch; lun++) {
            //         ppa_tmp.g.lun = lun;
            //         blk_tmp = get_blk(ssd, &ppa_tmp);
            //         line_read += blk_tmp->read_cnt;
            //     }
            // }
            if (line->vpc == 0) {
                continue;
            }
            // printf("READ0: 1\n");
            if (line->id == ppa->g.blk) {
                line_cur_read = line->read_cnt;
                continue;
            }
            // printf("READ0: 2\n");
            if (line == (&ssd->wp)->curline) {
                continue;
            }
            // printf("READ0: 3\n");
            if (blk_tmp->erase_cnt < avg_ec && line_read < 0.3*line_avg_read) { // blk->erase_cnt < blk_tmp->erase_cnt due to blk->erase_cnt > avg_ec
                // printf("READ0: 4\n");
                if (line_read_min_id == -1 || blk_tmp->erase_cnt < line_read_min_ec) {
                    // printf("READ5: 5\n");
                    line_read_min_id = line->id;
                    line_read_min_ec = blk_tmp->erase_cnt;
                }
                // printf("READ0: 6\n");
            }
            // printf("READ0: one line is checked\n");
        }
        // printf("READ1: line_cur_read = %ld, line_avg_read %ld, line_read_min_ec %d\n", line_cur_read, line_avg_read, line_read_min_ec);

        // check if target is a read-heavy (super)block
        if (line_cur_read > line_avg_read && line_cur_read > 0.3*line_max_read && line_read_min_id != -1) {
            //check benefit
            // printf("READ2: check benefit\n");
            if (swap_is_benefit(blk->erase_cnt, line_read_min_ec, spp)) {
                // printf("READ3: check benefit pass\n");
                //swap with candidate
                ppa_tmp.g.pl = 0;
                for (ch = 0; ch < spp->nchs; ch++) {
                    ppa_tmp.g.ch = ch;
                    for (lun = 0; lun < spp->luns_per_ch; lun++) {
                        ppa_tmp.g.lun = lun;
                        ppa_tmp.g.blk = line_read_min_id;
                        struct nand_block *blk_target = get_blk(ssd, &ppa_tmp);
                        ppa_tmp.g.blk = ppa->g.blk;
                        struct nand_block *blk_cur = get_blk(ssd, &ppa_tmp);
                        blk_target->erase_cnt = blk_cur->erase_cnt + 1;
                        blk_cur->erase_cnt = line_read_min_ec;
                        spp->pages_from_wl += blk_cur->vpc;
                        // spp->pages_from_wl += blk_target->vpc;
                        spp->total_ec += 1;
                    }
                }
            }
            
        }
        // printf("READ4: done\n");
    }

    return 0;
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    struct nand_block *blk = get_blk(ssd, ppa);
    struct line *line = get_line(ssd, ppa);
    uint64_t lat = 0;
    // uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    
    // read-heavy data check
    int avg_ec = (int)(spp->total_ec/spp->tt_blks);
    // check if target is an aged (super)block
    // if (c == NAND_READ && ncmd->type == USER_IO && spp->wl != 1 && ssd->cv_moderate == 0 && blk->erase_cnt > avg_ec) {
    if (false) {
        struct line_mgmt *lm = &ssd->lm;
        struct ppa ppa_tmp;
        uint64_t line_cur_read = 0, line_read = 0;
        int line_read_min_id = -1;
        int line_read_min_ec = -1;
        int i, ch, lun;
        struct nand_block *blk_tmp = NULL;
        uint64_t line_avg_read = spp->total_host_read/(spp->tt_lines - lm->bad_line_cnt);
        for (i = 0; i < lm->tt_lines; i++) { // calculate avg_read and find a candidate to relocate
            ppa_tmp.g.blk = i;
            ppa_tmp.g.pl = 0;
            line_read = 0;
            for (ch = 0; ch < spp->nchs; ch++) {
                ppa_tmp.g.ch = ch;
                for (lun = 0; lun < spp->luns_per_ch; lun++) {
                    ppa_tmp.g.lun = lun;
                    blk_tmp = get_blk(ssd, &ppa_tmp);
                    line_read += blk_tmp->read_cnt;
                }
            }
            if (i == ppa->g.blk) {
                line_cur_read = line_read;
                continue;
            }
            if (i == (&ssd->wp)->curline->id) {
                continue;
            }
            if (blk_tmp->erase_cnt < avg_ec && line_read < line_avg_read) { // blk->erase_cnt < blk_tmp->erase_cnt due to blk->erase_cnt > avg_ec
                if (line_read_min_id == -1 || blk_tmp->erase_cnt < line_read_min_ec) {
                    line_read_min_id = i;
                    line_read_min_ec = blk_tmp->erase_cnt;
                }
            }
        }
        // printf("READ1: line_cur_read = %d, line_avg_read %d, line_read_min_ec %d\n", line_cur_read, line_avg_read, line_read_min_ec);

        // check if target is a read-heavy (super)block
        if (line_cur_read > line_avg_read && line_read_min_id != -1) {
            //check benefit
            // printf("READ2: check benefit\n");
            if (swap_is_benefit(blk->erase_cnt, line_read_min_ec, spp)) {
                // printf("READ2: check benefit pass\n");
                //swap with candidate
                ppa_tmp.g.pl = 0;
                for (ch = 0; ch < spp->nchs; ch++) {
                    ppa_tmp.g.ch = ch;
                    for (lun = 0; lun < spp->luns_per_ch; lun++) {
                        ppa_tmp.g.lun = lun;
                        ppa_tmp.g.blk = line_read_min_id;
                        struct nand_block *blk_target = get_blk(ssd, &ppa_tmp);
                        ppa_tmp.g.blk = ppa->g.blk;
                        struct nand_block *blk_cur = get_blk(ssd, &ppa_tmp);
                        blk_target->erase_cnt = blk_cur->erase_cnt + 1;
                        blk_cur->erase_cnt = line_read_min_ec;
                        spp->pages_from_wl += blk_cur->vpc;
                        // spp->pages_from_wl += blk_target->vpc;
                        spp->total_ec += 1;
                    }
                }
            }
            
        }
    }
    uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    switch (c) {
        case NAND_READ:
            /* read: perform NAND cmd first */
            if (lun->next_lun_avail_time < cmd_stime) {
                nand_stime = cmd_stime;
            } else {
                nand_stime = lun->next_lun_avail_time;
                spp->host_read_block++;
            }
            // double ec = (double)blk->erase_cnt;
            // double rber = spp->epsilon + spp->alpha*pow(ec,spp->k);
            // rber = rber > 1.0 ? 1.0 : rber;
            // int bits_count = spp->secs_per_pg * spp->secsz * 8;

            // int read_retry = 0;
            // while ((int)(bits_count * rber) > spp->ecc_corr_str) {
            //     rber /= 2.0;
            //     read_retry += 1;
            // }

            int read_retry = get_read_retry_cnt(blk, spp);
            spp->read_retry += read_retry;

            if (ncmd->type == USER_IO) {
                blk->read_cnt += 1; // record # of host read
                spp->total_host_read += 1; // record total # of host read
                line->read_cnt += 1;
            }

            // if (ncmd->type == GC_IO && spp->wl == 1) { // emulate ttFlash performance;
            //     lun->next_lun_avail_time = nand_stime; 
            // }

            lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat * (1 + read_retry);
            lat = lun->next_lun_avail_time - cmd_stime;
            
#if 0
            lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
            break;

        case NAND_WRITE:
            /* write: transfer data through channel first */
            if (lun->next_lun_avail_time < cmd_stime) {
                nand_stime = cmd_stime;
            } else {
                nand_stime = lun->next_lun_avail_time;
                spp->host_write_block++;
            }
            // float percentage_used = (float)blk->erase_cnt/(float)spp->endurance;
            // this piece of code is used for WRAP work, not for CVSS
            /*
            double ratio = 0.9865636536464101 + (-1.5470682790191133) * percentage_used + \
                 2.4621867081807056 * pow(percentage_used,2) + \
                 (-1.1217434639021386) * pow(percentage_used,3);
            lun->next_lun_avail_time = nand_stime + (int)spp->pg_wr_lat*ratio;
            */

			
            // if (ncmd->type == GC_IO && spp->wl ==1) { // emulate ttFlash performance;
            //     return 0; 
            // }else{
            //     lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
            // }
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
            lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
            ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
            break;

        case NAND_ERASE:
            /* erase: only need to advance NAND status */
            nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
            lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

            lat = lun->next_lun_avail_time - cmd_stime;
            break;

        default:
            ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;
    struct write_pointer *wpp = &ssd->wp;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    if ((wpp->curline != line) && (!was_full_line)) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;
    struct line *line = get_line(ssd, ppa);

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
//    blk->erase_cnt += 1;
    blk->erase_cnt += spp->acceleration; // accelerate aging
    spp->total_ec += spp->acceleration;
    spp->total_host_read -= blk->read_cnt;
    line->read_cnt -= blk->read_cnt;
    blk->read_cnt = 0;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    // bool write_heavy = is_write_heavy(ssd, lpn);

    ftl_assert(valid_lpn(ssd, lpn));

    // if (((&ssd->sp)->wl) || (ssd->cv_moderate)) {
    //     new_ppa = get_new_page(ssd);
    // } else {
    //     new_ppa = get_new_page_for_read(ssd); // GC-ed data are considered as read-dominant
    // }
    new_ppa = get_new_page(ssd);
    
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    set_rmap_ent(ssd, INVALID_LPN, old_ppa);

    /* need to advance the write pointer here */
    // if (!write_heavy)
    //     ssd_advance_write_pointer_for_read(ssd);
    // else
    //     ssd_advance_write_pointer(ssd);
    // if (((&ssd->sp)->wl) || (ssd->cv_moderate)) {
    //     ssd_advance_write_pointer(ssd);
    // } else {
    //     ssd_advance_write_pointer_for_read(ssd);
    //     ssd->rwtbl[lpn] = false; // has relocated for read
    // }
    ssd_advance_write_pointer(ssd);
    
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}


static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    int total_vpc = lm->full_line_cnt * spp->pgs_per_line;
    // double line_score = -1.0;
    // double line_score_tmp = -1.0;

   if (lm->free_line_cnt < 5) { // avoid empty free pool because of too many bad lines
        struct line *line = NULL;
        struct nand_block *block_tmp = NULL;
        struct ppa ppa;
        ppa.g.ch = 0;
        ppa.g.lun = 0;
        ppa.g.pl = 0;
        int youngest_block_erase = INT_MAX;
        for (int i = 0; i < lm->victim_line_cnt; i++) {
            line = lm->victim_line_pq->d[i+1];
            ppa.g.blk = line->id;
            block_tmp = get_blk(ssd, &ppa);
            if ((line->ipc > ssd->sp.pgs_per_line / 2) && (block_tmp->erase_cnt < youngest_block_erase)) { // Search from candidates with WAF < 1.5
                youngest_block_erase = block_tmp->erase_cnt;
                victim_line = line;
            }
        }
        if (victim_line == NULL){
            ssd->cv_moderate = 1;
        } else {
            ftl_log("Low free pool!\n");
            goto gotit;
        }
    }

    if (victim_line == NULL) { // normal case
        victim_line = pqueue_peek(lm->victim_line_pq);
        if (!victim_line) {
            return NULL;
        }
        total_vpc += victim_line->vpc;

        /* CV victim selection policy */
        // for CV, we don't want lots of blocks to become bad at the same time
        // set preference low vpc, high ec, low line id
        if ((ssd->sp).wl == 0 && (ssd->cv_moderate == 0)) {
            struct line *line = NULL;
            struct nand_block *block_tmp = NULL;
            struct ppa ppa;
            ppa.g.ch = 0;
            ppa.g.lun = 0;
            ppa.g.pl = 0;
            ppa.g.blk = victim_line->id;
            struct nand_block *block_victim = get_blk(ssd, &ppa);
            // line_score = victim_line->ipc/spp->pgs_per_line*0.4 + 0.3*block_victim->erase_cnt/spp->endurance;
            for (int i = 1; i < lm->victim_line_cnt; i++) {
                line = lm->victim_line_pq->d[i + 1]; // d[1] is the result of peek(), we check the rest.
                ppa.g.blk = line->id;
                block_tmp = get_blk(ssd, &ppa);
                total_vpc += line->vpc;

                // line_score_tmp = line->ipc/spp->pgs_per_line*0.4 + 0.3*block_tmp->erase_cnt/spp->endurance;
                // if (line_score_tmp > line_score && line->vpc < ssd->sp.pgs_per_line / 8) {
                //     victim_line = line;
                //     block_victim = block_tmp;
                //     line_score = line_score_tmp;
                //	continue;
                // }
                // /***
                if (line->vpc > victim_line->vpc) {
                    continue;
                }
                if (line->vpc < victim_line->vpc) {
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
                if ((block_tmp->erase_cnt > block_victim->erase_cnt)) { // equal vpc, we compare ec
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
                if ((block_tmp->erase_cnt == block_victim->erase_cnt) && (line->id < victim_line->id)) { // equal vpc and ec, we prefer lower line id
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
                // ***/
            }
        } else { // define the behavior when multiple candidates have the same ipc for WL
            struct line *line = NULL;
            struct nand_block *block_tmp = NULL;
            struct ppa ppa;
            ppa.g.ch = 0;
            ppa.g.lun = 0;
            ppa.g.pl = 0;
            ppa.g.blk = victim_line->id;
            struct nand_block *block_victim = get_blk(ssd, &ppa);
            for (int i = 1; i < lm->victim_line_cnt; i++) {
                if (!lm->victim_line_pq->d[i+1]) {
                    ftl_log("lm->victim_line_pq->d[i+1] NULL\n");
                    continue;
                }
                line = lm->victim_line_pq->d[i+1]; // d[1] is the result of peek(), we check the rest.
                ppa.g.blk = line->id;
                block_tmp = get_blk(ssd, &ppa);
                total_vpc += line->vpc;
                if (line->vpc < victim_line->vpc) {
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
                if ((line->vpc == victim_line->vpc) && (block_tmp->erase_cnt < block_victim->erase_cnt)) {
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
            }
        }

        if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
            return NULL;
        }
    }

    if ((ssd->cv_moderate == 0) && (lm->bad_line_cnt > 0) && (victim_line->vpc > ssd->sp.pgs_per_line / 2)){ // If WAF is high, we should turn on CV-moderate to slow capacity loss.
        ssd->cv_moderate = 1;
    }
gotit: ;
    pqueue_remove(lm->victim_line_pq, victim_line);
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
//    struct ppa ppa;
//    ppa.g.ch = 0;
//    ppa.g.lun = 0;
//    ppa.g.pl = 0;
//    ppa.g.blk = victim_line->id;
//    struct nand_block *block_tmp = NULL;
//    block_tmp = get_blk(ssd, &ppa);
//    int ec = block_tmp->erase_cnt;
//    ftl_log("GC victim_line %d (%d/%d), valid pages = %d, invalid pages = %d!\n", victim_line->id, ec, (ssd->sp).endurance, victim_line->vpc, victim_line->ipc);
    // float util = (float)total_vpc / (spp->tt_pgs);
    // ftl_log("GC for write victim line %d, valid pages = %d, invalid pages = %d, device_util = %.3f!\n", victim_line->id, victim_line->vpc, victim_line->ipc, util);
    return victim_line;
}


/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }
    (ssd->sp).pages_from_gc += cnt;
    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}


static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}



static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    struct nand_block *block;
    int ch, lun;
    struct write_pointer *wpp = &ssd->wp;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        ftl_log("In do_gc: select_victim_line error\n");
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }
    // ftl_log("In do_gc: copy back valid data okay!\n");

    /* update line status */
    ppa.g.ch = 0;
    ppa.g.lun = 0;
    ppa.g.pl = 0;
    block = get_blk(ssd, &ppa);
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    if ((spp->wl == 0) && (ssd->cv_moderate) && (block->erase_cnt > (spp->retired_ec - 5)) && (block->erase_cnt < spp->retired_ec)) {
    // if ((spp->wl == 0) && (ssd->cv_moderate) && (block->erase_cnt > spp->retired_ec) && (block->erase_cnt < spp->endurance)) {
        goto maintainCapacity;
    }
    if (block->erase_cnt >= spp->endurance || (spp->wl == 0 && ssd->cv_moderate == 0 && block->erase_cnt > spp->retired_ec)) {
    // if (block->erase_cnt >= spp->retired_ec) {
        line = get_line(ssd, &ppa);
        line->ipc = 0;
        line->vpc = 0;
        /* move this line to bad line list */
        QTAILQ_INSERT_TAIL(&lm->bad_line_list, line, entry);
        lm->bad_line_cnt++;
        ftl_log("Line %d becomes bad! (bad_capacity = %dGiB, bad_lines = %d, budget = %d, gc_thres = %d) \n", victim_line->id, lm->bad_line_cnt*spp->blks_per_line/(1024*1024*1024/spp->secsz/spp->secs_per_blk),lm->bad_line_cnt, spp->tt_lines - spp->gc_thres_lines_high, spp->gc_thres_lines_high);
        if ((spp->wl == 1 && ((lm->bad_line_cnt * spp->secs_per_line) >=  spp->op)) || (lm->bad_line_cnt >= (spp->tt_lines - spp->gc_thres_lines_high))) {
            ftl_err("SSD reaches its end of the life!\n");
            abort();
        }

        if (spp->wl == 0 && ssd->cv_moderate == 0) { // after a mapping-out, we check if we should enable cv-moderate
            struct line *line;
            double util = 0.0;
            for (int i = 0; i < lm->tt_lines; i++) {
                line = &lm->lines[i];
                util += line->vpc;
            }
            util = util/((lm->tt_lines - lm->bad_line_cnt)*ssd->sp.pgs_per_line);
            if (util > 0.75) {
            //if (util > 0.8) {
                ssd->cv_moderate = 1;
            }
        }
    } else {
        // check for wear leveling (PWL)
        if (spp->wl) {
maintainCapacity: ;
            // ftl_log("In do_gc: In PWL!\n");
            int youngest_line_id = -1;
            int youngest_block_erase = INT_MAX;
            int erase_sum = 0;
            int wl_lines = 0;
            struct nand_block *block_tmp;
            for (int i = 0; i < lm->tt_lines; i++) {
                line = &lm->lines[i];
                ppa.g.blk = line->id;
                block_tmp = get_blk(ssd, &ppa);
                if (block_tmp->erase_cnt < spp->endurance) {
                    wl_lines++;
                    erase_sum += block_tmp->erase_cnt;
                }
                if (block_tmp->erase_cnt > block->erase_cnt) {
                    continue;
                }
                if ((line->id != victim_line->id) && (line != wpp->curline) && (line->vpc > 0) && (block_tmp->erase_cnt < youngest_block_erase)) { // full lines or victim lines
                    youngest_line_id = line->id;
                    youngest_block_erase = block_tmp->erase_cnt;
                }
                //else if (((ssd->sp).total_ec/(ssd->sp).tt_blks >= 100) && (block_tmp->erase_cnt == 0)) {
                //    ftl_log("WL: line %d ec = 0 is skipped, due to vpc = %d, ipc = %d, \n",line->id, line->vpc, line->ipc);
                //}
            }
            if (youngest_line_id != -1) { // line->id is only used to identify blocks' location. Don't swap line->id, which will mess up victim_line_pq's order
//                int threshold = erase_sum / spp->tt_lines / 2 + spp->endurance / 2;
//                int threshold = erase_sum / spp->tt_lines * 3 / 4 + spp->endurance / 4;
                int threshold = erase_sum / wl_lines * 9 / 10 + spp->endurance / 10;
                if (block->erase_cnt > threshold) {
//                    ftl_log("WL migrate pages, vpc = %d, ipc = %d\n",line->vpc, line->ipc);
                    // apply data migration latency: read and erase for the less-used block, write for the over-used block
                    /* copy back valid data */
                    // ftl_log("In do_gc: PWL initiated!\n");
                    struct nand_block *block_cold = NULL;
                    struct nand_block *block_hot = NULL;
                    int counter = 0;
                    for (ch = 0; ch < spp->nchs; ch++) {
                        for (lun = 0; lun < spp->luns_per_ch; lun++) { // operations are under the same lun(way)
                            ppa.g.ch = ch;
                            ppa.g.lun = lun;
                            ppa.g.pl = 0;
                            lunp = get_lun(ssd, &ppa);


                            ppa.g.blk = youngest_line_id; // read cold data from less-used block
                            block_cold = get_blk(ssd, &ppa);
                            int block_cold_vpc = block_cold->vpc;
                            (ssd->sp).pages_from_wl += block_cold_vpc;
                            counter = block_cold_vpc;
                            if (spp->enable_gc_delay) {
                                struct nand_cmd gcr;
                                gcr.type = WL_IO;
                                gcr.cmd = NAND_READ;
                                gcr.stime = 0;
                                while (counter > 0) {
                                    ssd_advance_status(ssd, &ppa, &gcr);
                                    lunp->gc_endtime = lunp->next_lun_avail_time;
                                    counter--;
                                }
                            }


                            ppa.g.blk = victim_line->id; // write cold data to over-used block
                            block_hot = get_blk(ssd, &ppa);
                            counter = block_cold_vpc;
                            if (ssd->sp.enable_gc_delay) {
                                struct nand_cmd gcw;
                                gcw.type = WL_IO;
                                gcw.cmd = NAND_WRITE;
                                gcw.stime = 0;
                                while (counter > 0) {
                                    ssd_advance_status(ssd, &ppa, &gcw);
                                    lunp->gc_endtime = lunp->next_lun_avail_time;
                                    counter--;
                                }
                            }

                            int hot_erase_cnt = block_hot->erase_cnt;
                            int hot_id = block_hot->id;

                            ppa.g.blk = youngest_line_id; // erase the less-used block
                            if (spp->enable_gc_delay) {
                                struct nand_cmd gce;
                                gce.type = WL_IO;
                                gce.cmd = NAND_ERASE;
                                gce.stime = 0;
                                ssd_advance_status(ssd, &ppa, &gce);
                                lunp->gc_endtime = lunp->next_lun_avail_time;
                                block_cold->erase_cnt += spp->acceleration;
                                spp->total_ec += spp->acceleration;
                            }


                            /* swap block info to simulate data migration*/
                            block_hot->read_cnt = 0;
                            block_hot->erase_cnt = block_cold->erase_cnt;
                            block_hot->id = block_cold->id;
                            block_cold->read_cnt = 0;
                            block_cold->erase_cnt = hot_erase_cnt;
                            block_cold->id = hot_id;

                        }
                    }
                    ftl_log("WL complete between line %d (ec = %d) and line %d (ec = %d)\n", victim_line->id, block_hot->erase_cnt, youngest_line_id, block_cold->erase_cnt);
                }
            }
        }
        ppa.g.blk = victim_line->id;
        mark_line_free(ssd, &ppa);
        lm->count_line_for_write--;
    }

    return 0;
}

static uint64_t do_gc_read(struct ssd *ssd, NvmeRequest *req)
{
    // ftl_log("In ssd_read: ssd_read start!\n");
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    
    if (end_lpn >= spp->tt_pgs) {
        end_lpn = spp->tt_pgs - 1;
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
//        ssd->reads++;
        // ssd->lpnrtbl[lpn]++;

        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        // if ((ssd->rwtbl[lpn]) && (ssd->lpnrtbl[lpn] > 10)) {
        //     ssd_relocate_for_read(ssd, lpn);
        //     ssd->rwtbl[lpn] = false;
        //     ppa = get_maptbl_ent(ssd, lpn);
        // }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        ssd_gc_read(ssd, &ppa, &srd);
    }
    // ftl_log("In ssd_read: ssd_read done!\n");
    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    // ftl_log("In ssd_read: ssd_read start!\n");
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    
    (ssd->sp).pages_from_host_read += (end_lpn - start_lpn) + 1;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
        end_lpn = spp->tt_pgs - 1;
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
//        ssd->reads++;
        // ssd->lpnrtbl[lpn]++;

        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        // if ((ssd->rwtbl[lpn]) && (ssd->lpnrtbl[lpn] > 10)) {
        //     ssd_relocate_for_read(ssd, lpn);
        //     ssd->rwtbl[lpn] = false;
        //     ppa = get_maptbl_ent(ssd, lpn);
        // }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
        // ssd_gc_read(ssd, &ppa, &srd);
    }
    // ftl_log("In ssd_read: ssd_read done!\n");
    return maxlat;
}


static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    // ftl_log("In ssd_write: ssd_write start!\n");
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    
    (ssd->sp).pages_from_host += (end_lpn - start_lpn) + 1;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
        end_lpn = spp->tt_pgs - 1;
    }
    // ftl_log("start_lpn=%"PRIu64"\n", start_lpn);
    while (should_gc_high(ssd)) {
        r = do_gc(ssd, true);
        if (r == -1) {
            abort();
        }
    }
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_remap(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    uint64_t source_lba = req->oc12_slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    uint64_t start_source_lpn = source_lba / spp->secs_per_pg;
    // uint64_t end_source_lpn = (source_lba + len - 1) / spp->secs_per_pg;
    uint64_t index = 0;
    uint64_t lpn_from = 0;
    uint64_t lpn_to = 0;
//    (ssd->sp).pages_from_host += (end_lpn - start_lpn) + 1;
    struct ppa ppa;
    uint64_t lpn;
    // ftl_log("start_to_lpn=%"PRIu64", end_to_lpn=%"PRIu64", start_from_lpn=%"PRIu64", end_from_lpn=%"PRIu64"\n", start_lpn, end_lpn, start_source_lpn, end_source_lpn);
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
        end_lpn = spp->tt_pgs - 1;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        index = lpn - start_lpn;
        lpn_to = start_lpn + index;
        lpn_from = start_source_lpn + index;
        ppa = get_maptbl_ent(ssd, lpn_from);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            ssd->maptbl[lpn_from].ppa = UNMAPPED_PPA;
            /* update maptbl */
            set_maptbl_ent(ssd, lpn_to, &ppa);
            /* update rmap */
            set_rmap_ent(ssd, lpn_to, &ppa);
        } else {
//            ftl_err("REMAP unmapped: start_lpn=%"PRIu64"\n", start_lpn);
        }
    }

    return 0;
}

static void ssd_dsm(FemuCtrl *n, struct ssd *ssd, NvmeRequest *req){
    // Mostly adapted from nvme_dsm() in nvme-io.c.
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t start_lpn;
    uint64_t end_lpn;

    if (req->cmd.cdw11 & NVME_DSMGMT_AD){
        if (spp->acceleration == 1) {
            ftl_log("FEMU trim initiating...\n");
        }
        uint16_t nr = (req->cmd.cdw10 & 0xff) + 1;
        NvmeDsmRange range[nr];

        // FEMU will handle the real I/O request first
        // and also finished all sanity check on DSM range.
        // See nvme_dsm() in nvme-io.c.
        // However, we still need to get range information using this function.
        dma_write_prp(n, (uint8_t *)range, sizeof(range), req->cmd.dptr.prp1, req->cmd.dptr.prp2);
        // We can skip sanity check here.
        for (int i = 0; i < nr; i++) {
            lba = le64_to_cpu(range[i].slba);
            len = le32_to_cpu(range[i].nlb);

            start_lpn = lba / spp->secs_per_pg;
            end_lpn = (lba + len - 1) / spp->secs_per_pg;
            if (end_lpn >= spp->tt_pgs) {
                ftl_err("start_lpn=%"PRIu64", end_lpn=%"PRIu64", tt_pgs=%d\n", start_lpn, end_lpn, ssd->sp.tt_pgs);
                end_lpn = spp->tt_pgs - 1;
            }
            // Mark these pages as invalid
            for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
                ppa = get_maptbl_ent(ssd, lpn);
                if (mapped_ppa(&ppa)) {
                    // This physical page is invalid now
                    mark_page_invalid(ssd, &ppa);
                    // This LPN is also invalid now
                    ssd->maptbl[lpn].ppa = UNMAPPED_PPA;
                    set_rmap_ent(ssd, INVALID_LPN, &ppa);
                }
            }
        }
    } else if (req->cmd.cdw11 & NVME_DSMGMT_IDR) {
        uint16_t nr = (req->cmd.cdw10 & 0xff) + 1;
        NvmeDsmRange range[nr];
        dma_write_prp(n, (uint8_t *)range, sizeof(range), req->cmd.dptr.prp1, req->cmd.dptr.prp2);
        for (int i = 0; i < nr; i++) {
            lba = le64_to_cpu(range[i].slba);
            len = le32_to_cpu(range[i].nlb);

            start_lpn = lba / spp->secs_per_pg;
            end_lpn = (lba + len - 1) / spp->secs_per_pg;
            if (end_lpn >= spp->tt_pgs) {
                ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
                end_lpn = spp->tt_pgs - 1;
            }
            // Mark these data as read dominant
            for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
                // if (spp->wl) {
                //     ssd->rwtbl[lpn] = false;
                //     continue;
                // }
                // if (ssd_relocate_for_read(ssd, lpn)) {
                //     ssd->rwtbl[lpn] = false;
                // }
            }
        }
        ftl_log("Mark LBA %lu with range %u as read dominant data!\n", lba, len);
    } else if (req->cmd.cdw11 & NVME_DSMGMT_IDW) {
        uint16_t nr = (req->cmd.cdw10 & 0xff) + 1;
        NvmeDsmRange range[nr];
        dma_write_prp(n, (uint8_t *)range, sizeof(range), req->cmd.dptr.prp1, req->cmd.dptr.prp2);
        for (int i = 0; i < nr; i++) {
            lba = le64_to_cpu(range[i].slba);
            len = le32_to_cpu(range[i].nlb);

            start_lpn = lba / spp->secs_per_pg;
            end_lpn = (lba + len - 1) / spp->secs_per_pg;
            if (end_lpn >= spp->tt_pgs) {
                ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
                end_lpn = spp->tt_pgs - 1;
            }
            // Mark these data as write dominant
            for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
                // ssd->rwtbl[lpn] = true;
            }
        }
        ftl_log("Mark LBA %lu with range %u as write dominant data!\n", lba, len);
    } else {
        return;
    }
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
                case NVME_CMD_WRITE:
                    lat = ssd_write(ssd, req);
                    break;
                case NVME_CMD_READ:
                    lat = ssd_read(ssd, req);
                    break;
                case NVME_CMD_DSM:
                    lat = 0;
                    // DZ Start
                    // Handle discard request here
                    ssd_dsm(n, ssd, req);
                    // DZ End
                    break;
                case NVME_CMD_REMAP:
                    // ftl_log("FEMU: REMAP is called in FTL.\n");
                    lat = ssd_remap(ssd, req);
                    break;
                default:
                    //ftl_err("FTL received unkown request type, ERROR\n");
                    ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            if (req->cmd.opcode == NVME_CMD_READ) {
                do_gc_read(ssd, req);
            }

            /* clean one line if needed (in the background) */
        //    if (should_gc(ssd)) {
        //        do_gc(ssd, false);
        //    }
        }
    }
#ifdef FEMU_MYDEBUG_FTL
    fclose(femu_log_file);
#endif
    return NULL;
}


