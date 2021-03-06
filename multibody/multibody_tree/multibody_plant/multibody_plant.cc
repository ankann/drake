#include "drake/multibody/multibody_tree/multibody_plant/multibody_plant.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "drake/common/default_scalars.h"
#include "drake/common/drake_throw.h"
#include "drake/geometry/frame_kinematics_vector.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/geometry_instance.h"
#include "drake/math/orthonormal_basis.h"

namespace drake {
namespace multibody {
namespace multibody_plant {

// Helper macro to throw an exception within methods that should not be called
// post-finalize.
#define DRAKE_MBP_THROW_IF_FINALIZED() ThrowIfFinalized(__func__)

// Helper macro to throw an exception within methods that should not be called
// pre-finalize.
#define DRAKE_MBP_THROW_IF_NOT_FINALIZED() ThrowIfNotFinalized(__func__)

using geometry::FrameId;
using geometry::FramePoseVector;
using geometry::GeometryFrame;
using geometry::GeometryId;
using geometry::GeometryInstance;
using geometry::PenetrationAsPointPair;
using geometry::SceneGraph;
using geometry::SourceId;
using systems::InputPortDescriptor;
using systems::OutputPort;
using systems::State;

using drake::multibody::MultibodyForces;
using drake::multibody::MultibodyTree;
using drake::multibody::MultibodyTreeContext;
using drake::multibody::PositionKinematicsCache;
using drake::multibody::SpatialAcceleration;
using drake::multibody::SpatialForce;
using drake::multibody::VelocityKinematicsCache;
using systems::BasicVector;
using systems::Context;
using systems::InputPortDescriptor;
using systems::InputPortIndex;
using systems::OutputPortIndex;

template<typename T>
MultibodyPlant<T>::MultibodyPlant(double time_step) :
    systems::LeafSystem<T>(systems::SystemTypeTag<
        drake::multibody::multibody_plant::MultibodyPlant>()),
    time_step_(time_step) {
  DRAKE_THROW_UNLESS(time_step >= 0);
  model_ = std::make_unique<MultibodyTree<T>>();
  visual_geometries_.emplace_back();  // Entries for the "world" body.
  collision_geometries_.emplace_back();
}

template <typename T>
geometry::SourceId MultibodyPlant<T>::RegisterAsSourceForSceneGraph(
    SceneGraph<T>* scene_graph) {
  DRAKE_THROW_UNLESS(scene_graph != nullptr);
  DRAKE_THROW_UNLESS(!geometry_source_is_registered());
  source_id_ = scene_graph->RegisterSource();
  // Save the GS pointer so that on later geometry registrations we can verify
  // the user is making calls on the same GS instance. Only used for that
  // purpose, it gets nullified at Finalize().
  scene_graph_ = scene_graph;
  return source_id_.value();
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterVisualGeometry(
    const Body<T>& body, const Isometry3<double>& X_BG,
    const geometry::Shape& shape, geometry::SceneGraph<T>* scene_graph) {
  return RegisterVisualGeometry(body, X_BG, shape, geometry::VisualMaterial(),
                                scene_graph);
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterVisualGeometry(
    const Body<T>& body, const Isometry3<double>& X_BG,
    const geometry::Shape& shape, const geometry::VisualMaterial& material,
    SceneGraph<T>* scene_graph) {
  // TODO(SeanCurtis-TRI): Consider simply adding an interface that takes a
  // unique pointer to an already instantiated GeometryInstance. This will
  // require shuffling around a fair amount of code and should ultimately be
  // supplanted by providing a cleaner interface between parsing MBP and SG
  // elements.
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(scene_graph != nullptr);
  DRAKE_THROW_UNLESS(geometry_source_is_registered());
  if (scene_graph != scene_graph_) {
    throw std::logic_error(
        "Geometry registration calls must be performed on the SAME instance of "
        "SceneGraph used on the first call to "
        "RegisterAsSourceForSceneGraph()");
  }
  GeometryId id;
  // TODO(amcastro-tri): Consider doing this after finalize so that we can
  // register anchored geometry on ANY body welded to the world.
  if (body.index() == world_index()) {
    id = RegisterAnchoredGeometry(X_BG, shape, material, scene_graph);
  } else {
    id = RegisterGeometry(body, X_BG, shape, material, scene_graph);
  }
  const int visual_index = geometry_id_to_visual_index_.size();
  geometry_id_to_visual_index_[id] = visual_index;
  DRAKE_ASSERT(num_bodies() == static_cast<int>(visual_geometries_.size()));
  visual_geometries_[body.index()].push_back(id);
  return id;
}

template <typename T>
const std::vector<geometry::GeometryId>&
MultibodyPlant<T>::GetVisualGeometriesForBody(const Body<T>& body) const {
  return visual_geometries_[body.index()];
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterCollisionGeometry(
    const Body<T>& body, const Isometry3<double>& X_BG,
    const geometry::Shape& shape,
    const CoulombFriction<double>& coulomb_friction,
    SceneGraph<T>* scene_graph) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(scene_graph != nullptr);
  DRAKE_THROW_UNLESS(geometry_source_is_registered());
  if (scene_graph != scene_graph_) {
    throw std::logic_error(
        "Geometry registration calls must be performed on the SAME instance of "
        "SceneGraph used on the first call to "
        "RegisterAsSourceForSceneGraph()");
  }
  GeometryId id;
  // TODO(amcastro-tri): Consider doing this after finalize so that we can
  // register anchored geometry on ANY body welded to the world.
  if (body.index() == world_index()) {
    id = RegisterAnchoredGeometry(X_BG, shape, {}, scene_graph);
  } else {
    id = RegisterGeometry(body, X_BG, shape, {}, scene_graph);
  }
  const int collision_index = geometry_id_to_collision_index_.size();
  geometry_id_to_collision_index_[id] = collision_index;
  DRAKE_ASSERT(
      static_cast<int>(default_coulomb_friction_.size()) == collision_index);
  default_coulomb_friction_.push_back(coulomb_friction);
  DRAKE_ASSERT(num_bodies() == static_cast<int>(collision_geometries_.size()));
  collision_geometries_[body.index()].push_back(id);
  return id;
}

template <typename T>
const std::vector<geometry::GeometryId>&
MultibodyPlant<T>::GetCollisionGeometriesForBody(const Body<T>& body) const {
  DRAKE_ASSERT(body.index() < num_bodies());
  return collision_geometries_[body.index()];
}

template <typename T>
geometry::GeometrySet MultibodyPlant<T>::CollectRegisteredGeometries(
    const std::vector<const RigidBody<T>*>& bodies) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(geometry_source_is_registered());

  geometry::GeometrySet geometry_set;
  for (const RigidBody<T>* body : bodies) {
    optional<FrameId> frame_id = GetBodyFrameIdIfExists(body->index());
    if (frame_id) {
      geometry_set.Add(frame_id.value());
    } else if (body->index() == world_index()) {
      // TODO(SeanCurtis-TRI): MBP shouldn't be storing these GeometryIds.
      // Remove this when SG supports world frame id that can be mapped to
      // MBP's world body.
      geometry_set.Add(collision_geometries_[body->index()]);
    }
  }
  return geometry_set;
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterGeometry(
    const Body<T>& body, const Isometry3<double>& X_BG,
    const geometry::Shape& shape,
    const optional<geometry::VisualMaterial>& material,
    SceneGraph<T>* scene_graph) {
  // This should never be called with the world index.
  DRAKE_DEMAND(body.index() != world_index());
  DRAKE_ASSERT(!is_finalized());
  DRAKE_ASSERT(geometry_source_is_registered());
  DRAKE_ASSERT(scene_graph == scene_graph_);
  // If not already done, register a frame for this body.
  if (!body_has_registered_frame(body)) {
    body_index_to_frame_id_[body.index()] = scene_graph->RegisterFrame(
        source_id_.value(),
        GeometryFrame(
            body.name(),
            /* Initial pose: Not really used by GS. Will get removed. */
            Isometry3<double>::Identity()));
  }

  // Register geometry in the body frame.
  std::unique_ptr<geometry::GeometryInstance> geometry_instance;
  if (material) {
    geometry_instance = std::make_unique<GeometryInstance>(X_BG, shape.Clone(),
                                                           material.value());
  } else {
    geometry_instance = std::make_unique<GeometryInstance>(X_BG, shape.Clone());
  }
  GeometryId geometry_id = scene_graph->RegisterGeometry(
      source_id_.value(), body_index_to_frame_id_[body.index()],
      std::move(geometry_instance));
  geometry_id_to_body_index_[geometry_id] = body.index();
  return geometry_id;
}

template <typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterAnchoredGeometry(
    const Isometry3<double>& X_WG, const geometry::Shape& shape,
    const optional<geometry::VisualMaterial>& material,
    SceneGraph<T>* scene_graph) {
  DRAKE_ASSERT(!is_finalized());
  DRAKE_ASSERT(geometry_source_is_registered());
  DRAKE_ASSERT(scene_graph == scene_graph_);

  std::unique_ptr<geometry::GeometryInstance> geometry_instance;
  if (material) {
    geometry_instance = std::make_unique<GeometryInstance>(X_WG, shape.Clone(),
                                                           material.value());
  } else {
    geometry_instance = std::make_unique<GeometryInstance>(X_WG, shape.Clone());
  }
  GeometryId geometry_id = scene_graph->RegisterAnchoredGeometry(
      source_id_.value(),
      std::move(geometry_instance));
  geometry_id_to_body_index_[geometry_id] = world_index();
  return geometry_id;
}

template<typename T>
void MultibodyPlant<T>::Finalize(geometry::SceneGraph<T>* scene_graph) {
  model_->Finalize();
  FilterAdjacentBodies(scene_graph);
  ExcludeCollisionsWithVisualGeometry(scene_graph);
  FinalizePlantOnly();
}

template<typename T>
void MultibodyPlant<T>::FinalizePlantOnly() {
  DeclareStateAndPorts();
  // Only declare ports to communicate with a SceneGraph if the plant is
  // provided with a valid source id.
  if (source_id_) DeclareSceneGraphPorts();
  DeclareCacheEntries();
  scene_graph_ = nullptr;  // must not be used after Finalize().
  if (num_collision_geometries() > 0 &&
      penalty_method_contact_parameters_.time_scale < 0)
    set_penetration_allowance();
  if (num_collision_geometries() > 0 &&
      stribeck_model_.stiction_tolerance() < 0)
    set_stiction_tolerance();
  // Make a contact solver when the plant is modeled as a discrete system.
  if (is_discrete()) {
    implicit_stribeck_solver_ =
        std::make_unique<implicit_stribeck::ImplicitStribeckSolver<T>>(
            num_velocities());
    // Set the stiction tolerance according to the values set by users with
    // set_stiction_tolerance().
    implicit_stribeck::Parameters solver_parameters;
    solver_parameters.stiction_tolerance =
        stribeck_model_.stiction_tolerance();
    implicit_stribeck_solver_->set_solver_parameters(solver_parameters);
  }
}

template <typename T>
void MultibodyPlant<T>::FilterAdjacentBodies(SceneGraph<T>* scene_graph) {
  if (geometry_source_is_registered()) {
    if (scene_graph == nullptr) {
      throw std::logic_error(
          "This MultibodyPlant has been registered as a SceneGraph geometry "
              "source. Finalize() should be invoked with a pointer to the "
              "SceneGraph instance");
    }

    if (scene_graph != scene_graph_) {
      throw std::logic_error(
          "Finalizing on a SceneGraph instance must be performed on the SAME "
              "instance of SceneGraph used on the first call to "
              "RegisterAsSourceForSceneGraph()");
    }
    // Disallow collisions between adjacent bodies. Adjacency is implied by the
    // existence of a joint between bodies.
    for (JointIndex j{0}; j < model_->num_joints(); ++j) {
      const Joint<T>& joint = model_->get_joint(j);
      const Body<T>& child = joint.child_body();
      const Body<T>& parent = joint.parent_body();
      // TODO(SeanCurtis-TRI): Determine the correct action for a body
      // joined to the world -- should it filter out collisions between the
      // body and all *anchored* geometry? That seems really heavy-handed. So,
      // for now, we skip the joints to the world.
      if (parent.index() == world_index()) continue;
      optional<FrameId> child_id = GetBodyFrameIdIfExists(child.index());
      optional<FrameId> parent_id = GetBodyFrameIdIfExists(parent.index());

      if (child_id && parent_id) {
        scene_graph->ExcludeCollisionsBetween(
            geometry::GeometrySet(*child_id),
            geometry::GeometrySet(*parent_id));
      }
    }
  }
}

template <typename T>
void MultibodyPlant<T>::ExcludeCollisionsWithVisualGeometry(
    geometry::SceneGraph<T>* scene_graph) {
  if (geometry_source_is_registered()) {
    if (scene_graph == nullptr) {
      throw std::logic_error(
          "This MultibodyPlant has been registered as a SceneGraph geometry "
              "source. Finalize() should be invoked with a pointer to the "
              "SceneGraph instance");
    }
    geometry::GeometrySet visual;
    for (const auto& body_geometries : visual_geometries_) {
      visual.Add(body_geometries);
    }
    geometry::GeometrySet collision;
    for (const auto& body_geometries : collision_geometries_) {
      collision.Add(body_geometries);
    }
    scene_graph->ExcludeCollisionsWithin(visual);
    scene_graph->ExcludeCollisionsBetween(visual, collision);
  }
}

template<typename T>
std::unique_ptr<systems::LeafContext<T>>
MultibodyPlant<T>::DoMakeLeafContext() const {
  DRAKE_THROW_UNLESS(is_finalized());
  return std::make_unique<MultibodyTreeContext<T>>(
      model_->get_topology(), is_discrete());
}

template<typename T>
MatrixX<T> MultibodyPlant<T>::CalcNormalSeparationVelocitiesJacobian(
    const Context<T>& context,
    const std::vector<PenetrationAsPointPair<T>>& point_pairs_set) const {
  const int num_contacts = point_pairs_set.size();
  MatrixX<T> N(num_contacts, num_velocities());

  for (int icontact = 0; icontact < num_contacts; ++icontact) {
    const auto& point_pair = point_pairs_set[icontact];

    const GeometryId geometryA_id = point_pair.id_A;
    const GeometryId geometryB_id = point_pair.id_B;

    BodyIndex bodyA_index = geometry_id_to_body_index_.at(geometryA_id);
    const Body<T>& bodyA = model().get_body(bodyA_index);
    BodyIndex bodyB_index = geometry_id_to_body_index_.at(geometryB_id);
    const Body<T>& bodyB = model().get_body(bodyB_index);

    // Penetration depth, > 0 if bodies interpenetrate.
    const Vector3<T>& nhat_BA_W = point_pair.nhat_BA_W;
    const Vector3<T>& p_WCa = point_pair.p_WCa;
    const Vector3<T>& p_WCb = point_pair.p_WCb;

    // Geometric Jacobian for the velocity of the contact point C as moving with
    // body A, s.t.: v_WAc = Jv_WAc * v
    // where v is the vector of generalized velocities.
    MatrixX<T> Jv_WAc(3, this->num_velocities());
    model().CalcPointsGeometricJacobianExpressedInWorld(
        context, bodyA.body_frame(), p_WCa, &Jv_WAc);

    // Geometric Jacobian for the velocity of the contact point C as moving with
    // body B, s.t.: v_WBc = Jv_WBc * v.
    MatrixX<T> Jv_WBc(3, this->num_velocities());
    model().CalcPointsGeometricJacobianExpressedInWorld(
        context, bodyB.body_frame(), p_WCb, &Jv_WBc);

    // The velocity of Bc relative to Ac is
    //   v_AcBc_W = v_WBc - v_WAc.
    // From where the separation velocity is computed as
    //   vn = -v_AcBc_W.dot(nhat_BA_W) = -nhat_BA_Wᵀ⋅v_AcBc_W
    // where the negative sign stems from the sign convention for vn and xdot.
    // This can be written in terms of the Jacobians as
    //   vn = -nhat_BA_Wᵀ⋅(Jv_WBc - Jv_WAc)⋅v

    N.row(icontact) = nhat_BA_W.transpose() * (Jv_WAc - Jv_WBc);
  }

  return N;
}

template<typename T>
MatrixX<T> MultibodyPlant<T>::CalcTangentVelocitiesJacobian(
    const Context<T>& context,
    const std::vector<PenetrationAsPointPair<T>>& point_pairs_set,
    std::vector<Matrix3<T>>* R_WC_set) const {
  const int num_contacts = point_pairs_set.size();
  // D is defined such that vt = D * v, with vt of size 2nc.
  MatrixX<T> D(2 * num_contacts, num_velocities());

  DRAKE_ASSERT(R_WC_set);
  R_WC_set->clear();
  if (R_WC_set != nullptr) R_WC_set->reserve(point_pairs_set.size());
  for (int icontact = 0; icontact < num_contacts; ++icontact) {
    const auto& point_pair = point_pairs_set[icontact];

    const GeometryId geometryA_id = point_pair.id_A;
    const GeometryId geometryB_id = point_pair.id_B;

    BodyIndex bodyA_index = geometry_id_to_body_index_.at(geometryA_id);
    const Body<T>& bodyA = model().get_body(bodyA_index);
    BodyIndex bodyB_index = geometry_id_to_body_index_.at(geometryB_id);
    const Body<T>& bodyB = model().get_body(bodyB_index);

    // Penetration depth, > 0 if bodies interpenetrate.
    const T& x = point_pair.depth;
    DRAKE_ASSERT(x >= 0);
    const Vector3<T>& nhat_BA_W = point_pair.nhat_BA_W;
    const Vector3<T>& p_WCa = point_pair.p_WCa;
    const Vector3<T>& p_WCb = point_pair.p_WCb;

    // Compute the orientation of a contact frame C at the contact point such
    // that the z-axis Cz equals to nhat_BA_W. The tangent vectors are
    // arbitrary, with the only requirement being that they form a valid right
    // handed basis with nhat_BA.
    const Matrix3<T> R_WC = math::ComputeBasisFromAxis(2, nhat_BA_W);
    if (R_WC_set != nullptr) {
      R_WC_set->push_back(R_WC);
    }

    const Vector3<T> that1_W = R_WC.col(0);  // that1 = Cx.
    const Vector3<T> that2_W = R_WC.col(1);  // that2 = Cy.

    // TODO(amcastro-tri): Consider using the midpoint between Ac and Bc for
    // stability reasons. Besides that, there is no other reason to use the
    // midpoint (or any other point between Ac and Bc for that matter) since,
    // in the limit to rigid contact, Ac = Bc.

    MatrixX<T> Jv_WAc(3, this->num_velocities());  // s.t.: v_WAc = Jv_WAc * v.
    model().CalcPointsGeometricJacobianExpressedInWorld(
        context, bodyA.body_frame(), p_WCa, &Jv_WAc);

    MatrixX<T> Jv_WBc(3, this->num_velocities());  // s.t.: v_WBc = Jv_WBc * v.
    model().CalcPointsGeometricJacobianExpressedInWorld(
        context, bodyB.body_frame(), p_WCb, &Jv_WBc);

    // The velocity of Bc relative to Ac is
    //   v_AcBc_W = v_WBc - v_WAc.
    // The first two components of this velocity in C corresponds to the
    // tangential velocities in a plane normal to nhat_BA.
    //   vx_AcBc_C = that1⋅v_AcBc = that1ᵀ⋅(Jv_WBc - Jv_WAc)⋅v
    //   vy_AcBc_C = that2⋅v_AcBc = that2ᵀ⋅(Jv_WBc - Jv_WAc)⋅v

    D.row(2 * icontact)     = that1_W.transpose() * (Jv_WBc - Jv_WAc);
    D.row(2 * icontact + 1) = that2_W.transpose() * (Jv_WBc - Jv_WAc);
  }
  return D;
}

template<typename T>
void MultibodyPlant<T>::set_penetration_allowance(
    double penetration_allowance) {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  // Default to Earth's gravity for this estimation.
  const double g = gravity_field_.has_value() ?
                   gravity_field_.value()->gravity_vector().norm() : 9.81;

  // TODO(amcastro-tri): Improve this heuristics in future PR's for when there
  // are several flying objects and fixed base robots (E.g.: manipulation
  // cases.)

  // The heuristic now is very simple. We should update it to:
  //  - Only scan free bodies for weight.
  //  - Consider an estimate of maximum velocities (context dependent).
  // Right now we are being very conservative and use the maximum mass in the
  // system.
  double mass = 0.0;
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const Body<T>& body = model().get_body(body_index);
    mass = std::max(mass, body.get_default_mass());
  }

  // For now, we use the model of a critically damped spring mass oscillator
  // to estimate these parameters: mẍ+cẋ+kx=mg
  // Notice however that normal forces are computed according to: fₙ=kx(1+dẋ)
  // which translate to a second order oscillator of the form:
  // mẍ+(kdx)ẋ+kx=mg
  // Therefore, for this more complex, non-linear, oscillator, we estimate the
  // damping constant d using a time scale related to the free oscillation
  // (omega below) and the requested penetration allowance as a length scale.

  // We first estimate the stiffness based on static equilibrium.
  const double stiffness = mass * g / penetration_allowance;
  // Frequency associated with the stiffness above.
  const double omega = sqrt(stiffness / mass);

  // Estimated contact time scale. The relative velocity of objects coming into
  // contact goes to zero in this time scale.
  const double time_scale = 1.0 / omega;

  // Damping ratio for a critically damped model. We could allow users to set
  // this. Right now, critically damp the normal direction.
  // This corresponds to a non-penetraion constraint in the limit for
  // contact_penetration_allowance_ goint to zero (no bounce off).
  const double damping_ratio = 1.0;
  // We form the damping (with units of 1/velocity) using dimensional analysis.
  // Thus we use 1/omega for the time scale and penetration_allowance for the
  // length scale. We then scale it by the damping ratio.
  const double damping = damping_ratio * time_scale / penetration_allowance;

  // Final parameters used in the penalty method:
  penalty_method_contact_parameters_.stiffness = stiffness;
  penalty_method_contact_parameters_.damping = damping;
  // The time scale can be requested to hint the integrator's time step.
  penalty_method_contact_parameters_.time_scale = time_scale;
}

template <>
std::vector<PenetrationAsPointPair<double>>
MultibodyPlant<double>::CalcPointPairPenetrations(
    const systems::Context<double>& context) const {
  if (num_collision_geometries() > 0) {
    if (!geometry_query_port_.is_valid()) {
      throw std::logic_error(
          "This MultibodyPlant registered geometry for contact handling. "
          "However its query input port (get_geometry_query_input_port()) "
          "is not connected. ");
    }
    const geometry::QueryObject<double>& query_object =
        this->EvalAbstractInput(context, geometry_query_port_)
            ->template GetValue<geometry::QueryObject<double>>();
    return query_object.ComputePointPairPenetration();
  }
  return std::vector<PenetrationAsPointPair<double>>();
}

template<typename T>
std::vector<PenetrationAsPointPair<T>>
MultibodyPlant<T>::CalcPointPairPenetrations(
    const systems::Context<T>&) const {
  DRAKE_ABORT_MSG("This method only supports T = double.");
}

template<typename T>
std::vector<CoulombFriction<double>>
MultibodyPlant<T>::CalcCombinedFrictionCoefficients(
    const std::vector<PenetrationAsPointPair<T>>& point_pairs) const {
  std::vector<CoulombFriction<double>> combined_frictions;
  combined_frictions.reserve(point_pairs.size());
  for (const auto& pair : point_pairs) {
    const GeometryId geometryA_id = pair.id_A;
    const GeometryId geometryB_id = pair.id_B;

    const int collision_indexA =
        geometry_id_to_collision_index_.at(geometryA_id);
    const int collision_indexB =
        geometry_id_to_collision_index_.at(geometryB_id);
    const CoulombFriction<double>& geometryA_friction =
        default_coulomb_friction_[collision_indexA];
    const CoulombFriction<double>& geometryB_friction =
        default_coulomb_friction_[collision_indexB];

    combined_frictions.push_back(CalcContactFrictionFromSurfaceProperties(
        geometryA_friction, geometryB_friction));
  }
  return combined_frictions;
}

template<typename T>
void MultibodyPlant<T>::CalcContactResultsOutput(
    const systems::Context<T>&,
    ContactResults<T>* contact_results) const {
  DRAKE_DEMAND(contact_results != nullptr);
  // TODO(amcastro-tri): Eval() contact results when caching lands.
  *contact_results = contact_results_;
}

template<typename T>
void MultibodyPlant<T>::CalcContactResults(
    const systems::Context<T>&,
    const std::vector<PenetrationAsPointPair<T>>& point_pairs,
    const std::vector<Matrix3<T>>& R_WC_set,
    ContactResults<T>* contact_results) const {
  if (num_collision_geometries() == 0) return;
  DRAKE_DEMAND(contact_results != nullptr);
  const int num_contacts = point_pairs.size();
  DRAKE_DEMAND(static_cast<int>(R_WC_set.size()) == num_contacts);

  // Note: auto below resolves to VectorBlock<const VectorX<T>>.
  using VectorXBlock = Eigen::VectorBlock<const VectorX<T>>;
  const VectorXBlock fn = implicit_stribeck_solver_->get_normal_forces();
  const VectorXBlock ft = implicit_stribeck_solver_->get_friction_forces();
  const VectorXBlock vt =
      implicit_stribeck_solver_->get_tangential_velocities();
  const VectorXBlock vn = implicit_stribeck_solver_->get_normal_velocities();

  DRAKE_DEMAND(fn.size() == num_contacts);
  DRAKE_DEMAND(ft.size() == 2 * num_contacts);
  DRAKE_DEMAND(vn.size() == num_contacts);
  DRAKE_DEMAND(vt.size() == 2 * num_contacts);

  contact_results->Clear();
  for (size_t icontact = 0; icontact < point_pairs.size(); ++icontact) {
    const auto& pair = point_pairs[icontact];
    const GeometryId geometryA_id = pair.id_A;
    const GeometryId geometryB_id = pair.id_B;

    BodyIndex bodyA_index = geometry_id_to_body_index_.at(geometryA_id);
    BodyIndex bodyB_index = geometry_id_to_body_index_.at(geometryB_id);

    const Vector3<T> p_WC = 0.5 * (pair.p_WCa + pair.p_WCb);

    const Matrix3<T>& R_WC = R_WC_set[icontact];

    // Contact forces applied on B at contact point C.
    const Vector3<T> f_Bc_C(
        ft(2 * icontact), ft(2 * icontact + 1), fn(icontact));
    const Vector3<T> f_Bc_W = R_WC * f_Bc_C;

    // Slip velocity.
    const T slip = vt.template segment<2>(2 * icontact).norm();

    // Separation velocity in the normal direction.
    const T separation_velocity = vn(icontact);

    // Add pair info to the contact results.
    contact_results->AddContactInfo(
        {bodyA_index, bodyB_index, f_Bc_W, p_WC,
         separation_velocity, slip, pair});
  }
}

template<typename T>
void MultibodyPlant<T>::CalcAndAddContactForcesByPenaltyMethod(
    const systems::Context<T>&,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc,
    const std::vector<PenetrationAsPointPair<T>>& point_pairs,
    std::vector<SpatialForce<T>>* F_BBo_W_array) const {
  if (num_collision_geometries() == 0) return;

  std::vector<CoulombFriction<double>> combined_friction_pairs =
      CalcCombinedFrictionCoefficients(point_pairs);

  for (size_t icontact = 0; icontact < point_pairs.size(); ++icontact) {
    const auto& pair = point_pairs[icontact];
    const GeometryId geometryA_id = pair.id_A;
    const GeometryId geometryB_id = pair.id_B;

    BodyIndex bodyA_index = geometry_id_to_body_index_.at(geometryA_id);
    BodyIndex bodyB_index = geometry_id_to_body_index_.at(geometryB_id);

    BodyNodeIndex bodyA_node_index =
        model().get_body(bodyA_index).node_index();
    BodyNodeIndex bodyB_node_index =
        model().get_body(bodyB_index).node_index();

    // Penetration depth, > 0 during pair.
    const T& x = pair.depth;
    DRAKE_ASSERT(x >= 0);
    const Vector3<T>& nhat_BA_W = pair.nhat_BA_W;
    const Vector3<T>& p_WCa = pair.p_WCa;
    const Vector3<T>& p_WCb = pair.p_WCb;

    // Contact point C.
    const Vector3<T> p_WC = 0.5 * (p_WCa + p_WCb);

    // Contact point position on body A.
    const Vector3<T>& p_WAo =
        pc.get_X_WB(bodyA_node_index).translation();
    const Vector3<T>& p_CoAo_W = p_WAo - p_WC;

    // Contact point position on body B.
    const Vector3<T>& p_WBo =
        pc.get_X_WB(bodyB_node_index).translation();
    const Vector3<T>& p_CoBo_W = p_WBo - p_WC;

    // Separation velocity, > 0  if objects separate.
    const Vector3<T> v_WAc =
        vc.get_V_WB(bodyA_node_index).Shift(-p_CoAo_W).translational();
    const Vector3<T> v_WBc =
        vc.get_V_WB(bodyB_node_index).Shift(-p_CoBo_W).translational();
    const Vector3<T> v_AcBc_W = v_WBc - v_WAc;

    // if xdot = vn > 0 ==> they are getting closer.
    const T vn = v_AcBc_W.dot(nhat_BA_W);

    // Magnitude of the normal force on body A at contact point C.
    const T k = penalty_method_contact_parameters_.stiffness;
    const T d = penalty_method_contact_parameters_.damping;
    const T fn_AC = k * x * (1.0 + d * vn);

    if (fn_AC > 0) {
      // Normal force on body A, at C, expressed in W.
      const Vector3<T> fn_AC_W = fn_AC * nhat_BA_W;

      // Compute tangential velocity, that is, v_AcBc projected onto the tangent
      // plane with normal nhat_BA:
      const Vector3<T> vt_AcBc_W = v_AcBc_W - vn * nhat_BA_W;
      // Tangential speed (squared):
      const T vt_squared = vt_AcBc_W.squaredNorm();

      // Consider a value indistinguishable from zero if it is smaller
      // then 1e-14 and test against that value squared.
      const T kNonZeroSqd = 1e-14 * 1e-14;
      // Tangential friction force on A at C, expressed in W.
      Vector3<T> ft_AC_W = Vector3<T>::Zero();
      if (vt_squared > kNonZeroSqd) {
        const T vt = sqrt(vt_squared);
        // Stribeck friction coefficient.
        const T mu_stribeck = stribeck_model_.ComputeFrictionCoefficient(
            vt, combined_friction_pairs[icontact]);
        // Tangential direction.
        const Vector3<T> that_W = vt_AcBc_W / vt;

        // Magnitude of the friction force on A at C.
        const T ft_AC = mu_stribeck * fn_AC;
        ft_AC_W = ft_AC * that_W;
      }

      // Spatial force on body A at C, expressed in the world frame W.
      const SpatialForce<T> F_AC_W(Vector3<T>::Zero(),
                                   fn_AC_W + ft_AC_W);

      if (F_BBo_W_array != nullptr) {
        if (bodyA_index != world_index()) {
          // Spatial force on body A at Ao, expressed in W.
          const SpatialForce<T> F_AAo_W = F_AC_W.Shift(p_CoAo_W);
          F_BBo_W_array->at(bodyA_node_index) += F_AAo_W;
        }

        if (bodyB_index != world_index()) {
          // Spatial force on body B at Bo, expressed in W.
          const SpatialForce<T> F_BBo_W = -F_AC_W.Shift(p_CoBo_W);
          F_BBo_W_array->at(bodyB_node_index) += F_BBo_W;
        }
      }
    }
  }
}

template<typename T>
void MultibodyPlant<T>::AddJointDampingForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  DRAKE_DEMAND(forces != nullptr);
  for (JointIndex joint_index(0); joint_index < num_joints(); ++joint_index) {
    const Joint<T>& joint = model().get_joint(joint_index);
    joint.AddInDamping(context, forces);
  }
}

template<typename T>
void MultibodyPlant<T>::AddJointActuationForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  DRAKE_DEMAND(forces != nullptr);
  if (num_actuators() > 0) {
    const VectorX<T> u = AssembleActuationInput(context);
    for (JointActuatorIndex actuator_index(0);
         actuator_index < num_actuators(); ++actuator_index) {
      const JointActuator<T>& actuator =
          model().get_joint_actuator(actuator_index);
      // We only support actuators on single dof joints for now.
      DRAKE_DEMAND(actuator.joint().num_dofs() == 1);
      for (int joint_dof = 0;
           joint_dof < actuator.joint().num_dofs(); ++joint_dof) {
        actuator.AddInOneForce(context, joint_dof, u[actuator_index], forces);
      }
    }
  }
}

template<typename T>
VectorX<T> MultibodyPlant<T>::AssembleActuationInput(
    const systems::Context<T>& context) const {
  // Assemble the vector from the model instance input ports.
  VectorX<T> actuation_input(num_actuated_dofs());
  int u_offset = 0;
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const int instance_num_dofs =
        model_->num_actuated_dofs(model_instance_index);
    if (instance_num_dofs == 0) {
      continue;
    }
    Eigen::VectorBlock<const VectorX<T>> u_instance =
        this->EvalEigenVectorInput(
            context, instance_actuation_ports_[model_instance_index]);
    actuation_input.segment(u_offset, instance_num_dofs) = u_instance;
    u_offset += instance_num_dofs;
  }
  DRAKE_ASSERT(u_offset == num_actuated_dofs());
  return actuation_input;
}

template<typename T>
void MultibodyPlant<T>::DoCalcTimeDerivatives(
    const systems::Context<T>& context,
    systems::ContinuousState<T>* derivatives) const {
  // No derivatives to compute if state is discrete.
  if (is_discrete()) return;

  const auto x =
      dynamic_cast<const systems::BasicVector<T>&>(
          context.get_continuous_state_vector()).get_value();
  const int nv = this->num_velocities();

  // Allocate workspace. We might want to cache these to avoid allocations.
  // Mass matrix.
  MatrixX<T> M(nv, nv);
  // Forces.
  MultibodyForces<T> forces(*model_);
  // Bodies' accelerations, ordered by BodyNodeIndex.
  std::vector<SpatialAcceleration<T>> A_WB_array(model_->num_bodies());
  // Generalized accelerations.
  VectorX<T> vdot = VectorX<T>::Zero(nv);

  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);

