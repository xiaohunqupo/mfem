// Copyright (c) 2010-2025, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "nurbs.hpp"

#include "point.hpp"
#include "segment.hpp"
#include "quadrilateral.hpp"
#include "hexahedron.hpp"
#include "../fem/gridfunc.hpp"
#include "../general/text.hpp"

#include <fstream>
#include <algorithm>
#if defined(_MSC_VER) && (_MSC_VER < 1800)
#include <float.h>
#define copysign _copysign
#endif

namespace mfem
{

using namespace std;

const int KnotVector::MaxOrder = 10;

KnotVector::KnotVector(istream &input)
{
   input >> Order >> NumOfControlPoints;

   knot.Load(input, NumOfControlPoints + Order + 1);
   GetElements();
   coarse = false;
}

KnotVector::KnotVector(int order, int NCP)
{
   Order = order;
   NumOfControlPoints = NCP;
   knot.SetSize(NumOfControlPoints + Order + 1);
   NumOfElements = 0;
   coarse = false;

   knot = -1.;
}

KnotVector::KnotVector(int order, const Vector& intervals,
                       const Array<int>& continuity)
{
   // NOTE: This may need to be generalized to support periodicity
   // in the future.
   MFEM_ASSERT(continuity.Size() == (intervals.Size() + 1),
               "Incompatible sizes of continuity and intervals.");
   Order = order;
   const int num_knots = Order * continuity.Size() - continuity.Sum();
   // Some continuities may still be invalid; this assert only avoids
   // passing a negative num_knots to Vector::SetSize().
   MFEM_ASSERT(num_knots >= 0, "Invalid continuity vector for order.");
   NumOfControlPoints = num_knots - Order - 1;
   knot.SetSize(num_knots);
   real_t accum = 0.0;
   int iknot = 0;
   for (int i = 0; i < continuity.Size(); ++i)
   {
      const int multiplicity = Order - continuity[i];
      MFEM_ASSERT(multiplicity >= 1 && multiplicity <= Order+1,
                  "Invalid knot multiplicity for order.");
      for (int j = 0; j < multiplicity; ++j)
      {
         knot[iknot] = accum;
         ++iknot;
      }
      if (i < intervals.Size()) { accum += intervals[i]; }
   }
   // Assert that there are enough knots to provide a complete basis over all
   // the elements in the knot vector.
   MFEM_ASSERT(knot.Size() >= (2*(Order+1)),
               "Insufficient number of knots to define NURBS.");
   // Calculate the number of elements provided by the knot vector
   NumOfElements = 0;
   for (int i = 0; i < GetNKS(); ++i)
   {
      if (isElement(i))
      {
         ++NumOfElements;
      }
   }
   coarse = false;
}

KnotVector &KnotVector::operator=(const KnotVector &kv)
{
   Order = kv.Order;
   NumOfControlPoints = kv.NumOfControlPoints;
   NumOfElements = kv.NumOfElements;
   knot = kv.knot;
   coarse = kv.coarse;
   if (kv.spacing) { spacing = kv.spacing->Clone(); }

   return *this;
}

KnotVector *KnotVector::DegreeElevate(int t) const
{
   if (t < 0)
   {
      mfem_error("KnotVector::DegreeElevate :\n"
                 " Parent KnotVector order higher than child");
   }

   const int nOrder = Order + t;
   KnotVector *newkv = new KnotVector(nOrder, GetNCP() + t);

   for (int i = 0; i <= nOrder; i++)
   {
      (*newkv)[i] = knot(0);
   }
   for (int i = nOrder + 1; i < newkv->GetNCP(); i++)
   {
      (*newkv)[i] = knot(i - t);
   }
   for (int i = 0; i <= nOrder; i++)
   {
      (*newkv)[newkv->GetNCP() + i] = knot(knot.Size()-1);
   }

   newkv->GetElements();

   return newkv;
}

void KnotVector::UniformRefinement(Vector &newknots, int rf) const
{
   MFEM_VERIFY(rf > 1, "Refinement factor must be at least 2.");

   const real_t h = 1.0 / ((real_t) rf);

   newknots.SetSize(NumOfElements * (rf - 1));
   int j = 0;
   for (int i = 0; i < knot.Size()-1; i++)
   {
      if (knot(i) != knot(i+1))
      {
         for (int m = 1; m < rf; ++m)
         {
            newknots(j) = ((1.0 - (m * h)) * knot(i)) + (m * h * knot(i+1));
            j++;
         }
      }
   }
}

int KnotVector::GetCoarseningFactor() const
{
   if (spacing)
   {
      if (spacing->Nested())
      {
         return 1;
      }
      else
      {
         return spacing->Size();   // Coarsen only if non-nested
      }
   }
   else
   {
      return 1;
   }
}

Vector KnotVector::GetFineKnots(const int cf) const
{
   Vector fine;
   if (cf < 2) { return fine; }

   const int cne = NumOfElements / cf;  // Coarse number of elements
   MFEM_VERIFY(cne > 0 && cne * cf == NumOfElements, "Invalid coarsening factor");

   fine.SetSize(cne * (cf - 1));

   int fcnt = 0;
   int i = Order;
   real_t kprev = knot(Order);
   for (int c=0; c<cne; ++c)  // Loop over coarse elements
   {
      int cnt = 0;
      while (cnt < cf)
      {
         i++;
         if (knot(i) != kprev)
         {
            kprev = knot(i);
            cnt++;
            if (cnt < cf)
            {
               fine[fcnt] = knot(i);
               fcnt++;
            }
         }
      }
   }

   MFEM_VERIFY(fcnt == fine.Size(), "");

   return fine;
}

void KnotVector::Refinement(Vector &newknots, int rf) const
{
   MFEM_VERIFY(rf > 1, "Refinement factor must be at least 2.");

   if (spacing)
   {
      spacing->ScaleParameters(1.0 / ((real_t) rf));
      spacing->SetSize(rf * NumOfElements);
      Vector s;
      spacing->EvalAll(s);

      newknots.SetSize((rf - 1) * NumOfElements);

      const real_t k0 = knot(0);
      const real_t k1 = knot(knot.Size()-1);

      Array<int> span0(NumOfElements + 1);
      span0[0] = 0;

      int j = 1;
      for (int i = 0; i < knot.Size()-1; i++)
      {
         if (knot(i) != knot(i+1))
         {
            span0[j] = i+1;
            j++;
         }
      }

      MFEM_VERIFY(j == NumOfElements + 1, "bug");

      real_t s0 = 0.0;

      for (int i=0; i<NumOfElements; ++i)
      {
         // Note that existing coarse knots are not modified here according to
         // the spacing formula, because modifying them will not produce a
         // correctly spaced mesh without also updating control points. Here, we
         // only define new knots according to the spacing formula. Non-nested
         // refinement should be done by using a single element per patch and
         // a sufficiently large refinement factor to produce the desired mesh
         // with only one refinement.

         s0 += s[rf*i];

         for (j=0; j<rf-1; ++j)
         {
            // Define a new knot between the modified coarse knots
            newknots(((rf - 1) * i) + j) = ((1.0 - s0) * k0) + (s0 * k1);

            s0 += s[(rf*i) + j + 1];
         }
      }
   }
   else
   {
      UniformRefinement(newknots, rf);
   }
}

void KnotVector::GetElements()
{
   NumOfElements = 0;
   for (int i = Order; i < NumOfControlPoints; i++)
   {
      if (knot(i) != knot(i+1))
      {
         NumOfElements++;
      }
   }
}

void KnotVector::Flip()
{
   real_t apb = knot(0) + knot(knot.Size()-1);

   int ns = (NumOfControlPoints - Order)/2;
   for (int i = 1; i <= ns; i++)
   {
      real_t tmp = apb - knot(Order + i);
      knot(Order + i) = apb - knot(NumOfControlPoints - i);
      knot(NumOfControlPoints - i) = tmp;
   }
}

void KnotVector::Print(std::ostream &os) const
{
   os << Order << ' ' << NumOfControlPoints << ' ';
   knot.Print(os, knot.Size());
}

void KnotVector::PrintFunctions(std::ostream &os, int samples) const
{
   MFEM_VERIFY(GetNE(), "Elements not counted. Use GetElements().");

   Vector shape(Order+1);

   real_t x, dx = 1.0/real_t (samples - 1);

   /* @a cnt is a counter including elements between repeated knots if
      present. This is required for usage of CalcShape. */
   int cnt = 0;

   for (int e = 0; e < GetNE(); e++, cnt++)
   {
      // Avoid printing shapes between repeated knots
      if (!isElement(cnt)) { e--; continue; }

      for (int j = 0; j <samples; j++)
      {
         x = j*dx;
         os << x + e;

         CalcShape(shape, cnt, x);
         for (int d = 0; d < Order+1; d++) { os<<"\t"<<shape[d]; }

         CalcDShape(shape, cnt, x);
         for (int d = 0; d < Order+1; d++) { os<<"\t"<<shape[d]; }

         CalcD2Shape(shape, cnt, x);
         for (int d = 0; d < Order+1; d++) { os<<"\t"<<shape[d]; }
         os << endl;
      }
   }
}

// Routine from "The NURBS Book" - 2nd ed - Piegl and Tiller
// Algorithm A2.2 p. 70
void KnotVector::CalcShape(Vector &shape, int i, real_t xi) const
{
   MFEM_ASSERT(Order <= MaxOrder, "Order > MaxOrder!");

   int    p = Order;
   int    ip = (i >= 0) ? (i + p) : (-1 - i + p);
   real_t u = getKnotLocation((i >= 0) ? xi : 1. - xi, ip), saved, tmp;
   real_t left[MaxOrder+1], right[MaxOrder+1];

   shape(0) = 1.;
   for (int j = 1; j <= p; ++j)
   {
      left[j]  = u - knot(ip+1-j);
      right[j] = knot(ip+j) - u;
      saved    = 0.;
      for (int r = 0; r < j; ++r)
      {
         tmp      = shape(r)/(right[r+1] + left[j-r]);
         shape(r) = saved + right[r+1]*tmp;
         saved    = left[j-r]*tmp;
      }
      shape(j) = saved;
   }
}

// Routine from "The NURBS Book" - 2nd ed - Piegl and Tiller
// Algorithm A2.3 p. 72
void KnotVector::CalcDShape(Vector &grad, int i, real_t xi) const
{
   int    p = Order, rk, pk;
   int    ip = (i >= 0) ? (i + p) : (-1 - i + p);
   real_t u = getKnotLocation((i >= 0) ? xi : 1. - xi, ip), temp, saved, d;
   real_t ndu[MaxOrder+1][MaxOrder+1], left[MaxOrder+1], right[MaxOrder+1];

#ifdef MFEM_DEBUG
   if (p > MaxOrder)
   {
      mfem_error("KnotVector::CalcDShape : Order > MaxOrder!");
   }
#endif

   ndu[0][0] = 1.0;
   for (int j = 1; j <= p; j++)
   {
      left[j] = u - knot(ip-j+1);
      right[j] = knot(ip+j) - u;
      saved = 0.0;
      for (int r = 0; r < j; r++)
      {
         ndu[j][r] = right[r+1] + left[j-r];
         temp = ndu[r][j-1]/ndu[j][r];
         ndu[r][j] = saved + right[r+1]*temp;
         saved = left[j-r]*temp;
      }
      ndu[j][j] = saved;
   }

   for (int r = 0; r <= p; ++r)
   {
      d = 0.0;
      rk = r-1;
      pk = p-1;
      if (r >= 1)
      {
         d = ndu[rk][pk]/ndu[p][rk];
      }
      if (r <= pk)
      {
         d -= ndu[r][pk]/ndu[p][r];
      }
      grad(r) = d;
   }

   if (i >= 0)
   {
      grad *= p*(knot(ip+1) - knot(ip));
   }
   else
   {
      grad *= p*(knot(ip) - knot(ip+1));
   }
}

// Routine from "The NURBS Book" - 2nd ed - Piegl and Tiller
// Algorithm A2.3 p. 72
void KnotVector::CalcDnShape(Vector &gradn, int n, int i, real_t xi) const
{
   int    p = Order, rk, pk, j1, j2,r,j,k;
   int    ip = (i >= 0) ? (i + p) : (-1 - i + p);
   real_t u = getKnotLocation((i >= 0) ? xi : 1. - xi, ip);
   real_t temp, saved, d;
   real_t a[2][MaxOrder+1],ndu[MaxOrder+1][MaxOrder+1], left[MaxOrder+1],
          right[MaxOrder+1];

#ifdef MFEM_DEBUG
   if (p > MaxOrder)
   {
      mfem_error("KnotVector::CalcDnShape : Order > MaxOrder!");
   }
#endif

   ndu[0][0] = 1.0;
   for (j = 1; j <= p; j++)
   {
      left[j] = u - knot(ip-j+1);
      right[j] = knot(ip+j)- u;

      saved = 0.0;
      for (r = 0; r < j; r++)
      {
         ndu[j][r] = right[r+1] + left[j-r];
         temp = ndu[r][j-1]/ndu[j][r];
         ndu[r][j] = saved + right[r+1]*temp;
         saved = left[j-r]*temp;
      }
      ndu[j][j] = saved;
   }

   for (r = 0; r <= p; r++)
   {
      int s1 = 0;
      int s2 = 1;
      a[0][0] = 1.0;
      for (k = 1; k <= n; k++)
      {
         d = 0.0;
         rk = r-k;
         pk = p-k;
         if (r >= k)
         {
            a[s2][0] = a[s1][0]/ndu[pk+1][rk];
            d = a[s2][0]*ndu[rk][pk];
         }

         if (rk >= -1)
         {
            j1 = 1;
         }
         else
         {
            j1 = -rk;
         }

         if (r-1<= pk)
         {
            j2 = k-1;
         }
         else
         {
            j2 = p-r;
         }

         for (j = j1; j <= j2; j++)
         {
            a[s2][j] = (a[s1][j] - a[s1][j-1])/ndu[pk+1][rk+j];
            d += a[s2][j]*ndu[rk+j][pk];
         }

         if (r <= pk)
         {
            a[s2][k] = - a[s1][k-1]/ndu[pk+1][r];
            d += a[s2][j]*ndu[rk+j][pk];
         }
         gradn[r] = d;
         j = s1;
         s1 = s2;
         s2 = j;
      }
   }

   if (i >= 0)
   {
      u = (knot(ip+1) - knot(ip));
   }
   else
   {
      u = (knot(ip) - knot(ip+1));
   }

   temp = p*u;
   for (k = 1; k <= n-1; k++) { temp *= (p-k)*u; }

   for (j = 0; j <= p; j++) { gradn[j] *= temp; }

}

void KnotVector::FindMaxima(Array<int> &ks, Vector &xi, Vector &u) const
{
   Vector shape(Order+1);
   Vector maxima(GetNCP());
   real_t arg1, arg2, arg, max1, max2, max;

   xi.SetSize(GetNCP());
   u.SetSize(GetNCP());
   ks.SetSize(GetNCP());
   for (int j = 0; j <GetNCP(); j++)
   {
      maxima[j] = 0;
      for (int d = 0; d < Order+1; d++)
      {
         int i = j - d;
         if (isElement(i))
         {
            arg1 = std::numeric_limits<real_t>::epsilon() / 2_r;
            CalcShape(shape, i, arg1);
            max1 = shape[d];

            arg2 = 1_r - arg1;
            CalcShape(shape, i, arg2);
            max2 = shape[d];

            arg = (arg1 + arg2)/2;
            CalcShape(shape, i, arg);
            max = shape[d];

            while ( ( max > max1 ) || (max > max2) )
            {
               if (max1 < max2)
               {
                  max1 = max;
                  arg1 = arg;
               }
               else
               {
                  max2 = max;
                  arg2 = arg;
               }

               arg = (arg1 + arg2)/2;
               CalcShape(shape, i, arg);
               max = shape[d];
            }

            if (max > maxima[j])
            {
               maxima[j] = max;
               ks[j] = i;
               xi[j] = arg;
               u[j]  = getKnotLocation(arg, i+Order);
            }
         }
      }
   }
}

// Routine from "The NURBS Book" - 2nd ed - Piegl and Tiller
// Algorithm A9.1 p. 369
void KnotVector::FindInterpolant(Array<Vector*> &x, bool reuse_inverse)
{
   int order = GetOrder();
   int ncp = GetNCP();

   // Find interpolation points
   Vector xi_args, u_args;
   Array<int> i_args;
   FindMaxima(i_args, xi_args, u_args);

   // Assemble collocation matrix
#ifdef MFEM_USE_LAPACK
   // If using LAPACK, we use banded matrix storage (order + 1 nonzeros per row).
   // Find banded structure of matrix.
   int KL = 0; // Number of subdiagonals
   int KU = 0; // Number of superdiagonals
   for (int i = 0; i < ncp; i++)
   {
      for (int p = 0; p < order+1; p++)
      {
         const int col = i_args[i] + p;
         if (col < i)
         {
            KL = std::max(KL, i - col);
         }
         else if (i < col)
         {
            KU = std::max(KU, col - i);
         }
      }
   }

   const int LDAB = (2*KL) + KU + 1;
   const int N = ncp;

   fact_AB.SetSize(LDAB, N);
#else
   // Without LAPACK, we store and invert a DenseMatrix (inefficient).
   if (!reuse_inverse)
   {
      A_coll_inv.SetSize(ncp, ncp);
      A_coll_inv = 0.0;
   }
#endif

   Vector shape(order+1);

   if (!reuse_inverse) // Set collocation matrix entries
   {
      for (int i = 0; i < ncp; i++)
      {
         CalcShape(shape, i_args[i], xi_args[i]);
         for (int p = 0; p < order+1; p++)
         {
            const int j = i_args[i] + p;
#ifdef MFEM_USE_LAPACK
            fact_AB(KL+KU+i-j,j) = shape[p];
#else
            A_coll_inv(i,j) = shape[p];
#endif
         }
      }
   }

   // Solve the system
#ifdef MFEM_USE_LAPACK
   const int NRHS = x.Size();
   DenseMatrix B(N, NRHS);
   for (int j=0; j<NRHS; ++j)
   {
      for (int i=0; i<N; ++i) { B(i, j) = (*x[j])[i]; }
   }

   if (reuse_inverse)
   {
      BandedFactorizedSolve(KL, KU, fact_AB, B, false, fact_ipiv);
   }
   else
   {
      BandedSolve(KL, KU, fact_AB, B, fact_ipiv);
   }

   for (int j=0; j<NRHS; ++j)
   {
      for (int i=0; i<N; ++i) { (*x[j])[i] = B(i, j); }
   }
#else
   if (!reuse_inverse) { A_coll_inv.Invert(); }
   Vector tmp;
   for (int i = 0; i < x.Size(); i++)
   {
      tmp = *x[i];
      A_coll_inv.Mult(tmp, *x[i]);
   }
#endif
}

int KnotVector::findKnotSpan(real_t u) const
{
   int low, mid, high;

   if (u == knot(NumOfControlPoints+Order))
   {
      mid = NumOfControlPoints;
   }
   else
   {
      low = Order;
      high = NumOfControlPoints + 1;
      mid = (low + high)/2;
      while ( (u < knot(mid-1)) || (u > knot(mid)) )
      {
         if (u < knot(mid-1))
         {
            high = mid;
         }
         else
         {
            low = mid;
         }
         mid = (low + high)/2;
      }
   }
   return mid;
}

void KnotVector::Difference(const KnotVector &kv, Vector &diff) const
{
   if (Order != kv.GetOrder())
   {
      mfem_error("KnotVector::Difference :\n"
                 " Can not compare knot vectors with different orders!");
   }

   int s = kv.Size() - Size();
   if (s < 0)
   {
      kv.Difference(*this, diff);
      return;
   }

   diff.SetSize(s);

   if (s == 0) { return; }

   s = 0;
   int i = 0;
   for (int j = 0; j < kv.Size(); j++)
   {
      if (abs(knot(i) - kv[j]) < 2 * std::numeric_limits<real_t>::epsilon())
      {
         i++;
      }
      else
      {
         diff(s) = kv[j];
         s++;
      }
   }
}

void NURBSPatch::init(int dim)
{
   MFEM_ASSERT(dim > 1, "NURBS patch dimension (including weight) must be "
               "greater than 1.");
   Dim = dim;
   sd = nd = -1;

   if (kv.Size() == 1)
   {
      ni = kv[0]->GetNCP();
      MFEM_ASSERT(ni > 0, "Invalid knot vector dimension.");
      nj = -1;
      nk = -1;

      data = new real_t[ni*Dim];

#ifdef MFEM_DEBUG
      for (int i = 0; i < ni*Dim; i++)
      {
         data[i] = -999.99;
      }
#endif
   }
   else if (kv.Size() == 2)
   {
      ni = kv[0]->GetNCP();
      nj = kv[1]->GetNCP();
      MFEM_ASSERT(ni > 0 && nj > 0, "Invalid knot vector dimensions.");
      nk = -1;

      data = new real_t[ni*nj*Dim];

#ifdef MFEM_DEBUG
      for (int i = 0; i < ni*nj*Dim; i++)
      {
         data[i] = -999.99;
      }
#endif
   }
   else if (kv.Size() == 3)
   {
      ni = kv[0]->GetNCP();
      nj = kv[1]->GetNCP();
      nk = kv[2]->GetNCP();
      MFEM_ASSERT(ni > 0 && nj > 0 && nk > 0,
                  "Invalid knot vector dimensions.");

      data = new real_t[ni*nj*nk*Dim];

#ifdef MFEM_DEBUG
      for (int i = 0; i < ni*nj*nk*Dim; i++)
      {
         data[i] = -999.99;
      }
#endif
   }
   else
   {
      mfem_error("NURBSPatch::init : Wrong dimension of knotvectors!");
   }
}

NURBSPatch::NURBSPatch(const NURBSPatch &orig)
   : ni(orig.ni), nj(orig.nj), nk(orig.nk), Dim(orig.Dim),
     data(NULL), kv(orig.kv.Size()), nd(orig.nd), ls(orig.ls), sd(orig.sd)
{
   // Allocate and copy data:
   const int data_size = Dim*ni*nj*((kv.Size() == 2) ? 1 : nk);
   data = new real_t[data_size];
   std::memcpy(data, orig.data, data_size*sizeof(real_t));

   // Copy the knot vectors:
   for (int i = 0; i < kv.Size(); i++)
   {
      kv[i] = new KnotVector(*orig.kv[i]);
   }
}

NURBSPatch::NURBSPatch(std::istream &input)
{
   int pdim, dim, size = 1;
   string ident;

   input >> ws >> ident >> pdim; // knotvectors
   kv.SetSize(pdim);
   for (int i = 0; i < pdim; i++)
   {
      kv[i] = new KnotVector(input);
      size *= kv[i]->GetNCP();
   }

   input >> ws >> ident >> dim; // dimension
   init(dim + 1);

   input >> ws >> ident; // controlpoints (homogeneous coordinates)
   if (ident == "controlpoints" || ident == "controlpoints_homogeneous")
   {
      for (int j = 0, i = 0; i < size; i++)
         for (int d = 0; d <= dim; d++, j++)
         {
            input >> data[j];
         }
   }
   else // "controlpoints_cartesian" (Cartesian coordinates with weight)
   {
      for (int j = 0, i = 0; i < size; i++)
      {
         for (int d = 0; d <= dim; d++)
         {
            input >> data[j+d];
         }
         for (int d = 0; d < dim; d++)
         {
            data[j+d] *= data[j+dim];
         }
         j += (dim+1);
      }
   }
}

NURBSPatch::NURBSPatch(const KnotVector *kv0, const KnotVector *kv1, int dim)
{
   kv.SetSize(2);
   kv[0] = new KnotVector(*kv0);
   kv[1] = new KnotVector(*kv1);
   init(dim);
}

NURBSPatch::NURBSPatch(const KnotVector *kv0, const KnotVector *kv1,
                       const KnotVector *kv2, int dim)
{
   kv.SetSize(3);
   kv[0] = new KnotVector(*kv0);
   kv[1] = new KnotVector(*kv1);
   kv[2] = new KnotVector(*kv2);
   init(dim);
}

NURBSPatch::NURBSPatch(Array<const KnotVector *> &kvs, int dim)
{
   kv.SetSize(kvs.Size());
   for (int i = 0; i < kv.Size(); i++)
   {
      kv[i] = new KnotVector(*kvs[i]);
   }
   init(dim);
}

NURBSPatch::NURBSPatch(NURBSPatch *parent, int dir, int Order, int NCP)
{
   kv.SetSize(parent->kv.Size());
   for (int i = 0; i < kv.Size(); i++)
      if (i != dir)
      {
         kv[i] = new KnotVector(*parent->kv[i]);
      }
      else
      {
         kv[i] = new KnotVector(Order, NCP);
      }
   init(parent->Dim);
}

void NURBSPatch::swap(NURBSPatch *np)
{
   if (data != NULL)
   {
      delete [] data;
   }

   for (int i = 0; i < kv.Size(); i++)
   {
      if (kv[i]) { delete kv[i]; }
   }

   data = np->data;
   np->kv.Copy(kv);

   ni  = np->ni;
   nj  = np->nj;
   nk  = np->nk;
   Dim = np->Dim;

   np->data = NULL;
   np->kv.SetSize(0);

   delete np;
}

NURBSPatch::~NURBSPatch()
{
   if (data != NULL)
   {
      delete [] data;
   }

   for (int i = 0; i < kv.Size(); i++)
   {
      if (kv[i]) { delete kv[i]; }
   }
}

void NURBSPatch::Print(std::ostream &os) const
{
   int size = 1;

   os << "knotvectors\n" << kv.Size() << '\n';
   for (int i = 0; i < kv.Size(); i++)
   {
      kv[i]->Print(os);
      size *= kv[i]->GetNCP();
   }

   os << "\ndimension\n" << Dim - 1
      << "\n\ncontrolpoints\n";
   for (int j = 0, i = 0; i < size; i++)
   {
      os << data[j++];
      for (int d = 1; d < Dim; d++)
      {
         os << ' ' << data[j++];
      }
      os << '\n';
   }
}

int NURBSPatch::SetLoopDirection(int dir)
{
   if (nj == -1)  // 1D case
   {
      if (dir == 0)
      {
         sd = Dim;
         nd = ni;
         ls = Dim;
         return ls;
      }
      else
      {
         mfem::err << "NURBSPatch::SetLoopDirection :\n"
                   " Direction error in 1D patch, dir = " << dir << '\n';
         mfem_error();
      }
   }
   else if (nk == -1)  // 2D case
   {
      if (dir == 0)
      {
         sd = Dim;
         nd = ni;
         ls = nj*Dim;
         return ls;
      }
      else if (dir == 1)
      {
         sd = ni*Dim;
         nd = nj;
         ls = ni*Dim;
         return ls;
      }
      else
      {
         mfem::err << "NURBSPatch::SetLoopDirection :\n"
                   " Direction error in 2D patch, dir = " << dir << '\n';
         mfem_error();
      }
   }
   else  // 3D case
   {
      if (dir == 0)
      {
         sd = Dim;
         nd = ni;
         ls = nj*nk*Dim;
         return ls;
      }
      else if (dir == 1)
      {
         sd = ni*Dim;
         nd = nj;
         ls = ni*nk*Dim;
         return ls;
      }
      else if (dir == 2)
      {
         sd = ni*nj*Dim;
         nd = nk;
         ls = ni*nj*Dim;
         return ls;
      }
      else
      {
         mfem::err << "NURBSPatch::SetLoopDirection :\n"
                   " Direction error in 3D patch, dir = " << dir << '\n';
         mfem_error();
      }
   }

   return -1;
}

void NURBSPatch::UniformRefinement(Array<int> const& rf)
{
   Vector newknots;
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      if (rf[dir] != 1)
      {
         kv[dir]->Refinement(newknots, rf[dir]);
         KnotInsert(dir, newknots);
      }
   }
}

