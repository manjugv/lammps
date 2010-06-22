/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Paul Crozier (SNL)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair_lj_charmm_coul_charmm_omp.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "memory.h"
#include "error.h"

#if defined(_OPENMP)
#include <omp.h>
#endif

using namespace LAMMPS_NS;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

/* ---------------------------------------------------------------------- */

PairLJCharmmCoulCharmmOMP::PairLJCharmmCoulCharmmOMP(LAMMPS *lmp) : PairOMP(lmp)
{
  implicit = 0;
}

/* ---------------------------------------------------------------------- */

PairLJCharmmCoulCharmmOMP::~PairLJCharmmCoulCharmmOMP()
{
  if (allocated) {
    memory->destroy_2d_int_array(setflag);
    memory->destroy_2d_double_array(cutsq);

    memory->destroy_2d_double_array(epsilon);
    memory->destroy_2d_double_array(sigma);
    memory->destroy_2d_double_array(eps14);
    memory->destroy_2d_double_array(sigma14);
    memory->destroy_2d_double_array(lj1);
    memory->destroy_2d_double_array(lj2);
    memory->destroy_2d_double_array(lj3);
    memory->destroy_2d_double_array(lj4);
    memory->destroy_2d_double_array(lj14_1);
    memory->destroy_2d_double_array(lj14_2);
    memory->destroy_2d_double_array(lj14_3);
    memory->destroy_2d_double_array(lj14_4);
  }
}

