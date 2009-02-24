#include "copyright.h"
/*==============================================================================
 * FILE: integrate_3d_ctu.c
 *
 * PURPOSE: Integrate MHD equations in 2D using the directionally unsplit CTU
 *   method of Colella (1990).  The variables updated are:
 *      U.[d,M1,M2,M3,E,B1c,B2c,B3c,s] -- where U is of type Gas
 *      B1i, B2i, B3i  -- interface magnetic field
 *   Also adds gravitational source terms, self-gravity, optically thin cooling,
 *   and the H-correction of Sanders et al.
 *     For adb hydro, requires (9*Cons1D +  3*Real) = 48 3D arrays
 *     For adb mhd, requires   (9*Cons1D + 10*Real) = 73 3D arrays
 *   The H-correction of Sanders et al. adds another 3 arrays.  
 *
 * REFERENCES:
 *   P. Colella, "Multidimensional upwind methods for hyperbolic conservation
 *   laws", JCP, 87, 171 (1990)
 *
 *   T. Gardiner & J.M. Stone, "An unsplit Godunov method for ideal MHD via
 *   constrained transport in three dimensions", JCP, 227, 4123 (2008)
 *
 *   R. Sanders, E. Morano, & M.-C. Druguet, "Multidimensinal dissipation for
 *   upwind schemes: stability and applications to gas dynamics", JCP, 145, 511
 *   (1998)
 *
 *   J.M. Stone et al., "Athena: A new code for astrophysical MHD", ApJS,
 *   178, 137 (2008)
 *
 * CONTAINS PUBLIC FUNCTIONS: 
 *   integrate_3d_ctu()
 *   integrate_init_3d()
 *   integrate_destruct_3d()
 *============================================================================*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defs.h"
#include "athena.h"
#include "globals.h"
#include "prototypes.h"

/* The L/R states of conserved variables and fluxes at each cell face */
static Cons1D ***Ul_x1Face=NULL, ***Ur_x1Face=NULL;
static Cons1D ***Ul_x2Face=NULL, ***Ur_x2Face=NULL;
static Cons1D ***Ul_x3Face=NULL, ***Ur_x3Face=NULL;
Cons1D ***x1Flux=NULL, ***x2Flux=NULL, ***x3Flux=NULL;

/* The interface magnetic fields and emfs */
#ifdef MHD
static Real ***B1_x1Face=NULL, ***B2_x2Face=NULL, ***B3_x3Face=NULL;
Real ***emf1=NULL, ***emf2=NULL, ***emf3=NULL;
static Real ***emf1_cc=NULL, ***emf2_cc=NULL, ***emf3_cc=NULL;
#endif /* MHD */

/* 1D scratch vectors used by lr_states and flux functions */
#ifdef MHD
static Real *Bxc=NULL, *Bxi=NULL;
#endif /* MHD */
static Prim1D *W=NULL, *Wl=NULL, *Wr=NULL;
static Cons1D *U1d=NULL;

/* density and Pressure at t^{n+1/2} needed by MHD, cooling, and gravity */
static Real ***dhalf = NULL, ***phalf=NULL;

/* variables needed for H-correction of Sanders et al (1998) */
extern Real etah;
#ifdef H_CORRECTION
static Real ***eta1=NULL, ***eta2=NULL, ***eta3=NULL;
#endif

/* variables needed to conserve net Bz in shearing box */
#ifdef SHEARING_BOX
static Real **remapEyiib=NULL, **remapEyoib=NULL;
#endif

/*==============================================================================
 * PRIVATE FUNCTION PROTOTYPES: 
 *   integrate_emf1_corner() - the upwind CT method in GS05, for emf1
 *   integrate_emf2_corner() - the upwind CT method in GS05, for emf2
 *   integrate_emf3_corner() - the upwind CT method in GS05, for emf3
 *============================================================================*/

#ifdef MHD
static void integrate_emf1_corner(const Grid *pG);
static void integrate_emf2_corner(const Grid *pG);
static void integrate_emf3_corner(const Grid *pG);
#endif /* MHD */

/*=========================== PUBLIC FUNCTIONS ===============================*/
/*----------------------------------------------------------------------------*/
/* integrate_3d: 3D CTU integrator for MHD using 6-solve method */

void integrate_3d_ctu(Grid *pG, Domain *pD)
{
  Real dtodx1=pG->dt/pG->dx1, dtodx2=pG->dt/pG->dx2, dtodx3=pG->dt/pG->dx3;
  Real dx1i=1.0/pG->dx1, dx2i=1.0/pG->dx2, dx3i=1.0/pG->dx3;
  Real q1 = 0.5*dtodx1, q2 = 0.5*dtodx2, q3 = 0.5*dtodx3;
  int i, is = pG->is, ie = pG->ie;
  int j, js = pG->js, je = pG->je;
  int k, ks = pG->ks, ke = pG->ke;
  Real x1,x2,x3,phicl,phicr,phifc,phil,phir,phic;
  Real coolfl,coolfr,coolf,M1h,M2h,M3h,Eh=0.0;
#ifdef MHD
  Real MHD_src_By,MHD_src_Bz,mdb1,mdb2,mdb3;
  Real db1,db2,db3,l1,l2,l3,B1,B2,B3,V1,V2,V3;
  Real B1ch,B2ch,B3ch;
  Real hdt = 0.5*pG->dt;
#endif
#ifdef H_CORRECTION
  Real cfr,cfl,lambdar,lambdal;
#endif
#if (NSCALARS > 0)
  int n;
#endif
#ifdef SELF_GRAVITY
  Real gxl,gxr,gyl,gyr,gzl,gzr,flx_m1l,flx_m1r,flx_m2l,flx_m2r,flx_m3l,flx_m3r;
#endif
#ifdef SHEARING_BOX
  int my_iproc,my_jproc,my_kproc;
  Real M1n, dM2n; /* M1, dM2=(My+d*1.5*Omega*x) at time n */
  Real M1e, dM2e; /* M1, dM2 evolved by dt/2 */
  Real flx1_dM2, frx1_dM2, flx2_dM2, frx2_dM2, flx3_dM2, frx3_dM2;
  Real fact, TH_om, om_dt = Omega*pG->dt;
#endif /* SHEARING_BOX */

/*=== STEP 1: Compute L/R x1-interface states and 1D x1-Fluxes ===============*/

/*--- Step 1a ------------------------------------------------------------------
 * Load 1D vector of conserved variables;
 * U1d = (d, M1, M2, M3, E, B2c, B3c, s[n])
 */

  for (k=ks-2; k<=ke+2; k++) {
    for (j=js-2; j<=je+2; j++) {
      for (i=is-nghost; i<=ie+nghost; i++) {
        U1d[i].d  = pG->U[k][j][i].d;
        U1d[i].Mx = pG->U[k][j][i].M1;
        U1d[i].My = pG->U[k][j][i].M2;
        U1d[i].Mz = pG->U[k][j][i].M3;
#ifndef BAROTROPIC
        U1d[i].E  = pG->U[k][j][i].E;
#endif /* BAROTROPIC */
#ifdef MHD
        U1d[i].By = pG->U[k][j][i].B2c;
        U1d[i].Bz = pG->U[k][j][i].B3c;
        Bxc[i] = pG->U[k][j][i].B1c;
        Bxi[i] = pG->B1i[k][j][i];
        B1_x1Face[k][j][i] = pG->B1i[k][j][i];
#endif /* MHD */
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) U1d[i].s[n] = pG->U[k][j][i].s[n];
#endif
      }

/*--- Step 1b ------------------------------------------------------------------
 * Compute L and R states at X1-interfaces, add "MHD source terms" for 0.5*dt
 */

     for (i=is-nghost; i<=ie+nghost; i++) {
       Cons1D_to_Prim1D(&U1d[i],&W[i] MHDARG( , &Bxc[i]));
     }

     lr_states(W, MHDARG( Bxc , ) pG->dt,dtodx1,is-1,ie+1,Wl,Wr);

#ifdef MHD
      for (i=is-1; i<=ie+2; i++) {
/* Source terms for left states in zone i-1 */
        db1 = (pG->B1i[k  ][j  ][i  ] - pG->B1i[k][j][i-1])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i-1] - pG->B2i[k][j][i-1])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i-1] - pG->B3i[k][j][i-1])*dx3i;

	if(db1 >= 0.0){
	  l3 = db1 < -db3 ? db1 : -db3;
	  l3 = l3 > 0.0 ? l3 : 0.0;

	  l2 = db1 < -db2 ? db1 : -db2;
	  l2 = l2 > 0.0 ? l2 : 0.0;
	}
	else{
	  l3 = db1 > -db3 ? db1 : -db3;
	  l3 = l3 < 0.0 ? l3 : 0.0;

	  l2 = db1 > -db2 ? db1 : -db2;
	  l2 = l2 < 0.0 ? l2 : 0.0;
	}

        MHD_src_By = (pG->U[k][j][i-1].M2/pG->U[k][j][i-1].d)*l2;
        MHD_src_Bz = (pG->U[k][j][i-1].M3/pG->U[k][j][i-1].d)*l3;

        Wl[i].By += hdt*MHD_src_By;
        Wl[i].Bz += hdt*MHD_src_Bz;

/* Source terms for right states in zone i */
        db1 = (pG->B1i[k  ][j  ][i+1] - pG->B1i[k][j][i])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i  ] - pG->B2i[k][j][i])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i  ] - pG->B3i[k][j][i])*dx3i;

        if(db1 >= 0.0){
          l3 = db1 < -db3 ? db1 : -db3;
          l3 = l3 > 0.0 ? l3 : 0.0;

          l2 = db1 < -db2 ? db1 : -db2;
          l2 = l2 > 0.0 ? l2 : 0.0;
        }
        else{
          l3 = db1 > -db3 ? db1 : -db3;
          l3 = l3 < 0.0 ? l3 : 0.0;

          l2 = db1 > -db2 ? db1 : -db2;
          l2 = l2 < 0.0 ? l2 : 0.0;
        }

        MHD_src_By = (pG->U[k][j][i].M2/pG->U[k][j][i].d)*l2;
        MHD_src_Bz = (pG->U[k][j][i].M3/pG->U[k][j][i].d)*l3;

        Wr[i].By += hdt*MHD_src_By;
        Wr[i].Bz += hdt*MHD_src_Bz;
      }
#endif

/*--- Step 1c ------------------------------------------------------------------
 * Add source terms from static gravitational potential for 0.5*dt to L/R states
 */

      if (StaticGravPot != NULL){
        for (i=is-1; i<=ie+2; i++) {
          cc_pos(pG,i,j,k,&x1,&x2,&x3);
          phicr = (*StaticGravPot)( x1             ,x2,x3);
          phicl = (*StaticGravPot)((x1-    pG->dx1),x2,x3);
          phifc = (*StaticGravPot)((x1-0.5*pG->dx1),x2,x3);

          Wl[i].Vx -= dtodx1*(phifc - phicl);
          Wr[i].Vx -= dtodx1*(phicr - phifc);
        }
      }

/*--- Step 1c (cont) -----------------------------------------------------------
 * Add source terms for self-gravity for 0.5*dt to L/R states
 */

#ifdef SELF_GRAVITY
      for (i=is-1; i<=ie+2; i++) {
        Wl[i].Vx -= q1*(pG->Phi[k][j][i] - pG->Phi[k][j][i-1]);
        Wr[i].Vx -= q1*(pG->Phi[k][j][i] - pG->Phi[k][j][i-1]);
      }
#endif

/*--- Step 1c (cont) -----------------------------------------------------------
 * Add source terms from optically-thin cooling for 0.5*dt to L/R states
 */

#ifndef BAROTROPIC
      if (CoolingFunc != NULL){
        for (i=is-1; i<=ie+2; i++) {
          coolfl = (*CoolingFunc)(Wl[i].d,Wl[i].P,(0.5*pG->dt));
          coolfr = (*CoolingFunc)(Wr[i].d,Wr[i].P,(0.5*pG->dt));

          Wl[i].P -= 0.5*pG->dt*Gamma_1*coolfl;
          Wr[i].P -= 0.5*pG->dt*Gamma_1*coolfr;
        }
      }
#endif /* BAROTROPIC */

/*--- Step 1c (cont) -----------------------------------------------------------
 * Add source terms for shearing box (Coriolis forces) for 0.5*dt to L/R states
 */

#ifdef SHEARING_BOX
      for (i=is-1; i<=ie+2; i++) {
	Wl[i].Vx += pG->dt*Omega*W[i-1].Vy; /* (dt/2)*( 2 Omega Vy) */
#ifdef FARGO
	Wl[i].Vy -= 0.25*pG->dt*Omega*W[i-1].Vx; /* (dt/2)*(-1/2 Omega Vx) */
#else
	Wl[i].Vy -= pG->dt*Omega*W[i-1].Vx; /* (dt/2)*(-2 Omega Vx) */
#endif

	Wr[i].Vx += pG->dt*Omega*W[i].Vy; /* (dt/2)*( 2 Omega Vy) */
#ifdef FARGO
	Wr[i].Vy -= 0.25*pG->dt*Omega*W[i].Vx; /* (dt/2)*(-1/2 Omega Vx) */
#else
	Wr[i].Vy -= pG->dt*Omega*W[i].Vx; /* (dt/2)*(-2 Omega Vx) */
#endif
      }
#endif /* SHEARING_BOX */

/*--- Step 1d ------------------------------------------------------------------
 * Compute 1D fluxes in x1-direction, storing into 3D array
 */

      for (i=is-1; i<=ie+2; i++) {
        Prim1D_to_Cons1D(&Ul_x1Face[k][j][i],&Wl[i] MHDARG( , &Bxi[i]));
        Prim1D_to_Cons1D(&Ur_x1Face[k][j][i],&Wr[i] MHDARG( , &Bxi[i]));

        GET_FLUXES(Ul_x1Face[k][j][i],Ur_x1Face[k][j][i],Wl[i],Wr[i],
                   MHDARG( B1_x1Face[k][j][i] , ) &x1Flux[k][j][i]);
      }
    }
  }

/*=== STEP 2: Compute L/R x2-interface states and 1D x2-Fluxes ===============*/

