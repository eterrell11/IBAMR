// ---------------------------------------------------------------------
//
// Copyright (c) 2019 - 2024 by the IBAMR developers
// All rights reserved.
//
// This file is part of IBAMR.
//
// IBAMR is free software and is distributed under the 3-clause BSD
// license. The full text of the license can be found in the file
// COPYRIGHT at the top level directory of IBAMR.
//
// ---------------------------------------------------------------------

#include "ibamr/CFGiesekusStrategy.h"
#include "ibamr/CFINSForcing.h"
#include "ibamr/CFOldroydBStrategy.h"
#include "ibamr/CFRoliePolyStrategy.h"
#include "ibamr/CFStrategy.h"
#include "ibamr/ConvectiveOperator.h"
#include "ibamr/INSHierarchyIntegrator.h"

#include "ibtk/HierarchyGhostCellInterpolation.h"
#include "ibtk/IBTK_MPI.h"
#include "ibtk/ibtk_utilities.h"
#include "ibtk/muParserRobinBcCoefs.h"

#include "BasePatchHierarchy.h"
#include "Box.h"
#include "CartesianGridGeometry.h"
#include "CartesianPatchGeometry.h"
#include "CellData.h"
#include "CellIndex.h"
#include "CellIterator.h"
#include "HierarchyDataOpsManager.h"
#include "HierarchyDataOpsReal.h"
#include "Index.h"
#include "MultiblockDataTranslator.h"
#include "Patch.h"
#include "PatchData.h"
#include "PatchGeometry.h"
#include "RobinBcCoefStrategy.h"
#include "SideData.h"
#include "SideIndex.h"
#include "VariableDatabase.h"
#include "VisItDataWriter.h"
#include "tbox/Database.h"
#include "tbox/PIO.h"
#include "tbox/Utilities.h"

IBTK_DISABLE_EXTRA_WARNINGS
#include <Eigen/Cholesky>
#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <unsupported/Eigen/MatrixFunctions>
IBTK_ENABLE_EXTRA_WARNINGS

#include <algorithm>
#include <cmath>
#include <ostream>
#include <utility>

#include "ibamr/app_namespaces.h" // IWYU pragma: keep

extern "C"
{
#if (NDIM == 2)
    void div_tensor_c_to_s_2d_(const double* dx,
                               const double* d_data_0,
                               const double* d_data_1,
                               const int& d_gcw,
                               const double* s_data,
                               const int& s_gcw,
                               const int& ilower0,
                               const int& iupper0,
                               const int& ilower1,
                               const int& iupper1);
    void div_tensor_c_to_c_2d_(const double* dx,
                               const double* d_data,
                               const int& d_gcw,
                               const double* s_data,
                               const int& s_gcw,
                               const int& ilower0,
                               const int& iupper0,
                               const int& ilower1,
                               const int& iupper1);
#endif
#if (NDIM == 3)
    void div_tensor_c_to_s_3d_(const double* dx,
                               const double* d_data_0,
                               const double* d_data_1,
                               const double* d_data_2,
                               const int& d_gcw,
                               const double* s_data,
                               const int& s_gcw,
                               const int& ilower0,
                               const int& iupper0,
                               const int& ilower1,
                               const int& iupper1,
                               const int& ilower2,
                               const int& iupper2);
    void div_tensor_c_to_c_3d_(const double* dx,
                               const double* d_data,
                               const int& d_gcw,
                               const double* s_data,
                               const int& s_gcw,
                               const int& ilower0,
                               const int& iupper0,
                               const int& ilower1,
                               const int& iupper1,
                               const int& ilower2,
                               const int& iupper2);
#endif
}
// Namespace
namespace IBAMR
{
CFINSForcing::CFINSForcing(const std::string& object_name,
                           Pointer<Database> input_db,
                           Pointer<CartGridFunction> u_fcn,
                           Pointer<CartesianGridGeometry<NDIM> > grid_geometry,
                           Pointer<AdvDiffSemiImplicitHierarchyIntegrator> adv_diff_integrator,
                           Pointer<VisItDataWriter<NDIM> > visit_data_writer)
    : CartGridFunction(object_name),
      d_C_cc_var(new CellVariable<NDIM, double>(d_object_name + "::C_cc", NDIM * (NDIM + 1) / 2)),
      d_adv_diff_integrator(adv_diff_integrator),
      d_u_fcn(u_fcn),
      d_u_var(new FaceVariable<NDIM, double>("Complex Fluid Velocity"))
{
    // Set up common values
    commonConstructor(input_db, visit_data_writer, grid_geometry, std::vector<RobinBcCoefStrategy<NDIM>*>());
    // Set up velocity
    d_adv_diff_integrator->registerAdvectionVelocity(d_u_var);
    d_adv_diff_integrator->setAdvectionVelocityFunction(d_u_var, d_u_fcn);
    d_adv_diff_integrator->setAdvectionVelocity(d_C_cc_var, d_u_var);
    return;
} // Constructor

CFINSForcing::CFINSForcing(const std::string& object_name,
                           Pointer<Database> input_db,
                           const Pointer<INSHierarchyIntegrator> fluid_solver,
                           Pointer<CartesianGridGeometry<NDIM> > grid_geometry,
                           Pointer<AdvDiffSemiImplicitHierarchyIntegrator> adv_diff_integrator,
                           Pointer<VisItDataWriter<NDIM> > visit_data_writer)
    : CartGridFunction(object_name),
      d_C_cc_var(new CellVariable<NDIM, double>(d_object_name + "::C_cc", NDIM * (NDIM + 1) / 2)),
      d_adv_diff_integrator(adv_diff_integrator),
      d_u_var(fluid_solver->getAdvectionVelocityVariable())
{
    // Set up common values
    commonConstructor(input_db, visit_data_writer, grid_geometry, fluid_solver->getVelocityBoundaryConditions());
    // Set up correct fluid velocity
    d_adv_diff_integrator->setAdvectionVelocity(d_C_cc_var, d_u_var);
    return;
} // Constructor

CFINSForcing::~CFINSForcing()
{
    // deallocate draw data...
    int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = 0; ln <= finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        if (d_conform_draw && level->checkAllocated(d_conform_idx_draw)) level->deallocatePatchData(d_conform_idx_draw);
        if (d_stress_draw && level->checkAllocated(d_stress_idx_draw)) level->deallocatePatchData(d_stress_idx_draw);
        if ((d_div_sig_idx_draw != IBTK::invalid_index) && level->checkAllocated(d_div_sig_idx_draw))
            level->deallocatePatchData(d_div_sig_idx_draw);
    }
    return;
} // Destructor