  // Compute forces applied through force elements. This effectively resets
  // the forces to zero and adds in contributions due to force elements.
  model_->CalcForceElementsContribution(context, pc, vc, &forces);

  // If there is any input actuation, add it to the multibody forces.
  AddJointActuationForces(context, &forces);

  AddJointDampingForces(context, &forces);

  model_->CalcMassMatrixViaInverseDynamics(context, &M);

  // WARNING: to reduce memory foot-print, we use the input applied arrays also
  // as output arrays. This means that both the array of applied body forces and
  // the array of applied generalized forces get overwritten on output. This is
  // not important in this case since we don't need their values anymore.
  // Please see the documentation for CalcInverseDynamics() for details.

  // With vdot = 0, this computes:
  //   tau = C(q, v)v - tau_app - ∑ J_WBᵀ(q) Fapp_Bo_W.
  std::vector<SpatialForce<T>>& F_BBo_W_array = forces.mutable_body_forces();
  VectorX<T>& tau_array = forces.mutable_generalized_forces();

  // Compute contact forces on each body by penalty method.
  if (num_collision_geometries() > 0) {
    std::vector<PenetrationAsPointPair<T>> point_pairs =
        CalcPointPairPenetrations(context);
    CalcAndAddContactForcesByPenaltyMethod(
        context, pc, vc, point_pairs, &F_BBo_W_array);
  }

