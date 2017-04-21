/*
Copyright (c) 2013, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

* Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
* Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products
      derived from this software without specific prior written
      permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

/*******************************************************************

NAME:    Stencil_FT

PURPOSE: This program tests the efficiency with which a space-invariant,
         linear, symmetric filter (stencil) can be applied to a square
         grid or image, with Fenix fault tolerance added

USAGE:   The program takes as input the linear dimension of the grid,
         the number of iterations on the grid, the number of spare ranks,
         a file containing instructions for eliminating ranks at certain 
         times, and a flag indicating how data recovery should take place.

               <progname> <# iterations> <grid size> <spare ranks> \
                          <kill set size> <kill period> <checkpointing>

         The output consists of diagnostics to make sure the
         algorithm worked, and of timing statistics.

FUNCTIONS CALLED:

         Other than MPI or standard C functions, the following
         functions are used in this program:

         wtime()
         bail_out()

HISTORY: - Written by Rob Van der Wijngaart, November 2006.
         - RvdW, August 2013: Removed unrolling pragmas for clarity;
           fixed bug in computation of width of strip assigned to
           each rank;
         - RvdW, August 2013: added constant to array "in" at end of
           each iteration to force refreshing of neighbor data in
           parallel versions
         - RvdW, October 2014: introduced 2D domain decomposition
         - RvdW, October 2014: removed barrier at start of each iteration
         - RvdW, October 2014: replaced single rank/single iteration timing
           with global timing of all iterations across all ranks

*********************************************************************************/

#include <signal.h>
#include <sys/types.h>
#include <par-res-kern_general.h>
#include <par-res-kern_fenix.h>
#include <random_draw.h>
#include <unistd.h>

#if DOUBLE
  #define DTYPE     double
  #define MPI_DTYPE MPI_DOUBLE
  #define EPSILON   1.e-8
  #define COEFX     1.0
  #define COEFY     1.0
  #define FSTR      "%lf"
#else
  #define DTYPE     float
  #define MPI_DTYPE MPI_FLOAT
  #define EPSILON   0.0001f
  #define COEFX     1.0f
  #define COEFY     1.0f
  #define FSTR      "%f"
#endif

/* define shorthand for indexing multi-dimensional arrays with offsets           */
#define INDEXIN(i,j)  (i+RADIUS+(j+RADIUS)*(width+2*RADIUS))
/* need to add offset of RADIUS to j to account for ghost points                 */
#define IN(i,j)       in[INDEXIN(i-istart,j-jstart)]
#define INDEXOUT(i,j) (i+(j)*(width))
#define OUT(i,j)      out[INDEXOUT(i-istart,j-jstart)]
#define WEIGHT(ii,jj) weight[ii+RADIUS][jj+RADIUS]