/*--- Step 2a ------------------------------------------------------------------
 * Load 1D vector of conserved variables;
 * U1d = (d, M2, M3, M1, E, B3c, B1c, s[n])
 */

  for (k=ks-2; k<=ke+2; k++) {
    for (i=is-2; i<=ie+2; i++) {
      for (j=js-nghost; j<=je+nghost; j++) {
        U1d[j].d  = pG->U[k][j][i].d;
        U1d[j].Mx = pG->U[k][j][i].M2;
        U1d[j].My = pG->U[k][j][i].M3;
        U1d[j].Mz = pG->U[k][j][i].M1;
#ifndef BAROTROPIC
        U1d[j].E  = pG->U[k][j][i].E;
#endif /* BAROTROPIC */
#ifdef MHD
        U1d[j].By = pG->U[k][j][i].B3c;
        U1d[j].Bz = pG->U[k][j][i].B1c;
        Bxc[j] = pG->U[k][j][i].B2c;
        Bxi[j] = pG->B2i[k][j][i];
        B2_x2Face[k][j][i] = pG->B2i[k][j][i];
#endif /* MHD */
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) U1d[j].s[n] = pG->U[k][j][i].s[n];
#endif
      }

/*--- Step 2b ------------------------------------------------------------------
 * Compute L and R states at X2-interfaces, add "MHD source terms" for 0.5*dt
 */

      for (j=js-nghost; j<=je+nghost; j++) {
        Cons1D_to_Prim1D(&U1d[j],&W[j] MHDARG( , &Bxc[j]));
      }

      lr_states(W, MHDARG( Bxc , ) pG->dt,dtodx2,js-1,je+1,Wl,Wr);

#ifdef MHD
      for (j=js-1; j<=je+2; j++) {
/* Source terms for left states in zone j-1 */
        db1 = (pG->B1i[k  ][j-1][i+1] - pG->B1i[k][j-1][i])*dx1i;
        db2 = (pG->B2i[k  ][j  ][i  ] - pG->B2i[k][j-1][i])*dx2i;
        db3 = (pG->B3i[k+1][j-1][i  ] - pG->B3i[k][j-1][i])*dx3i;

	if(db2 >= 0.0){
	  l1 = db2 < -db1 ? db2 : -db1;
	  l1 = l1 > 0.0 ? l1 : 0.0;

	  l3 = db2 < -db3 ? db2 : -db3;
	  l3 = l3 > 0.0 ? l3 : 0.0;
	}
	else{
	  l1 = db2 > -db1 ? db2 : -db1;
	  l1 = l1 < 0.0 ? l1 : 0.0;

	  l3 = db2 > -db3 ? db2 : -db3;
	  l3 = l3 < 0.0 ? l3 : 0.0;
	}

	MHD_src_By = (pG->U[k][j-1][i].M3/pG->U[k][j-1][i].d)*l3;
	MHD_src_Bz = (pG->U[k][j-1][i].M1/pG->U[k][j-1][i].d)*l1;

        Wl[j].By += hdt*MHD_src_By;
        Wl[j].Bz += hdt*MHD_src_Bz;

/* Source terms for right states in zone j */
        db1 = (pG->B1i[k  ][j  ][i+1] - pG->B1i[k][j][i])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i  ] - pG->B2i[k][j][i])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i  ] - pG->B3i[k][j][i])*dx3i;

        if(db2 >= 0.0){
          l1 = db2 < -db1 ? db2 : -db1;
          l1 = l1 > 0.0 ? l1 : 0.0;

          l3 = db2 < -db3 ? db2 : -db3;
          l3 = l3 > 0.0 ? l3 : 0.0;
        }
        else{
          l1 = db2 > -db1 ? db2 : -db1;
          l1 = l1 < 0.0 ? l1 : 0.0;

          l3 = db2 > -db3 ? db2 : -db3;
          l3 = l3 < 0.0 ? l3 : 0.0;
        }

        MHD_src_By = (pG->U[k][j][i].M3/pG->U[k][j][i].d)*l3;
        MHD_src_Bz = (pG->U[k][j][i].M1/pG->U[k][j][i].d)*l1;

        Wr[j].By += hdt*MHD_src_By;
        Wr[j].Bz += hdt*MHD_src_Bz;
      }
#endif

/*--- Step 2c ------------------------------------------------------------------
 * Add source terms from static gravitational potential for 0.5*dt to L/R states
 */

      if (StaticGravPot != NULL){
        for (j=js-1; j<=je+2; j++) {
          cc_pos(pG,i,j,k,&x1,&x2,&x3);
          phicr = (*StaticGravPot)(x1, x2             ,x3);
          phicl = (*StaticGravPot)(x1,(x2-    pG->dx2),x3);
          phifc = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),x3);

          Wl[j].Vx -= dtodx2*(phifc - phicl);
          Wr[j].Vx -= dtodx2*(phicr - phifc);
        }
      }

/*--- Step 2c (cont) -----------------------------------------------------------
 * Add source terms for self-gravity for 0.5*dt to L/R states
 */

#ifdef SELF_GRAVITY
      for (j=js-1; j<=je+2; j++) {
        Wl[j].Vx -= q2*(pG->Phi[k][j][i] - pG->Phi[k][j-1][i]);
        Wr[j].Vx -= q2*(pG->Phi[k][j][i] - pG->Phi[k][j-1][i]);
      }
#endif

/*--- Step 2c (cont) -----------------------------------------------------------
 * Add source terms from optically-thin cooling for 0.5*dt to L/R states
 */

#ifndef BAROTROPIC
      if (CoolingFunc != NULL){
        for (j=js-1; j<=je+2; j++) {
          coolfl = (*CoolingFunc)(Wl[j].d,Wl[j].P,(0.5*pG->dt));
          coolfr = (*CoolingFunc)(Wr[j].d,Wr[j].P,(0.5*pG->dt));

          Wl[j].P -= 0.5*pG->dt*Gamma_1*coolfl;
          Wr[j].P -= 0.5*pG->dt*Gamma_1*coolfr;
        }
      }
#endif /* BAROTROPIC */

/*--- Step 2d ------------------------------------------------------------------
 * Compute 1D fluxes in x2-direction, storing into 3D array
 */

      for (j=js-1; j<=je+2; j++) {
        Prim1D_to_Cons1D(&Ul_x2Face[k][j][i],&Wl[j] MHDARG( , &Bxi[j]));
        Prim1D_to_Cons1D(&Ur_x2Face[k][j][i],&Wr[j] MHDARG( , &Bxi[j]));

        GET_FLUXES(Ul_x2Face[k][j][i],Ur_x2Face[k][j][i],Wl[j],Wr[j],
                   MHDARG( B2_x2Face[k][j][i] , ) &x2Flux[k][j][i]);
      }
    }
  }

/*=== STEP 3: Compute L/R x3-interface states and 1D x3-Fluxes ===============*/

/*--- Step 3a ------------------------------------------------------------------
 * Load 1D vector of conserved variables;
 * U1d = (d, M3, M1, M2, E, B1c, B2c, s[n])
 */

  for (j=js-2; j<=je+2; j++) {
    for (i=is-2; i<=ie+2; i++) {
      for (k=ks-nghost; k<=ke+nghost; k++) {
        U1d[k].d  = pG->U[k][j][i].d;
        U1d[k].Mx = pG->U[k][j][i].M3;
        U1d[k].My = pG->U[k][j][i].M1;
        U1d[k].Mz = pG->U[k][j][i].M2;
#ifndef BAROTROPIC
        U1d[k].E  = pG->U[k][j][i].E;
#endif /* BAROTROPIC */
#ifdef MHD
        U1d[k].By = pG->U[k][j][i].B1c;
        U1d[k].Bz = pG->U[k][j][i].B2c;
        Bxc[k] = pG->U[k][j][i].B3c;
        Bxi[k] = pG->B3i[k][j][i];
        B3_x3Face[k][j][i] = pG->B3i[k][j][i];
#endif /* MHD */
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) U1d[k].s[n] = pG->U[k][j][i].s[n];
#endif
      }

/*--- Step 3b ------------------------------------------------------------------
 * Compute L and R states at X3-interfaces, add "MHD source terms" for 0.5*dt
 */

      for (k=ks-nghost; k<=ke+nghost; k++) {
        Cons1D_to_Prim1D(&U1d[k],&W[k] MHDARG( , &Bxc[k]));
      }

      lr_states(W, MHDARG( Bxc , ) pG->dt,dtodx3,ks-1,ke+1,Wl,Wr);

#ifdef MHD
      for (k=ks-1; k<=ke+2; k++) {
/* Source terms for left states in zone k-1 */
        db1 = (pG->B1i[k-1][j  ][i+1] - pG->B1i[k-1][j][i])*dx1i;
        db2 = (pG->B2i[k-1][j+1][i  ] - pG->B2i[k-1][j][i])*dx2i;
        db3 = (pG->B3i[k  ][j  ][i  ] - pG->B3i[k-1][j][i])*dx3i;

	if(db3 >= 0.0){
	  l1 = db3 < -db1 ? db3 : -db1;
	  l1 = l1 > 0.0 ? l1 : 0.0;

	  l2 = db3 < -db2 ? db3 : -db2;
	  l2 = l2 > 0.0 ? l2 : 0.0;
	}
	else{
	  l1 = db3 > -db1 ? db3 : -db1;
	  l1 = l1 < 0.0 ? l1 : 0.0;

	  l2 = db3 > -db2 ? db3 : -db2;
	  l2 = l2 < 0.0 ? l2 : 0.0;
	}

	MHD_src_By = (pG->U[k-1][j][i].M1/pG->U[k-1][j][i].d)*l1;
	MHD_src_Bz = (pG->U[k-1][j][i].M2/pG->U[k-1][j][i].d)*l2;

        Wl[k].By += hdt*MHD_src_By;
        Wl[k].Bz += hdt*MHD_src_Bz;

/* Source terms for right states in zone k */
        db1 = (pG->B1i[k][j][i+1] - pG->B1i[k][j][i])*dx1i;
        db2 = (pG->B2i[k][j+1][i] - pG->B2i[k][j][i])*dx2i;
        db3 = (pG->B3i[k+1][j][i] - pG->B3i[k][j][i])*dx3i;

        if(db3 >= 0.0){
          l1 = db3 < -db1 ? db3 : -db1;
          l1 = l1 > 0.0 ? l1 : 0.0;

          l2 = db3 < -db2 ? db3 : -db2;
          l2 = l2 > 0.0 ? l2 : 0.0;
        }
        else{
          l1 = db3 > -db1 ? db3 : -db1;
          l1 = l1 < 0.0 ? l1 : 0.0;

          l2 = db3 > -db2 ? db3 : -db2;
          l2 = l2 < 0.0 ? l2 : 0.0;
        }

        MHD_src_By = (pG->U[k][j][i].M1/pG->U[k][j][i].d)*l1;
        MHD_src_Bz = (pG->U[k][j][i].M2/pG->U[k][j][i].d)*l2;

        Wr[k].By += hdt*MHD_src_By;
        Wr[k].Bz += hdt*MHD_src_Bz;
      }
#endif

/*--- Step 3c ------------------------------------------------------------------
 * Add source terms from static gravitational potential for 0.5*dt to L/R states
 */

      if (StaticGravPot != NULL){
        for (k=ks-1; k<=ke+2; k++) {
          cc_pos(pG,i,j,k,&x1,&x2,&x3);
          phicr = (*StaticGravPot)(x1,x2, x3             );
          phicl = (*StaticGravPot)(x1,x2,(x3-    pG->dx3));
          phifc = (*StaticGravPot)(x1,x2,(x3-0.5*pG->dx3));

          Wl[k].Vx -= dtodx3*(phifc - phicl);
          Wr[k].Vx -= dtodx3*(phicr - phifc);
        }
      }

/*--- Step 3c (cont) -----------------------------------------------------------
 * Add source terms for self-gravity for 0.5*dt to L/R states
 */

#ifdef SELF_GRAVITY
      for (k=ks-1; k<=ke+2; k++) {
        Wl[k].Vx -= q3*(pG->Phi[k][j][i] - pG->Phi[k-1][j][i]);
        Wr[k].Vx -= q3*(pG->Phi[k][j][i] - pG->Phi[k-1][j][i]);
      }
#endif

/*--- Step 3c (cont) -----------------------------------------------------------
 * Add source terms from optically-thin cooling for 0.5*dt to L/R states
 */

#ifndef BAROTROPIC
      if (CoolingFunc != NULL){
        for (k=ks-1; k<=ke+2; k++) {
          coolfl = (*CoolingFunc)(Wl[k].d,Wl[k].P,(0.5*pG->dt));
          coolfr = (*CoolingFunc)(Wr[k].d,Wr[k].P,(0.5*pG->dt));
  
          Wl[k].P -= 0.5*pG->dt*Gamma_1*coolfl;
          Wr[k].P -= 0.5*pG->dt*Gamma_1*coolfr;
        }
      }
#endif /* BAROTROPIC */

/*--- Step 3d ------------------------------------------------------------------
 * Compute 1D fluxes in x3-direction, storing into 3D array
 */

      for (k=ks-1; k<=ke+2; k++) {
        Prim1D_to_Cons1D(&Ul_x3Face[k][j][i],&Wl[k] MHDARG( , &Bxi[k]));
        Prim1D_to_Cons1D(&Ur_x3Face[k][j][i],&Wr[k] MHDARG( , &Bxi[k]));

        GET_FLUXES(Ul_x3Face[k][j][i],Ur_x3Face[k][j][i],Wl[k],Wr[k],
                   MHDARG( B3_x3Face[k][j][i] , ) &x3Flux[k][j][i]);
      }
    }
  }

/*=== STEP 4:  Update face-centered B for 0.5*dt =============================*/

/*--- Step 4a ------------------------------------------------------------------
 * Calculate the cell centered value of emf1,2,3 at t^{n} and integrate
 * to corner.
 */

#ifdef MHD
/* emf1 */
  for (k=ks-2; k<=ke+2; k++) {
    for (j=js-2; j<=je+2; j++) {
      for (i=is-2; i<=ie+2; i++) {
        emf1_cc[k][j][i] = (pG->U[k][j][i].B2c*pG->U[k][j][i].M3 -
			    pG->U[k][j][i].B3c*pG->U[k][j][i].M2)
                              /pG->U[k][j][i].d;
        emf2_cc[k][j][i] = (pG->U[k][j][i].B3c*pG->U[k][j][i].M1 -
			    pG->U[k][j][i].B1c*pG->U[k][j][i].M3)
                              /pG->U[k][j][i].d;
        emf3_cc[k][j][i] = (pG->U[k][j][i].B1c*pG->U[k][j][i].M2 -
			    pG->U[k][j][i].B2c*pG->U[k][j][i].M1)
                              /pG->U[k][j][i].d;
      }
    }
  }
  integrate_emf1_corner(pG);
  integrate_emf2_corner(pG);
  integrate_emf3_corner(pG);

/*--- Step 4b ------------------------------------------------------------------
 * Update the interface magnetic fields using CT for a half time step.
 */

  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        B1_x1Face[k][j][i] += q3*(emf2[k+1][j  ][i  ] - emf2[k][j][i]) -
                              q2*(emf3[k  ][j+1][i  ] - emf3[k][j][i]);
        B2_x2Face[k][j][i] += q1*(emf3[k  ][j  ][i+1] - emf3[k][j][i]) -
                              q3*(emf1[k+1][j  ][i  ] - emf1[k][j][i]);
        B3_x3Face[k][j][i] += q2*(emf1[k  ][j+1][i  ] - emf1[k][j][i]) -
                              q1*(emf2[k  ][j  ][i+1] - emf2[k][j][i]);
      }
      B1_x1Face[k][j][ie+2] += q3*(emf2[k+1][j  ][ie+2]-emf2[k][j][ie+2]) -
                               q2*(emf3[k  ][j+1][ie+2]-emf3[k][j][ie+2]);
    }
    for (i=is-1; i<=ie+1; i++) {
      B2_x2Face[k][je+2][i] += q1*(emf3[k  ][je+2][i+1]-emf3[k][je+2][i]) -
                               q3*(emf1[k+1][je+2][i  ]-emf1[k][je+2][i]);
    }
  }
  for (j=js-1; j<=je+1; j++) {
    for (i=is-1; i<=ie+1; i++) {
      B3_x3Face[ke+2][j][i] += q2*(emf1[ke+2][j+1][i  ]-emf1[ke+2][j][i]) -
                               q1*(emf2[ke+2][j  ][i+1]-emf2[ke+2][j][i]);
    }
  }
#endif /* MHD */

/*=== STEP 5: Correct x1-interface states with transverse flux gradients =====*/