  model_->CalcInverseDynamics(
      context, pc, vc, vdot,
      F_BBo_W_array, tau_array,
      &A_WB_array,
      &F_BBo_W_array, /* Notice these arrays gets overwritten on output. */
      &tau_array);

  vdot = M.ldlt().solve(-tau_array);

  auto v = x.bottomRows(nv);
  VectorX<T> xdot(this->num_multibody_states());
  VectorX<T> qdot(this->num_positions());
  model_->MapVelocityToQDot(context, v, &qdot);
  xdot << qdot, vdot;
  derivatives->SetFromVector(xdot);
}

// TODO(amcastro-tri): Consider splitting this method into smaller pieces.
template<typename T>
void MultibodyPlant<T>::DoCalcDiscreteVariableUpdates(
    const drake::systems::Context<T>& context0,
    const std::vector<const drake::systems::DiscreteUpdateEvent<T>*>&,
    drake::systems::DiscreteValues<T>* updates) const {
  // Assert this method was called on a context storing discrete state.
  DRAKE_ASSERT(context0.get_num_discrete_state_groups() == 1);
  DRAKE_ASSERT(context0.get_continuous_state().size() == 0);

  const double dt = time_step_;  // just a shorter alias.

  const int nq = this->num_positions();
  const int nv = this->num_velocities();

  // Get the system state as raw Eigen vectors
  // (solution at the previous time step).
  auto x0 = context0.get_discrete_state(0).get_value();
  VectorX<T> q0 = x0.topRows(nq);
  VectorX<T> v0 = x0.bottomRows(nv);

  // Mass matrix and its factorization.
  MatrixX<T> M0(nv, nv);
  model_->CalcMassMatrixViaInverseDynamics(context0, &M0);
  auto M0_ldlt = M0.ldlt();

  // Forces at the previous time step.
  MultibodyForces<T> forces0(*model_);

  const PositionKinematicsCache<T>& pc0 = EvalPositionKinematics(context0);
  const VelocityKinematicsCache<T>& vc0 = EvalVelocityKinematics(context0);

  // Compute forces applied through force elements.
  model_->CalcForceElementsContribution(context0, pc0, vc0, &forces0);

  // If there is any input actuation, add it to the multibody forces.
  AddJointActuationForces(context0, &forces0);

  // TODO(amcastro-tri): Update ImplicitStribeckSolver to treat this term
  // implicitly.
  AddJointDampingForces(context0, &forces0);

  // TODO(amcastro-tri): Eval() point_pairs0 when caching lands.
  const std::vector<PenetrationAsPointPair<T>> point_pairs0 =
      CalcPointPairPenetrations(context0);

  // Workspace for inverse dynamics:
  // Bodies' accelerations, ordered by BodyNodeIndex.
  std::vector<SpatialAcceleration<T>> A_WB_array(model_->num_bodies());
  // Generalized accelerations.
  VectorX<T> vdot = VectorX<T>::Zero(nv);
  // Body forces (alias to forces0).
  std::vector<SpatialForce<T>>& F_BBo_W_array = forces0.mutable_body_forces();

  // With vdot = 0, this computes:
  //   -tau = C(q, v)v - tau_app - ∑ J_WBᵀ(q) Fapp_Bo_W.
  VectorX<T>& minus_tau = forces0.mutable_generalized_forces();
  model_->CalcInverseDynamics(
      context0, pc0, vc0, vdot,
      F_BBo_W_array, minus_tau,
      &A_WB_array,
      &F_BBo_W_array, /* Note: these arrays get overwritten on output. */
      &minus_tau);

  // Compute discrete update before applying friction forces.
  // We denote this state x* = [q*, v*], the "star" state.
  // Generalized momentum "star", before contact forces are applied.
  VectorX<T> p_star = M0 * v0 - dt * minus_tau;

  // Compute normal and tangential velocity Jacobians at t0.
  const int num_contacts = point_pairs0.size();
  MatrixX<T> Jn(num_contacts, nv);
  MatrixX<T> Jt(2 * num_contacts, nv);
  // For each contact point pair, the rotation matrix R_WC giving the
  // orientation of the contact frame C in the world frame W.
  // TODO(amcastro-tri): cache R_WC_set as soon as caching lands.
  std::vector<Matrix3<T>> R_WC_set;
  if (num_contacts > 0) {
    // TODO(amcastro-tri): when it becomes a bottleneck, update the contact
    // solver to use operators instead so that we don't have to form these
    // Jacobian matrices explicitly (an O(num_contacts * nv) operation).
    Jn = CalcNormalSeparationVelocitiesJacobian(context0, point_pairs0);
    Jt = CalcTangentVelocitiesJacobian(context0, point_pairs0, &R_WC_set);
  }

  // Get friction coefficient into a single vector. Dynamic friction is ignored
  // by the time stepping scheme.
  std::vector<CoulombFriction<double>> combined_friction_pairs =
      CalcCombinedFrictionCoefficients(point_pairs0);
  VectorX<T> mu(num_contacts);
  std::transform(combined_friction_pairs.begin(), combined_friction_pairs.end(),
                 mu.data(),
                 [](const CoulombFriction<double>& coulomb_friction) {
                   return coulomb_friction.static_friction();
                 });

  // Place all the penetration depths within a single vector as required by
  // the solver.
  VectorX<T> phi0(num_contacts);
  std::transform(point_pairs0.begin(), point_pairs0.end(),
                 phi0.data(),
                 [](const PenetrationAsPointPair<T>& pair) {
                   return pair.depth;
                 });

  // TODO(amcastro-tri): Consider using different penalty parameters at each
  // contact point.
  // Compliance parameters used by the solver for each contact point.
  VectorX<T> stiffness = VectorX<T>::Constant(
      num_contacts, penalty_method_contact_parameters_.stiffness);
  VectorX<T> damping = VectorX<T>::Constant(
      num_contacts, penalty_method_contact_parameters_.damping);

  // Update the solver with the new data defining the problem for this update.
  implicit_stribeck_solver_->SetTwoWayCoupledProblemData(
      &M0, &Jn, &Jt, &p_star, &phi0, &stiffness, &damping, &mu);

  // Solve for v and the contact forces.
  implicit_stribeck::ComputationInfo info =
      implicit_stribeck_solver_->SolveWithGuess(dt, v0);
  DRAKE_DEMAND(info == implicit_stribeck::Success);

  // TODO(amcastro-tri): implement capability to dump solver statistics to a
  // file for analysis.

  // Retrieve the solution velocity for the next time step.
  VectorX<T> v_next = implicit_stribeck_solver_->get_generalized_velocities();

  VectorX<T> qdot_next(this->num_positions());
  model_->MapVelocityToQDot(context0, v_next, &qdot_next);
  VectorX<T> q_next = q0 + dt * qdot_next;

  VectorX<T> x_next(this->num_multibody_states());
  x_next << q_next, v_next;
  updates->get_mutable_vector(0).SetFromVector(x_next);

  // Save contact results for analysis and visualization.
  // TODO(amcastro-tri): remove next line once caching lands since point_pairs0
  // and R_WC_set will be cached.
  CalcContactResults(context0, point_pairs0, R_WC_set, &contact_results_);
}

