#include "copyright.h"
/*==============================================================================
 * FILE: radMHD1d.c
 *
 * PURPOSE: Problem generator to test the radiation MHD code. 
 *  Development is underway. To be modified later 
 *============================================================================*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "defs.h"
#include "athena.h"
#include "globals.h"
#include "prototypes.h"

/*----------------------------------------------------------------------------*/
/* problem:    */


void problem(DomainS *pDomain)
{
  GridS *pGrid=(pDomain->Grid);
  int i, j, k, iu, il, ju, jl, ku, kl;
  int shift;

/* Parse global variables of unit ratio */


/* Set up the index bounds for initializing the grid */
  iu = pGrid->ie;
  il = pGrid->is;

  if (pGrid->Nx[1] > 1) {
    ju = pGrid->je + nghost;
    jl = pGrid->js - nghost;
  }
  else {
    ju = pGrid->je;
    jl = pGrid->js;
  }

  if (pGrid->Nx[2] > 1) {
    ku = pGrid->ke + nghost;
    kl = pGrid->ks - nghost;
  }
  else {
    ku = pGrid->ke;
    kl = pGrid->ks;
  }

/* Initialize the grid including the ghost cells.  */
	Real d0, u0, x1, x2, x3, t, theta, omega, P0, wavenumber;
	d0 = 1.0;
	u0 = 0.0;
	P0 = 1.0;

	four_pi_G = 300.;
	grav_mean_rho = d0;
	t = pGrid->time;
	Real flag = 1.0;
	Real factor = 300.;

	Real costheta, sintheta;
	sintheta = 0.4472136;
	costheta = 2.0 * sintheta;

	wavenumber = 2.0 * PI;
	if(Gamma * P0 * wavenumber * wavenumber/d0 - four_pi_G * d0 > 0){
	  omega = sqrt(Gamma * P0 * wavenumber * wavenumber/d0 - four_pi_G * d0);
	} else {
	  omega = sqrt(-(Gamma * P0 * wavenumber * wavenumber/d0 - four_pi_G * d0));	  
	}


    for (k=kl; k<=ku; k++) {
      for (j=jl; j<=ju; j++) {
        for (i=il; i<=iu; i++) {

	cc_pos(pGrid, i, j,k, &x1, &x2, &x3);

/* Initialize conserved (and  the primitive) variables in Grid */
	theta = wavenumber * (costheta * x1 + sintheta * x2) - omega * t;	

          pGrid->U[k][j][i].d  = 1.0 + flag * factor * 1.e-3 * cos(theta);
	if(Gamma * P0 * wavenumber * wavenumber/d0 - four_pi_G * d0 > 0){
          pGrid->U[k][j][i].M1 = costheta * flag * factor * omega * 1.0e-3 * cos(theta)/wavenumber;
	} else {
	  pGrid->U[k][j][i].M1 = -costheta * flag * factor * omega * 1.0e-3 * sin(theta)/wavenumber;
	}
          
	 if(Gamma * P0 * wavenumber * wavenumber/d0 - four_pi_G * d0 > 0){
          pGrid->U[k][j][i].M2 = sintheta * flag * factor * omega * 1.0e-3 * cos(theta)/wavenumber;
        } else {
          pGrid->U[k][j][i].M2 = -sintheta * flag * factor * omega * 1.0e-3 * sin(theta)/wavenumber;
        }


          pGrid->U[k][j][i].M3 = 0.0;

          pGrid->U[k][j][i].E = 1.0/(Gamma - 1.0) + flag * factor * Gamma * P0 * 1.0e-3 * cos(theta)/(d0 * (Gamma - 1.0));


#ifdef MHD
          pGrid->B1i[k][j][i] = 0.0;
          pGrid->B2i[k][j][i] = 0.0;
          pGrid->B3i[k][j][i] = 0.0;
          pGrid->U[k][j][i].B1c = 0.0;
          pGrid->U[k][j][i].B2c = 0.0;
          pGrid->U[k][j][i].B3c = 0.0;
#endif

        }
      }
    }
/*
	bvals_mhd_fun(pDomain, left_x1, radMHD_inflow);
	bvals_rad_fun(pDomain, left_x1, radMHD_rad_inflow);
	bvals_mhd_fun(pDomain, right_x1, radMHD_inflow2);
	bvals_rad_fun(pDomain, right_x1, radMHD_rad_inflow2);
*/
  return;
}


/*==============================================================================
 * PROBLEM USER FUNCTIONS:
 * problem_write_restart() - writes problem-specific user data to restart files
 * problem_read_restart()  - reads problem-specific user data from restart files
 * get_usr_expr()          - sets pointer to expression for special output data
 * get_usr_out_fun()       - returns a user defined output function pointer
 * get_usr_par_prop()      - returns a user defined particle selection function
 * Userwork_in_loop        - problem specific work IN     main loop
 * Userwork_after_loop     - problem specific work AFTER  main loop
 *----------------------------------------------------------------------------*/

void problem_write_restart(MeshS *pM, FILE *fp)
{

  return;
}

void problem_read_restart(MeshS *pM, FILE *fp)
{

	

  return;
}

ConsFun_t get_usr_expr(const char *expr)
{
  return NULL;
}

VOutFun_t get_usr_out_fun(const char *name){
  return NULL;
}

void Userwork_in_loop(MeshS *pM)
{
  return;
}

void Userwork_after_loop(MeshS *pM)
{
  return;
}
