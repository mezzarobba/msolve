/* This file is part of msolve.
 *
 * msolve is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * msolve is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with msolve.  If not, see <https://www.gnu.org/licenses/>
 *
 * Authors:
 * Jérémy Berthomieu
 * Christian Eder
 * Mohab Safey El Din */


#include "sba.h"

static inline crit_t *initialize_signature_criteria(
        const stat_t * const st
        )
{
    crit_t *crit    =   calloc((unsigned long)st->ngens, sizeof(crit_t));

    for (int i = 0; i < st->ngens; ++i) {
        crit[i].sz = 1;
        crit[i].sdm =  realloc(crit[i].sdm,
                (unsigned long)crit[i].sz * sizeof(sdm_t));
        crit[i].hm  =  realloc(crit[i].hm,
                (unsigned long)crit[i].sz * sizeof(hm_t));
    }

    return crit;
}

static inline void reset_signature_criteria(
        crit_t *crit,
        const stat_t * const st
        )
{
    for (len_t i = 0; i < st->ngens; ++i) {
        crit[i].ld = 0;
    }
}

static inline void free_signature_criteria(
        crit_t **critp,
        const stat_t * const st
        )
{
    crit_t *crit    =   *critp;
    for (len_t i = 0; i < st->ngens; ++i) {
        free(crit[i].sdm);
        crit[i].sdm =   NULL;
        free(crit[i].hm);
        crit[i].hm  =   NULL;
        crit[i].ld  =   0;
    }
    free(crit);
    crit    =   NULL;

    *critp  =   crit;
}

static len_t sba_add_new_elements_to_basis(
        const smat_t * const smat,
        const ht_t * const ht,
        bs_t *bs,
        const stat_t * const st
        )
{
    len_t i, j, k, ne;

    const len_t nr  = smat->cld;
    const len_t bld = bs->ld;

    bs->lo = bld;

    /* row indices of new elements are getting precached */
    len_t *rine = calloc((unsigned long)nr, sizeof(len_t));

    k = 0;
    i = 0;
next:
    for (; i < nr; ++i) {
        const hm_t lm = smat->cr[i][SM_OFFSET];
        for (j = 0; j < bld; ++j) {
            if (check_monomial_division(bs->hm[j][SM_OFFSET], lm, ht) == 1) {
                goto next;
            }
        }
        rine[k++] = i;
    }
    check_enlarge_basis(bs, k, st);

    /* now enter elements to basis */
    for (i = 0; i < k; ++i) {
        bs->hm[bs->ld] = (hm_t *)malloc(
                (unsigned long)(smat->cr[rine[i]][SM_LEN]+SM_OFFSET) *
                sizeof(hm_t));
        memcpy(bs->hm[bs->ld], smat->cr[rine[i]],
                (unsigned long)(smat->cr[rine[i]][SM_LEN]+SM_OFFSET) *
                sizeof(hm_t));
        bs->cf_32[bs->ld] = (cf32_t *)malloc(
                (unsigned long)(smat->cr[rine[i]][SM_LEN]) *
                sizeof(cf32_t));
        memcpy(bs->cf_32[bs->ld], smat->cc32[rine[i]],
                (unsigned long)(smat->cr[rine[i]][SM_LEN]) * sizeof(cf32_t));
        bs->ld++;
    }
    printf("add smat->cr %p\n", smat->cr);

    free(rine);
    rine = NULL;

    /* check number of new elements here, later on bs->ld might change */
    ne = k;

    /* If the input is NOT homogeneous, check if the new elements in
     * the basis makes older elements redundant. */
    if (st->homogeneous != 1) {
        /* Note: Now, bs->ld is the new load of the basis after adding
         * new elements, bld was set before thus it represents the old
         * load of the basis. */
        const len_t bln = bs->ld;
        k = 0;
        for (i = 0; i < bld; ++i) {
            const hm_t lm = bs->hm[i][SM_OFFSET];
            for (j = bld; j < bln; ++j) {
                if (check_monomial_division(bs->hm[j][SM_OFFSET], lm, ht) == 1) {
                    free(bs->hm[i]);
                    bs->hm[i] = NULL;
                    free(bs->cf_32[i]);
                    bs->cf_32[i] = NULL;
                }
                bs->hm[k]    = bs->hm[i];
                bs->cf_32[k] = bs->cf_32[i];
                k++;
            }
        }
        /* now add new elements correctly in minimized basis */
        for (i = bld; i < bln; ++i) {
            bs->hm[k]    = bs->hm[i];
            bs->cf_32[k] = bs->cf_32[i];
            k++;
        }
        bs->ld = k;
    }
    return ne;
}