/*--- Step 5a ------------------------------------------------------------------
 * Correct x1-interface states using x2-fluxes computed in Step 2d.
 * Since the fluxes come from an x2-sweep, (x,y,z) on RHS -> (z,x,y) on LHS 
 */

  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+2; i++) {
        Ul_x1Face[k][j][i].d -=q2*(x2Flux[k][j+1][i-1].d -x2Flux[k][j][i-1].d );
        Ul_x1Face[k][j][i].Mx-=q2*(x2Flux[k][j+1][i-1].Mz-x2Flux[k][j][i-1].Mz);
        Ul_x1Face[k][j][i].My-=q2*(x2Flux[k][j+1][i-1].Mx-x2Flux[k][j][i-1].Mx);
        Ul_x1Face[k][j][i].Mz-=q2*(x2Flux[k][j+1][i-1].My-x2Flux[k][j][i-1].My);
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E -=q2*(x2Flux[k][j+1][i-1].E -x2Flux[k][j][i-1].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B3 */
	Ul_x1Face[k][j][i].Bz+=q2*0.5*
	  ((emf1[k  ][j+1][i-1] - emf1[k  ][j][i-1]) +
	   (emf1[k+1][j+1][i-1] - emf1[k+1][j][i-1]));
#endif

        Ur_x1Face[k][j][i].d -=q2*(x2Flux[k][j+1][i  ].d -x2Flux[k][j][i  ].d );
        Ur_x1Face[k][j][i].Mx-=q2*(x2Flux[k][j+1][i  ].Mz-x2Flux[k][j][i  ].Mz);
        Ur_x1Face[k][j][i].My-=q2*(x2Flux[k][j+1][i  ].Mx-x2Flux[k][j][i  ].Mx);
        Ur_x1Face[k][j][i].Mz-=q2*(x2Flux[k][j+1][i  ].My-x2Flux[k][j][i  ].My);
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E -=q2*(x2Flux[k][j+1][i  ].E -x2Flux[k][j][i  ].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B3 */
	Ur_x1Face[k][j][i].Bz+=q2*0.5*
	  ((emf1[k  ][j+1][i] - emf1[k  ][j][i]) +
	   (emf1[k+1][j+1][i] - emf1[k+1][j][i]));
#endif
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) {
          Ul_x1Face[k][j][i].s[n] -=
             q2*(x2Flux[k][j+1][i-1].s[n] - x2Flux[k][j][i-1].s[n]);
          Ur_x1Face[k][j][i].s[n] -=
             q2*(x2Flux[k][j+1][i  ].s[n] - x2Flux[k][j][i  ].s[n]);
        }
#endif

/*--- Step 5b ------------------------------------------------------------------
 * Correct x1-interface states using x3-fluxes computed in Step 3d.
 * Since the fluxes come from an x3-sweep, (x,y,z) on RHS -> (y,z,x) on LHS
 */

        Ul_x1Face[k][j][i].d -=q3*(x3Flux[k+1][j][i-1].d -x3Flux[k][j][i-1].d );
        Ul_x1Face[k][j][i].Mx-=q3*(x3Flux[k+1][j][i-1].My-x3Flux[k][j][i-1].My);
        Ul_x1Face[k][j][i].My-=q3*(x3Flux[k+1][j][i-1].Mz-x3Flux[k][j][i-1].Mz);
        Ul_x1Face[k][j][i].Mz-=q3*(x3Flux[k+1][j][i-1].Mx-x3Flux[k][j][i-1].Mx);
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E -=q3*(x3Flux[k+1][j][i-1].E -x3Flux[k][j][i-1].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B2 */
	Ul_x1Face[k][j][i].By-=q3*0.5*
	  ((emf1[k+1][j  ][i-1] - emf1[k][j  ][i-1]) +
	   (emf1[k+1][j+1][i-1] - emf1[k][j+1][i-1]));
#endif

        Ur_x1Face[k][j][i].d -=q3*(x3Flux[k+1][j][i  ].d -x3Flux[k][j][i  ].d );
        Ur_x1Face[k][j][i].Mx-=q3*(x3Flux[k+1][j][i  ].My-x3Flux[k][j][i  ].My);
        Ur_x1Face[k][j][i].My-=q3*(x3Flux[k+1][j][i  ].Mz-x3Flux[k][j][i  ].Mz);
        Ur_x1Face[k][j][i].Mz-=q3*(x3Flux[k+1][j][i  ].Mx-x3Flux[k][j][i  ].Mx);
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E -=q3*(x3Flux[k+1][j][i  ].E -x3Flux[k][j][i  ].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B2 */
	Ur_x1Face[k][j][i].By-=q3*0.5*
	  ((emf1[k+1][j  ][i] - emf1[k][j  ][i]) +
	   (emf1[k+1][j+1][i] - emf1[k][j+1][i]));
#endif
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) {
          Ul_x1Face[k][j][i].s[n] -=
             q3*(x3Flux[k+1][j][i-1].s[n] - x3Flux[k][j][i-1].s[n]);
          Ur_x1Face[k][j][i].s[n] -=
             q3*(x3Flux[k+1][j][i  ].s[n] - x3Flux[k][j][i  ].s[n]);
        }
#endif
      }
    }
  }

/*--- Step 5c ------------------------------------------------------------------
 * Add the "MHD source terms" from the x2- and x3-flux-gradients to the
 * conservative variables on the x1Face.  Limiting is used as in GS (2007)
 */

#ifdef MHD
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+2; i++) {
        db1 = (pG->B1i[k  ][j  ][i  ] - pG->B1i[k][j][i-1])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i-1] - pG->B2i[k][j][i-1])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i-1] - pG->B3i[k][j][i-1])*dx3i;
        B1 = pG->U[k][j][i-1].B1c;
        B2 = pG->U[k][j][i-1].B2c;
        B3 = pG->U[k][j][i-1].B3c;
        V2 = pG->U[k][j][i-1].M2/pG->U[k][j][i-1].d;
        V3 = pG->U[k][j][i-1].M3/pG->U[k][j][i-1].d;

/* Calculate mdb2 = min_mod(-db1,db2) */
        if(db1 > 0.0 && db2 < 0.0){
          mdb2 = db2 > -db1 ? db2 : -db1;
        }
        else if(db1 < 0.0 && db2 > 0.0){
          mdb2 = db2 < -db1 ? db2 : -db1;
        }
        else mdb2 = 0.0;

/* Calculate mdb3 = min_mod(-db1,db3) */
        if(db1 > 0.0 && db3 < 0.0){
          mdb3 = db3 > -db1 ? db3 : -db1;
        }
        else if(db1 < 0.0 && db3 > 0.0){
          mdb3 = db3 < -db1 ? db3 : -db1;
        }
        else mdb3 = 0.0;

        Ul_x1Face[k][j][i].Mx += hdt*B1*db1;
        Ul_x1Face[k][j][i].My += hdt*B2*db1;
        Ul_x1Face[k][j][i].Mz += hdt*B3*db1;
        Ul_x1Face[k][j][i].By += hdt*V2*(-mdb3);
        Ul_x1Face[k][j][i].Bz += hdt*V3*(-mdb2);
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E  += hdt*(B2*V2*(-mdb3) + B3*V3*(-mdb2) );
#endif /* BAROTROPIC */

        db1 = (pG->B1i[k  ][j  ][i+1] - pG->B1i[k][j][i])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i  ] - pG->B2i[k][j][i])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i  ] - pG->B3i[k][j][i])*dx3i;
        B1 = pG->U[k][j][i].B1c;
        B2 = pG->U[k][j][i].B2c;
        B3 = pG->U[k][j][i].B3c;
        V2 = pG->U[k][j][i].M2/pG->U[k][j][i].d;
        V3 = pG->U[k][j][i].M3/pG->U[k][j][i].d;

/* Calculate mdb2 = min_mod(-db1,db2) */
        if(db1 > 0.0 && db2 < 0.0){
          mdb2 = db2 > -db1 ? db2 : -db1;
        }
        else if(db1 < 0.0 && db2 > 0.0){
          mdb2 = db2 < -db1 ? db2 : -db1;
        }
        else mdb2 = 0.0;

/* Calculate mdb3 = min_mod(-db1,db3) */
        if(db1 > 0.0 && db3 < 0.0){
          mdb3 = db3 > -db1 ? db3 : -db1;
        }
        else if(db1 < 0.0 && db3 > 0.0){
          mdb3 = db3 < -db1 ? db3 : -db1;
        }
        else mdb3 = 0.0;

        Ur_x1Face[k][j][i].Mx += hdt*B1*db1;
        Ur_x1Face[k][j][i].My += hdt*B2*db1;
        Ur_x1Face[k][j][i].Mz += hdt*B3*db1;
        Ur_x1Face[k][j][i].By += hdt*V2*(-mdb3);
        Ur_x1Face[k][j][i].Bz += hdt*V3*(-mdb2);
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E  += hdt*(B2*V2*(-mdb3) + B3*V3*(-mdb2) );
#endif /* BAROTROPIC */
      }
    }
  }
#endif /* MHD */

/*--- Step 5d ------------------------------------------------------------------
 * Add source terms for a static gravitational potential arising from x2-Flux
 * and x3-Flux gradients.  To improve conservation of total energy, average
 * the energy source term computed at cell faces.
 *    S_{M} = -(\rho) Grad(Phi);   S_{E} = -(\rho v) Grad{Phi}
 */

  if (StaticGravPot != NULL){
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+2; i++) {
        cc_pos(pG,i,j,k,&x1,&x2,&x3);
        phic = (*StaticGravPot)(x1, x2             ,x3);
        phir = (*StaticGravPot)(x1,(x2+0.5*pG->dx2),x3);
        phil = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),x3);

/* correct right states; x2 and x3 gradients */
        Ur_x1Face[k][j][i].My -= q2*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E -= q2*(x2Flux[k][j  ][i  ].d*(phic - phil)
                                  + x2Flux[k][j+1][i  ].d*(phir - phic));
#endif

        phir = (*StaticGravPot)(x1,x2,(x3+0.5*pG->dx3));
        phil = (*StaticGravPot)(x1,x2,(x3-0.5*pG->dx3));
        
        Ur_x1Face[k][j][i].Mz -= q3*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E -= q3*(x3Flux[k  ][j][i  ].d*(phic - phil)
                                  + x3Flux[k+1][j][i  ].d*(phir - phic));
#endif

/* correct left states; x2 and x3 gradients */
        phic = (*StaticGravPot)((x1-pG->dx1), x2             ,x3);
        phir = (*StaticGravPot)((x1-pG->dx1),(x2+0.5*pG->dx2),x3);
        phil = (*StaticGravPot)((x1-pG->dx1),(x2-0.5*pG->dx2),x3);

        Ul_x1Face[k][j][i].My -= q2*(phir-phil)*pG->U[k][j][i-1].d;
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E -= q2*(x2Flux[k][j  ][i-1].d*(phic - phil)
                                  + x2Flux[k][j+1][i-1].d*(phir - phic));
#endif

        phir = (*StaticGravPot)((x1-pG->dx1),x2,(x3+0.5*pG->dx3));
        phil = (*StaticGravPot)((x1-pG->dx1),x2,(x3-0.5*pG->dx3));
        
        Ul_x1Face[k][j][i].Mz -= q3*(phir-phil)*pG->U[k][j][i-1].d;
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E -= q3*(x3Flux[k  ][j][i-1].d*(phic - phil)
                                  + x3Flux[k+1][j][i-1].d*(phir - phic));
#endif
      }
    }
  }}

/*--- Step 5d (cont) -----------------------------------------------------------
 * Add source terms for self gravity arising from x2-Flux and x3-Flux gradients
 *    S_{M} = -(\rho) Grad(Phi);   S_{E} = -(\rho v) Grad{Phi}
 */

#ifdef SELF_GRAVITY
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+2; i++) {
        phic = pG->Phi[k][j][i];
        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j+1][i]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j-1][i]);

/* correct right states; x2 and x3 gradients */
        Ur_x1Face[k][j][i].My -= q2*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E -= q2*(x2Flux[k][j  ][i  ].d*(phic - phil)
                                  + x2Flux[k][j+1][i  ].d*(phir - phic));
#endif

        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k+1][j][i]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k-1][j][i]);

        Ur_x1Face[k][j][i].Mz -= q3*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x1Face[k][j][i].E -= q3*(x3Flux[k  ][j][i  ].d*(phic - phil)
                                  + x3Flux[k+1][j][i  ].d*(phir - phic));
#endif

/* correct left states; x2 and x3 gradients */
        phic = pG->Phi[k][j][i-1];
        phir = 0.5*(pG->Phi[k][j][i-1] + pG->Phi[k][j+1][i-1]);
        phil = 0.5*(pG->Phi[k][j][i-1] + pG->Phi[k][j-1][i-1]);

        Ul_x1Face[k][j][i].My -= q2*(phir-phil)*pG->U[k][j][i-1].d;
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E -= q2*(x2Flux[k][j  ][i-1].d*(phic - phil)
                                  + x2Flux[k][j+1][i-1].d*(phir - phic));
#endif

        phir = 0.5*(pG->Phi[k][j][i-1] + pG->Phi[k+1][j][i-1]);
        phil = 0.5*(pG->Phi[k][j][i-1] + pG->Phi[k-1][j][i-1]);

        Ul_x1Face[k][j][i].Mz -= q3*(phir-phil)*pG->U[k][j][i-1].d;
#ifndef BAROTROPIC
        Ul_x1Face[k][j][i].E -= q3*(x3Flux[k  ][j][i-1].d*(phic - phil)
                                  + x3Flux[k+1][j][i-1].d*(phir - phic));
#endif
      }
    }
  }
#endif /* SELF_GRAVITY */


/*=== STEP 6: Correct x2-interface states with transverse flux gradients =====*/

