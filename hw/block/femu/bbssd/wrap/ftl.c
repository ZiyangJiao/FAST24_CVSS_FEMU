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

static bool is_write_heavy(struct ssd *ssd, uint64_t lpn)
{
    // write_heavy = ssd->rwtbl[lpn] || (ssd->lpnwtbl[lpn] > ssd->writes/(ssd->sp).tt_pgs);
    if (((&ssd->sp)->wl) || (ssd->cv_moderate)) {
        return true;
    }
    return (ssd->rwtbl[lpn]);
}

static inline bool should_gc(struct ssd *ssd)
{
//    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = pqueue_peek(lm->victim_line_pq);
//    return ((victim_line->ipc) >= (ssd->sp.pgs_per_line / 4));
    if (!victim_line)
        return false;
    return ((victim_line->vpc) == 0);
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
    wpp = &ssd->wp2;

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
    ftl_log("Init write line %d for read!\n", wpp->blk);

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

    // if (((ssd->sp).wl == 1) && lm->free_line_cnt < 60) { // youngest block first
    //     struct nand_block *block_tmp = NULL;
    //     int hottest = 0;
    //     struct ppa ppa;
    //     ppa.g.ch = 0;
    //     ppa.g.lun = 0;
    //     ppa.g.pl = 0;
    //     ppa.g.blk = curline->id;
    //     block_tmp = get_blk(ssd, &ppa);
    //     hottest = block_tmp->erase_cnt;
    //     tmpline = curline;
    //     for (int i = 1; i < lm->free_line_cnt; i++) {
    //         tmpline = QTAILQ_NEXT(tmpline, entry);
    //         ppa.g.blk = tmpline->id;
    //         block_tmp = get_blk(ssd, &ppa);
    //         if (block_tmp->erase_cnt < hottest) {
    //             hottest = block_tmp->erase_cnt;
    //             curline = tmpline;
    //         }
    //     }
    // }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    lm->count_line_for_write++;
    return curline;
}

static struct line *get_next_free_line_for_read(struct ssd *ssd)
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
        int coldest = 0;
        struct ppa ppa;
        ppa.g.ch = 0;
        ppa.g.lun = 0;
        ppa.g.pl = 0;
        ppa.g.blk = curline->id;
        block_tmp = get_blk(ssd, &ppa);
        coldest = block_tmp->erase_cnt;
        tmpline = curline;
        for (int i = 1; i < lm->free_line_cnt; i++) {
            tmpline = QTAILQ_NEXT(tmpline, entry);
            ppa.g.blk = tmpline->id;
            block_tmp = get_blk(ssd, &ppa);
            if (block_tmp->erase_cnt < coldest) {
                coldest = block_tmp->erase_cnt;
                curline = tmpline;
            }
        }
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    lm->count_line_for_read++;
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
//                    struct line *line = lm->victim_line_pq->d[1];
//                    ftl_log("lm->victim_line_pq peak line id = %d, ipc = %d\n", line->id,line->ipc);
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

static void ssd_advance_write_pointer_for_read(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp2;
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
//                    struct line *line = lm->victim_line_pq->d[1];
//                    ftl_log("lm->victim_line_pq peak line id = %d, ipc = %d\n", line->id,line->ipc);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line_for_read(ssd);
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

static struct ppa get_new_page_for_read(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp2;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);
//    ppa.read_heavy = true;

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
//	spp->secs_per_pg = 8*4; /* Page = 16KiB*/
//  spp->pgs_per_blk = 256; /* BLK = 1MiB */
//  spp->pgs_per_blk = 256*4*2; /* BLK = 8MiB */
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
    spp->acceleration = 1;
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

static void ssd_init_rwtbl(struct ssd *ssd)
{
    ssd->reads = 0;
    ssd->writes = 0;
    struct ssdparams *spp = &ssd->sp;

    ssd->rwtbl = g_malloc0(sizeof(bool) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rwtbl[i] = true;
    }

    ssd->lpnrtbl = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->lpnrtbl[i] = 0;
    }

    ssd->lpnwtbl = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->lpnwtbl[i] = 0;
    }
}