void
CFINSForcing::commonConstructor(const Pointer<Database> input_db,
                                Pointer<VisItDataWriter<NDIM> > visit_data_writer,
                                Pointer<CartesianGridGeometry<NDIM> > grid_geom,
                                const std::vector<RobinBcCoefStrategy<NDIM>*> vel_bcs)
{
    // Set up initial conditions
    d_init_conds = new muParserCartGridFunction(d_object_name, input_db->getDatabase("InitialConditions"), grid_geom);
    d_interp_type = input_db->getStringWithDefault("interp_type", d_interp_type);
    // Register Variables and variable context objects.
    auto var_db = VariableDatabase<NDIM>::getDatabase();
    d_context = var_db->getContext(d_object_name + "::CONTEXT");
    const IntVector<NDIM> ghosts_cc = 3;
    // Set up Advection Diffusion Integrator
    d_adv_diff_integrator->registerTransportedQuantity(d_C_cc_var);
    d_adv_diff_integrator->setInitialConditions(d_C_cc_var, d_init_conds);
    d_C_scratch_idx = var_db->registerVariableAndContext(d_C_cc_var, d_context, ghosts_cc);

    // Read parameters from input file
    if (input_db->keyExists("evolution_type"))
        d_evolve_type = string_to_enum<TensorEvolutionType>(input_db->getString("evolution_type"));
    if (input_db->keyExists("evolve_type"))
        d_evolve_type = string_to_enum<TensorEvolutionType>(input_db->getString("evolve_type"));
    if (input_db->keyExists("D")) d_adv_diff_integrator->setDiffusionCoefficient(d_C_cc_var, input_db->getDouble("D"));
    d_log_det = input_db->getBoolWithDefault("log_determinant", d_log_det);
    d_fluid_model = input_db->getStringWithDefault("fluid_model", d_fluid_model);
    for (auto& c : d_fluid_model) c = std::toupper(c);
    d_error_on_spd = input_db->getBoolWithDefault("error_on_spd", d_error_on_spd);
    d_log_div_sig = input_db->getBoolWithDefault("log_divergence", d_log_div_sig);
    d_project_conform = input_db->getBoolWithDefault("project_conformation_tensor", d_project_conform);
    // Have advection diffusion integrator project conformation tensor if necessary.
    if (d_project_conform)
        d_adv_diff_integrator->registerIntegrateHierarchyCallback(&apply_project_tensor_callback,
                                                                  static_cast<void*>(this));

    // Set up drawing variables if necessary
    d_conform_draw = input_db->getBoolWithDefault("output_conformation_tensor", d_conform_draw);
    if (d_conform_draw)
    {
        d_conform_var_draw = new CellVariable<NDIM, double>(d_object_name + "::conform_draw", NDIM * NDIM);
        d_conform_idx_draw = var_db->registerVariableAndContext(d_conform_var_draw, d_context, IntVector<NDIM>(0));
        visit_data_writer->registerPlotQuantity("Conformation_Tensor", "TENSOR", d_conform_idx_draw);
    }
    d_stress_draw = input_db->getBoolWithDefault("output_stress_tensor", d_stress_draw);
    if (d_stress_draw)
    {
        d_stress_var_draw = new CellVariable<NDIM, double>(d_object_name + "::stress_draw", NDIM * NDIM);
        d_stress_idx_draw = var_db->registerVariableAndContext(d_stress_var_draw, d_context, IntVector<NDIM>(0));
        visit_data_writer->registerPlotQuantity("Stress_Tensor", "TENSOR", d_stress_idx_draw);
    }

    // Set up AMR tagging if necessary
    d_div_sig_draw = input_db->getBoolWithDefault("output_divergence", d_div_sig_draw);
    d_div_sig_rel_tag = input_db->getBoolWithDefault("divergence_rel_tagging", d_div_sig_rel_tag);
    d_div_sig_abs_tag = input_db->getBoolWithDefault("divergence_abs_tagging", d_div_sig_abs_tag);
    if (d_log_div_sig || d_div_sig_draw || d_div_sig_abs_tag || d_div_sig_rel_tag)
    {
        d_div_sig_var_draw = new CellVariable<NDIM, double>(d_object_name + "::divW_draw", NDIM);
        d_div_sig_idx_draw = var_db->registerVariableAndContext(d_div_sig_var_draw, d_context, IntVector<NDIM>(0));
        if (d_div_sig_draw) visit_data_writer->registerPlotQuantity("Stress_Divergence", "VECTOR", d_div_sig_idx_draw);
    }
    if (d_div_sig_rel_tag) d_div_sig_rel_thresh = input_db->getDoubleArray("divergence_rel_thresh");
    if (d_div_sig_abs_tag) d_div_sig_abs_thresh = input_db->getDoubleArray("divergence_abs_thresh");
    if (d_div_sig_abs_tag || d_div_sig_rel_tag)
    {
        d_adv_diff_integrator->registerApplyGradientDetectorCallback(&apply_gradient_detector_callback, this);
    }

    // Create boundary conditions for advected materials if not periodic
    const IntVector<NDIM>& periodic_shift = grid_geom->getPeriodicShift();
    if (periodic_shift.min() <= 0)
    {
        d_conc_bc_coefs.resize(NDIM * (NDIM + 1) / 2);
        for (int d = 0; d < NDIM * (NDIM + 1) / 2; ++d)
        {
            const std::string bc_coefs_name = "c_bc_coef_" + std::to_string(d);
            const std::string bc_coefs_db_name = "ExtraStressBoundaryConditions_" + std::to_string(d);
            d_conc_bc_coefs[d] = std::make_unique<muParserRobinBcCoefs>(
                bc_coefs_name, input_db->getDatabase(bc_coefs_db_name), grid_geom);
            d_conc_bc_coefs_ptrs.push_back(d_conc_bc_coefs[d].get());
        }
        d_adv_diff_integrator->setPhysicalBcCoefs(d_C_cc_var, d_conc_bc_coefs_ptrs);
    }

    // Setup the convective operator
    std::string convec_oper_type = input_db->getStringWithDefault("convective_operator_type", "CENTERED");
    auto difference_form =
        string_to_enum<ConvectiveDifferencingType>(input_db->getStringWithDefault("difference_form", "ADVECTIVE"));
    d_convec_oper = new CFUpperConvectiveOperator("ComplexFluidConvectiveOperator",
                                                  d_C_cc_var,
                                                  input_db,
                                                  convec_oper_type,
                                                  difference_form,
                                                  d_conc_bc_coefs_ptrs,
                                                  vel_bcs);
    d_adv_diff_integrator->setConvectiveOperator(d_C_cc_var, d_convec_oper);

    // Register relaxation function
    if (d_fluid_model == "OLDROYDB")
    {
        registerCFStrategy(new CFOldroydBStrategy("CFOldroydBStrategy", input_db));
    }
    else if (d_fluid_model == "GIESEKUS")
    {
        registerCFStrategy(new CFGiesekusStrategy("CFGiesekusStrategy", input_db));
    }
    else if (d_fluid_model == "ROLIEPOLY")
    {
        registerCFStrategy(new CFRoliePolyStrategy("CFRoliePolyStrategy", input_db));
    }
    else if (d_fluid_model != "USER_DEFINED")
    {
        TBOX_ERROR("Fluid model: " + d_fluid_model + " not known.\n\n");
    }
    return;
}

