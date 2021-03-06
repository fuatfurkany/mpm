// Constructor with id
template <unsigned Tdim>
mpm::Mesh<Tdim>::Mesh(unsigned id, bool isoparametric)
    : id_{id}, isoparametric_{isoparametric} {
  // Check if the dimension is between 1 & 3
  static_assert((Tdim >= 1 && Tdim <= 3), "Invalid global dimension");
  //! Logger
  std::string logger = "mesh::" + std::to_string(id);
  console_ = std::make_unique<spdlog::logger>(logger, mpm::stdout_sink);

  particles_.clear();
}

//! Create nodes from coordinates
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_nodes(mpm::Index gnid,
                                   const std::string& node_type,
                                   const std::vector<VectorDim>& coordinates,
                                   bool check_duplicates) {
  bool status = true;
  try {
    // Check if nodal coordinates is empty
    if (coordinates.empty())
      throw std::runtime_error("List of coordinates is empty");
    // Iterate over all coordinates
    for (const auto& node_coordinates : coordinates) {
      // Add node to mesh and check
      bool insert_status = this->add_node(
          // Create a node of particular
          Factory<mpm::NodeBase<Tdim>, mpm::Index,
                  const Eigen::Matrix<double, Tdim, 1>&>::instance()
              ->create(node_type, static_cast<mpm::Index>(gnid),
                       node_coordinates),
          check_duplicates);

      // Increment node id
      if (insert_status) ++gnid;
      // When addition of node fails
      else
        throw std::runtime_error("Addition of node to mesh failed!");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Add a node to the mesh
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::add_node(const std::shared_ptr<mpm::NodeBase<Tdim>>& node,
                               bool check_duplicates) {
  bool insertion_status = nodes_.add(node, check_duplicates);
  // Add node to map
  if (insertion_status) map_nodes_.insert(node->id(), node);
  return insertion_status;
}

//! Remove a node from the mesh
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::remove_node(
    const std::shared_ptr<mpm::NodeBase<Tdim>>& node) {
  const mpm::Index id = node->id();
  // Remove a node if found in the container
  return (nodes_.remove(node) && map_nodes_.remove(id));
}

//! Iterate over nodes
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_nodes(Toper oper) {
  tbb::parallel_for(tbb::blocked_range<int>(size_t(0), size_t(nodes_.size()),
                                            tbb_grain_size_),
                    [&](const tbb::blocked_range<int>& range) {
                      for (int i = range.begin(); i != range.end(); ++i)
                        oper(nodes_[i]);
                    },
                    tbb::simple_partitioner());
}

//! Iterate over nodes
template <unsigned Tdim>
template <typename Toper, typename Tpred>
void mpm::Mesh<Tdim>::iterate_over_nodes_predicate(Toper oper, Tpred pred) {
  tbb::parallel_for(tbb::blocked_range<int>(size_t(0), size_t(nodes_.size()),
                                            tbb_grain_size_),
                    [&](const tbb::blocked_range<int>& range) {
                      for (int i = range.begin(); i != range.end(); ++i)
                        if (pred(nodes_[i])) oper(nodes_[i]);
                    },
                    tbb::simple_partitioner());
}

//! Create a list of active nodes in mesh
template <unsigned Tdim>
void mpm::Mesh<Tdim>::find_active_nodes() {
  // Clear existing list of active nodes
  this->active_nodes_.clear();

  for (auto nitr = nodes_.cbegin(); nitr != nodes_.cend(); ++nitr)
    if ((*nitr)->status()) this->active_nodes_.add(*nitr);
}

//! Iterate over active nodes
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_active_nodes(Toper oper) {
  tbb::parallel_for(
      tbb::blocked_range<int>(size_t(0), size_t(active_nodes_.size()),
                              tbb_grain_size_),
      [&](const tbb::blocked_range<int>& range) {
        for (int i = range.begin(); i != range.end(); ++i)
          oper(active_nodes_[i]);
      },
      tbb::simple_partitioner());
}

#ifdef USE_MPI
#ifdef USE_HALO_EXCHANGE
//! Nodal halo exchange
template <unsigned Tdim>
template <typename Ttype, unsigned Tnparam, typename Tgetfunctor,
          typename Tsetfunctor>
void mpm::Mesh<Tdim>::nodal_halo_exchange(Tgetfunctor getter,
                                          Tsetfunctor setter) {
  // Create vector of nodal vectors
  unsigned nnodes = this->domain_shared_nodes_.size();

  // Get number of MPI ranks
  int mpi_size;
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
  int mpi_rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

  if (mpi_size > 1) {
    // Vector of send requests
    std::vector<MPI_Request> send_requests;
    send_requests.reserve(ncomms_);

    unsigned j = 0;
    // Non-blocking send
    for (unsigned i = 0; i < nnodes; ++i) {
      Ttype property = getter(domain_shared_nodes_[i]);
      std::set<unsigned> node_mpi_ranks = domain_shared_nodes_[i]->mpi_ranks();
      for (auto& node_rank : node_mpi_ranks) {
        if (node_rank != mpi_rank) {
          MPI_Isend(&property, Tnparam, MPI_DOUBLE, node_rank,
                    domain_shared_nodes_[i]->id(), MPI_COMM_WORLD,
                    &send_requests[j]);
          ++j;
        }
      }
    }

    // send complete
    for (unsigned i = 0; i < ncomms_; ++i)
      MPI_Wait(&send_requests[i], MPI_STATUS_IGNORE);

    for (unsigned i = 0; i < nnodes; ++i) {
      // Get value at current node
      Ttype property = getter(domain_shared_nodes_[i]);

      std::set<unsigned> node_mpi_ranks = domain_shared_nodes_[i]->mpi_ranks();
      // Receive from all shared ranks
      for (auto& node_rank : node_mpi_ranks) {
        if (node_rank != mpi_rank) {
          Ttype value;
          MPI_Recv(&value, Tnparam, MPI_DOUBLE, node_rank,
                   domain_shared_nodes_[i]->id(), MPI_COMM_WORLD,
                   MPI_STATUS_IGNORE);
          property += value;
        }
      }
      setter(domain_shared_nodes_[i], property);
    }
  }
}

#else
//! All reduce over nodal scalar property
template <unsigned Tdim>
template <typename Ttype, unsigned Tnparam, typename Tgetfunctor,
          typename Tsetfunctor>
void mpm::Mesh<Tdim>::nodal_halo_exchange(Tgetfunctor getter,
                                          Tsetfunctor setter) {
  // Create vector of nodal scalars
  std::vector<Ttype> prop_get(nhalo_nodes_, mpm::zero<Ttype>());
  std::vector<Ttype> prop_set(nhalo_nodes_, mpm::zero<Ttype>());

  tbb::parallel_for_each(
      domain_shared_nodes_.cbegin(), domain_shared_nodes_.cend(),
      [=, &prop_get](std::shared_ptr<mpm::NodeBase<Tdim>> node) {
        prop_get.at(node->ghost_id()) = getter(node);
      });

  MPI_Allreduce(prop_get.data(), prop_set.data(), nhalo_nodes_ * Tnparam,
                MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  tbb::parallel_for_each(
      domain_shared_nodes_.cbegin(), domain_shared_nodes_.cend(),
      [=, &prop_set](std::shared_ptr<mpm::NodeBase<Tdim>> node) {
        setter(node, prop_set.at(node->ghost_id()));
      });
}
#endif
#endif

//! Create cells from node lists
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_cells(
    mpm::Index gcid, const std::shared_ptr<mpm::Element<Tdim>>& element,
    const std::vector<std::vector<mpm::Index>>& cells, bool check_duplicates) {
  bool status = true;
  try {
    // Check if nodes in cell list is not empty
    if (cells.empty())
      throw std::runtime_error("List of nodes of cells is empty");

    for (const auto& nodes : cells) {
      // Create cell with element
      auto cell = std::make_shared<mpm::Cell<Tdim>>(gcid, nodes.size(), element,
                                                    this->isoparametric_);

      // Cell local node id
      unsigned local_nid = 0;
      // For nodeids in a given cell
      for (auto nid : nodes) {
        cell->add_node(local_nid, map_nodes_[nid]);
        ++local_nid;
      }

      // Add cell to mesh
      bool insert_cell = false;
      // Check if cell has all nodes before inserting to mesh
      if (cell->nnodes() == nodes.size()) {
        // Initialise cell before insertion
        cell->initialise();
        // If cell is initialised insert to mesh
        if (cell->is_initialised())
          insert_cell = this->add_cell(cell, check_duplicates);
      } else
        throw std::runtime_error("Invalid node ids for cell!");

      // Increment global cell id
      if (insert_cell) ++gcid;
      // When addition of cell fails
      else
        throw std::runtime_error("Addition of cell to mesh failed!");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Add a cell to the mesh
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::add_cell(const std::shared_ptr<mpm::Cell<Tdim>>& cell,
                               bool check_duplicates) {
  bool insertion_status = cells_.add(cell, check_duplicates);
  // Add cell to map
  if (insertion_status) map_cells_.insert(cell->id(), cell);
  return insertion_status;
}

//! Remove a cell from the mesh
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::remove_cell(
    const std::shared_ptr<mpm::Cell<Tdim>>& cell) {
  const mpm::Index id = cell->id();
  // Remove a cell if found in the container
  return (cells_.remove(cell) && map_cells_.remove(id));
}

//! Iterate over cells
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_cells(Toper oper) {
  tbb::parallel_for(tbb::blocked_range<int>(size_t(0), size_t(cells_.size()),
                                            tbb_grain_size_),
                    [&](const tbb::blocked_range<int>& range) {
                      for (int i = range.begin(); i != range.end(); ++i)
                        oper(cells_[i]);
                    },
                    tbb::simple_partitioner());
}

//! Create cells from node lists
template <unsigned Tdim>
void mpm::Mesh<Tdim>::compute_cell_neighbours() {
  // Initialize and compute node cell map
  tsl::robin_map<mpm::Index, std::set<mpm::Index>> node_cell_map;
  for (auto citr = cells_.cbegin(); citr != cells_.cend(); ++citr) {
    // Populate node_cell_map with the node_id and multiple cell_id
    auto cell_id = (*citr)->id();
    for (auto id : (*citr)->nodes_id()) node_cell_map[id].insert(cell_id);
  }

  // Assign neighbour to cells
  tbb::parallel_for(tbb::blocked_range<int>(size_t(0), size_t(cells_.size()),
                                            tbb_grain_size_),
                    [&](const tbb::blocked_range<int>& range) {
                      for (int i = range.begin(); i != range.end(); ++i) {
                        // Iterate over each node in current cell
                        for (auto id : cells_[i]->nodes_id()) {
                          auto cell_id = cells_[i]->id();
                          // Get the cells associated with each node
                          for (auto neighbour_id : node_cell_map[id])
                            if (neighbour_id != cell_id)
                              cells_[i]->add_neighbour(neighbour_id);
                        }
                      }
                    },
                    tbb::simple_partitioner());
}

//! Find ghost cell neighbours
template <unsigned Tdim>
void mpm::Mesh<Tdim>::find_ghost_boundary_cells() {
#ifdef USE_MPI
  // Get number of MPI ranks
  int mpi_size;
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
  int mpi_rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  bool check_duplicates = true;
  if (mpi_size > 1) {
    ghost_cells_.clear();
    local_ghost_cells_.clear();
    ghost_cells_neighbour_ranks_.clear();
    // Iterate through cells
    for (auto citr = cells_.cbegin(); citr != cells_.cend(); ++citr) {
      std::set<unsigned> neighbour_ranks;
      // If cell rank is the current MPI rank
      if ((*citr)->rank() == mpi_rank) {
        // Iterate through the neighbours of a cell
        auto neighbours = (*citr)->neighbours();
        for (auto neighbour : neighbours) {
          // If the neighbour is in a different MPI rank
          if (map_cells_[neighbour]->rank() != mpi_rank) {
            ghost_cells_.add(map_cells_[neighbour], check_duplicates);
            // Add mpi rank to set
            neighbour_ranks.insert(map_cells_[neighbour]->rank());
          }
        }
      }
      // Set the number of different MPI rank neighbours to a ghost cell
      if (neighbour_ranks.size() > 0) {
        // Also add the current cell, as this would be a receiver
        local_ghost_cells_.add(*citr, check_duplicates);

        // Update the neighbouring ranks of the local ghost cell
        std::vector<unsigned> mpi_neighbours;
        for (auto rank : neighbour_ranks) mpi_neighbours.emplace_back(rank);
        ghost_cells_neighbour_ranks_[(*citr)->id()] = mpi_neighbours;
      }
    }
  }
#endif
}

//! Create cells from node lists
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::generate_material_points(unsigned nquadratures,
                                               const std::string& particle_type,
                                               unsigned material_id,
                                               int cset_id) {
  bool status = true;
  try {
    if (cells_.size() > 0) {
      unsigned before_generation = this->nparticles();
      bool checks = false;
      // Get material
      auto material = materials_.at(material_id);

      // If set id is -1, use all cells
      auto cset = (cset_id == -1) ? this->cells_ : cell_sets_.at(cset_id);
      // Iterate over each cell to generate points
      for (auto citr = cset.cbegin(); citr != cset.cend(); ++citr) {
        (*citr)->assign_quadrature(nquadratures);
        // Genereate particles at the Gauss points
        const auto cpoints = (*citr)->generate_points();
        // Iterate over each coordinate to generate material points
        for (const auto& coordinates : cpoints) {
          // Particle id
          mpm::Index pid = particles_.size();
          // Create particle
          auto particle =
              Factory<mpm::ParticleBase<Tdim>, mpm::Index,
                      const Eigen::Matrix<double, Tdim, 1>&>::instance()
                  ->create(particle_type, static_cast<mpm::Index>(pid),
                           coordinates);

          // Add particle to mesh
          status = this->add_particle(particle, checks);
          if (status) {
            map_particles_[pid]->assign_cell(*citr);
            map_particles_[pid]->assign_material(material);
          } else
            throw std::runtime_error("Generate particles in mesh failed");
        }
      }
      if (before_generation == this->nparticles())
        throw std::runtime_error("No particles were generated!");
      console_->info(
          "Generate points:\n# of cells: {}\nExpected # of points: {}\n"
          "# of points generated: {}",
          cells_.size(), cells_.size() * std::pow(nquadratures, Tdim),
          particles_.size());
    } else
      throw std::runtime_error("No cells are found in the mesh!");
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Create particles from coordinates
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_particles(
    const std::string& particle_type, const std::vector<VectorDim>& coordinates,
    unsigned material_id, bool check_duplicates) {
  bool status = true;
  try {
    // Get material
    auto material = materials_.at(material_id);
    // Check if particle coordinates is empty
    if (coordinates.empty())
      throw std::runtime_error("List of coordinates is empty");
    // Iterate over particle coordinates
    for (const auto& particle_coordinates : coordinates) {
      // Particle id
      mpm::Index pid = particles_.size();
      // Create particle
      auto particle = Factory<mpm::ParticleBase<Tdim>, mpm::Index,
                              const Eigen::Matrix<double, Tdim, 1>&>::instance()
                          ->create(particle_type, static_cast<mpm::Index>(pid),
                                   particle_coordinates);

      // Add particle to mesh and check
      bool insert_status = this->add_particle(particle, check_duplicates);

      // If insertion is successful
      if (insert_status)
        map_particles_[pid]->assign_material(material);
      else
        throw std::runtime_error("Addition of particle to mesh failed!");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Add a particle pointer to the mesh
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::add_particle(
    const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle, bool checks) {
  bool status = false;
  try {
    if (checks) {
      // Add only if particle can be located in any cell of the mesh
      if (this->locate_particle_cells(particle)) {
        status = particles_.add(particle, checks);
        particles_cell_ids_.insert(std::pair<mpm::Index, mpm::Index>(
            particle->id(), particle->cell_id()));
        map_particles_.insert(particle->id(), particle);
      } else {
        throw std::runtime_error("Particle not found in mesh");
      }
    } else {
      status = particles_.add(particle, checks);
      particles_cell_ids_.insert(std::pair<mpm::Index, mpm::Index>(
          particle->id(), particle->cell_id()));
      map_particles_.insert(particle->id(), particle);
    }
    if (!status) throw std::runtime_error("Particle addition failed");
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Remove a particle pointer from the mesh
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::remove_particle(
    const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle) {
  const mpm::Index id = particle->id();
  // Remove associated cell for the particle
  map_particles_[id]->remove_cell();
  // Remove a particle if found in the container and map
  return (particles_.remove(particle) && map_particles_.remove(id));
}

//! Remove a particle by id
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::remove_particle_by_id(mpm::Index id) {
  // Remove associated cell for the particle
  map_particles_[id]->remove_cell();
  bool result = particles_.remove(map_particles_[id]);
  return (result && map_particles_.remove(id));
}

//! Remove a particle by id
template <unsigned Tdim>
void mpm::Mesh<Tdim>::remove_particles(const std::vector<mpm::Index>& pids) {
  if (!pids.empty()) {
    // Get MPI rank
    int mpi_size = 1;
#ifdef USE_MPI
    MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
#endif
    for (auto& id : pids) {
      map_particles_[id]->remove_cell();
      map_particles_.remove(id);
    }

    // Get number of particles to reserve size
    unsigned nparticles = this->nparticles();
    // Clear particles and start a new element of particles
    particles_.clear();
    particles_.reserve(static_cast<int>(nparticles / mpi_size));
    // Iterate over the map of particles and add them to container
    for (auto& particle : map_particles_)
      particles_.add(particle.second, false);
  }
}

//! Remove all particles in a cell given cell id
template <unsigned Tdim>
void mpm::Mesh<Tdim>::remove_all_nonrank_particles() {
  // Get MPI rank
  int mpi_rank = 0;
  int mpi_size = 1;
#ifdef USE_MPI
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
#endif

  // Remove associated cell for the particle
  for (auto citr = this->cells_.cbegin(); citr != this->cells_.cend(); ++citr) {
    // If cell is non empty
    if ((*citr)->particles().size() != 0 && (*citr)->rank() != mpi_rank) {
      auto pids = (*citr)->particles();
      // Remove particles from map
      for (auto& id : pids) {
        map_particles_[id]->remove_cell();
        map_particles_.remove(id);
      }
      (*citr)->clear_particle_ids();
    }
  }

  // Get number of particles to reserve size
  unsigned nparticles = this->nparticles();
  // Clear particles and start a new element of particles
  particles_.clear();
  particles_.reserve(static_cast<int>(nparticles / mpi_size));
  // Iterate over the map of particles and add them to container
  for (auto& particle : map_particles_) particles_.add(particle.second, false);
}

//! Transfer all particles in cells that are not in local rank
template <unsigned Tdim>
void mpm::Mesh<Tdim>::transfer_nonrank_particles() {
#ifdef USE_MPI
  // Get number of MPI ranks
  int mpi_size;
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
  int mpi_rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

  if (mpi_size > 1) {
    std::vector<MPI_Request> send_requests;
    send_requests.reserve(ghost_cells_.size());
    unsigned i = 0;

    std::vector<mpm::Index> remove_pids;
    // Iterate through the ghost cells and send particles
    for (auto citr = this->ghost_cells_.cbegin();
         citr != this->ghost_cells_.cend(); ++citr, ++i) {
      // Create a vector of h5_particles
      std::vector<mpm::HDF5Particle> h5_particles;
      auto particle_ids = (*citr)->particles();
      // Create a vector of HDF5 data of particles to send
      // delete particle
      for (auto& id : particle_ids) {
        // Append to vector of particles
        h5_particles.emplace_back(map_particles_[id]->hdf5());
        // Particles to be removed from the current rank
        remove_pids.emplace_back(id);
      }
      (*citr)->clear_particle_ids();

      // Send number of particles to receiver rank
      unsigned nparticles = h5_particles.size();
      MPI_Isend(&nparticles, 1, MPI_UNSIGNED, (*citr)->rank(), 0,
                MPI_COMM_WORLD, &send_requests[i]);
      if (nparticles != 0) {
        mpm::HDF5Particle h5_particle;
        // Initialize MPI datatypes and send vector of particles
        MPI_Datatype particle_type =
            mpm::register_mpi_particle_type(h5_particle);
        MPI_Send(h5_particles.data(), nparticles, particle_type,
                 (*citr)->rank(), 0, MPI_COMM_WORLD);
        mpm::deregister_mpi_particle_type(particle_type);
      }
      h5_particles.clear();
    }
    // Remove all sent particles
    this->remove_particles(remove_pids);
    // Send complete
    for (unsigned i = 0; i < this->ghost_cells_.size(); ++i)
      MPI_Wait(&send_requests[i], MPI_STATUS_IGNORE);

    // Iterate through the local ghost cells and receive particles
    for (auto citr = this->local_ghost_cells_.cbegin();
         citr != this->local_ghost_cells_.cend(); ++citr) {
      std::vector<unsigned> neighbour_ranks =
          ghost_cells_neighbour_ranks_[(*citr)->id()];

      for (unsigned i = 0; i < neighbour_ranks.size(); ++i) {
        // MPI status
        MPI_Status recv_status;
        // Receive number of particles
        unsigned nrecv_particles;
        MPI_Recv(&nrecv_particles, 1, MPI_UNSIGNED, neighbour_ranks[i], 0,
                 MPI_COMM_WORLD, &recv_status);

        if (nrecv_particles != 0) {
          std::vector<mpm::HDF5Particle> recv_particles;
          recv_particles.resize(nrecv_particles);
          // Receive the vector of particles
          mpm::HDF5Particle received;
          MPI_Status status_recv;
          MPI_Datatype particle_type =
              mpm::register_mpi_particle_type(received);
          MPI_Recv(recv_particles.data(), nrecv_particles, particle_type,
                   neighbour_ranks[i], 0, MPI_COMM_WORLD, &status_recv);
          mpm::deregister_mpi_particle_type(particle_type);

          // Iterate through n number of received particles
          for (const auto& rparticle : recv_particles) {
            mpm::Index id = 0;
            // Initial particle coordinates
            Eigen::Matrix<double, Tdim, 1> pcoordinates;
            pcoordinates.setZero();

            // Received particle
            auto received_particle =
                std::make_shared<mpm::Particle<Tdim>>(id, pcoordinates);
            // Get material
            auto material = materials_.at(rparticle.material_id);
            // Reinitialise particle from HDF5 data
            received_particle->initialise_particle(rparticle, material);

            // Add particle to mesh
            this->add_particle(received_particle, true);
          }
        }
      }
    }
  }
#endif
}

//! Find shared nodes across MPI domains
template <unsigned Tdim>
void mpm::Mesh<Tdim>::find_domain_shared_nodes() {
  // Clear MPI rank at the nodes
  tbb::parallel_for_each(nodes_.cbegin(), nodes_.cend(),
                         [=](std::shared_ptr<mpm::NodeBase<Tdim>> node) {
                           node->clear_mpi_ranks();
                         });
  // Get MPI rank
  int mpi_rank = 0;
#ifdef USE_MPI
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
#endif
  // Iterate through all the cells
  tbb::parallel_for_each(cells_.cbegin(), cells_.cend(),
                         [=](std::shared_ptr<mpm::Cell<Tdim>> cell) {
                           cell->assign_mpi_rank_to_nodes();
                         });

  this->domain_shared_nodes_.clear();

#ifdef USE_HALO_EXCHANGE
  ncomms_ = 0;
  for (auto nitr = nodes_.cbegin(); nitr != nodes_.cend(); ++nitr) {
    // If node has more than 1 MPI rank
    std::set<unsigned> nodal_mpi_ranks = (*nitr)->mpi_ranks();
    const unsigned nodal_mpi_ranks_size = nodal_mpi_ranks.size();
    if (nodal_mpi_ranks_size > 1) {
      if (nodal_mpi_ranks.find(mpi_rank) != nodal_mpi_ranks.end()) {
        // Create Ghost ID
        (*nitr)->ghost_id(ncomms_);
        // Add to list of shared nodes on local rank
        domain_shared_nodes_.add(*nitr);
        ncomms_ += nodal_mpi_ranks_size - 1;
      }
    }
  }
#else
  nhalo_nodes_ = 0;
  for (auto nitr = nodes_.cbegin(); nitr != nodes_.cend(); ++nitr) {
    std::set<unsigned> nodal_mpi_ranks = (*nitr)->mpi_ranks();
    // If node has more than 1 MPI rank
    if (nodal_mpi_ranks.size() > 1) {
      (*nitr)->ghost_id(nhalo_nodes_);
      nhalo_nodes_ += 1;
      // Add to domain shared nodes only if active on current MPI rank
      if (nodal_mpi_ranks.find(mpi_rank) != nodal_mpi_ranks.end())
        domain_shared_nodes_.add(*nitr);
    }
  }
#endif
}

//! Locate particles in a cell
template <unsigned Tdim>
std::vector<std::shared_ptr<mpm::ParticleBase<Tdim>>>
    mpm::Mesh<Tdim>::locate_particles_mesh() {

  std::vector<std::shared_ptr<mpm::ParticleBase<Tdim>>> particles;

  std::for_each(particles_.cbegin(), particles_.cend(),
                [=, &particles](
                    const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle) {
                  // If particle is not found in mesh add to a list of particles
                  if (!this->locate_particle_cells(particle))
                    particles.emplace_back(particle);
                });

  return particles;
}

//! Locate particles in a cell
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::locate_particle_cells(
    const std::shared_ptr<mpm::ParticleBase<Tdim>>& particle) {
  // Check the current cell if it is not invalid
  if (particle->cell_id() != std::numeric_limits<mpm::Index>::max()) {
    // If a cell id is present, but not a cell locate the cell from map
    if (!particle->cell_ptr())
      particle->assign_cell(map_cells_[particle->cell_id()]);
    if (particle->compute_reference_location()) return true;

    // Check if material point is in any of its nearest neighbours
    const auto neighbours = map_cells_[particle->cell_id()]->neighbours();
    Eigen::Matrix<double, Tdim, 1> xi;
    Eigen::Matrix<double, Tdim, 1> coordinates = particle->coordinates();
    for (auto neighbour : neighbours) {
      if (map_cells_[neighbour]->is_point_in_cell(coordinates, &xi)) {
        particle->assign_cell_xi(map_cells_[neighbour], xi);
        return true;
      }
    }
  }

  bool status = false;
  tbb::parallel_for_each(
      cells_.cbegin(), cells_.cend(),
      [=, &status](const std::shared_ptr<mpm::Cell<Tdim>>& cell) {
        // Check if particle is already found, if so don't run for other cells
        // Check if co-ordinates is within the cell, if true
        // add particle to cell
        Eigen::Matrix<double, Tdim, 1> xi;
        if (!status && cell->is_point_in_cell(particle->coordinates(), &xi)) {
          particle->assign_cell_xi(cell, xi);
          status = true;
        }
      });

  return status;
}

//! Iterate over particles
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_particles(Toper oper) {
  tbb::parallel_for(tbb::blocked_range<int>(
                        size_t(0), size_t(particles_.size()), tbb_grain_size_),
                    [&](const tbb::blocked_range<int>& range) {
                      for (int i = range.begin(); i != range.end(); ++i)
                        oper(particles_[i]);
                    },
                    tbb::simple_partitioner());
}

//! Iterate over particle set
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_particle_set(int set_id, Toper oper) {
  // If set id is -1, use all particles
  if (set_id == -1) {
    tbb::parallel_for(
        tbb::blocked_range<int>(size_t(0), size_t(particles_.size()),
                                tbb_grain_size_),
        [&](const tbb::blocked_range<int>& range) {
          for (int i = range.begin(); i != range.end(); ++i)
            oper(particles_[i]);
        },
        tbb::simple_partitioner());
  } else {
    // Iterate over the particle set
    auto set = particle_sets_.at(set_id);
    tbb::parallel_for(
        tbb::blocked_range<int>(size_t(0), size_t(set.size()), tbb_grain_size_),
        [&](const tbb::blocked_range<int>& range) {
          for (int i = range.begin(); i != range.end(); ++i) {
            unsigned id = set[i];
            if (map_particles_.find(id) != map_particles_.end())
              oper(map_particles_[id]);
          }
        },
        tbb::simple_partitioner());
  }
}

//! Add a neighbour mesh, using the local id of the mesh and a mesh pointer
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::add_neighbour(
    unsigned local_id, const std::shared_ptr<mpm::Mesh<Tdim>>& mesh) {
  bool insertion_status = false;
  try {
    // If the mesh id is not the current mesh id
    if (mesh->id() != this->id()) {
      insertion_status = neighbour_meshes_.insert(local_id, mesh);
    } else {
      throw std::runtime_error("Invalid local id of a mesh neighbour");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
  }
  return insertion_status;
}

//! Return particle coordinates
template <unsigned Tdim>
std::vector<Eigen::Matrix<double, 3, 1>>
    mpm::Mesh<Tdim>::particle_coordinates() {
  std::vector<Eigen::Matrix<double, 3, 1>> particle_coordinates;
  for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr) {
    Eigen::Vector3d coordinates;
    coordinates.setZero();
    auto pcoords = (*pitr)->coordinates();
    // Fill coordinates to the size of dimensions
    for (unsigned i = 0; i < Tdim; ++i) coordinates(i) = pcoords(i);
    particle_coordinates.emplace_back(coordinates);
  }
  return particle_coordinates;
}

//! Return particle vector data
template <unsigned Tdim>
std::vector<Eigen::Matrix<double, 3, 1>> mpm::Mesh<Tdim>::particles_vector_data(
    const std::string& attribute) {
  std::vector<Eigen::Matrix<double, 3, 1>> vector_data;
  try {
    // Iterate over particles
    for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr) {
      Eigen::Vector3d data;
      data.setZero();
      auto pdata = (*pitr)->vector_data(attribute);
      // Fill stresses to the size of dimensions
      for (unsigned i = 0; i < Tdim; ++i) data(i) = pdata(i);

      // Add to a vector of data
      vector_data.emplace_back(data);
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {} {}\n", __FILE__, __LINE__, exception.what(),
                    attribute);
    vector_data.clear();
  }
  return vector_data;
}

//! Return particle scalar data
template <unsigned Tdim>
std::vector<double> mpm::Mesh<Tdim>::particles_statevars_data(
    const std::string& attribute) {
  std::vector<double> scalar_data;
  scalar_data.reserve(particles_.size());
  // Iterate over particles and add scalar value to data
  for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr)
    scalar_data.emplace_back((*pitr)->state_variable(attribute));
  return scalar_data;
}

//! Assign particles volumes
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_particles_volumes(
    const std::vector<std::tuple<mpm::Index, double>>& particle_volumes) {
  bool status = true;
  const unsigned phase = 0;
  try {
    if (!particles_.size())
      throw std::runtime_error(
          "No particles have been assigned in mesh, cannot assign volume");

    for (const auto& particle_volume : particle_volumes) {
      // Particle id
      mpm::Index pid = std::get<0>(particle_volume);
      // Volume
      double volume = std::get<1>(particle_volume);

      if (map_particles_.find(pid) != map_particles_.end())
        status = map_particles_[pid]->assign_volume(volume);

      if (!status)
        throw std::runtime_error("Cannot assign invalid particle volume");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Compute and assign rotation matrix to nodes
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::compute_nodal_rotation_matrices(
    const std::map<mpm::Index, Eigen::Matrix<double, Tdim, 1>>& euler_angles) {
  bool status = false;
  try {
    if (!nodes_.size())
      throw std::runtime_error(
          "No nodes have been assigned in mesh, cannot assign rotation "
          "matrix");

    // Loop through nodal_euler_angles of different nodes
    for (const auto& nodal_euler_angles : euler_angles) {
      // Node id
      mpm::Index nid = nodal_euler_angles.first;
      // Euler angles
      Eigen::Matrix<double, Tdim, 1> angles = nodal_euler_angles.second;
      // Compute rotation matrix
      const auto rotation_matrix = mpm::geometry::rotation_matrix(angles);

      // Apply rotation matrix to nodes
      map_nodes_[nid]->assign_rotation_matrix(rotation_matrix);
      status = true;
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Create particle tractions
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_particles_tractions(
    const std::shared_ptr<FunctionBase>& mfunction, int set_id, unsigned dir,
    double traction) {
  bool status = true;
  try {
    if (set_id == -1 || particle_sets_.find(set_id) != particle_sets_.end())
      // Create a particle traction load
      particle_tractions_.emplace_back(
          std::make_shared<mpm::Traction>(set_id, mfunction, dir, traction));
    else
      throw std::runtime_error("No particle set found to assign traction");

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Apply particle tractions
template <unsigned Tdim>
void mpm::Mesh<Tdim>::apply_traction_on_particles(double current_time) {
  // Iterate over all particle tractions
  for (const auto& ptraction : particle_tractions_) {
    int set_id = ptraction->setid();
    unsigned dir = ptraction->dir();
    double traction = ptraction->traction(current_time);
    this->iterate_over_particle_set(
        set_id, std::bind(&mpm::ParticleBase<Tdim>::assign_traction,
                          std::placeholders::_1, dir, traction));
  }
  if (!particle_tractions_.empty()) {
    this->iterate_over_particles(std::bind(
        &mpm::ParticleBase<Tdim>::map_traction_force, std::placeholders::_1));
  }
}

//! Create particle velocity constraints
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_particle_velocity_constraint(
    int set_id, const std::shared_ptr<mpm::VelocityConstraint>& constraint) {
  bool status = true;
  try {
    if (set_id == -1 || particle_sets_.find(set_id) != particle_sets_.end()) {
      // Create a particle velocity constraint
      if (constraint->dir() < Tdim)
        particle_velocity_constraints_.emplace_back(constraint);
      else
        throw std::runtime_error("Invalid direction of velocity constraint");
    } else
      throw std::runtime_error(
          "No particle set found to assign velocity constraint");

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Apply particle tractions
template <unsigned Tdim>
void mpm::Mesh<Tdim>::apply_particle_velocity_constraints() {
  // Iterate over all particle velocity constraints
  for (const auto& pvelocity : particle_velocity_constraints_) {
    // If set id is -1, use all particles
    int set_id = pvelocity->setid();
    unsigned dir = pvelocity->dir();
    double velocity = pvelocity->velocity();

    this->iterate_over_particle_set(
        set_id,
        std::bind(&mpm::ParticleBase<Tdim>::apply_particle_velocity_constraints,
                  std::placeholders::_1, dir, velocity));
  }
}

//! Assign nodal velocity constraints
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_nodal_velocity_constraint(
    int set_id, const std::shared_ptr<mpm::VelocityConstraint>& vconstraint) {
  bool status = true;
  try {
    if (set_id == -1 || node_sets_.find(set_id) != node_sets_.end()) {
      int set_id = vconstraint->setid();
      // If set id is -1, use all nodes
      auto nset = (set_id == -1) ? this->nodes_ : node_sets_.at(set_id);
      unsigned dir = vconstraint->dir();
      double velocity = vconstraint->velocity();
      tbb::parallel_for(
          tbb::blocked_range<int>(size_t(0), size_t(nset.size()),
                                  tbb_grain_size_),
          [&](const tbb::blocked_range<int>& range) {
            for (int i = range.begin(); i != range.end(); ++i) {
              status = nset[i]->assign_velocity_constraint(dir, velocity);
              if (!status)
                throw std::runtime_error(
                    "Failed to initialise velocity constraint at node");
            }
          },
          tbb::simple_partitioner());
    } else
      throw std::runtime_error("No node set found to assign velocity con");

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Assign friction constraints to nodes
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_nodal_frictional_constraint(
    int nset_id, const std::shared_ptr<mpm::FrictionConstraint>& fconstraint) {
  bool status = false;
  try {
    if (nset_id == -1 || node_sets_.find(nset_id) != node_sets_.end()) {
      int set_id = fconstraint->setid();
      // If set id is -1, use all nodes
      auto nset = (set_id == -1) ? this->nodes_ : node_sets_.at(set_id);
      unsigned dir = fconstraint->dir();
      int nsign_n = fconstraint->sign_n();
      double friction = fconstraint->friction();
      tbb::parallel_for(
          tbb::blocked_range<int>(size_t(0), size_t(nset.size()),
                                  tbb_grain_size_),
          [&](const tbb::blocked_range<int>& range) {
            for (int i = range.begin(); i != range.end(); ++i) {
              status =
                  nset[i]->assign_friction_constraint(dir, nsign_n, friction);
              if (!status)
                throw std::runtime_error(
                    "Failed to initialise velocity constraint at node");
            }
          },
          tbb::simple_partitioner());
    } else
      throw std::runtime_error("No node set found to assign velocity con");

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Assign node tractions
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_nodal_concentrated_forces(
    const std::shared_ptr<FunctionBase>& mfunction, int set_id, unsigned dir,
    double concentrated_force) {
  bool status = true;
  // TODO: Remove phase
  const unsigned phase = 0;
  try {
    if (!nodes_.size())
      throw std::runtime_error(
          "No nodes have been assigned in mesh, cannot assign concentrated "
          "force");

    // Set id of -1, is all nodes
    Container<NodeBase<Tdim>> nodes =
        (set_id == -1) ? this->nodes_ : node_sets_.at(set_id);

    tbb::parallel_for(
        tbb::blocked_range<int>(size_t(0), size_t(nodes.size()),
                                tbb_grain_size_),
        [&](const tbb::blocked_range<int>& range) {
          for (int i = range.begin(); i != range.end(); ++i) {
            if (!nodes[i]->assign_concentrated_force(
                    phase, dir, concentrated_force, mfunction))
              throw std::runtime_error("Setting concentrated force failed");
          }
        },
        tbb::simple_partitioner());

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Assign particle stresses
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_particles_stresses(
    const std::vector<Eigen::Matrix<double, 6, 1>>& particle_stresses) {
  bool status = true;
  // TODO: Remove phase
  const unsigned phase = 0;
  try {
    if (!particles_.size())
      throw std::runtime_error(
          "No particles have been assigned in mesh, cannot assign stresses");

    if (particles_.size() != particle_stresses.size())
      throw std::runtime_error(
          "Number of particles in mesh and initial stresses don't match");

    unsigned i = 0;
    for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr) {
      (*pitr)->initial_stress(particle_stresses.at(i));
      ++i;
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Assign particle cells
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_particles_cells(
    const std::vector<std::array<mpm::Index, 2>>& particles_cells) {
  bool status = true;
  try {
    if (!particles_.size())
      throw std::runtime_error(
          "No particles have been assigned in mesh, cannot assign cells");
    for (const auto& particle_cell : particles_cells) {
      // Particle id
      mpm::Index pid = particle_cell[0];
      // Cell id
      mpm::Index cid = particle_cell[1];

      map_particles_[pid]->assign_cell_id(cid);
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Return particle cells
template <unsigned Tdim>
std::vector<std::array<mpm::Index, 2>> mpm::Mesh<Tdim>::particles_cells()
    const {
  std::vector<std::array<mpm::Index, 2>> particles_cells;
  try {
    if (!particles_.size())
      throw std::runtime_error(
          "No particles have been assigned in mesh, cannot write cells");
    for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr) {
      if ((*pitr)->cell_id() != std::numeric_limits<mpm::Index>::max())
        particles_cells.emplace_back(
            std::array<mpm::Index, 2>({(*pitr)->id(), (*pitr)->cell_id()}));
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    particles_cells.clear();
  }
  return particles_cells;
}

//! Write particles to HDF5
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::write_particles_hdf5(unsigned phase,
                                           const std::string& filename) {
  const unsigned nparticles = this->nparticles();

  std::vector<HDF5Particle> particle_data;  // = new HDF5Particle[nparticles];
  particle_data.reserve(nparticles);

  for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr)
    particle_data.emplace_back((*pitr)->hdf5());

  // Calculate the size and the offsets of our struct members in memory
  const hsize_t NRECORDS = nparticles;

  const hsize_t NFIELDS = mpm::hdf5::particle::NFIELDS;

  hid_t string_type;
  hid_t file_id;
  hsize_t chunk_size = 10000;
  int* fill_data = NULL;
  int compress = 0;

  // Create a new file using default properties.
  file_id =
      H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

  // make a table
  H5TBmake_table(
      "Table Title", file_id, "table", NFIELDS, NRECORDS,
      mpm::hdf5::particle::dst_size, mpm::hdf5::particle::field_names,
      mpm::hdf5::particle::dst_offset, mpm::hdf5::particle::field_type,
      chunk_size, fill_data, compress, particle_data.data());

  H5Fclose(file_id);
  return true;
}

//! Write particles to HDF5
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::read_particles_hdf5(unsigned phase,
                                          const std::string& filename) {

  // Create a new file using default properties.
  hid_t file_id = H5Fopen(filename.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  // Throw an error if file can't be found
  if (file_id < 0) throw std::runtime_error("HDF5 particle file is not found");

  // Calculate the size and the offsets of our struct members in memory
  const unsigned nparticles = this->nparticles();
  const hsize_t NRECORDS = nparticles;

  const hsize_t NFIELDS = mpm::hdf5::particle::NFIELDS;

  std::vector<HDF5Particle> dst_buf;
  dst_buf.reserve(nparticles);
  // Read the table
  H5TBread_table(file_id, "table", mpm::hdf5::particle::dst_size,
                 mpm::hdf5::particle::dst_offset,
                 mpm::hdf5::particle::dst_sizes, dst_buf.data());

  unsigned i = 0;
  for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr) {
    HDF5Particle particle = dst_buf[i];
    // Get particle's material from list of materials
    auto material = materials_.at(particle.material_id);
    // Initialise particle with HDF5 data
    (*pitr)->initialise_particle(particle, material);
    ++i;
  }
  // close the file
  H5Fclose(file_id);
  return true;
}

//! Write particles to HDF5
template <unsigned Tdim>
std::vector<mpm::HDF5Particle> mpm::Mesh<Tdim>::particles_hdf5() const {
  const unsigned nparticles = this->nparticles();

  std::vector<mpm::HDF5Particle> particles_hdf5;
  particles_hdf5.reserve(nparticles);

  for (auto pitr = particles_.cbegin(); pitr != particles_.cend(); ++pitr)
    particles_hdf5.emplace_back((*pitr)->hdf5());

  return particles_hdf5;
}

//! Nodal coordinates
template <unsigned Tdim>
std::vector<Eigen::Matrix<double, 3, 1>> mpm::Mesh<Tdim>::nodal_coordinates()
    const {

  // Nodal coordinates
  std::vector<Eigen::Matrix<double, 3, 1>> coordinates;
  coordinates.reserve(nodes_.size());

  try {
    if (nodes_.size() == 0)
      throw std::runtime_error("No nodes have been initialised!");

    // Fill nodal coordinates
    for (auto nitr = nodes_.cbegin(); nitr != nodes_.cend(); ++nitr) {
      // initialise coordinates
      Eigen::Matrix<double, 3, 1> node;
      node.setZero();
      auto coords = (*nitr)->coordinates();

      for (unsigned i = 0; i < coords.size(); ++i) node(i) = coords(i);

      coordinates.emplace_back(node);
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    coordinates.clear();
  }
  return coordinates;
}

//! Cell node pairs
template <unsigned Tdim>
std::vector<std::array<mpm::Index, 2>> mpm::Mesh<Tdim>::node_pairs() const {
  // Vector of node_pairs
  std::vector<std::array<mpm::Index, 2>> node_pairs;

  try {
    if (cells_.size() == 0)
      throw std::runtime_error("No cells have been initialised!");

    for (auto citr = cells_.cbegin(); citr != cells_.cend(); ++citr) {
      const auto pairs = (*citr)->side_node_pairs();
      node_pairs.insert(std::end(node_pairs), std::begin(pairs),
                        std::end(pairs));
    }

  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    node_pairs.clear();
  }
  return node_pairs;
}

//! Create map of container of particles in sets
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_particle_sets(
    const tsl::robin_map<mpm::Index, std::vector<mpm::Index>>& particle_sets,
    bool check_duplicates) {
  bool status = false;
  try {
    // Create container for each particle set
    for (auto sitr = particle_sets.begin(); sitr != particle_sets.end();
         ++sitr) {
      // Create a container for the set
      tbb::concurrent_vector<mpm::Index> particles((sitr->second).begin(),
                                                   (sitr->second).end());

      // Create the map of the container
      status =
          this->particle_sets_
              .insert(std::pair<mpm::Index, tbb::concurrent_vector<mpm::Index>>(
                  sitr->first, particles))
              .second;
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
  }
  return status;
}

//! Create map of container of nodes in sets
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_node_sets(
    const tsl::robin_map<mpm::Index, std::vector<mpm::Index>>& node_sets,
    bool check_duplicates) {
  bool status = false;
  try {
    // Create container for each node set
    for (auto sitr = node_sets.begin(); sitr != node_sets.end(); ++sitr) {
      // Create a container for the set
      Container<NodeBase<Tdim>> nodes;
      // Reserve the size of the container
      nodes.reserve((sitr->second).size());
      // Add nodes to the container
      for (auto pid : sitr->second) {
        bool insertion_status = nodes.add(map_nodes_[pid], check_duplicates);
      }

      // Create the map of the container
      status = this->node_sets_
                   .insert(std::pair<mpm::Index, Container<NodeBase<Tdim>>>(
                       sitr->first, nodes))
                   .second;
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
  }
  return status;
}

// Return cells
template <unsigned Tdim>
mpm::Container<mpm::Cell<Tdim>> mpm::Mesh<Tdim>::cells() {
  return this->cells_;
}

//! Create map of container of cells in sets
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::create_cell_sets(
    const tsl::robin_map<mpm::Index, std::vector<mpm::Index>>& cell_sets,
    bool check_duplicates) {
  bool status = false;
  try {
    // Create container for each cell set
    for (auto sitr = cell_sets.begin(); sitr != cell_sets.end(); ++sitr) {
      // Create a container for the set
      Container<Cell<Tdim>> cells;
      // Reserve the size of the container
      cells.reserve((sitr->second).size());
      // Add cells to the container
      for (auto pid : sitr->second) {
        bool insertion_status = cells.add(map_cells_[pid], check_duplicates);
      }

      // Create the map of the container
      status = this->cell_sets_
                   .insert(std::pair<mpm::Index, Container<Cell<Tdim>>>(
                       sitr->first, cells))
                   .second;
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
  }
  return status;
}

//! return particle_ptr
template <unsigned Tdim>
std::map<mpm::Index, mpm::Index>* mpm::Mesh<Tdim>::particles_cell_ids() {
  return &(this->particles_cell_ids_);
}

//! Generate particles
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::generate_particles(const std::shared_ptr<mpm::IO>& io,
                                         const Json& generator) {
  bool status = true;
  try {
    // Particle generator
    const auto generator_type = generator["type"].template get<std::string>();

    // Generate particles from file
    if (generator_type == "file") {
      status = this->read_particles_file(io, generator);
    }

    // Generate material points at the Gauss location in all cells
    else if (generator_type == "gauss") {
      // Number of particles per dir
      unsigned nparticles_dir =
          generator["nparticles_per_dir"].template get<unsigned>();
      // Particle type
      auto particle_type =
          generator["particle_type"].template get<std::string>();
      // Material id
      unsigned material_id = generator["material_id"].template get<unsigned>();
      // Cell set id
      int cset_id = generator["cset_id"].template get<int>();
      status = this->generate_material_points(nparticles_dir, particle_type,
                                              material_id, cset_id);
    }

    else
      throw std::runtime_error(
          "Particle generator type is not properly specified");

  } catch (std::exception& exception) {
    console_->error("{}: #{} Generating particle failed", __FILE__, __LINE__);
    status = false;
  }
  return status;
}

// Read particles file
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::read_particles_file(const std::shared_ptr<mpm::IO>& io,
                                          const Json& generator) {
  // Particle type
  auto particle_type = generator["particle_type"].template get<std::string>();

  // File location
  auto file_loc =
      io->file_name(generator["location"].template get<std::string>());

  // Check duplicates
  bool check_duplicates = generator["check_duplicates"].template get<bool>();

  // Material id
  unsigned material_id = generator["material_id"].template get<unsigned>();

  const std::string reader = generator["io_type"].template get<std::string>();

  // Create a particle reader
  auto particle_io = Factory<mpm::IOMesh<Tdim>>::instance()->create(reader);

  // Get coordinates
  auto coords = particle_io->read_particles(file_loc);

  // Create particles from coordinates
  bool status = this->create_particles(particle_type, coords, material_id,
                                       check_duplicates);

  if (!status) throw std::runtime_error("Addition of particles to mesh failed");

  return status;
}

//! Assign nodal concentrated force
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_nodal_concentrated_forces(
    const std::vector<std::tuple<mpm::Index, unsigned, double>>& nodal_forces) {
  bool status = true;
  // TODO: Remove phase
  const unsigned phase = 0;
  try {
    if (!nodes_.size())
      throw std::runtime_error(
          "No nodes have been assigned in mesh, cannot assign traction");
    for (const auto& nodal_force : nodal_forces) {
      // Node id
      mpm::Index pid = std::get<0>(nodal_force);
      // Direction
      unsigned dir = std::get<1>(nodal_force);
      // Force
      double force = std::get<2>(nodal_force);

      if (map_nodes_.find(pid) != map_nodes_.end())
        status = map_nodes_[pid]->assign_concentrated_force(phase, dir, force,
                                                            nullptr);

      if (!status) throw std::runtime_error("Force is invalid for node");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Assign velocity constraints to nodes
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_nodal_velocity_constraints(
    const std::vector<std::tuple<mpm::Index, unsigned, double>>&
        velocity_constraints) {
  bool status = false;
  try {
    if (!nodes_.size())
      throw std::runtime_error(
          "No nodes have been assigned in mesh, cannot assign velocity "
          "constraints");

    for (const auto& velocity_constraint : velocity_constraints) {
      // Node id
      mpm::Index nid = std::get<0>(velocity_constraint);
      // Direction
      unsigned dir = std::get<1>(velocity_constraint);
      // Velocity
      double velocity = std::get<2>(velocity_constraint);

      // Apply constraint
      status = map_nodes_[nid]->assign_velocity_constraint(dir, velocity);

      if (!status)
        throw std::runtime_error("Node or velocity constraint is invalid");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Assign friction constraints to nodes
template <unsigned Tdim>
bool mpm::Mesh<Tdim>::assign_nodal_friction_constraints(
    const std::vector<std::tuple<mpm::Index, unsigned, int, double>>&
        friction_constraints) {
  bool status = false;
  try {
    if (!nodes_.size())
      throw std::runtime_error(
          "No nodes have been assigned in mesh, cannot assign friction "
          "constraints");

    for (const auto& friction_constraint : friction_constraints) {
      // Node id
      mpm::Index nid = std::get<0>(friction_constraint);
      // Direction
      unsigned dir = std::get<1>(friction_constraint);
      // Sign
      int sign = std::get<2>(friction_constraint);
      // Friction
      double friction = std::get<3>(friction_constraint);

      // Apply constraint
      status = map_nodes_[nid]->assign_friction_constraint(dir, sign, friction);

      if (!status)
        throw std::runtime_error("Node or friction constraint is invalid");
    }
  } catch (std::exception& exception) {
    console_->error("{} #{}: {}\n", __FILE__, __LINE__, exception.what());
    status = false;
  }
  return status;
}

//! Iterate over boundary particles
//! TODO: parallel
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_boundary_particles(Toper oper) {
  std::for_each(boundary_particles_.cbegin(), boundary_particles_.cend(), oper);
}

//! Iterate over boundary segments
//! TODO:parallel
template <unsigned Tdim>
template <typename Toper>
void mpm::Mesh<Tdim>::iterate_over_boundary_segments(Toper oper) {
  std::for_each(boundary_segments_.cbegin(), boundary_segments_.cend(), oper);
}