template<typename T>
void MultibodyPlant<T>::DoMapQDotToVelocity(
    const systems::Context<T>& context,
    const Eigen::Ref<const VectorX<T>>& qdot,
    systems::VectorBase<T>* generalized_velocity) const {
  const int nq = model_->num_positions();
  const int nv = model_->num_velocities();

  DRAKE_ASSERT(qdot.size() == nq);
  DRAKE_DEMAND(generalized_velocity != nullptr);
  DRAKE_DEMAND(generalized_velocity->size() == nv);

  VectorX<T> v(nv);
  model_->MapQDotToVelocity(context, qdot, &v);
  generalized_velocity->SetFromVector(v);
}

template<typename T>
void MultibodyPlant<T>::DoMapVelocityToQDot(
    const systems::Context<T>& context,
    const Eigen::Ref<const VectorX<T>>& generalized_velocity,
    systems::VectorBase<T>* positions_derivative) const {
  const int nq = model_->num_positions();
  const int nv = model_->num_velocities();

  DRAKE_ASSERT(generalized_velocity.size() == nv);
  DRAKE_DEMAND(positions_derivative != nullptr);
  DRAKE_DEMAND(positions_derivative->size() == nq);

  VectorX<T> qdot(nq);
  model_->MapVelocityToQDot(context, generalized_velocity, &qdot);
  positions_derivative->SetFromVector(qdot);
}

