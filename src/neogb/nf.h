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


#ifndef GB_NF_H
#define GB_NF_H

#include "data.h"
#include "f4.h"

void get_normal_form_matrix(
        const bs_t * const tbr,
        ht_t * bht,
        const len_t start,
        md_t *st,
        ht_t **shtp,
        hi_t **hcmp,
        mat_t **matp
        );

bs_t *core_nf(
        bs_t *tbr,
        md_t *md,
        const exp_t * const mul,
        bs_t *bs,
        int32_t *errp
        );
#endif