int main(int argc, char ** argv) {

  int    Num_procs;       /* number of ranks                                     */
  int    Num_procsx, Num_procsy; /* number of ranks in each coord direction      */
  int    my_ID;           /* MPI rank                                            */
  int    my_IDx, my_IDy;  /* coordinates of rank in rank grid                    */
  int    right_nbr;       /* global rank of right neighboring tile               */
  int    left_nbr;        /* global rank of left neighboring tile                */
  int    top_nbr;         /* global rank of top neighboring tile                 */
  int    bottom_nbr;      /* global rank of bottom neighboring tile              */
  DTYPE *top_buf_out;     /* communication buffer                                */
  DTYPE *top_buf_in;      /*       "         "                                   */
  DTYPE *bottom_buf_out;  /*       "         "                                   */
  DTYPE *bottom_buf_in;   /*       "         "                                   */
  DTYPE *right_buf_out;   /*       "         "                                   */
  DTYPE *right_buf_in;    /*       "         "                                   */
  DTYPE *left_buf_out;    /*       "         "                                   */
  DTYPE *left_buf_in;     /*       "         "                                   */
  int    root = 0;
  int    n, width, height;/* linear global and local grid dimension              */
  long   nsquare;         /* total number of grid points                         */
  int    iter, iter_init, leftover;  /* dummies                                        */
  int    istart, iend;    /* bounds of grid tile assigned to calling rank        */
  int    jstart, jend;    /* bounds of grid tile assigned to calling rank        */
  DTYPE  norm,            /* L1 norm of solution                                 */
         local_norm,      /* contribution of calling rank to L1 norm             */
         reference_norm;
  DTYPE  f_active_points; /* interior of grid with respect to stencil            */
  DTYPE  flops;           /* floating point ops per iteration                    */
  int    iterations;      /* number of times to run the algorithm                */
  double local_stencil_time,/* timing parameters                                 */
         stencil_time,
         avgtime;
  int    stencil_size;    /* number of points in stencil                         */
  DTYPE  * RESTRICT in;   /* input grid values                                   */
  DTYPE  * RESTRICT out;  /* output grid values                                  */
  long   total_length_in; /* total required length to store input array          */
  long   total_length_out;/* total required length to store output array         */
  int    error=0;         /* error flag                                          */
  DTYPE  weight[2*RADIUS+1][2*RADIUS+1]; /* weights of points in the stencil     */
  MPI_Request request[8];
  int    spare_ranks;     /* number of ranks to keep in reserve                  */
  int    kill_ranks;      /* number of ranks that die with each failure          */
  int    *kill_set;       /* instance of set of ranks to be killed               */
  int    kill_period;     /* average number of iterations between failures       */
  int    *fail_iter;      /* list of iterations when a failure will be triggered */
  int    fail_iter_s=0;   /* latest  */
  DTYPE  init_add;        /* used to offset initial solutions                    */
  int    checkpointing;   /* indicates if data is restored using Fenix or
                             analytically                                        */
  int    num_fenix_init=1;/* number of times Fenix_Init is called                */
  int    num_fenix_init_loc;/* number of times Fenix_Init was called             */
  int    fenix_status;
  random_draw_t dice;

  /*******************************************************************************
  ** Initialize the MPI environment
  ********************************************************************************/
  MPI_Init(&argc,&argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_ID);
  MPI_Comm_size(MPI_COMM_WORLD, &Num_procs);

  /*******************************************************************************
  ** process, test, and broadcast input parameters
  ********************************************************************************/

  if (my_ID == root) {
    printf("Parallel Research Kernels version %s\n", PRKVERSION);
    printf("MPI stencil execution on 2D grid with Fenix fault tolerance\n");
#if !STAR
    printf("ERROR: Compact stencil not supported\n");
    error = 1;
    goto ENDOFTESTS;
#endif

    if (argc != 7){
      printf("Usage: %s <# iterations> <array dimension> <spare ranks> <kill set size> ",
             *argv);
      printf("<kill period> <checkpointing>\n");
      error = 1;
      goto ENDOFTESTS;
    }

    iterations  = atoi(argv[1]);
    if (iterations < 1){
      printf("ERROR: iterations must be >= 1 : %d \n",iterations);
      error = 1;
      goto ENDOFTESTS;
    }

    n       = atoi(argv[2]);
    nsquare = (long) n * (long) n;
    if (nsquare < Num_procs){
      printf("ERROR: grid size %ld must be at least # ranks: %d\n",
	     nsquare, Num_procs);
      error = 1;
      goto ENDOFTESTS;
    }

    if (RADIUS < 0) {
      printf("ERROR: Stencil radius %d should be non-negative\n", RADIUS);
      error = 1;
      goto ENDOFTESTS;
    }

    if (2*RADIUS +1 > n) {
      printf("ERROR: Stencil radius %d exceeds grid size %d\n", RADIUS, n);
      error = 1;
      goto ENDOFTESTS;
    }

    spare_ranks  = atoi(argv[3]);
    if (spare_ranks < 0 || spare_ranks >= Num_procs){
      printf("ERROR: Illegal number of spare ranks : %d \n", spare_ranks);
      error = 1;
      goto ENDOFTESTS;     
    }

    kill_ranks = atoi(argv[4]);
    if (kill_ranks < 0 || kill_ranks > spare_ranks) {
      printf("ERROR: Number of ranks in kill set invalid: %d\n", kill_ranks);
      error = 1;
      goto ENDOFTESTS;     
    }

    kill_period = atoi(argv[5]);
    if (kill_period < 1) {
      printf("ERROR: rank kill period must be positive: %d\n", kill_period);
      error = 1;
      goto ENDOFTESTS;     
    }

    checkpointing = atoi(argv[6]);
    if (checkpointing) {
      printf("ERROR: Fenix checkpointing not yet implemented\n");
      error = 1;
      goto ENDOFTESTS;     
    }

    ENDOFTESTS:;
  }
  bail_out(error, MPI_COMM_WORLD);

  /* before calling Fenix_Init, all ranks need to know how many spare ranks 
     to reserve; broadcast other parameters as well                          */
  MPI_Bcast(&n,             1, MPI_INT, root, MPI_COMM_WORLD);
  MPI_Bcast(&iterations,    1, MPI_INT, root, MPI_COMM_WORLD);
  MPI_Bcast(&spare_ranks,   1, MPI_INT, root, MPI_COMM_WORLD);
  MPI_Bcast(&kill_ranks,    1, MPI_INT, root, MPI_COMM_WORLD);
  MPI_Bcast(&kill_period,   1, MPI_INT, root, MPI_COMM_WORLD);
  MPI_Bcast(&checkpointing, 1, MPI_INT, root, MPI_COMM_WORLD);

  /* determine best way to create a 2D grid of ranks (closest to square)     */
  factor(Num_procs-spare_ranks, &Num_procsx, &Num_procsy);

  if (my_ID == root) {
    printf("Number of ranks          = %d\n", Num_procs);
    printf("Grid size                = %d\n", n);
    printf("Radius of stencil        = %d\n", RADIUS);
    printf("Tiles in x/y-direction   = %d/%d\n", Num_procsx, Num_procsy);
    printf("Type of stencil          = star\n");
#if DOUBLE
    printf("Data type                = double precision\n");
#else
    printf("Data type                = single precision\n");
#endif
#if LOOPGEN
    printf("Loop body representation = expanded by script\n");
#else
    printf("Loop body representation = compact\n");
#endif
    printf("Number of iterations     = %d\n", iterations);
    printf("Spare ranks              = %d\n", spare_ranks);
    printf("Kill set size            = %d\n", kill_ranks);
    printf("Fault period             = %d\n", kill_period);
    if (checkpointing)
      printf("Data recovery            = Fenix checkpointing\n");
    else
      printf("Data recovery            = analytical\n");
  }

  /* initialize the random number generator for each rank; we do that before
     starting Fenix, so that all ranks, including spares, are initialized      */
  LCG_init(&dice);
  /* compute the iterations during which errors will be incurred               */
  for (iter=0; iter<iterations; iter++) {
    fail_iter_s += random_draw(kill_period, &dice);
    if (fail_iter_s >= iterations) break;
    num_fenix_init++;
  }
  if ((num_fenix_init-1)*kill_ranks>spare_ranks) {
    if (my_ID==0) printf("ERROR: number of injected errors %d exceeds spare ranks %d\n",
                         (num_fenix_init-1)*kill_ranks, spare_ranks);
    error = 1;
  }
  else if(my_ID==0) printf("Total injected failures  = %d*%d\n", 
                           num_fenix_init-1, kill_ranks);
  bail_out(error, MPI_COMM_WORLD);

  fail_iter = (int *) prk_malloc(sizeof(int)*num_fenix_init);
  if (!fail_iter) {
    printf("ERROR: Rank %d could not allocate space for array fail_iter\n", my_ID);
    error = 1;
  }
  bail_out(error, MPI_COMM_WORLD);
  /* now record the actual failure iterations                                  */
  for (fail_iter_s=iter=0; iter<num_fenix_init; iter++) {
    fail_iter_s += random_draw(kill_period, &dice);
    fail_iter[iter] = fail_iter_s;
  }

  /* start timer for all ranks, including spares                               */
  MPI_Barrier(MPI_COMM_WORLD);
  local_stencil_time = wtime();

  /* Here is where we initialize Fenix and mark the return point after failure */
  Fenix_Init(&fenix_status, MPI_COMM_WORLD, NULL, &argc, &argv, spare_ranks, 
             0, MPI_INFO_NULL, &error);
  if (error==FENIX_WARNING_SPARE_RANKS_DEPLETED) 
    printf("ERROR: Rank %d: Cannot reconsitute original communicator\n", my_ID);
  bail_out(error, MPI_COMM_WORLD);

  MPI_Comm newcomm, newcomm2, newcomm3;
  MPI_Comm_dup(MPI_COMM_WORLD, &newcomm);
  MPI_Comm_dup(MPI_COMM_WORLD, &newcomm3);

  MPI_Comm_size(MPI_COMM_WORLD, &Num_procs);
  MPI_Comm_rank(MPI_COMM_WORLD, &my_ID);

  MPI_Comm_split(newcomm, 1, my_ID, &newcomm2);


  /* if rank is recovered, set iter to a negative number, to be increased
     to the actual value corresponding to the current iter value among
     survivor ranks; handle number of Fenix_Init calls similarly               */
  switch (fenix_status){
    case FENIX_ROLE_INITIAL_RANK:   iter_init = num_fenix_init_loc = 0;    break;
    case FENIX_ROLE_RECOVERED_RANK: iter_init = num_fenix_init_loc = -1;   break;
    case FENIX_ROLE_SURVIVOR_RANK:  iter_init = iter;  num_fenix_init_loc++;
  }

  MPI_Allreduce(&iter_init, &iter, 1, MPI_INT, MPI_MAX, newcomm);
  MPI_Allreduce(&num_fenix_init_loc, &num_fenix_init, 1, MPI_INT, MPI_MAX, newcomm);

  my_IDx = my_ID%Num_procsx;
  my_IDy = my_ID/Num_procsx;
  /* compute neighbors; don't worry about dropping off the edges of the grid   */
  right_nbr  = my_ID+1;
  left_nbr   = my_ID-1;
  top_nbr    = my_ID+Num_procsx;
  bottom_nbr = my_ID-Num_procsx;

  /* compute amount of space required for input and solution arrays             */

  width = n/Num_procsx;
  leftover = n%Num_procsx;
  if (my_IDx<leftover) {
    istart = (width+1) * my_IDx;
    iend = istart + width;
  }
  else {
    istart = (width+1) * leftover + width * (my_IDx-leftover);
    iend = istart + width - 1;
  }

  width = iend - istart + 1;
  if (width == 0) {
    printf("ERROR: rank %d has no work to do\n", my_ID);
    error = 1;
  }
  bail_out(error, MPI_COMM_WORLD);

  height = n/Num_procsy;
  leftover = n%Num_procsy;
  if (my_IDy<leftover) {
    jstart = (height+1) * my_IDy;
    jend = jstart + height;
  }
  else {
    jstart = (height+1) * leftover + height * (my_IDy-leftover);
    jend = jstart + height - 1;
  }

  height = jend - jstart + 1;
  if (height == 0) {
    printf("ERROR: rank %d has no work to do\n", my_ID);
    error = 1;
  }
  bail_out(error, MPI_COMM_WORLD);

  if (width < RADIUS || height < RADIUS) {
    printf("ERROR: rank %d has work tile smaller then stencil radius\n",
           my_ID);
    error = 1;
  }
  bail_out(error, MPI_COMM_WORLD);

  total_length_in  = (long) (width+2*RADIUS)*(long) (height+2*RADIUS)*sizeof(DTYPE);
  total_length_out = (long) width* (long) height*sizeof(DTYPE);

  if (fenix_status != FENIX_ROLE_SURVIVOR_RANK) {
    in  = (DTYPE *) prk_malloc(total_length_in);
    out = (DTYPE *) prk_malloc(total_length_out);
    if (!in || !out) {
      printf("ERROR: rank %d could not allocate space for input/output array\n",
              my_ID);
      error = 1;
    }
  }
  bail_out(error, MPI_COMM_WORLD);

  /* fill the stencil weights to reflect a discrete divergence operator         */
  for (int jj=-RADIUS; jj<=RADIUS; jj++) for (int ii=-RADIUS; ii<=RADIUS; ii++)
    WEIGHT(ii,jj) = (DTYPE) 0.0;

  stencil_size = 4*RADIUS+1;
  for (int ii=1; ii<=RADIUS; ii++) {
    WEIGHT(0, ii) = WEIGHT( ii,0) =  (DTYPE) (1.0/(2.0*ii*RADIUS));
    WEIGHT(0,-ii) = WEIGHT(-ii,0) = -(DTYPE) (1.0/(2.0*ii*RADIUS));
  }

  norm = (DTYPE) 0.0;
  f_active_points = (DTYPE) (n-2*RADIUS)*(DTYPE) (n-2*RADIUS);
  /* intialize the input and output arrays                                     */
  if (checkpointing) init_add = 0.0;
  else               init_add = (DTYPE) iter;
  for (int j=jstart; j<=jend; j++) for (int i=istart; i<=iend; i++) {
    IN(i,j)  = COEFX*i+COEFY*j+init_add;
    OUT(i,j) = (COEFX+COEFY)*init_add;
  }

  if (Num_procs > 1) {
    if (fenix_status != FENIX_ROLE_SURVIVOR_RANK) {
      /* allocate communication buffers for halo values                        */
      top_buf_out = (DTYPE *) prk_malloc(4*sizeof(DTYPE)*RADIUS*width);
      if (!top_buf_out) {
        printf("ERROR: Rank %d could not allocated comm buffers for y-direction\n", my_ID);
        error = 1;
      }
      top_buf_in     = top_buf_out +   RADIUS*width;
      bottom_buf_out = top_buf_out + 2*RADIUS*width;
      bottom_buf_in  = top_buf_out + 3*RADIUS*width;
    }
    bail_out(error, MPI_COMM_WORLD);

    if (fenix_status != FENIX_ROLE_SURVIVOR_RANK) {
      right_buf_out  = (DTYPE *) prk_malloc(4*sizeof(DTYPE)*RADIUS*height);
      if (!right_buf_out) {
        printf("ERROR: Rank %d could not allocated comm buffers for x-direction\n", my_ID);
        error = 1;
      }
      right_buf_in   = right_buf_out +   RADIUS*height;
      left_buf_out   = right_buf_out + 2*RADIUS*height;
      left_buf_in    = right_buf_out + 3*RADIUS*height;
    }
    bail_out(error, MPI_COMM_WORLD);
  }

  for (; iter<=iterations; iter++){

    /* inject failure if appropriate                                                */
    if (iter == fail_iter[num_fenix_init]) {
      pid_t pid = getpid();
      if (my_ID < kill_ranks) {
#if VERBOSE
        printf("Rank %d, pid %d commits suicide in iter %d\n", my_ID, pid, iter);
#endif
        kill(pid, SIGKILL);
      }
#if VERBOSE
      else printf("Rank %d, pid %d is survivor rank in iter %d\n", my_ID, pid, iter);
#endif
    }

    /* need to fetch ghost point data from neighbors in y-direction                 */
    if (my_IDy < Num_procsy-1) {
      MPI_Irecv(top_buf_in, RADIUS*width, MPI_DTYPE, top_nbr, 101,
                newcomm, &(request[1]));
      for (int kk=0,j=jend-RADIUS+1; j<=jend; j++) for (int i=istart; i<=iend; i++) {
          top_buf_out[kk++]= IN(i,j);
      }
      MPI_Isend(top_buf_out, RADIUS*width,MPI_DTYPE, top_nbr, 99,
                newcomm, &(request[0]));
    }
    if (my_IDy > 0) {
      MPI_Irecv(bottom_buf_in,RADIUS*width, MPI_DTYPE, bottom_nbr, 99,
                newcomm, &(request[3]));
      for (int kk=0,j=jstart; j<=jstart+RADIUS-1; j++) for (int i=istart; i<=iend; i++) {
          bottom_buf_out[kk++]= IN(i,j);
      }
      MPI_Isend(bottom_buf_out, RADIUS*width,MPI_DTYPE, bottom_nbr, 101,
                newcomm, &(request[2]));
    }
    if (my_IDy < Num_procsy-1) {
      MPI_Wait(&(request[0]), MPI_STATUS_IGNORE);
      MPI_Wait(&(request[1]), MPI_STATUS_IGNORE);
      for (int kk=0,j=jend+1; j<=jend+RADIUS; j++) for (int i=istart; i<=iend; i++) {
          IN(i,j) = top_buf_in[kk++];
      }
    }
    if (my_IDy > 0) {
      MPI_Wait(&(request[2]), MPI_STATUS_IGNORE);
      MPI_Wait(&(request[3]), MPI_STATUS_IGNORE);
      for (int kk=0,j=jstart-RADIUS; j<=jstart-1; j++) for (int i=istart; i<=iend; i++) {
          IN(i,j) = bottom_buf_in[kk++];
      }
    }

    /* need to fetch ghost point data from neighbors in x-direction                 */
    if (my_IDx < Num_procsx-1) {
      MPI_Irecv(right_buf_in, RADIUS*height, MPI_DTYPE, right_nbr, 1010,
                MPI_COMM_WORLD, &(request[1+4]));
      for (int kk=0,j=jstart; j<=jend; j++) for (int i=iend-RADIUS+1; i<=iend; i++) {
          right_buf_out[kk++]= IN(i,j);
      }
      MPI_Isend(right_buf_out, RADIUS*height, MPI_DTYPE, right_nbr, 990,
              MPI_COMM_WORLD, &(request[0+4]));
    }
    if (my_IDx > 0) {
      MPI_Irecv(left_buf_in, RADIUS*height, MPI_DTYPE, left_nbr, 990,
                MPI_COMM_WORLD, &(request[3+4]));
      for (int kk=0,j=jstart; j<=jend; j++) for (int i=istart; i<=istart+RADIUS-1; i++) {
          left_buf_out[kk++]= IN(i,j);
      }
      MPI_Isend(left_buf_out, RADIUS*height, MPI_DTYPE, left_nbr, 1010,
                MPI_COMM_WORLD, &(request[2+4]));
    }
    if (my_IDx < Num_procsx-1) {
      MPI_Wait(&(request[0+4]), MPI_STATUS_IGNORE);
      MPI_Wait(&(request[1+4]), MPI_STATUS_IGNORE);
      for (int kk=0,j=jstart; j<=jend; j++) for (int i=iend+1; i<=iend+RADIUS; i++) {
          IN(i,j) = right_buf_in[kk++];
      }
    }
    if (my_IDx > 0) {
      MPI_Wait(&(request[2+4]), MPI_STATUS_IGNORE);
      MPI_Wait(&(request[3+4]), MPI_STATUS_IGNORE);
      for (int kk=0,j=jstart; j<=jend; j++) for (int i=istart-RADIUS; i<=istart-1; i++) {
          IN(i,j) = left_buf_in[kk++];
      }
    }

    /* Apply the stencil operator */
    for (int j=MAX(jstart,RADIUS); j<=MIN(n-RADIUS-1,jend); j++) {
      for (int i=MAX(istart,RADIUS); i<=MIN(n-RADIUS-1,iend); i++) {
        #if LOOPGEN
          #include "loop_body_star.incl"
        #else
          for (int jj=-RADIUS; jj<=RADIUS; jj++) OUT(i,j) += WEIGHT(0,jj)*IN(i,j+jj);
          for (int ii=-RADIUS; ii<0; ii++)       OUT(i,j) += WEIGHT(ii,0)*IN(i+ii,j);
          for (int ii=1; ii<=RADIUS; ii++)       OUT(i,j) += WEIGHT(ii,0)*IN(i+ii,j);
        #endif
      }
    }

    /* add constant to solution to force refresh of neighbor data, if any */
    for (int j=jstart; j<=jend; j++) for (int i=istart; i<=iend; i++) IN(i,j)+= 1.0;

  } /* end of iterations                                                   */

  local_stencil_time = wtime() - local_stencil_time;

  MPI_Reduce(&local_stencil_time, &stencil_time, 1, MPI_DOUBLE, MPI_MAX, root,
             MPI_COMM_WORLD);

  /* compute L1 norm in parallel                                                */
  local_norm = (DTYPE) 0.0;
  for (int j=MAX(jstart,RADIUS); j<=MIN(n-RADIUS-1,jend); j++) {
    for (int i=MAX(istart,RADIUS); i<=MIN(n-RADIUS-1,iend); i++) {
      local_norm += (DTYPE)ABS(OUT(i,j));
    }
  }

  MPI_Reduce(&local_norm, &norm, 1, MPI_DTYPE, MPI_SUM, root, MPI_COMM_WORLD);

  /*******************************************************************************
  ** Analyze and output results.
  ********************************************************************************/

/* verify correctness                                                            */
  if (my_ID == root) {
    norm /= f_active_points;
    reference_norm = (DTYPE) (iterations+1) * (COEFX + COEFY);

    if (ABS(norm-reference_norm) > EPSILON) {
      printf("ERROR: L1 norm = "FSTR", Reference L1 norm = "FSTR"\n",
             norm, reference_norm);
      error = 1;
    }
    else {
      printf("Solution validates\n");
#if VERBOSE
      printf("Reference L1 norm = "FSTR", L1 norm = "FSTR"\n",
             reference_norm, norm);
#endif
    }
  }
  bail_out(error, MPI_COMM_WORLD);

  if (my_ID == root) {
    /* flops/stencil: 2 flops (fma) for each point in the stencil,
       plus one flop for the update of the input of the array        */
    flops = (DTYPE) (2*stencil_size+1) * f_active_points;
    avgtime = stencil_time/(iterations+1);
    printf("Rate (MFlops/s): "FSTR"  Avg time (s): %lf\n",
           1.0E-06 * flops/avgtime, avgtime);
  }

  Fenix_Finalize();
  MPI_Finalize();
  exit(EXIT_SUCCESS);
}