/*--- Step 6a ------------------------------------------------------------------
 * Correct x2-interface states using x1-fluxes computed in Step 1d.
 * Since the fluxes come from an x1-sweep, (x,y,z) on RHS -> (y,z,x) on LHS
 */

  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+1; i++) {
        Ul_x2Face[k][j][i].d -=q1*(x1Flux[k][j-1][i+1].d -x1Flux[k][j-1][i].d );
        Ul_x2Face[k][j][i].Mx-=q1*(x1Flux[k][j-1][i+1].My-x1Flux[k][j-1][i].My);
        Ul_x2Face[k][j][i].My-=q1*(x1Flux[k][j-1][i+1].Mz-x1Flux[k][j-1][i].Mz);
        Ul_x2Face[k][j][i].Mz-=q1*(x1Flux[k][j-1][i+1].Mx-x1Flux[k][j-1][i].Mx);
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E -=q1*(x1Flux[k][j-1][i+1].E -x1Flux[k][j-1][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B3 */
	Ul_x2Face[k][j][i].By-=q1*0.5*
	  ((emf2[k  ][j-1][i+1] - emf2[k  ][j-1][i]) + 
	   (emf2[k+1][j-1][i+1] - emf2[k+1][j-1][i]));
#endif

        Ur_x2Face[k][j][i].d -=q1*(x1Flux[k][j  ][i+1].d -x1Flux[k][j  ][i].d );
        Ur_x2Face[k][j][i].Mx-=q1*(x1Flux[k][j  ][i+1].My-x1Flux[k][j  ][i].My);
        Ur_x2Face[k][j][i].My-=q1*(x1Flux[k][j  ][i+1].Mz-x1Flux[k][j  ][i].Mz);
        Ur_x2Face[k][j][i].Mz-=q1*(x1Flux[k][j  ][i+1].Mx-x1Flux[k][j  ][i].Mx);
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E -=q1*(x1Flux[k][j  ][i+1].E -x1Flux[k][j  ][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B3 */
	Ur_x2Face[k][j][i].By-=q1*0.5*
	  ((emf2[k  ][j][i+1] - emf2[k  ][j][i]) + 
	   (emf2[k+1][j][i+1] - emf2[k+1][j][i]));
#endif
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) {
          Ul_x2Face[k][j][i].s[n] -=
             q1*(x1Flux[k][j-1][i+1].s[n] - x1Flux[k][j-1][i].s[n]);
          Ur_x2Face[k][j][i].s[n] -=
             q1*(x1Flux[k][j  ][i+1].s[n] - x1Flux[k][j  ][i].s[n]);
        }
#endif

/*--- Step 6b ------------------------------------------------------------------
 * Correct x2-interface states using x3-fluxes computed in Step 3d.
 * Since the fluxes come from an x3-sweep, (x,y,z) on RHS -> (z,x,y) on LHS 
 */

        Ul_x2Face[k][j][i].d -=q3*(x3Flux[k+1][j-1][i].d -x3Flux[k][j-1][i].d );
        Ul_x2Face[k][j][i].Mx-=q3*(x3Flux[k+1][j-1][i].Mz-x3Flux[k][j-1][i].Mz);
        Ul_x2Face[k][j][i].My-=q3*(x3Flux[k+1][j-1][i].Mx-x3Flux[k][j-1][i].Mx);
        Ul_x2Face[k][j][i].Mz-=q3*(x3Flux[k+1][j-1][i].My-x3Flux[k][j-1][i].My);
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E -=q3*(x3Flux[k+1][j-1][i].E -x3Flux[k][j-1][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B1 */
	Ul_x2Face[k][j][i].Bz+=q3*0.5*
	  ((emf2[k+1][j-1][i  ] - emf2[k][j-1][i  ]) +
	   (emf2[k+1][j-1][i+1] - emf2[k][j-1][i+1]));
#endif

        Ur_x2Face[k][j][i].d -=q3*(x3Flux[k+1][j  ][i].d -x3Flux[k][j  ][i].d );
        Ur_x2Face[k][j][i].Mx-=q3*(x3Flux[k+1][j  ][i].Mz-x3Flux[k][j  ][i].Mz);
        Ur_x2Face[k][j][i].My-=q3*(x3Flux[k+1][j  ][i].Mx-x3Flux[k][j  ][i].Mx);
        Ur_x2Face[k][j][i].Mz-=q3*(x3Flux[k+1][j  ][i].My-x3Flux[k][j  ][i].My);
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E -=q3*(x3Flux[k+1][j  ][i].E -x3Flux[k][j  ][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B1 */
	Ur_x2Face[k][j][i].Bz+=q3*0.5*
	  ((emf2[k+1][j][i  ] - emf2[k][j][i  ]) +
	   (emf2[k+1][j][i+1] - emf2[k][j][i+1]));
#endif
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) {
          Ul_x2Face[k][j][i].s[n] -=
             q3*(x3Flux[k+1][j-1][i].s[n] - x3Flux[k][j-1][i].s[n]);
          Ur_x2Face[k][j][i].s[n] -=
             q3*(x3Flux[k+1][j  ][i].s[n] - x3Flux[k][j  ][i].s[n]);
        }
#endif
      }
    }
  }

/*--- Step 6c ------------------------------------------------------------------
 * Add the "MHD source terms" from the x1- and x3-flux-gradients to the
 * conservative variables on the x2Face.  Limiting is used as in GS (2007)
 */

#ifdef MHD
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+1; i++) {
        db1 = (pG->B1i[k  ][j-1][i+1] - pG->B1i[k][j-1][i])*dx1i;
        db2 = (pG->B2i[k  ][j  ][i  ] - pG->B2i[k][j-1][i])*dx2i;
        db3 = (pG->B3i[k+1][j-1][i  ] - pG->B3i[k][j-1][i])*dx3i;
        B1 = pG->U[k][j-1][i].B1c;
        B2 = pG->U[k][j-1][i].B2c;
        B3 = pG->U[k][j-1][i].B3c;
        V1 = pG->U[k][j-1][i].M1/pG->U[k][j-1][i].d;
        V3 = pG->U[k][j-1][i].M3/pG->U[k][j-1][i].d;

/* Calculate mdb1 = min_mod(-db2,db1) */
        if(db2 > 0.0 && db1 < 0.0){
          mdb1 = db1 > -db2 ? db1 : -db2;
        }
        else if(db2 < 0.0 && db1 > 0.0){
          mdb1 = db1 < -db2 ? db1 : -db2;
        }
        else mdb1 = 0.0;

/* Calculate mdb3 = min_mod(-db2,db3) */
        if(db2 > 0.0 && db3 < 0.0){
          mdb3 = db3 > -db2 ? db3 : -db2;
        }
        else if(db2 < 0.0 && db3 > 0.0){
          mdb3 = db3 < -db2 ? db3 : -db2;
        }
        else mdb3 = 0.0;

        Ul_x2Face[k][j][i].Mz += hdt*B1*db2;
        Ul_x2Face[k][j][i].Mx += hdt*B2*db2;
        Ul_x2Face[k][j][i].My += hdt*B3*db2;
        Ul_x2Face[k][j][i].By += hdt*V3*(-mdb1);
        Ul_x2Face[k][j][i].Bz += hdt*V1*(-mdb3);
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E  += hdt*(B3*V3*(-mdb1) + B1*V1*(-mdb3) );
#endif /* BAROTROPIC */

        db1 = (pG->B1i[k  ][j  ][i+1] - pG->B1i[k][j][i])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i  ] - pG->B2i[k][j][i])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i  ] - pG->B3i[k][j][i])*dx3i;
        B1 = pG->U[k][j][i].B1c;
        B2 = pG->U[k][j][i].B2c;
        B3 = pG->U[k][j][i].B3c;
        V1 = pG->U[k][j][i].M1/pG->U[k][j][i].d;
        V3 = pG->U[k][j][i].M3/pG->U[k][j][i].d;

/* Calculate mdb1 = min_mod(-db2,db1) */
        if(db2 > 0.0 && db1 < 0.0){
          mdb1 = db1 > -db2 ? db1 : -db2;
        }
        else if(db2 < 0.0 && db1 > 0.0){
          mdb1 = db1 < -db2 ? db1 : -db2;
        }
        else mdb1 = 0.0;

/* Calculate mdb3 = min_mod(-db2,db3) */
        if(db2 > 0.0 && db3 < 0.0){
          mdb3 = db3 > -db2 ? db3 : -db2;
        }
        else if(db2 < 0.0 && db3 > 0.0){
          mdb3 = db3 < -db2 ? db3 : -db2;
        }
        else mdb3 = 0.0;

        Ur_x2Face[k][j][i].Mz += hdt*B1*db2;
        Ur_x2Face[k][j][i].Mx += hdt*B2*db2;
        Ur_x2Face[k][j][i].My += hdt*B3*db2;
        Ur_x2Face[k][j][i].By += hdt*V3*(-mdb1);
        Ur_x2Face[k][j][i].Bz += hdt*V1*(-mdb3);
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E  += hdt*(B3*V3*(-mdb1) + B1*V1*(-mdb3) );
#endif /* BAROTROPIC */
      }
    }
  }
#endif /* MHD */

/*--- Step 6d ------------------------------------------------------------------
 * Add source terms for a static gravitational potential arising from x1-Flux
 * and x3-Flux gradients. To improve conservation of total energy,
 * average the energy source term computed at cell faces.
 *    S_{M} = -(\rho) Grad(Phi);   S_{E} = -(\rho v) Grad{Phi}
 */

  if (StaticGravPot != NULL){
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+1; i++) {
        cc_pos(pG,i,j,k,&x1,&x2,&x3);
        phic = (*StaticGravPot)((x1            ),x2,x3);
        phir = (*StaticGravPot)((x1+0.5*pG->dx1),x2,x3);
        phil = (*StaticGravPot)((x1-0.5*pG->dx1),x2,x3);

/* correct right states; x1 and x3 gradients */
        Ur_x2Face[k][j][i].Mz -= q1*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E -= q1*(x1Flux[k][j  ][i  ].d*(phic - phil)
                                  + x1Flux[k][j  ][i+1].d*(phir - phic));
#endif

        phir = (*StaticGravPot)(x1,x2,(x3+0.5*pG->dx3));
        phil = (*StaticGravPot)(x1,x2,(x3-0.5*pG->dx3));

        Ur_x2Face[k][j][i].My -= q3*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E -= q3*(x3Flux[k  ][j  ][i].d*(phic - phil)
                                  + x3Flux[k+1][j  ][i].d*(phir - phic));
#endif

/* correct left states; x1 and x3 gradients */
        phic = (*StaticGravPot)((x1            ),(x2-pG->dx2),x3);
        phir = (*StaticGravPot)((x1+0.5*pG->dx1),(x2-pG->dx2),x3);
        phil = (*StaticGravPot)((x1-0.5*pG->dx1),(x2-pG->dx2),x3);

        Ul_x2Face[k][j][i].Mz -= q1*(phir-phil)*pG->U[k][j-1][i].d;
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E -= q1*(x1Flux[k][j-1][i  ].d*(phic - phil)
                                  + x1Flux[k][j-1][i+1].d*(phir - phic));
#endif
        phir = (*StaticGravPot)(x1,(x2-pG->dx2),(x3+0.5*pG->dx3));
        phil = (*StaticGravPot)(x1,(x2-pG->dx2),(x3-0.5*pG->dx3));

        Ul_x2Face[k][j][i].My -= q3*(phir-phil)*pG->U[k][j-1][i].d;
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E -= q3*(x3Flux[k  ][j-1][i].d*(phic - phil)
                                  + x3Flux[k+1][j-1][i].d*(phir - phic));
#endif
      }
    }
  }}

/*--- Step 6d (cont) -----------------------------------------------------------
 * Add source terms for self gravity arising from x1-Flux and x3-Flux gradients
 *    S_{M} = -(\rho) Grad(Phi);   S_{E} = -(\rho v) Grad{Phi}
 */

#ifdef SELF_GRAVITY
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+1; i++) {
        phic = pG->Phi[k][j][i];
        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j][i+1]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j][i-1]);

/* correct right states; x1 and x3 gradients */
        Ur_x2Face[k][j][i].Mz -= q1*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E -= q1*(x1Flux[k][j][i  ].d*(phic - phil)
                                  + x1Flux[k][j][i+1].d*(phir - phic));
#endif

        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k+1][j][i]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k-1][j][i]);

        Ur_x2Face[k][j][i].My -= q3*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x2Face[k][j][i].E -= q3*(x3Flux[k  ][j][i].d*(phic - phil)
                                  + x3Flux[k+1][j][i].d*(phir - phic));
#endif

/* correct left states; x1 and x3 gradients */
        phic = pG->Phi[k][j-1][i];
        phir = 0.5*(pG->Phi[k][j-1][i] + pG->Phi[k][j-1][i+1]);
        phil = 0.5*(pG->Phi[k][j-1][i] + pG->Phi[k][j-1][i-1]);

        Ul_x2Face[k][j][i].Mz -= q1*(phir-phil)*pG->U[k][j-1][i].d;
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E -= q1*(x1Flux[k][j-1][i  ].d*(phic - phil)
                                  + x1Flux[k][j-1][i+1].d*(phir - phic));
#endif
        phir = 0.5*(pG->Phi[k][j-1][i] + pG->Phi[k+1][j-1][i]);
        phil = 0.5*(pG->Phi[k][j-1][i] + pG->Phi[k-1][j-1][i]);

        Ul_x2Face[k][j][i].My -= q3*(phir-phil)*pG->U[k][j-1][i].d;
#ifndef BAROTROPIC
        Ul_x2Face[k][j][i].E -= q3*(x3Flux[k  ][j-1][i].d*(phic - phil)
                                  + x3Flux[k+1][j-1][i].d*(phir - phic));
#endif
      }
    }
  }
#endif /* SELF_GRAVITY */

/*--- Step 6d (cont) -----------------------------------------------------------
 * Add source terms for shearing box arising from x1-Flux gradient
 *    Vx source term is (dt/2)( 2 Omega V y); Vx on x2Face is z-comp.
 *    Vy source term is (dt/2)(-2 Omega V x); Vy on x2Face is x-comp.
 *    With FARGO Vy source term is (dt/2)(-1/2 Omega V x)
 */

#ifdef SHEARING_BOX
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+1; i++) {
        Ur_x2Face[k][j][i].Mz += pG->dt*Omega*pG->U[k][j][i].M2;
#ifdef FARGO
        Ur_x2Face[k][j][i].Mx -= 0.25*pG->dt*Omega*pG->U[k][j][i].M1;
#else
        Ur_x2Face[k][j][i].Mx -= pG->dt*Omega*pG->U[k][j][i].M1;
#endif

        Ul_x2Face[k][j][i].Mz += pG->dt*Omega*pG->U[k][j-1][i].M2;
#ifdef FARGO
        Ul_x2Face[k][j][i].Mx -= 0.25*pG->dt*Omega*pG->U[k][j-1][i].M1;
#else
        Ul_x2Face[k][j][i].Mx -= pG->dt*Omega*pG->U[k][j-1][i].M1;
#endif
      }
    }
  }
#endif /* SHEARING_BOX */

/*=== STEP 7: Correct x3-interface states with transverse flux gradients =====*/

/*--- Step 7a ------------------------------------------------------------------
 * Correct x3-interface states using x1-fluxes computed in Step 1d.
 * Since the fluxes come from an x1-sweep, (x,y,z) on RHS -> (z,x,y) on LHS 
 */

  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        Ul_x3Face[k][j][i].d -=q1*(x1Flux[k-1][j][i+1].d -x1Flux[k-1][j][i].d );
        Ul_x3Face[k][j][i].Mx-=q1*(x1Flux[k-1][j][i+1].Mz-x1Flux[k-1][j][i].Mz);
        Ul_x3Face[k][j][i].My-=q1*(x1Flux[k-1][j][i+1].Mx-x1Flux[k-1][j][i].Mx);
        Ul_x3Face[k][j][i].Mz-=q1*(x1Flux[k-1][j][i+1].My-x1Flux[k-1][j][i].My);