bool
CFINSForcing::isTimeDependent() const
{
    return true;
} // isTimeDependent

void
CFINSForcing::setDataOnPatchHierarchy(const int data_idx,
                                      Pointer<Variable<NDIM> > var,
                                      Pointer<PatchHierarchy<NDIM> > hierarchy,
                                      const double data_time,
                                      const bool initial_time,
                                      const int coarsest_ln_in,
                                      const int finest_ln_in)
{
    d_hierarchy = hierarchy;
    const int coarsest_ln = (coarsest_ln_in == IBTK::invalid_level_number ? 0 : coarsest_ln_in);
    const int finest_ln =
        (finest_ln_in == IBTK::invalid_level_number ? hierarchy->getFinestLevelNumber() : finest_ln_in);
    auto var_db = VariableDatabase<NDIM>::getDatabase();

    // Allocate Data to store components of the Complex stress tensor
    for (int level_num = coarsest_ln; level_num <= finest_ln; ++level_num)
    {
        Pointer<PatchLevel<NDIM> > level = hierarchy->getPatchLevel(level_num);
        if (!level->checkAllocated(d_C_scratch_idx)) level->allocatePatchData(d_C_scratch_idx);
        if (d_stress_draw && !level->checkAllocated(d_stress_idx_draw)) level->allocatePatchData(d_stress_idx_draw);
        if (d_conform_draw && !level->checkAllocated(d_conform_idx_draw)) level->allocatePatchData(d_conform_idx_draw);
        if ((d_div_sig_idx_draw != IBTK::invalid_index) && !level->checkAllocated(d_div_sig_idx_draw))
            level->allocatePatchData(d_div_sig_idx_draw);
    }
    if (initial_time)
    {
        d_init_conds->setDataOnPatchHierarchy(
            d_C_scratch_idx, d_C_cc_var, hierarchy, data_time, coarsest_ln, finest_ln);
    }
    else
    {
        // Copy data from the most recent time to the scratch index.
        const int W_current_idx =
            var_db->mapVariableAndContextToIndex(d_C_cc_var, d_adv_diff_integrator->getCurrentContext());
        const int W_new_idx = var_db->mapVariableAndContextToIndex(d_C_cc_var, d_adv_diff_integrator->getNewContext());
        const bool W_new_is_allocated = d_adv_diff_integrator->isAllocatedPatchData(W_new_idx);
        HierarchyDataOpsManager<NDIM>* hier_data_ops_manager = HierarchyDataOpsManager<NDIM>::getManager();
        Pointer<HierarchyDataOpsReal<NDIM, double> > hier_cc_data_ops =
            hier_data_ops_manager->getOperationsDouble(d_C_cc_var, hierarchy, true);
        if (d_adv_diff_integrator->getCurrentCycleNumber() == 0 || !W_new_is_allocated)
        {
            hier_cc_data_ops->copyData(d_C_scratch_idx, W_current_idx);
        }
        else
        {
            // If the new data is allocated, then we use the midpoint value (defined as the average of current and new
            // data).
            hier_cc_data_ops->linearSum(d_C_scratch_idx, 0.5, W_current_idx, 0.5, W_new_idx);
        }
    }

    HierarchyDataOpsManager<NDIM>* hier_data_ops_manager = HierarchyDataOpsManager<NDIM>::getManager();
    Pointer<HierarchyDataOpsReal<NDIM, double> > hier_cc_data_ops =
        hier_data_ops_manager->getOperationsDouble(d_C_cc_var, hierarchy, true);

    // Fill in boundary conditions for evolved quantity.
    using InterpolationTransactionComponent = HierarchyGhostCellInterpolation::InterpolationTransactionComponent;
    std::vector<InterpolationTransactionComponent> ghost_cell_components(1);
    ghost_cell_components[0] = InterpolationTransactionComponent(d_C_scratch_idx,
                                                                 "CONSERVATIVE_LINEAR_REFINE",
                                                                 true,
                                                                 "CONSERVATIVE_COARSEN",
                                                                 "LINEAR",
                                                                 false,
                                                                 d_conc_bc_coefs_ptrs,
                                                                 nullptr,
                                                                 d_interp_type);
    HierarchyGhostCellInterpolation ghost_fill_op;
    ghost_fill_op.initializeOperatorState(ghost_cell_components, hierarchy);
    ghost_fill_op.fillData(data_time);

    // Convert evolved quantity including ghost cells to conformation tensor.
    switch (d_evolve_type)
    {
    case SQUARE_ROOT:
        squareMatrix(d_C_scratch_idx,
                     d_C_cc_var,
                     hierarchy,
                     data_time,
                     initial_time,
                     coarsest_ln,
                     finest_ln,
                     true /*extended_box*/);
        break;
    case LOGARITHM:
        exponentiateMatrix(d_C_scratch_idx,
                           d_C_cc_var,
                           hierarchy,
                           data_time,
                           initial_time,
                           coarsest_ln,
                           finest_ln,
                           true /*extended_box*/);
        break;
    case STANDARD:
        if (d_project_conform)
            projectTensor(d_C_scratch_idx, d_C_cc_var, data_time, initial_time, true /*extended_box*/);
        break;
    case UNKNOWN_TENSOR_EVOLUTION_TYPE:
        TBOX_ERROR(d_object_name << "\n:"
                                 << "  Unknown tensor evolution type");
        break;
    }

    // Check to ensure conformation tensor is positive definite
    d_positive_def = true;
    checkPositiveDefinite(d_C_scratch_idx, d_C_cc_var, data_time, initial_time);
    int temp = d_positive_def ? 1 : 0;
    d_positive_def = IBTK_MPI::maxReduction(temp) == 1 ? true : false;
    plog << "Conformation tensor is " << (d_positive_def ? "SPD" : "NOT SPD") << "\n";
    if (d_error_on_spd && !d_positive_def)
    {
        TBOX_ERROR("ERROR: \n "
                   << "    CONFORMATION TENSOR IS NOT SPD! \n\n");
    }

    // Check max and min determinant
    if (d_log_det)
    {
        d_max_det = 0.0;
        d_min_det = std::numeric_limits<double>::max();
        findDeterminant(d_C_scratch_idx, d_C_cc_var, data_time, initial_time);
        d_max_det = IBTK_MPI::maxReduction(d_max_det);
        d_min_det = IBTK_MPI::minReduction(d_min_det);
        plog << "Largest det:  " << d_max_det << "\n";
        plog << "Smallest det: " << d_min_det << "\n";
    }

    // If necessary, output the conformation tensor.
    setupPlotConformationTensor(d_C_scratch_idx);

    // Convert conformation tensor to stress tensor
    d_cf_strategy->computeStress(d_C_scratch_idx, d_C_cc_var, hierarchy, data_time);

    // Compute Div W on each patch level
    d_min_norm = std::numeric_limits<double>::max();
    d_max_norm = 0.0;
    for (int level_num = coarsest_ln; level_num <= finest_ln; ++level_num)
    {
        setDataOnPatchLevel(data_idx, var, hierarchy->getPatchLevel(level_num), data_time, initial_time);
    }
    // Output largest and smallest max norm of Div W
    if (d_log_div_sig || d_div_sig_rel_tag)
    {
        IBTK_MPI::maxReduction(d_max_norm);
        IBTK_MPI::minReduction(d_min_norm);
        plog << "Largest max norm of Div W:  " << d_max_norm << "\n";
        plog << "Smallest max norm of Div W: " << d_min_norm << "\n";
    }

    // Deallocate data as needed.
    for (int level_num = coarsest_ln; level_num <= finest_ln; ++level_num)
    {
        Pointer<PatchLevel<NDIM> > level = hierarchy->getPatchLevel(level_num);
        if (level->checkAllocated(d_C_scratch_idx)) level->deallocatePatchData(d_C_scratch_idx);
    }
    return;
} // End setDataOnPatchHierarchy

