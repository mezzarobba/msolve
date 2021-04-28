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

#if HAVE_CONFIG_H
#include "../../config.h"
#endif

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<time.h>
#ifdef HAVE_OPENMP
#include<omp.h>
#endif

#define MIN(x, y) ((x) > (y) ? (y) : (x))

#define DEBUGFGLM 0

#include "data_fglm.c"
#include "linalg-fglm.c"
#include "berlekamp_massey.c"


void display_nmod_poly(FILE *file, nmod_poly_t pol){
  fprintf(file, "[%ld,\n", pol->length-1);
  if(pol->length != 0){
    fprintf(file, "[");
    for(long i = 0; i < pol->length - 1 ; i++){
      fprintf(file, "%lu, ", pol->coeffs[i]);
    }
    fprintf(file, "%lu]", pol->coeffs[pol->length - 1]);
  }
  else{
    fprintf(file, "[0]");
  }
  fprintf(file, "]");
}


void display_fglm_param(FILE * file, param_t *param){
  fprintf(file, "%ld\n", param->charac);
  fprintf(file, "%ld\n", param->nvars);

  display_nmod_poly(file, param->elim);
  fprintf(file, "\n");
  display_nmod_poly(file, param->denom);
  fprintf(file, "\n");

  for(int c = param->nvars-2; c >= 0; c--){
    display_nmod_poly(file, param->coords[c]);
    fprintf(file, "\n");
  }
}

void display_fglm_param_maple(FILE * file, param_t *param){
  fprintf(file, "[%ld, \n", param->charac);
  fprintf(file, "%ld, \n", param->nvars);

  display_nmod_poly(file, param->elim);
  fprintf(file, ", \n");
  display_nmod_poly(file, param->denom);
  fprintf(file, ", \n");

  for(int c = param->nvars-2; c > 0; c--){
    display_nmod_poly(file, param->coords[c]);
    fprintf(file, ", \n");
  }
  display_nmod_poly(file, param->coords[0]);
  fprintf(file, "]:\n");
}

/* Points of B are u_0, .., u_l */
/* They become u_l, ..., u_0 */

static inline void mirror_points(nmod_berlekamp_massey_t B, szmat_t length){
  CF_t tmp;
  szmat_t mid = length / 2;
  for(szmat_t i = 0; i < mid; i++){
    tmp = B->points->coeffs[i];
    B->points->coeffs[i] = B->points->coeffs[length - i - 1];
    B->points->coeffs[length - i - 1] = tmp;
  }
}

static inline void mirror_poly(nmod_poly_t out, nmod_poly_t in){
  szmat_t mid = in->length / 2;
  for(long i = 0; i <= mid; i++){
    out->coeffs[i] = in->coeffs[in->length - i - 1];
    out->coeffs[in->length - i - 1] = in->coeffs[i];
  }
  out->length=in->length;
}

static inline void mirror_poly_solve(nmod_poly_t out, nmod_poly_t in, szmat_t length){

  long i;
  long m = MIN(in->length, length);
  if(out->alloc < length){
    nmod_poly_fit_length(out, length);
  }
  out->length=length;
  for(i = 0; i < m; i++){
    out->coeffs[length - 1 - i] = in->coeffs[i];
  }
  for(;i<length; i++){
    out->coeffs[length-1-i] = 0;
  }
}



static inline void mirror_poly_inplace(nmod_poly_t in){
  CF_t tmp;
  szmat_t mid = in->length / 2;
  for(long i = 0; i < mid; i++){
    tmp = in->coeffs[i];
    in->coeffs[i] = in->coeffs[in->length - i - 1];
    in->coeffs[in->length - i - 1] = tmp;
  }
}



/*

  U is a Hankel matrix of size dim

  returns 0 when U is not invertible else it returns 1.
 */