void NURBSPatch::UniformRefinement(int rf)
{
   Array<int> rf_array(kv.Size());
   rf_array = rf;
   UniformRefinement(rf_array);
}

void NURBSPatch::Coarsen(Array<int> const& cf, real_t tol)
{
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      if (!kv[dir]->coarse)
      {
         const int ne_fine = kv[dir]->GetNE();
         KnotRemove(dir, kv[dir]->GetFineKnots(cf[dir]), tol);
         kv[dir]->coarse = true;
         kv[dir]->GetElements();

         const int ne_coarse = kv[dir]->GetNE();
         MFEM_VERIFY(ne_fine == cf[dir] * ne_coarse, "");
         if (kv[dir]->spacing)
         {
            kv[dir]->spacing->SetSize(ne_coarse);
            kv[dir]->spacing->ScaleParameters((real_t) cf[dir]);
         }
      }
   }
}

void NURBSPatch::Coarsen(int cf, real_t tol)
{
   Array<int> cf_array(kv.Size());
   cf_array = cf;
   Coarsen(cf_array, tol);
}

void NURBSPatch::GetCoarseningFactors(Array<int> & f) const
{
   f.SetSize(kv.Size());
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      f[dir] = kv[dir]->GetCoarseningFactor();
   }
}

void NURBSPatch::KnotInsert(Array<KnotVector *> &newkv)
{
   MFEM_ASSERT(newkv.Size() == kv.Size(), "Invalid input to KnotInsert");
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      KnotInsert(dir, *newkv[dir]);
   }
}

void NURBSPatch::KnotInsert(int dir, const KnotVector &newkv)
{
   if (dir >= kv.Size() || dir < 0)
   {
      mfem_error("NURBSPatch::KnotInsert : Incorrect direction!");
   }

   int t = newkv.GetOrder() - kv[dir]->GetOrder();

   if (t > 0)
   {
      DegreeElevate(dir, t);
   }
   else if (t < 0)
   {
      mfem_error("NURBSPatch::KnotInsert : Incorrect order!");
   }

   Vector diff;
   GetKV(dir)->Difference(newkv, diff);
   if (diff.Size() > 0)
   {
      KnotInsert(dir, diff);
   }
}

void NURBSPatch::KnotInsert(Array<Vector *> &newkv)
{
   MFEM_ASSERT(newkv.Size() == kv.Size(), "Invalid input to KnotInsert");
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      KnotInsert(dir, *newkv[dir]);
   }
}

void NURBSPatch::KnotRemove(Array<Vector *> &rmkv, real_t tol)
{
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      KnotRemove(dir, *rmkv[dir], tol);
   }
}

void NURBSPatch::KnotRemove(int dir, const Vector &knot, real_t tol)
{
   // TODO: implement an efficient version of this!
   for (auto k : knot)
   {
      KnotRemove(dir, k, 1, tol);
   }
}

// Algorithm A5.5 from "The NURBS Book", 2nd ed, Piegl and Tiller, chapter 5.
void NURBSPatch::KnotInsert(int dir, const Vector &knot)
{
   if (knot.Size() == 0 ) { return; }

   if (dir >= kv.Size() || dir < 0)
   {
      mfem_error("NURBSPatch::KnotInsert : Invalid direction!");
   }

   NURBSPatch &oldp  = *this;
   KnotVector &oldkv = *kv[dir];

   NURBSPatch *newpatch = new NURBSPatch(this, dir, oldkv.GetOrder(),
                                         oldkv.GetNCP() + knot.Size());
   NURBSPatch &newp  = *newpatch;
   KnotVector &newkv = *newp.GetKV(dir);

   newkv.spacing = oldkv.spacing;

   int size = oldp.SetLoopDirection(dir);
   if (size != newp.SetLoopDirection(dir))
   {
      mfem_error("NURBSPatch::KnotInsert : Size mismatch!");
   }

   int rr = knot.Size() - 1;
   int a  = oldkv.findKnotSpan(knot(0))  - 1;
   int b  = oldkv.findKnotSpan(knot(rr)) - 1;
   int pl = oldkv.GetOrder();
   int ml = oldkv.GetNCP();

   for (int j = 0; j <= a; j++)
   {
      newkv[j] = oldkv[j];
   }
   for (int j = b+pl; j <= ml+pl; j++)
   {
      newkv[j+rr+1] = oldkv[j];
   }
   for (int k = 0; k <= (a-pl); k++)
   {
      for (int ll = 0; ll < size; ll++)
      {
         newp.slice(k,ll) = oldp.slice(k,ll);
      }
   }
   for (int k = (b-1); k < ml; k++)
   {
      for (int ll = 0; ll < size; ll++)
      {
         newp.slice(k+rr+1,ll) = oldp.slice(k,ll);
      }
   }

   int i = b+pl-1;
   int k = b+pl+rr;

   for (int j = rr; j >= 0; j--)
   {
      while ( (knot(j) <= oldkv[i]) && (i > a) )
      {
         newkv[k] = oldkv[i];
         for (int ll = 0; ll < size; ll++)
         {
            newp.slice(k-pl-1,ll) = oldp.slice(i-pl-1,ll);
         }

         k--;
         i--;
      }

      for (int ll = 0; ll < size; ll++)
      {
         newp.slice(k-pl-1,ll) = newp.slice(k-pl,ll);
      }

      for (int l = 1; l <= pl; l++)
      {
         int ind = k-pl+l;
         real_t alfa = newkv[k+l] - knot(j);
         if (fabs(alfa) == 0.0)
         {
            for (int ll = 0; ll < size; ll++)
            {
               newp.slice(ind-1,ll) = newp.slice(ind,ll);
            }
         }
         else
         {
            alfa = alfa/(newkv[k+l] - oldkv[i-pl+l]);
            for (int ll = 0; ll < size; ll++)
            {
               newp.slice(ind-1,ll) = alfa*newp.slice(ind-1,ll) +
                                      (1.0-alfa)*newp.slice(ind,ll);
            }
         }
      }

      newkv[k] = knot(j);
      k--;
   }

   newkv.GetElements();

   swap(newpatch);
}

// Algorithm A5.8 from "The NURBS Book", 2nd ed, Piegl and Tiller, chapter 5.
int NURBSPatch::KnotRemove(int dir, real_t knot, int ntimes, real_t tol)
{
   if (dir >= kv.Size() || dir < 0)
   {
      mfem_error("NURBSPatch::KnotRemove : Invalid direction!");
   }

   NURBSPatch &oldp  = *this;
   KnotVector &oldkv = *kv[dir];

   // Find the index of the last occurrence of the knot.
   int id = -1;
   int multiplicity = 0;
   for (int i=0; i<oldkv.Size(); ++i)
   {
      if (oldkv[i] == knot)
      {
         id = i;
         multiplicity++;
      }
   }

   MFEM_VERIFY(0 < id && id < oldkv.Size() - 1 && ntimes <= multiplicity,
               "Only interior knots of sufficient multiplicity may be removed.");

   const int p = oldkv.GetOrder();

   NURBSPatch tmpp(this, dir, p, oldkv.GetNCP());

   const int size = oldp.SetLoopDirection(dir);
   if (size != tmpp.SetLoopDirection(dir))
   {
      mfem_error("NURBSPatch::KnotRemove : Size mismatch!");
   }

   // Copy old data
   for (int k = 0; k < oldp.nd; ++k)
   {
      for (int ll = 0; ll < size; ll++)
      {
         tmpp.slice(k,ll) = oldp.slice(k,ll);
      }
   }

   const int r = id;
   const int s = multiplicity;

   int last = r - s;
   int first = r - p;

   int i = first;
   int j = last;

   Array2D<real_t> temp(last + ntimes + 1, size);

   for (int t=0; t<ntimes; ++t)
   {
      int off = first - 1;  // Difference in index between temp and P.

      for (int ll = 0; ll < size; ll++)
      {
         temp(0, ll) = oldp.slice(off, ll);
         temp(last + 1 - off, ll) = oldp.slice(last + 1, ll);
      }

      int ii = 1;
      int jj = last - off;

      while (j - i > t)
      {
         // Compute new control points for one removal step
         const real_t a_i = (knot - oldkv[i]) / (oldkv[i+p+1] - oldkv[i]);
         const real_t a_j = (knot - oldkv[j]) / (oldkv[j+p+1] - oldkv[j]);

         for (int ll = 0; ll < size; ll++)
         {
            temp(ii,ll) = (1.0 / a_i) * oldp.slice(i,ll) -
                          ((1.0/a_i) - 1.0) * temp(ii - 1, ll);

            temp(jj,ll) = (1.0 / (1.0 - a_j)) * (oldp.slice(j,ll) -
                                                 (a_j * temp(jj + 1, ll)));
         }

         i++;  ii++;
         j--;  jj--;
      }

      // Check whether knot is removable
      Vector diff(size);
      if (j - i < t)
      {
         for (int ll = 0; ll < size; ll++)
         {
            diff[ll] = temp(ii-1, ll) - temp(jj+1, ll);
         }
      }
      else
      {
         const real_t a_i = (knot - oldkv[i]) / (oldkv[i+p+1] - oldkv[i]);
         for (int ll = 0; ll < size; ll++)
            diff[ll] = oldp.slice(i,ll) - (a_i * temp(ii+1, ll))
                       - ((1.0 - a_i) * temp(ii-1, ll));
      }

      const real_t dist = diff.Norml2();
      if (dist >= tol)
      {
         // Removal failed. Return the number of successful removals.
         mfem::out << "Knot removal failed after " << t
                   << " successful removals" << endl;
         return t;
      }

      // Note that the new weights may not be positive.

      // Save new control points
      i = first;
      j = last;

      while (j - i > t)
      {
         for (int ll = 0; ll < size; ll++)
         {
            tmpp.slice(i,ll) = temp(i - off,ll);
            tmpp.slice(j,ll) = temp(j - off,ll);
         }
         i++;
         j--;
      }

      first--;
      last++;
   }  // End of loop (t) over ntimes.

   const int fout = ((2*r) - s - p) / 2;  // First control point out
   j = fout;
   i = j;

   for (int k=1; k<ntimes; ++k)
   {
      if (k % 2 == 1)
      {
         i++;
      }
      else
      {
         j--;
      }
   }

   NURBSPatch *newpatch = new NURBSPatch(this, dir, p,
                                         oldkv.GetNCP() - ntimes);
   NURBSPatch &newp = *newpatch;
   if (size != newp.SetLoopDirection(dir))
   {
      mfem_error("NURBSPatch::KnotRemove : Size mismatch!");
   }

   for (int k = 0; k < fout; ++k)
   {
      for (int ll = 0; ll < size; ll++)
      {
         newp.slice(k,ll) = oldp.slice(k,ll);  // Copy old data
      }
   }

   for (int k = i+1; k < oldp.nd; ++k)
   {
      for (int ll = 0; ll < size; ll++)
      {
         newp.slice(j,ll) = tmpp.slice(k,ll);  // Shift
      }

      j++;
   }

   KnotVector &newkv = *newp.GetKV(dir);
   MFEM_VERIFY(newkv.Size() == oldkv.Size() - ntimes, "");

   newkv.spacing = oldkv.spacing;
   newkv.coarse = oldkv.coarse;

   for (int k = 0; k < id - ntimes + 1; k++)
   {
      newkv[k] = oldkv[k];
   }
   for (int k = id + 1; k < oldkv.Size(); k++)
   {
      newkv[k - ntimes] = oldkv[k];
   }

   newkv.GetElements();

   swap(newpatch);

   return ntimes;
}

void NURBSPatch::DegreeElevate(int t)
{
   for (int dir = 0; dir < kv.Size(); dir++)
   {
      DegreeElevate(dir, t);
   }
}