static int is_signature_needed(
        const smat_t * const smat,
        const crit_t * const syz,
        const crit_t * const rew,
        const len_t idx,
        const len_t var_idx,
        ht_t *ht
        )
{
    /* get exponent vector and increment entry for var_idx */
    exp_t *ev   =   ht->ev[0];
    memcpy(ev, ht->ev[smat->pr[idx][SM_SMON]],
            (unsigned long)ht->evl * sizeof(exp_t));;
    /* Note: ht->ebl = #elimination variables + 1 */
    len_t shift   = var_idx < ht->ebl - 1 ? 1: 2;
    len_t deg_pos = shift == 2 ? ht->ebl : 0;
    ev[var_idx+shift]++;
    ev[deg_pos]++;

    const len_t sig_idx = smat->pr[idx][SM_SIDX];
    const hm_t hm       = insert_in_hash_table(ev, ht);
    const sdm_t nsdm    = ~ht->hd[hm].sdm;

    const len_t evl     = ht->evl;

    /* syzygy criterion */
syz:
    const crit_t syz_idx = syz[sig_idx];
    for (len_t i = 0; i < syz_idx.ld; ++i) {
        if (nsdm & syz_idx.sdm[i]) {
            continue;
        }
        const exp_t *sev = ht->ev[syz_idx.hm[i]];
        for (len_t j = 0; j < evl; ++j) {
            if (sev[j] > ev[j]) {
                goto syz;
            }
        }
        return 0;
    }
    /* rewrite criterion */
rew:
    const crit_t rew_idx = rew[sig_idx];
    for (len_t i = 0; i < rew_idx.ld; ++i) {
        if (nsdm & rew_idx.sdm[i]) {
            continue;
        }
        const exp_t *rev = ht->ev[rew_idx.hm[i]];
        for (len_t j = 0; j < evl; ++j) {
            if (rev[j] > ev[j]) {
                goto rew;
            }
        }
        return 0;
    }
    return 1;
}

static inline void enlarge_sba_matrix(
        smat_t *smat
        )
{
        smat->csz *= 2;
        smat->cr = realloc(
                smat->cr, (unsigned long)smat->csz * sizeof(hm_t *));
        smat->cc32 = realloc(
                smat->cc32, (unsigned long)smat->csz * sizeof(cf32_t *));
        smat->pc32 = realloc(
                smat->pc32, (unsigned long)smat->csz * sizeof(cf32_t *));
}

static inline void check_enlarge_rewrite_rule_array(
        crit_t *rew,
        const len_t sidx
        )
{
    if (rew[sidx].ld >= rew[sidx].sz) {
        rew[sidx].sz  *= 2;
        rew[sidx].sdm =  realloc(rew[sidx].sdm,
                (unsigned long)rew[sidx].sz * sizeof(sdm_t));
        rew[sidx].hm  =  realloc(rew[sidx].hm,
                (unsigned long)rew[sidx].sz * sizeof(hm_t));
    }
}

static inline void add_rewrite_rule(
        crit_t *rew,
        const smat_t * const smat,
        const ht_t * const ht
        )
{
    const len_t sidx = smat->cr[smat->cld-1][SM_SIDX];
    check_enlarge_rewrite_rule_array(rew, sidx);
    rew[sidx].hm[rew[sidx].ld]  = smat->cr[smat->cld-1][SM_SMON];
    rew[sidx].sdm[rew[sidx].ld] = ht->hd[smat->cr[smat->cld-1][SM_SMON]].sdm;
    rew[sidx].ld++;
}

static void add_row_to_sba_matrix(
        smat_t *smat,
        const len_t idx,
        const len_t var_idx,
        ht_t *ht
        )
{
    if (smat->cld >= smat->csz) {
        enlarge_sba_matrix(smat);
    }
    const len_t cld = smat->cld;
    /* Note: ht->ebl = #elimination variables + 1 */
    len_t shift     = var_idx < ht->ebl - 1 ? 1: 2;
    len_t deg_pos   = shift == 2 ? ht->ebl : 0;
    /* copy monomial entries in row */
    smat->cr[cld]  =   malloc(
            ((unsigned long)smat->pr[idx][SM_LEN]+SM_OFFSET) * sizeof(hm_t));
    memcpy(smat->cr[cld], smat->pr[idx],
            ((unsigned long)smat->pr[idx][SM_LEN]+SM_OFFSET) * sizeof(hm_t));

    /* now multiply each column entry with the corresponding variable */
    hm_t *cr            =   smat->cr[cld];
    /* Note that ht->ev[0] is already the multiplied signature, we have already
     * checked if we need this signature in is_signature_needed(), thus we can
     * just use it without further updating it. */
    exp_t *ev           =   ht->ev[0];
    /* multiply signature */
    cr[SM_SMON] = insert_in_hash_table(ev, ht);

    /* multiply monomials in corresp. polnoymial */
    const len_t len =  cr[SM_LEN] + SM_OFFSET;
    for (len_t i = SM_OFFSET; i < len; ++i) {
        memcpy(ev, ht->ev[cr[i]], (unsigned long)ht->evl * sizeof(exp_t));
        ev[var_idx+shift]++;
        ev[deg_pos]++;
        cr[i] = insert_in_hash_table(ev, ht);
    }
    smat->cld++;
}