static int invert_hankel_matrix(fglm_bms_data_t *data_bms, szmat_t deg){

  nmod_poly_one(data_bms->BMS->R0);
  nmod_poly_zero(data_bms->BMS->R1);
  nmod_poly_zero(data_bms->BMS->V0);
  nmod_poly_one(data_bms->BMS->V1);
  szmat_t dim = deg; //B->points->length / 2;
  data_bms->BMS->npoints = 0;

  data_bms->BMS->points->length = 2 * dim - 1;

  mirror_points(data_bms->BMS, data_bms->BMS->points->length);

  nmod_em_gcd(data_bms->BMS, 0);

  if(data_bms->BMS->R1->length-1 < dim-1){
    fprintf(stderr, "Singular matrix\n");
    return 0;
  }

  if(data_bms->BMS->V1->coeffs[0]!=0){
    //Compute Z1 = LC(R1)^{-1} * V1

    mp_limb_t inv = n_invmod(data_bms->BMS->R1->coeffs[data_bms->BMS->R1->length-1],
                             (data_bms->BMS->R1->mod).n);
    nmod_poly_scalar_mul_nmod(data_bms->Z1, data_bms->BMS->V1, inv);

    mirror_points(data_bms->BMS, data_bms->BMS->points->length);
    nmod_poly_one(data_bms->BMS->R0); //x^(2dim -1)
    nmod_poly_zero(data_bms->BMS->R1);
    nmod_poly_zero(data_bms->BMS->V0);
    nmod_poly_one(data_bms->BMS->V1);
    data_bms->BMS->npoints = 0;

    //(R_i, R_{i+1}, V_i, V_{i+1}) = EMGCD(R_0, R_1)
    nmod_em_gcd(data_bms->BMS, 0);
    //Z2 = LC(R_{i+1})^{-1} x (V_{i+1})
    inv = n_invmod(data_bms->BMS->R1->coeffs[data_bms->BMS->R1->length-1],
                   (data_bms->BMS->R1->mod).n);
    nmod_poly_scalar_mul_nmod(data_bms->Z2, data_bms->BMS->V1, inv);

  }
  else{//V1(0) = 0
    fprintf(stderr, "Warning: this part of the code has not been tested intensively\n");
    nmod_poly_one(data_bms->BMS->R0);
    nmod_poly_zero(data_bms->BMS->R1);
    nmod_poly_zero(data_bms->BMS->V0);
    nmod_poly_one(data_bms->BMS->V1);
    data_bms->BMS->npoints = 0;

    data_bms->BMS->points->length = 2 * dim + 1;

    long queue_lo = (data_bms->BMS->npoints);
    long queue_len = (data_bms->BMS->points->length) - queue_lo;
    nmod_poly_zero(data_bms->BMS->rt);
    nmod_poly_set_coeff_ui(data_bms->BMS->rt, queue_len+1, 1);
    for (long i = 0; i < queue_len; i++)
      {
        nmod_poly_set_coeff_ui(data_bms->BMS->rt, queue_len - i,
                               data_bms->BMS->points->coeffs[queue_lo + i]);
      }
    nmod_poly_set_coeff_ui(data_bms->BMS->rt, 0, 1);

    nmod_em_gcd_preinstantiated(data_bms->BMS, 0);//B->rt is pre-instantied

    if(data_bms->BMS->R1->length-1 == dim){
      mp_limb_t inv = n_invmod(data_bms->BMS->R1->coeffs[data_bms->BMS->R1->length-1],
                               (data_bms->BMS->R1->mod).n);
      nmod_poly_scalar_mul_nmod(data_bms->Z1, data_bms->BMS->V1, inv);


      nmod_poly_set_coeff_ui(data_bms->BMS->rt, queue_len+1, 1);
      for (long i = 0; i < queue_len; i++)
        {
          nmod_poly_set_coeff_ui(data_bms->BMS->rt, i + 1,
                                 data_bms->BMS->points->coeffs[queue_lo + i]);
        }
      nmod_poly_set_coeff_ui(data_bms->BMS->rt, 0, 1);

      nmod_poly_one(data_bms->BMS->R0); //x^(2dim +1)
      nmod_poly_zero(data_bms->BMS->R1);
      nmod_poly_zero(data_bms->BMS->V0);
      nmod_poly_one(data_bms->BMS->V1);
      data_bms->BMS->npoints = 0;

      //(R_i, R_{i+1}, V_i, V_{i+1}) = EMGCD(R_0, R_1)
      nmod_em_gcd_preinstantiated(data_bms->BMS, 0);
      //Z2 = LC(R_{i+1})^{-1} x (V_{i+1})
      inv = n_invmod(data_bms->BMS->R1->coeffs[data_bms->BMS->R1->length-1],
                     (data_bms->BMS->R1->mod).n);
      nmod_poly_scalar_mul_nmod(data_bms->Z2, data_bms->BMS->V1, inv);
      fprintf(stderr, "Something should be checked\n");
      return 1;
    }
    else{
      nmod_poly_one(data_bms->BMS->R0);
      nmod_poly_zero(data_bms->BMS->R1);
      nmod_poly_zero(data_bms->BMS->V0);
      nmod_poly_one(data_bms->BMS->V1);
      data_bms->BMS->npoints = 0;

      data_bms->BMS->points->length = 2 * dim + 1;

      queue_lo = (data_bms->BMS->npoints);
      queue_len = (data_bms->BMS->points->length) - queue_lo;
      nmod_poly_zero(data_bms->BMS->rt);
      nmod_poly_set_coeff_ui(data_bms->BMS->rt, queue_len+1, data_bms->BMS->R1->mod.n - 1);
      for (long i = 0; i < queue_len; i++)
        {
          nmod_poly_set_coeff_ui(data_bms->BMS->rt, queue_len - i,
                                 data_bms->BMS->points->coeffs[queue_lo + i]);
        }
      nmod_poly_set_coeff_ui(data_bms->BMS->rt, 0, 1);

      nmod_em_gcd_preinstantiated(data_bms->BMS, 0);//B->rt is pre-instantied

      if(data_bms->BMS->R1->length-1 == dim){
        mp_limb_t inv = n_invmod(data_bms->BMS->R1->coeffs[data_bms->BMS->R1->length-1],
                                 (data_bms->BMS->R1->mod).n);
        nmod_poly_scalar_mul_nmod(data_bms->Z1, data_bms->BMS->V1, inv);


        nmod_poly_set_coeff_ui(data_bms->BMS->rt, queue_len+1, 1);
        for (long i = 0; i < queue_len; i++)
          {
            nmod_poly_set_coeff_ui(data_bms->BMS->rt, i + 1,
                                   data_bms->BMS->points->coeffs[queue_lo + i]);
          }
        nmod_poly_set_coeff_ui(data_bms->BMS->rt, 0, data_bms->BMS->R1->mod.n - 1);

        nmod_poly_one(data_bms->BMS->R0); //x^(2dim +1)
        nmod_poly_zero(data_bms->BMS->R1);
        nmod_poly_zero(data_bms->BMS->V0);
        nmod_poly_one(data_bms->BMS->V1);
        data_bms->BMS->npoints = 0;

        //(R_i, R_{i+1}, V_i, V_{i+1}) = EMGCD(R_0, R_1)
        nmod_em_gcd_preinstantiated(data_bms->BMS, 0);
        //Z2 = LC(R_{i+1})^{-1} x (V_{i+1})
        inv = n_invmod(data_bms->BMS->R1->coeffs[data_bms->BMS->R1->length-1],
                       (data_bms->BMS->R1->mod).n);
        nmod_poly_scalar_mul_nmod(data_bms->Z2, data_bms->BMS->V1, inv);
        fprintf(stderr, "Something should be checked\n");
        return 1;
      }
      else{
        fprintf(stderr, "There should be a bug here (invert_hankel)\n");
        return 0;
      }
    }
  }
  return 1;
}


/*

  Z1 and Z2 must be arrays of length d + 1
  Mirroring them will give an array of length d + 1

*/


static inline void solveHankel(nmod_poly_t param,
                               nmod_poly_t Z1, nmod_poly_t Z2,
                               nmod_poly_t rZ1, nmod_poly_t rZ2,
                               nmod_poly_t A, nmod_poly_t B,
                               szmat_t dimquot,
                               szmat_t dim,
                               CF_t *res,
                               int ncoord,
                               nmod_poly_t V,
                               long disp){

  V->length = dim;

  for(long i = 0; i < dim; i++){
    V->coeffs[i] = res[ncoord-1+i*(dimquot)];
  }

  #if DEBUGFGLM > 0
  fprintf(stdout, "\n ncoord = %d\n", ncoord);
  fprintf(stdout, "V = "); nmod_poly_fprint_pretty(stdout, V, "x");fprintf(stdout, "\n\n");
  #endif
  mirror_poly_inplace(V);
  mirror_poly_solve(rZ1, Z1, dim + 1);

  mirror_poly_solve(rZ2, Z2, dim + 1);

  nmod_poly_mullow(A, rZ1, V, dim); // mod t^dim

  nmod_poly_mullow(B, Z2, V, dim); // mod t^dim

  mirror_poly_inplace(B);
  mirror_poly_inplace(A);
  nmod_poly_mullow(rZ1, Z1, B, dim);

  nmod_poly_mullow(rZ2, rZ2, A, dim);
  nmod_poly_neg(rZ2, rZ2);

  nmod_poly_add(param, rZ1, rZ2);


  mp_limb_t inv = n_invmod(Z1->coeffs[0], (Z1->mod).n);

  nmod_poly_scalar_mul_nmod(param, param, inv);

}