// Routine from "The NURBS Book" - 2nd ed - Piegl and Tiller
void NURBSPatch::DegreeElevate(int dir, int t)
{
   if (dir >= kv.Size() || dir < 0)
   {
      mfem_error("NURBSPatch::DegreeElevate : Incorrect direction!");
   }

   MFEM_ASSERT(t >= 0, "DegreeElevate cannot decrease the degree.");

   int i, j, k, kj, mpi, mul, mh, kind, cind, first, last;
   int r, a, b, oldr, save, s, tr, lbz, rbz, l;
   real_t inv, ua, ub, numer, alf, den, bet, gam;

   NURBSPatch &oldp  = *this;
   KnotVector &oldkv = *kv[dir];
   oldkv.GetElements();

   auto *newpatch = new NURBSPatch(this, dir, oldkv.GetOrder() + t,
                                   oldkv.GetNCP() + oldkv.GetNE()*t);
   NURBSPatch &newp  = *newpatch;
   KnotVector &newkv = *newp.GetKV(dir);

   if (oldkv.spacing) { newkv.spacing = oldkv.spacing; }

   int size = oldp.SetLoopDirection(dir);
   if (size != newp.SetLoopDirection(dir))
   {
      mfem_error("NURBSPatch::DegreeElevate : Size mismatch!");
   }

   int p = oldkv.GetOrder();
   int n = oldkv.GetNCP()-1;

   DenseMatrix bezalfs (p+t+1, p+1);
   DenseMatrix bpts    (p+1,   size);
   DenseMatrix ebpts   (p+t+1, size);
   DenseMatrix nextbpts(p-1,   size);
   Vector      alphas  (p-1);

   int m = n + p + 1;
   int ph = p + t;
   int ph2 = ph/2;

   {
      Array2D<int> binom(ph+1, ph+1);
      for (i = 0; i <= ph; i++)
      {
         binom(i,0) = binom(i,i) = 1;
         for (j = 1; j < i; j++)
         {
            binom(i,j) = binom(i-1,j) + binom(i-1,j-1);
         }
      }

      bezalfs(0,0)  = 1.0;
      bezalfs(ph,p) = 1.0;

      for (i = 1; i <= ph2; i++)
      {
         inv = 1.0/binom(ph,i);
         mpi = min(p,i);
         for (j = max(0,i-t); j <= mpi; j++)
         {
            bezalfs(i,j) = inv*binom(p,j)*binom(t,i-j);
         }
      }
   }

   for (i = ph2+1; i < ph; i++)
   {
      mpi = min(p,i);
      for (j = max(0,i-t); j <= mpi; j++)
      {
         bezalfs(i,j) = bezalfs(ph-i,p-j);
      }
   }

   mh = ph;
   kind = ph + 1;
   r = -1;
   a = p;
   b = p + 1;
   cind = 1;
   ua = oldkv[0];
   for (l = 0; l < size; l++)
   {
      newp.slice(0,l) = oldp.slice(0,l);
   }
   for (i = 0; i <= ph; i++)
   {
      newkv[i] = ua;
   }

   for (i = 0; i <= p; i++)
   {
      for (l = 0; l < size; l++)
      {
         bpts(i,l) = oldp.slice(i,l);
      }
   }

   while (b < m)
   {
      i = b;
      while (b < m && oldkv[b] == oldkv[b+1]) { b++; }

      mul = b-i+1;

      mh = mh + mul + t;
      ub = oldkv[b];
      oldr = r;
      r = p-mul;
      if (oldr > 0) { lbz = (oldr+2)/2; }
      else { lbz = 1; }

      if (r > 0) { rbz = ph-(r+1)/2; }
      else { rbz = ph; }

      if (r > 0)
      {
         numer = ub - ua;
         for (k = p ; k > mul; k--)
         {
            alphas[k-mul-1] = numer/(oldkv[a+k]-ua);
         }

         for (j = 1; j <= r; j++)
         {
            save = r-j;
            s = mul+j;
            for (k = p; k >= s; k--)
            {
               for (l = 0; l < size; l++)
                  bpts(k,l) = (alphas[k-s]*bpts(k,l) +
                               (1.0-alphas[k-s])*bpts(k-1,l));
            }
            for (l = 0; l < size; l++)
            {
               nextbpts(save,l) = bpts(p,l);
            }
         }
      }

      for (i = lbz; i <= ph; i++)
      {
         for (l = 0; l < size; l++)
         {
            ebpts(i,l) = 0.0;
         }
         mpi = min(p,i);
         for (j = max(0,i-t); j <= mpi; j++)
         {
            for (l = 0; l < size; l++)
            {
               ebpts(i,l) += bezalfs(i,j)*bpts(j,l);
            }
         }
      }

      if (oldr > 1)
      {
         first = kind-2;
         last = kind;
         den = ub-ua;
         bet = (ub-newkv[kind-1])/den;

         for (tr = 1; tr < oldr; tr++)
         {
            i = first;
            j = last;
            kj = j-kind+1;
            while (j-i > tr)
            {
               if (i < cind)
               {
                  alf = (ub-newkv[i])/(ua-newkv[i]);
                  for (l = 0; l < size; l++)
                  {
                     newp.slice(i,l) = alf*newp.slice(i,l)-(1.0-alf)*newp.slice(i-1,l);
                  }
               }
               if (j >= lbz)
               {
                  if ((j-tr) <= (kind-ph+oldr))
                  {
                     gam = (ub-newkv[j-tr])/den;
                     for (l = 0; l < size; l++)
                     {
                        ebpts(kj,l) = gam*ebpts(kj,l) + (1.0-gam)*ebpts(kj+1,l);
                     }
                  }
                  else
                  {
                     for (l = 0; l < size; l++)
                     {
                        ebpts(kj,l) = bet*ebpts(kj,l) + (1.0-bet)*ebpts(kj+1,l);
                     }
                  }
               }
               i = i+1;
               j = j-1;
               kj = kj-1;
            }
            first--;
            last++;
         }
      }

      if (a != p)
      {
         for (i = 0; i < (ph-oldr); i++)
         {
            newkv[kind] = ua;
            kind = kind+1;
         }
      }
      for (j = lbz; j <= rbz; j++)
      {
         for (l = 0; l < size; l++)
         {
            newp.slice(cind,l) =  ebpts(j,l);
         }
         cind = cind +1;
      }

      if (b < m)
      {
         for (j = 0; j <r; j++)
            for (l = 0; l < size; l++)
            {
               bpts(j,l) = nextbpts(j,l);
            }

         for (j = r; j <= p; j++)
            for (l = 0; l < size; l++)
            {
               bpts(j,l) = oldp.slice(b-p+j,l);
            }

         a = b;
         b = b+1;
         ua = ub;
      }
      else
      {
         for (i = 0; i <= ph; i++)
         {
            newkv[kind+i] = ub;
         }
      }
   }
   newkv.GetElements();

   swap(newpatch);
}

void NURBSPatch::FlipDirection(int dir)
{
   int size = SetLoopDirection(dir);

   for (int id = 0; id < nd/2; id++)
      for (int i = 0; i < size; i++)
      {
         Swap<real_t>((*this).slice(id,i), (*this).slice(nd-1-id,i));
      }
   kv[dir]->Flip();
}

void NURBSPatch::SwapDirections(int dir1, int dir2)
{
   if (abs(dir1-dir2) == 2)
   {
      mfem_error("NURBSPatch::SwapDirections :"
                 " directions 0 and 2 are not supported!");
   }

   Array<const KnotVector *> nkv(kv);

   Swap<const KnotVector *>(nkv[dir1], nkv[dir2]);
   NURBSPatch *newpatch = new NURBSPatch(nkv, Dim);

   int size = SetLoopDirection(dir1);
   newpatch->SetLoopDirection(dir2);

   for (int id = 0; id < nd; id++)
      for (int i = 0; i < size; i++)
      {
         (*newpatch).slice(id,i) = (*this).slice(id,i);
      }

   swap(newpatch);
}

void NURBSPatch::Rotate(real_t angle, real_t n[])
{
   if (Dim == 3)
   {
      Rotate2D(angle);
   }
   else
   {
      if (n == NULL)
      {
         mfem_error("NURBSPatch::Rotate : Specify an angle for a 3D rotation.");
      }

      Rotate3D(n, angle);
   }
}

void NURBSPatch::Get2DRotationMatrix(real_t angle, DenseMatrix &T)
{
   real_t s = sin(angle);
   real_t c = cos(angle);

   T.SetSize(2);
   T(0,0) = c;
   T(0,1) = -s;
   T(1,0) = s;
   T(1,1) = c;
}

void NURBSPatch::Rotate2D(real_t angle)
{
   if (Dim != 3)
   {
      mfem_error("NURBSPatch::Rotate2D : not a NURBSPatch in 2D!");
   }

   DenseMatrix T(2);
   Vector x(2), y(NULL, 2);

   Get2DRotationMatrix(angle, T);

   int size = 1;
   for (int i = 0; i < kv.Size(); i++)
   {
      size *= kv[i]->GetNCP();
   }

   for (int i = 0; i < size; i++)
   {
      y.SetData(data + i*Dim);
      x = y;
      T.Mult(x, y);
   }
}

void NURBSPatch::Get3DRotationMatrix(real_t n[], real_t angle, real_t r,
                                     DenseMatrix &T)
{
   real_t c, s, c1;
   const real_t l2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
   const real_t l = sqrt(l2);

   MFEM_ASSERT(l2 > 0.0, "3D rotation axis is undefined");

   if (fabs(angle) == (real_t)(M_PI_2))
   {
      s = r*copysign(1., angle);
      c = 0.;
      c1 = -1.;
   }
   else if (fabs(angle) == (real_t)(M_PI))
   {
      s = 0.;
      c = -r;
      c1 = c - 1.;
   }
   else
   {
      s = r*sin(angle);
      c = r*cos(angle);
      c1 = c - 1.;
   }

   T.SetSize(3);

   T(0,0) =  (n[0]*n[0] + (n[1]*n[1] + n[2]*n[2])*c)/l2;
   T(0,1) = -(n[0]*n[1]*c1)/l2 - (n[2]*s)/l;
   T(0,2) = -(n[0]*n[2]*c1)/l2 + (n[1]*s)/l;
   T(1,0) = -(n[0]*n[1]*c1)/l2 + (n[2]*s)/l;
   T(1,1) =  (n[1]*n[1] + (n[0]*n[0] + n[2]*n[2])*c)/l2;
   T(1,2) = -(n[1]*n[2]*c1)/l2 - (n[0]*s)/l;
   T(2,0) = -(n[0]*n[2]*c1)/l2 - (n[1]*s)/l;
   T(2,1) = -(n[1]*n[2]*c1)/l2 + (n[0]*s)/l;
   T(2,2) =  (n[2]*n[2] + (n[0]*n[0] + n[1]*n[1])*c)/l2;
}

void NURBSPatch::Rotate3D(real_t n[], real_t angle)
{
   if (Dim != 4)
   {
      mfem_error("NURBSPatch::Rotate3D : not a NURBSPatch in 3D!");
   }

   DenseMatrix T(3);
   Vector x(3), y(NULL, 3);

   Get3DRotationMatrix(n, angle, 1., T);

   int size = 1;
   for (int i = 0; i < kv.Size(); i++)
   {
      size *= kv[i]->GetNCP();
   }

   for (int i = 0; i < size; i++)
   {
      y.SetData(data + i*Dim);
      x = y;
      T.Mult(x, y);
   }
}

int NURBSPatch::MakeUniformDegree(int degree)
{
   int maxd = degree;

   if (maxd == -1)
   {
      for (int dir = 0; dir < kv.Size(); dir++)
      {
         maxd = std::max(maxd, kv[dir]->GetOrder());
      }
   }

   for (int dir = 0; dir < kv.Size(); dir++)
   {
      if (maxd > kv[dir]->GetOrder())
      {
         DegreeElevate(dir, maxd - kv[dir]->GetOrder());
      }
   }

   return maxd;
}

NURBSPatch *Interpolate(NURBSPatch &p1, NURBSPatch &p2)
{
   if (p1.kv.Size() != p2.kv.Size() || p1.Dim != p2.Dim)
   {
      mfem_error("Interpolate(NURBSPatch &, NURBSPatch &)");
   }

   int size = 1, dim = p1.Dim;
   Array<const KnotVector *> kv(p1.kv.Size() + 1);

   for (int i = 0; i < p1.kv.Size(); i++)
   {
      if (p1.kv[i]->GetOrder() < p2.kv[i]->GetOrder())
      {
         p1.KnotInsert(i, *p2.kv[i]);
         p2.KnotInsert(i, *p1.kv[i]);
      }
      else
      {
         p2.KnotInsert(i, *p1.kv[i]);
         p1.KnotInsert(i, *p2.kv[i]);
      }
      kv[i] = p1.kv[i];
      size *= kv[i]->GetNCP();
   }

   KnotVector &nkv = *(new KnotVector(1, 2));
   nkv[0] = nkv[1] = 0.0;
   nkv[2] = nkv[3] = 1.0;
   nkv.GetElements();
   kv.Last() = &nkv;

   NURBSPatch *patch = new NURBSPatch(kv, dim);
   delete kv.Last();

   for (int i = 0; i < size; i++)
   {
      for (int d = 0; d < dim; d++)
      {
         patch->data[i*dim+d] = p1.data[i*dim+d];
         patch->data[(i+size)*dim+d] = p2.data[i*dim+d];
      }
   }

   return patch;
}

NURBSPatch *Revolve3D(NURBSPatch &patch, real_t n[], real_t ang, int times)
{
   if (patch.Dim != 4)
   {
      mfem_error("Revolve3D(NURBSPatch &, real_t [], real_t)");
   }

   int size = 1, ns;
   Array<const KnotVector *> nkv(patch.kv.Size() + 1);

   for (int i = 0; i < patch.kv.Size(); i++)
   {
      nkv[i] = patch.kv[i];
      size *= nkv[i]->GetNCP();
   }
   ns = 2*times + 1;
   KnotVector &lkv = *(new KnotVector(2, ns));
   nkv.Last() = &lkv;
   lkv[0] = lkv[1] = lkv[2] = 0.0;
   for (int i = 1; i < times; i++)
   {
      lkv[2*i+1] = lkv[2*i+2] = i;
   }
   lkv[ns] = lkv[ns+1] = lkv[ns+2] = times;
   lkv.GetElements();
   NURBSPatch *newpatch = new NURBSPatch(nkv, 4);
   delete nkv.Last();

   DenseMatrix T(3), T2(3);
   Vector u(NULL, 3), v(NULL, 3);

   NURBSPatch::Get3DRotationMatrix(n, ang, 1., T);
   real_t c = cos(ang/2);
   NURBSPatch::Get3DRotationMatrix(n, ang/2, 1./c, T2);
   T2 *= c;

   real_t *op = patch.data, *np;
   for (int i = 0; i < size; i++)
   {
      np = newpatch->data + 4*i;
      for (int j = 0; j < 4; j++)
      {
         np[j] = op[j];
      }
      for (int j = 0; j < times; j++)
      {
         u.SetData(np);
         v.SetData(np += 4*size);
         T2.Mult(u, v);
         v[3] = c*u[3];
         v.SetData(np += 4*size);
         T.Mult(u, v);
         v[3] = u[3];
      }
      op += 4;
   }

   return newpatch;
}

void NURBSPatch::SetKnotVectorsCoarse(bool c)
{
   for (int i=0; i<kv.Size(); ++i)
   {
      kv[i]->coarse = c;
   }
}

NURBSExtension::NURBSExtension(const NURBSExtension &orig)
   : mOrder(orig.mOrder), mOrders(orig.mOrders),
     NumOfKnotVectors(orig.NumOfKnotVectors),
     NumOfVertices(orig.NumOfVertices),
     NumOfElements(orig.NumOfElements),
     NumOfBdrElements(orig.NumOfBdrElements),
     NumOfDofs(orig.NumOfDofs),
     NumOfActiveVertices(orig.NumOfActiveVertices),
     NumOfActiveElems(orig.NumOfActiveElems),
     NumOfActiveBdrElems(orig.NumOfActiveBdrElems),
     NumOfActiveDofs(orig.NumOfActiveDofs),
     activeVert(orig.activeVert),
     activeElem(orig.activeElem),
     activeBdrElem(orig.activeBdrElem),
     activeDof(orig.activeDof),
     patchTopo(new Mesh(*orig.patchTopo)),
     own_topo(true),
     edge_to_ukv(orig.edge_to_ukv),
     knotVectors(orig.knotVectors.Size()), // knotVectors are copied in the body
     knotVectorsCompr(orig.knotVectorsCompr.Size()),
     weights(orig.weights),
     d_to_d(orig.d_to_d),
     master(orig.master),
     slave(orig.slave),
     v_meshOffsets(orig.v_meshOffsets),
     e_meshOffsets(orig.e_meshOffsets),
     f_meshOffsets(orig.f_meshOffsets),
     p_meshOffsets(orig.p_meshOffsets),
     v_spaceOffsets(orig.v_spaceOffsets),
     e_spaceOffsets(orig.e_spaceOffsets),
     f_spaceOffsets(orig.f_spaceOffsets),
     p_spaceOffsets(orig.p_spaceOffsets),
     el_dof(orig.el_dof ? new Table(*orig.el_dof) : NULL),
     bel_dof(orig.bel_dof ? new Table(*orig.bel_dof) : NULL),
     el_to_patch(orig.el_to_patch),
     bel_to_patch(orig.bel_to_patch),
     el_to_IJK(orig.el_to_IJK),
     bel_to_IJK(orig.bel_to_IJK),
     patches(orig.patches.Size()) // patches are copied in the body
{
   // Copy the knot vectors:
   for (int i = 0; i < knotVectors.Size(); i++)
   {
      knotVectors[i] = new KnotVector(*orig.knotVectors[i]);
   }
   CreateComprehensiveKV();

   // Copy the patches:
   for (int p = 0; p < patches.Size(); p++)
   {
      patches[p] = new NURBSPatch(*orig.patches[p]);
   }
}

NURBSExtension::NURBSExtension(std::istream &input, bool spacing)
{
   // Read topology
   patchTopo = new Mesh;
   patchTopo->LoadPatchTopo(input, edge_to_ukv);
   own_topo = true;

   CheckPatches();
   // CheckBdrPatches();

   skip_comment_lines(input, '#');

   // Read knotvectors or patches
   string ident;
   input >> ws >> ident; // 'knotvectors' or 'patches'
   if (ident == "knotvectors")
   {
      input >> NumOfKnotVectors;
      knotVectors.SetSize(NumOfKnotVectors);
      for (int i = 0; i < NumOfKnotVectors; i++)
      {
         knotVectors[i] = new KnotVector(input);
      }

      if (spacing)  // Read spacing formulas for knotvectors
      {
         input >> ws >> ident; // 'spacing'
         MFEM_VERIFY(ident == "spacing",
                     "Spacing formula section missing from NURBS mesh file");
         int numSpacing = 0;
         input >> numSpacing;
         for (int j = 0; j < numSpacing; j++)
         {
            int ki, spacingType, numIntParam, numRealParam;
            input >> ki >> spacingType >> numIntParam >> numRealParam;

            MFEM_VERIFY(0 <= ki && ki < NumOfKnotVectors,
                        "Invalid knotvector index");
            MFEM_VERIFY(numIntParam >= 0 && numRealParam >= 0,
                        "Invalid number of parameters in KnotVector");

            Array<int> ipar(numIntParam);
            Vector dpar(numRealParam);

            for (int i=0; i<numIntParam; ++i)
            {
               input >> ipar[i];
            }

            for (int i=0; i<numRealParam; ++i)
            {
               input >> dpar[i];
            }

            const SpacingType s = (SpacingType) spacingType;
            knotVectors[ki]->spacing = GetSpacingFunction(s, ipar, dpar);
         }
      }
   }
   else if (ident == "patches")
   {
      patches.SetSize(GetNP());
      for (int p = 0; p < patches.Size(); p++)
      {
         skip_comment_lines(input, '#');
         patches[p] = new NURBSPatch(input);
      }

      NumOfKnotVectors = 0;
      for (int i = 0; i < patchTopo->GetNEdges(); i++)
         if (NumOfKnotVectors < KnotInd(i))
         {
            NumOfKnotVectors = KnotInd(i);
         }
      NumOfKnotVectors++;
      knotVectors.SetSize(NumOfKnotVectors);
      knotVectors = NULL;

      Array<int> edges, oedge;
      for (int p = 0; p < patches.Size(); p++)
      {
         if (Dimension() == 1)
         {
            if (knotVectors[KnotInd(p)] == NULL)
            {
               knotVectors[KnotInd(p)] =
                  new KnotVector(*patches[p]->GetKV(0));
            }
         }
         else if (Dimension() == 2)
         {
            patchTopo->GetElementEdges(p, edges, oedge);
            if (knotVectors[KnotInd(edges[0])] == NULL)
            {
               knotVectors[KnotInd(edges[0])] =
                  new KnotVector(*patches[p]->GetKV(0));
            }
            if (knotVectors[KnotInd(edges[1])] == NULL)
            {
               knotVectors[KnotInd(edges[1])] =
                  new KnotVector(*patches[p]->GetKV(1));
            }
         }
         else if (Dimension() == 3)
         {
            patchTopo->GetElementEdges(p, edges, oedge);
            if (knotVectors[KnotInd(edges[0])] == NULL)
            {
               knotVectors[KnotInd(edges[0])] =
                  new KnotVector(*patches[p]->GetKV(0));
            }
            if (knotVectors[KnotInd(edges[3])] == NULL)
            {
               knotVectors[KnotInd(edges[3])] =
                  new KnotVector(*patches[p]->GetKV(1));
            }
            if (knotVectors[KnotInd(edges[8])] == NULL)
            {
               knotVectors[KnotInd(edges[8])] =
                  new KnotVector(*patches[p]->GetKV(2));
            }
         }
      }
   }
   else
   {
      MFEM_ABORT("invalid section: " << ident);
   }

   CreateComprehensiveKV();

   SetOrdersFromKnotVectors();

   GenerateOffsets();
   CountElements();
   CountBdrElements();
   // NumOfVertices, NumOfElements, NumOfBdrElements, NumOfDofs

   skip_comment_lines(input, '#');

   // Check for a list of mesh elements
   if (patches.Size() == 0)
   {
      input >> ws >> ident;
   }
   if (patches.Size() == 0 && ident == "mesh_elements")
   {
      input >> NumOfActiveElems;
      activeElem.SetSize(GetGNE());
      activeElem = false;
      int glob_elem;
      for (int i = 0; i < NumOfActiveElems; i++)
      {
         input >> glob_elem;
         activeElem[glob_elem] = true;
      }

      skip_comment_lines(input, '#');
      input >> ws >> ident;
   }
   else
   {
      NumOfActiveElems = NumOfElements;
      activeElem.SetSize(NumOfElements);
      activeElem = true;
   }

   GenerateActiveVertices();
   InitDofMap();
   GenerateElementDofTable();
   GenerateActiveBdrElems();
   GenerateBdrElementDofTable();

   // periodic
   if (ident == "periodic")
   {
      master.Load(input);
      slave.Load(input);

      skip_comment_lines(input, '#');
      input >> ws >> ident;
   }

   if (patches.Size() == 0)
   {
      // weights
      if (ident == "weights")
      {
         weights.Load(input, GetNDof());
      }
      else // e.g. ident = "unitweights" or "autoweights"
      {
         weights.SetSize(GetNDof());
         weights = 1.0;
      }
   }

   // periodic
   ConnectBoundaries();
}