static void add_multiples_of_previous_degree_row(
        smat_t *smat,
        const len_t idx,
        const crit_t * const syz,
        crit_t *rew,
        ht_t *ht,
        stat_t *st)
{
    const len_t nv  =   ht->nv;

    for (len_t i = 0; i < nv; ++i) {
        /* check syzygy and rewrite criterion */
        if (is_signature_needed(smat, syz, rew, idx, i, ht) == 1) {
            add_row_to_sba_matrix(smat, idx, i, ht);
            /* add rewrite rule */
            add_rewrite_rule(rew, smat, ht);
        }
    }
}

static inline void add_row_with_signature(
        smat_t *smat,
        const bs_t * const bs,
        const len_t pos
        )
{
    /* here we introduce initial generators to the matrix, note
     * that memory was already allocated correspondingly when
     * we prepared the next degree step */
    const len_t cld         = smat->cld;
    const len_t pld         = smat->pld;
    const len_t len = bs->hm[pos][LENGTH];
    smat->cr[cld]           = (hm_t *)malloc(
            (len + SM_OFFSET) * sizeof(hm_t));
    /* copy polynomial data, take a look at the difference between
     * the meta data stored in hm arrays from bs and the 
     * meta data stored in signature-based smat rows. */
    memcpy(smat->cr[cld]+SM_PRE,bs->hm[pos]+PRELOOP,
            (len + OFFSET - PRELOOP) * sizeof(hm_t));
    smat->pc32[pld]        = bs->cf_32[bs->hm[pos][COEFFS]];
    smat->cr[cld][SM_CFS]  = pld;
    printf("cld %u -> cfs pld %u\n", cld, pld);
    /* store also signature data */
    smat->cr[cld][SM_SMON] = bs->sm[pos];
    printf("sm[%u] = %u | %u\n", pos, bs->sm[pos], bs->si[pos]);
    smat->cr[cld][SM_SIDX] = bs->si[pos];
    smat->cld++;
    smat->pld++;
}

inline void add_syzygy_schreyer(
        crit_t *syz,
        const hm_t sm,
        const len_t si,
        const ht_t * const ht
        )
{
    while (syz[si].ld >= syz[si].sz) {
        syz[si].sz *= 2;
        syz[si].hm = realloc(syz[si].hm,
                (unsigned long)syz[si].sz * sizeof(hm_t));
        syz[si].sdm = realloc(syz[si].sdm,
                (unsigned long)syz[si].sz * sizeof(sdm_t));
    }
    syz[si].hm[syz[si].ld]  = sm;
    syz[si].sdm[syz[si].ld] = ht->hd[sm].sdm;
    syz[si].ld++;
}

static inline crit_t *initialize_syzygies_schreyer(
        const bs_t * const bs,
        ht_t *ht
        )
{
    /* when initializing syzygies we assume that bs->ld == st->ngens */
    crit_t *syz =   calloc((unsigned long)bs->ld, sizeof(crit_t));
    syz[0].ld   =   0;
    syz[0].sz   =   0;
    for (len_t i = 1; i < bs->ld; ++i) {
        syz[i].hm   =   calloc((unsigned long)i, sizeof(hm_t));
        syz[i].sdm  =   calloc((unsigned long)i, sizeof(sdm_t));
        syz[i].ld   =   0;
        syz[i].sz   =   i;
        for (len_t j = 0; j < i; ++j) {
            syz[i].hm[j]    = insert_multiplied_signature_in_hash_table(
                    bs->hm[j][OFFSET], bs->sm[i], ht);
            syz[i].sdm[j]   = ht->hd[syz[i].hm[j]].sdm;
        }
    }
    return syz;
}

static inline void initialize_signatures_schreyer(
        bs_t *bs
        )
{
    for (len_t i = 0; i < bs->ld; ++i) {
        bs->si[i]   =   bs->ld-1-i;
        bs->sm[i]   =   bs->hm[i][OFFSET];
    }
}