/*
 Z1 and Z2 must be arrays of length d + 1
 Mirroring them will give an array of length d + 1
 */

static inline void solve_hankel(fglm_bms_data_t *data_bms, 
                                szmat_t dimquot,
                                szmat_t dim,
                                szmat_t block_size,
                                CF_t *res,
                                int ncoord){
  data_bms->V->length = dim;

  for(long i = 0; i < dim; i++){
    data_bms->V->coeffs[i] = res[ncoord-1+i*(block_size)];
  }

  #if DEBUGFGLM > 0
  fprintf(stdout, "\n ncoord = %d\n", ncoord);
  fprintf(stdout, "V = ");
  nmod_poly_fprint_pretty(stdout, data_bms->V, "x");
  fprintf(stdout, "\n");
  #endif

  mirror_poly_inplace(data_bms->V);
  mirror_poly_solve(data_bms->rZ1, data_bms->Z1, dim + 1);
  mirror_poly_solve(data_bms->rZ2, data_bms->Z2, dim + 1);

  nmod_poly_mullow(data_bms->A, data_bms->rZ1, data_bms->V, dim); // mod t^dim
  nmod_poly_mullow(data_bms->B, data_bms->Z2, data_bms->V, dim); // mod t^dim

  mirror_poly_solve(data_bms->rZ1, data_bms->B, dim); 

  for(szmat_t i = 0; i < dim ; i++){
    data_bms->B->coeffs[i] = data_bms->rZ1->coeffs[i];
  }
  data_bms->B->length = data_bms->rZ1->length;
  mirror_poly_solve(data_bms->rZ1, data_bms->A, dim); 
  for(szmat_t i = 0; i < dim ; i++){
    data_bms->A->coeffs[i] = data_bms->rZ1->coeffs[i];
  }
  data_bms->A->length = data_bms->rZ1->length;

  nmod_poly_mullow(data_bms->rZ1, data_bms->Z1, data_bms->B, dim);
  nmod_poly_mullow(data_bms->rZ2, data_bms->rZ2, data_bms->A, dim);

  nmod_poly_neg(data_bms->rZ2, data_bms->rZ2);

  nmod_poly_add(data_bms->param, data_bms->rZ1, data_bms->rZ2);

  mp_limb_t inv = n_invmod(data_bms->Z1->coeffs[0], (data_bms->Z1->mod).n);

  nmod_poly_scalar_mul_nmod(data_bms->param, data_bms->param, inv);

}


/*

Matrix vector product.
Result is stored in res.
vres will contain the product of the dense part.

 */
static inline void sparse_mat_fglm_mult_vec(CF_t *res, sp_matfglm_t *mat,
                                            CF_t *vec,
                                            CF_t *vres,
                                            const mod_t prime,
                                            //cf_l_t *vec_cache, //obsolete
                                            const uint32_t RED_32,
                                            const uint64_t RED_64,
                                            const uint32_t preinv,
                                            const uint32_t pi1,
                                            const uint32_t pi2){

  szmat_t ncols = mat->ncols;
  szmat_t nrows = mat->nrows;
  szmat_t ntriv = ncols - nrows;
  for(szmat_t i = 0; i < ntriv; i++){
    res[mat->triv_idx[i]] = vec[mat->triv_pos[i]];
  }
  /* printf("ncols %u\n", ncols); */
#ifdef HAVE_AVX2
  /* matrix_vector_product(vres, mat->dense_mat, vec, ncols, nrows, prime, RED_32, RED_64); */
  _8mul_matrix_vector_product(vres, mat->dense_mat, vec, mat->dst,
                              ncols, nrows, prime, RED_32, RED_64);
#else
  non_avx_matrix_vector_product(vres, mat->dense_mat, vec,
                              ncols, nrows, prime, RED_32, RED_64);
#endif
    for(szmat_t i = 0; i < nrows; i++){
      res[mat->dense_idx[i]] = vres[i];
    }
}




/**

   FGLM mod prime.
   All needed intermediate data are allocated inside the function.
   Useful only for first call.

 **/


#ifdef DEBUGFGLM
static inline void print_vec(FILE *file, CF_t *vec, szmat_t len){
  fprintf(file, "[");
  for(szmat_t i = 0; i < len-1; ++i){
    fprintf(file, "%u, ", vec[i]);
  }
  fprintf(file, "%u]\n",vec[len-1]);
}

static inline void mynmod_berlekamp_massey_print(
                                                 const nmod_berlekamp_massey_t B)
{
  slong i;
  nmod_poly_fprint_pretty(stderr, B->V1, "x");
  fprintf(stderr, ", ");
  for (i = 0; i < B->points->length; i++)
    {
      fprintf(stderr, " %lu", B->points->coeffs[i]);
    }
}

static inline void mynmod_berlekamp_massey_print_poly(FILE *file,
                                               const nmod_berlekamp_massey_t B)
{
  slong i;
  fprintf(file, "%lu\n", B->V1->mod.n);
  for(i = 0; i < B->V1->length; ++i ){
    fprintf(file, "%lu ", (B->V1->coeffs)[i]);
  }
}
#endif

static inline void generate_sequence(sp_matfglm_t *matrix, fglm_data_t * data,
                                     szmat_t block_size, long dimquot,
                                     mod_t prime){
  uint32_t RED_32 = ((uint64_t)2<<31) % prime;

#if DEBUGFGLM > 2
  fprintf(stderr, "RED_32 = %u\n", RED_32);
#endif

  uint32_t RED_64 = ((uint64_t)1<<63) % prime;
  RED_64 = (RED_64*2) % prime;

#if DEBUGFGLM > 2
  fprintf(stderr, "RED_64 = %u\n", RED_64);
#endif

  uint32_t preinv = 2^(62) % prime;
  uint32_t pi1 = ((uint64_t)pow(2, 32)) / RED_64;
  uint32_t pi2 = (uint64_t)pow(2, 32) / RED_32;

  for(szmat_t i = 1; i < matrix->ncols; i++){
    sparse_mat_fglm_mult_vec(data->vvec, matrix,
                             data->vecinit, data->vecmult,
                             prime, RED_32, RED_64, preinv, pi1, pi2);
#if DEBUGFGLM > 1
    print_vec(stderr, data->vvec, matrix->ncols);
#endif


    CF_t *tmp = data->vecinit;
    data->vecinit = data->vvec;
    data->vvec = tmp;
    data->res[i*block_size] = data->vecinit[0];

    for(szmat_t j = 1; j < block_size; j++){
      data->res[j+i*block_size] = data->vecinit[j+1];
    }

#if DEBUGFGLM > 1
    print_vec(stdout, data->res, 2*block_size * matrix->ncols);
#endif

#if DEBUGFGLM > 2
    fprintf(stderr, "res = ");
    print_vec(stdout, data->res+i*matrix->ncols, matrix->ncols);
#endif
  }
  for(szmat_t i = matrix->ncols; i < 2*matrix->ncols; i++){
    sparse_mat_fglm_mult_vec(data->vvec, matrix,
                             data->vecinit, data->vecmult,
                             prime, RED_32, RED_64, preinv, pi1, pi2);
#if DEBUGFGLM > 1
    print_vec(stderr, data->vvec, matrix->ncols);
#endif


    CF_t *tmp = data->vecinit;
    data->vecinit = data->vvec;
    data->vvec = tmp;
    data->res[i*block_size] = data->vecinit[0];

#if DEBUGFGLM > 1
    print_vec(stdout, data->res, 2*block_size * matrix->ncols);
#endif

#if DEBUGFGLM > 2
    fprintf(stderr, "res = ");
    print_vec(stdout, data->res+i*matrix->ncols, matrix->ncols);
#endif
  }

  /* now res contains our generating sequence */

  for(ulong i = 0; i < 2 * dimquot; i++){
    data->pts[i] = data->res[i*block_size];
  }

}