#ifndef BAROTROPIC
        Ul_x3Face[k][j][i].E -=q1*(x1Flux[k-1][j][i+1].E -x1Flux[k-1][j][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B2 */
	Ul_x3Face[k][j][i].Bz+=q1*0.5*
	  ((emf3[k-1][j  ][i+1] - emf3[k-1][j  ][i]) +
	   (emf3[k-1][j+1][i+1] - emf3[k-1][j+1][i]));
#endif

        Ur_x3Face[k][j][i].d -=q1*(x1Flux[k  ][j][i+1].d -x1Flux[k  ][j][i].d );
        Ur_x3Face[k][j][i].Mx-=q1*(x1Flux[k  ][j][i+1].Mz-x1Flux[k  ][j][i].Mz);
        Ur_x3Face[k][j][i].My-=q1*(x1Flux[k  ][j][i+1].Mx-x1Flux[k  ][j][i].Mx);
        Ur_x3Face[k][j][i].Mz-=q1*(x1Flux[k  ][j][i+1].My-x1Flux[k  ][j][i].My);
#ifndef BAROTROPIC
        Ur_x3Face[k][j][i].E -=q1*(x1Flux[k  ][j][i+1].E -x1Flux[k  ][j][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B2 */
	Ur_x3Face[k][j][i].Bz+=q1*0.5*
	  ((emf3[k][j  ][i+1] - emf3[k][j  ][i]) +
	   (emf3[k][j+1][i+1] - emf3[k][j+1][i]));
#endif
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) {
          Ul_x3Face[k][j][i].s[n] -=
             q1*(x1Flux[k-1][j][i+1].s[n] - x1Flux[k-1][j][i].s[n]);
          Ur_x3Face[k][j][i].s[n] -=
             q1*(x1Flux[k  ][j][i+1].s[n] - x1Flux[k  ][j][i].s[n]);
        }
#endif

/*--- Step 7b ------------------------------------------------------------------
 * Correct x3-interface states using x2-fluxes computed in Step 2d.
 * Since the fluxes come from an x2-sweep, (x,y,z) on RHS -> (y,z,x) on LHS 
 */

        Ul_x3Face[k][j][i].d -=q2*(x2Flux[k-1][j+1][i].d -x2Flux[k-1][j][i].d );
        Ul_x3Face[k][j][i].Mx-=q2*(x2Flux[k-1][j+1][i].My-x2Flux[k-1][j][i].My);
        Ul_x3Face[k][j][i].My-=q2*(x2Flux[k-1][j+1][i].Mz-x2Flux[k-1][j][i].Mz);
        Ul_x3Face[k][j][i].Mz-=q2*(x2Flux[k-1][j+1][i].Mx-x2Flux[k-1][j][i].Mx);
#ifndef BAROTROPIC
        Ul_x3Face[k][j][i].E -=q2*(x2Flux[k-1][j+1][i].E -x2Flux[k-1][j][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B1 */
	Ul_x3Face[k][j][i].By-=q2*0.5*
	  ((emf3[k-1][j+1][i  ] - emf3[k-1][j][i  ]) +
	   (emf3[k-1][j+1][i+1] - emf3[k-1][j][i+1]));
#endif

        Ur_x3Face[k][j][i].d -=q2*(x2Flux[k  ][j+1][i].d -x2Flux[k  ][j][i].d );
        Ur_x3Face[k][j][i].Mx-=q2*(x2Flux[k  ][j+1][i].My-x2Flux[k  ][j][i].My);
        Ur_x3Face[k][j][i].My-=q2*(x2Flux[k  ][j+1][i].Mz-x2Flux[k  ][j][i].Mz);
        Ur_x3Face[k][j][i].Mz-=q2*(x2Flux[k  ][j+1][i].Mx-x2Flux[k  ][j][i].Mx);
#ifndef BAROTROPIC
        Ur_x3Face[k][j][i].E -=q2*(x2Flux[k  ][j+1][i].E -x2Flux[k  ][j][i].E );
#endif /* BAROTROPIC */
#ifdef MHD
/* Update B1 */
	Ur_x3Face[k][j][i].By-=q2*0.5*
	  ((emf3[k][j+1][i  ] - emf3[k][j][i  ]) +
	   (emf3[k][j+1][i+1] - emf3[k][j][i+1]));
#endif
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++) {
          Ul_x3Face[k][j][i].s[n] -=
             q2*(x2Flux[k-1][j+1][i].s[n] - x2Flux[k-1][j][i].s[n]);
          Ur_x3Face[k][j][i].s[n] -=
             q2*(x2Flux[k  ][j+1][i].s[n] - x2Flux[k  ][j][i].s[n]);
        }
#endif
      }
    }
  }

/*--- Step 7c ------------------------------------------------------------------
 * Add the "MHD source terms" from the x1- and x2-flux-gradients to the
 * conservative variables on the x3Face.  Limiting is used as in GS07.
 */

#ifdef MHD
  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        db1 = (pG->B1i[k-1][j  ][i+1] - pG->B1i[k-1][j][i])*dx1i;
        db2 = (pG->B2i[k-1][j+1][i  ] - pG->B2i[k-1][j][i])*dx2i;
        db3 = (pG->B3i[k  ][j  ][i  ] - pG->B3i[k-1][j][i])*dx3i;
        B1 = pG->U[k-1][j][i].B1c;
        B2 = pG->U[k-1][j][i].B2c;
        B3 = pG->U[k-1][j][i].B3c;
	V1 = pG->U[k-1][j][i].M1/pG->U[k-1][j][i].d;
	V2 = pG->U[k-1][j][i].M2/pG->U[k-1][j][i].d;

/* Calculate mdb1 = min_mod(-db3,db1) */
	if(db3 > 0.0 && db1 < 0.0){
	  mdb1 = db1 > -db3 ? db1 : -db3;
	}
	else if(db3 < 0.0 && db1 > 0.0){
	  mdb1 = db1 < -db3 ? db1 : -db3;
	}
	else mdb1 = 0.0;

/* Calculate mdb2 = min_mod(-db3,db2) */
	if(db3 > 0.0 && db2 < 0.0){
	  mdb2 = db2 > -db3 ? db2 : -db3;
	}
	else if(db3 < 0.0 && db2 > 0.0){
	  mdb2 = db2 < -db3 ? db2 : -db3;
	}
	else mdb2 = 0.0;

        Ul_x3Face[k][j][i].My += hdt*B1*db3;
        Ul_x3Face[k][j][i].Mz += hdt*B2*db3;
        Ul_x3Face[k][j][i].Mx += hdt*B3*db3;
	Ul_x3Face[k][j][i].By += hdt*V1*(-mdb2);
	Ul_x3Face[k][j][i].Bz += hdt*V2*(-mdb1);
#ifndef BAROTROPIC
	Ul_x3Face[k][j][i].E  += hdt*(B1*V1*(-mdb2) + B2*V2*(-mdb1) );
#endif /* BAROTROPIC */

        db1 = (pG->B1i[k  ][j  ][i+1] - pG->B1i[k][j][i])*dx1i;
        db2 = (pG->B2i[k  ][j+1][i  ] - pG->B2i[k][j][i])*dx2i;
        db3 = (pG->B3i[k+1][j  ][i  ] - pG->B3i[k][j][i])*dx3i;
        B1 = pG->U[k][j][i].B1c;
        B2 = pG->U[k][j][i].B2c;
        B3 = pG->U[k][j][i].B3c;
	V1 = pG->U[k][j][i].M1/pG->U[k][j][i].d;
	V2 = pG->U[k][j][i].M2/pG->U[k][j][i].d;

/* Calculate mdb1 = min_mod(-db3,db1) */
	if(db3 > 0.0 && db1 < 0.0){
	  mdb1 = db1 > -db3 ? db1 : -db3;
	}
	else if(db3 < 0.0 && db1 > 0.0){
	  mdb1 = db1 < -db3 ? db1 : -db3;
	}
	else mdb1 = 0.0;

/* Calculate mdb2 = min_mod(-db3,db2) */
	if(db3 > 0.0 && db2 < 0.0){
	  mdb2 = db2 > -db3 ? db2 : -db3;
	}
	else if(db3 < 0.0 && db2 > 0.0){
	  mdb2 = db2 < -db3 ? db2 : -db3;
	}
	else mdb2 = 0.0;

        Ur_x3Face[k][j][i].My += hdt*B1*db3;
        Ur_x3Face[k][j][i].Mz += hdt*B2*db3;
        Ur_x3Face[k][j][i].Mx += hdt*B3*db3;
	Ur_x3Face[k][j][i].By += hdt*V1*(-mdb2);
	Ur_x3Face[k][j][i].Bz += hdt*V2*(-mdb1);
#ifndef BAROTROPIC
	Ur_x3Face[k][j][i].E  += hdt*(B1*V1*(-mdb2) + B2*V2*(-mdb1) );
#endif /* BAROTROPIC */
      }
    }
  }
#endif /* MHD */

/*--- Step 7d ------------------------------------------------------------------
 * Add source terms for a static gravitational potential arising from x1-Flux
 * and x2-Flux gradients. To improve conservation of total energy,
 * average the energy source term computed at cell faces.
 *    S_{M} = -(\rho) Grad(Phi);   S_{E} = -(\rho v) Grad{Phi}
 */

  if (StaticGravPot != NULL){
  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        cc_pos(pG,i,j,k,&x1,&x2,&x3);
        phic = (*StaticGravPot)((x1            ),x2,x3);
        phir = (*StaticGravPot)((x1+0.5*pG->dx1),x2,x3);
        phil = (*StaticGravPot)((x1-0.5*pG->dx1),x2,x3);

/* correct right states; x1 and x2 gradients */
        Ur_x3Face[k][j][i].My -= q1*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x3Face[k][j][i].E -= q1*(x1Flux[k  ][j][i  ].d*(phic - phil)
                                  + x1Flux[k  ][j][i+1].d*(phir - phic));
#endif

        phir = (*StaticGravPot)(x1,(x2+0.5*pG->dx2),x3);
        phil = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),x3);

        Ur_x3Face[k][j][i].Mz -= q2*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x3Face[k][j][i].E -= q2*(x2Flux[k  ][j  ][i].d*(phic - phil)
                                  + x2Flux[k  ][j+1][i].d*(phir - phic));
#endif

/* correct left states; x1 and x2 gradients */
        phic = (*StaticGravPot)((x1            ),x2,(x3-pG->dx3));
        phir = (*StaticGravPot)((x1+0.5*pG->dx1),x2,(x3-pG->dx3));
        phil = (*StaticGravPot)((x1-0.5*pG->dx1),x2,(x3-pG->dx3));

        Ul_x3Face[k][j][i].My -= q1*(phir-phil)*pG->U[k-1][j][i].d;
#ifndef BAROTROPIC
        Ul_x3Face[k][j][i].E -= q1*(x1Flux[k-1][j][i  ].d*(phic - phil)
                                  + x1Flux[k-1][j][i+1].d*(phir - phic));
#endif

        phir = (*StaticGravPot)(x1,(x2+0.5*pG->dx2),(x3-pG->dx3));
        phil = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),(x3-pG->dx3));

        Ul_x3Face[k][j][i].Mz -= q2*(phir-phil)*pG->U[k-1][j][i].d;
#ifndef BAROTROPIC
        Ul_x3Face[k][j][i].E -= q2*(x2Flux[k-1][j  ][i].d*(phic - phil)
                                  + x2Flux[k-1][j+1][i].d*(phir - phic));
#endif
      }
    }
  }}

/*--- Step 7d (cont) -----------------------------------------------------------
 * Add source terms for self gravity arising from x1-Flux and x2-Flux gradients
 *    S_{M} = -(\rho) Grad(Phi);   S_{E} = -(\rho v) Grad{Phi}
 */

#ifdef SELF_GRAVITY
  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        phic = pG->Phi[k][j][i];
        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j][i+1]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j][i-1]);

/* correct right states; x1 and x2 gradients */
        Ur_x3Face[k][j][i].My -= q1*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x3Face[k][j][i].E -= q1*(x1Flux[k][j][i  ].d*(phic - phil)
                                  + x1Flux[k][j][i+1].d*(phir - phic));
#endif

        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j+1][i]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j-1][i]);

        Ur_x3Face[k][j][i].Mz -= q2*(phir-phil)*pG->U[k][j][i].d;
#ifndef BAROTROPIC
        Ur_x3Face[k][j][i].E -= q2*(x2Flux[k][j  ][i].d*(phic - phil)
                                  + x2Flux[k][j+1][i].d*(phir - phic));
#endif

/* correct left states; x1 and x2 gradients */
        phic = pG->Phi[k-1][j][i];
        phir = 0.5*(pG->Phi[k-1][j][i] + pG->Phi[k-1][j][i+1]);
        phil = 0.5*(pG->Phi[k-1][j][i] + pG->Phi[k-1][j][i-1]);

        Ul_x3Face[k][j][i].My -= q1*(phir-phil)*pG->U[k-1][j][i].d;
#ifndef BAROTROPIC
        Ul_x3Face[k][j][i].E -= q1*(x1Flux[k-1][j][i  ].d*(phic - phil)
                                  + x1Flux[k-1][j][i+1].d*(phir - phic));
#endif

        phir = 0.5*(pG->Phi[k-1][j][i] + pG->Phi[k-1][j+1][i]);
        phil = 0.5*(pG->Phi[k-1][j][i] + pG->Phi[k-1][j-1][i]);

        Ul_x3Face[k][j][i].Mz -= q2*(phir-phil)*pG->U[k-1][j][i].d;
#ifndef BAROTROPIC
        Ul_x3Face[k][j][i].E -= q2*(x2Flux[k-1][j  ][i].d*(phic - phil)
                                  + x2Flux[k-1][j+1][i].d*(phir - phic));
#endif
      }
    }
  }
#endif /* SELF_GRAVITY */

/*--- Step 7d (cont) -----------------------------------------------------------
 * Add source terms for shearing box arising from x1-Flux gradient
 *    Vx source term is (dt/2)( 2 Omega V y); Vx on x3Face is y-comp.
 *    Vy source term is (dt/2)(-2 Omega V x); Vy on x3Face is z-comp.
 *    With FARGO Vy source term is (dt/2)(-1/2 Omega V x)
 */

#ifdef SHEARING_BOX
  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        Ur_x3Face[k][j][i].My += pG->dt*Omega*pG->U[k][j][i].M2;
#ifdef FARGO
        Ur_x3Face[k][j][i].Mz -= 0.25*pG->dt*Omega*pG->U[k][j][i].M1;
#else
        Ur_x3Face[k][j][i].Mz -= pG->dt*Omega*pG->U[k][j][i].M1;
#endif

        Ul_x3Face[k][j][i].My += pG->dt*Omega*pG->U[k-1][j][i].M2;
#ifdef FARGO
        Ul_x3Face[k][j][i].Mz -= 0.25*pG->dt*Omega*pG->U[k-1][j][i].M1;
#else
        Ul_x3Face[k][j][i].Mz -= pG->dt*Omega*pG->U[k-1][j][i].M1;
#endif
      }
    }
  }
#endif /* SHEARING_BOX */

/*=== STEP 8: Compute cell-centered values at n+1/2 ==========================*/

/*--- Step 8a ------------------------------------------------------------------
 * Calculate d^{n+1/2} (needed with static potential, cooling, or MHD)
 */

#ifndef MHD
  if ((StaticGravPot != NULL) || (CoolingFunc != NULL))
#endif
  {
    for (k=ks-1; k<=ke+1; k++) {
      for (j=js-1; j<=je+1; j++) {
	for (i=is-1; i<=ie+1; i++) {
	  dhalf[k][j][i] = pG->U[k][j][i].d 
	    - q1*(x1Flux[k  ][j  ][i+1].d - x1Flux[k][j][i].d)
	    - q2*(x2Flux[k  ][j+1][i  ].d - x2Flux[k][j][i].d)
	    - q3*(x3Flux[k+1][j  ][i  ].d - x3Flux[k][j][i].d);
	}
      }
    }
  }

/*--- Step 8b ------------------------------------------------------------------
 * Calculate P^{n+1/2} (needed with cooling), and cell centered emf_cc^{n+1/2}
 */

#ifndef MHD
  if (CoolingFunc != NULL)
#endif /* MHD */
  {
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        M1h = pG->U[k][j][i].M1
           - q1*(x1Flux[k  ][j  ][i+1].Mx - x1Flux[k][j][i].Mx)
           - q2*(x2Flux[k  ][j+1][i  ].Mz - x2Flux[k][j][i].Mz)
           - q3*(x3Flux[k+1][j  ][i  ].My - x3Flux[k][j][i].My);

        M2h = pG->U[k][j][i].M2
           - q1*(x1Flux[k  ][j  ][i+1].My - x1Flux[k][j][i].My)
           - q2*(x2Flux[k  ][j+1][i  ].Mx - x2Flux[k][j][i].Mx)
           - q3*(x3Flux[k+1][j  ][i  ].Mz - x3Flux[k][j][i].Mz);

        M3h = pG->U[k][j][i].M3
           - q1*(x1Flux[k  ][j  ][i+1].Mz - x1Flux[k][j][i].Mz)
           - q2*(x2Flux[k  ][j+1][i  ].My - x2Flux[k][j][i].My)
           - q3*(x3Flux[k+1][j  ][i  ].Mx - x3Flux[k][j][i].Mx);

#ifndef BAROTROPIC
        Eh = pG->U[k][j][i].E
           - q1*(x1Flux[k  ][j  ][i+1].E - x1Flux[k][j][i].E)
           - q2*(x2Flux[k  ][j+1][i  ].E - x2Flux[k][j][i].E)
           - q3*(x3Flux[k+1][j  ][i  ].E - x3Flux[k][j][i].E);
#endif

/* Add source terms for fixed gravitational potential */
        if (StaticGravPot != NULL){
          cc_pos(pG,i,j,k,&x1,&x2,&x3);
          phir = (*StaticGravPot)((x1+0.5*pG->dx1),x2,x3);
          phil = (*StaticGravPot)((x1-0.5*pG->dx1),x2,x3);
          M1h -= q1*(phir-phil)*pG->U[k][j][i].d;

          phir = (*StaticGravPot)(x1,(x2+0.5*pG->dx2),x3);
          phil = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),x3);
          M2h -= q2*(phir-phil)*pG->U[k][j][i].d;

          phir = (*StaticGravPot)(x1,x2,(x3+0.5*pG->dx3));
          phil = (*StaticGravPot)(x1,x2,(x3-0.5*pG->dx3));
          M3h -= q3*(phir-phil)*pG->U[k][j][i].d;
        }

/* Add source terms due to self-gravity  */
#ifdef SELF_GRAVITY
        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j][i+1]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j][i-1]);
        M1h -= q1*(phir-phil)*pG->U[k][j][i].d;

        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j+1][i]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k][j-1][i]);
        M2h -= q2*(phir-phil)*pG->U[k][j][i].d;

        phir = 0.5*(pG->Phi[k][j][i] + pG->Phi[k+1][j][i]);
        phil = 0.5*(pG->Phi[k][j][i] + pG->Phi[k-1][j][i]);
        M3h -= q3*(phir-phil)*pG->U[k][j][i].d;
#endif /* SELF_GRAVITY */

/* Add the Coriolis terms for shearing box.  Tidal potential already added by
 * StaticGravPot above.  */
#ifdef SHEARING_BOX
        M1h += pG->dt*Omega*pG->U[k][j][i].M2;
#ifdef FARGO
        M2h -= 0.25*pG->dt*Omega*pG->U[k][j][i].M1;
#else
        M2h -= pG->dt*Omega*pG->U[k][j][i].M1;