void ssd_init(FemuCtrl *n)
{
#ifdef FEMU_MYDEBUG_FTL
    char str[80];
    sprintf(str, "/mnt/tmp_sdc/femu/build-femu//femu_debug.log");
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

    ssd_init_rwtbl(ssd);

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

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    struct nand_block *blk = get_blk(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
        case NAND_READ:
            /* read: perform NAND cmd first */
            nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
            double ec = (double)blk->erase_cnt;
            double rc = 100;
//            double rc = (double)blk->read_cnt; /* User IO + Internal IO*/
            double rber = spp->epsilon + spp->alpha*pow(ec,spp->k) + \
                     spp->gamma*pow(ec,spp->p)*pow(rc,spp->q);
            rber = rber > 1.0 ? 1.0 : rber;
            int bits_count = spp->secs_per_pg * spp->secsz * 8;
//            if (ncmd->type == WL_IO) {
//                blk->read_cnt += 1;
//            }
            //blk->read_cnt += 1;
            int read_retry = 0;
            // while (blk->erase_cnt > 100 && ((int)(bits_count * rber) > spp->ecc_corr_str)) {
            while ((int)(bits_count * rber) > spp->ecc_corr_str) {
                // assuming read retry process will not introduce extra error
                // assuming each read retry will lower the RBER by half
                rber /= 2.0;
                read_retry += 1;
            }
            /*
            if (read_retry > 2) {
                read_retry -= 2; // normalization (base would trigger 2 times)
                if (spp->wl == 0){ // blk->erase_cnt <= 263
                    read_retry = 3;
                }
                if (spp->wl == 0 && blk->erase_cnt <= 185){
                    read_retry = 2;
                }
                if (spp->wl == 0 && blk->erase_cnt <= 127){
                    read_retry = 1;
                }
                spp->read_retry += read_retry;
            }
            */
            spp->read_retry += read_retry;
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
            nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                lun->next_lun_avail_time;
            float percentage_used = (float)blk->erase_cnt/(float)spp->endurance;
            double ratio = 0.9865636536464101 + (-1.5470682790191133) * percentage_used + \
                2.4621867081807056 * pow(percentage_used,2) + \
                (-1.1217434639021386) * pow(percentage_used,3);
            if (ncmd->type == USER_IO) {
                lun->next_lun_avail_time = nand_stime + (int)spp->pg_wr_lat*ratio;
            } else {
                lun->next_lun_avail_time = nand_stime + (int)spp->pg_wr_lat*ratio;
            }
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
    struct write_pointer *wpp2 = &ssd->wp2;

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
//    line->vpc--;
    if ((wpp->curline != line) && (wpp2->curline != line) && (!was_full_line)) {
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
//        struct line *line = lm->victim_line_pq->d[1];
//        ftl_log("lm->victim_line_pq peak line id = %d, ipc = %d\n", line->id,line->ipc);
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
    // if (!write_heavy)
    //     new_ppa = get_new_page_for_read(ssd);
    // else
    //     new_ppa = get_new_page(ssd);
    if (((&ssd->sp)->wl) || (ssd->cv_moderate)) {
        new_ppa = get_new_page(ssd);
    } else {
        new_ppa = get_new_page_for_read(ssd); // GC-ed data are considered as read-dominant
    }
    
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
    if (((&ssd->sp)->wl) || (ssd->cv_moderate)) {
        ssd_advance_write_pointer(ssd);
    } else {
        ssd_advance_write_pointer_for_read(ssd);
        ssd->rwtbl[lpn] = false; // has relocated for read
    }
    
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

static uint64_t gc_write_page_for_read(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
//    bool read_heavy = ssd->rwtbl[lpn];

    ftl_assert(valid_lpn(ssd, lpn));
//    if (read_heavy)
//        new_ppa = get_new_page_for_read(ssd);
//    else
//        new_ppa = get_new_page(ssd);
    new_ppa = get_new_page_for_read(ssd); // GC-ed data are considered as read-dominant
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    set_rmap_ent(ssd, INVALID_LPN, old_ppa);

    ssd->rwtbl[lpn] = false; // has relocated for read

    /* need to advance the write pointer here */
    ssd_advance_write_pointer_for_read(ssd); // since we consider GC-ed data as read, we advance wp2 here.

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

//    int ec = -1;
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
            if ((line->ipc > ssd->sp.pgs_per_line / 4) && (block_tmp->erase_cnt < youngest_block_erase)) {
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

    if (victim_line == NULL) {
        victim_line = pqueue_peek(lm->victim_line_pq);
        if (!victim_line) {
            return NULL;
        }
        total_vpc += victim_line->vpc;

        /* CV victim selection policy */
        // for CV, we don't want lots of blocks become bad at the same time
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
            for (int i = 1; i < lm->victim_line_cnt; i++) {
                line = lm->victim_line_pq->d[i + 1]; // d[1] is the result of peek(), we check the rest.
                ppa.g.blk = line->id;
                block_tmp = get_blk(ssd, &ppa);
                total_vpc += line->vpc;
                if (line->vpc > victim_line->vpc) {
                    continue;
                }
                if (line->vpc < victim_line->vpc) {
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
                if ((block_tmp->erase_cnt > block_victim->erase_cnt)) {
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
                if ((block_tmp->erase_cnt == block_victim->erase_cnt) && (line->id < victim_line->id)) {
                    victim_line = line;
                    block_victim = block_tmp;
                    continue;
                }
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
//            ec = block_victim->erase_cnt;
            for (int i = 1; i < lm->victim_line_cnt; i++) {
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

    // if ((ssd->cv_moderate == 0) && (lm->bad_line_cnt > 0) && (victim_line->vpc > ssd->sp.pgs_per_line / 4)){
    //     ssd->cv_moderate = 1;
    // }
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
gotit: ;
    return victim_line;
}

static struct line *select_victim_line_for_read(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    int total_vpc = lm->full_line_cnt * spp->pgs_per_line;

//    int ec = -1;
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
            if ((line->ipc > ssd->sp.pgs_per_line / 4) && (block_tmp->erase_cnt < youngest_block_erase)) {
                youngest_block_erase = block_tmp->erase_cnt;
                victim_line = line;
            }
        }
        if (victim_line == NULL){
            ssd->cv_moderate = 1;
        } else {
            goto gotit;
        }
    }

    if (victim_line == NULL) {
        victim_line = pqueue_peek(lm->victim_line_pq);
        if (!victim_line) {
            return NULL;
        }
        total_vpc += victim_line->vpc;

        struct line *line = NULL;
        struct nand_block *block_tmp = NULL;
        struct ppa ppa;
        ppa.g.ch = 0;
        ppa.g.lun = 0;
        ppa.g.pl = 0;
        ppa.g.blk = victim_line->id;
        struct nand_block *block_victim = get_blk(ssd, &ppa);
        for (int i = 1; i < lm->victim_line_cnt; i++) {
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

        if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
            return NULL;
        }
    }


//    pqueue_pop(lm->victim_line_pq);
    // if ((ssd->cv_moderate == 0) && (lm->bad_line_cnt > 0) && (victim_line->vpc > ssd->sp.pgs_per_line / 4)){
    //     ssd->cv_moderate = 1;
    // }
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
    // ftl_log("GC for read victim line %d, valid pages = %d, invalid pages = %d, device_util = %.3f!\n", victim_line->id, victim_line->vpc, victim_line->ipc, util);
gotit: ;    
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

static void clean_one_block_for_read(struct ssd *ssd, struct ppa *ppa)
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
            gc_write_page_for_read(ssd, ppa);
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

static int do_gc_for_read(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    struct nand_block *block;
    int ch, lun;
    struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *wpp2 = &ssd->wp2;

    victim_line = select_victim_line_for_read(ssd, force);
    if (!victim_line) {
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
            clean_one_block_for_read(ssd, &ppa);
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

    /* update line status */
    ppa.g.ch = 0;
    ppa.g.lun = 0;
    ppa.g.pl = 0;
    block = get_blk(ssd, &ppa);
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    if ((spp->wl == 0) && (ssd->cv_moderate) && (block->erase_cnt >= spp->retired_ec) && (block->erase_cnt < spp->endurance)) {
        goto maintainCapacity;
    }

    if (block->erase_cnt >= spp->endurance) {
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

        //
        // if (spp->wl == 0 && ssd->cv_moderate == 0) {
        //     struct line *line;
        //     double util = 0.0;
        //     for (int i = 0; i < lm->tt_lines; i++) {
        //         line = &lm->lines[i];
        //         util += line->vpc;
        //     }
        //     util = util/((lm->tt_lines - lm->bad_line_cnt)*ssd->sp.pgs_per_line);
        //     if (util > 0.75) {
        //         ssd->cv_moderate = 1;
        //     }
        // }
    } else {
        // check for wear leveling (PWL)
        if (spp->wl) {
maintainCapacity: ;
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
                if ((line->id != victim_line->id) && (line != wpp->curline) && (line != wpp2->curline) && (line->vpc > 0) && (block_tmp->erase_cnt < youngest_block_erase)) { // full lines or victim lines
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
//                            (ssd->sp).pages_from_gc += block_cold_vpc;
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
                                    // block_cold->read_cnt++;
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

//                            int hot_read_cnt = block_hot->read_cnt;
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
//                                block_cold->erase_cnt++;
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
        lm->count_line_for_read--;
    }

    return 0;
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
    struct write_pointer *wpp2 = &ssd->wp2;

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

    if ((spp->wl == 0) && (ssd->cv_moderate) && (block->erase_cnt >= spp->retired_ec) && (block->erase_cnt < spp->endurance)) {
        goto maintainCapacity;
    }
    if (block->erase_cnt >= spp->endurance) {
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

        //
        // if (spp->wl == 0 && ssd->cv_moderate == 0) {
        //     struct line *line;
        //     double util = 0.0;
        //     for (int i = 0; i < lm->tt_lines; i++) {
        //         line = &lm->lines[i];
        //         util += line->vpc;
        //     }
        //     util = util/((lm->tt_lines - lm->bad_line_cnt)*ssd->sp.pgs_per_line);
        //     if (util > 0.75) {
        //         ssd->cv_moderate = 1;
        //     }
        // }
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
                if ((line->id != victim_line->id) && (line != wpp->curline) && (line != wpp2->curline) && (line->vpc > 0) && (block_tmp->erase_cnt < youngest_block_erase)) { // full lines or victim lines
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
                // ftl_log("In do_gc: youngest_line_id != -1!\n");
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
//                            (ssd->sp).pages_from_gc += block_cold_vpc;
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
                                    // block_cold->read_cnt++;
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

//                            int hot_read_cnt = block_hot->read_cnt;
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
//                                block_cold->erase_cnt++;
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

static uint64_t ssd_relocate_for_read(struct ssd *ssd, uint64_t lpn);
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
    }
    // ftl_log("In ssd_read: ssd_read done!\n");
    return maxlat;
}

static uint64_t ssd_relocate_for_read(struct ssd *ssd, uint64_t lpn)
{
//    ftl_log("In ssd_relocate_for_read\n");
    struct nand_block *current_block;
    struct nand_block *wpp2_block;
    struct ppa current_ppa;
    struct ppa wpp2_ppa;
    // uint64_t curlat = 0;
    int r;
    struct line_mgmt *lm = &ssd->lm;
    lm->count_line_for_write = 0;
    lm->count_line_for_read = 0;

    current_ppa = get_maptbl_ent(ssd, lpn);
    if (!mapped_ppa(&current_ppa)) { // sanity test
        return 0;
    }
    if ((ssd->cv_moderate == 1) || ((&ssd->sp)->wl)) { // we want to evenly use blocks
        return 0;
    }
    if (!is_write_heavy(ssd, lpn)) { // has been handled before
        // ftl_log("has been handled before\n");
        return 0;
    }
    current_block = get_blk(ssd, &current_ppa);
    wpp2_ppa = get_new_page_for_read(ssd);
    wpp2_block = get_blk(ssd, &wpp2_ppa);
    if (current_block->erase_cnt <= wpp2_block->erase_cnt) { // has been stored in a young block
        return 1;
    }
    // ftl_log("ssd_relocate_for_read: ready to relocate!\n");
    (ssd->sp).pages_from_wl += 1;
    /* update old page information first */
    mark_page_invalid(ssd, &current_ppa);
    set_rmap_ent(ssd, INVALID_LPN, &current_ppa);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &wpp2_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &wpp2_ppa);
    mark_page_valid(ssd, &wpp2_ppa);
    ssd_advance_write_pointer_for_read(ssd);

    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    /* get latency statistics */
    ssd_advance_status(ssd, &wpp2_ppa, &swr);
    // ftl_log("relocate lpn\n");

    // GC fully invalidated blocks
    // while (should_gc(ssd)) { // if we can skip should_gc?
    //     r = do_gc(ssd, false);
    //     if (r == -1)
    //         break;
    // }
    while (should_gc_high(ssd)) {
        // ftl_log("do_gc_for_read!\n");
        r = do_gc_for_read(ssd, true);
        if (r == -1) {
            ftl_err("SSD reaches its end of the life!\n");
            abort();
        }
    }
    // ftl_log("ssd_relocate_for_read: done!\n");
    return 1;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    // ftl_log("In ssd_write: ssd_write start!\n");
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    // Add WA tracker
    (ssd->sp).pages_from_host += (end_lpn - start_lpn) + 1;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    bool write_heavy = true;
    struct line_mgmt *lm = &ssd->lm;
    lm->count_line_for_write = 0;
    lm->count_line_for_read = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
        end_lpn = spp->tt_pgs - 1;
    }
    // ftl_log("start_lpn=%"PRIu64"\n", start_lpn);
    /***
    while (should_gc_high(ssd)) {
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }
    ***/
   // GC after write -> determine read_gc or write_gc
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
//        ssd->writes++;
//        ssd->lpnwtbl[lpn]++;
        ssd->lpnrtbl[lpn] = 0;
        ssd->rwtbl[lpn] = true;

        ppa = get_maptbl_ent(ssd, lpn);
        write_heavy = is_write_heavy(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        if (write_heavy)
            ppa = get_new_page(ssd);
        else
            ppa = get_new_page_for_read(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        if (write_heavy) {
            ssd_advance_write_pointer(ssd);
        } else {
            ssd_advance_write_pointer_for_read(ssd);
        }

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }
    // ftl_log("In ssd_write: host write done!\n");

    // GC fully invalidated blocks
    // while (should_gc(ssd)) { // if we can skip should_gc?
    //     r = do_gc(ssd, false);
    //     if (r == -1)
    //         break;
    // }

    while (should_gc_high(ssd)) {
        // ftl_log("In ssd_write: GC triggered!\n");
        while(lm->count_line_for_write > 0) {
            r = do_gc(ssd, true); // GC-ed data will be considered as read-dominant
            if (r == -1) {
                ftl_err("SSD reaches its end of the life!\n");
                abort();
            }
        }
        while((lm->count_line_for_read > 0) || should_gc_high(ssd)) {
            if (spp->wl) {
                r = do_gc(ssd, true);
            } else {
                r = do_gc_for_read(ssd, true);
            }
            if (r == -1) {
                ftl_err("SSD reaches its end of the life!\n");
                abort();
            }
            // ftl_log("Current # of free lines: %d\n", ssd->lm.free_line_cnt);
        }
        // ftl_log("In ssd_write: GC done!\n");
    }
    // ftl_log("In ssd_write: ssd_write done!\n");
    return maxlat;
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
                if (spp->wl) {
                    ssd->rwtbl[lpn] = false;
                    continue;
                }
                if (ssd_relocate_for_read(ssd, lpn)) {
                    ssd->rwtbl[lpn] = false;
                }
//                ppa = get_maptbl_ent(ssd, lpn);
//                if (mapped_ppa(&ppa)) {
//                    ppa.read_heavy = true;
//                }
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
                ssd->rwtbl[lpn] = true;
//                ppa = get_maptbl_ent(ssd, lpn);
//                if (mapped_ppa(&ppa)) {
//                    ppa.read_heavy = true;
//                }
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

            /* clean one line if needed (in the background) */
//            if (should_gc(ssd)) {
//                do_gc(ssd, false);
//            }
        }
    }
#ifdef FEMU_MYDEBUG_FTL
    fclose(femu_log_file);
#endif
    return NULL;
}


