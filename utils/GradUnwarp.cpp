#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "romp_support.h"
#include "mriBSpline.h"

#include "GradUnwarp.h"
#include "legendre.h"

/*******************************************************************************************/
/******************** Implementation of GradUnwarp class ***********************************/
/*********************   ennvironment variables to enable debug info:  *********************/
/**********     GRADUNWARP_PRN_GRADCOEFF, GRADUNWARP_PRN_GRADCOEFF_READ,       *************/
/**********     GRADUNWARP_PRN_LEGENDRE_NORMFACT, GRADUNWARP_PRN_LEGENDRE,     *************/
/**********             GRADUNWARP_PRN_SIEMENS_B, GRADUNWARP_PRN_DEBUG         *************/
GradUnwarp::GradUnwarp(int nthreads0)
{
  nthreads = nthreads0;
  fgrad = NULL;

  nmax = 0;
  mmax = 0;

  coeff = NULL;
  coeffCount = 0;
  coeffDim = 0;

  R0 = 0;
  Alpha_x = NULL; Alpha_y = NULL; Alpha_z = NULL;
  Beta_x  = NULL; Beta_y  = NULL; Beta_z  = NULL;

  Alpha_Beta_initialized = false;

  gcam = NULL;
}

GradUnwarp::~GradUnwarp()
{
  if (!Alpha_Beta_initialized)
  {
    if (gcam != NULL)
      GCAMfree(&gcam);

    return;
  }

  if (coeff != NULL)
    free(coeff);

  int i;
  for (i = 0; i < coeffDim; i++)
  {
    free(Alpha_x[i]);
    free(Alpha_y[i]);
    free(Alpha_z[i]);
    free(Beta_x[i]);
    free(Beta_y[i]);
    free(Beta_z[i]);
  }

  free(Alpha_x);
  free(Alpha_y);
  free(Alpha_z);
  free(Beta_x);
  free(Beta_y);
  free(Beta_z);  

  free(minusonepow);
  free(factorials);

  for (i = 0; i < coeffDim; i++)
    free(normfact[i]);
  free(normfact);
}