/* ---------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::compute(int eflag, int vflag)
{
  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(eflag,vflag);
  } else evflag = vflag_fdotr = 0;

  if (evflag) {
    if (eflag) {
      if (force->newton_pair) return eval<1,1,1>();
      else return eval<1,1,0>();
    } else {
      if (force->newton_pair) return eval<1,0,1>();
      else return eval<1,0,0>();
    }
  } else {
    if (force->newton_pair) return eval<0,0,1>();
    else return eval<0,0,0>();
  }
}

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairLJCharmmCoulCharmmOMP::eval()
{

#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {
    int i,j,ii,jj,inum,jnum,itype,jtype,tid;
    double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,evdwl,ecoul,fpair;
    double rsq,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
    double philj,switch1,switch2;
    int *ilist,*jlist,*numneigh,**firstneigh;

    evdwl = ecoul = 0.0;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    double *q = atom->q;
    int *type = atom->type;
    double *special_coul = force->special_coul;
    double *special_lj = force->special_lj;
    double qqrd2e = force->qqrd2e;

    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;

    // loop over neighbors of my atoms

    int iifrom, iito;
    double **f = loop_setup_thr(atom->f,iifrom,iito,tid,inum,nall,nthreads);
    for (ii = iifrom; ii < iito; ++ii) {
      i = ilist[ii];
      qtmp = q[i];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];
      jlist = firstneigh[i];
      jnum = numneigh[i];

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_coul = factor_lj = 1.0;
	else {
	  factor_coul = special_coul[j/nall];
	  factor_lj = special_lj[j/nall];
	  j %= nall;
	}

	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;

	if (rsq < cut_bothsq) {
	  r2inv = 1.0/rsq;

	  if (rsq < cut_coulsq) {
	    forcecoul = qqrd2e * qtmp*q[j]*sqrt(r2inv);
	    if (rsq > cut_coul_innersq) {
	      switch1 = (cut_coulsq-rsq) * (cut_coulsq-rsq) *
		(cut_coulsq + 2.0*rsq - 3.0*cut_coul_innersq) / denom_coul;
	      switch2 = 12.0*rsq * (cut_coulsq-rsq) *
		(rsq-cut_coul_innersq) / denom_coul;
	      forcecoul *= switch1 + switch2;
	    }
	  } else forcecoul = 0.0;

	  if (rsq < cut_ljsq) {
	    r6inv = r2inv*r2inv*r2inv;
	    jtype = type[j];
	    forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
	    if (rsq > cut_lj_innersq) {
	      switch1 = (cut_ljsq-rsq) * (cut_ljsq-rsq) *
		(cut_ljsq + 2.0*rsq - 3.0*cut_lj_innersq) / denom_lj;
	      switch2 = 12.0*rsq * (cut_ljsq-rsq) *
		(rsq-cut_lj_innersq) / denom_lj;
	      philj = r6inv * (lj3[itype][jtype]*r6inv - lj4[itype][jtype]);
	      forcelj = forcelj*switch1 + philj*switch2;
	    }
	  } else forcelj = 0.0;

	  fpair = (factor_coul*forcecoul + factor_lj*forcelj) * r2inv;

	  f[i][0] += delx*fpair;
	  f[i][1] += dely*fpair;
	  f[i][2] += delz*fpair;
	  if (NEWTON_PAIR || j < nlocal) {
	    f[j][0] -= delx*fpair;
	    f[j][1] -= dely*fpair;
	    f[j][2] -= delz*fpair;
	  }

	  if (EFLAG) {
	    if (rsq < cut_coulsq) {
	      ecoul = qqrd2e * qtmp*q[j]*sqrt(r2inv);
	      if (rsq > cut_coul_innersq) {
		switch1 = (cut_coulsq-rsq) * (cut_coulsq-rsq) *
		  (cut_coulsq + 2.0*rsq - 3.0*cut_coul_innersq) /
		  denom_coul;
		ecoul *= switch1;
	      }
	      ecoul *= factor_coul;
	    } else ecoul = 0.0;
	    if (rsq < cut_ljsq) {
	      evdwl = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]);
	      if (rsq > cut_lj_innersq) {
		switch1 = (cut_ljsq-rsq) * (cut_ljsq-rsq) *
		  (cut_ljsq + 2.0*rsq - 3.0*cut_lj_innersq) / denom_lj;
		evdwl *= switch1;
	      }
	      evdwl *= factor_lj;
	    } else evdwl = 0.0;
	  }

	  if (EVFLAG) ev_tally_thr(i,j,nlocal,NEWTON_PAIR,
				   evdwl,ecoul,fpair,delx,dely,delz,tid);
	}
      }
    }
    // reduce per thread forces and torques into global force/torque arrays.
    force_reduce_thr(atom->f, nall, nthreads, tid);
  }
  if (EVFLAG) ev_reduce_thr();
  if (vflag_fdotr) virial_compute();
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  setflag = memory->create_2d_int_array(n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  cutsq = memory->create_2d_double_array(n+1,n+1,"pair:cutsq");

  epsilon = memory->create_2d_double_array(n+1,n+1,"pair:epsilon");
  sigma = memory->create_2d_double_array(n+1,n+1,"pair:sigma");
  eps14 = memory->create_2d_double_array(n+1,n+1,"pair:eps14");
  sigma14 = memory->create_2d_double_array(n+1,n+1,"pair:sigma14");
  lj1 = memory->create_2d_double_array(n+1,n+1,"pair:lj1");
  lj2 = memory->create_2d_double_array(n+1,n+1,"pair:lj2");
  lj3 = memory->create_2d_double_array(n+1,n+1,"pair:lj3");
  lj4 = memory->create_2d_double_array(n+1,n+1,"pair:lj4");
  lj14_1 = memory->create_2d_double_array(n+1,n+1,"pair:lj14_1");
  lj14_2 = memory->create_2d_double_array(n+1,n+1,"pair:lj14_2");
  lj14_3 = memory->create_2d_double_array(n+1,n+1,"pair:lj14_3");
  lj14_4 = memory->create_2d_double_array(n+1,n+1,"pair:lj14_4");
}

/* ----------------------------------------------------------------------
   global settings
   unlike other pair styles,
     there are no individual pair settings that these override
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::settings(int narg, char **arg)
{
  if (narg != 2 && narg != 4) 
    error->all("Illegal pair_style command");

  cut_lj_inner = force->numeric(arg[0]);
  cut_lj = force->numeric(arg[1]);
  if (narg == 2) {
    cut_coul_inner = cut_lj_inner;
    cut_coul = cut_lj;
  } else {
    cut_coul_inner = force->numeric(arg[2]);
    cut_coul = force->numeric(arg[3]);
  }
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::coeff(int narg, char **arg)
{
  if (narg != 4 && narg != 6) 
    error->all("Incorrect args for pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  force->bounds(arg[0],atom->ntypes,ilo,ihi);
  force->bounds(arg[1],atom->ntypes,jlo,jhi);

  double epsilon_one = force->numeric(arg[2]);
  double sigma_one = force->numeric(arg[3]);
  double eps14_one = epsilon_one;
  double sigma14_one = sigma_one;
  if (narg == 6) {
    eps14_one = force->numeric(arg[4]);
    sigma14_one = force->numeric(arg[5]);
  }

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      epsilon[i][j] = epsilon_one;
      sigma[i][j] = sigma_one;
      eps14[i][j] = eps14_one;
      sigma14[i][j] = sigma14_one;
      setflag[i][j] = 1;
      count++;
    }
  }

  if (count == 0) error->all("Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::init_style()
{
  if (!atom->q_flag)
    error->all("Pair style lj/charmm/coul/charmm requires atom attribute q");

  int irequest = neighbor->request(this);

  // require cut_lj_inner < cut_lj, cut_coul_inner < cut_coul

  if (cut_lj_inner >= cut_lj || cut_coul_inner >= cut_coul)
    error->all("Pair inner cutoff >= Pair outer cutoff");

  cut_lj_innersq = cut_lj_inner * cut_lj_inner;
  cut_ljsq = cut_lj * cut_lj;
  cut_coul_innersq = cut_coul_inner * cut_coul_inner;
  cut_coulsq = cut_coul * cut_coul;
  cut_bothsq = MAX(cut_ljsq,cut_coulsq);

  denom_lj = (cut_ljsq-cut_lj_innersq) * (cut_ljsq-cut_lj_innersq) * 
    (cut_ljsq-cut_lj_innersq);
  denom_coul = (cut_coulsq-cut_coul_innersq) * (cut_coulsq-cut_coul_innersq) * 
    (cut_coulsq-cut_coul_innersq);
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairLJCharmmCoulCharmmOMP::init_one(int i, int j)
{
  // always mix arithmetically

  if (setflag[i][j] == 0) {
    epsilon[i][j] = sqrt(epsilon[i][i]*epsilon[j][j]);
    sigma[i][j] = 0.5 * (sigma[i][i] + sigma[j][j]);
    eps14[i][j] = sqrt(eps14[i][i]*eps14[j][j]);
    sigma14[i][j] = 0.5 * (sigma14[i][i] + sigma14[j][j]);
  }

  double cut = MAX(cut_lj,cut_coul);

  lj1[i][j] = 48.0 * epsilon[i][j] * pow(sigma[i][j],12.0);
  lj2[i][j] = 24.0 * epsilon[i][j] * pow(sigma[i][j],6.0);
  lj3[i][j] = 4.0 * epsilon[i][j] * pow(sigma[i][j],12.0);
  lj4[i][j] = 4.0 * epsilon[i][j] * pow(sigma[i][j],6.0);
  lj14_1[i][j] = 48.0 * eps14[i][j] * pow(sigma14[i][j],12.0);
  lj14_2[i][j] = 24.0 * eps14[i][j] * pow(sigma14[i][j],6.0);
  lj14_3[i][j] = 4.0 * eps14[i][j] * pow(sigma14[i][j],12.0);
  lj14_4[i][j] = 4.0 * eps14[i][j] * pow(sigma14[i][j],6.0);
     
  lj1[j][i] = lj1[i][j];
  lj2[j][i] = lj2[i][j];
  lj3[j][i] = lj3[i][j];
  lj4[j][i] = lj4[i][j];
  lj14_1[j][i] = lj14_1[i][j];
  lj14_2[j][i] = lj14_2[i][j];
  lj14_3[j][i] = lj14_3[i][j];
  lj14_4[j][i] = lj14_4[i][j];

  return cut;
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::write_restart(FILE *fp)
{
  write_restart_settings(fp);

  int i,j;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
	fwrite(&epsilon[i][j],sizeof(double),1,fp);
	fwrite(&sigma[i][j],sizeof(double),1,fp);
	fwrite(&eps14[i][j],sizeof(double),1,fp);
	fwrite(&sigma14[i][j],sizeof(double),1,fp);
      }
    }
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
	if (me == 0) {
	  fread(&epsilon[i][j],sizeof(double),1,fp);
	  fread(&sigma[i][j],sizeof(double),1,fp);
	  fread(&eps14[i][j],sizeof(double),1,fp);
	  fread(&sigma14[i][j],sizeof(double),1,fp);
	}
	MPI_Bcast(&epsilon[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&sigma[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&eps14[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&sigma14[i][j],1,MPI_DOUBLE,0,world);
      }
    }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::write_restart_settings(FILE *fp)
{
  fwrite(&cut_lj_inner,sizeof(double),1,fp);
  fwrite(&cut_lj,sizeof(double),1,fp);
  fwrite(&cut_coul_inner,sizeof(double),1,fp);
  fwrite(&cut_coul,sizeof(double),1,fp);
  fwrite(&offset_flag,sizeof(int),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLJCharmmCoulCharmmOMP::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_lj_inner,sizeof(double),1,fp);
    fread(&cut_lj,sizeof(double),1,fp);
    fread(&cut_coul_inner,sizeof(double),1,fp);
    fread(&cut_coul,sizeof(double),1,fp);
    fread(&offset_flag,sizeof(int),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_lj_inner,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&cut_lj,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&cut_coul_inner,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&cut_coul,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&offset_flag,1,MPI_INT,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);
}

/* ---------------------------------------------------------------------- */