#endif
#endif /* SHEARING_BOX */

#ifndef BAROTROPIC
        phalf[k][j][i] = Eh - 0.5*(M1h*M1h + M2h*M2h + M3h*M3h)/dhalf[k][j][i];
#endif

#ifdef MHD
        B1ch = 0.5*(B1_x1Face[k][j][i] + B1_x1Face[k  ][j  ][i+1]);
        B2ch = 0.5*(B2_x2Face[k][j][i] + B2_x2Face[k  ][j+1][i  ]);
        B3ch = 0.5*(B3_x3Face[k][j][i] + B3_x3Face[k+1][j  ][i  ]);
        emf1_cc[k][j][i] = (B2ch*M3h - B3ch*M2h)/dhalf[k][j][i];
        emf2_cc[k][j][i] = (B3ch*M1h - B1ch*M3h)/dhalf[k][j][i];
        emf3_cc[k][j][i] = (B1ch*M2h - B2ch*M1h)/dhalf[k][j][i];
#ifndef BAROTROPIC
        phalf[k][j][i] -= 0.5*(B1ch*B1ch + B2ch*B2ch + B3ch*B3ch);
#endif
#endif /* MHD */

#ifndef BAROTROPIC
        phalf[k][j][i] *= Gamma_1;
#endif

      }
    }
  }
  }

/*=== STEP 9: Compute 3D x1-Flux, x2-Flux, x3-Flux ===========================*/

/*--- Step 9a ------------------------------------------------------------------
 * Compute maximum wavespeeds in multidimensions (eta in eq. 10 from Sanders et
 *  al. (1998)) for H-correction
 */

#ifdef H_CORRECTION
  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+2; i++) {
        cfr = cfast(&(Ur_x1Face[k][j][i]) MHDARG( , &(B1_x1Face[k][j][i])));
        cfl = cfast(&(Ul_x1Face[k][j][i]) MHDARG( , &(B1_x1Face[k][j][i])));
        lambdar = Ur_x1Face[k][j][i].Mx/Ur_x1Face[k][j][i].d + cfr;
        lambdal = Ul_x1Face[k][j][i].Mx/Ul_x1Face[k][j][i].d - cfl;
        eta1[k][j][i] = 0.5*fabs(lambdar - lambdal);
      }
    }
  }

  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+1; i++) {
        cfr = cfast(&(Ur_x2Face[k][j][i]) MHDARG( , &(B2_x2Face[k][j][i])));
        cfl = cfast(&(Ul_x2Face[k][j][i]) MHDARG( , &(B2_x2Face[k][j][i])));
        lambdar = Ur_x2Face[k][j][i].Mx/Ur_x2Face[k][j][i].d + cfr;
        lambdal = Ul_x2Face[k][j][i].Mx/Ul_x2Face[k][j][i].d - cfl;
        eta2[k][j][i] = 0.5*fabs(lambdar - lambdal);
      }
    }
  }

  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
        cfr = cfast(&(Ur_x3Face[k][j][i]) MHDARG( , &(B3_x3Face[k][j][i])));
        cfl = cfast(&(Ul_x3Face[k][j][i]) MHDARG( , &(B3_x3Face[k][j][i])));
        lambdar = Ur_x3Face[k][j][i].Mx/Ur_x3Face[k][j][i].d + cfr;
        lambdal = Ul_x3Face[k][j][i].Mx/Ul_x3Face[k][j][i].d - cfl;
        eta3[k][j][i] = 0.5*fabs(lambdar - lambdal);
      }
    }
  }
#endif /* H_CORRECTION */

/*--- Step 9b ------------------------------------------------------------------
 * Compute 3D x1-fluxes from corrected L/R states.
 */

  for (k=ks-1; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is; i<=ie+1; i++) {
#ifdef H_CORRECTION
        etah = MAX(eta2[k][j][i-1],eta2[k][j][i]);
        etah = MAX(etah,eta2[k][j+1][i-1]);
        etah = MAX(etah,eta2[k][j+1][i  ]);

        etah = MAX(etah,eta3[k  ][j][i-1]);
        etah = MAX(etah,eta3[k  ][j][i  ]);
        etah = MAX(etah,eta3[k+1][j][i-1]);
        etah = MAX(etah,eta3[k+1][j][i  ]);

        etah = MAX(etah,eta1[k  ][j][i  ]);
#endif /* H_CORRECTION */
        Cons1D_to_Prim1D(&Ul_x1Face[k][j][i],&Wl[i]
                         MHDARG( , &B1_x1Face[k][j][i]));
        Cons1D_to_Prim1D(&Ur_x1Face[k][j][i],&Wr[i]
                         MHDARG( , &B1_x1Face[k][j][i]));

        GET_FLUXES(Ul_x1Face[k][j][i],Ur_x1Face[k][j][i],Wl[i],Wr[i],
                   MHDARG( B1_x1Face[k][j][i] , ) &x1Flux[k][j][i]);
      }
    }
  }

/*--- Step 9c ------------------------------------------------------------------
 * Compute 3D x2-fluxes from corrected L/R states.
 */

  for (k=ks-1; k<=ke+1; k++) {
    for (j=js; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
#ifdef H_CORRECTION
        etah = MAX(eta1[k][j-1][i],eta1[k][j][i]);
        etah = MAX(etah,eta1[k][j-1][i+1]);
        etah = MAX(etah,eta1[k][j  ][i+1]);

        etah = MAX(etah,eta3[k  ][j-1][i]);
        etah = MAX(etah,eta3[k  ][j  ][i]);
        etah = MAX(etah,eta3[k+1][j-1][i]);
        etah = MAX(etah,eta3[k+1][j  ][i]);

        etah = MAX(etah,eta2[k  ][j  ][i]);
#endif /* H_CORRECTION */
        Cons1D_to_Prim1D(&Ul_x2Face[k][j][i],&Wl[i]
                         MHDARG( , &B2_x2Face[k][j][i]));
        Cons1D_to_Prim1D(&Ur_x2Face[k][j][i],&Wr[i]
                         MHDARG( , &B2_x2Face[k][j][i]));

        GET_FLUXES(Ul_x2Face[k][j][i],Ur_x2Face[k][j][i],Wl[i],Wr[i],
                   MHDARG( B2_x2Face[k][j][i] , ) &x2Flux[k][j][i]);
      }
    }
  }

/*--- Step 9d ------------------------------------------------------------------
 * Compute 3D x3-fluxes from corrected L/R states.
 */

  for (k=ks; k<=ke+1; k++) {
    for (j=js-1; j<=je+1; j++) {
      for (i=is-1; i<=ie+1; i++) {
#ifdef H_CORRECTION
        etah = MAX(eta1[k-1][j][i],eta1[k][j][i]);
        etah = MAX(etah,eta1[k-1][j][i+1]);
        etah = MAX(etah,eta1[k][j  ][i+1]);

        etah = MAX(etah,eta2[k-1][j  ][i]);
        etah = MAX(etah,eta2[k  ][j  ][i]);
        etah = MAX(etah,eta2[k-1][j+1][i]);
        etah = MAX(etah,eta2[k  ][j+1][i]);

        etah = MAX(etah,eta3[k  ][j  ][i]);
#endif /* H_CORRECTION */
        Cons1D_to_Prim1D(&Ul_x3Face[k][j][i],&Wl[i]
                         MHDARG( , &B3_x3Face[k][j][i]));
        Cons1D_to_Prim1D(&Ur_x3Face[k][j][i],&Wr[i]
                         MHDARG( , &B3_x3Face[k][j][i]));

        GET_FLUXES(Ul_x3Face[k][j][i],Ur_x3Face[k][j][i],Wl[i],Wr[i],
                   MHDARG( B3_x3Face[k][j][i] , ) &x3Flux[k][j][i]);
      }
    }
  }

/*=== STEP 10: Update face-centered B for a full timestep ====================*/

/*--- Step 10a -----------------------------------------------------------------
 * Integrate emf*^{n+1/2} to the grid cell corners
 */

#ifdef MHD
  integrate_emf1_corner(pG);
  integrate_emf2_corner(pG);
  integrate_emf3_corner(pG);

/* Remap Ey at is and ie+1 to conserve Bz in shearing box */
#ifdef SHEARING_BOX
    get_myGridIndex(pD, pG->my_id, &my_iproc, &my_jproc, &my_kproc);

/* compute remapped Ey from opposite side of grid */

    if (my_iproc == 0) {
      RemapEy_ix1(pG, pD, emf2, remapEyiib);
    }
    if (my_iproc == (pD->NGrid_x1-1)) {
      RemapEy_ox1(pG, pD, emf2, remapEyoib);
    }

/* Now average Ey and remapped Ey */

    if (my_iproc == 0) {
      for(k=ks; k<=ke+1; k++) {
        for(j=js; j<=je; j++){
          emf2[k][j][is]  = 0.5*(emf2[k][j][is] + remapEyiib[k][j]);
        }
      }
    }

    if (my_iproc == (pD->NGrid_x1-1)) {
      for(k=ks; k<=ke+1; k++) {
        for(j=js; j<=je; j++){
          emf2[k][j][ie+1]  = 0.5*(emf2[k][j][ie+1] + remapEyoib[k][j]);
        }
      }
    }
#endif /* SHEARING_BOX */

/*--- Step 10b -----------------------------------------------------------------
 * Update the interface magnetic fields using CT for a full time step.
 */

  for (k=ks; k<=ke; k++) {
    for (j=js; j<=je; j++) {
      for (i=is; i<=ie; i++) {
        pG->B1i[k][j][i] += dtodx3*(emf2[k+1][j  ][i  ] - emf2[k][j][i]) -
                            dtodx2*(emf3[k  ][j+1][i  ] - emf3[k][j][i]);
        pG->B2i[k][j][i] += dtodx1*(emf3[k  ][j  ][i+1] - emf3[k][j][i]) -
                            dtodx3*(emf1[k+1][j  ][i  ] - emf1[k][j][i]);
        pG->B3i[k][j][i] += dtodx2*(emf1[k  ][j+1][i  ] - emf1[k][j][i]) -
                            dtodx1*(emf2[k  ][j  ][i+1] - emf2[k][j][i]);
      }
      pG->B1i[k][j][ie+1] +=
        dtodx3*(emf2[k+1][j  ][ie+1] - emf2[k][j][ie+1]) -
        dtodx2*(emf3[k  ][j+1][ie+1] - emf3[k][j][ie+1]);
    }
    for (i=is; i<=ie; i++) {
      pG->B2i[k][je+1][i] +=
        dtodx1*(emf3[k  ][je+1][i+1] - emf3[k][je+1][i]) -
        dtodx3*(emf1[k+1][je+1][i  ] - emf1[k][je+1][i]);
    }
  }
  for (j=js; j<=je; j++) {
    for (i=is; i<=ie; i++) {
      pG->B3i[ke+1][j][i] += 
        dtodx2*(emf1[ke+1][j+1][i  ] - emf1[ke+1][j][i]) -
        dtodx1*(emf2[ke+1][j  ][i+1] - emf2[ke+1][j][i]);
    }
  }
#endif

/*=== STEP 11: Add source terms for a full timestep using n+1/2 states =======*/

/*--- Step 11a -----------------------------------------------------------------
 * Add gravitational (or shearing box) source terms as a Static Potential.
 *   A Crank-Nicholson update is used for shearing box terms.
 *   The energy source terms computed at cell faces are averaged to improve
 * conservation of total energy.
 *    S_{M} = -(\rho)^{n+1/2} Grad(Phi);   S_{E} = -(\rho v)^{n+1/2} Grad{Phi}
 */

#ifdef SHEARING_BOX
  fact = om_dt/(1.0 + 0.25*om_dt*om_dt);
  TH_om = 1.5*Omega; /* Three-Halves Omega */
  for(k=ks; k<=ke; ++k) {
    for(j=js; j<=je; ++j) {
      for(i=is; i<=ie; ++i) {
	cc_pos(pG,i,j,k,&x1,&x2,&x3);

/* Store the current state */
	M1n  = pG->U[k][j][i].M1;
#ifdef FARGO
	dM2n = pG->U[k][j][i].M2;
#else
	dM2n = pG->U[k][j][i].M2 + pG->U[k][j][i].d*TH_om*x1;
#endif

/* Calculate the flux for the y-momentum fluctuation */
	frx1_dM2 = x1Flux[k][j][i+1].My;
	flx1_dM2 = x1Flux[k][j][i  ].My;
	frx2_dM2 = x2Flux[k][j+1][i].Mx;
	flx2_dM2 = x2Flux[k][j  ][i].Mx;
	frx3_dM2 = x3Flux[k+1][j][i].Mz;
	flx3_dM2 = x3Flux[k  ][j][i].Mz;
#ifndef FARGO
	frx1_dM2 += TH_om*(x1+0.5*pG->dx1)*x1Flux[k][j][i+1].d;
	flx1_dM2 += TH_om*(x1-0.5*pG->dx1)*x1Flux[k][j][i  ].d;
	frx2_dM2 += TH_om*(x1)*x2Flux[k][j+1][i].d;
	flx2_dM2 += TH_om*(x1)*x2Flux[k][j  ][i].d;
	frx3_dM2 += TH_om*(x1)*x3Flux[k+1][j][i].d;
	flx3_dM2 += TH_om*(x1)*x3Flux[k  ][j][i].d;
#endif

/* Now evolve M1n and dM2n by dt/2 using Forward Euler */
	M1e = M1n - q1*(x1Flux[k][j][i+1].Mx - x1Flux[k][j][i].Mx)
	          - q2*(x2Flux[k][j+1][i].Mz - x2Flux[k][j][i].Mz)
	          - q3*(x3Flux[k+1][j][i].My - x3Flux[k][j][i].My);

	dM2e = dM2n - q1*(frx1_dM2 - flx1_dM2)
	            - q2*(frx2_dM2 - flx2_dM2) 
                    - q3*(frx3_dM2 - flx3_dM2);

/* Update the 1- and 2-momentum for the Coriolis and tidal
 * potential momentum source terms using a Crank-Nicholson
 * discretization for the momentum fluctuation equation. */

	pG->U[k][j][i].M1 += (2.0*dM2e - 0.5*om_dt*M1e)*fact;
	pG->U[k][j][i].M2 -= 0.5*(M1e + om_dt*dM2e)*fact;
#ifndef FARGO
	pG->U[k][j][i].M2 -= 0.75*om_dt*(x1Flux[k][j][i].d+x1Flux[k][j][i+1].d);
#endif

/* Update the energy for a fixed potential, and add the Z-component (M3)
 * of the gravitational acceleration.
 * This update is identical to non-SHEARING_BOX below  */

	phic = (*StaticGravPot)((x1            ),x2,x3);
	phir = (*StaticGravPot)((x1+0.5*pG->dx1),x2,x3);
	phil = (*StaticGravPot)((x1-0.5*pG->dx1),x2,x3);
#ifndef BAROTROPIC
	pG->U[k][j][i].E -= dtodx1*(x1Flux[k][j][i  ].d*(phic - phil) +
                                    x1Flux[k][j][i+1].d*(phir - phic));
#endif

	phir = (*StaticGravPot)(x1,(x2+0.5*pG->dx2),x3);
	phil = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),x3);
#ifndef BAROTROPIC
	pG->U[k][j][i].E -= dtodx2*(x2Flux[k][j  ][i].d*(phic - phil) +
                                    x2Flux[k][j+1][i].d*(phir - phic));
#endif

	phir = (*StaticGravPot)(x1,x2,(x3+0.5*pG->dx3));
	phil = (*StaticGravPot)(x1,x2,(x3-0.5*pG->dx3));
	pG->U[k][j][i].M3 -= dtodx3*(phir-phil)*dhalf[k][j][i];
#ifndef BAROTROPIC
	pG->U[k][j][i].E -= dtodx3*(x3Flux[k  ][j][i].d*(phic - phil) +
                                    x3Flux[k+1][j][i].d*(phir - phic));
#endif
      }
    }
  }