template<typename T>
void MultibodyPlant<T>::DeclareStateAndPorts() {
  // The model must be finalized.
  DRAKE_DEMAND(this->is_finalized());

  if (is_discrete()) {
    this->DeclarePeriodicDiscreteUpdate(time_step_);
    this->DeclareDiscreteState(num_multibody_states());
  } else {
    this->DeclareContinuousState(
        BasicVector<T>(model_->num_states()),
        model_->num_positions(),
        model_->num_velocities(), 0 /* num_z */);
  }

  // Declare per model instance actuation ports.
  int num_actuated_instances = 0;
  ModelInstanceIndex last_actuated_instance;
  instance_actuation_ports_.resize(num_model_instances());
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const int instance_num_dofs =
        model_->num_actuated_dofs(model_instance_index);
    if (instance_num_dofs == 0) {
      continue;
    }
    ++num_actuated_instances;
    last_actuated_instance = model_instance_index;
    instance_actuation_ports_[model_instance_index] =
        this->DeclareVectorInputPort(
            systems::BasicVector<T>(instance_num_dofs)).get_index();
  }

  if (num_actuated_instances == 1) {
    actuated_instance_ = last_actuated_instance;
  }

  // Declare one output port for the entire state vector.
  continuous_state_output_port_ =
      this->DeclareVectorOutputPort(
          BasicVector<T>(num_multibody_states()),
          &MultibodyPlant::CopyContinuousStateOut).get_index();

  // Declare per model instance state output ports.
  instance_continuous_state_output_ports_.resize(num_model_instances());
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const int instance_num_states =
        model_->num_states(model_instance_index);
    if (instance_num_states == 0) {
      continue;
    }

    auto calc = [this, model_instance_index](const systems::Context<T>& context,
                                             systems::BasicVector<T>* result) {
      this->CopyContinuousStateOut(model_instance_index, context, result);
    };
    instance_continuous_state_output_ports_[model_instance_index] =
        this->DeclareVectorOutputPort(
            BasicVector<T>(instance_num_states), calc).get_index();
  }

  // Declare per model instance output port of generalized contact forces.
  instance_generalized_contact_forces_output_ports_.resize(
      num_model_instances());
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    const int instance_num_velocities =
        model_->num_velocities(model_instance_index);
    if (instance_num_velocities == 0) {
      continue;
    }
    auto calc = [this, model_instance_index](const systems::Context<T>& context,
                                             systems::BasicVector<T>* result) {
      this->CopyGeneralizedContactForcesOut(
          model_instance_index, context, result);
    };
    instance_generalized_contact_forces_output_ports_[model_instance_index] =
        this->DeclareVectorOutputPort(
            BasicVector<T>(instance_num_velocities), calc).get_index();
  }

  // Contact results output port.
  contact_results_port_ = this->DeclareAbstractOutputPort(
      ContactResults<T>(),
      &MultibodyPlant<T>::CalcContactResultsOutput).get_index();
}

