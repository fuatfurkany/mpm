#ifndef MPM_MESH_H_
#define MPM_MESH_H_

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

// Eigen
#include "Eigen/Dense"
// MPI
#ifdef USE_MPI
#include "mpi.h"
#endif
// TBB
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>

#include <tsl/robin_map.h>
// JSON
#include "json.hpp"
using Json = nlohmann::json;

#include "boundary_segment.h"
#include "cell.h"
#include "container.h"
#include "factory.h"
#include "friction_constraint.h"
#include "function_base.h"
#include "geometry.h"
#include "hdf5_particle.h"
#include "io.h"
#include "io_mesh.h"
#include "logger.h"
#include "material/material.h"
#include "mpi_datatypes.h"
#include "node.h"
#include "particle.h"
#include "particle_base.h"
#include "traction.h"
#include "velocity_constraint.h"

namespace mpm {

//! Mesh class
//! \brief Base class that stores the information about meshes
//! \details Mesh class which stores the particles, nodes, cells and neighbours
//! \tparam Tdim Dimension
template <unsigned Tdim>
class Mesh {
 public:
  //! Define a vector of size dimension
  using VectorDim = Eigen::Matrix<double, Tdim, 1>;

  // Construct a mesh with a global unique id
  //! \param[in] id Global mesh id
  //! \param[in] isoparametric Mesh is isoparametric
  Mesh(unsigned id, bool isoparametric = true);

  //! Default destructor
  ~Mesh() = default;

  //! Delete copy constructor
  Mesh(const Mesh<Tdim>&) = delete;

  //! Delete assignement operator
  Mesh& operator=(const Mesh<Tdim>&) = delete;

  //! Return id of the mesh
  unsigned id() const { return id_; }

  //! Return if a mesh is isoparametric
  bool is_isoparametric() const { return isoparametric_; }

  //! Create nodes from coordinates
  //! \param[in] gnid Global node id
  //! \param[in] node_type Node type
  //! \param[in] coordinates Nodal coordinates
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval status Create node status
  bool create_nodes(mpm::Index gnid, const std::string& node_type,
                    const std::vector<VectorDim>& coordinates,
                    bool check_duplicates = true);

  //! Add a node to the mesh
  //! \param[in] node A shared pointer to node
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval insertion_status Return the successful addition of a node
  bool add_node(const std::shared_ptr<mpm::NodeBase<Tdim>>& node,
                bool check_duplicates = true);

  //! Remove a node from the mesh
  //! \param[in] node A shared pointer to node
  //! \retval insertion_status Return the successful addition of a node
  bool remove_node(const std::shared_ptr<mpm::NodeBase<Tdim>>& node);

  //! Return the number of nodes
  mpm::Index nnodes() const { return nodes_.size(); }

  //! Iterate over nodes
  //! \tparam Toper Callable object typically a baseclass functor
  template <typename Toper>
  void iterate_over_nodes(Toper oper);

  //! Iterate over nodes with predicate
  //! \tparam Toper Callable object typically a baseclass functor
  //! \tparam Tpred Predicate
  template <typename Toper, typename Tpred>
  void iterate_over_nodes_predicate(Toper oper, Tpred pred);

  //! Create a list of active nodes in mesh
  void find_active_nodes();

  //! Iterate over active nodes
  //! \tparam Toper Callable object typically a baseclass functor
  template <typename Toper>
  void iterate_over_active_nodes(Toper oper);

#ifdef USE_MPI
  //! All reduce over nodal property
  //! \tparam Ttype Type of property to accumulate
  //! \tparam Tnparam Size of individual property
  //! \tparam Tgetfunctor Functor for getter
  //! \tparam Tsetfunctor Functor for setter
  //! \param[in] getter Getter function
  template <typename Ttype, unsigned Tnparam, typename Tgetfunctor,
            typename Tsetfunctor>
  void nodal_halo_exchange(Tgetfunctor getter, Tsetfunctor setter);
#endif

  //! Create cells from list of nodes
  //! \param[in] gcid Global cell id
  //! \param[in] element Element type
  //! \param[in] cells Node ids of cells
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval status Create cells status
  bool create_cells(mpm::Index gnid,
                    const std::shared_ptr<mpm::Element<Tdim>>& element,
                    const std::vector<std::vector<mpm::Index>>& cells,
                    bool check_duplicates = true);

  //! Add a cell from the mesh
  //! \param[in] cell A shared pointer to cell
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval insertion_status Return the successful addition of a cell
  bool add_cell(const std::shared_ptr<mpm::Cell<Tdim>>& cell,
                bool check_duplicates = true);