void
CFINSForcing::setDataOnPatchLevel(const int data_idx,
                                  Pointer<Variable<NDIM> > var,
                                  Pointer<PatchLevel<NDIM> > level,
                                  const double data_time,
                                  const bool initial_time)
{
    if (initial_time)
    {
        // The integrators do not call setDataOnPatchHierarchy for the initial
        // iteration. We need to allocate draw and scratch data here at the
        // initial time.
        if (!level->checkAllocated(d_C_scratch_idx)) level->allocatePatchData(d_C_scratch_idx);
        if (d_conform_draw && !level->checkAllocated(d_conform_idx_draw)) level->allocatePatchData(d_conform_idx_draw);
        if (d_stress_draw && !level->checkAllocated(d_stress_idx_draw)) level->allocatePatchData(d_stress_idx_draw);
        if ((d_div_sig_idx_draw != IBTK::invalid_index) && !level->checkAllocated(d_div_sig_idx_draw))
            level->allocatePatchData(d_div_sig_idx_draw);
        d_init_conds->setDataOnPatchLevel(d_C_scratch_idx, d_C_cc_var, level, data_time, initial_time);
    }
    for (PatchLevel<NDIM>::Iterator p(level); p; p++)
    {
        Pointer<Patch<NDIM> > patch = level->getPatch(p());
        setDataOnPatch(data_idx, var, patch, data_time, initial_time, level);
    }
    return;
}