void GradUnwarp::read_siemens_coeff(const char *gradfilename)
{
  // check if gradfile has extension .grad

  // open gradfilename
  fgrad = fopen(gradfilename, "r");
  if (fgrad == NULL)
  {
    printf("ERROR: could not read the coefficient file %s\n", gradfilename);
    return;
  }

  printf("==> reading coefficients from gradient coil file %s\n" , gradfilename) ;

  _skipCoeffComment();

  // hard-coded limits:
  // number of coeff entries - 100
  // length of each entry    - 1024
  char coeffline[1024];

  // skip the next line. (It contains an information about the system type.)
  fgets(coeffline, sizeof(coeffline), fgrad);

  printf("==> reading system type string from coeff.grad file...\n");
  printf("%s\n", coeffline);

  // check if first paramline contains "win_"; and, if so, parse
  fgets(coeffline, sizeof(coeffline), fgrad);
  if (strncmp(coeffline, " win_", 5) == 0)
  {

    // parse into the four parameters (these don't seem to be used anywhere...)
    //[dum, iThreshLo, dum, iThreshUp, dum, iAlgoTyp, dum, iWinDummy] ...
    //	= strread(paramline, " %10c%d%13c%d%13c%d%14c%d;");

    // read next line
    fgets(coeffline, sizeof(coeffline), fgrad);
  }

  // only extract radius and ignore rest
  sscanf(coeffline, "%f\n", &R0);
  printf("WARN: returning R0 = %f in units of METERS!\n", R0);
  R0 = R0 * 1000;  // R0 is now in mm
  
  // read next line, which contains gradient system mode "(0 = typ. tunnel magnet system; 1 = typ. open magnet system)"
  fgets(coeffline, sizeof(coeffline), fgrad);
  int CoSyMode;
  sscanf(coeffline, "%d = %*s\n", &CoSyMode);
  printf("CoSyMode = %d\n", CoSyMode);

  // skip the next 5 lines
  for (int ind = 0; ind < 5; ind++)
      fgets(coeffline, sizeof(coeffline), fgrad);

  // remember the starting of spherical harmonic coefficients
  size_t fpos = ftell(fgrad);

  // first pass: get the entry counts
  while (fgets(coeffline, sizeof(coeffline), fgrad) != NULL)
  {
    int len = strlen(coeffline);
    char* ptr = coeffline;
    char* endptr = ptr + len;

    // skip leading spaces and tabs, also empty lines
    while (ptr != endptr && (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n'))
      ptr++;

    if (*ptr == '\0')
      continue;

    if (getenv("GRADUNWARP_PRN_GRADCOEFF_READ"))
      printf("(first pass) entry #%d: %s\n", coeffCount, coeffline);

    coeffCount++;
  }

  coeff = new COEFF[coeffCount];
  coeffCount = 0;

  // rewind file pointer
  fseek(fgrad, fpos, SEEK_SET);

  /******************** second pass **************************/
  /****** begin reading spherical harmonic coefficients ******/
  while (fgets(coeffline, sizeof(coeffline), fgrad) != NULL)
  {
    int len = strlen(coeffline);
    char* ptr = coeffline;
    char* endptr = ptr + len;

    // skip leading spaces and tabs, also empty lines
    while (ptr != endptr && (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n'))
      ptr++;

    if (*ptr == '\0')
      continue;

    if (getenv("GRADUNWARP_PRN_GRADCOEFF_READ"))
      printf("(second pass) entry #%d: %s\n", coeffCount, coeffline);

    sscanf(ptr, "%d %c(%d, %d) %f %c", 
                      &coeff[coeffCount].num, &coeff[coeffCount].A_or_B, &coeff[coeffCount].n, &coeff[coeffCount].m, 
                      &coeff[coeffCount].value, &coeff[coeffCount].xyz);
    nmax = (coeff[coeffCount].n > nmax) ? coeff[coeffCount].n : nmax;
    mmax = (coeff[coeffCount].m > mmax) ? coeff[coeffCount].m : mmax;

    if (getenv("GRADUNWARP_PRN_GRADCOEFF_READ"))
      printf("(second pass) %d %c (%d, %d) %f %c\n", coeff[coeffCount].num, 
             coeff[coeffCount].A_or_B, coeff[coeffCount].n, coeff[coeffCount].m, coeff[coeffCount].value, coeff[coeffCount].xyz);

    coeffCount++;
  }

  fclose(fgrad);    

  nmax = (nmax > mmax) ? nmax : mmax;
  coeffDim = nmax+1;

  _initCoeff();

  /**************************************************************************/
  /****** organize coefficient values ******/
  int n;
  for (n = 0; n < coeffCount; n++)
  {
    float **arrPtr = NULL;
    if (coeff[n].A_or_B == 'A')
    {
      if (coeff[n].xyz == 'x')
        arrPtr = Alpha_x;
      else if (coeff[n].xyz == 'y')
        arrPtr = Alpha_y;
      else if (coeff[n].xyz == 'z')
        arrPtr = Alpha_z;
    }   
    else if (coeff[n].A_or_B == 'B')
    {
      if (coeff[n].xyz == 'x')
        arrPtr = Beta_x;
      else if (coeff[n].xyz == 'y')
        arrPtr = Beta_y;
      else if (coeff[n].xyz == 'z')
        arrPtr = Beta_z;
    }
    
    if (arrPtr == NULL)
    {
      printf("ERROR: unrecognized coefficient string: '%c%c'\n", coeff[n].A_or_B, coeff[n].xyz);
      continue;
    }

    int row = coeff[n].n;
    int col = coeff[n].m; 
    arrPtr[row][col] = coeff[n].value;
  }

  Alpha_Beta_initialized = true;
}

void GradUnwarp::initSiemensLegendreNormfact()
{
  // initialize variables to pre-calculate normfact for siemens_legendre()
  minusonepow = new double[coeffDim];
  factorials  = new double[2*coeffDim];
  normfact = new double*[coeffDim];

  int n;
  for (n = 0; n < coeffDim; n++)
    normfact[n] = new double[coeffDim];

  // pre-calculate minusonepow, factorials, & normfact
  for (n = 0; n < coeffDim; n++)
    minusonepow[n] = pow((-1), n);

  for (n = 0; n < 2*coeffDim; n++)
    factorials[n] = factorial(n);

  if (getenv("GRADUNWARP_PRN_LEGENDRE_NORMFACT"))
    printf("\n");

  for (n = 0; n < coeffDim; n++)
  {
    int m;
    for (m = 0; m < n; m++)
    {
      normfact[n][m] = minusonepow[m+1] * sqrt((2*n+1)*factorials[n-m-1]/(2*factorials[n+m+1]));
   
      if (getenv("GRADUNWARP_PRN_LEGENDRE_NORMFACT"))
        printf("normfact[%2d][%2d] = %s%.6lf, pow((-1), %2d) * sqrt((%2d)*factorial(%2d)/(2*factorial(%2d)))\n",
               n, m, (normfact[n][m] > 0) ? " " : "", normfact[n][m], m+1, 2*n+1, n-m-1, n+m+1);
    }

    if (getenv("GRADUNWARP_PRN_LEGENDRE_NORMFACT"))
      printf("\n");
  }
}

void GradUnwarp::spharm_evaluate(float X, float Y, float Z, float *Dx, float *Dy, float *Dz)
{
  if (!Alpha_Beta_initialized)
  {
    printf("gradient file not loaded!\n");
    exit(1);
    //return;
  }

  Siemens_B *siemens_B = new Siemens_B(coeffDim, nmax, R0, normfact, X, Y, Z);

  float bx = siemens_B->siemens_B_x(Alpha_x, Beta_x);
  float by = siemens_B->siemens_B_y(Alpha_y, Beta_y);
  float bz = siemens_B->siemens_B_z(Alpha_z, Beta_z);

  if (getenv("GRADUNWARP_PRN_SIEMENS_B"))
    printf("bx=%lf, by=%lf, bz=%lf\n", bx, by, bz);

  *Dx = bx * R0;
  *Dy = by * R0;
  *Dz = bz * R0;

  delete siemens_B;
}

void GradUnwarp::printCoeff()
{
  const char *arrs = "AxAyAzBxByBz";

  float **arrPtr = NULL;

  int i = 0;
  for (; i < strlen(arrs); i++)
  {
    if (arrs[i] == 'A')
    {
      i++;
      if (arrs[i] == 'x')
        arrPtr = Alpha_x;
      else if (arrs[i] == 'y')
        arrPtr = Alpha_y;
      else if (arrs[i] == 'z')
        arrPtr = Alpha_z;
    }   
    else if (arrs[i] == 'B')
    {
      i++;
      if (arrs[i] == 'x')
        arrPtr = Beta_x;
      else if (arrs[i] == 'y')
        arrPtr = Beta_y;
      else if (arrs[i] == 'z')
        arrPtr = Beta_z;
    }
    
    if (arrPtr == NULL)
    {
      printf("ERROR: unrecognized coefficient string: '%c%c'\n", arrs[i-1], arrs[i]);
      continue;
    }

    printf("\n%s_%c = \n", (arrs[i-1] == 'A') ? "Alpha" : "Beta", arrs[i]);
    int row = 0;
    for (; row < coeffDim; row++)
    {
      int col = 0;
      for (; col < coeffDim; col++)
      {
        printf("\t%lf", arrPtr[row][col]); 
      }
      printf("\n");
    }
  }
}

void GradUnwarp::create_transtable(VOL_GEOM *vg, MATRIX *vox2ras, MATRIX *inv_vox2ras)
{
  printf("GradUnwarp::create_transtable() ...\n");
  if (gcam != NULL)
    GCAMfree(&gcam);

  gcam = GCAMalloc(vg->width, vg->height, vg->depth);

  // save volgeom orig and target
  //GCAMinitVolGeom(gcam, origvol, origvol);

#ifdef HAVE_OPENMP
  printf("\nSet OPEN MP NUM threads to %d (create_transtable)\n", nthreads);
  omp_set_num_threads(nthreads);
#endif

  int c; 
#ifdef HAVE_OPENMP
#pragma omp parallel for
#endif
  for (c = 0; c < vg->width; c++)
  {
    // You could make a vector of CRS nthreads long
    MATRIX *CRS = MatrixAlloc(4, 1, MATRIX_REAL);
    MATRIX *RAS = MatrixAlloc(4, 1, MATRIX_REAL);;
    MATRIX *DeltaRAS = MatrixAlloc(4, 1, MATRIX_REAL);
    MATRIX *DistortedRAS = MatrixAlloc(4, 1, MATRIX_REAL);
    MATRIX *DistortedCRS = MatrixAlloc(4, 1, MATRIX_REAL);

    int r = 0, s = 0;
    for (r = 0; r < vg->height; r++)
    {
      for (s = 0; s < vg->depth; s++)
      {
        // clear CRS, RAS, DeltaRAS, DistortedRAS, DistortedCRS
        MatrixClear(CRS);
        MatrixClear(RAS);
        MatrixClear(DeltaRAS);
        MatrixClear(DistortedRAS);
        MatrixClear(DistortedCRS);

        CRS->rptr[1][1] = c;
        CRS->rptr[2][1] = r;
        CRS->rptr[3][1] = s;
        CRS->rptr[4][1] = 1;

        // Convert the CRS to RAS
        RAS->rptr[4][1] = 1;
        RAS = MatrixMultiply(vox2ras, CRS, RAS);

        // convert RAS to LAI
        RAS->rptr[1][1] = -RAS->rptr[1][1];
        RAS->rptr[2][1] =  RAS->rptr[2][1];
        RAS->rptr[3][1] = -RAS->rptr[3][1];

        float Dx = 0, Dy = 0, Dz = 0;
        spharm_evaluate(RAS->rptr[1][1], RAS->rptr[2][1], RAS->rptr[3][1], &Dx, &Dy, &Dz);

        DeltaRAS->rptr[1][1] = Dx;
        DeltaRAS->rptr[2][1] = Dy;
        DeltaRAS->rptr[3][1] = Dz;
        DeltaRAS->rptr[4][1] = 1; 
        
        DistortedRAS = MatrixAdd(RAS, DeltaRAS, DistortedRAS);
        DistortedRAS->rptr[4][1] = 1;

        // convert LAI to RAS
        DistortedRAS->rptr[1][1] = -DistortedRAS->rptr[1][1];
        DistortedRAS->rptr[2][1] =  DistortedRAS->rptr[2][1];
        DistortedRAS->rptr[3][1] = -DistortedRAS->rptr[3][1];

        DistortedCRS = MatrixMultiply(inv_vox2ras, DistortedRAS, DistortedCRS);

        float fcs = DistortedCRS->rptr[1][1];
        float frs = DistortedCRS->rptr[2][1];
        float fss = DistortedCRS->rptr[3][1];

        //printf("%f => %f, %f => %f, %f => %f\n", (float)c, fcs, (float)r, frs, (float)s, fss);

        // update GCAM nodes
        _update_GCAMnode(c, r, s, fcs, frs, fss);
      }   // s
    }     // r

    MatrixFree(&CRS);
    MatrixFree(&RAS);
    MatrixFree(&DeltaRAS);
    MatrixFree(&DistortedRAS);
    MatrixFree(&DistortedCRS);
  }       // c
}

void GradUnwarp::_update_GCAMnode(int c, int r, int s, float fcs, float frs, float fss)
{
  // gcam->nodes are indexed by unwarped (c, r, s)
  GCA_MORPH_NODE *gcamn = &gcam->nodes[c][r][s];
  gcamn->origx = c; gcamn->origy = r; gcamn->origz = s;
  gcamn->x = fcs; gcamn->y = frs; gcamn->z = fss;
}

void GradUnwarp::load_transtable(const char* transfile)
{
  printf("GradUnwarp::load_transtable(%s) ...\n", transfile);
  gcam = GCAMread(transfile);
}

void GradUnwarp::save_transtable(const char* transfile)
{
  printf("GradUnwarp::save_transtable(%s) ...\n", transfile);
  GCAMwrite(gcam, transfile);
}

MRI *GradUnwarp::unwarp_volume_gradfile(MRI *warpedvol, MRI *unwarpedvol, MATRIX *vox2ras, MATRIX *inv_vox2ras, int interpcode, int sinchw)
{
  printf("GradUnwarp::unwarp_volume_gradfile() ...\n");

  int (*nintfunc)( double );
  nintfunc = &nint;

  if (unwarpedvol == NULL)
  {
    unwarpedvol = MRIallocSequence(warpedvol->width, warpedvol->height, warpedvol->depth, MRI_FLOAT, warpedvol->nframes);
    MRIcopyHeader(warpedvol, unwarpedvol);
    MRIcopyPulseParameters(warpedvol, unwarpedvol);
  }

  MRI_BSPLINE *bspline = NULL;
  if (interpcode == SAMPLE_CUBIC_BSPLINE)
    bspline = MRItoBSpline(warpedvol, NULL, 3);

#ifdef HAVE_OPENMP
  printf("\nSet OPEN MP NUM threads to %d (unwarp_volume_gradfile)\n", nthreads);
  omp_set_num_threads(nthreads);
#endif

  int c; 
  int outofrange_total = 0;
#ifdef HAVE_OPENMP
#pragma omp parallel for reduction(+ : outofrange_total)
#endif
  for (c = 0; c < unwarpedvol->width; c++)
  {
    // You could make a vector of CRS nthreads long
    MATRIX *unwarpedCRS = MatrixAlloc(4, 1, MATRIX_REAL);
    MATRIX *unwarpedRAS = MatrixAlloc(4, 1, MATRIX_REAL);;
    MATRIX *DeltaRAS    = MatrixAlloc(4, 1, MATRIX_REAL);
    MATRIX *warpedRAS   = MatrixAlloc(4, 1, MATRIX_REAL);
    MATRIX *warpedCRS   = MatrixAlloc(4, 1, MATRIX_REAL);

    int r = 0, s = 0;
    for (r = 0; r < unwarpedvol->height; r++)
    {
      for (s = 0; s < unwarpedvol->depth; s++)
      {
        // clear unwarpedCRS, unwarpedRAS, DeltaRAS, warpedRAS, warpedCRS
        MatrixClear(unwarpedCRS);
        MatrixClear(unwarpedRAS);
        MatrixClear(DeltaRAS);
        MatrixClear(warpedRAS);
        MatrixClear(warpedCRS);

        unwarpedCRS->rptr[1][1] = c;
        unwarpedCRS->rptr[2][1] = r;
        unwarpedCRS->rptr[3][1] = s;
        unwarpedCRS->rptr[4][1] = 1;

        // Convert the CRS to RAS
        unwarpedRAS->rptr[4][1] = 1;
        unwarpedRAS = MatrixMultiply(vox2ras, unwarpedCRS, unwarpedRAS);

        // convert RAS to LAI
        unwarpedRAS->rptr[1][1] = -unwarpedRAS->rptr[1][1];
        unwarpedRAS->rptr[2][1] =  unwarpedRAS->rptr[2][1];
        unwarpedRAS->rptr[3][1] = -unwarpedRAS->rptr[3][1];

        float Dx = 0, Dy = 0, Dz = 0;
        spharm_evaluate(unwarpedRAS->rptr[1][1], unwarpedRAS->rptr[2][1], unwarpedRAS->rptr[3][1], &Dx, &Dy, &Dz);

        DeltaRAS->rptr[1][1] = Dx;
        DeltaRAS->rptr[2][1] = Dy;
        DeltaRAS->rptr[3][1] = Dz;
        DeltaRAS->rptr[4][1] = 1; 
        
        warpedRAS = MatrixAdd(unwarpedRAS, DeltaRAS, warpedRAS);
        warpedRAS->rptr[4][1] = 1;

        // convert LAI to RAS
        warpedRAS->rptr[1][1] = -warpedRAS->rptr[1][1];
        warpedRAS->rptr[2][1] =  warpedRAS->rptr[2][1];
        warpedRAS->rptr[3][1] = -warpedRAS->rptr[3][1];

        warpedCRS = MatrixMultiply(inv_vox2ras, warpedRAS, warpedCRS);

        // (c, r, s) is in unwarped volume, (fcs, frs, fss) is in warped volume
        float fcs = warpedCRS->rptr[1][1];
        float frs = warpedCRS->rptr[2][1];
        float fss = warpedCRS->rptr[3][1];

        int ics =  nintfunc(fcs);
        int irs =  nintfunc(frs);
        int iss =  nintfunc(fss);

        if (ics < 0 || ics >= unwarpedvol->width  ||
            irs < 0 || irs >= unwarpedvol->height || 
            iss < 0 || iss >= unwarpedvol->depth)
        {
          outofrange_total++;
#if 0
#ifdef HAVE_OPENMP
          outofrange_local++;
#else
          outofrange_total++;
#endif
#endif
          continue;
        }

        //printf("%f => %f, %f => %f, %f => %f\n", (float)c, fcs, (float)r, frs, (float)s, fss);
        _assignUnWarpedVolumeValues(warpedvol, unwarpedvol, bspline, interpcode, sinchw, c, r, s, fcs, frs, fss);
      }   // s
    }     // r


#if 0
#ifdef HAVE_OPENMP
#pragma omp critical 
    outofrange_total += outofrange_local; 
    //printf("update out of range voxel count: + %d = %d\n", outofrange_local, outofrange_total);
#endif
#endif

    MatrixFree(&unwarpedCRS);
    MatrixFree(&unwarpedRAS);
    MatrixFree(&DeltaRAS);
    MatrixFree(&warpedRAS);
    MatrixFree(&warpedCRS);
  }       // c

  printf("Total %d voxels are out of range\n", outofrange_total);

  if (bspline)
    MRIfreeBSpline(&bspline);

  return unwarpedvol;
}

MRI *GradUnwarp::unwarp_volume(MRI *warpedvol, MRI *unwarpedvol, int interpcode, int sinchw)
{
  printf("GradUnwarp::unwarp_volume() ...\n");

  int (*nintfunc)( double );
  nintfunc = &nint;

  if (unwarpedvol == NULL)
  {
    unwarpedvol = MRIallocSequence(warpedvol->width, warpedvol->height, warpedvol->depth, MRI_FLOAT, warpedvol->nframes);
    MRIcopyHeader(warpedvol, unwarpedvol);
    MRIcopyPulseParameters(warpedvol, unwarpedvol);
  }

  MRI_BSPLINE *bspline = NULL;
  if (interpcode == SAMPLE_CUBIC_BSPLINE)
    bspline = MRItoBSpline(warpedvol, NULL, 3);

#ifdef HAVE_OPENMP
  printf("\nSet OPEN MP NUM threads to %d (unwarp_volume)\n", nthreads);
  omp_set_num_threads(nthreads);
#endif

  int c; 
  int outofrange_total = 0;
  int out_of_gcam;
#ifdef HAVE_OPENMP
#pragma omp parallel for reduction(+ : outofrange_total) 
#endif
  for (c = 0; c < unwarpedvol->width; c++)
  {
    int r = 0, s = 0;
    //int outofrange_local = 0;
    for (r = 0; r < unwarpedvol->height; r++)
    {
      for (s = 0; s < unwarpedvol->depth; s++)
      {
        float fcs = 0, frs = 0, fss = 0;
        int ics = 0, irs = 0, iss = 0;

        // (c, r, s) is in unwarped volume, (fcs, frs, fss) is in warped volume
	// (c, r, s) => (fcs, frs, fss)
        out_of_gcam = GCAMsampleMorph(gcam, (float)c, (float)r, (float)s, &fcs, &frs, &fss);

        ics =  nintfunc(fcs);
        irs =  nintfunc(frs);
        iss =  nintfunc(fss);

        if (ics < 0 || ics >= unwarpedvol->width  ||
            irs < 0 || irs >= unwarpedvol->height || 
            iss < 0 || iss >= unwarpedvol->depth)
        {
          outofrange_total++;
#if 0
#ifdef HAVE_OPENMP
          outofrange_local++;
#else
          outofrange_total++;
#endif
#endif
          continue;
        }

        //printf("%f => %f, %f => %f, %f => %f\n", (float)c, fcs, (float)r, frs, (float)s, fss);
        _assignUnWarpedVolumeValues(warpedvol, unwarpedvol, bspline, interpcode, sinchw, c, r, s, fcs, frs, fss);
      }   // s
    }     // r

#if 0
#ifdef HAVE_OPENMP
#pragma omp critical 
    outofrange_total += outofrange_local; 
    //printf("update out of range voxel count: + %d = %d\n", outofrange_local, outofrange_total);
#endif
#endif
  }       // c

  printf("Total %d voxels are out of range\n", outofrange_total);

  if (bspline)
    MRIfreeBSpline(&bspline);

  return unwarpedvol;
}

// (c, r, s) is in unwarped volume, (fcs, frs, fss) is in warped volume
// find value at (fcs, frs, fss) in warped volume,
// put it at (c, r, s) in unwarped volume
void GradUnwarp::_assignUnWarpedVolumeValues(MRI* warpedvol, MRI* unwarpedvol, MRI_BSPLINE *bspline, int interpcode, int sinchw, 
                                             int c, int r, int s, float fcs, float frs, float fss)
{
  int (*nintfunc)( double );
  nintfunc = &nint;

  int ics =  nintfunc(fcs);
  int irs =  nintfunc(frs);
  int iss =  nintfunc(fss);

  if (interpcode == SAMPLE_TRILINEAR) {
    float *valvect = new float[warpedvol->nframes];
    MRIsampleSeqVolume(warpedvol, fcs, frs, fss, valvect, 0, warpedvol->nframes - 1);

    int f;
    for (f = 0; f < warpedvol->nframes; f++)
      MRIsetVoxVal2(unwarpedvol, c, r, s, f, valvect[f]);

    free(valvect);
  } else {
    double rval = 0;

    int f;
    for (f = 0; f < warpedvol->nframes; f++) {
      switch (interpcode) {
        case SAMPLE_NEAREST:
          rval = MRIgetVoxVal2(warpedvol, ics, irs, iss, f);
          break;
        case SAMPLE_CUBIC_BSPLINE:
          MRIsampleBSpline(bspline, fcs, frs, fss, f, &rval);
          break;
        case SAMPLE_SINC: /* no multi-frame */
          MRIsincSampleVolume(warpedvol, fcs, frs, fss, sinchw, &rval);
          break;
        default:
          printf("ERROR: MR: interpolation method '%i' unknown\n", interpcode);
          exit(1);
      } // switch

      MRIsetVoxVal2(unwarpedvol, c, r, s, f, rval);
    } // f
  }
}

/*
 * MRIS *surf = MRISread('lh.white');
 * for n = 0:surf->nvertices-1
 *  VERTEX *v = surf->vertices[n]
 *     v->x, v->y, v->z // by default these are in the warped space
 *     tkRAS->rptr[1][1] = v->x;
 *     tkRAS->rptr[2][1] = v->y;
 *     tkRAS->rptr[3][1] = v->z;
 *     MATRIX *Tinv = TkrRAS2VoxfromVolGeom(&surf->vg); // convert from tkreg space to voxel
 *     MATRIX *S = vg_i_to_r(&surf->vg); // converts from voxel to RAS
 *     MATRIX *M = MatrixMultiply(S, Tinv, NULL); // convert from tkreg space to RAS
 *     DistortedRAS = MatrixMultiply(M, tkRAS, DistortedCRS);
 *     spharm_evaluate(Distx, Disty, Distz, &Dx, &Dy, &Dz);
 *     v->x +/- Dx;
 *     v->y +/- Dy;
 *     v->z +/- Dz;
 *
 *  (include/transform.h:#define vg_getVoxelToRasXform vg_i_to_r)
 */
MRIS* GradUnwarp::unwarp_surface_gradfile(MRIS *warpedsurf, MRIS *unwarpedsurf)
{
  printf("GradUnwarp::unwarp_surface_gradfile() ...\n");

  if (unwarpedsurf == NULL)
  {
    unwarpedsurf = MRISalloc(warpedsurf->nvertices, 0);
  }

#ifdef HAVE_OPENMP
  printf("\nSet OPEN MP NUM threads to %d (unwarp_surface_gradfile)\n", nthreads);
  omp_set_num_threads(nthreads);
#endif

  // to do: extract VOL_GEOM out of transform.h/transform.cpp
  //MATRIX *Tinv = warpedsurf->vg.getTkregRAS2Vox();       // tkreg space, RAS to VOX
  //MATRIX *S    = warpedsurf->vg.getVox2RAS();            // scanner space, VOX to RAS
  //MATRIX *Q    = warpedsurf->vg.getRAS2Vox();            // scanner space, RAS to VOX

  MATRIX *T    = TkrVox2RASfromVolGeom(&warpedsurf->vg); // tkreg space, VOX to RAS ???
  MATRIX *Tinv = TkrRAS2VoxfromVolGeom(&warpedsurf->vg); // tkreg space, RAS to VOX
  MATRIX *S    = vg_i_to_r(&warpedsurf->vg);             // scanner space, VOX to RAS
  MATRIX *Sinv = vg_r_to_i(&warpedsurf->vg);             // scanner space, RAS to VOX ???

  MATRIX *M    = MatrixMultiply(S, Tinv, NULL);        // RAS to RAS, tkreg space to scanner space
  MATRIX *Q    = MatrixMultiply(T, Sinv, NULL);        // RAS to RAS, scanner space to tkreg space ???

  if (getenv("GRADUNWARP_PRN_DEBUG"))
  {
    _printMatrix(Tinv, "tkreg space, RAS to VOX");                   // TkrRas2Vox
    _printMatrix(S,    "scanner space, VOX to RAS");                 // SVox2Ras
    _printMatrix(M,    "tkreg space to scanner space, RAS to RAS");  // Tkr2SRas2Ras
    _printMatrix(Q,    "scanner space to tkreg space, RAS to RAS");  // S2TkrRas2Ras
  }

  int n; 
#ifdef HAVE_OPENMP
#pragma omp parallel for
#endif
  for (n = 0; n < warpedsurf->nvertices; n++)
  {
    VERTEX *v = &warpedsurf->vertices[n];

    // v->x, v->y, v->z // by default these are in the warped space
    MATRIX *tkregRAS = MatrixAlloc(4, 1, MATRIX_REAL);
    tkregRAS->rptr[1][1] = v->x;
    tkregRAS->rptr[2][1] = v->y;
    tkregRAS->rptr[3][1] = v->z;
    tkregRAS->rptr[4][1] = 1;

    // Convert surface xyz coords from tkregister space to scanner space
    MATRIX *warpedRAS = MatrixMultiply(M, tkregRAS, NULL);

    // convert warpedRAS from RAS to LAI
    warpedRAS->rptr[1][1] = -warpedRAS->rptr[1][1];
    warpedRAS->rptr[2][1] =  warpedRAS->rptr[2][1];
    warpedRAS->rptr[3][1] = -warpedRAS->rptr[3][1];

    float  Dx = 0, Dy = 0, Dz = 0;
    spharm_evaluate(warpedRAS->rptr[1][1], warpedRAS->rptr[2][1], warpedRAS->rptr[3][1], &Dx, &Dy, &Dz);  //spharm_evaluate(Sx, Sy, Sz, &Dx, &Dy, &Dz);
    

    // warped => unwarped scanner xyz in LAI orientation
    MATRIX *unwarpedRAS = MatrixAlloc(4, 1, MATRIX_REAL);
    unwarpedRAS->rptr[1][1] = warpedRAS->rptr[1][1] - Dx; // + Dx, warping
    unwarpedRAS->rptr[2][1] = warpedRAS->rptr[2][1] - Dy; // + Dy, warping
    unwarpedRAS->rptr[3][1] = warpedRAS->rptr[3][1] - Dz; // + Dz, warping
    unwarpedRAS->rptr[4][1] = 1;

    // convert unwarpedRAS from LAI to RAS
    unwarpedRAS->rptr[1][1] = -unwarpedRAS->rptr[1][1];
    unwarpedRAS->rptr[2][1] =  unwarpedRAS->rptr[2][1];
    unwarpedRAS->rptr[3][1] = -unwarpedRAS->rptr[3][1];

    // convert unwarpedRAS from scanner space to tkregister space
    MATRIX *unwarpedtkregRAS = MatrixMultiply(Q, unwarpedRAS, NULL);

    // set unwarped vertext xyz
    MRISsetXYZ(unwarpedsurf, n, unwarpedtkregRAS->rptr[1][1], unwarpedtkregRAS->rptr[2][1], unwarpedtkregRAS->rptr[3][1]);

    if (getenv("GRADUNWARP_PRN_DEBUG"))
    {
      printf("%d) \n", n);
      printf("\ttkregRAS         (x=%f, y=%f, z=%f)\n", tkregRAS->rptr[1][1], tkregRAS->rptr[2][1], tkregRAS->rptr[3][1]); //v->x, v->y, v->z);
      printf("\twarpedRAS (LAI)  (x=%f, y=%f, z=%f)\n", warpedRAS->rptr[1][1], warpedRAS->rptr[2][1], warpedRAS->rptr[3][1]);  //Sx, Sy, Sz);
      printf("\tunwarpedRAS      (x=%f, y=%f, z=%f)\n", unwarpedRAS->rptr[1][1], unwarpedRAS->rptr[2][1], unwarpedRAS->rptr[3][1]);
      //printf("\tdeltaRAS         (x=%f, y=%f, z=%f)\n", Dx, Dy, Dz);
      printf("\tunwarpedtkregRAS (x=%f, y=%f, z=%f)\n", unwarpedtkregRAS->rptr[1][1], unwarpedtkregRAS->rptr[2][1], unwarpedtkregRAS->rptr[3][1]);
    }
    
    MatrixFree(&warpedRAS);
    MatrixFree(&unwarpedRAS);
    MatrixFree(&tkregRAS);
    MatrixFree(&unwarpedtkregRAS);
  }       // n

  // Copy the volume geometry
  //copyVolGeom(&(warpedsurf->vg), &(unwarpedsurf->vg));

  MatrixFree(&Q);
  MatrixFree(&M);
  MatrixFree(&S);
  MatrixFree(&Sinv);
  MatrixFree(&T);
  MatrixFree(&Tinv);

  return unwarpedsurf;
}

// using m3z transform table
MRIS* GradUnwarp::unwarp_surface(MRIS *warpedsurf, MRIS *unwarpedsurf)
{
  if (gcam == NULL)
  {
    printf("GCAM not initialized!\n");
    return NULL;
  }

  int (*nintfunc)( double );
  nintfunc = &nint;

  printf("GradUnwarp::unwarp_surface() ...\n");

  if (unwarpedsurf == NULL)
  {
    unwarpedsurf = MRISalloc(warpedsurf->nvertices, 0);
  }

  printf("create GCAM inverse ...\n");
  gcam->spacing = 1;
  MRI *tempMri = MRIallocFromVolGeom(&warpedsurf->vg, MRI_VOLUME_TYPE_UNKNOWN, 1, 1);
  GCAMinvert(gcam, tempMri);

#ifdef HAVE_OPENMP
  printf("\nSet OPEN MP NUM threads to %d (unwarp_surface)\n", nthreads);
  omp_set_num_threads(nthreads);
#endif

  // to do: extract VOL_GEOM out of transform.h/transform.cpp
  //MATRIX *Tinv = warpedsurf->vg.getTkregRAS2Vox();       // tkreg space, RAS to VOX
  //MATRIX *S    = warpedsurf->vg.getVox2RAS();            // scanner space, VOX to RAS

  MATRIX *T    = TkrVox2RASfromVolGeom(&warpedsurf->vg); // tkreg space, VOX to RAS ???
  MATRIX *Tinv = TkrRAS2VoxfromVolGeom(&warpedsurf->vg); // tkreg space, RAS to VOX
  MATRIX *S    = vg_i_to_r(&warpedsurf->vg);             // scanner space, VOX to RAS
  MATRIX *Sinv = vg_r_to_i(&warpedsurf->vg);             // scanner space, RAS to VOX ???

  MATRIX *M    = MatrixMultiply(S, Tinv, NULL);        // RAS to RAS, tkreg space to scanner space
  MATRIX *Q    = MatrixMultiply(T, Sinv, NULL);        // RAS to RAS, scanner space to tkreg space ???

  if (getenv("GRADUNWARP_PRN_DEBUG"))
  {
    _printMatrix(Tinv, "tkreg space, RAS to VOX");                   // TkrRas2Vox
    _printMatrix(S,    "scanner space, VOX to RAS");                 // SVox2Ras
    _printMatrix(M,    "tkreg space to scanner space, RAS to RAS");  // Tkr2SRas2Ras
    _printMatrix(Q,    "scanner space to tkreg space, RAS to RAS");  // S2TkrRas2Ras
  }

  int n; 
  int outofrange_total = 0;
#ifdef HAVE_OPENMP
#pragma omp parallel for reduction(+ : outofrange_total) 
#endif
  for (n = 0; n < warpedsurf->nvertices; n++)
  {
    VERTEX *v = &warpedsurf->vertices[n];

    // v->x, v->y, v->z // by default these are in the warped space
    MATRIX *tkregRAS = MatrixAlloc(4, 1, MATRIX_REAL);;
    tkregRAS->rptr[1][1] = v->x;
    tkregRAS->rptr[2][1] = v->y;
    tkregRAS->rptr[3][1] = v->z;
    tkregRAS->rptr[4][1] = 1;

    // Convert surface xyz coords from tkregister space to scanner space
    MATRIX *warpedRAS = MatrixMultiply(M, tkregRAS, NULL);

    // convert to surface xyz coords to CRS
    MATRIX *warpedCRS = MatrixMultiply(Tinv, tkregRAS, NULL);

    // warped CRS (c, r, s) => unwarped CRS (fcs, frs, fss)
    float fcs = 0, frs = 0, fss = 0;
    float c = warpedCRS->rptr[1][1];
    float r = warpedCRS->rptr[2][1];
    float s = warpedCRS->rptr[3][1];
    GCAMsampleInverseMorph(gcam, c, r, s, &fcs, &frs, &fss);

    // these are unwarped CRS
    int ics =  nintfunc(fcs);
    int irs =  nintfunc(frs);
    int iss =  nintfunc(fss);

    if (ics < 0 || ics >= unwarpedsurf->vg.width  ||
        irs < 0 || irs >= unwarpedsurf->vg.height  || 
        iss < 0 || iss >= unwarpedsurf->vg.depth)
    {
      outofrange_total++;
      continue;
    }

    // convert CRS to RAS in scanner space
    MATRIX *unwarpedCRS = MatrixAlloc(4, 1, MATRIX_REAL); 
    unwarpedCRS->rptr[1][1] = fcs;
    unwarpedCRS->rptr[2][1] = frs;
    unwarpedCRS->rptr[3][1] = fss;
    unwarpedCRS->rptr[4][1] = 1;

    MATRIX *unwarpedRAS = MatrixMultiply(S, unwarpedCRS, NULL);
    unwarpedRAS->rptr[4][1] = 1;
    MATRIX *unwarpedtkregRAS = MatrixMultiply(Q, unwarpedRAS, NULL);

    // set unwarped vertext xyz
    MRISsetXYZ(unwarpedsurf, n, unwarpedtkregRAS->rptr[1][1], unwarpedtkregRAS->rptr[2][1], unwarpedtkregRAS->rptr[3][1]);

    if (getenv("GRADUNWARP_PRN_DEBUG"))
    {
      printf("%d) \n", n);
      printf("\ttkregRAS         (x=%f, y=%f, z=%f)\n", tkregRAS->rptr[1][1],    tkregRAS->rptr[2][1], tkregRAS->rptr[3][1]); //v->x, v->y, v->z);
      printf("\twarpedRAS        (x=%f, y=%f, z=%f) (%f, %f, %f)\n", warpedRAS->rptr[1][1],   warpedRAS->rptr[2][1],   warpedRAS->rptr[3][1], c, r, s);
      printf("\tunwarpedRAS      (x=%f, y=%f, z=%f) (%f, %f, %f)\n", unwarpedRAS->rptr[1][1], unwarpedRAS->rptr[2][1], unwarpedRAS->rptr[3][1], fcs, frs, fss);
      //      printf("\tdeltaRAS    (x=%f, y=%f, z=%f)\n", Dx, Dy, Dz);
      printf("\tunwarpedtkregRAS (x=%f, y=%f, z=%f)\n", unwarpedtkregRAS->rptr[1][1], unwarpedtkregRAS->rptr[2][1], unwarpedtkregRAS->rptr[3][1]);
    }
 
    MatrixFree(&unwarpedRAS);
    MatrixFree(&unwarpedCRS);
    MatrixFree(&warpedRAS);
    MatrixFree(&warpedCRS);
    MatrixFree(&tkregRAS);
    MatrixFree(&unwarpedtkregRAS);
  }       // n

  // Copy the volume geometry
  //copyVolGeom(&(warpedsurf->vg), &(unwarpedsurf->vg));

  MatrixFree(&Q);
  MatrixFree(&M);
  MatrixFree(&S);
  MatrixFree(&Sinv);
  MatrixFree(&T);
  MatrixFree(&Tinv);

  return unwarpedsurf;
}

void GradUnwarp::_printMatrix(MATRIX *matrix, const char *desc)
{
  printf("\n%s\n", desc);

  MatrixPrint(stdout, matrix);
}

void GradUnwarp::_skipCoeffComment()
{
  char line[1024];
  while (fgets(line, sizeof(line), fgrad) != NULL)
  {
    int len = strlen(line);
    char* ptr = line;
    char* endptr = ptr + len;

    // skip leading spaces and tabs, also empty lines
    while (ptr != endptr && (*ptr == ' ' || *ptr == '\t' || *ptr == '\n'))
      ptr++;

    // skip the comment lines. The comment section ends with line #*] END: 
    if (*ptr != '\0' && strncmp(ptr, "#*] END:", 8) == 0)
      return;
  }
}

void GradUnwarp::_initCoeff()
{
  printf("nmax = %d, coeffDim = %d, coeffCount = %d\n", nmax, coeffDim, coeffCount);

  // allocate float *Alpha_x, *Alpha_y, *Alpha_z; float *Beta_x,  *Beta_y,  *Beta_z;
  Alpha_x = new float*[coeffDim];
  Alpha_y = new float*[coeffDim];
  Alpha_z = new float*[coeffDim];
  Beta_x  = new float*[coeffDim];
  Beta_y  = new float*[coeffDim];
  Beta_z  = new float*[coeffDim];

  // initialize coefficient arrays
  int i;
  for (i = 0; i < coeffDim; i++)
  {
    Alpha_x[i] = new float[coeffDim];
    memset(Alpha_x[i], 0, sizeof(float)*coeffDim);

    Alpha_y[i] = new float[coeffDim];
    memset(Alpha_y[i], 0, sizeof(float)*coeffDim);

    Alpha_z[i] = new float[coeffDim];
    memset(Alpha_z[i], 0, sizeof(float)*coeffDim);

    Beta_x[i]  = new float[coeffDim];
    memset(Beta_x[i], 0, sizeof(float)*coeffDim);

    Beta_y[i]  = new float[coeffDim];
    memset(Beta_y[i], 0, sizeof(float)*coeffDim);

    Beta_z[i]  = new float[coeffDim];
    memset(Beta_z[i], 0, sizeof(float)*coeffDim);
  }
}



/***********************************************************************************/
/************************ Implementation of Siemens_B class ************************/
Siemens_B::Siemens_B(int coeffDim0, int nmax0, float R0, double **normfact0, float X, float Y, float Z)
{
  coeffDim = coeffDim0;    // coeffDime = nmax + 1
  nmax = nmax0;
  R0_mm = R0;
  normfact = normfact0;

  P = new double*[coeffDim];

  int n;
  for (n = 0; n < coeffDim; n++)
  {
    P[n] = new double[coeffDim];
    memset(P[n], 0, (coeffDim)*sizeof(double));
  }

  F = new float[coeffDim];
  cosPhi = new double[coeffDim];
  sinPhi = new double[coeffDim];

  memset(F, 0, coeffDim*sizeof(float));
  memset(cosPhi, 0, coeffDim*sizeof(double));
  memset(sinPhi, 0, coeffDim*sizeof(double));

  // hack to avoid singularities at origin (R==0)
  X = X+0.0001;

  // convert to spherical coordinates
  R = sqrt(X*X + Y*Y + Z*Z);
  Theta = acos(Z/R);
  Phi = atan2(Y/R, X/R);

  // evaluate the Legendre polynomial (using Siemens's normalization)
  for (n = 0; n < coeffDim; n++)
  {
    siemens_legendre(n, cos(Theta));

    F[n] = pow((R/R0_mm), n);
    cosPhi[n] = cos(n*Phi);
    sinPhi[n] = sin(n*Phi);
  }
}

Siemens_B::~Siemens_B()
{
  int n;
  for (n = 0; n < coeffDim; n++)
    free(P[n]);

  free(P);
  free(F);
  free(cosPhi);
  free(sinPhi);
}

float Siemens_B::siemens_B_x(float **Alpha_x, float **Beta_x)
{
  return Siemens_B::siemens_B(Alpha_x, Beta_x);
}

float Siemens_B::siemens_B_y(float **Alpha_y, float **Beta_y)
{
  return Siemens_B::siemens_B(Alpha_y, Beta_y);
}

float Siemens_B::siemens_B_z(float **Alpha_z, float **Beta_z)
{
  return Siemens_B::siemens_B(Alpha_z, Beta_z);
}

float Siemens_B::siemens_B(float **Alpha, float **Beta)
{
  float B = 0;
  int n;
  for (n = 0; n < coeffDim; n++)
  {
    int m;
    for (m = 0; m <= n; m++)
    {
      float F2 = Alpha[n][m] * cosPhi[m] + Beta[n][m] * sinPhi[m];
      B = B + F[n]*P[n][m]*F2;
    }
  }

  return B;
}

void Siemens_B::siemens_legendre(int n, double x)
{
  int m;
  for (m = 0; m <= n; m++)
  {
    P[n][m] = gsl_sf_legendre_Plm_e(n, m, x);
  }

  if (getenv("GRADUNWARP_PRN_LEGENDRE"))
  {
    printf("\nlegendre (n=%d, x=%lf) = \n", n, x);
    for (m = 0; m <= n; m++)
      printf("\tP[%d][%d] = %lf\n", n, m, P[n][m]);
  }

  for (m = 0; m < n; m++)
  {
    P[n][m+1] *= normfact[n][m];
  }

  if (getenv("GRADUNWARP_PRN_LEGENDRE"))
  {
    printf("\nsiemens_legendre (n=%d, x=%lf) = \n", n, x);
    for (m = 0; m <= n; m++)
      printf("\tP[%d][%d] = %lf\n", n, m, P[n][m]);
  }
}