  //! Remove a cell from the mesh
  //! \param[in] cell A shared pointer to cell
  //! \retval insertion_status Return the successful addition of a cell
  bool remove_cell(const std::shared_ptr<mpm::Cell<Tdim>>& cell);

  //! Number of cells in the mesh
  mpm::Index ncells() const { return cells_.size(); }

  //! Iterate over cells
  //! \tparam Toper Callable object typically a baseclass functor
  template <typename Toper>
  void iterate_over_cells(Toper oper);

  //! Create particles from coordinates
  //! \param[in] particle_type Particle type
  //! \param[in] coordinates Nodal coordinates
  //! \param[in] material_id ID of the material
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval status Create particle status
  bool create_particles(const std::string& particle_type,
                        const std::vector<VectorDim>& coordinates,
                        unsigned material_id, bool check_duplicates = true);

  //! Add a particle to the mesh
  //! \param[in] particle A shared pointer to particle
  //! \param[in] checks Parameter to check duplicates and addition
  //! \retval insertion_status Return the successful addition of a particle
  bool add_particle(const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle,
                    bool checks = true);

  //! Remove a particle from the mesh
  //! \param[in] particle A shared pointer to particle
  //! \retval insertion_status Return the successful addition of a particle
  bool remove_particle(
      const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle);

  //! Remove a particle by id
  bool remove_particle_by_id(mpm::Index id);

  //! Remove a particle from the mesh
  //! \param[in] pids Vector of particle ids
  void remove_particles(const std::vector<mpm::Index>& pids);

  //! Remove all particles in a cell in nonlocal rank
  void remove_all_nonrank_particles();

  //! Transfer particles to different ranks in nonlocal rank cells
  void transfer_nonrank_particles();

  //! Find shared nodes across MPI domains in the mesh
  void find_domain_shared_nodes();

  //! Number of particles in the mesh
  mpm::Index nparticles() const { return particles_.size(); }

  //! Locate particles in a cell
  //! Iterate over all cells in a mesh to find the cell in which particles
  //! are located.
  //! \retval particles Particles which cannot be located in the mesh
  std::vector<std::shared_ptr<mpm::ParticleBase<Tdim>>> locate_particles_mesh();

  //! Iterate over particles
  //! \tparam Toper Callable object typically a baseclass functor
  template <typename Toper>
  void iterate_over_particles(Toper oper);

  //! Iterate over particle set
  //! \tparam Toper Callable object typically a baseclass functor
  //! \param[in] set_id particle set id
  template <typename Toper>
  void iterate_over_particle_set(int set_id, Toper oper);

  //! Return coordinates of particles
  std::vector<Eigen::Matrix<double, 3, 1>> particle_coordinates();

  //! Return particles vector data
  //! \param[in] attribute Name of the vector data attribute
  std::vector<Eigen::Matrix<double, 3, 1>> particles_vector_data(
      const std::string& attribute);

  //! Return particles scalar data
  //! \param[in] attribute Name of the scalar data attribute
  std::vector<double> particles_statevars_data(const std::string& attribute);

  //! Compute and assign rotation matrix to nodes
  //! \param[in] euler_angles Map of node number and respective euler_angles
  bool compute_nodal_rotation_matrices(
      const std::map<mpm::Index, Eigen::Matrix<double, Tdim, 1>>& euler_angles);

  //! Assign particles volumes
  //! \param[in] particle_volumes Volume at dir on particle
  bool assign_particles_volumes(
      const std::vector<std::tuple<mpm::Index, double>>& particle_volumes);

  //! Create particles tractions
  //! \param[in] mfunction Math function if defined
  //! \param[in] setid Particle set id
  //! \param[in] dir Direction of traction load
  //! \param[in] traction Particle traction
  bool create_particles_tractions(
      const std::shared_ptr<FunctionBase>& mfunction, int set_id, unsigned dir,
      double traction);

  //! Apply traction to particles
  //! \param[in] current_time Current time
  void apply_traction_on_particles(double current_time);

  //! Create particle velocity constraints tractions
  //! \param[in] setid Node set id
  bool create_particle_velocity_constraint(
      int set_id, const std::shared_ptr<mpm::VelocityConstraint>& constraint);

  //! Apply particles velocity constraints
  void apply_particle_velocity_constraints();

  //! Assign nodal velocity constraints
  //! \param[in] setid Node set id
  //! \param[in] velocity_constraints Velocity constraint at node, dir, velocity
  bool assign_nodal_velocity_constraint(
      int set_id, const std::shared_ptr<mpm::VelocityConstraint>& constraint);

