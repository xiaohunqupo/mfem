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

#include "unit_tests.hpp"
#include "mfem.hpp"

namespace mfem
{

constexpr real_t EPS = 1.e-12;

TEST_CASE("FormLinearSystem", "[FormLinearSystem]")
{
   for (int dim = 2; dim <=3; ++dim)
   {
      for (int ne = 1; ne <= 4; ++ne)
      {
         const int n_elements = static_cast<int>(std::pow(ne, dim));
         CAPTURE(dim, n_elements);
         for (int order = 1; order <= 3; ++order)
         {
            Mesh mesh;
            if (dim == 2)
            {
               mesh = Mesh::MakeCartesian2D(
                         ne, ne, Element::QUADRILATERAL, 1, 1.0, 1.0);
            }
            else
            {
               mesh = Mesh::MakeCartesian3D(
                         ne, ne, ne, Element::HEXAHEDRON, 1.0, 1.0, 1.0);
            }
            FiniteElementCollection *fec = new H1_FECollection(order, dim);
            FiniteElementSpace fes(&mesh, fec);

            Array<int> ess_tdof_list;
            Array<int> ess_bdr(mesh.bdr_attributes.Max());
            ess_bdr = 1;
            fes.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

            ConstantCoefficient one(1.0);
            GridFunction x0(&fes), x1(&fes), b(&fes);
            Vector B[2], X[2];

            OperatorPtr A_pa, A_fa;
            BilinearForm pa(&fes), fa(&fes);

            x0 = 0.0;
            b = 1.0;
            pa.SetAssemblyLevel(AssemblyLevel::PARTIAL);
            pa.AddDomainIntegrator(new DiffusionIntegrator(one));
            pa.Assemble();
            pa.FormLinearSystem(ess_tdof_list, x0, b, A_pa, X[0], B[0]);
            OperatorJacobiSmoother M_pa(pa, ess_tdof_list);
            PCG(*A_pa, M_pa, B[0], X[0], 0, 1000, EPS*EPS, 0.0);
            pa.RecoverFEMSolution(X[0], b, x0);

            x1 = 0.0;
            b = 1.0;
            fa.AddDomainIntegrator(new DiffusionIntegrator(one));
            fa.Assemble();
            fa.FormLinearSystem(ess_tdof_list, x1, b, A_fa, X[1], B[1]);
            DSmoother M_fa((SparseMatrix&)(*A_fa));
            PCG(*A_fa, M_fa, B[1], X[1], 0, 1000, EPS*EPS, 0.0);
            fa.RecoverFEMSolution(X[1], b, x1);

            x0 -= x1;
            real_t error = x0.Norml2();
            CAPTURE(error, order);
            REQUIRE(x0.Norml2() == MFEM_Approx(0.0, 1e2*EPS));

            delete fec;
         }
      }
   }
}

#ifdef MFEM_USE_MPI

TEST_CASE("ParallelFormLinearSystem", "[Parallel], [ParallelFormLinearSystem]")
{
   for (int dim = 2; dim <= 3; ++dim)
   {
      for (int ne = 4; ne <= 5; ++ne)
      {
         const int n_elements = static_cast<int>(std::pow(ne, dim));
         CAPTURE(dim, n_elements);
         for (int order = 1; order <= 3; ++order)
         {
            Mesh mesh;
            if (dim == 2)
            {
               mesh = Mesh::MakeCartesian2D(
                         ne, ne, Element::QUADRILATERAL, 1, 1.0, 1.0);
            }
            else
            {
               mesh = Mesh::MakeCartesian3D(
                         ne, ne, ne, Element::HEXAHEDRON, 1.0, 1.0, 1.0);
            }
            ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, mesh);
            mesh.Clear();

            FiniteElementCollection *fec = new H1_FECollection(order, dim);
            ParFiniteElementSpace fes(pmesh, fec);

            Array<int> ess_tdof_list;
            Array<int> ess_bdr(pmesh->bdr_attributes.Max());
            ess_bdr = 1;
            fes.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

            ConstantCoefficient one(1.0);
            ParGridFunction x0(&fes), x1(&fes), b(&fes);
            Vector B[2], X[2];

            OperatorPtr A_pa, A_fa;
            ParBilinearForm pa(&fes), fa(&fes);

            x0 = 0.0;
            b = 1.0;
            pa.SetAssemblyLevel(AssemblyLevel::PARTIAL);
            pa.AddDomainIntegrator(new DiffusionIntegrator(one));
            pa.Assemble();
            pa.FormLinearSystem(ess_tdof_list, x0, b, A_pa, X[0], B[0]);
            Solver *M_pa = new OperatorJacobiSmoother(pa, ess_tdof_list);
            CGSolver cg_pa(MPI_COMM_WORLD);
            cg_pa.SetRelTol(EPS);
            cg_pa.SetMaxIter(1000);
            cg_pa.SetPrintLevel(0);
            cg_pa.SetPreconditioner(*M_pa);
            cg_pa.SetOperator(*A_pa);
            cg_pa.Mult(B[0], X[0]);
            delete M_pa;
            pa.RecoverFEMSolution(X[0], b, x0);

            x1 = 0.0;
            b = 1.0;
            fa.AddDomainIntegrator(new DiffusionIntegrator(one));
            fa.Assemble();
            fa.FormLinearSystem(ess_tdof_list, x1, b, A_fa, X[1], B[1]);
            HypreSmoother M_fa;
            M_fa.SetType(HypreSmoother::Jacobi);
            CGSolver cg_fa(MPI_COMM_WORLD);
            cg_fa.SetRelTol(EPS);
            cg_fa.SetMaxIter(1000);
            cg_fa.SetPrintLevel(0);
            cg_fa.SetPreconditioner(M_fa);
            cg_fa.SetOperator(*A_fa);
            cg_fa.Mult(B[1], X[1]);
            fa.RecoverFEMSolution(X[1], b, x1);

            x0 -= x1;
            real_t error = x0.Norml2();
            CAPTURE(order, error);
            REQUIRE(x0.Norml2() == MFEM_Approx(0.0, 2e2*EPS));

            delete pmesh;
            delete fec;
         }
      }
   }
}