NURBSExtension::NURBSExtension(NURBSExtension *parent, int newOrder)
{
   patchTopo = parent->patchTopo;
   own_topo = false;

   parent->edge_to_ukv.Copy(edge_to_ukv);

   NumOfKnotVectors = parent->GetNKV();
   knotVectors.SetSize(NumOfKnotVectors);
   knotVectorsCompr.SetSize(parent->GetNP()*parent->Dimension());
   const Array<int> &pOrders = parent->GetOrders();
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      if (newOrder > pOrders[i])
      {
         knotVectors[i] =
            parent->GetKnotVector(i)->DegreeElevate(newOrder - pOrders[i]);
      }
      else
      {
         knotVectors[i] = new KnotVector(*parent->GetKnotVector(i));
      }
   }
   CreateComprehensiveKV();

   // copy some data from parent
   NumOfElements    = parent->NumOfElements;
   NumOfBdrElements = parent->NumOfBdrElements;

   SetOrdersFromKnotVectors();

   GenerateOffsets(); // dof offsets will be different from parent

   NumOfActiveVertices = parent->NumOfActiveVertices;
   NumOfActiveElems    = parent->NumOfActiveElems;
   NumOfActiveBdrElems = parent->NumOfActiveBdrElems;
   parent->activeVert.Copy(activeVert);
   InitDofMap();
   parent->activeElem.Copy(activeElem);
   parent->activeBdrElem.Copy(activeBdrElem);

   GenerateElementDofTable();
   GenerateBdrElementDofTable();

   weights.SetSize(GetNDof());
   weights = 1.0;

   // periodic
   parent->master.Copy(master);
   parent->slave.Copy(slave);
   ConnectBoundaries();
}

NURBSExtension::NURBSExtension(NURBSExtension *parent,
                               const Array<int> &newOrders, Mode mode)
   : mode(mode)
{
   newOrders.Copy(mOrders);
   SetOrderFromOrders();

   patchTopo = parent->patchTopo;
   own_topo = false;

   parent->edge_to_ukv.Copy(edge_to_ukv);

   NumOfKnotVectors = parent->GetNKV();
   MFEM_VERIFY(mOrders.Size() == NumOfKnotVectors, "invalid newOrders array");
   knotVectors.SetSize(NumOfKnotVectors);
   const Array<int> &pOrders = parent->GetOrders();

   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      if (mOrders[i] > pOrders[i])
      {
         knotVectors[i] =
            parent->GetKnotVector(i)->DegreeElevate(mOrders[i] - pOrders[i]);
      }
      else
      {
         knotVectors[i] = new KnotVector(*parent->GetKnotVector(i));
      }
   }
   CreateComprehensiveKV();

   // copy some data from parent
   NumOfElements    = parent->NumOfElements;
   NumOfBdrElements = parent->NumOfBdrElements;

   GenerateOffsets(); // dof offsets will be different from parent

   NumOfActiveVertices = parent->NumOfActiveVertices;
   NumOfActiveElems    = parent->NumOfActiveElems;
   NumOfActiveBdrElems = parent->NumOfActiveBdrElems;
   parent->activeVert.Copy(activeVert);
   InitDofMap();
   parent->activeElem.Copy(activeElem);
   parent->activeBdrElem.Copy(activeBdrElem);

   GenerateElementDofTable();
   GenerateBdrElementDofTable();

   weights.SetSize(GetNDof());
   weights = 1.0;

   parent->master.Copy(master);
   parent->slave.Copy(slave);
   ConnectBoundaries();
}

NURBSExtension::NURBSExtension(Mesh *mesh_array[], int num_pieces)
{
   NURBSExtension *parent = mesh_array[0]->NURBSext;

   if (!parent->own_topo)
   {
      mfem_error("NURBSExtension::NURBSExtension :\n"
                 "  parent does not own the patch topology!");
   }
   patchTopo = parent->patchTopo;
   own_topo = true;
   parent->own_topo = false;

   parent->edge_to_ukv.Copy(edge_to_ukv);

   parent->GetOrders().Copy(mOrders);
   mOrder = parent->GetOrder();

   NumOfKnotVectors = parent->GetNKV();
   knotVectors.SetSize(NumOfKnotVectors);
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      knotVectors[i] = new KnotVector(*parent->GetKnotVector(i));
   }
   CreateComprehensiveKV();

   GenerateOffsets();
   CountElements();
   CountBdrElements();

   // assuming the meshes define a partitioning of all the elements
   NumOfActiveElems = NumOfElements;
   activeElem.SetSize(NumOfElements);
   activeElem = true;

   GenerateActiveVertices();
   InitDofMap();
   GenerateElementDofTable();
   GenerateActiveBdrElems();
   GenerateBdrElementDofTable();

   weights.SetSize(GetNDof());
   MergeWeights(mesh_array, num_pieces);
}

NURBSExtension::NURBSExtension(const Mesh *patch_topology,
                               const Array<const NURBSPatch*> &patches_)
{
   // Basic topology checks
   MFEM_VERIFY(patches_.Size() > 0, "Must have at least one patch");
   MFEM_VERIFY(patches_.Size() == patch_topology->GetNE(),
               "Number of patches must equal number of elements in patch_topology");

   // Copy patch_topology mesh and NURBSPatch(es)
   patchTopo = new Mesh( *patch_topology );
   patches.SetSize(patches_.Size());
   for (int p = 0; p < patches.Size(); p++)
   {
      patches[p] = new NURBSPatch(*patches_[p]);
   }

   Array<int> ukv_to_rpkv;
   patchTopo->GetEdgeToUniqueKnotvector(edge_to_ukv, ukv_to_rpkv);
   own_topo = true;

   CheckPatches(); // This is checking the edge_to_ukv mapping

   // Set number of unique (not comprehensive) knot vectors
   NumOfKnotVectors = ukv_to_rpkv.Size();
   knotVectors.SetSize(NumOfKnotVectors);
   knotVectors = NULL;

   // Assign the unique knot vectors from patches
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      // pkv = p*dim + d for an arbitrarily chosen patch p,
      // in its reference direction d
      const int pkv = ukv_to_rpkv[i];
      const int p = pkv / Dimension();
      const int d = pkv % Dimension();
      knotVectors[i] = new KnotVector(*patches[p]->GetKV(d));
   }

   CreateComprehensiveKV();
   SetOrdersFromKnotVectors();

   GenerateOffsets();
   CountElements();
   CountBdrElements();

   NumOfActiveElems = NumOfElements;
   activeElem.SetSize(NumOfElements);
   activeElem = true;

   GenerateActiveVertices();
   InitDofMap();
   GenerateElementDofTable();
   GenerateActiveBdrElems();
   GenerateBdrElementDofTable();

   ConnectBoundaries();
}

NURBSExtension::~NURBSExtension()
{
   if (bel_dof) { delete bel_dof; }
   if (el_dof) { delete el_dof; }

   for (int i = 0; i < knotVectors.Size(); i++)
   {
      delete knotVectors[i];
   }

   for (int i = 0; i < knotVectorsCompr.Size(); i++)
   {
      delete knotVectorsCompr[i];
   }

   for (int i = 0; i < patches.Size(); i++)
   {
      delete patches[i];
   }

   if (own_topo)
   {
      delete patchTopo;
   }
}

void NURBSExtension::Print(std::ostream &os, const std::string &comments) const
{
   Array<int> kvSpacing;
   if (patches.Size() == 0)
   {
      for (int i = 0; i < NumOfKnotVectors; i++)
      {
         if (knotVectors[i]->spacing) { kvSpacing.Append(i); }
      }
   }

   const int version = kvSpacing.Size() > 0 ? 11 : 10;  // v1.0 or v1.1
   patchTopo->PrintTopo(os, edge_to_ukv, version, comments);
   if (patches.Size() == 0)
   {
      os << "\nknotvectors\n" << NumOfKnotVectors << '\n';
      for (int i = 0; i < NumOfKnotVectors; i++)
      {
         knotVectors[i]->Print(os);
      }

      if (kvSpacing.Size() > 0)
      {
         os << "\nspacing\n" << kvSpacing.Size() << '\n';
         for (auto kv : kvSpacing)
         {
            os << kv << " ";
            knotVectors[kv]->spacing->Print(os);
         }
      }

      if (NumOfActiveElems < NumOfElements)
      {
         os << "\nmesh_elements\n" << NumOfActiveElems << '\n';
         for (int i = 0; i < NumOfElements; i++)
            if (activeElem[i])
            {
               os << i << '\n';
            }
      }

      os << "\nweights\n";
      weights.Print(os, 1);
   }
   else
   {
      os << "\npatches\n";
      for (int p = 0; p < patches.Size(); p++)
      {
         os << "\n# patch " << p << "\n\n";
         patches[p]->Print(os);
      }
   }
}

void NURBSExtension::PrintCharacteristics(std::ostream &os) const
{
   os <<
      "NURBS Mesh entity sizes:\n"
      "Dimension           = " << Dimension() << "\n"
      "Unique Orders       = ";
   Array<int> unique_orders(mOrders);
   unique_orders.Sort();
   unique_orders.Unique();
   unique_orders.Print(os, unique_orders.Size());
   os <<
      "NumOfKnotVectors    = " << GetNKV() << "\n"
      "NumOfPatches        = " << GetNP() << "\n"
      "NumOfBdrPatches     = " << GetNBP() << "\n"
      "NumOfVertices       = " << GetGNV() << "\n"
      "NumOfElements       = " << GetGNE() << "\n"
      "NumOfBdrElements    = " << GetGNBE() << "\n"
      "NumOfDofs           = " << GetNTotalDof() << "\n"
      "NumOfActiveVertices = " << GetNV() << "\n"
      "NumOfActiveElems    = " << GetNE() << "\n"
      "NumOfActiveBdrElems = " << GetNBE() << "\n"
      "NumOfActiveDofs     = " << GetNDof() << '\n';
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      os << ' ' << i + 1 << ") ";
      knotVectors[i]->Print(os);
   }
   os << endl;
}

void NURBSExtension::PrintFunctions(const char *basename, int samples) const
{
   std::ofstream os;
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      std::ostringstream filename;
      filename << basename << "_" << i << ".dat";
      os.open(filename.str().c_str());
      knotVectors[i]->PrintFunctions(os,samples);
      os.close();
   }
}

void NURBSExtension::InitDofMap()
{
   master.SetSize(0);
   slave.SetSize(0);
   d_to_d.SetSize(0);
}

void NURBSExtension::ConnectBoundaries(Array<int> &bnds0, Array<int> &bnds1)
{
   bnds0.Copy(master);
   bnds1.Copy(slave);
   ConnectBoundaries();
}

void NURBSExtension::ConnectBoundaries()
{
   if (master.Size() != slave.Size())
   {
      mfem_error("NURBSExtension::ConnectBoundaries() boundary lists not of equal size");
   }
   if (master.Size() == 0 ) { return; }

   // Initialize d_to_d
   d_to_d.SetSize(NumOfDofs);
   for (int i = 0; i < NumOfDofs; i++) { d_to_d[i] = i; }

   // Connect
   for (int i = 0; i < master.Size(); i++)
   {
      int bnd0 = -1, bnd1 = -1;
      for (int b = 0; b < GetNBP(); b++)
      {
         if (master[i] == patchTopo->GetBdrAttribute(b)) { bnd0 = b; }
         if (slave[i]== patchTopo->GetBdrAttribute(b)) { bnd1  = b; }
      }
      MFEM_VERIFY(bnd0  != -1,"Bdr 0 not found");
      MFEM_VERIFY(bnd1  != -1,"Bdr 1 not found");

      if (Dimension() == 1)
      {
         ConnectBoundaries1D(bnd0, bnd1);
      }
      else if (Dimension() == 2)
      {
         ConnectBoundaries2D(bnd0, bnd1);
      }
      else
      {
         ConnectBoundaries3D(bnd0, bnd1);
      }
   }

   // Clean d_to_d
   Array<int> tmp(d_to_d.Size()+1);
   tmp = 0;

   for (int i = 0; i < d_to_d.Size(); i++)
   {
      tmp[d_to_d[i]] = 1;
   }

   int cnt = 0;
   for (int i = 0; i < tmp.Size(); i++)
   {
      if (tmp[i] == 1) { tmp[i] = cnt++; }
   }
   NumOfDofs = cnt;

   for (int i = 0; i < d_to_d.Size(); i++)
   {
      d_to_d[i] = tmp[d_to_d[i]];
   }

   // Finalize
   if (el_dof) { delete el_dof; }
   if (bel_dof) { delete bel_dof; }
   GenerateElementDofTable();
   GenerateBdrElementDofTable();
}

void NURBSExtension::ConnectBoundaries1D(int bnd0, int bnd1)
{
   NURBSPatchMap p2g0(this);
   NURBSPatchMap p2g1(this);

   int okv0[1],okv1[1];
   const KnotVector *kv0[1],*kv1[1];

   p2g0.SetBdrPatchDofMap(bnd0, kv0, okv0);
   p2g1.SetBdrPatchDofMap(bnd1, kv1, okv1);

   d_to_d[p2g0(0)] = d_to_d[p2g1(0)];
}

void NURBSExtension::ConnectBoundaries2D(int bnd0, int bnd1)
{
   NURBSPatchMap p2g0(this);
   NURBSPatchMap p2g1(this);

   int okv0[1],okv1[1];
   const KnotVector *kv0[1],*kv1[1];

   p2g0.SetBdrPatchDofMap(bnd0, kv0, okv0);
   p2g1.SetBdrPatchDofMap(bnd1, kv1, okv1);

   int nx = p2g0.nx();
   int nks0 = kv0[0]->GetNKS();

#ifdef MFEM_DEBUG
   bool compatible = true;
   if (p2g0.nx() != p2g1.nx()) { compatible = false; }
   if (kv0[0]->GetNKS() != kv1[0]->GetNKS()) { compatible = false; }
   if (kv0[0]->GetOrder() != kv1[0]->GetOrder()) { compatible = false; }

   if (!compatible)
   {
      mfem::out<<p2g0.nx()<<" "<<p2g1.nx()<<endl;
      mfem::out<<kv0[0]->GetNKS()<<" "<<kv1[0]->GetNKS()<<endl;
      mfem::out<<kv0[0]->GetOrder()<<" "<<kv1[0]->GetOrder()<<endl;
      mfem_error("NURBS boundaries not compatible");
   }
#endif

   for (int i = 0; i < nks0; i++)
   {
      if (kv0[0]->isElement(i))
      {
         if (!kv1[0]->isElement(i)) { mfem_error("isElement does not match"); }
         for (int ii = 0; ii <= kv0[0]->GetOrder(); ii++)
         {
            int ii0 = (okv0[0] >= 0) ? (i+ii) : (nx-i-ii);
            int ii1 = (okv1[0] >= 0) ? (i+ii) : (nx-i-ii);

            d_to_d[p2g0(ii0)] = d_to_d[p2g1(ii1)];
         }

      }
   }
}

void NURBSExtension::ConnectBoundaries3D(int bnd0, int bnd1)
{
   NURBSPatchMap p2g0(this);
   NURBSPatchMap p2g1(this);

   int okv0[2],okv1[2];
   const KnotVector *kv0[2],*kv1[2];

   p2g0.SetBdrPatchDofMap(bnd0, kv0, okv0);
   p2g1.SetBdrPatchDofMap(bnd1, kv1, okv1);

   int nx = p2g0.nx();
   int ny = p2g0.ny();

   int nks0 = kv0[0]->GetNKS();
   int nks1 = kv0[1]->GetNKS();

#ifdef MFEM_DEBUG
   bool compatible = true;
   if (p2g0.nx() != p2g1.nx()) { compatible = false; }
   if (p2g0.ny() != p2g1.ny()) { compatible = false; }

   if (kv0[0]->GetNKS() != kv1[0]->GetNKS()) { compatible = false; }
   if (kv0[1]->GetNKS() != kv1[1]->GetNKS()) { compatible = false; }

   if (kv0[0]->GetOrder() != kv1[0]->GetOrder()) { compatible = false; }
   if (kv0[1]->GetOrder() != kv1[1]->GetOrder()) { compatible = false; }

   if (!compatible)
   {
      mfem::out<<p2g0.nx()<<" "<<p2g1.nx()<<endl;
      mfem::out<<p2g0.ny()<<" "<<p2g1.ny()<<endl;

      mfem::out<<kv0[0]->GetNKS()<<" "<<kv1[0]->GetNKS()<<endl;
      mfem::out<<kv0[1]->GetNKS()<<" "<<kv1[1]->GetNKS()<<endl;

      mfem::out<<kv0[0]->GetOrder()<<" "<<kv1[0]->GetOrder()<<endl;
      mfem::out<<kv0[1]->GetOrder()<<" "<<kv1[1]->GetOrder()<<endl;
      mfem_error("NURBS boundaries not compatible");
   }
#endif

   for (int j = 0; j < nks1; j++)
   {
      if (kv0[1]->isElement(j))
      {
         if (!kv1[1]->isElement(j)) { mfem_error("isElement does not match #1"); }
         for (int i = 0; i < nks0; i++)
         {
            if (kv0[0]->isElement(i))
            {
               if (!kv1[0]->isElement(i)) { mfem_error("isElement does not match #0"); }
               for (int jj = 0; jj <= kv0[1]->GetOrder(); jj++)
               {
                  int jj0 = (okv0[1] >= 0) ? (j+jj) : (ny-j-jj);
                  int jj1 = (okv1[1] >= 0) ? (j+jj) : (ny-j-jj);

                  for (int ii = 0; ii <= kv0[0]->GetOrder(); ii++)
                  {
                     int ii0 = (okv0[0] >= 0) ? (i+ii) : (nx-i-ii);
                     int ii1 = (okv1[0] >= 0) ? (i+ii) : (nx-i-ii);

                     d_to_d[p2g0(ii0,jj0)] = d_to_d[p2g1(ii1,jj1)];
                  }
               }
            }
         }
      }
   }
}