void
CFINSForcing::setDataOnPatch(const int data_idx,
                             Pointer<Variable<NDIM> > /*var*/,
                             Pointer<Patch<NDIM> > patch,
                             const double /*data_time*/,
                             const bool initial_time,
                             Pointer<PatchLevel<NDIM> > /*patch_level*/)
{
    const Box<NDIM>& patch_box = patch->getBox();
    const Pointer<CartesianPatchGeometry<NDIM> > p_geom = patch->getPatchGeometry();
    const double* dx = p_geom->getDx();
    // NOTE: We precomputed the stress, which is stored in d_C_scratch_idx.
    Pointer<CellData<NDIM, double> > sig_data = patch->getPatchData(d_C_scratch_idx);
    Pointer<CellData<NDIM, double> > div_sig_draw_data =
        d_div_sig_idx_draw != IBTK::invalid_index ? patch->getPatchData(d_div_sig_idx_draw) : nullptr;
    if (d_log_div_sig || d_div_sig_draw || d_div_sig_abs_tag || d_div_sig_rel_tag) div_sig_draw_data->fillAll(0.0);
    Pointer<SideData<NDIM, double> > div_sig_sc_data = patch->getPatchData(data_idx);
    Pointer<CellData<NDIM, double> > div_sig_cc_data = patch->getPatchData(data_idx);
    // If we are drawing the stress tensor, print it out.
    if (d_stress_draw)
    {
        Pointer<CellData<NDIM, double> > stress_data_draw = patch->getPatchData(d_stress_idx_draw);
        for (CellIterator<NDIM> ci(patch_box); ci; ci++)
        {
            CellIndex<NDIM> idx = *ci;
#if (NDIM == 2)
            (*stress_data_draw)(idx, 0) = (*sig_data)(idx, 0);
            (*stress_data_draw)(idx, 1) = (*sig_data)(idx, 2);
            (*stress_data_draw)(idx, 2) = (*sig_data)(idx, 2);
            (*stress_data_draw)(idx, 3) = (*sig_data)(idx, 1);
#endif
#if (NDIM == 3)
            (*stress_data_draw)(idx, 0) = (*sig_data)(idx, 0);
            (*stress_data_draw)(idx, 1) = (*sig_data)(idx, 5);
            (*stress_data_draw)(idx, 2) = (*sig_data)(idx, 4);
            (*stress_data_draw)(idx, 3) = (*sig_data)(idx, 5);
            (*stress_data_draw)(idx, 4) = (*sig_data)(idx, 1);
            (*stress_data_draw)(idx, 5) = (*sig_data)(idx, 3);
            (*stress_data_draw)(idx, 6) = (*sig_data)(idx, 4);
            (*stress_data_draw)(idx, 7) = (*sig_data)(idx, 3);
            (*stress_data_draw)(idx, 8) = (*sig_data)(idx, 2);
#endif
        }
    }
    if (div_sig_sc_data)
    {
        div_sig_sc_data->fillAll(0.0);
        if (initial_time) return;
        const IntVector<NDIM> sig_ghosts = sig_data->getGhostCellWidth();
        const IntVector<NDIM> div_sig_sc_ghosts = div_sig_sc_data->getGhostCellWidth();
        const IntVector<NDIM>& patch_lower = patch_box.lower();
        const IntVector<NDIM>& patch_upper = patch_box.upper();
#if (NDIM == 2)
        div_tensor_c_to_s_2d_(dx,
                              div_sig_sc_data->getPointer(0),
                              div_sig_sc_data->getPointer(1),
                              div_sig_sc_ghosts.min(),
                              sig_data->getPointer(0),
                              sig_ghosts.min(),
                              patch_lower(0),
                              patch_upper(0),
                              patch_lower(1),
                              patch_upper(1));
#endif
#if (NDIM == 3)
        div_tensor_c_to_s_3d_(dx,
                              div_sig_sc_data->getPointer(0),
                              div_sig_sc_data->getPointer(1),
                              div_sig_sc_data->getPointer(2),
                              div_sig_sc_ghosts.min(),
                              sig_data->getPointer(0),
                              sig_ghosts.min(),
                              patch_lower(0),
                              patch_upper(0),
                              patch_lower(1),
                              patch_upper(1),
                              patch_lower(2),
                              patch_upper(2));
#endif
        if (d_log_div_sig || d_div_sig_draw || d_div_sig_abs_tag || d_div_sig_rel_tag)
        {
            for (CellIterator<NDIM> ci(patch_box); ci; ci++)
            {
                CellIndex<NDIM> idx = *ci;
                SideIndex<NDIM> f_x_n = SideIndex<NDIM>(idx, 0, 0);
                SideIndex<NDIM> f_x_p = SideIndex<NDIM>(idx, 0, 1);
                SideIndex<NDIM> f_y_n = SideIndex<NDIM>(idx, 1, 0);
                SideIndex<NDIM> f_y_p = SideIndex<NDIM>(idx, 1, 1);
                double max_norm;
#if (NDIM == 2)
                max_norm = std::max<double>(std::fabs(0.5 * ((*div_sig_sc_data)(f_x_n) + (*div_sig_sc_data)(f_x_p))),
                                            std::fabs(0.5 * ((*div_sig_sc_data)(f_y_n) + (*div_sig_sc_data)(f_y_p))));
                (*div_sig_draw_data)(idx, 0) = 0.5 * ((*div_sig_sc_data)(f_x_n) + (*div_sig_sc_data)(f_x_p));
                (*div_sig_draw_data)(idx, 1) = 0.5 * ((*div_sig_sc_data)(f_y_n) + (*div_sig_sc_data)(f_y_p));
#endif
#if (NDIM == 3)
                SideIndex<NDIM> f_z_n = SideIndex<NDIM>(idx, 2, 0);
                SideIndex<NDIM> f_z_p = SideIndex<NDIM>(idx, 2, 1);
                max_norm = std::max<double>(
                    std::fabs(0.5 * ((*div_sig_sc_data)(f_x_n) + (*div_sig_sc_data)(f_x_p))),
                    std::max<double>(std::fabs(0.5 * ((*div_sig_sc_data)(f_y_n) + (*div_sig_sc_data)(f_y_p))),
                                     std::fabs(0.5 * ((*div_sig_sc_data)(f_z_n) + (*div_sig_sc_data)(f_z_p)))));
                (*div_sig_draw_data)(idx, 0) = 0.5 * ((*div_sig_sc_data)(f_x_n) + (*div_sig_sc_data)(f_x_p));
                (*div_sig_draw_data)(idx, 1) = 0.5 * ((*div_sig_sc_data)(f_y_n) + (*div_sig_sc_data)(f_y_p));
                (*div_sig_draw_data)(idx, 2) = 0.5 * ((*div_sig_sc_data)(f_z_n) + (*div_sig_sc_data)(f_z_p));
#endif
                if (d_log_div_sig || d_div_sig_rel_tag)
                {
                    d_min_norm = (d_min_norm > max_norm) ? max_norm : d_min_norm;
                    d_max_norm = (d_max_norm > max_norm) ? d_max_norm : max_norm;
                }
            }
        }
    }
    else if (div_sig_cc_data)
    {
        div_sig_cc_data->fillAll(0.0);
        if (initial_time) return;
        const IntVector<NDIM> sig_ghosts = sig_data->getGhostCellWidth();
        const IntVector<NDIM> div_sig_ghosts = div_sig_cc_data->getGhostCellWidth();
        const IntVector<NDIM>& patch_lower = patch_box.lower();
        const IntVector<NDIM>& patch_upper = patch_box.upper();
#if (NDIM == 2)
        div_tensor_c_to_c_2d_(dx,
                              div_sig_cc_data->getPointer(0),
                              div_sig_ghosts.min(),
                              sig_data->getPointer(0),
                              sig_ghosts.min(),
                              patch_lower(0),
                              patch_upper(0),
                              patch_lower(1),
                              patch_upper(1));
#endif
#if (NDIM == 3)
        div_tensor_c_to_c_3d_(dx,
                              div_sig_cc_data->getPointer(0),
                              div_sig_ghosts.min(),
                              sig_data->getPointer(0),
                              sig_ghosts.min(),
                              patch_lower(0),
                              patch_upper(0),
                              patch_lower(1),
                              patch_upper(1),
                              patch_lower(2),
                              patch_upper(2));
#endif
        if (d_log_div_sig || d_div_sig_rel_tag)
        {
            for (CellIterator<NDIM> ci(patch_box); ci; ci++)
            {
                CellIndex<NDIM> idx = *ci;
                double max_norm;
#if (NDIM == 2)
                max_norm =
                    std::max<double>(std::fabs((*div_sig_cc_data)(idx, 0)), std::fabs((*div_sig_cc_data)(idx, 1)));
#endif
#if (NDIM == 3)
                max_norm = std::max<double>(
                    std::fabs((*div_sig_cc_data)(idx, 0)),
                    std::max<double>(std::fabs((*div_sig_cc_data)(idx, 1)), std::fabs((*div_sig_cc_data)(idx, 2))));
#endif
                d_min_norm = (d_min_norm > max_norm) ? max_norm : d_min_norm;
                d_max_norm = (d_max_norm > max_norm) ? d_max_norm : max_norm;
            }
        }
        if (d_div_sig_draw || d_div_sig_abs_tag || d_div_sig_rel_tag) div_sig_draw_data->copy(*div_sig_cc_data);
    }
} // setDataOnPatch