static void generate_sequence_verif(sp_matfglm_t *matrix, fglm_data_t * data,
					   szmat_t block_size, long dimquot,
					   uint64_t* squvars,
					   uint64_t* linvars,
					   long nvars,
					   mod_t prime){
  uint32_t RED_32 = ((uint64_t)2<<31) % prime;


  uint32_t RED_64 = ((uint64_t)1<<63) % prime;
  RED_64 = (RED_64*2) % prime;

  uint32_t preinv = 2^(62) % prime;
  uint32_t pi1 = ((uint64_t)pow(2, 32)) / RED_64;
  uint32_t pi2 = (uint64_t)pow(2, 32) / RED_32;

  long dec= 0;
  for(szmat_t j = 1; j < block_size; j++){
    while (nvars-1-j-dec > 0 && linvars[nvars-1-j-dec] != 0) {
      dec++;
    }
    data->res[j+matrix->ncols*block_size]
      = data->vecinit[squvars[nvars-1-j-dec]];
  }
  for(szmat_t i = 1; i < matrix->ncols; i++){
    sparse_mat_fglm_mult_vec(data->vvec, matrix,
                             data->vecinit, data->vecmult,
                             prime, RED_32, RED_64, preinv, pi1, pi2);
#if DEBUGFGLM > 1
    print_vec(stderr, data->vvec, matrix->ncols);
#endif


    CF_t *tmp = data->vecinit;
    data->vecinit = data->vvec;
    data->vvec = tmp;
    data->res[i*block_size] = data->vecinit[0];

    dec= 0;
    for(szmat_t j = 1; j < block_size; j++){
      data->res[j+i*block_size] = data->vecinit[j+1];
      while (linvars[nvars-1-j-dec] != 0) {
	dec++;
      }
      data->res[j+(i+matrix->ncols)*block_size]
	= data->vecinit[squvars[nvars-1-j-dec]];
    }

#if DEBUGFGLM > 1
    print_vec(stdout, data->res, 2*block_size * matrix->ncols);
#endif

#if DEBUGFGLM > 2
    fprintf(stderr, "res = ");
    print_vec(stdout, data->res+i*matrix->ncols, matrix->ncols);
#endif
  }
  for(szmat_t i = matrix->ncols; i < 2*matrix->ncols; i++){
    sparse_mat_fglm_mult_vec(data->vvec, matrix,
                             data->vecinit, data->vecmult,
                             prime, RED_32, RED_64, preinv, pi1, pi2);
#if DEBUGFGLM > 1
    print_vec(stderr, data->vvec, matrix->ncols);
#endif


    CF_t *tmp = data->vecinit;
    data->vecinit = data->vvec;
    data->vvec = tmp;
    data->res[i*block_size] = data->vecinit[0];

#if DEBUGFGLM > 1
    print_vec(stdout, data->res, 2*block_size * matrix->ncols);
#endif

#if DEBUGFGLM > 2
    fprintf(stderr, "res = ");
    print_vec(stdout, data->res+i*matrix->ncols, matrix->ncols);
#endif
  }

  /* now res contains our generating sequence */

  for(ulong i = 0; i < 2 * dimquot; i++){
    data->pts[i] = data->res[i*block_size];
  }

}



static inline void compute_elim_poly(fglm_data_t *data,
                                     fglm_bms_data_t *data_bms,
                                     long dimquot){
    nmod_berlekamp_massey_add_points(data_bms->BMS, data->pts, 2*dimquot);

    nmod_berlekamp_massey_reduce(data_bms->BMS);
    nmod_poly_make_monic(data_bms->BMS->V1, data_bms->BMS->V1);
}


static inline long make_square_free_elim_poly(param_t *param,
                                              fglm_bms_data_t *data_bms,
                                              long dimquot,
                                              int info_level){
  long dim = data_bms->BMS->V1->length - 1;
  int boo = nmod_poly_is_squarefree(data_bms->BMS->V1);

  if(boo && dim == dimquot){
    nmod_poly_set(param->elim, data_bms->BMS->V1);
    if(info_level){
      fprintf(stderr, "The ideal is radical: %ld solutions\n", dim);
    }
  }
  else{

    if(boo==0){
      if(info_level){
        fprintf(stderr, "Mininimal polynomial is not square-free\n");
      }
    }
    nmod_poly_factor_squarefree(data_bms->sqf, data_bms->BMS->V1);
    nmod_poly_one(param->elim);
    for(ulong i = 0; i < data_bms->sqf->num; i++){
      nmod_poly_mul(param->elim, param->elim, data_bms->sqf->p+i);
    }
    if(info_level){
      fprintf(stderr, "The ideal is not radical: %ld solutions\n", param->elim->length-1);
      fprintf(stderr, "[%ld, %ld, %ld]\n", dimquot, dim, param->elim->length - 1);
    }
  }

  data_bms->sqf->num=0;
  return param->elim->length - 1;
}