void NURBSExtension::GenerateActiveVertices()
{
   int vert[8], nv, g_el, nx, ny, nz, dim = Dimension();

   NURBSPatchMap p2g(this);
   const KnotVector *kv[3];

   g_el = 0;
   activeVert.SetSize(GetGNV());
   activeVert = -1;
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchVertexMap(p, kv);

      nx = p2g.nx();
      ny = (dim >= 2) ? p2g.ny() : 1;
      nz = (dim == 3) ? p2g.nz() : 1;

      for (int k = 0; k < nz; k++)
      {
         for (int j = 0; j < ny; j++)
         {
            for (int i = 0; i < nx; i++)
            {
               if (activeElem[g_el])
               {
                  if (dim == 1)
                  {
                     vert[0] = p2g(i  );
                     vert[1] = p2g(i+1);
                     nv = 2;
                  }
                  else if (dim == 2)
                  {
                     vert[0] = p2g(i,  j  );
                     vert[1] = p2g(i+1,j  );
                     vert[2] = p2g(i+1,j+1);
                     vert[3] = p2g(i,  j+1);
                     nv = 4;
                  }
                  else
                  {
                     vert[0] = p2g(i,  j,  k);
                     vert[1] = p2g(i+1,j,  k);
                     vert[2] = p2g(i+1,j+1,k);
                     vert[3] = p2g(i,  j+1,k);

                     vert[4] = p2g(i,  j,  k+1);
                     vert[5] = p2g(i+1,j,  k+1);
                     vert[6] = p2g(i+1,j+1,k+1);
                     vert[7] = p2g(i,  j+1,k+1);
                     nv = 8;
                  }

                  for (int v = 0; v < nv; v++)
                  {
                     activeVert[vert[v]] = 1;
                  }
               }
               g_el++;
            }
         }
      }
   }

   NumOfActiveVertices = 0;
   for (int i = 0; i < GetGNV(); i++)
      if (activeVert[i] == 1)
      {
         activeVert[i] = NumOfActiveVertices++;
      }
}

void NURBSExtension::GenerateActiveBdrElems()
{
   int dim = Dimension();
   Array<KnotVector *> kv(dim);

   activeBdrElem.SetSize(GetGNBE());
   if (GetGNE() == GetNE())
   {
      activeBdrElem = true;
      NumOfActiveBdrElems = GetGNBE();
      return;
   }
   activeBdrElem = false;
   NumOfActiveBdrElems = 0;
   // the mesh will generate the actual boundary including boundary
   // elements that are not on boundary patches. we use this for
   // visualization of processor boundaries

   // TODO: generate actual boundary?
}


void NURBSExtension::MergeWeights(Mesh *mesh_array[], int num_pieces)
{
   Array<int> lelem_elem;

   for (int i = 0; i < num_pieces; i++)
   {
      NURBSExtension *lext = mesh_array[i]->NURBSext;

      lext->GetElementLocalToGlobal(lelem_elem);

      for (int lel = 0; lel < lext->GetNE(); lel++)
      {
         int gel = lelem_elem[lel];

         int nd = el_dof->RowSize(gel);
         int *gdofs = el_dof->GetRow(gel);
         int *ldofs = lext->el_dof->GetRow(lel);
         for (int j = 0; j < nd; j++)
         {
            weights(gdofs[j]) = lext->weights(ldofs[j]);
         }
      }
   }
}

void NURBSExtension::MergeGridFunctions(
   GridFunction *gf_array[], int num_pieces, GridFunction &merged)
{
   FiniteElementSpace *gfes = merged.FESpace();
   Array<int> lelem_elem, dofs;
   Vector lvec;

   for (int i = 0; i < num_pieces; i++)
   {
      FiniteElementSpace *lfes = gf_array[i]->FESpace();
      NURBSExtension *lext = lfes->GetMesh()->NURBSext;

      lext->GetElementLocalToGlobal(lelem_elem);

      for (int lel = 0; lel < lext->GetNE(); lel++)
      {
         lfes->GetElementVDofs(lel, dofs);
         gf_array[i]->GetSubVector(dofs, lvec);

         gfes->GetElementVDofs(lelem_elem[lel], dofs);
         merged.SetSubVector(dofs, lvec);
      }
   }
}

void NURBSExtension::CheckPatches()
{
   if (Dimension() == 1 ) { return; }

   Array<int> edges;
   Array<int> oedge;

   for (int p = 0; p < GetNP(); p++)
   {
      patchTopo->GetElementEdges(p, edges, oedge);

      for (int i = 0; i < edges.Size(); i++)
      {
         edges[i] = edge_to_ukv[edges[i]];
         if (oedge[i] < 0)
         {
            edges[i] = -1 - edges[i];
         }
      }

      if ((Dimension() == 2 &&
           (edges[0] != -1 - edges[2] || edges[1] != -1 - edges[3])) ||

          (Dimension() == 3 &&
           (edges[0] != edges[2] || edges[0] != edges[4] ||
            edges[0] != edges[6] || edges[1] != edges[3] ||
            edges[1] != edges[5] || edges[1] != edges[7] ||
            edges[8] != edges[9] || edges[8] != edges[10] ||
            edges[8] != edges[11])))
      {
         mfem::err << "NURBSExtension::CheckPatch (patch = " << p
                   << ")\n  Inconsistent edge-to-knotvector mapping!";
         mfem_error();
      }
   }
}

void NURBSExtension::CheckBdrPatches()
{
   Array<int> edges;
   Array<int> oedge;

   for (int p = 0; p < GetNBP(); p++)
   {
      patchTopo->GetBdrElementEdges(p, edges, oedge);

      for (int i = 0; i < edges.Size(); i++)
      {
         edges[i] = edge_to_ukv[edges[i]];
         if (oedge[i] < 0)
         {
            edges[i] = -1 - edges[i];
         }
      }

      if ((Dimension() == 2 && (edges[0] < 0)) ||
          (Dimension() == 3 && (edges[0] < 0 || edges[1] < 0)))
      {
         mfem::err << "NURBSExtension::CheckBdrPatch (boundary patch = "
                   << p << ") : Bad orientation!\n";
         mfem_error();
      }
   }
}

void NURBSExtension::CheckKVDirection(int p, Array <int> &kvdir)
{
   // patchTopo->GetElementEdges is not yet implemented for 1D
   MFEM_VERIFY(Dimension()>1, "1D not yet implemented.");

   kvdir.SetSize(Dimension());
   kvdir = 0;

   Array<int> patchvert, edges, orient, edgevert;

   patchTopo->GetElementVertices(p, patchvert);

   patchTopo->GetElementEdges(p, edges, orient);

   // Compare the vertices of the patches with the vertices of the knotvectors of knot2dge
   // Based on the match the orientation will be a 1 or a -1
   // -1: direction is flipped
   //  1: direction is not flipped

   for (int i = 0; i < edges.Size(); i++)
   {
      // First side
      patchTopo->GetEdgeVertices(edges[i], edgevert);
      if (edgevert[0] == patchvert[0]  && edgevert[1] == patchvert[1])
      {
         kvdir[0] = 1;
      }

      if (edgevert[0] == patchvert[1]  && edgevert[1] == patchvert[0])
      {
         kvdir[0] = -1;
      }

      // Second side
      if (edgevert[0] == patchvert[1]  && edgevert[1] == patchvert[2])
      {
         kvdir[1] = 1;
      }

      if (edgevert[0] == patchvert[2]  && edgevert[1] == patchvert[1])
      {
         kvdir[1] = -1;
      }
   }

   if (Dimension() == 3)
   {
      // Third side
      for (int i = 0; i < edges.Size(); i++)
      {
         patchTopo->GetEdgeVertices(edges[i], edgevert);

         if (edgevert[0] == patchvert[0]  && edgevert[1] == patchvert[4])
         {
            kvdir[2] = 1;
         }

         if (edgevert[0] == patchvert[4]  && edgevert[1] == patchvert[0])
         {
            kvdir[2] = -1;
         }
      }
   }

   MFEM_VERIFY(kvdir.Find(0) == -1, "Could not find direction of knotvector.");
}

void NURBSExtension::CreateComprehensiveKV()
{
   Array<int> edges, orient, kvdir;
   Array<int> e(Dimension());

   // 1D: comprehensive and unique KV are the same
   if (Dimension() == 1)
   {
      knotVectorsCompr.SetSize(GetNKV());
      for (int i = 0; i < GetNKV(); i++)
      {
         knotVectorsCompr[i] = new KnotVector(*(KnotVec(i)));
      }
      return;
   }
   else if (Dimension() == 2)
   {
      knotVectorsCompr.SetSize(GetNP()*Dimension());
      e[0] = 0;
      e[1] = 1;
   }
   else if (Dimension() == 3)
   {
      knotVectorsCompr.SetSize(GetNP()*Dimension());
      e[0] = 0;
      e[1] = 3;
      e[2] = 8;
   }

   for (int p = 0; p < GetNP(); p++)
   {
      CheckKVDirection(p, kvdir);

      patchTopo->GetElementEdges(p, edges, orient);

      for (int d = 0; d < Dimension(); d++)
      {
         // Indices in unique and comprehensive sets of the KnotVector
         int iun = edges[e[d]];
         int icomp = Dimension()*p+d;

         knotVectorsCompr[icomp] = new KnotVector(*(KnotVec(iun)));

         if (kvdir[d] == -1) {knotVectorsCompr[icomp]->Flip();}
      }
   }

   MFEM_VERIFY(ConsistentKVSets(), "Mismatch in KnotVectors");
}

void NURBSExtension::UpdateUniqueKV()
{
   Array<int> e(Dimension());

   // 1D: comprehensive and unique KV are the same
   if (Dimension() == 1)
   {
      for (int i = 0; i < GetNKV(); i++)
      {
         *(KnotVec(i)) = *(knotVectorsCompr[i]);
      }
      return;
   }
   else if (Dimension() == 2)
   {
      e[0] = 0;
      e[1] = 1;
   }
   else if (Dimension() == 3)
   {
      e[0] = 0;
      e[1] = 3;
      e[2] = 8;
   }

   for (int p = 0; p < GetNP(); p++)
   {
      Array<int> edges, orient, kvdir;

      patchTopo->GetElementEdges(p, edges, orient);
      CheckKVDirection(p, kvdir);

      for ( int d = 0; d < Dimension(); d++)
      {
         bool flip = false;
         if (kvdir[d] == -1) {flip = true;}

         // Indices in unique and comprehensive sets of the KnotVector
         int iun = edges[e[d]];
         int icomp = Dimension()*p+d;

         // Check if difference in order
         int o1 = KnotVec(iun)->GetOrder();
         int o2 = knotVectorsCompr[icomp]->GetOrder();
         int diffo = abs(o1 - o2);

         if (diffo)
         {
            // Update reduced set of knotvectors
            *(KnotVec(iun)) = *(knotVectorsCompr[icomp]);

            // Give correct direction to unique knotvector.
            if (flip) { KnotVec(iun)->Flip(); }
         }

         // Check if difference between knots
         Vector diffknot;

         if (flip) { knotVectorsCompr[icomp]->Flip(); }

         KnotVec(iun)->Difference(*(knotVectorsCompr[icomp]), diffknot);

         if (flip) { knotVectorsCompr[icomp]->Flip(); }

         if (diffknot.Size() > 0)
         {
            // Update reduced set of knotvectors
            *(KnotVec(iun)) = *(knotVectorsCompr[icomp]);

            // Give correct direction to unique knotvector.
            if (flip) {KnotVec(iun)->Flip();}
         }
      }
   }

   MFEM_VERIFY(ConsistentKVSets(), "Mismatch in KnotVectors");
}

bool NURBSExtension::ConsistentKVSets()
{
   // patchTopo->GetElementEdges is not yet implemented for 1D
   MFEM_VERIFY(Dimension() > 1, "1D not yet implemented.");

   Array<int> edges, orient, kvdir;
   Vector diff;

   Array<int> e(Dimension());

   e[0] = 0;

   if (Dimension() == 2)
   {
      e[1] = 1;
   }
   else if (Dimension() == 3)
   {
      e[1] = 3;
      e[2] = 8;
   }

   for (int p = 0; p < GetNP(); p++)
   {
      patchTopo->GetElementEdges(p, edges, orient);

      CheckKVDirection(p, kvdir);

      for (int d = 0; d < Dimension(); d++)
      {
         bool flip = false;
         if (kvdir[d] == -1) {flip = true;}

         // Indices in unique and comprehensive sets of the KnotVector
         int iun = edges[e[d]];
         int icomp = Dimension()*p+d;

         // Check if KnotVectors are of equal order
         int o1 = KnotVec(iun)->GetOrder();
         int o2 = knotVectorsCompr[icomp]->GetOrder();
         int diffo = abs(o1 - o2);

         if (diffo)
         {
            mfem::out << "\norder of knotVectorsCompr " << d << " of patch " << p;
            mfem::out << " does not agree with knotVectors " << KnotInd(iun) << "\n";
            return false;
         }

         // Check if Knotvectors have the same knots
         if (flip) {knotVectorsCompr[icomp]->Flip();}

         KnotVec(iun)->Difference(*(knotVectorsCompr[icomp]), diff);

         if (flip) {knotVectorsCompr[icomp]->Flip();}

         if (diff.Size() > 0)
         {
            mfem::out << "\nknotVectorsCompr " << d << " of patch " << p;
            mfem::out << " does not agree with knotVectors " << KnotInd(iun) << "\n";
            return false;
         }
      }
   }
   return true;
}

void NURBSExtension::GetPatchKnotVectors(int p, Array<KnotVector *> &kv)
{
   Array<int> edges, orient;

   kv.SetSize(Dimension());

   if (Dimension() == 1)
   {
      kv[0] = knotVectorsCompr[Dimension()*p];
   }
   else if (Dimension() == 2)
   {
      kv[0] = knotVectorsCompr[Dimension()*p];
      kv[1] = knotVectorsCompr[Dimension()*p + 1];
   }
   else
   {
      kv[0] = knotVectorsCompr[Dimension()*p];
      kv[1] = knotVectorsCompr[Dimension()*p + 1];
      kv[2] = knotVectorsCompr[Dimension()*p + 2];
   }
}

void NURBSExtension::GetPatchKnotVectors(int p, Array<const KnotVector *> &kv)
const
{
   Array<int> edges, orient;

   kv.SetSize(Dimension());

   if (Dimension() == 1)
   {
      kv[0] = knotVectorsCompr[Dimension()*p];
   }
   else if (Dimension() == 2)
   {
      kv[0] = knotVectorsCompr[Dimension()*p];
      kv[1] = knotVectorsCompr[Dimension()*p + 1];
   }
   else
   {
      kv[0] = knotVectorsCompr[Dimension()*p];
      kv[1] = knotVectorsCompr[Dimension()*p + 1];
      kv[2] = knotVectorsCompr[Dimension()*p + 2];
   }
}

void NURBSExtension::GetBdrPatchKnotVectors(int bp, Array<KnotVector *> &kv)
{
   Array<int> edges;
   Array<int> orient;

   kv.SetSize(Dimension() - 1);

   if (Dimension() == 2)
   {
      patchTopo->GetBdrElementEdges(bp, edges, orient);
      kv[0] = KnotVec(edges[0]);
   }
   else if (Dimension() == 3)
   {
      patchTopo->GetBdrElementEdges(bp, edges, orient);
      kv[0] = KnotVec(edges[0]);
      kv[1] = KnotVec(edges[1]);
   }
}

void NURBSExtension::GetBdrPatchKnotVectors(
   int bp, Array<const KnotVector *> &kv) const
{
   Array<int> edges;
   Array<int> orient;

   kv.SetSize(Dimension() - 1);

   if (Dimension() == 2)
   {
      patchTopo->GetBdrElementEdges(bp, edges, orient);
      kv[0] = KnotVec(edges[0]);
   }
   else if (Dimension() == 3)
   {
      patchTopo->GetBdrElementEdges(bp, edges, orient);
      kv[0] = KnotVec(edges[0]);
      kv[1] = KnotVec(edges[1]);
   }
}

void NURBSExtension::SetOrderFromOrders()
{
   MFEM_VERIFY(mOrders.Size() > 0, "");
   mOrder = mOrders[0];
   for (int i = 1; i < mOrders.Size(); i++)
   {
      if (mOrders[i] != mOrder)
      {
         mOrder = NURBSFECollection::VariableOrder;
         return;
      }
   }
}

void NURBSExtension::SetOrdersFromKnotVectors()
{
   mOrders.SetSize(NumOfKnotVectors);
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      mOrders[i] = knotVectors[i]->GetOrder();
   }
   SetOrderFromOrders();
}

void NURBSExtension::GenerateOffsets()
{
   int nv = patchTopo->GetNV();
   int ne = patchTopo->GetNEdges();
   int nf = patchTopo->GetNFaces();
   int np = patchTopo->GetNE();
   int meshCounter, spaceCounter, dim = Dimension();

   Array<int> edges;
   Array<int> orient;

   v_meshOffsets.SetSize(nv);
   e_meshOffsets.SetSize(ne);
   f_meshOffsets.SetSize(nf);
   p_meshOffsets.SetSize(np);

   v_spaceOffsets.SetSize(nv);
   e_spaceOffsets.SetSize(ne);
   f_spaceOffsets.SetSize(nf);
   p_spaceOffsets.SetSize(np);

   // Get vertex offsets
   for (meshCounter = 0; meshCounter < nv; meshCounter++)
   {
      v_meshOffsets[meshCounter]  = meshCounter;
      v_spaceOffsets[meshCounter] = meshCounter;
   }
   spaceCounter = meshCounter;

   // Get edge offsets
   for (int e = 0; e < ne; e++)
   {
      e_meshOffsets[e]  = meshCounter;
      e_spaceOffsets[e] = spaceCounter;
      meshCounter  += KnotVec(e)->GetNE() - 1;
      spaceCounter += KnotVec(e)->GetNCP() - 2;
   }

   // Get face offsets
   for (int f = 0; f < nf; f++)
   {
      f_meshOffsets[f]  = meshCounter;
      f_spaceOffsets[f] = spaceCounter;

      patchTopo->GetFaceEdges(f, edges, orient);

      meshCounter +=
         (KnotVec(edges[0])->GetNE() - 1) *
         (KnotVec(edges[1])->GetNE() - 1);
      spaceCounter +=
         (KnotVec(edges[0])->GetNCP() - 2) *
         (KnotVec(edges[1])->GetNCP() - 2);
   }

   // Get patch offsets
   for (int p = 0; p < np; p++)
   {
      p_meshOffsets[p]  = meshCounter;
      p_spaceOffsets[p] = spaceCounter;

      if (dim == 1)
      {
         meshCounter  += KnotVec(0)->GetNE() - 1;
         spaceCounter += KnotVec(0)->GetNCP() - 2;
      }
      else if (dim == 2)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         meshCounter +=
            (KnotVec(edges[0])->GetNE() - 1) *
            (KnotVec(edges[1])->GetNE() - 1);
         spaceCounter +=
            (KnotVec(edges[0])->GetNCP() - 2) *
            (KnotVec(edges[1])->GetNCP() - 2);
      }
      else
      {
         patchTopo->GetElementEdges(p, edges, orient);
         meshCounter +=
            (KnotVec(edges[0])->GetNE() - 1) *
            (KnotVec(edges[3])->GetNE() - 1) *
            (KnotVec(edges[8])->GetNE() - 1);
         spaceCounter +=
            (KnotVec(edges[0])->GetNCP() - 2) *
            (KnotVec(edges[3])->GetNCP() - 2) *
            (KnotVec(edges[8])->GetNCP() - 2);
      }
   }
   NumOfVertices = meshCounter;
   NumOfDofs     = spaceCounter;
}