static inline void initialize_signatures_not_schreyer(
        bs_t *bs
        )
{
    for (len_t i = 0; i < bs->ld; ++i) {
        bs->si[i]   =   i;
        bs->sm[i]   =   0;
    }
}

static void sba_prepare_next_degree(
        smat_t *smat,
        const bs_t * const in,
        const len_t ni, /* number of input generators added */
        const stat_t * const st
        )
{

    printf("1 pr %p - cr %p\n", smat->pr, smat->cr);
    smat->pr   = smat->cr;
    printf("2 pr %p - cr %p\n", smat->pr, smat->cr);
    smat->pc32 = smat->cc32;
    smat->pld  = smat->cld;

    /* reset smat data */
    smat->cr   = NULL;
    printf("3 pr %p - cr %p\n", smat->pr, smat->cr);
    smat->cc32 = NULL;
    smat->cld  = smat->csz = smat->nz = smat->nc = 0;

    smat->csz = (smat->pld * st->nvars) + ni;
    smat->cr  = (hm_t **)calloc(
            (unsigned long)smat->csz,sizeof(hm_t *));

    /* allocate memory to store initial generators in pr and pc32 */
    smat->pc32 = realloc(smat->pc32,
            (unsigned long)(smat->pld + ni) * sizeof(cf32_t *));
}

static len_t get_number_of_initial_generators_in_next_degree(
        const bs_t * const in,
        const deg_t nd /* next degree */
        )
{
    len_t ctr = 0;
    for (int l = 0; l < in->ld; ++l) {
        printf("deg[%d] = %d\n", l, in->hm[l][DEG]);
    }

    int32_t i = in->ld-1;

    while (i >= 0 && in->hm[i][DEG] == nd) {
        ctr++;
        i--;
    }
    return ctr;
}

static void add_initial_generators(
        smat_t *smat,
        bs_t *in,
        const len_t ne
        )
{
    len_t j = 0;
    len_t i = in->ld-1;

    while (j < ne) {
        add_row_with_signature(smat, in, i);
        /* memory for coeffs vector is freed in sba
         * linear algebra later on */
        in->cf_32[in->hm[i][COEFFS]] = NULL;
        free(in->hm[i]);
        in->hm[i] = NULL;
        in->ld--;
        i--;
        j++;
    }
}

static void generate_next_degree_matrix_from_previous(
        smat_t *smat,
        const crit_t * const syz,
        crit_t *rew,
        ht_t *ht,
        stat_t *st
        )
{
    const len_t pld = smat->pld;
    for (len_t i = pld; i > 0 ; --i) {
        add_multiples_of_previous_degree_row(smat, i-1, syz, rew, ht, st);
        free(smat->pr[i-1]);
        smat->pr[i-1] = NULL;
    }
}

static void sba_final_reduction_step(
        bs_t *bs,
        ht_t **htp,
        hi_t *hcm,
        stat_t *st
        )
{
    ht_t *ht = *htp;

    /* prepare basis data to apply final reduction process */
    for (len_t i = 0; i < bs->ld; ++i) {
        bs->lm[i]   = ht->hd[bs->hm[i][SM_OFFSET]].sdm;
        bs->lmps[i] = i;
    }
    bs->lml = bs->ld;

    ht_t *sht  = initialize_secondary_hash_table(ht, st);
    mat_t *mat = (mat_t *)calloc(1, sizeof(mat_t));
    /* note: bht will become sht, and sht will become NULL,
     * thus we need pointers */
    reduce_basis(bs, mat, &hcm, &ht, &sht, st);
    if (sht != NULL) {
        free_hash_table(&sht);
    }
    free(mat);
    mat = NULL;

    *htp = ht;
}

static void free_sba_matrix(
        smat_t **smatp
        )
{
    smat_t *smat = *smatp;
    for (len_t i = 0; i < smat->csz; ++i) {
        free(smat->cr[i]);
        free(smat->cc32[i]);
        free(smat->pc32[i]);
    }
    free(smat);
    smat = NULL;
    *smatp = smat;
}

static void generate_next_degree_sba_matrix(
                smat_t *smat,
                bs_t *in,
                crit_t *syz,
                crit_t *rew,
                ht_t *ht,
                stat_t *st
                )
{
    /* check if we have initial generators not handled in lower degree
     * until now */
    const len_t ni = get_number_of_initial_generators_in_next_degree(
            in, smat->cd);
    /* prepare signature matrix for next degree */
    sba_prepare_next_degree(smat, in, ni, st);

    reset_signature_criteria(rew, st);

    /* generate rows from previous degree matrix, start with the highest
     * signatures in order to get an efficient rewrite criterion test */
    generate_next_degree_matrix_from_previous(
            smat, syz, rew, ht, st);

    add_initial_generators(smat, in, ni);
}