#else /* ! SHEARING_BOX */

  if (StaticGravPot != NULL){
    for (k=ks; k<=ke; k++) {
      for (j=js; j<=je; j++) {
        for (i=is; i<=ie; i++) {
          cc_pos(pG,i,j,k,&x1,&x2,&x3);
          phic = (*StaticGravPot)((x1            ),x2,x3);
          phir = (*StaticGravPot)((x1+0.5*pG->dx1),x2,x3);
          phil = (*StaticGravPot)((x1-0.5*pG->dx1),x2,x3);
          pG->U[k][j][i].M1 -= dtodx1*(phir-phil)*dhalf[k][j][i];
#ifndef BAROTROPIC
          pG->U[k][j][i].E -= dtodx1*(x1Flux[k][j][i  ].d*(phic - phil) +
                                      x1Flux[k][j][i+1].d*(phir - phic));
#endif
          phir = (*StaticGravPot)(x1,(x2+0.5*pG->dx2),x3);
          phil = (*StaticGravPot)(x1,(x2-0.5*pG->dx2),x3);
          pG->U[k][j][i].M2 -= dtodx2*(phir-phil)*dhalf[k][j][i];
#ifndef BAROTROPIC
          pG->U[k][j][i].E -= dtodx2*(x2Flux[k][j  ][i].d*(phic - phil) +
                                      x2Flux[k][j+1][i].d*(phir - phic));
#endif
          phir = (*StaticGravPot)(x1,x2,(x3+0.5*pG->dx3));
          phil = (*StaticGravPot)(x1,x2,(x3-0.5*pG->dx3));
          pG->U[k][j][i].M3 -= dtodx3*(phir-phil)*dhalf[k][j][i];
#ifndef BAROTROPIC
          pG->U[k][j][i].E -= dtodx3*(x3Flux[k  ][j][i].d*(phic - phil) +
                                      x3Flux[k+1][j][i].d*(phir - phic));
#endif
        }
      }
    }
  }
#endif /* SHEARING_BOX */

/*--- Step 11b -----------------------------------------------------------------
 * Add source terms for self-gravity.
 * A flux correction using Phi^{n+1} in the main loop is required to make
 * the source terms 2nd order: see selfg_flux_correction().
 */

#ifdef SELF_GRAVITY
/* Add fluxes and source terms due to (d/dx1) terms  */

  for (k=ks; k<=ke; k++){
    for (j=js; j<=je; j++){
      for (i=is; i<=ie; i++){
        phic = pG->Phi[k][j][i];
        phil = 0.5*(pG->Phi[k][j][i-1] + pG->Phi[k][j][i  ]);
        phir = 0.5*(pG->Phi[k][j][i  ] + pG->Phi[k][j][i+1]);

/* gx, gy and gz centered at L and R x1-faces */
        gxl = (pG->Phi[k][j][i-1] - pG->Phi[k][j][i  ])*(dx1i);
        gxr = (pG->Phi[k][j][i  ] - pG->Phi[k][j][i+1])*(dx1i);

        gyl = 0.25*((pG->Phi[k][j-1][i-1] - pG->Phi[k][j+1][i-1]) +
                    (pG->Phi[k][j-1][i  ] - pG->Phi[k][j+1][i  ]) )*(dx2i);
        gyr = 0.25*((pG->Phi[k][j-1][i  ] - pG->Phi[k][j+1][i  ]) +
                    (pG->Phi[k][j-1][i+1] - pG->Phi[k][j+1][i+1]) )*(dx2i);

        gzl = 0.25*((pG->Phi[k-1][j][i-1] - pG->Phi[k+1][j][i-1]) +
                    (pG->Phi[k-1][j][i  ] - pG->Phi[k+1][j][i  ]) )*(dx3i);
        gzr = 0.25*((pG->Phi[k-1][j][i  ] - pG->Phi[k+1][j][i  ]) +
                    (pG->Phi[k-1][j][i+1] - pG->Phi[k+1][j][i+1]) )*(dx3i);

/* momentum fluxes in x1.  2nd term is needed only if Jean's swindle used */
        flx_m1l = 0.5*(gxl*gxl-gyl*gyl-gzl*gzl)/four_pi_G + grav_mean_rho*phil;
        flx_m1r = 0.5*(gxr*gxr-gyr*gyr-gzr*gzr)/four_pi_G + grav_mean_rho*phir;

        flx_m2l = gxl*gyl/four_pi_G;
        flx_m2r = gxr*gyr/four_pi_G;

        flx_m3l = gxl*gzl/four_pi_G;
        flx_m3r = gxr*gzr/four_pi_G;

/* Update momenta and energy with d/dx1 terms  */
        pG->U[k][j][i].M1 -= dtodx1*(flx_m1r - flx_m1l);
        pG->U[k][j][i].M2 -= dtodx1*(flx_m2r - flx_m2l);
        pG->U[k][j][i].M3 -= dtodx1*(flx_m3r - flx_m3l);
#ifndef BAROTROPIC
        pG->U[k][j][i].E -= dtodx1*(x1Flux[k][j][i  ].d*(phic - phil) +
                                    x1Flux[k][j][i+1].d*(phir - phic));
#endif /* BAROTROPIC */
      }
    }
  }

/* Add fluxes and source terms due to (d/dx2) terms  */

  for (k=ks; k<=ke; k++){
    for (j=js; j<=je; j++){
      for (i=is; i<=ie; i++){
        phic = pG->Phi[k][j][i];
        phil = 0.5*(pG->Phi[k][j-1][i] + pG->Phi[k][j  ][i]);
        phir = 0.5*(pG->Phi[k][j  ][i] + pG->Phi[k][j+1][i]);

/* gx, gy and gz centered at L and R x2-faces */
        gxl = 0.25*((pG->Phi[k][j-1][i-1] - pG->Phi[k][j-1][i+1]) +
                    (pG->Phi[k][j  ][i-1] - pG->Phi[k][j  ][i+1]) )*(dx1i);
        gxr = 0.25*((pG->Phi[k][j  ][i-1] - pG->Phi[k][j  ][i+1]) +
                    (pG->Phi[k][j+1][i-1] - pG->Phi[k][j+1][i+1]) )*(dx1i);

        gyl = (pG->Phi[k][j-1][i] - pG->Phi[k][j  ][i])*(dx2i);
        gyr = (pG->Phi[k][j  ][i] - pG->Phi[k][j+1][i])*(dx2i);

        gzl = 0.25*((pG->Phi[k-1][j-1][i] - pG->Phi[k+1][j-1][i]) +
                    (pG->Phi[k-1][j  ][i] - pG->Phi[k+1][j  ][i]) )*(dx3i);
        gzr = 0.25*((pG->Phi[k-1][j  ][i] - pG->Phi[k+1][j  ][i]) +
                    (pG->Phi[k-1][j+1][i] - pG->Phi[k+1][j+1][i]) )*(dx3i);

/* momentum fluxes in x2.  2nd term is needed only if Jean's swindle used */
        flx_m1l = gyl*gxl/four_pi_G;
        flx_m1r = gyr*gxr/four_pi_G;

        flx_m2l = 0.5*(gyl*gyl-gxl*gxl-gzl*gzl)/four_pi_G + grav_mean_rho*phil;
        flx_m2r = 0.5*(gyr*gyr-gxr*gxr-gzr*gzr)/four_pi_G + grav_mean_rho*phir;

        flx_m3l = gyl*gzl/four_pi_G;
        flx_m3r = gyr*gzr/four_pi_G;

/* Update momenta and energy with d/dx2 terms  */
        pG->U[k][j][i].M1 -= dtodx2*(flx_m1r - flx_m1l);
        pG->U[k][j][i].M2 -= dtodx2*(flx_m2r - flx_m2l);
        pG->U[k][j][i].M3 -= dtodx2*(flx_m3r - flx_m3l);
#ifndef BAROTROPIC
        pG->U[k][j][i].E -= dtodx2*(x2Flux[k][j  ][i].d*(phic - phil) +
                                    x2Flux[k][j+1][i].d*(phir - phic));
#endif /* BAROTROPIC */
      }
    }
  }

/* Add fluxes and source terms due to (d/dx3) terms  */

  for (k=ks; k<=ke; k++){
    for (j=js; j<=je; j++){
      for (i=is; i<=ie; i++){
        phic = pG->Phi[k][j][i];
        phil = 0.5*(pG->Phi[k-1][j][i] + pG->Phi[k  ][j][i]);
        phir = 0.5*(pG->Phi[k  ][j][i] + pG->Phi[k+1][j][i]);

/* gx, gy and gz centered at L and R x3-faces */
        gxl = 0.25*((pG->Phi[k-1][j][i-1] - pG->Phi[k-1][j][i+1]) +
                    (pG->Phi[k  ][j][i-1] - pG->Phi[k  ][j][i+1]) )*(dx1i);
        gxr = 0.25*((pG->Phi[k  ][j][i-1] - pG->Phi[k  ][j][i+1]) +
                    (pG->Phi[k+1][j][i-1] - pG->Phi[k+1][j][i+1]) )*(dx1i);

        gyl = 0.25*((pG->Phi[k-1][j-1][i] - pG->Phi[k-1][j+1][i]) +
                    (pG->Phi[k  ][j-1][i] - pG->Phi[k  ][j+1][i]) )*(dx2i);
        gyr = 0.25*((pG->Phi[k  ][j-1][i] - pG->Phi[k  ][j+1][i]) +
                    (pG->Phi[k+1][j-1][i] - pG->Phi[k+1][j+1][i]) )*(dx2i);

        gzl = (pG->Phi[k-1][j][i] - pG->Phi[k  ][j][i])*(dx3i);
        gzr = (pG->Phi[k  ][j][i] - pG->Phi[k+1][j][i])*(dx3i);

/* momentum fluxes in x3.  2nd term is needed only if Jean's swindle used */
        flx_m1l = gzl*gxl/four_pi_G;
        flx_m1r = gzr*gxr/four_pi_G;

        flx_m2l = gzl*gyl/four_pi_G;
        flx_m2r = gzr*gyr/four_pi_G;

        flx_m3l = 0.5*(gzl*gzl-gxl*gxl-gyl*gyl)/four_pi_G + grav_mean_rho*phil;
        flx_m3r = 0.5*(gzr*gzr-gxr*gxr-gyr*gyr)/four_pi_G + grav_mean_rho*phir;

/* Update momenta and energy with d/dx3 terms  */
        pG->U[k][j][i].M1 -= dtodx3*(flx_m1r - flx_m1l);
        pG->U[k][j][i].M2 -= dtodx3*(flx_m2r - flx_m2l);
        pG->U[k][j][i].M3 -= dtodx3*(flx_m3r - flx_m3l);
#ifndef BAROTROPIC
        pG->U[k][j][i].E -= dtodx3*(x3Flux[k  ][j][i].d*(phic - phil) +
                                    x3Flux[k+1][j][i].d*(phir - phic));
#endif /* BAROTROPIC */
      }
    }
  }

/* Save mass fluxes in Grid structure for source term correction in main loop */

  for (k=ks; k<=ke+1; k++) {
    for (j=js; j<=je+1; j++) {
      for (i=is; i<=ie+1; i++) {
        pG->x1MassFlux[k][j][i] = x1Flux[k][j][i].d;
        pG->x2MassFlux[k][j][i] = x2Flux[k][j][i].d;
        pG->x3MassFlux[k][j][i] = x3Flux[k][j][i].d;
      }
    }
  }
#endif /* SELF_GRAVITY */

/*--- Step 11c -----------------------------------------------------------------
 * Add source terms for optically thin cooling
 */

#ifndef BAROTROPIC
  if (CoolingFunc != NULL){
    for (k=ks; k<=ke; k++){
      for (j=js; j<=je; j++){
        for (i=is; i<=ie; i++){
          coolf = (*CoolingFunc)(dhalf[k][j][i],phalf[k][j][i],pG->dt);
          pG->U[k][j][i].E -= pG->dt*coolf;
        }
      }
    }
  }
#endif /* BAROTROPIC */

/*=== STEP 12: Update cell-centered values for a full timestep ===============*/

/*--- Step 12a -----------------------------------------------------------------
 * Update cell-centered variables in pG using 3D x1-Fluxes
 */

  for (k=ks; k<=ke; k++) {
    for (j=js; j<=je; j++) {
      for (i=is; i<=ie; i++) {
        pG->U[k][j][i].d  -= dtodx1*(x1Flux[k][j][i+1].d -x1Flux[k][j][i].d );
        pG->U[k][j][i].M1 -= dtodx1*(x1Flux[k][j][i+1].Mx-x1Flux[k][j][i].Mx);
        pG->U[k][j][i].M2 -= dtodx1*(x1Flux[k][j][i+1].My-x1Flux[k][j][i].My);
        pG->U[k][j][i].M3 -= dtodx1*(x1Flux[k][j][i+1].Mz-x1Flux[k][j][i].Mz);
#ifndef BAROTROPIC
        pG->U[k][j][i].E  -= dtodx1*(x1Flux[k][j][i+1].E -x1Flux[k][j][i].E );
#endif /* BAROTROPIC */
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++)
          pG->U[k][j][i].s[n] -= dtodx1*(x1Flux[k][j][i+1].s[n]
                                       - x1Flux[k][j][i  ].s[n]);
#endif
      }
    }
  }

/*--- Step 12b -----------------------------------------------------------------
 * Update cell-centered variables in pG using 3D x2-Fluxes
 */

  for (k=ks; k<=ke; k++) {
    for (j=js; j<=je; j++) {
      for (i=is; i<=ie; i++) {
        pG->U[k][j][i].d  -= dtodx2*(x2Flux[k][j+1][i].d -x2Flux[k][j][i].d );
        pG->U[k][j][i].M1 -= dtodx2*(x2Flux[k][j+1][i].Mz-x2Flux[k][j][i].Mz);
        pG->U[k][j][i].M2 -= dtodx2*(x2Flux[k][j+1][i].Mx-x2Flux[k][j][i].Mx);
        pG->U[k][j][i].M3 -= dtodx2*(x2Flux[k][j+1][i].My-x2Flux[k][j][i].My);
#ifndef BAROTROPIC
        pG->U[k][j][i].E -=dtodx2*(x2Flux[k][j+1][i].E -x2Flux[k][j][i].E );
#endif /* BAROTROPIC */
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++)
          pG->U[k][j][i].s[n] -= dtodx2*(x2Flux[k][j+1][i].s[n]
                                       - x2Flux[k][j  ][i].s[n]);
#endif
      }
    }
  }

/*--- Step 12c -----------------------------------------------------------------
 * Update cell-centered variables in pG using 3D x3-Fluxes
 */

  for (k=ks; k<=ke; k++) {
    for (j=js; j<=je; j++) {
      for (i=is; i<=ie; i++) {
        pG->U[k][j][i].d  -= dtodx3*(x3Flux[k+1][j][i].d -x3Flux[k][j][i].d );
        pG->U[k][j][i].M1 -= dtodx3*(x3Flux[k+1][j][i].My-x3Flux[k][j][i].My);
        pG->U[k][j][i].M2 -= dtodx3*(x3Flux[k+1][j][i].Mz-x3Flux[k][j][i].Mz);
        pG->U[k][j][i].M3 -= dtodx3*(x3Flux[k+1][j][i].Mx-x3Flux[k][j][i].Mx);
#ifndef BAROTROPIC
        pG->U[k][j][i].E  -= dtodx3*(x3Flux[k+1][j][i].E -x3Flux[k][j][i].E );
#endif /* BAROTROPIC */
#if (NSCALARS > 0)
        for (n=0; n<NSCALARS; n++)
          pG->U[k][j][i].s[n] -= dtodx3*(x3Flux[k+1][j][i].s[n]
                                       - x3Flux[k  ][j][i].s[n]);
#endif
      }
    }
  }

/*--- Step 12d -----------------------------------------------------------------
 * LAST STEP!
 * Set cell centered magnetic fields to average of updated face centered fields.
 */