static inline void compute_minpoly(param_t *param,
                                   fglm_data_t *data,
                                   fglm_bms_data_t *data_bms,
                                   long dimquot,
                                   uint64_t *linvars,
                                   uint32_t *lineqs,
                                   long nvars,
                                   long *dim,
                                   int info_level){
  compute_elim_poly(data, data_bms, dimquot);

  if(dimquot > 1){
    *dim = make_square_free_elim_poly(param, data_bms, dimquot, info_level);
  }
  else{
    nmod_poly_fit_length(param->elim, 2);
    param->elim->length = 2;
    param->elim->coeffs[1] = 1;
    param->elim->coeffs[0] = lineqs[nvars*(nvars+1)-1];
    *dim = 1;
  }

}
static void set_param_linear_vars(param_t *param,
                                  long nlins,
                                  uint64_t *linvars,
                                  uint32_t *lineqs,
                                  long nvars){


  int cnt = 1;
  const uint32_t fc = param->charac;
  /* fprintf(stderr, "\n"); */
  /* fprintf(stderr, "nvars = %ld\n", nvars); */
  /* fprintf(stderr, "nlins = %ld\n", nlins); */
  /* for(int i = 0; i < nlins; i++){ */
  /*   for(int j = 0; j < nvars + 1; j++){ */
  /*     fprintf(stderr, "%d, ", lineqs[j + i*(nvars+1)]); */
  /*   } */
  /*   fprintf(stderr, "\n"); */
  /* } */

  int nr = 0;
  if(nlins==nvars){
    nr = nvars - 1;
    param->elim->length = 2;
    param->elim->coeffs[1] = 1;
    param->elim->coeffs[0] = lineqs[nlins*(nvars + 1) - 1];
  }
  else{
    nr = nlins;
  }
  /* for(int nc = 0; nc <= nvars-2; nc++){ */
  for(int nc = nvars - 2; nc >= 0; nc--){
    int ind = nvars - 2 - nc;
    ind = nc;
    if(linvars[nc] != 0){
      int64_t lc = lineqs[nc +(nvars+1)*(nr-1 - (cnt-1))];
      if(lc != 1){
        fprintf(stderr, "There should be a bug\n");
        exit(1);
      }
      for(int k = nc + 1; k < nvars - 1; k++){
        int32_t c = lineqs[k+(nvars+1)*(nr-(cnt-1)-1)];
        if(c){
          for(int i = 0; i < param->coords[k]->length; i++){
            int64_t tmp = (fc-c) * param->coords[k]->coeffs[i];
            tmp = tmp % fc;
            tmp += param->coords[ind]->coeffs[i];
            tmp = tmp % fc;
            param->coords[ind]->coeffs[i] = tmp;
          }
        }
      }

      int32_t c1 = lineqs[nvars-1+(nvars+1)*(nr-(cnt-1)-1)];
      param->coords[ind]->coeffs[1] = ((int64_t)(param->coords[ind]->coeffs[1] + c1)) % fc;

      int32_t c0 = lineqs[nvars+(nvars+1)*(nr-(cnt-1)-1)];
      param->coords[ind]->coeffs[0] = ((int64_t)(param->coords[ind]->coeffs[0] + c0)) % fc;

      for(long k = param->coords[ind]->length - 1; k >= 0; k--){
        if(param->coords[ind]->coeffs[k] == 0){
          param->coords[ind]->length--;
        }
        else{
          break;
        }
      }

      nmod_poly_rem(param->coords[ind], param->coords[ind],
                    param->elim);

      cnt++;
    }
  }

#if DEBUGFGLM >0
  display_fglm_param(stderr, param);
#endif
}

static int compute_parametrizations(param_t *param,
                                    fglm_data_t *data,
                                    fglm_bms_data_t *data_bms,
                                    long dim,
                                    long dimquot,
                                    long block_size,
                                    long nlins,
                                    uint64_t *linvars,
                                    uint32_t *lineqs,
                                    long nvars){
  nmod_poly_one(param->denom);

  if(invert_hankel_matrix(data_bms, dim)){

#if DEBUGFGLM > 0
    fprintf(stdout, "Z1 = "); nmod_poly_fprint_pretty(stdout, data_bms->Z1, "x");fprintf(stdout, "\n");
    fprintf(stdout, "Z2 = "); nmod_poly_fprint_pretty(stdout, data_bms->Z2, "x");fprintf(stdout, "\n");
#endif
    long dec = 0;
    for(long nc = 0; nc < nvars - 1 ; nc++){
      if(linvars[nvars - 2- nc] == 0){
        solve_hankel(data_bms, dimquot, dim, block_size, data->res,
                     nc + 2 - dec);

        nmod_poly_neg(data_bms->param, data_bms->param);
        nmod_poly_reverse(param->coords[nvars-2-nc], data_bms->param, dim);
        nmod_poly_rem(param->coords[nvars-2-nc], param->coords[nvars-2-nc],
                      param->elim);

#if DEBUGFGLM > 0
        nmod_poly_fprint_pretty(stdout, param->coords[nvars-2-nc], "X");
        fprintf(stdout, "\n");
#endif
      }
      else{
        if(param->coords[nvars-2-nc]->alloc <  param->elim->alloc - 1){
          nmod_poly_fit_length(param->coords[nvars-2-nc],
                               param->elim->alloc );
        }
        param->coords[nvars-2-nc]->length = param->elim->length ;
        for(long i = 0; i < param->elim->length ; i++){
          param->coords[nvars-2-nc]->coeffs[i] = 0;
        }
        dec++;
      }
    }

    set_param_linear_vars(param, nlins, linvars, lineqs, nvars);

#if DEBUGFGLM > 0
    fprintf(stderr, "PARAM = ");
    display_fglm_param_maple(stderr, param);
    fprintf(stderr, "\n");
#endif
    return 1;
  }
  else{
    return 0;
  }
}

static inline int invert_table_polynomial (param_t *param,
					   fglm_data_t *data,
					   fglm_bms_data_t *data_bms,
					   ulong dimquot,
					   szmat_t block_size,
					   mod_t prime,
					   int ncoord,
					   uint64_t lambda) {

  szmat_t length= data_bms->BMS->V1->length-1;
  nmod_poly_zero(data_bms->BMS->R0);
  nmod_poly_zero(data_bms->Z1);
  nmod_poly_zero(data_bms->Z2);
  nmod_poly_fit_length(data_bms->BMS->R0, length);
  nmod_poly_fit_length(data_bms->BMS->R0, length);

  for (long i = 0; i < length; i++){

    if (lambda == 0) {
      nmod_poly_set_coeff_ui (data_bms->BMS->R0,i,
			      data->res[(length-i-1)*block_size+ncoord]);

    }
    else {
      uint64_t coeff= (lambda*data->res[(length-i-1)*block_size]) % prime;
      coeff= (data->res[(length-i-1)*block_size+ncoord] + coeff) % prime;
      nmod_poly_set_coeff_ui (data_bms->BMS->R0,i,
			      coeff);
    }

  }

  nmod_poly_mul (data_bms->Z1,data_bms->BMS->R0,data_bms->BMS->V1);
  nmod_poly_shift_right (data_bms->Z1,data_bms->Z1,length);
  nmod_poly_xgcd (data_bms->BMS->R0,data_bms->BMS->R1,data_bms->Z2,param->elim,
		  data_bms->Z1);

  return (nmod_poly_degree (data_bms->BMS->R0) == 0);
}