template <typename T>
const systems::BasicVector<T>& MultibodyPlant<T>::GetStateVector(
    const Context<T>& context) const {
  if (is_discrete()) {
    return context.get_discrete_state(0);
  } else {
    return dynamic_cast<const systems::BasicVector<T>&>(
        context.get_continuous_state_vector());
  }
}

template <typename T>
void MultibodyPlant<T>::CopyContinuousStateOut(
    const Context<T>& context, BasicVector<T>* state_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  state_vector->SetFrom(GetStateVector(context));
}

template <typename T>
void MultibodyPlant<T>::CopyContinuousStateOut(
    ModelInstanceIndex model_instance,
    const Context<T>& context, BasicVector<T>* state_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();

  VectorX<T> continuous_state_vector =
      GetStateVector(context).CopyToVector();

  VectorX<T> instance_state_vector(model_->num_states(model_instance));
  instance_state_vector.head(num_positions(model_instance)) =
      model_->get_positions_from_array(
          model_instance, continuous_state_vector.head(num_positions()));
  instance_state_vector.tail(num_velocities(model_instance)) =
      model_->get_velocities_from_array(
          model_instance, continuous_state_vector.tail(num_velocities()));

  state_vector->set_value(instance_state_vector);
}

template <typename T>
void MultibodyPlant<T>::CopyGeneralizedContactForcesOut(
    ModelInstanceIndex model_instance, const Context<T>&,
    BasicVector<T>* tau_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(is_discrete());

  // Vector of generalized contact forces for the entire plant's multibody
  // system.
  // TODO(amcastro-tri): Contact forces should be computed into a cache entry
  // and evaluated here. Update this to use caching as soon as the capability
  // lands.
  const VectorX<T>& tau_contact =
      implicit_stribeck_solver_->get_generalized_contact_forces();

  // Generalized velocities and generalized forces are ordered in the same way.
  // Thus we can call get_velocities_from_array().
  const VectorX<T> instance_tau_contact =
      model_->get_velocities_from_array(model_instance, tau_contact);

  tau_vector->set_value(instance_tau_contact);
}