void
CFINSForcing::registerCFStrategy(Pointer<CFStrategy> strategy)
{
    d_cf_strategy = strategy;
    d_convec_oper->registerCFStrategy(strategy);
    return;
} // registerRelaxationOperator

void
CFINSForcing::checkPositiveDefinite(const int data_idx,
                                    const Pointer<Variable<NDIM> > /*var*/,
                                    const double /*data_time*/,
                                    const bool initial_time)
{
    for (int ln = 0; ln <= d_hierarchy->getFinestLevelNumber(); ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            const Box<NDIM>& box = patch->getBox();
            const Pointer<PatchGeometry<NDIM> > p_geom = patch->getPatchGeometry();
            if (initial_time) return;
            Pointer<CellData<NDIM, double> > s_data = patch->getPatchData(data_idx);
            for (CellIterator<NDIM> it(box); it; it++)
            {
                const CellIndex<NDIM>& ci = *it;
                MatrixNd tens;
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    tens(idx.first, idx.second) = tens(idx.second, idx.first) = (*s_data)(ci, k);
                }
                Eigen::LLT<MatrixNd> llt;
                llt.compute(tens);
                if (llt.info() == Eigen::NumericalIssue) d_positive_def = false;
            }
        }
    }
    return;
} // checkPositiveDefinite