static inline void divide_table_polynomials (param_t *param,
					     fglm_data_t *data,
					     fglm_bms_data_t *data_bms,
					     ulong dimquot,
					     szmat_t block_size,
					     mod_t prime,
					     int ncoord,
					     uint64_t lambda) {

  szmat_t length= data_bms->BMS->V1->length-1;
  nmod_poly_zero (data_bms->BMS->R0);
  nmod_poly_fit_length(data_bms->BMS->R0, length);

  for (long i = 0; i < length; i++){
    if (lambda == 0) {
      nmod_poly_set_coeff_ui (data_bms->BMS->R0,i,
			      data->res[(length-i-1)*block_size+ncoord]);

    }
    else {
      uint64_t coeff= (lambda*data->res[(length-i-1)*block_size+ncoord]) % prime;
      coeff= (data->res[(dimquot+length-i-1)*block_size+ncoord] + coeff) % prime;
      nmod_poly_set_coeff_ui (data_bms->BMS->R0,i,
			      coeff);
    }
  }

  nmod_poly_mul (data_bms->BMS->R1,data_bms->BMS->R0,data_bms->BMS->V1);
  nmod_poly_shift_right (data_bms->BMS->R1,data_bms->BMS->R1,length);
  nmod_poly_mul (data_bms->BMS->R1,data_bms->BMS->R1,data_bms->Z2);
  nmod_poly_rem (data_bms->BMS->R1,data_bms->BMS->R1,param->elim);

}



static
int compute_parametrizations_non_shape_position_case(param_t *param,
                                                     fglm_data_t *data,
                                                     fglm_bms_data_t *data_bms,
                                                     ulong dimquot,
                                                     szmat_t block_size,
                                                     long nlins,
                                                     uint64_t *linvars,
                                                     uint32_t *lineqs,
                                                     uint64_t *squvars,
                                                     long nvars,
                                                     mod_t prime,
                                                     int verif){
  int nr_fail_param=-1;
  if (invert_table_polynomial (param, data, data_bms, dimquot, block_size,
                               prime, 0, 0)) {
#if DEBUGFGLM > 0
    fprintf (stdout,"C1=");
    nmod_poly_fprint_pretty (stdout, data_bms->Z1, "x"); fprintf (stdout,"\n");
    fprintf(stdout, "invC1=");
    nmod_poly_fprint_pretty (stdout, data_bms->Z2, "x"); fprintf (stdout,"\n");
#endif
    long dec = 0;
    for(long nc = 0; nc < nvars - 1 ; nc++){
      if(linvars[nvars - 2 - nc] == 0){
        divide_table_polynomials(param,data,data_bms, dimquot, block_size, prime,
                                 nc + 1-dec,0);
        nmod_poly_neg(param->coords[nvars-2-nc], data_bms->BMS->R1);
#if DEBUGFGLM > 0
        nmod_poly_fprint_pretty(stdout, param->coords[nvars-2-nc], "X");
        fprintf(stdout, "\n");
#endif
      }
      else{
        dec++;
      }
    }
    /* parametrizations verification */
    if (verif) {
      dec = 0;
      for(long nc = 0; nc < nvars - 1 ; nc++){
        if(linvars[nvars - 2 - nc] == 0
           && squvars[nvars - 2 - nc] != 0){
          uint64_t lambda= 1 + ((uint64_t) rand() % (prime-1));
          /* needed for verification */

          invert_table_polynomial (param, data, data_bms, dimquot, block_size,
                                   prime, nc+1-dec, lambda);
#if DEBUGFGLM > 1
          fprintf (stdout,"C2=");
          nmod_poly_fprint_pretty (stdout, data_bms->Z1, "x"); fprintf (stdout,"\n");
          fprintf(stdout, "invC2=");
          nmod_poly_fprint_pretty (stdout, data_bms->Z2, "x"); fprintf (stdout,"\n");
#endif

          divide_table_polynomials(param,data,data_bms, dimquot, block_size,
                                   prime, nc+1-dec,lambda);
          nmod_poly_neg(data_bms->BMS->R1, data_bms->BMS->R1);

#if DEBUGFGLM > 1
          nmod_poly_fprint_pretty(stdout, data_bms->BMS->R1, "X");
          fprintf(stdout, "\n");
#endif
          if (! nmod_poly_equal (param->coords[nvars-2-nc],data_bms->BMS->R1)) {
            if (nr_fail_param == -1) {
              nr_fail_param= nvars-2-nc;
            }
          }
        }
        else{
          if(param->coords[nvars-2-nc]->alloc <  param->elim->alloc - 1){
            nmod_poly_fit_length(param->coords[nvars-2-nc],
                                 param->elim->alloc );
          }
          param->coords[nvars-2-nc]->length = param->elim->length ;
          for(long i = 0; i < param->elim->length ; i++){
            param->coords[nvars-2-nc]->coeffs[i] = 0;
          }
          dec++;
        }
      }
    }

    set_param_linear_vars(param, nlins, linvars, lineqs, nvars);

    /* display_fglm_param_maple(stderr, param); */
    return nvars-1-nr_fail_param;
  } else {
    return 0;
  }
}



static inline long initialize_fglm_data(sp_matfglm_t *matrix,
                                        fglm_data_t *data,
                                        const mod_t prime,
                                        const long sz,
                                        const szmat_t block_size){
  long nb = 0;
  for(long i = 0; i < sz; i++){
    if(matrix->dense_mat[i]==0)
      nb++;
  }
  srand(time(NULL));
  for(szmat_t i = 0; i < matrix->ncols; i++){
    data->vecinit[i] = (CF_t)rand() % prime;
    data->vecinit[i] += (CF_t)rand() % prime;
    /* data->vecinit[i] = (CF_t)(i+1) % prime; */
  }
  data->res[0] = data->vecinit[0];
  for(szmat_t i = 1; i < block_size; i++){
    data->res[i] = data->vecinit[i+1];
  }

  return nb;
}