TEST_CASE("HypreParMatrixBlocksSquare",
          "[Parallel], [BlockMatrix]")
{
   SECTION("HypreParMatrixFromBlocks")
   {
      int rank;
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);

      Mesh mesh = Mesh::MakeCartesian2D(
                     10, 10, Element::QUADRILATERAL, 0, 1.0, 1.0);
      int dim = mesh.Dimension();
      int order = 2;

      int nattr = mesh.bdr_attributes.Max();
      Array<int> ess_trial_tdof_list, ess_test_tdof_list;

      Array<int> ess_bdr(nattr);
      ess_bdr = 0;
      ess_bdr[0] = 1;

      ParMesh pmesh(MPI_COMM_WORLD, mesh);

      FiniteElementCollection *hdiv_coll(new RT_FECollection(order, dim));
      FiniteElementCollection *l2_coll(new L2_FECollection(order, dim));

      ParFiniteElementSpace R_space(&pmesh, hdiv_coll);
      ParFiniteElementSpace W_space(&pmesh, l2_coll);

      ParBilinearForm RmVarf(&R_space);
      ParBilinearForm WmVarf(&W_space);
      ParMixedBilinearForm bVarf(&R_space, &W_space);

      HypreParMatrix *MR, *MW, *B;

      RmVarf.AddDomainIntegrator(new VectorFEMassIntegrator());
      RmVarf.Assemble();
      RmVarf.Finalize();
      MR = RmVarf.ParallelAssemble();

      WmVarf.AddDomainIntegrator(new MassIntegrator());
      WmVarf.Assemble();
      WmVarf.Finalize();
      MW = WmVarf.ParallelAssemble();

      bVarf.AddDomainIntegrator(new VectorFEDivergenceIntegrator);
      bVarf.Assemble();
      bVarf.Finalize();
      B = bVarf.ParallelAssemble();
      (*B) *= -1;

      HypreParMatrix *BT = B->Transpose();

      Array<int> blockRow_trueOffsets(3); // number of variables + 1
      blockRow_trueOffsets[0] = 0;
      blockRow_trueOffsets[1] = R_space.TrueVSize();
      blockRow_trueOffsets[2] = W_space.TrueVSize();
      blockRow_trueOffsets.PartialSum();

      BlockOperator blockOper(blockRow_trueOffsets, blockRow_trueOffsets);
      blockOper.SetBlock(0, 0, MR);
      blockOper.SetBlock(0, 1, BT);
      blockOper.SetBlock(1, 0, B);
      blockOper.SetBlock(1, 1, MW, 3.14);

      Array2D<const HypreParMatrix*> hBlocks(2,2);
      hBlocks = NULL;
      hBlocks(0, 0) = MR;
      hBlocks(0, 1) = BT;
      hBlocks(1, 0) = B;
      hBlocks(1, 1) = MW;

      Array2D<real_t> blockCoeff(2,2);
      blockCoeff = 1.0;
      blockCoeff(1, 1) = 3.14;
      HypreParMatrix *H = HypreParMatrixFromBlocks(hBlocks, &blockCoeff);

      Vector yB(blockRow_trueOffsets[2]);
      Vector yH(blockRow_trueOffsets[2]);
      Vector yBR(yB, 0, R_space.TrueVSize());
      Vector yBW(yB, R_space.TrueVSize(), W_space.TrueVSize());

      yB = 0.0;
      yH = 0.0;

      MR->GetDiag(yBR);
      yBR.SyncAliasMemory(yB);
      MW->GetDiag(yBW);
      yBW *= 3.14;
      yBW.SyncAliasMemory(yB);
      H->GetDiag(yH);

      yH -= yB;
      real_t error = yH.Norml2();
      mfem::out << "  order: " << order
                << ", block matrix error norm on rank " << rank << ": " << error << std::endl;
      REQUIRE(error < EPS);

      delete H;
      delete BT;
      delete B;
      delete MW;
      delete MR;
      delete l2_coll;
      delete hdiv_coll;
   }
}

#endif // MFEM_USE_MPI

} // namespace mfem