void NURBSExtension::CountElements()
{
   int dim = Dimension();
   Array<const KnotVector *> kv(dim);

   NumOfElements = 0;
   for (int p = 0; p < GetNP(); p++)
   {
      GetPatchKnotVectors(p, kv);

      int ne = kv[0]->GetNE();
      for (int d = 1; d < dim; d++)
      {
         ne *= kv[d]->GetNE();
      }

      NumOfElements += ne;
   }
}

void NURBSExtension::CountBdrElements()
{
   int dim = Dimension() - 1;
   Array<KnotVector *> kv(dim);

   NumOfBdrElements = 0;
   for (int p = 0; p < GetNBP(); p++)
   {
      GetBdrPatchKnotVectors(p, kv);

      int ne = 1;
      for (int d = 0; d < dim; d++)
      {
         ne *= kv[d]->GetNE();
      }

      NumOfBdrElements += ne;
   }
}

void NURBSExtension::GetElementTopo(Array<Element *> &elements) const
{
   elements.SetSize(GetNE());

   if (Dimension() == 1)
   {
      Get1DElementTopo(elements);
   }
   else if (Dimension() == 2)
   {
      Get2DElementTopo(elements);
   }
   else
   {
      Get3DElementTopo(elements);
   }
}

void NURBSExtension::Get1DElementTopo(Array<Element *> &elements) const
{
   int el = 0;
   int eg = 0;
   int ind[2];
   NURBSPatchMap p2g(this);
   const KnotVector *kv[1];

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchVertexMap(p, kv);
      int nx = p2g.nx();

      int patch_attr = patchTopo->GetAttribute(p);

      for (int i = 0; i < nx; i++)
      {
         if (activeElem[eg])
         {
            ind[0] = activeVert[p2g(i)];
            ind[1] = activeVert[p2g(i+1)];

            elements[el] = new Segment(ind, patch_attr);
            el++;
         }
         eg++;
      }
   }
}

void NURBSExtension::Get2DElementTopo(Array<Element *> &elements) const
{
   int el = 0;
   int eg = 0;
   int ind[4];
   NURBSPatchMap p2g(this);
   const KnotVector *kv[2];

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchVertexMap(p, kv);
      int nx = p2g.nx();
      int ny = p2g.ny();

      int patch_attr = patchTopo->GetAttribute(p);

      for (int j = 0; j < ny; j++)
      {
         for (int i = 0; i < nx; i++)
         {
            if (activeElem[eg])
            {
               ind[0] = activeVert[p2g(i,  j  )];
               ind[1] = activeVert[p2g(i+1,j  )];
               ind[2] = activeVert[p2g(i+1,j+1)];
               ind[3] = activeVert[p2g(i,  j+1)];

               elements[el] = new Quadrilateral(ind, patch_attr);
               el++;
            }
            eg++;
         }
      }
   }
}

void NURBSExtension::Get3DElementTopo(Array<Element *> &elements) const
{
   int el = 0;
   int eg = 0;
   int ind[8];
   NURBSPatchMap p2g(this);
   const KnotVector *kv[3];

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchVertexMap(p, kv);
      int nx = p2g.nx();
      int ny = p2g.ny();
      int nz = p2g.nz();

      int patch_attr = patchTopo->GetAttribute(p);

      for (int k = 0; k < nz; k++)
      {
         for (int j = 0; j < ny; j++)
         {
            for (int i = 0; i < nx; i++)
            {
               if (activeElem[eg])
               {
                  ind[0] = activeVert[p2g(i,  j,  k)];
                  ind[1] = activeVert[p2g(i+1,j,  k)];
                  ind[2] = activeVert[p2g(i+1,j+1,k)];
                  ind[3] = activeVert[p2g(i,  j+1,k)];

                  ind[4] = activeVert[p2g(i,  j,  k+1)];
                  ind[5] = activeVert[p2g(i+1,j,  k+1)];
                  ind[6] = activeVert[p2g(i+1,j+1,k+1)];
                  ind[7] = activeVert[p2g(i,  j+1,k+1)];

                  elements[el] = new Hexahedron(ind, patch_attr);
                  el++;
               }
               eg++;
            }
         }
      }
   }
}

void NURBSExtension::GetBdrElementTopo(Array<Element *> &boundary) const
{
   boundary.SetSize(GetNBE());

   if (Dimension() == 1)
   {
      Get1DBdrElementTopo(boundary);
   }
   else if (Dimension() == 2)
   {
      Get2DBdrElementTopo(boundary);
   }
   else
   {
      Get3DBdrElementTopo(boundary);
   }
}

void NURBSExtension::Get1DBdrElementTopo(Array<Element *> &boundary) const
{
   int g_be, l_be;
   int ind[2], okv[1];
   NURBSPatchMap p2g(this);
   const KnotVector *kv[1];

   g_be = l_be = 0;
   for (int b = 0; b < GetNBP(); b++)
   {
      p2g.SetBdrPatchVertexMap(b, kv, okv);
      int bdr_patch_attr = patchTopo->GetBdrAttribute(b);

      if (activeBdrElem[g_be])
      {
         ind[0] = activeVert[p2g[0]];
         boundary[l_be] = new Point(ind, bdr_patch_attr);
         l_be++;
      }
      g_be++;
   }
}

void NURBSExtension::Get2DBdrElementTopo(Array<Element *> &boundary) const
{
   int g_be, l_be;
   int ind[2], okv[1];
   NURBSPatchMap p2g(this);
   const KnotVector *kv[1];

   g_be = l_be = 0;
   for (int b = 0; b < GetNBP(); b++)
   {
      p2g.SetBdrPatchVertexMap(b, kv, okv);
      int nx = p2g.nx();

      int bdr_patch_attr = patchTopo->GetBdrAttribute(b);

      for (int i = 0; i < nx; i++)
      {
         if (activeBdrElem[g_be])
         {
            int i_ = (okv[0] >= 0) ? i : (nx - 1 - i);
            ind[0] = activeVert[p2g[i_  ]];
            ind[1] = activeVert[p2g[i_+1]];

            boundary[l_be] = new Segment(ind, bdr_patch_attr);
            l_be++;
         }
         g_be++;
      }
   }
}

void NURBSExtension::Get3DBdrElementTopo(Array<Element *> &boundary) const
{
   int g_be, l_be;
   int ind[4], okv[2];
   NURBSPatchMap p2g(this);
   const KnotVector *kv[2];

   g_be = l_be = 0;
   for (int b = 0; b < GetNBP(); b++)
   {
      p2g.SetBdrPatchVertexMap(b, kv, okv);
      int nx = p2g.nx();
      int ny = p2g.ny();

      int bdr_patch_attr = patchTopo->GetBdrAttribute(b);

      for (int j = 0; j < ny; j++)
      {
         int j_ = (okv[1] >= 0) ? j : (ny - 1 - j);
         for (int i = 0; i < nx; i++)
         {
            if (activeBdrElem[g_be])
            {
               int i_ = (okv[0] >= 0) ? i : (nx - 1 - i);
               ind[0] = activeVert[p2g(i_,  j_  )];
               ind[1] = activeVert[p2g(i_+1,j_  )];
               ind[2] = activeVert[p2g(i_+1,j_+1)];
               ind[3] = activeVert[p2g(i_,  j_+1)];

               boundary[l_be] = new Quadrilateral(ind, bdr_patch_attr);
               l_be++;
            }
            g_be++;
         }
      }
   }
}

void NURBSExtension::GenerateElementDofTable()
{
   activeDof.SetSize(GetNTotalDof());
   activeDof = 0;

   if (Dimension() == 1)
   {
      Generate1DElementDofTable();
   }
   else if (Dimension() == 2)
   {
      Generate2DElementDofTable();
   }
   else
   {
      Generate3DElementDofTable();
   }

   SetPatchToElements();

   NumOfActiveDofs = 0;
   for (int d = 0; d < GetNTotalDof(); d++)
      if (activeDof[d])
      {
         NumOfActiveDofs++;
         activeDof[d] = NumOfActiveDofs;
      }

   int *dof = el_dof->GetJ();
   int ndof = el_dof->Size_of_connections();
   for (int i = 0; i < ndof; i++)
   {
      dof[i] = activeDof[dof[i]] - 1;
   }
}

void NURBSExtension::Generate1DElementDofTable()
{
   int el = 0;
   int eg = 0;
   const KnotVector *kv[2];
   NURBSPatchMap p2g(this);

   Array<Connection> el_dof_list;
   el_to_patch.SetSize(NumOfActiveElems);
   el_to_IJK.SetSize(NumOfActiveElems, 2);

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);

      // Load dofs
      const int ord0 = kv[0]->GetOrder();
      for (int i = 0; i < kv[0]->GetNKS(); i++)
      {
         if (kv[0]->isElement(i))
         {
            if (activeElem[eg])
            {
               Connection conn(el,0);
               for (int ii = 0; ii <= ord0; ii++)
               {
                  conn.to = DofMap(p2g(i+ii));
                  activeDof[conn.to] = 1;
                  el_dof_list.Append(conn);
               }
               el_to_patch[el] = p;
               el_to_IJK(el,0) = i;

               el++;
            }
            eg++;
         }
      }
   }
   // We must NOT sort el_dof_list in this case.
   el_dof = new Table(NumOfActiveElems, el_dof_list);
}

void NURBSExtension::Generate2DElementDofTable()
{
   int el = 0;
   int eg = 0;
   const KnotVector *kv[2];
   NURBSPatchMap p2g(this);

   Array<Connection> el_dof_list;
   el_to_patch.SetSize(NumOfActiveElems);
   el_to_IJK.SetSize(NumOfActiveElems, 2);

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);

      // Load dofs
      const int ord0 = kv[0]->GetOrder();
      const int ord1 = kv[1]->GetOrder();
      for (int j = 0; j < kv[1]->GetNKS(); j++)
      {
         if (kv[1]->isElement(j))
         {
            for (int i = 0; i < kv[0]->GetNKS(); i++)
            {
               if (kv[0]->isElement(i))
               {
                  if (activeElem[eg])
                  {
                     Connection conn(el,0);
                     for (int jj = 0; jj <= ord1; jj++)
                     {
                        for (int ii = 0; ii <= ord0; ii++)
                        {
                           conn.to = DofMap(p2g(i+ii,j+jj));
                           activeDof[conn.to] = 1;
                           el_dof_list.Append(conn);
                        }
                     }
                     el_to_patch[el] = p;
                     el_to_IJK(el,0) = i;
                     el_to_IJK(el,1) = j;

                     el++;
                  }
                  eg++;
               }
            }
         }
      }
   }
   // We must NOT sort el_dof_list in this case.
   el_dof = new Table(NumOfActiveElems, el_dof_list);
}

void NURBSExtension::Generate3DElementDofTable()
{
   int el = 0;
   int eg = 0;
   const KnotVector *kv[3];
   NURBSPatchMap p2g(this);

   Array<Connection> el_dof_list;
   el_to_patch.SetSize(NumOfActiveElems);
   el_to_IJK.SetSize(NumOfActiveElems, 3);

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);

      // Load dofs
      const int ord0 = kv[0]->GetOrder();
      const int ord1 = kv[1]->GetOrder();
      const int ord2 = kv[2]->GetOrder();
      for (int k = 0; k < kv[2]->GetNKS(); k++)
      {
         if (kv[2]->isElement(k))
         {
            for (int j = 0; j < kv[1]->GetNKS(); j++)
            {
               if (kv[1]->isElement(j))
               {
                  for (int i = 0; i < kv[0]->GetNKS(); i++)
                  {
                     if (kv[0]->isElement(i))
                     {
                        if (activeElem[eg])
                        {
                           Connection conn(el,0);
                           for (int kk = 0; kk <= ord2; kk++)
                           {
                              for (int jj = 0; jj <= ord1; jj++)
                              {
                                 for (int ii = 0; ii <= ord0; ii++)
                                 {
                                    conn.to = DofMap(p2g(i+ii, j+jj, k+kk));
                                    activeDof[conn.to] = 1;
                                    el_dof_list.Append(conn);
                                 }
                              }
                           }

                           el_to_patch[el] = p;
                           el_to_IJK(el,0) = i;
                           el_to_IJK(el,1) = j;
                           el_to_IJK(el,2) = k;

                           el++;
                        }
                        eg++;
                     }
                  }
               }
            }
         }
      }
   }
   // We must NOT sort el_dof_list in this case.
   el_dof = new Table(NumOfActiveElems, el_dof_list);
}

void NURBSExtension::GetPatchDofs(const int patch, Array<int> &dofs)
{
   const KnotVector *kv[3];
   NURBSPatchMap p2g(this);

   p2g.SetPatchDofMap(patch, kv);

   if (Dimension() == 1)
   {
      const int nx = kv[0]->GetNCP();
      dofs.SetSize(nx);

      for (int i=0; i<nx; ++i)
      {
         dofs[i] = DofMap(p2g(i));
      }
   }
   else if (Dimension() == 2)
   {
      const int nx = kv[0]->GetNCP();
      const int ny = kv[1]->GetNCP();
      dofs.SetSize(nx * ny);

      for (int j=0; j<ny; ++j)
         for (int i=0; i<nx; ++i)
         {
            dofs[i + (nx * j)] = DofMap(p2g(i, j));
         }
   }
   else if (Dimension() == 3)
   {
      const int nx = kv[0]->GetNCP();
      const int ny = kv[1]->GetNCP();
      const int nz = kv[2]->GetNCP();
      dofs.SetSize(nx * ny * nz);

      for (int k=0; k<nz; ++k)
         for (int j=0; j<ny; ++j)
            for (int i=0; i<nx; ++i)
            {
               dofs[i + (nx * (j + (k * ny)))] = DofMap(p2g(i, j, k));
            }
   }
   else
   {
      MFEM_ABORT("Only 1D/2D/3D supported currently in NURBSExtension::GetPatchDofs");
   }
}

void NURBSExtension::GenerateBdrElementDofTable()
{
   if (Dimension() == 1)
   {
      Generate1DBdrElementDofTable();
   }
   else if (Dimension() == 2)
   {
      Generate2DBdrElementDofTable();
   }
   else
   {
      Generate3DBdrElementDofTable();
   }

   SetPatchToBdrElements();

   int *dof = bel_dof->GetJ();
   int ndof = bel_dof->Size_of_connections();
   for (int i = 0; i < ndof; i++)
   {
      int idx = dof[i];
      if (idx < 0)
      {
         dof[i] = -1 - (activeDof[-1-idx] - 1);
         dof[i] = -activeDof[-1-idx];
      }
      else
      {
         dof[i] = activeDof[idx] - 1;
      }
   }
}

void NURBSExtension::Generate1DBdrElementDofTable()
{
   int gbe = 0;
   int lbe = 0, okv[1];
   const KnotVector *kv[1];
   NURBSPatchMap p2g(this);

   Array<Connection> bel_dof_list;
   bel_to_patch.SetSize(NumOfActiveBdrElems);
   bel_to_IJK.SetSize(NumOfActiveBdrElems, 1);

   for (int b = 0; b < GetNBP(); b++)
   {
      p2g.SetBdrPatchDofMap(b, kv, okv);
      // Load dofs
      if (activeBdrElem[gbe])
      {
         Connection conn(lbe,0);
         conn.to = DofMap(p2g[0]);
         bel_dof_list.Append(conn);
         bel_to_patch[lbe] = b;
         bel_to_IJK(lbe,0) = 0;
         lbe++;
      }
      gbe++;
   }
   // We must NOT sort bel_dof_list in this case.
   bel_dof = new Table(NumOfActiveBdrElems, bel_dof_list);
}

void NURBSExtension::Generate2DBdrElementDofTable()
{
   int gbe = 0;
   int lbe = 0, okv[1];
   const KnotVector *kv[1];
   NURBSPatchMap p2g(this);

   Array<Connection> bel_dof_list;
   bel_to_patch.SetSize(NumOfActiveBdrElems);
   bel_to_IJK.SetSize(NumOfActiveBdrElems, 1);

   for (int b = 0; b < GetNBP(); b++)
   {
      p2g.SetBdrPatchDofMap(b, kv, okv);
      const int nx = p2g.nx(); // NCP-1
      // Load dofs
      const int nks0 = kv[0]->GetNKS();
      const int ord0 = kv[0]->GetOrder();

      bool add_dofs = true;
      int  s = 1;

      if (mode == Mode::H_DIV)
      {
         int fn = patchTopo->GetBdrElementFaceIndex(b);
         if (ord0 == mOrders.Max()) { add_dofs = false; }
         if (fn == 0) { s = -1; }
         if (fn == 2) { s = -1; }
      }
      else if (mode == Mode::H_CURL)
      {
         if (ord0 == mOrders.Max()) { add_dofs = false; }
      }

      for (int i = 0; i < nks0; i++)
      {
         if (kv[0]->isElement(i))
         {
            if (activeBdrElem[gbe])
            {
               Connection conn(lbe,0);
               if (add_dofs)
               {
                  for (int ii = 0; ii <= ord0; ii++)
                  {
                     conn.to = DofMap(p2g[(okv[0] >= 0) ? (i+ii) : (nx-i-ii)]);
                     if (s == -1) { conn.to = -1 -conn.to; }
                     bel_dof_list.Append(conn);
                  }
               }
               bel_to_patch[lbe] = b;
               bel_to_IJK(lbe,0) = (okv[0] >= 0) ? i : (-1-i);
               lbe++;
            }
            gbe++;
         }
      }
   }
   // We must NOT sort bel_dof_list in this case.
   bel_dof = new Table(NumOfActiveBdrElems, bel_dof_list);
}


void NURBSExtension::Generate3DBdrElementDofTable()
{
   int gbe = 0;
   int lbe = 0, okv[2];
   const KnotVector *kv[2];
   NURBSPatchMap p2g(this);

   Array<Connection> bel_dof_list;
   bel_to_patch.SetSize(NumOfActiveBdrElems);
   bel_to_IJK.SetSize(NumOfActiveBdrElems, 2);

   for (int b = 0; b < GetNBP(); b++)
   {
      p2g.SetBdrPatchDofMap(b, kv, okv);
      const int nx = p2g.nx(); // NCP0-1
      const int ny = p2g.ny(); // NCP1-1

      // Load dofs
      const int nks0 = kv[0]->GetNKS();
      const int ord0 = kv[0]->GetOrder();
      const int nks1 = kv[1]->GetNKS();
      const int ord1 = kv[1]->GetOrder();

      // Check if dofs are actually defined on boundary
      bool add_dofs = true;
      int  s = 1;

      if (mode == Mode::H_DIV)
      {
         int fn = patchTopo->GetBdrElementFaceIndex(b);
         if (ord0 != ord1) { add_dofs = false; }
         if (fn == 4) { s = -1; }
         if (fn == 1) { s = -1; }
         if (fn == 0) { s = -1; }
      }
      else if (mode == Mode::H_CURL)
      {
         if (ord0 == ord1) { add_dofs = false; }
      }


      for (int j = 0; j < nks1; j++)
      {
         if (kv[1]->isElement(j))
         {
            for (int i = 0; i < nks0; i++)
            {
               if (kv[0]->isElement(i))
               {
                  if (activeBdrElem[gbe])
                  {
                     Connection conn(lbe,0);
                     if (add_dofs)
                     {
                        for (int jj = 0; jj <= ord1; jj++)
                        {
                           const int jj_ = (okv[1] >= 0) ? (j+jj) : (ny-j-jj);
                           for (int ii = 0; ii <= ord0; ii++)
                           {
                              const int ii_ = (okv[0] >= 0) ? (i+ii) : (nx-i-ii);
                              conn.to = DofMap(p2g(ii_, jj_));
                              if (s == -1) { conn.to = -1 -conn.to; }
                              bel_dof_list.Append(conn);
                           }
                        }
                     }
                     bel_to_patch[lbe] = b;
                     bel_to_IJK(lbe,0) = (okv[0] >= 0) ? i : (-1-i);
                     bel_to_IJK(lbe,1) = (okv[1] >= 0) ? j : (-1-j);
                     lbe++;
                  }
                  gbe++;
               }
            }
         }
      }
   }
   // We must NOT sort bel_dof_list in this case.
   bel_dof = new Table(NumOfActiveBdrElems, bel_dof_list);
}