#ifdef MHD
  for (k=ks; k<=ke; k++) {
    for (j=js; j<=je; j++) {
      for (i=is; i<=ie; i++) {
        pG->U[k][j][i].B1c = 0.5*(pG->B1i[k][j][i]+pG->B1i[k][j][i+1]);
        pG->U[k][j][i].B2c = 0.5*(pG->B2i[k][j][i]+pG->B2i[k][j+1][i]);
        pG->U[k][j][i].B3c = 0.5*(pG->B3i[k][j][i]+pG->B3i[k+1][j][i]);
      }
    }
  }
#endif /* MHD */

  return;
}

/*----------------------------------------------------------------------------*/
/* integrate_destruct_3d:  Free temporary integration arrays 
 */

void integrate_destruct_3d(void)
{

#ifdef MHD
  if (emf1    != NULL) free_3d_array(emf1);
  if (emf2    != NULL) free_3d_array(emf2);
  if (emf3    != NULL) free_3d_array(emf3);
  if (emf1_cc != NULL) free_3d_array(emf1_cc);
  if (emf2_cc != NULL) free_3d_array(emf2_cc);
  if (emf3_cc != NULL) free_3d_array(emf3_cc);
#endif /* MHD */
#ifdef H_CORRECTION
  if (eta1 != NULL) free_3d_array(eta1);
  if (eta2 != NULL) free_3d_array(eta2);
  if (eta3 != NULL) free_3d_array(eta3);
#endif /* H_CORRECTION */

#ifdef MHD
  if (Bxc != NULL) free(Bxc);
  if (Bxi != NULL) free(Bxi);
  if (B1_x1Face != NULL) free_3d_array(B1_x1Face);
  if (B2_x2Face != NULL) free_3d_array(B2_x2Face);
  if (B3_x3Face != NULL) free_3d_array(B3_x3Face);
#endif /* MHD */

  if (U1d      != NULL) free(U1d);
  if (W        != NULL) free(W);
  if (Wl       != NULL) free(Wl);
  if (Wr       != NULL) free(Wr);

  if (Ul_x1Face != NULL) free_3d_array(Ul_x1Face);
  if (Ur_x1Face != NULL) free_3d_array(Ur_x1Face);
  if (Ul_x2Face != NULL) free_3d_array(Ul_x2Face);
  if (Ur_x2Face != NULL) free_3d_array(Ur_x2Face);
  if (Ul_x3Face != NULL) free_3d_array(Ul_x3Face);
  if (Ur_x3Face != NULL) free_3d_array(Ur_x3Face);
  if (x1Flux    != NULL) free_3d_array(x1Flux);
  if (x2Flux    != NULL) free_3d_array(x2Flux);
  if (x3Flux    != NULL) free_3d_array(x3Flux);
  if (dhalf     != NULL) free_3d_array(dhalf);
  if (phalf     != NULL) free_3d_array(phalf);
#ifdef SHEARING_BOX
  if (remapEyiib != NULL) free_2d_array(remapEyiib);
  if (remapEyoib != NULL) free_2d_array(remapEyoib);
#endif

  return;
}

/*----------------------------------------------------------------------------*/
/* integrate_init_3d: Allocate temporary integration arrays 
*/

void integrate_init_3d(int nx1, int nx2, int nx3)
{
  int nmax;
  int Nx1 = nx1 + 2*nghost;
  int Nx2 = nx2 + 2*nghost;
  int Nx3 = nx3 + 2*nghost;
  nmax = MAX(MAX(Nx1,Nx2),Nx3);

#ifdef MHD
  if ((emf1 = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((emf2 = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((emf3 = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;

  if ((emf1_cc = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((emf2_cc = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((emf3_cc = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
#endif /* MHD */
#ifdef H_CORRECTION
  if ((eta1 = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((eta2 = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((eta3 = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
#endif /* H_CORRECTION */

#ifdef MHD
  if ((Bxc = (Real*)malloc(nmax*sizeof(Real))) == NULL) goto on_error;
  if ((Bxi = (Real*)malloc(nmax*sizeof(Real))) == NULL) goto on_error;

  if ((B1_x1Face = (Real***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((B2_x2Face = (Real***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((B3_x3Face = (Real***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Real))) == NULL)
    goto on_error;
#endif /* MHD */

  if ((U1d =      (Cons1D*)malloc(nmax*sizeof(Cons1D))) == NULL) goto on_error;
  if ((W   =      (Prim1D*)malloc(nmax*sizeof(Prim1D))) == NULL) goto on_error;
  if ((Wl  =      (Prim1D*)malloc(nmax*sizeof(Prim1D))) == NULL) goto on_error;
  if ((Wr  =      (Prim1D*)malloc(nmax*sizeof(Prim1D))) == NULL) goto on_error;

  if ((Ul_x1Face = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D)))
    == NULL) goto on_error;
  if ((Ur_x1Face = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D)))
    == NULL) goto on_error;
  if ((Ul_x2Face = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D)))
    == NULL) goto on_error;
  if ((Ur_x2Face = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D)))
    == NULL) goto on_error;
  if ((Ul_x3Face = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D))) 
    == NULL) goto on_error;
  if ((Ur_x3Face = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D))) 
    == NULL) goto on_error;
  if ((x1Flux    = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D))) 
    == NULL) goto on_error;
  if ((x2Flux    = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D))) 
    == NULL) goto on_error;
  if ((x3Flux    = (Cons1D***)calloc_3d_array(Nx3,Nx2,Nx1, sizeof(Cons1D))) 
    == NULL) goto on_error;


#ifndef MHD
  if((StaticGravPot != NULL) || (CoolingFunc != NULL))
#endif
  {
  if ((dhalf = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  if ((phalf = (Real***)calloc_3d_array(Nx3, Nx2, Nx1, sizeof(Real))) == NULL)
    goto on_error;
  }

#ifdef SHEARING_BOX
  if ((remapEyiib = (Real**)calloc_2d_array(Nx3,Nx2, sizeof(Real))) == NULL)
    goto on_error;
  if ((remapEyoib = (Real**)calloc_2d_array(Nx3,Nx2, sizeof(Real))) == NULL)
    goto on_error;
#endif

  return;

  on_error:
  integrate_destruct();
  ath_error("[integrate_init]: malloc returned a NULL pointer\n");
}


/*=========================== PRIVATE FUNCTIONS ==============================*/

/*----------------------------------------------------------------------------*/
/* integrate_emf1_corner
 * integrate_emf2_corner
 * integrate_emf3_corner
 *   Integrates face centered B-fluxes to compute corner EMFs.  Note:
 *   x1Flux.By = VxBy - BxVy = v1*b2-b1*v2 = -EMFZ
 *   x1Flux.Bz = VxBz - BxVz = v1*b3-b1*v3 = EMFY
 *   x2Flux.By = VxBy - BxVy = v2*b3-b2*v3 = -EMFX
 *   x2Flux.Bz = VxBz - BxVz = v2*b1-b2*v1 = EMFZ
 *   x3Flux.By = VxBy - BxVy = v3*b1-b3*v1 = -EMFY
 *   x3Flux.Bz = VxBz - BxVz = v3*b2-b3*v2 = EMFX 
 */

#ifdef MHD
static void integrate_emf1_corner(const Grid *pG)
{
  int i, is = pG->is, ie = pG->ie;
  int j, js = pG->js, je = pG->je;
  int k, ks = pG->ks, ke = pG->ke;
  Real de1_l2, de1_r2, de1_l3, de1_r3;

  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-2; i<=ie+2; i++) {
/* NOTE: The x2-Flux of By is -E1. */
/*       The x3-Flux of Bz is +E1. */
	if (x2Flux[k-1][j][i].d > 0.0)
	  de1_l3 = x3Flux[k][j-1][i].Bz - emf1_cc[k-1][j-1][i];
	else if (x2Flux[k-1][j][i].d < 0.0)
	  de1_l3 = x3Flux[k][j][i].Bz - emf1_cc[k-1][j][i];
	else {
	  de1_l3 = 0.5*(x3Flux[k][j-1][i].Bz - emf1_cc[k-1][j-1][i] +
			x3Flux[k][j  ][i].Bz - emf1_cc[k-1][j  ][i] );
	}

	if (x2Flux[k][j][i].d > 0.0)
	  de1_r3 = x3Flux[k][j-1][i].Bz - emf1_cc[k][j-1][i];
	else if (x2Flux[k][j][i].d < 0.0)
	  de1_r3 = x3Flux[k][j][i].Bz - emf1_cc[k][j][i];
	else {
	  de1_r3 = 0.5*(x3Flux[k][j-1][i].Bz - emf1_cc[k][j-1][i] +
			x3Flux[k][j  ][i].Bz - emf1_cc[k][j  ][i] );
	}

	if (x3Flux[k][j-1][i].d > 0.0)
	  de1_l2 = -x2Flux[k-1][j][i].By - emf1_cc[k-1][j-1][i];
	else if (x3Flux[k][j-1][i].d < 0.0)
	  de1_l2 = -x2Flux[k][j][i].By - emf1_cc[k][j-1][i];
	else {
	  de1_l2 = 0.5*(-x2Flux[k-1][j][i].By - emf1_cc[k-1][j-1][i]
			-x2Flux[k  ][j][i].By - emf1_cc[k  ][j-1][i] );
	}

	if (x3Flux[k][j][i].d > 0.0)
	  de1_r2 = -x2Flux[k-1][j][i].By - emf1_cc[k-1][j][i];
	else if (x3Flux[k][j][i].d < 0.0)
	  de1_r2 = -x2Flux[k][j][i].By - emf1_cc[k][j][i];
	else {
	  de1_r2 = 0.5*(-x2Flux[k-1][j][i].By - emf1_cc[k-1][j][i]
			-x2Flux[k  ][j][i].By - emf1_cc[k  ][j][i] );
	}

        emf1[k][j][i] = 0.25*(  x3Flux[k][j][i].Bz + x3Flux[k][j-1][i].Bz
                              - x2Flux[k][j][i].By - x2Flux[k-1][j][i].By 
			      + de1_l2 + de1_r2 + de1_l3 + de1_r3);
      }
    }
  }

  return;
}


static void integrate_emf2_corner(const Grid *pG)
{
  int i, is = pG->is, ie = pG->ie;
  int j, js = pG->js, je = pG->je;
  int k, ks = pG->ks, ke = pG->ke;
  Real de2_l1, de2_r1, de2_l3, de2_r3;

  for (k=ks-1; k<=ke+2; k++) {
    for (j=js-2; j<=je+2; j++) {
      for (i=is-1; i<=ie+2; i++) {
/* NOTE: The x1-Flux of Bz is +E2. */
/*       The x3-Flux of By is -E2. */
	if (x1Flux[k-1][j][i].d > 0.0)
	  de2_l3 = -x3Flux[k][j][i-1].By - emf2_cc[k-1][j][i-1];
	else if (x1Flux[k-1][j][i].d < 0.0)
	  de2_l3 = -x3Flux[k][j][i].By - emf2_cc[k-1][j][i];
	else {
	  de2_l3 = 0.5*(-x3Flux[k][j][i-1].By - emf2_cc[k-1][j][i-1] 
			-x3Flux[k][j][i  ].By - emf2_cc[k-1][j][i  ] );
	}

	if (x1Flux[k][j][i].d > 0.0)
	  de2_r3 = -x3Flux[k][j][i-1].By - emf2_cc[k][j][i-1];
	else if (x1Flux[k][j][i].d < 0.0)
	  de2_r3 = -x3Flux[k][j][i].By - emf2_cc[k][j][i];
	else {
	  de2_r3 = 0.5*(-x3Flux[k][j][i-1].By - emf2_cc[k][j][i-1] 
			-x3Flux[k][j][i  ].By - emf2_cc[k][j][i  ] );
	}

	if (x3Flux[k][j][i-1].d > 0.0)
	  de2_l1 = x1Flux[k-1][j][i].Bz - emf2_cc[k-1][j][i-1];
	else if (x3Flux[k][j][i-1].d < 0.0)
	  de2_l1 = x1Flux[k][j][i].Bz - emf2_cc[k][j][i-1];
	else {
	  de2_l1 = 0.5*(x1Flux[k-1][j][i].Bz - emf2_cc[k-1][j][i-1] +
			x1Flux[k  ][j][i].Bz - emf2_cc[k  ][j][i-1] );
	}

	if (x3Flux[k][j][i].d > 0.0)
	  de2_r1 = x1Flux[k-1][j][i].Bz - emf2_cc[k-1][j][i];
	else if (x3Flux[k][j][i].d < 0.0)
	  de2_r1 = x1Flux[k][j][i].Bz - emf2_cc[k][j][i];
	else {
	  de2_r1 = 0.5*(x1Flux[k-1][j][i].Bz - emf2_cc[k-1][j][i] +
			x1Flux[k  ][j][i].Bz - emf2_cc[k  ][j][i] );
	}

	emf2[k][j][i] = 0.25*(  x1Flux[k][j][i].Bz + x1Flux[k-1][j][i  ].Bz
                              - x3Flux[k][j][i].By - x3Flux[k  ][j][i-1].By
			      + de2_l1 + de2_r1 + de2_l3 + de2_r3);
      }
    }
  }

  return;
}

static void integrate_emf3_corner(const Grid *pG)
{
  int i, is = pG->is, ie = pG->ie;
  int j, js = pG->js, je = pG->je;
  int k, ks = pG->ks, ke = pG->ke;
  Real de3_l1, de3_r1, de3_l2, de3_r2;

  for (k=ks-2; k<=ke+2; k++) {
    for (j=js-1; j<=je+2; j++) {
      for (i=is-1; i<=ie+2; i++) {
/* NOTE: The x1-Flux of By is -E3. */
/*       The x2-Flux of Bx is +E3. */
	if (x1Flux[k][j-1][i].d > 0.0)
	  de3_l2 = x2Flux[k][j][i-1].Bz - emf3_cc[k][j-1][i-1];
	else if (x1Flux[k][j-1][i].d < 0.0)
	  de3_l2 = x2Flux[k][j][i].Bz - emf3_cc[k][j-1][i];
	else {
	  de3_l2 = 0.5*(x2Flux[k][j][i-1].Bz - emf3_cc[k][j-1][i-1] + 
			x2Flux[k][j][i  ].Bz - emf3_cc[k][j-1][i  ] );
	}

	if (x1Flux[k][j][i].d > 0.0)
	  de3_r2 = x2Flux[k][j][i-1].Bz - emf3_cc[k][j][i-1];
	else if (x1Flux[k][j][i].d < 0.0)
	  de3_r2 = x2Flux[k][j][i].Bz - emf3_cc[k][j][i];
	else {
	  de3_r2 = 0.5*(x2Flux[k][j][i-1].Bz - emf3_cc[k][j][i-1] + 
			x2Flux[k][j][i  ].Bz - emf3_cc[k][j][i  ] );
	}

	if (x2Flux[k][j][i-1].d > 0.0)
	  de3_l1 = -x1Flux[k][j-1][i].By - emf3_cc[k][j-1][i-1];
	else if (x2Flux[k][j][i-1].d < 0.0)
	  de3_l1 = -x1Flux[k][j][i].By - emf3_cc[k][j][i-1];
	else {
	  de3_l1 = 0.5*(-x1Flux[k][j-1][i].By - emf3_cc[k][j-1][i-1]
			-x1Flux[k][j  ][i].By - emf3_cc[k][j  ][i-1] );
	}

	if (x2Flux[k][j][i].d > 0.0)
	  de3_r1 = -x1Flux[k][j-1][i].By - emf3_cc[k][j-1][i];
	else if (x2Flux[k][j][i].d < 0.0)
	  de3_r1 = -x1Flux[k][j][i].By - emf3_cc[k][j][i];
	else {
	  de3_r1 = 0.5*(-x1Flux[k][j-1][i].By - emf3_cc[k][j-1][i]
			-x1Flux[k][j  ][i].By - emf3_cc[k][j  ][i] );
	}

	emf3[k][j][i] = 0.25*(  x2Flux[k][j  ][i-1].Bz + x2Flux[k][j][i].Bz
			      - x1Flux[k][j-1][i  ].By - x1Flux[k][j][i].By
			      + de3_l1 + de3_r1 + de3_l2 + de3_r2);
      }
    }
  }

  return;
}
#endif /* MHD */