  //! Assign nodal frictional constraints
  //! \param[in] setid Node set id
  //! \param[in] friction_constraints Constraint at node, dir, sign, friction
  bool assign_nodal_frictional_constraint(
      int nset_id,
      const std::shared_ptr<mpm::FrictionConstraint>& fconstraints);

  //! Assign velocity constraints to nodes
  //! \param[in] velocity_constraints Constraint at node, dir, and velocity
  bool assign_nodal_velocity_constraints(
      const std::vector<std::tuple<mpm::Index, unsigned, double>>&
          velocity_constraints);

  //! Assign friction constraints to nodes
  //! \param[in] friction_constraints Constraint at node, dir, sign, and
  //! friction
  bool assign_nodal_friction_constraints(
      const std::vector<std::tuple<mpm::Index, unsigned, int, double>>&
          friction_constraints);

  //! Assign nodal concentrated force
  //! \param[in] nodal_forces Force at dir on nodes
  bool assign_nodal_concentrated_forces(
      const std::vector<std::tuple<mpm::Index, unsigned, double>>&
          nodal_forces);

  //! Assign nodal concentrated force
  //! \param[in] mfunction Math function if defined
  //! \param[in] setid Node set id
  //! \param[in] dir Direction of force
  //! \param[in] node_forces Concentrated force at dir on nodes
  bool assign_nodal_concentrated_forces(
      const std::shared_ptr<FunctionBase>& mfunction, int set_id, unsigned dir,
      double force);

  //! Assign particles stresses
  //! \param[in] particle_stresses Initial stresses of particle
  bool assign_particles_stresses(
      const std::vector<Eigen::Matrix<double, 6, 1>>& particle_stresses);

  //! Assign particles cells
  //! \param[in] particles_cells Particles and cells
  bool assign_particles_cells(
      const std::vector<std::array<mpm::Index, 2>>& particles_cells);

  //! Return particles cells
  //! \retval particles_cells Particles and cells
  std::vector<std::array<mpm::Index, 2>> particles_cells() const;

  //! Return status of the mesh. A mesh is active, if at least one particle is
  //! present
  bool status() const { return particles_.size(); }

  //! Generate points
  //! \param[in] nquadratures Number of points per direction in cell
  //! \param[in] particle_type Particle type
  //! \param[in] material_id ID of the material
  //! \param[in] cset_id Set ID of the cell [-1 for all cells]
  //! \retval point Material point coordinates
  bool generate_material_points(unsigned nquadratures,
                                const std::string& particle_type,
                                unsigned material_id, int cset_id);

  //! Initialise material models
  //! \param[in] materials Material models
  void initialise_material_models(
      const std::map<unsigned, std::shared_ptr<mpm::Material<Tdim>>>&
          materials) {
    materials_ = materials;
  }

  //! Find cell neighbours
  void compute_cell_neighbours();

  //! Add a neighbour mesh, using the local id for the new mesh and a mesh
  //! pointer
  //! \param[in] local_id local id of the mesh
  //! \param[in] neighbour A shared pointer to the neighbouring mesh
  //! \retval insertion_status Return the successful addition of a node
  bool add_neighbour(unsigned local_id,
                     const std::shared_ptr<Mesh<Tdim>>& neighbour);

  //! Return the number of neighbouring meshes
  unsigned nneighbours() const { return neighbour_meshes_.size(); }

  //! Find ghost boundary cells
  void find_ghost_boundary_cells();

  //! Write HDF5 particles
  //! \param[in] phase Index corresponding to the phase
  //! \param[in] filename Name of HDF5 file to write particles data
  //! \retval status Status of writing HDF5 output
  bool write_particles_hdf5(unsigned phase, const std::string& filename);

  //! Read HDF5 particles
  //! \param[in] phase Index corresponding to the phase
  //! \param[in] filename Name of HDF5 file to write particles data
  //! \retval status Status of reading HDF5 output
  bool read_particles_hdf5(unsigned phase, const std::string& filename);

  //! Return HDF5 particles
  //! \retval particles_hdf5 Vector of HDF5 particles
  std::vector<mpm::HDF5Particle> particles_hdf5() const;

  //! Return nodal coordinates
  std::vector<Eigen::Matrix<double, 3, 1>> nodal_coordinates() const;

  //! Return node pairs
  std::vector<std::array<mpm::Index, 2>> node_pairs() const;

  //! Create map of container of particles in sets
  //! \param[in] map of particles ids in sets
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval status Status of  create particle sets
  bool create_particle_sets(
      const tsl::robin_map<mpm::Index, std::vector<mpm::Index>>& particle_sets,
      bool check_duplicates);

  //! Create map of container of nodes in sets
  //! \param[in] map of nodes ids in sets
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval status Status of  create node sets
  bool create_node_sets(
      const tsl::robin_map<mpm::Index, std::vector<mpm::Index>>& node_sets,
      bool check_duplicates);