void
CFINSForcing::squareMatrix(const int data_idx,
                           const Pointer<Variable<NDIM> > /*var*/,
                           const Pointer<PatchHierarchy<NDIM> > hierarchy,
                           const double /*data_time*/,
                           const bool initial_time,
                           const int coarsest_ln,
                           const int finest_ln,
                           const bool extended_box)
{
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            if (initial_time) return;
            Pointer<CellData<NDIM, double> > data = patch->getPatchData(data_idx);
            const Box<NDIM>& box = extended_box ? data->getGhostBox() : patch->getBox();
            const Pointer<PatchGeometry<NDIM> > p_geom = patch->getPatchGeometry();

            for (CellIterator<NDIM> it(box); it; it++)
            {
                CellIndex<NDIM> i = *it;
                MatrixNd tens;
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    tens(idx.first, idx.second) = tens(idx.second, idx.first) = (*data)(i, k);
                }
                tens = tens * tens;
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    (*data)(i, k) = tens(idx.first, idx.second);
                }
            }
        }
    }
    return;
} // squareMatrix

void
CFINSForcing::findDeterminant(const int data_idx,
                              const Pointer<Variable<NDIM> > /*var*/,
                              const double /*data_time*/,
                              const bool /*initial_time*/)
{
    double det;
    for (int ln = 0; ln <= d_hierarchy->getFinestLevelNumber(); ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator i(level); i; i++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(i());
            const Box<NDIM>& box = patch->getBox();
            Pointer<CellData<NDIM, double> > data = patch->getPatchData(data_idx);
            for (CellIterator<NDIM> it(box); it; it++)
            {
                CellIndex<NDIM> i = *it;
#if (NDIM == 2)
                det = (*data)(i, 0) * (*data)(i, 1) - (*data)(i, 2) * (*data)(i, 2);
#endif
#if (NDIM == 3)
                det = (*data)(i, 0) * (*data)(i, 1) * (*data)(i, 2) - (*data)(i, 0) * (*data)(i, 3) * (*data)(i, 3) -
                      (*data)(i, 1) * (*data)(i, 4) * (*data)(i, 4) +
                      2.0 * (*data)(i, 3) * (*data)(i, 4) * (*data)(i, 5) -
                      (*data)(i, 2) * (*data)(i, 5) * (*data)(i, 5);
#endif
                d_max_det = det > d_max_det ? det : d_max_det;
                d_min_det = det < d_min_det ? det : d_min_det;
            }
        }
    }
    return;
} // findDeterminant

void
CFINSForcing::exponentiateMatrix(const int data_idx,
                                 const Pointer<Variable<NDIM> > /*var*/,
                                 const Pointer<PatchHierarchy<NDIM> > hierarchy,
                                 const double /*data_time*/,
                                 const bool /*initial_time*/,
                                 const int coarsest_ln,
                                 const int finest_ln,
                                 const bool extended_box)
{
    for (int ln = coarsest_ln; ln <= finest_ln; ln++)
    {
        Pointer<PatchLevel<NDIM> > level = hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            Pointer<CellData<NDIM, double> > data = patch->getPatchData(data_idx);
            const Box<NDIM>& box = extended_box ? data->getGhostBox() : patch->getBox();
            for (CellIterator<NDIM> it(box); it; it++)
            {
                CellIndex<NDIM> i = *it;
                MatrixNd tens;
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    tens(idx.first, idx.second) = tens(idx.second, idx.first) = (*data)(i, k);
                }
                tens = tens.exp();
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    (*data)(i, k) = tens(idx.first, idx.second);
                }
            }
        }
    }
    return;
} // exponentiateMatrix

void
CFINSForcing::setupPlotConformationTensor(const int C_cc_idx)
{
    if (!d_conform_draw) return;
    for (int ln = 0; ln <= d_hierarchy->getFinestLevelNumber(); ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        if (!level->checkAllocated(d_conform_idx_draw)) level->allocatePatchData(d_conform_idx_draw);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            Pointer<CellData<NDIM, double> > C_data = patch->getPatchData(C_cc_idx);
            Pointer<CellData<NDIM, double> > conform_data_draw = patch->getPatchData(d_conform_idx_draw);
#if (NDIM == 2)
            conform_data_draw->copyDepth(0, *C_data, 0);
            conform_data_draw->copyDepth(1, *C_data, 2);
            conform_data_draw->copyDepth(2, *C_data, 2);
            conform_data_draw->copyDepth(3, *C_data, 1);
#endif
#if (NDIM == 3)
            conform_data_draw->copyDepth(0, *C_data, 0);
            conform_data_draw->copyDepth(1, *C_data, 5);
            conform_data_draw->copyDepth(2, *C_data, 4);
            conform_data_draw->copyDepth(3, *C_data, 5);
            conform_data_draw->copyDepth(4, *C_data, 1);
            conform_data_draw->copyDepth(5, *C_data, 3);
            conform_data_draw->copyDepth(6, *C_data, 4);
            conform_data_draw->copyDepth(7, *C_data, 3);
            conform_data_draw->copyDepth(8, *C_data, 2);
#endif
        }
    }
}