template <typename T>
const systems::InputPortDescriptor<T>&
MultibodyPlant<T>::get_actuation_input_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(num_actuators() > 0);
  DRAKE_THROW_UNLESS(actuated_instance_.is_valid());
  return get_actuation_input_port(actuated_instance_);
}

template <typename T>
const systems::InputPortDescriptor<T>&
MultibodyPlant<T>::get_actuation_input_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  DRAKE_THROW_UNLESS(num_actuated_dofs(model_instance) > 0);
  return systems::System<T>::get_input_port(
      instance_actuation_ports_.at(model_instance));
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_continuous_state_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_output_port(continuous_state_output_port_);
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_continuous_state_output_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  DRAKE_THROW_UNLESS(model_->num_states(model_instance) > 0);
  return this->get_output_port(
      instance_continuous_state_output_ports_.at(model_instance));
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_generalized_contact_forces_output_port(
    ModelInstanceIndex model_instance) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(is_discrete());
  DRAKE_THROW_UNLESS(model_instance.is_valid());
  DRAKE_THROW_UNLESS(model_instance < num_model_instances());
  DRAKE_THROW_UNLESS(model_->num_states(model_instance) > 0);
  return this->get_output_port(
      instance_generalized_contact_forces_output_ports_.at(model_instance));
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_contact_results_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_THROW_UNLESS(is_discrete());
  return this->get_output_port(contact_results_port_);
}