double PairLJCharmmCoulCharmmOMP::single(int i, int j, int itype, int jtype,
				      double rsq,
				      double factor_coul, double factor_lj,
				      double &fforce)
{
  double r2inv,r6inv,forcecoul,forcelj,phicoul,philj;
  double switch1,switch2;

  r2inv = 1.0/rsq;
  if (rsq < cut_coulsq) {
    forcecoul = force->qqrd2e * atom->q[i]*atom->q[j]*sqrt(r2inv);
    if (rsq > cut_coul_innersq) {
      switch1 = (cut_coulsq-rsq) * (cut_coulsq-rsq) *
	(cut_coulsq + 2.0*rsq - 3.0*cut_coul_innersq) / denom_coul;
      switch2 = 12.0*rsq * (cut_coulsq-rsq) * 
	(rsq-cut_coul_innersq) / denom_coul;
      forcecoul *= switch1 + switch2;
    }
  } else forcecoul = 0.0;

  if (rsq < cut_ljsq) {
    r6inv = r2inv*r2inv*r2inv;
    forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
    if (rsq > cut_lj_innersq) {
      switch1 = (cut_ljsq-rsq) * (cut_ljsq-rsq) *
	(cut_ljsq + 2.0*rsq - 3.0*cut_lj_innersq) / denom_lj;
      switch2 = 12.0*rsq * (cut_ljsq-rsq) * 
	(rsq-cut_lj_innersq) / denom_lj;
      philj = r6inv * (lj3[itype][jtype]*r6inv - lj4[itype][jtype]);
      forcelj = forcelj*switch1 + philj*switch2;
    }
  } else forcelj = 0.0;

  fforce = (factor_coul*forcecoul + factor_lj*forcelj) * r2inv;

  double eng = 0.0;
  if (rsq < cut_coulsq) {
    phicoul = force->qqrd2e * atom->q[i]*atom->q[j]*sqrt(r2inv);
    if (rsq > cut_coul_innersq) {
      switch1 = (cut_coulsq-rsq) * (cut_coulsq-rsq) *
	(cut_coulsq + 2.0*rsq - 3.0*cut_coul_innersq) / 
	denom_coul;
      phicoul *= switch1;
    }
    eng += factor_coul*phicoul;
  }
  if (rsq < cut_ljsq) {
    philj = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]);
    if (rsq > cut_lj_innersq) {
      switch1 = (cut_ljsq-rsq) * (cut_ljsq-rsq) *
	(cut_ljsq + 2.0*rsq - 3.0*cut_lj_innersq) / denom_lj;
      philj *= switch1;
    }
    eng += factor_lj*philj;
  }

  return eng;
}

/* ---------------------------------------------------------------------- */

void *PairLJCharmmCoulCharmmOMP::extract(char *str)
{
  if (strcmp(str,"lj14_1") == 0) return (void *) lj14_1;
  else if (strcmp(str,"lj14_2") == 0) return (void *) lj14_2;
  else if (strcmp(str,"lj14_3") == 0) return (void *) lj14_3;
  else if (strcmp(str,"lj14_4") == 0) return (void *) lj14_4;
  else if (strcmp(str,"implicit") == 0) return (void *) &implicit;
  return NULL;
}

/* ---------------------------------------------------------------------- */

double PairLJCharmmCoulCharmmOMP::memory_usage()
{
  const int n=atom->ntypes;

  double bytes = PairOMP::memory_usage();

  bytes += 9*((n+1)*(n+1) * sizeof(double) + (n+1)*sizeof(double *));
  bytes += 1*((n+1)*(n+1) * sizeof(int) + (n+1)*sizeof(int *));

  return bytes;
}