void NURBSExtension::GetVertexLocalToGlobal(Array<int> &lvert_vert)
{
   lvert_vert.SetSize(GetNV());
   for (int gv = 0; gv < GetGNV(); gv++)
      if (activeVert[gv] >= 0)
      {
         lvert_vert[activeVert[gv]] = gv;
      }
}

void NURBSExtension::GetElementLocalToGlobal(Array<int> &lelem_elem)
{
   lelem_elem.SetSize(GetNE());
   for (int le = 0, ge = 0; ge < GetGNE(); ge++)
      if (activeElem[ge])
      {
         lelem_elem[le++] = ge;
      }
}

void NURBSExtension::LoadFE(int i, const FiniteElement *FE) const
{
   const NURBSFiniteElement *NURBSFE =
      dynamic_cast<const NURBSFiniteElement *>(FE);

   if (NURBSFE->GetElement() != i)
   {
      Array<int> dofs;
      NURBSFE->SetIJK(el_to_IJK.GetRow(i));
      if (el_to_patch[i] != NURBSFE->GetPatch())
      {
         GetPatchKnotVectors(el_to_patch[i], NURBSFE->KnotVectors());
         NURBSFE->SetPatch(el_to_patch[i]);
         NURBSFE->SetOrder();
      }
      el_dof->GetRow(i, dofs);
      weights.GetSubVector(dofs, NURBSFE->Weights());
      NURBSFE->SetElement(i);
   }
}

void NURBSExtension::LoadBE(int i, const FiniteElement *BE) const
{
   if (Dimension() == 1) { return; }

   const NURBSFiniteElement *NURBSFE =
      dynamic_cast<const NURBSFiniteElement *>(BE);

   if (NURBSFE->GetElement() != i)
   {
      Array<int> dofs;
      NURBSFE->SetIJK(bel_to_IJK.GetRow(i));
      if (bel_to_patch[i] != NURBSFE->GetPatch())
      {
         GetBdrPatchKnotVectors(bel_to_patch[i], NURBSFE->KnotVectors());
         NURBSFE->SetPatch(bel_to_patch[i]);
         NURBSFE->SetOrder();
      }
      bel_dof->GetRow(i, dofs);
      weights.GetSubVector(dofs, NURBSFE->Weights());
      NURBSFE->SetElement(i);
   }
}

void NURBSExtension::ConvertToPatches(const Vector &Nodes)
{
   delete el_dof;
   delete bel_dof;

   if (patches.Size() == 0)
   {
      GetPatchNets(Nodes, Dimension());
   }
}

void NURBSExtension::SetCoordsFromPatches(Vector &Nodes)
{
   if (patches.Size() == 0) { return; }

   SetSolutionVector(Nodes, Dimension());
   patches.SetSize(0);
}

void NURBSExtension::SetKnotsFromPatches()
{
   if (patches.Size() == 0)
   {
      mfem_error("NURBSExtension::SetKnotsFromPatches :"
                 " No patches available!");
   }

   Array<KnotVector *> kv;

   for (int p = 0; p < patches.Size(); p++)
   {
      GetPatchKnotVectors(p, kv);

      for (int i = 0; i < kv.Size(); i++)
      {
         *kv[i] = *patches[p]->GetKV(i);
      }
   }

   UpdateUniqueKV();
   SetOrdersFromKnotVectors();

   GenerateOffsets();
   CountElements();
   CountBdrElements();

   // all elements must be active
   NumOfActiveElems = NumOfElements;
   activeElem.SetSize(NumOfElements);
   activeElem = true;

   GenerateActiveVertices();
   InitDofMap();
   GenerateElementDofTable();
   GenerateActiveBdrElems();
   GenerateBdrElementDofTable();

   ConnectBoundaries();
}

void NURBSExtension::LoadSolution(std::istream &input, GridFunction &sol) const
{
   const FiniteElementSpace *fes = sol.FESpace();
   MFEM_VERIFY(fes->GetNURBSext() == this, "");

   sol.SetSize(fes->GetVSize());

   Array<const KnotVector *> kv(Dimension());
   NURBSPatchMap p2g(this);
   const int vdim = fes->GetVDim();

   for (int p = 0; p < GetNP(); p++)
   {
      skip_comment_lines(input, '#');

      p2g.SetPatchDofMap(p, kv);
      const int nx = kv[0]->GetNCP();
      const int ny = kv[1]->GetNCP();
      const int nz = (kv.Size() == 2) ? 1 : kv[2]->GetNCP();
      for (int k = 0; k < nz; k++)
      {
         for (int j = 0; j < ny; j++)
         {
            for (int i = 0; i < nx; i++)
            {
               const int ll = (kv.Size() == 2) ? p2g(i,j) : p2g(i,j,k);
               const int l  = DofMap(ll);
               for (int vd = 0; vd < vdim; vd++)
               {
                  input >> sol(fes->DofToVDof(l,vd));
               }
            }
         }
      }
   }
}

void NURBSExtension::PrintSolution(const GridFunction &sol, std::ostream &os)
const
{
   const FiniteElementSpace *fes = sol.FESpace();
   MFEM_VERIFY(fes->GetNURBSext() == this, "");

   Array<const KnotVector *> kv(Dimension());
   NURBSPatchMap p2g(this);
   const int vdim = fes->GetVDim();

   for (int p = 0; p < GetNP(); p++)
   {
      os << "\n# patch " << p << "\n\n";

      p2g.SetPatchDofMap(p, kv);
      const int nx = kv[0]->GetNCP();
      const int ny = kv[1]->GetNCP();
      const int nz = (kv.Size() == 2) ? 1 : kv[2]->GetNCP();
      for (int k = 0; k < nz; k++)
      {
         for (int j = 0; j < ny; j++)
         {
            for (int i = 0; i < nx; i++)
            {
               const int ll = (kv.Size() == 2) ? p2g(i,j) : p2g(i,j,k);
               const int l  = DofMap(ll);
               os << sol(fes->DofToVDof(l,0));
               for (int vd = 1; vd < vdim; vd++)
               {
                  os << ' ' << sol(fes->DofToVDof(l,vd));
               }
               os << '\n';
            }
         }
      }
   }
}

void NURBSExtension::DegreeElevate(int rel_degree, int degree)
{
   for (int p = 0; p < patches.Size(); p++)
   {
      for (int dir = 0; dir < patches[p]->GetNKV(); dir++)
      {
         int oldd = patches[p]->GetKV(dir)->GetOrder();
         int newd = std::min(oldd + rel_degree, degree);
         if (newd > oldd)
         {
            patches[p]->DegreeElevate(dir, newd - oldd);
         }
      }
   }
}

NURBSExtension* NURBSExtension::GetDivExtension(int component)
{
   // Smarter routine
   if (GetNP() > 1)
   {
      mfem_error("NURBSExtension::GetDivExtension currently "
                 "only works for single patch NURBS meshes ");
   }

   Array<int> newOrders  = GetOrders();
   newOrders[component] += 1;

   return new NURBSExtension(this, newOrders, Mode::H_DIV);
}

NURBSExtension* NURBSExtension::GetCurlExtension(int component)
{
   // Smarter routine
   if (GetNP() > 1)
   {
      mfem_error("NURBSExtension::GetCurlExtension currently "
                 "only works for single patch NURBS meshes ");
   }

   Array<int> newOrders  = GetOrders();
   for (int c = 0; c < newOrders.Size(); c++) { newOrders[c]++; }
   newOrders[component] -= 1;

   return new NURBSExtension(this, newOrders, Mode::H_CURL);
}


void NURBSExtension::UniformRefinement(Array<int> const& rf)
{
   for (int p = 0; p < patches.Size(); p++)
   {
      patches[p]->UniformRefinement(rf);
   }
}

void NURBSExtension::UniformRefinement(int rf)
{
   Array<int> rf_array(Dimension());
   rf_array = rf;
   UniformRefinement(rf_array);
}

void NURBSExtension::Coarsen(Array<int> const& cf, real_t tol)
{
   // First, mark all knot vectors on all patches as not coarse. This prevents
   // coarsening the same knot vector twice.
   for (int p = 0; p < patches.Size(); p++)
   {
      patches[p]->SetKnotVectorsCoarse(false);
   }

   for (int p = 0; p < patches.Size(); p++)
   {
      patches[p]->Coarsen(cf, tol);
   }
}

void NURBSExtension::Coarsen(int cf, real_t tol)
{
   Array<int> cf_array(Dimension());
   cf_array = cf;
   Coarsen(cf_array, tol);
}

void NURBSExtension::GetCoarseningFactors(Array<int> & f) const
{
   f.SetSize(0);
   for (auto patch : patches)
   {
      Array<int> pf;
      patch->GetCoarseningFactors(pf);
      if (f.Size() == 0)
      {
         f = pf; // Initialize
      }
      else
      {
         MFEM_VERIFY(f.Size() == pf.Size(), "");
         for (int i=0; i<f.Size(); ++i)
         {
            MFEM_VERIFY(f[i] == pf[i] || f[i] == 1 || pf[i] == 1,
                        "Inconsistent patch coarsening factors");
            if (f[i] == 1 && pf[i] != 1)
            {
               f[i] = pf[i];
            }
         }
      }
   }
}

void NURBSExtension::KnotInsert(Array<KnotVector *> &kv)
{
   Array<int> edges;
   Array<int> orient;
   Array<int> kvdir;

   Array<KnotVector *> pkv(Dimension());

   for (int p = 0; p < patches.Size(); p++)
   {
      if (Dimension()==1)
      {
         pkv[0] = kv[KnotInd(p)];
      }
      else if (Dimension()==2)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         pkv[0] = kv[KnotInd(edges[0])];
         pkv[1] = kv[KnotInd(edges[1])];
      }
      else if (Dimension()==3)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         pkv[0] = kv[KnotInd(edges[0])];
         pkv[1] = kv[KnotInd(edges[3])];
         pkv[2] = kv[KnotInd(edges[8])];
      }

      // Check whether inserted knots should be flipped before inserting.
      // Knotvectors are stored in a different array pkvc such that the original
      // knots which are inserted are not changed.
      // We need those knots for multiple patches so they have to remain original
      CheckKVDirection(p, kvdir);

      Array<KnotVector *> pkvc(Dimension());
      for (int d = 0; d < Dimension(); d++)
      {
         pkvc[d] = new KnotVector(*(pkv[d]));

         if (kvdir[d] == -1)
         {
            pkvc[d]->Flip();
         }
      }

      patches[p]->KnotInsert(pkvc);
      for (int d = 0; d < Dimension(); d++) { delete pkvc[d]; }
   }
}

void NURBSExtension::KnotInsert(Array<Vector *> &kv)
{
   Array<int> edges;
   Array<int> orient;
   Array<int> kvdir;

   Array<Vector *> pkv(Dimension());

   for (int p = 0; p < patches.Size(); p++)
   {
      if (Dimension()==1)
      {
         pkv[0] = kv[KnotInd(p)];
      }
      else if (Dimension()==2)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         pkv[0] = kv[KnotInd(edges[0])];
         pkv[1] = kv[KnotInd(edges[1])];
      }
      else if (Dimension()==3)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         pkv[0] = kv[KnotInd(edges[0])];
         pkv[1] = kv[KnotInd(edges[3])];
         pkv[2] = kv[KnotInd(edges[8])];
      }

      // Check whether inserted knots should be flipped before inserting.
      // Knotvectors are stored in a different array pkvc such that the original
      // knots which are inserted are not changed.
      CheckKVDirection(p, kvdir);

      Array<Vector *> pkvc(Dimension());
      for (int d = 0; d < Dimension(); d++)
      {
         pkvc[d] = new Vector(*(pkv[d]));

         if (kvdir[d] == -1)
         {
            // Find flip point, for knotvectors that do not have the domain [0:1]
            KnotVector *kva = knotVectorsCompr[Dimension()*p+d];
            real_t apb = (*kva)[0] + (*kva)[kva->Size()-1];

            // Flip vector
            int size = pkvc[d]->Size();
            int ns = static_cast<int>(ceil(size/2.0));
            for (int j = 0; j < ns; j++)
            {
               real_t tmp = apb - pkvc[d]->Elem(j);
               pkvc[d]->Elem(j) = apb - pkvc[d]->Elem(size-1-j);
               pkvc[d]->Elem(size-1-j) = tmp;
            }
         }
      }

      patches[p]->KnotInsert(pkvc);

      for (int i = 0; i < Dimension(); i++) { delete pkvc[i]; }
   }
}

void NURBSExtension::KnotRemove(Array<Vector *> &kv, real_t tol)
{
   Array<int> edges;
   Array<int> orient;
   Array<int> kvdir;

   Array<Vector *> pkv(Dimension());

   for (int p = 0; p < patches.Size(); p++)
   {
      if (Dimension()==1)
      {
         pkv[0] = kv[KnotInd(p)];
      }
      else if (Dimension()==2)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         pkv[0] = kv[KnotInd(edges[0])];
         pkv[1] = kv[KnotInd(edges[1])];
      }
      else if (Dimension()==3)
      {
         patchTopo->GetElementEdges(p, edges, orient);
         pkv[0] = kv[KnotInd(edges[0])];
         pkv[1] = kv[KnotInd(edges[3])];
         pkv[2] = kv[KnotInd(edges[8])];
      }

      // Check whether knots should be flipped before removing.
      CheckKVDirection(p, kvdir);

      Array<Vector *> pkvc(Dimension());
      for (int d = 0; d < Dimension(); d++)
      {
         pkvc[d] = new Vector(*(pkv[d]));

         if (kvdir[d] == -1)
         {
            // Find flip point, for knotvectors that do not have the domain [0:1]
            KnotVector *kva = knotVectorsCompr[Dimension()*p+d];
            real_t apb = (*kva)[0] + (*kva)[kva->Size()-1];

            // Flip vector
            int size = pkvc[d]->Size();
            int ns = static_cast<int>(ceil(size/2.0));
            for (int j = 0; j < ns; j++)
            {
               real_t tmp = apb - pkvc[d]->Elem(j);
               pkvc[d]->Elem(j) = apb - pkvc[d]->Elem(size-1-j);
               pkvc[d]->Elem(size-1-j) = tmp;
            }
         }
      }

      patches[p]->KnotRemove(pkvc, tol);

      for (int i = 0; i < Dimension(); i++) { delete pkvc[i]; }
   }
}

void NURBSExtension::GetPatchNets(const Vector &coords, int vdim)
{
   if (Dimension() == 1)
   {
      Get1DPatchNets(coords, vdim);
   }
   else if (Dimension() == 2)
   {
      Get2DPatchNets(coords, vdim);
   }
   else
   {
      Get3DPatchNets(coords, vdim);
   }
}

void NURBSExtension::Get1DPatchNets(const Vector &coords, int vdim)
{
   Array<const KnotVector *> kv(1);
   NURBSPatchMap p2g(this);

   patches.SetSize(GetNP());
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);
      patches[p] = new NURBSPatch(kv, vdim+1);
      NURBSPatch &Patch = *patches[p];

      for (int i = 0; i < kv[0]->GetNCP(); i++)
      {
         const int l = DofMap(p2g(i));
         for (int d = 0; d < vdim; d++)
         {
            Patch(i,d) = coords(l*vdim + d)*weights(l);
         }
         Patch(i,vdim) = weights(l);
      }
   }

}

void NURBSExtension::Get2DPatchNets(const Vector &coords, int vdim)
{
   Array<const KnotVector *> kv(2);
   NURBSPatchMap p2g(this);

   patches.SetSize(GetNP());
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);
      patches[p] = new NURBSPatch(kv, vdim+1);
      NURBSPatch &Patch = *patches[p];

      for (int j = 0; j < kv[1]->GetNCP(); j++)
      {
         for (int i = 0; i < kv[0]->GetNCP(); i++)
         {
            const int l = DofMap(p2g(i,j));
            for (int d = 0; d < vdim; d++)
            {
               Patch(i,j,d) = coords(l*vdim + d)*weights(l);
            }
            Patch(i,j,vdim) = weights(l);
         }
      }
   }
}

void NURBSExtension::Get3DPatchNets(const Vector &coords, int vdim)
{
   Array<const KnotVector *> kv(3);
   NURBSPatchMap p2g(this);

   patches.SetSize(GetNP());
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);
      patches[p] = new NURBSPatch(kv, vdim+1);
      NURBSPatch &Patch = *patches[p];

      for (int k = 0; k < kv[2]->GetNCP(); k++)
      {
         for (int j = 0; j < kv[1]->GetNCP(); j++)
         {
            for (int i = 0; i < kv[0]->GetNCP(); i++)
            {
               const int l = DofMap(p2g(i,j,k));
               for (int d = 0; d < vdim; d++)
               {
                  Patch(i,j,k,d) = coords(l*vdim + d)*weights(l);
               }
               Patch(i,j,k,vdim) = weights(l);
            }
         }
      }
   }
}

void NURBSExtension::SetSolutionVector(Vector &coords, int vdim)
{
   if (Dimension() == 1)
   {
      Set1DSolutionVector(coords, vdim);
   }
   else if (Dimension() == 2)
   {
      Set2DSolutionVector(coords, vdim);
   }
   else
   {
      Set3DSolutionVector(coords, vdim);
   }
}

void NURBSExtension::Set1DSolutionVector(Vector &coords, int vdim)
{
   Array<const KnotVector *> kv(1);
   NURBSPatchMap p2g(this);

   weights.SetSize(GetNDof());
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);
      NURBSPatch &patch = *patches[p];
      MFEM_ASSERT(vdim+1 == patch.GetNC(), "");

      for (int i = 0; i < kv[0]->GetNCP(); i++)
      {
         const int l = p2g(i);
         for (int d = 0; d < vdim; d++)
         {
            coords(l*vdim + d) = patch(i,d)/patch(i,vdim);
         }
         weights(l) = patch(i,vdim);
      }

      delete patches[p];
   }
}


void NURBSExtension::Set2DSolutionVector(Vector &coords, int vdim)
{
   Array<const KnotVector *> kv(2);
   NURBSPatchMap p2g(this);

   weights.SetSize(GetNDof());
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);
      NURBSPatch &patch = *patches[p];
      MFEM_ASSERT(vdim+1 == patch.GetNC(), "");

      for (int j = 0; j < kv[1]->GetNCP(); j++)
      {
         for (int i = 0; i < kv[0]->GetNCP(); i++)
         {
            const int l = p2g(i,j);
            for (int d = 0; d < vdim; d++)
            {
               coords(l*vdim + d) = patch(i,j,d)/patch(i,j,vdim);
            }
            weights(l) = patch(i,j,vdim);
         }
      }
      delete patches[p];
   }
}

void NURBSExtension::Set3DSolutionVector(Vector &coords, int vdim)
{
   Array<const KnotVector *> kv(3);
   NURBSPatchMap p2g(this);

   weights.SetSize(GetNDof());
   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);
      NURBSPatch &patch = *patches[p];
      MFEM_ASSERT(vdim+1 == patch.GetNC(), "");

      for (int k = 0; k < kv[2]->GetNCP(); k++)
      {
         for (int j = 0; j < kv[1]->GetNCP(); j++)
         {
            for (int i = 0; i < kv[0]->GetNCP(); i++)
            {
               const int l = p2g(i,j,k);
               for (int d = 0; d < vdim; d++)
               {
                  coords(l*vdim + d) = patch(i,j,k,d)/patch(i,j,k,vdim);
               }
               weights(l) = patch(i,j,k,vdim);
            }
         }
      }
      delete patches[p];
   }
}

void NURBSExtension::GetElementIJK(int elem, Array<int> & ijk)
{
   MFEM_VERIFY(ijk.Size() == el_to_IJK.NumCols(), "");
   el_to_IJK.GetRow(elem, ijk);
}

void NURBSExtension::GetPatches(Array<NURBSPatch*> &patches_copy)
{
   const int NP = patches.Size();
   patches_copy.SetSize(NP);
   for (int p = 0; p < NP; p++)
   {
      patches_copy[p] = new NURBSPatch(*GetPatch(p));
   }
}