int core_sba_schreyer(
        bs_t **bsp,
        ht_t **htp,
        stat_t **stp
        )
{
    bs_t *in    = *bsp;
    ht_t *ht    = *htp;
    stat_t *st  = *stp;

    printf("in->ld %u | in->hm %p\n", in->ld, in->hm);
    /* timings for one round */
    double rrt0, rrt1;

    int try_termination = 0;

    /* hashes-to-columns map, initialized with length 1, is reallocated
     * in each call when generating matrices for linear algebra */
    hi_t *hcm = (hi_t *)malloc(sizeof(hi_t));

    /* signature matrix and previous degree signature matrix */
    smat_t *smat = calloc(1, sizeof(smat_t));
    /* initial degree is the lowest degree of the input generators */
    smat->cd = in->hm[in->ld-1][DEG];

    /* initialize signature related information */
    initialize_signatures_schreyer(in);
    printf("initial signatures\n");
    for (int j = 0; j < in->ld; ++j) {
        for (int i = 0; i < ht->evl; ++i) {
            printf("%u ", ht->ev[in->sm[j]][i]);
        }
        printf(" | %u --> %u\n", in->si[j], in->sm[j]);
    }
    crit_t *syz = initialize_syzygies_schreyer(in, ht);
    crit_t *rew = initialize_signature_criteria(st);

    /* initialize an empty basis for keeping the real basis elements */
    bs_t *bs = initialize_basis(st);

    /* sort initial elements, highest lead term first */
    sort_r(in->hm, (unsigned long)in->ld, sizeof(hm_t *),
            initial_input_cmp_sig, ht);

    if (st->info_level > 1) {
        printf("\ndeg     sel   pairs        mat          density \
                new data             time(rd)\n");
        printf("-------------------------------------------------\
                ----------------------------------------\n");
    }
    st->current_rd  =   0;

    while (!try_termination) {
        rrt0  = realtime();
        st->max_bht_size = st->max_bht_size > ht->esz ?
            st->max_bht_size : ht->esz;
        st->current_rd++;

        /* generate matrix for next degree step */
        generate_next_degree_sba_matrix(smat, in, syz, rew, ht, st);

        /* sort matrix rows by increasing signature */
        sort_matrix_rows_by_increasing_signature(smat, ht);
        for (int ii = 0; ii < smat->cld; ++ii) {
            printf("%u | %u \n", smat->cr[ii][SM_SMON], smat->cr[ii][SM_SIDX]);
        }

        /* map hashes to columns */
        sba_convert_hashes_to_columns(&hcm, smat, st, ht);

        /* s-reduce matrix and add syzygies when rows s-reduce to zero */
        sba_linear_algebra(smat, syz, st, ht);
        printf("0 cld %u %p\n", smat->cld, smat->cr);

        /* maps columns to hashes */
        sba_convert_columns_to_hashes(smat, hcm);

        /* reset indices in hash table*/
        reset_hash_table_indices(ht, hcm, smat->nc);

        printf("cld %u %p\n", smat->cld, smat->cr);

        /* add new elements to basis */
        smat->nlm = sba_add_new_elements_to_basis(smat, ht, bs, st);
        printf("2 cld %u %p\n", smat->cld, smat->cr);

        /* increase degree for next round */
        smat->cd++;

        if (st->info_level > 1) {
            printf("%7d new %7d zero", smat->nlm, smat->nz);
            fflush(stdout);
        }

        /* if we found a constant we are done, if we have added no new elements
         * we assume we are done*/
        if (bs->constant  == 1 || smat->nlm == 0) {
            try_termination =   1;
        }
        rrt1 = realtime();
        if (st->info_level > 1) {
            printf("%13.2f sec\n", rrt1-rrt0);
        }
    }
    /* Note: We cannot free all signature related data at this point, maybe
     * we terminated too early and need to further compute in higher degrees. */

    if (st->info_level > 1) {
        printf("-------------------------------------------------\
                ----------------------------------------\n");
    }
    /* fully reduce elements in basis. */
    if (st->reduce_gb == 1) {
        sba_final_reduction_step(bs, &ht, hcm, st);
    }


    *bsp    = bs;
    *htp    = ht;
    *stp    = st;

    /* free and clean up */
    free_sba_matrix(&smat);
    free_signature_criteria(&syz, st);
    free_signature_criteria(&rew, st);
    free(hcm);

    return 1;
}

