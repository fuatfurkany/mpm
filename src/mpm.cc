#include <memory>

#include "factory.h"
#include "io.h"
#include "mpm.h"
#include "mpm_explicit.h"
#include "mpm_semi_implicit_navierstokes.h"

namespace mpm {
// Stress update method
std::map<std::string, StressUpdate> stress_update = {
    {"usf", StressUpdate::USF},
    {"usl", StressUpdate::USL},
    {"musl", StressUpdate::MUSL}};
}  // namespace mpm

// 2D Explicit MPM
static Register<mpm::MPM, mpm::MPMExplicit<2>, const std::shared_ptr<mpm::IO>&>
    mpm_explicit_2d("MPMExplicit2D");

// 3D Explicit MPM
static Register<mpm::MPM, mpm::MPMExplicit<3>, const std::shared_ptr<mpm::IO>&>
    mpm_explicit_3d("MPMExplicit3D");

// 2D SemiImplicit Navier Stokes MPM
static Register<mpm::MPM, mpm::MPMSemiImplicitNavierStokes<2>,
                const std::shared_ptr<mpm::IO>&>
    mpm_semi_implicit_navierstokes_2d("MPMSemiImplicitNavierStokes2D");

// 3D SemiImplicit Navier Stokes MPM
static Register<mpm::MPM, mpm::MPMSemiImplicitNavierStokes<3>,
                const std::shared_ptr<mpm::IO>&>
    mpm_semi_implicit_navierstokes_3d("MPMSemiImplicitNavierStokes3D");