param_t *nmod_fglm_compute(sp_matfglm_t *matrix, const mod_t prime, const long nvars,
                           const long nlins,
                           uint64_t *linvars,
                           uint32_t *lineqs,
                           uint64_t *squvars,
                           const int info_level){
#if DEBUGFGLM > 0
  fprintf(stderr, "prime = %u\n", prime);
#endif

#if DEBUGFGLM >= 1
  FILE *fmat = fopen("/tmp/matrix.fglm", "w");
  display_fglm_matrix(fmat, matrix);
  fclose(fmat);
#endif
  /* 1147878294 */
    if(prime>=1518500213){
    fprintf(stderr, "Prime %u is too large.\n", prime);
    fprintf(stderr, "One needs to use update linear algebra fglm functions\n");
    return NULL;
  }

  szmat_t block_size = nvars-nlins;

  fglm_data_t *data = allocate_fglm_data(matrix->nrows, matrix->ncols, nvars);

  param_t *param = allocate_fglm_param(prime, nvars);

  long sz = matrix->ncols * matrix->nrows;
  long nb = initialize_fglm_data(matrix, data, prime, sz, block_size);

  if(info_level){
    fprintf(stderr, "[%u, %u], Non trivial / Trivial = %.2f%%\n",
            matrix->ncols, matrix->nrows,
            100*((double)matrix->nrows / (double)matrix->ncols));
    fprintf(stderr, "Density of non-trivial part %.2f%%\n",
            100-100*(float)nb/(float)sz);
  }

  ulong dimquot = (matrix->ncols);

#if DEBUGFGLM > 1
  print_vec(stderr, data->vecinit, matrix->ncols);
  print_vec(stderr, data->res, 2 * block_size * matrix->ncols);
  fprintf(stderr, "\n");
#endif

  double st = omp_get_wtime();


  /* generate_sequence(matrix, data, block_size, dimquot, prime); */
  generate_sequence_verif(matrix, data, block_size, dimquot,
			  squvars, linvars, nvars, prime);

  if(info_level > 1){
    double nops = 2 * (matrix->nrows/ 1000.0) * (matrix->ncols / 1000.0)  * (matrix->ncols / 1000.0);
    double rt = omp_get_wtime()-st;
    fprintf(stderr, "Time spent to generate sequence (elapsed): %.2f sec (%.2f Gops/sec)\n", rt, nops / rt);
  }

  st = omp_get_wtime();

  /* Berlekamp-Massey data */
  fglm_bms_data_t *data_bms = allocate_fglm_bms_data(dimquot, prime);

  long dim = 0;
  compute_minpoly(param, data, data_bms, dimquot, linvars, lineqs, nvars, &dim,
                  info_level);


  if(info_level){
    fprintf(stderr, "Time spent to compute eliminating polynomial (elapsed: %.2f sec\n",
            omp_get_wtime()-st);
    fprintf(stderr, "Elimination done.\n");
  }


  if (dimquot == dim) { 

    if(info_level){
      fprintf(stderr, "Elimination polynomial is squarefree.\n");
    }

    st = omp_get_wtime();
    if(compute_parametrizations(param, data, data_bms,
                                dim, dimquot, block_size,
                                nlins, linvars, lineqs,
                                nvars) == 0){

      fprintf(stderr, "Matrix is not invertible (there should be a bug)\n");
      free_fglm_bms_data(data_bms);
      free_fglm_data(data);
      return NULL;

    }

  } else {

    fprintf(stderr, "Elimination polynomial is not squarefree.\n");

    int right_param= compute_parametrizations_non_shape_position_case(param,
								      data,
								      data_bms,
								      dimquot,
								      block_size,
								      nlins, linvars,
								      lineqs, squvars,
								      nvars, prime,
								      1); /* verif */
    if (right_param == 0) {
      fprintf(stderr, "Matrix is not invertible (there should be a bug)\n");
      free_fglm_bms_data(data_bms);
      free_fglm_data(data);
      return NULL;
    } else if (right_param == 1) {
      fprintf(stderr, "Radical ideal might have no correct parametrization\n");
    } else if (right_param < nvars) {
      fprintf(stderr, "Only the first %d parametrizations of ",right_param-1);
      fprintf(stderr, "the radical ideal seem correct\n");
    } else {
      fprintf(stderr, "All the parametrizations of ");
      fprintf(stderr, "the radical ideal seem correct\n");
    }
  }
  if(info_level){
    fprintf(stderr, "Time spent to compute parametrizations (elapsed): %.2f sec\n",
            omp_get_wtime()-st);
    fprintf(stderr, "Parametrizations done.\n");
  }
  free_fglm_bms_data(data_bms);
  free_fglm_data(data);
  return param;
}