template <typename T>
void MultibodyPlant<T>::DeclareSceneGraphPorts() {
  geometry_query_port_ = this->DeclareAbstractInputPort().get_index();
  // This presupposes that the source id has been assigned and _all_ frames have
  // been registered.
  std::vector<FrameId> ids;
  for (auto it : body_index_to_frame_id_) {
    ids.push_back(it.second);
  }
  geometry_pose_port_ =
      this->DeclareAbstractOutputPort(
          FramePoseVector<T>(*source_id_, ids),
          &MultibodyPlant::CalcFramePoseOutput).get_index();
}

template <typename T>
void MultibodyPlant<T>::CalcFramePoseOutput(
    const Context<T>& context, FramePoseVector<T>* poses) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_ASSERT(source_id_ != nullopt);
  DRAKE_ASSERT(
      poses->size() == static_cast<int>(body_index_to_frame_id_.size()));
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);

  // TODO(amcastro-tri): Make use of Body::EvalPoseInWorld(context) once caching
  // lands.
  poses->clear();
  for (const auto it : body_index_to_frame_id_) {
    const BodyIndex body_index = it.first;
    const Body<T>& body = model_->get_body(body_index);

    // NOTE: The GeometryFrames for each body were registered in the world
    // frame, so we report poses in the world frame.
    poses->set_value(body_index_to_frame_id_.at(body_index),
                     pc.get_X_WB(body.node_index()));
  }
}

template <typename T>
const OutputPort<T>& MultibodyPlant<T>::get_geometry_poses_output_port()
const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(geometry_source_is_registered());
  return systems::System<T>::get_output_port(geometry_pose_port_);
}

template <typename T>
const systems::InputPortDescriptor<T>&
MultibodyPlant<T>::get_geometry_query_input_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(geometry_source_is_registered());
  return systems::System<T>::get_input_port(geometry_query_port_);
}

template<typename T>
void MultibodyPlant<T>::DeclareCacheEntries() {
  // TODO(amcastro-tri): User proper System::Declare() infrastructure to
  // declare cache entries when that lands.
  pc_ = std::make_unique<PositionKinematicsCache<T>>(model_->get_topology());
  vc_ = std::make_unique<VelocityKinematicsCache<T>>(model_->get_topology());
}

template<typename T>
const PositionKinematicsCache<T>& MultibodyPlant<T>::EvalPositionKinematics(
    const systems::Context<T>& context) const {
  // TODO(amcastro-tri): Replace Calc() for an actual Eval() when caching lands.
  model_->CalcPositionKinematicsCache(context, pc_.get());
  return *pc_;
}

template<typename T>
const VelocityKinematicsCache<T>& MultibodyPlant<T>::EvalVelocityKinematics(
    const systems::Context<T>& context) const {
  // TODO(amcastro-tri): Replace Calc() for an actual Eval() when caching lands.
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  model_->CalcVelocityKinematicsCache(context, pc, vc_.get());
  return *vc_;
}

template <typename T>
void MultibodyPlant<T>::ThrowIfFinalized(const char* source_method) const {
  if (is_finalized()) {
    throw std::logic_error(
        "Post-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; calls to this method must happen before Finalize().");
  }
}

template <typename T>
void MultibodyPlant<T>::ThrowIfNotFinalized(const char* source_method) const {
  if (!is_finalized()) {
    throw std::logic_error(
        "Pre-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; you must call Finalize() first.");
  }
}

template <typename T>
T MultibodyPlant<T>::StribeckModel::ComputeFrictionCoefficient(
    const T& speed_BcAc,
    const CoulombFriction<double>& friction) const {
  DRAKE_ASSERT(speed_BcAc >= 0);
  const double mu_d = friction.dynamic_friction();
  const double mu_s = friction.static_friction();
  const T v = speed_BcAc * inv_v_stiction_tolerance_;
  if (v >= 3) {
    return mu_d;
  } else if (v >= 1) {
    return mu_s - (mu_s - mu_d) * step5((v - 1) / 2);
  } else {
    return mu_s * step5(v);
  }
}

template <typename T>
T MultibodyPlant<T>::StribeckModel::step5(const T& x) {
  DRAKE_ASSERT(0 <= x && x <= 1);
  const T x3 = x * x * x;
  return x3 * (10 + x * (6 * x - 15));  // 10x³ - 15x⁴ + 6x⁵
}

}  // namespace multibody_plant
}  // namespace multibody
}  // namespace drake

DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_NONSYMBOLIC_SCALARS(
    class drake::multibody::multibody_plant::MultibodyPlant)