  //! Create map of container of cells in sets
  //! \param[in] map of cells ids in sets
  //! \param[in] check_duplicates Parameter to check duplicates
  //! \retval status Status of  create cell sets
  bool create_cell_sets(
      const tsl::robin_map<mpm::Index, std::vector<mpm::Index>>& cell_sets,
      bool check_duplicates);

  //! Get the container of cell
  mpm::Container<Cell<Tdim>> cells();

  //! Return particle cell ids
  std::map<mpm::Index, mpm::Index>* particles_cell_ids();

  //! Return nghost cells
  unsigned nghost_cells() const { return ghost_cells_.size(); }

  //! Return nlocal ghost cells
  unsigned nlocal_ghost_cells() const { return local_ghost_cells_.size(); }

  //! Generate particles
  //! \param[in] io IO object handle
  //! \param[in] generator Point generator object
  bool generate_particles(const std::shared_ptr<mpm::IO>& io,
                          const Json& generator);

  //! Iterate over boundary particles
  //! \tparam Toper Callable object typically a baseclass functor
  template <typename Toper>
  void iterate_over_boundary_particles(Toper oper);

  //! Iterate over boundary segments
  //! \tparam Toper Callable object typically a baseclass functor
  template <typename Toper>
  void iterate_over_boundary_segments(Toper oper);

 private:
  // Read particles from file
  bool read_particles_file(const std::shared_ptr<mpm::IO>& io,
                           const Json& generator);

  // Locate a particle in mesh cells
  bool locate_particle_cells(
      const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle);

 private:
  //! mesh id
  unsigned id_{std::numeric_limits<unsigned>::max()};
  //! Isoparametric mesh
  bool isoparametric_{true};
  //! Container of mesh neighbours
  Map<Mesh<Tdim>> neighbour_meshes_;
  //! Container of particles
  Container<ParticleBase<Tdim>> particles_;
  //! Container of particles ids and cell ids
  std::map<mpm::Index, mpm::Index> particles_cell_ids_;
  //! Container of particle sets
  tsl::robin_map<unsigned, tbb::concurrent_vector<mpm::Index>> particle_sets_;
  //! Map of particles for fast retrieval
  Map<ParticleBase<Tdim>> map_particles_;
  //! Container of nodes
  Container<NodeBase<Tdim>> nodes_;
  //! Container of domain shared nodes
  Container<NodeBase<Tdim>> domain_shared_nodes_;
  //! Boundary nodes
  Container<NodeBase<Tdim>> boundary_nodes_;
  //! Container of node sets
  tsl::robin_map<unsigned, Container<NodeBase<Tdim>>> node_sets_;
  //! Container of active nodes
  Container<NodeBase<Tdim>> active_nodes_;
  //! Map of nodes for fast retrieval
  Map<NodeBase<Tdim>> map_nodes_;
  //! Map of cells for fast retrieval
  Map<Cell<Tdim>> map_cells_;
  //! Container of cells
  Container<Cell<Tdim>> cells_;
  //! Container of ghost cells sharing the current MPI rank
  Container<Cell<Tdim>> ghost_cells_;
  //! Container of local ghost cells
  Container<Cell<Tdim>> local_ghost_cells_;
  //! Container of cell sets
  tsl::robin_map<unsigned, Container<Cell<Tdim>>> cell_sets_;
  //! Map of ghost cells to the neighbours ranks
  std::map<unsigned, std::vector<unsigned>> ghost_cells_neighbour_ranks_;
  //! Faces and cells
  std::multimap<std::vector<mpm::Index>, mpm::Index> faces_cells_;
  //! Materials
  std::map<unsigned, std::shared_ptr<mpm::Material<Tdim>>> materials_;
  //! Loading (Particle tractions)
  std::vector<std::shared_ptr<mpm::Traction>> particle_tractions_;
  //! Particle velocity constraints
  std::vector<std::shared_ptr<mpm::VelocityConstraint>>
      particle_velocity_constraints_;
  //! Logger
  std::unique_ptr<spdlog::logger> console_;
  //! TBB grain size
  int tbb_grain_size_{100};
  //! Maximum number of halo nodes
  unsigned nhalo_nodes_{0};
  //! Maximum number of halo nodes
  unsigned ncomms_{0};
  //! Container of boundary particles
  Container<ParticleBase<Tdim>> boundary_particles_;
  //! Container of boundary line segments
  Container<BoundarySegment<Tdim>> boundary_segments_;
};  // Mesh class
}  // namespace mpm

#include "mesh.tcc"

#endif  // MPM_MESH_H_