void NURBSExtension::SetPatchToElements()
{
   const int np = GetNP();
   patch_to_el.resize(np);

   for (int e=0; e<el_to_patch.Size(); ++e)
   {
      patch_to_el[el_to_patch[e]].Append(e);
   }
}

void NURBSExtension::SetPatchToBdrElements()
{
   const int nbp = GetNBP();
   patch_to_bel.resize(nbp);

   for (int e=0; e<bel_to_patch.Size(); ++e)
   {
      patch_to_bel[bel_to_patch[e]].Append(e);
   }
}

const Array<int>& NURBSExtension::GetPatchElements(int patch)
{
   MFEM_ASSERT(patch_to_el.size() > 0, "patch_to_el not set");

   return patch_to_el[patch];
}

const Array<int>& NURBSExtension::GetPatchBdrElements(int patch)
{
   MFEM_ASSERT(patch_to_bel.size() > 0, "patch_to_el not set");

   return patch_to_bel[patch];
}

NURBSPatch::NURBSPatch(const KnotVector *kv0, const KnotVector *kv1, int dim_,
                       const real_t* control_points)
{
   kv.SetSize(2);
   kv[0] = new KnotVector(*kv0);
   kv[1] = new KnotVector(*kv1);
   init(dim_);
   memcpy(data, control_points, sizeof (real_t) * ni * nj * dim_);
}

NURBSPatch::NURBSPatch(const KnotVector *kv0, const KnotVector *kv1,
                       const KnotVector *kv2, int dim_,
                       const real_t* control_points)
{
   kv.SetSize(3);
   kv[0] = new KnotVector(*kv0);
   kv[1] = new KnotVector(*kv1);
   kv[2] = new KnotVector(*kv2);
   init(dim_);
   memcpy(data, control_points, sizeof (real_t) * ni * nj * nk * dim_);
}

NURBSPatch::NURBSPatch(Array<const KnotVector *> &kv_,  int dim_,
                       const real_t* control_points)
{
   kv.SetSize(kv_.Size());
   int n = dim_;
   for (int i = 0; i < kv.Size(); i++)
   {
      kv[i] = new KnotVector(*kv_[i]);
      n *= kv[i]->GetNCP();
   }
   init(dim_);
   memcpy(data, control_points, sizeof(real_t)*n);
}

#ifdef MFEM_USE_MPI
ParNURBSExtension::ParNURBSExtension(const ParNURBSExtension &orig)
   : NURBSExtension(orig),
     partitioning(orig.partitioning),
     gtopo(orig.gtopo),
     ldof_group(orig.ldof_group)
{
}

ParNURBSExtension::ParNURBSExtension(MPI_Comm comm, NURBSExtension *parent,
                                     const int *partitioning_,
                                     const Array<bool> &active_bel)
   : gtopo(comm)
{
   if (parent->NumOfActiveElems < parent->NumOfElements)
   {
      // SetActive (BuildGroups?) and the way the weights are copied
      // do not support this case
      mfem_error("ParNURBSExtension::ParNURBSExtension :\n"
                 " all elements in the parent must be active!");
   }

   patchTopo = parent->patchTopo;
   // steal ownership of patchTopo from the 'parent' NURBS extension
   if (!parent->own_topo)
   {
      mfem_error("ParNURBSExtension::ParNURBSExtension :\n"
                 "  parent does not own the patch topology!");
   }
   own_topo = true;
   parent->own_topo = false;

   parent->edge_to_ukv.Copy(edge_to_ukv);

   parent->GetOrders().Copy(mOrders);
   mOrder = parent->GetOrder();

   NumOfKnotVectors = parent->GetNKV();
   knotVectors.SetSize(NumOfKnotVectors);
   for (int i = 0; i < NumOfKnotVectors; i++)
   {
      knotVectors[i] = new KnotVector(*parent->GetKnotVector(i));
   }
   CreateComprehensiveKV();

   GenerateOffsets();
   CountElements();
   CountBdrElements();

   // copy 'partitioning_' to 'partitioning'
   partitioning.SetSize(GetGNE());
   for (int i = 0; i < GetGNE(); i++)
   {
      partitioning[i] = partitioning_[i];
   }
   SetActive(partitioning, active_bel);

   GenerateActiveVertices();
   GenerateElementDofTable();
   // GenerateActiveBdrElems(); // done by SetActive for now
   GenerateBdrElementDofTable();

   Table *serial_elem_dof = parent->GetElementDofTable();
   BuildGroups(partitioning, *serial_elem_dof);

   weights.SetSize(GetNDof());
   // copy weights from parent
   for (int gel = 0, lel = 0; gel < GetGNE(); gel++)
   {
      if (activeElem[gel])
      {
         int  ndofs = el_dof->RowSize(lel);
         int *ldofs = el_dof->GetRow(lel);
         int *gdofs = serial_elem_dof->GetRow(gel);
         for (int i = 0; i < ndofs; i++)
         {
            weights(ldofs[i]) = parent->weights(gdofs[i]);
         }
         lel++;
      }
   }
}

ParNURBSExtension::ParNURBSExtension(NURBSExtension *parent,
                                     const ParNURBSExtension *par_parent)
   : gtopo(par_parent->gtopo.GetComm())
{
   // steal all data from parent
   mOrder = parent->mOrder;
   Swap(mOrders, parent->mOrders);

   patchTopo = parent->patchTopo;
   own_topo = parent->own_topo;
   parent->own_topo = false;

   Swap(edge_to_ukv, parent->edge_to_ukv);

   NumOfKnotVectors = parent->NumOfKnotVectors;
   Swap(knotVectors, parent->knotVectors);
   Swap(knotVectorsCompr, parent->knotVectorsCompr);

   NumOfVertices    = parent->NumOfVertices;
   NumOfElements    = parent->NumOfElements;
   NumOfBdrElements = parent->NumOfBdrElements;
   NumOfDofs        = parent->NumOfDofs;

   Swap(v_meshOffsets, parent->v_meshOffsets);
   Swap(e_meshOffsets, parent->e_meshOffsets);
   Swap(f_meshOffsets, parent->f_meshOffsets);
   Swap(p_meshOffsets, parent->p_meshOffsets);

   Swap(v_spaceOffsets, parent->v_spaceOffsets);
   Swap(e_spaceOffsets, parent->e_spaceOffsets);
   Swap(f_spaceOffsets, parent->f_spaceOffsets);
   Swap(p_spaceOffsets, parent->p_spaceOffsets);

   Swap(d_to_d, parent->d_to_d);
   Swap(master, parent->master);
   Swap(slave,  parent->slave);

   NumOfActiveVertices = parent->NumOfActiveVertices;
   NumOfActiveElems    = parent->NumOfActiveElems;
   NumOfActiveBdrElems = parent->NumOfActiveBdrElems;
   NumOfActiveDofs     = parent->NumOfActiveDofs;

   Swap(activeVert, parent->activeVert);
   Swap(activeElem, parent->activeElem);
   Swap(activeBdrElem, parent->activeBdrElem);
   Swap(activeDof, parent->activeDof);

   el_dof  = parent->el_dof;
   bel_dof = parent->bel_dof;
   parent->el_dof = parent->bel_dof = NULL;

   Swap(el_to_patch, parent->el_to_patch);
   Swap(bel_to_patch, parent->bel_to_patch);
   Swap(el_to_IJK, parent->el_to_IJK);
   Swap(bel_to_IJK, parent->bel_to_IJK);

   Swap(weights, parent->weights);
   MFEM_VERIFY(!parent->HavePatches(), "");

   delete parent;

   MFEM_VERIFY(par_parent->partitioning,
               "parent ParNURBSExtension has no partitioning!");

   // Support for the case when 'parent' is not a local NURBSExtension, i.e.
   // NumOfActiveElems is not the same as in 'par_parent'. In that case, we
   // assume 'parent' is a global NURBSExtension, i.e. all elements are active.
   bool extract_weights = false;
   if (NumOfActiveElems != par_parent->NumOfActiveElems)
   {
      MFEM_ASSERT(NumOfActiveElems == NumOfElements, "internal error");

      SetActive(par_parent->partitioning, par_parent->activeBdrElem);
      GenerateActiveVertices();
      delete el_dof;
      el_to_patch.DeleteAll();
      el_to_IJK.DeleteAll();
      GenerateElementDofTable();
      // GenerateActiveBdrElems(); // done by SetActive for now
      delete bel_dof;
      bel_to_patch.DeleteAll();
      bel_to_IJK.DeleteAll();
      GenerateBdrElementDofTable();
      extract_weights = true;
   }

   Table *glob_elem_dof = GetGlobalElementDofTable();
   BuildGroups(par_parent->partitioning, *glob_elem_dof);
   if (extract_weights)
   {
      Vector glob_weights;
      Swap(weights, glob_weights);
      weights.SetSize(GetNDof());
      // Copy the local 'weights' from the 'glob_weights'.
      // Assumption: the local element ids follow the global ordering.
      for (int gel = 0, lel = 0; gel < GetGNE(); gel++)
      {
         if (activeElem[gel])
         {
            int  ndofs = el_dof->RowSize(lel);
            int *ldofs = el_dof->GetRow(lel);
            int *gdofs = glob_elem_dof->GetRow(gel);
            for (int i = 0; i < ndofs; i++)
            {
               weights(ldofs[i]) = glob_weights(gdofs[i]);
            }
            lel++;
         }
      }
   }
   delete glob_elem_dof;
}

Table *ParNURBSExtension::GetGlobalElementDofTable()
{
   if (Dimension() == 1)
   {
      return Get1DGlobalElementDofTable();
   }
   else if (Dimension() == 2)
   {
      return Get2DGlobalElementDofTable();
   }
   else
   {
      return Get3DGlobalElementDofTable();
   }
}

Table *ParNURBSExtension::Get1DGlobalElementDofTable()
{
   int el = 0;
   const KnotVector *kv[1];
   NURBSPatchMap p2g(this);
   Array<Connection> gel_dof_list;

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);

      // Load dofs
      const int ord0 = kv[0]->GetOrder();

      for (int i = 0; i < kv[0]->GetNKS(); i++)
      {
         if (kv[0]->isElement(i))
         {
            Connection conn(el,0);
            for (int ii = 0; ii <= ord0; ii++)
            {
               conn.to = DofMap(p2g(i+ii));
               gel_dof_list.Append(conn);
            }
            el++;
         }
      }
   }
   // We must NOT sort gel_dof_list in this case.
   return (new Table(GetGNE(), gel_dof_list));
}

Table *ParNURBSExtension::Get2DGlobalElementDofTable()
{
   int el = 0;
   const KnotVector *kv[2];
   NURBSPatchMap p2g(this);
   Array<Connection> gel_dof_list;

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);

      // Load dofs
      const int ord0 = kv[0]->GetOrder();
      const int ord1 = kv[1]->GetOrder();
      for (int j = 0; j < kv[1]->GetNKS(); j++)
      {
         if (kv[1]->isElement(j))
         {
            for (int i = 0; i < kv[0]->GetNKS(); i++)
            {
               if (kv[0]->isElement(i))
               {
                  Connection conn(el,0);
                  for (int jj = 0; jj <= ord1; jj++)
                  {
                     for (int ii = 0; ii <= ord0; ii++)
                     {
                        conn.to = DofMap(p2g(i+ii,j+jj));
                        gel_dof_list.Append(conn);
                     }
                  }
                  el++;
               }
            }
         }
      }
   }
   // We must NOT sort gel_dof_list in this case.
   return (new Table(GetGNE(), gel_dof_list));
}

Table *ParNURBSExtension::Get3DGlobalElementDofTable()
{
   int el = 0;
   const KnotVector *kv[3];
   NURBSPatchMap p2g(this);
   Array<Connection> gel_dof_list;

   for (int p = 0; p < GetNP(); p++)
   {
      p2g.SetPatchDofMap(p, kv);

      // Load dofs
      const int ord0 = kv[0]->GetOrder();
      const int ord1 = kv[1]->GetOrder();
      const int ord2 = kv[2]->GetOrder();
      for (int k = 0; k < kv[2]->GetNKS(); k++)
      {
         if (kv[2]->isElement(k))
         {
            for (int j = 0; j < kv[1]->GetNKS(); j++)
            {
               if (kv[1]->isElement(j))
               {
                  for (int i = 0; i < kv[0]->GetNKS(); i++)
                  {
                     if (kv[0]->isElement(i))
                     {
                        Connection conn(el,0);
                        for (int kk = 0; kk <= ord2; kk++)
                        {
                           for (int jj = 0; jj <= ord1; jj++)
                           {
                              for (int ii = 0; ii <= ord0; ii++)
                              {
                                 conn.to = DofMap(p2g(i+ii,j+jj,k+kk));
                                 gel_dof_list.Append(conn);
                              }
                           }
                        }
                        el++;
                     }
                  }
               }
            }
         }
      }
   }
   // We must NOT sort gel_dof_list in this case.
   return (new Table(GetGNE(), gel_dof_list));
}

void ParNURBSExtension::SetActive(const int *partition,
                                  const Array<bool> &active_bel)
{
   activeElem.SetSize(GetGNE());
   activeElem = false;
   NumOfActiveElems = 0;
   const int MyRank = gtopo.MyRank();
   for (int i = 0; i < GetGNE(); i++)
      if (partition[i] == MyRank)
      {
         activeElem[i] = true;
         NumOfActiveElems++;
      }

   active_bel.Copy(activeBdrElem);
   NumOfActiveBdrElems = 0;
   for (int i = 0; i < GetGNBE(); i++)
      if (activeBdrElem[i])
      {
         NumOfActiveBdrElems++;
      }
}

void ParNURBSExtension::BuildGroups(const int *partition,
                                    const Table &elem_dof)
{
   Table dof_proc;

   ListOfIntegerSets  groups;
   IntegerSet         group;

   Transpose(elem_dof, dof_proc); // dof_proc is dof_elem
   // convert elements to processors
   for (int i = 0; i < dof_proc.Size_of_connections(); i++)
   {
      dof_proc.GetJ()[i] = partition[dof_proc.GetJ()[i]];
   }

   // the first group is the local one
   int MyRank = gtopo.MyRank();
   group.Recreate(1, &MyRank);
   groups.Insert(group);

   int dof = 0;
   ldof_group.SetSize(GetNDof());
   for (int d = 0; d < GetNTotalDof(); d++)
      if (activeDof[d])
      {
         group.Recreate(dof_proc.RowSize(d), dof_proc.GetRow(d));
         ldof_group[dof] = groups.Insert(group);

         dof++;
      }

   gtopo.Create(groups, 1822);
}
#endif // MFEM_USE_MPI


void NURBSPatchMap::GetPatchKnotVectors(int p, const KnotVector *kv[])
{
   Ext->patchTopo->GetElementVertices(p, verts);

   if (Ext->Dimension() == 1)
   {
      kv[0] = Ext->knotVectorsCompr[Ext->Dimension()*p];
   }
   else if (Ext->Dimension() == 2)
   {
      Ext->patchTopo->GetElementEdges(p, edges, oedge);

      kv[0] = Ext->knotVectorsCompr[Ext->Dimension()*p];
      kv[1] = Ext->knotVectorsCompr[Ext->Dimension()*p + 1];
   }
   else if (Ext->Dimension() == 3)
   {
      Ext->patchTopo->GetElementEdges(p, edges, oedge);
      Ext->patchTopo->GetElementFaces(p, faces, oface);

      kv[0] = Ext->knotVectorsCompr[Ext->Dimension()*p];
      kv[1] = Ext->knotVectorsCompr[Ext->Dimension()*p + 1];
      kv[2] = Ext->knotVectorsCompr[Ext->Dimension()*p + 2];
   }
   opatch = 0;
}

void NURBSPatchMap::GetBdrPatchKnotVectors(int p, const KnotVector *kv[],
                                           int *okv)
{
   Ext->patchTopo->GetBdrElementVertices(p, verts);

   if (Ext->Dimension() == 2)
   {
      Ext->patchTopo->GetBdrElementEdges(p, edges, oedge);
      kv[0] = Ext->KnotVec(edges[0], oedge[0], &okv[0]);
      opatch = oedge[0];
   }
   else if (Ext->Dimension() == 3)
   {
      faces.SetSize(1);
      Ext->patchTopo->GetBdrElementEdges(p, edges, oedge);
      Ext->patchTopo->GetBdrElementFace(p, &faces[0], &opatch);

      kv[0] = Ext->KnotVec(edges[0], oedge[0], &okv[0]);
      kv[1] = Ext->KnotVec(edges[1], oedge[1], &okv[1]);
   }

}

void NURBSPatchMap::SetPatchVertexMap(int p, const KnotVector *kv[])
{
   GetPatchKnotVectors(p, kv);

   I = kv[0]->GetNE() - 1;

   for (int i = 0; i < verts.Size(); i++)
   {
      verts[i] = Ext->v_meshOffsets[verts[i]];
   }

   if (Ext->Dimension() >= 2)
   {
      J = kv[1]->GetNE() - 1;
      for (int i = 0; i < edges.Size(); i++)
      {
         edges[i] = Ext->e_meshOffsets[edges[i]];
      }
   }
   if (Ext->Dimension() == 3)
   {
      K = kv[2]->GetNE() - 1;

      for (int i = 0; i < faces.Size(); i++)
      {
         faces[i] = Ext->f_meshOffsets[faces[i]];
      }
   }

   pOffset = Ext->p_meshOffsets[p];
}

void NURBSPatchMap::SetPatchDofMap(int p, const KnotVector *kv[])
{
   GetPatchKnotVectors(p, kv);

   I = kv[0]->GetNCP() - 2;

   for (int i = 0; i < verts.Size(); i++)
   {
      verts[i] = Ext->v_spaceOffsets[verts[i]];
   }
   if (Ext->Dimension() >= 2)
   {
      J = kv[1]->GetNCP() - 2;
      for (int i = 0; i < edges.Size(); i++)
      {
         edges[i] = Ext->e_spaceOffsets[edges[i]];
      }
   }
   if (Ext->Dimension() == 3)
   {
      K = kv[2]->GetNCP() - 2;

      for (int i = 0; i < faces.Size(); i++)
      {
         faces[i] = Ext->f_spaceOffsets[faces[i]];
      }
   }

   pOffset = Ext->p_spaceOffsets[p];
}

void NURBSPatchMap::SetBdrPatchVertexMap(int p, const KnotVector *kv[],
                                         int *okv)
{
   GetBdrPatchKnotVectors(p, kv, okv);

   for (int i = 0; i < verts.Size(); i++)
   {
      verts[i] = Ext->v_meshOffsets[verts[i]];
   }

   if (Ext->Dimension() == 1)
   {
      I = 0;
   }
   else if (Ext->Dimension() == 2)
   {
      I = kv[0]->GetNE() - 1;
      pOffset = Ext->e_meshOffsets[edges[0]];
   }
   else if (Ext->Dimension() == 3)
   {
      I = kv[0]->GetNE() - 1;
      J = kv[1]->GetNE() - 1;

      for (int i = 0; i < edges.Size(); i++)
      {
         edges[i] = Ext->e_meshOffsets[edges[i]];
      }

      pOffset = Ext->f_meshOffsets[faces[0]];
   }
}

void NURBSPatchMap::SetBdrPatchDofMap(int p, const KnotVector *kv[],  int *okv)
{
   GetBdrPatchKnotVectors(p, kv, okv);

   for (int i = 0; i < verts.Size(); i++)
   {
      verts[i] = Ext->v_spaceOffsets[verts[i]];
   }

   if (Ext->Dimension() == 1)
   {
      I = 0;
   }
   else if (Ext->Dimension() == 2)
   {
      I = kv[0]->GetNCP() - 2;
      pOffset = Ext->e_spaceOffsets[edges[0]];
   }
   else if (Ext->Dimension() == 3)
   {
      I = kv[0]->GetNCP() - 2;
      J = kv[1]->GetNCP() - 2;

      for (int i = 0; i < edges.Size(); i++)
      {
         edges[i] = Ext->e_spaceOffsets[edges[i]];
      }

      pOffset = Ext->f_spaceOffsets[faces[0]];
   }
}

}