param_t *nmod_fglm_compute_trace_data(sp_matfglm_t *matrix, mod_t prime,
                                      long nvars,
                                      szmat_t block_size, //taille de bloc dans data->res
                                      long nlins,
                                      uint64_t *linvars,
                                      uint32_t *lineqs,
                                      uint64_t *squvars,
                                      int info_level,
                                      fglm_data_t **bdata,
                                      fglm_bms_data_t **bdata_bms,
                                      int *success){
#if DEBUGFGLM > 0
  fprintf(stderr, "prime = %u\n", prime);
#endif

#if DEBUGFGLM >= 1
  FILE *fmat = fopen("/tmp/matrix.fglm", "w");
  display_fglm_matrix(fmat, matrix);
  fclose(fmat);
#endif

  /* 1147878294 */
  if(prime>=1518500213){
    fprintf(stderr, "Prime %u is too large.\n", prime);
    fprintf(stderr, "One needs to use updated linear algebra fglm functions\n");
    return NULL;
  }

  /* to store the terms we need */
  *bdata = allocate_fglm_data(matrix->nrows, matrix->ncols, nvars);

  param_t *param = allocate_fglm_param(prime, nvars);

  long sz = matrix->ncols * matrix->nrows;
  long nb = initialize_fglm_data(matrix, *bdata, prime, sz, block_size);

  if(info_level){
    fprintf(stderr, "[%u, %u], Non trivial / Trivial = %.2f%%\n",
            matrix->ncols, matrix->nrows,
            100*((double)matrix->nrows / (double)matrix->ncols));
    fprintf(stderr, "Density of non-trivial part %.2f%%\n",
            100-100*(float)nb/(float)sz);
  }

  ulong dimquot = (matrix->ncols);

  double st = omp_get_wtime();

  //////////////////////////////////////////////////////////////////

  /* generate_sequence(matrix, *bdata, block_size, dimquot, prime); */
  generate_sequence_verif(matrix, *bdata, block_size, dimquot,
                          squvars, linvars, nvars, prime);

  if(info_level){
    double nops = 2 * (matrix->nrows/ 1000.0) * (matrix->ncols / 1000.0)  * (matrix->ncols / 1000.0);
    double rt = omp_get_wtime()-st;
    fprintf(stderr, "Time spent to generate sequence (elapsed): %.2f sec (%.2f Gops/sec)\n", rt, nops / rt);
  }

  st = omp_get_wtime();

  /* Berlekamp-Massey data */
  *bdata_bms = allocate_fglm_bms_data(dimquot, prime);

  long dim = 0;
  compute_minpoly(param, *bdata, *bdata_bms, dimquot, linvars, lineqs, nvars, &dim,
                  info_level);
  if(info_level){
    fprintf(stderr, "Time spent to compute eliminating polynomial (elapsed): %.2f sec\n",
            omp_get_wtime()-st);
    fprintf(stderr, "Elimination done.\n");
  }


  if (dimquot == dim) {

    if(info_level){
      fprintf(stderr, "Elimination polynomial has degree %ld.\n", dimquot);
    }

    if(compute_parametrizations(param, *bdata, *bdata_bms,
				dim, dimquot, block_size,
				nlins, linvars, lineqs,
				nvars) == 0){

      fprintf(stderr, "Matrix is not invertible (there should be a bug)\n");
      return NULL;

    }

  }
  else {
    /* computes the param of the radical */
    if(info_level){
      fprintf(stderr, "Elimination polynomial is not squarefree.\n");
    }

    int right_param= compute_parametrizations_non_shape_position_case(param,
								      *bdata,
								      *bdata_bms,
								      dimquot,
								      block_size,
								      nlins, linvars,
								      lineqs,
								      squvars,
								      nvars, prime,
								      1); /* verif */

    if (right_param == 0) {
      if(info_level){
        fprintf(stderr, "Matrix is not invertible (there should be a bug)\n");
      }
      *success = 0;
      return NULL;
    }
    else
      if (right_param == 1) {
        if(info_level){
          fprintf(stderr,
                  "Parametrizations of the radical of the input ideal are not correct\n");
        }
        *success = 0;
      }
      else
        /* if (right_param == 2) { */
        /*   if(info_level){ */
        /*     fprintf(stderr, */
        /*             "Parametrizations of the radical of the input ideal is not correct\n"); */
        /*     fprintf (stderr,"(except the first one)\n"); */
        /*   } */
        /*   *success = 0; */
        /* } */
        /* else */
          if (right_param < nvars) {
            if(info_level){
              fprintf(stderr, "Only the first %d parametrizations of ",right_param-1);
              fprintf(stderr, "the radical ideal seem correct\n");
            }
            *success = 0;
          }
  }
  return param;
}

/*

  Fonction qui applique sparse FGLM apres le premier round dans
  un process multi-mod.

  Renvoie 0 si le calcul est correct ; si non on renvoie 1

 */
int nmod_fglm_compute_apply_trace_data(sp_matfglm_t *matrix,
                                       const mod_t prime,
                                       param_t *param,
                                       const long nvars,
                                       const long bsz,
                                       const long nlins,
                                       uint64_t *linvars,
                                       uint32_t *lineqs,
                                       uint64_t *squvars,
                                       fglm_data_t *data_fglm,
                                       fglm_bms_data_t *data_bms,
                                       const long deg_init,
                                       const int info_level){
#if DEBUGFGLM > 0
  fprintf(stderr, "prime = %u\n", prime);
#endif

#if DEBUGFGLM >= 1
  FILE *fmat = fopen("/tmp/matrix.fglm", "w");
  display_fglm_matrix(fmat, matrix);
  fclose(fmat);
#endif

  if(prime>=1518500213){
    fprintf(stderr, "Prime %u is too large.\n", prime);
    fprintf(stderr, "One needs to use update linear algebra fglm functions\n");
    exit(1);
  }

  /* block-size in  data->res */
  /* to store the terms we need */
  const szmat_t block_size = bsz; 


  fglm_param_set_prime(param, prime);

  const long sz = matrix->ncols * matrix->nrows;
  const long nb = initialize_fglm_data(matrix, data_fglm, prime, sz, block_size);

  if(info_level){
    fprintf(stderr, "[%u, %u], Non trivial / Trivial = %.2f%%\n",
            matrix->ncols, matrix->nrows,
            100*((double)matrix->nrows / (double)matrix->ncols));
    fprintf(stderr, "Density of non-trivial part %.2f%%\n",
            100-100*(float)nb/(float)sz);
  }

  const ulong dimquot = (matrix->ncols);

#if DEBUGFGLM > 0
  print_vec(stderr, data_fglm->vecinit, matrix->ncols);
  print_vec(stderr, data_fglm->res, 2 * block_size * matrix->ncols);
  fprintf(stderr, "\n");
#endif

  double st = omp_get_wtime();

  //////////////////////////////////////////////////////////////////

  /* generate_sequence(matrix, data_fglm, block_size, dimquot, prime); */
  generate_sequence_verif(matrix, data_fglm, block_size, dimquot,
                          squvars, linvars, nvars, prime);
  /* fprintf(stderr, "Matrix: \n"); */
  /* display_fglm_matrix(stderr, matrix); */
  /* fprintf(stderr, "\n"); */

  //////////////////////////////////////////////////////////////////

  if(info_level){
    double nops = 2 * (matrix->nrows/ 1000.0) * (matrix->ncols / 1000.0)  * (matrix->ncols / 1000.0);
    double rt = omp_get_wtime()-st;
    fprintf(stderr, "Time spent to generate sequence (elapsed): %.2f sec (%.2f Gops/sec)\n", rt, nops / rt);
  }

  st = omp_get_wtime();

  fglm_bms_data_set_prime(data_bms, prime);

  long dim = 0;
  compute_minpoly(param, data_fglm, data_bms, dimquot, linvars, lineqs, nvars, &dim,
                  info_level);

  if(info_level){
    fprintf(stderr, "Time spent to compute eliminating polynomial (elapsed): %.2f sec\n",
            omp_get_wtime()-st);
    fprintf(stderr, "Elimination done.\n");
  }
  if(param->elim->length-1 != deg_init){
    return 1;
  }

  if (dimquot == dim) {

    if(compute_parametrizations(param, data_fglm, data_bms,
				dim, dimquot, block_size,
				nlins, linvars, lineqs,
				nvars) == 0){

      fprintf(stderr, "Matrix is not invertible (there should be a bug)\n");
      exit(1);
    }

  } else {
    /* parametrization of the radical */
    compute_parametrizations_non_shape_position_case(param,
                                                     data_fglm,
                                                     data_bms,
                                                     dimquot,
                                                     block_size,
                                                     nlins, linvars,
                                                     lineqs,
                                                     squvars,
                                                     nvars, prime,
                                                     1);
  }
  return 0;
}