void
CFINSForcing::projectTensor(const int data_idx,
                            const Pointer<Variable<NDIM> > /*var*/,
                            const double /*data_time*/,
                            const bool initial_time,
                            const bool extended_box)
{
    Pointer<PatchHierarchy<NDIM> > hierarchy = d_adv_diff_integrator->getPatchHierarchy();
    for (int ln = 0; ln <= hierarchy->getFinestLevelNumber(); ln++)
    {
        Pointer<PatchLevel<NDIM> > level = hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            if (initial_time) return;
            Pointer<CellData<NDIM, double> > data = patch->getPatchData(data_idx);
            const Box<NDIM>& box = extended_box ? data->getGhostBox() : patch->getBox();
            for (CellIterator<NDIM> it(box); it; it++)
            {
                CellIndex<NDIM> i = *it;
                MatrixNd tens;
                Eigen::SelfAdjointEigenSolver<MatrixNd> eigs;
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    tens(idx.first, idx.second) = tens(idx.second, idx.first) = (*data)(i, k);
                }
                eigs.computeDirect(tens);
                MatrixNd eig_vals(MatrixNd::Zero());
                for (int d = 0; d < NDIM; ++d)
                {
                    eig_vals(d, d) = std::max(eigs.eigenvalues()(d), 0.0);
                }
                MatrixNd eig_vecs = eigs.eigenvectors();
                tens = eig_vecs * eig_vals * eig_vecs.transpose();
                for (int k = 0; k < NDIM * (NDIM + 1) / 2; ++k)
                {
                    const std::pair<int, int>& idx = voigt_to_tensor_idx(k);
                    (*data)(i, k) = tens(idx.first, idx.second);
                }
            }
        }
    }
    return;
} // projectTensor

void
CFINSForcing::applyGradientDetector(Pointer<BasePatchHierarchy<NDIM> > hierarchy,
                                    int level_number,
                                    double /*error_data_time*/,
                                    int tag_index,
                                    bool initial_time,
                                    bool /*richardson_extrapolation_too*/)
{
    if (initial_time) return;
    Pointer<PatchLevel<NDIM> > level = hierarchy->getPatchLevel(level_number);
    double divC_rel_thresh = 0.0;
    if (d_div_sig_rel_thresh.size() > 0)
        divC_rel_thresh = d_div_sig_rel_thresh[std::max(std::min(level_number, d_div_sig_rel_thresh.size() - 1), 0)];
    double divC_abs_thresh = 0.0;
    if (d_div_sig_abs_thresh.size() > 0)
        divC_abs_thresh = d_div_sig_abs_thresh[std::max(std::min(level_number, d_div_sig_abs_thresh.size() - 1), 0)];
    if (divC_rel_thresh > 0.0 || divC_abs_thresh > 0.0)
    {
        double thresh = std::numeric_limits<double>::max();
        if (divC_abs_thresh > 0.0) thresh = std::min(thresh, divC_abs_thresh);
        if (divC_rel_thresh > 0.0) thresh = std::min(thresh, divC_rel_thresh * d_max_norm);
        thresh += sqrt(std::numeric_limits<double>::epsilon());
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            Pointer<CellData<NDIM, double> > C_data = patch->getPatchData(d_div_sig_idx_draw);
            if (!C_data) continue;
            Pointer<CellData<NDIM, int> > tag_data = patch->getPatchData(tag_index);
            const Box<NDIM>& box = patch->getBox();
            for (CellIterator<NDIM> ic(box); ic; ic++)
            {
                const CellIndex<NDIM>& i = ic();
                double norm = 0.0;
                for (int d = 0; d < NDIM; ++d) norm += (*C_data)(i, d) * (*C_data)(i, d);
                norm = sqrt(norm);
                if (norm > thresh) (*tag_data)(i) = 1;
            }
        }
    }
    return;
} // applyGradientDetector

void
CFINSForcing::apply_gradient_detector_callback(Pointer<BasePatchHierarchy<NDIM> > hierarchy,
                                               int level_number,
                                               double error_data_time,
                                               int tag_index,
                                               bool initial_time,
                                               bool richardson_extrapolation_too,
                                               void* ctx)
{
    CFINSForcing* object = static_cast<CFINSForcing*>(ctx);
    object->applyGradientDetector(
        hierarchy, level_number, error_data_time, tag_index, initial_time, richardson_extrapolation_too);
    return;
} // apply_gradient_detector_callback

void
CFINSForcing::apply_project_tensor_callback(const double current_time,
                                            const double /*new_time*/,
                                            const int /*cycle_num*/,
                                            void* ctx)
{
    auto object = static_cast<CFINSForcing*>(ctx);
    auto var_db = VariableDatabase<NDIM>::getDatabase();
    const int C_idx = var_db->mapVariableAndContextToIndex(object->getVariable(),
                                                           object->getAdvDiffHierarchyIntegrator()->getNewContext());
    object->projectTensor(C_idx, object->getVariable(), current_time, false /*initial_time*/, false /*extended_box*/);
    return;
} // apply_project_tensor_callback
} // namespace IBAMR
