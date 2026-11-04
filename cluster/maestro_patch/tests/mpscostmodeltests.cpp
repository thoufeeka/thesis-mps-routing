#define BOOST_TEST_MODULE CostModelTestModule
#include <boost/test/included/unit_test.hpp>
#include "Simulators/MPSDummySimulator.h"

BOOST_AUTO_TEST_SUITE(MPSCostModelTests)

BOOST_AUTO_TEST_CASE(TestComputeBondCostProfiles) {
  const size_t N = 20;
  Simulators::MPSDummySimulator sim(N);
  sim.SetMaxBondDimension(1024);

  // Middle bond index = 10
  std::vector<double> dims(N - 1, 1.0);

  // Test uniform (1, 1, 1)
  sim.SetCurrentBondDimensions(dims);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::CentralBondCubic);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 1.0); // 1^3

  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 1.0); // 1*1*min(1,1)

  // Test profile (chiL, chiC, chiR) = (4, 32, 4) at bond 10
  dims[9] = 4.0;
  dims[10] = 32.0;
  dims[11] = 4.0;
  sim.SetCurrentBondDimensions(dims);

  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::CentralBondCubic);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 32.0 * 32.0 * 32.0); // 32768

  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 4.0 * 4.0 * 4.0); // 4*4*min(4,4) = 64

  // Test (32, 32, 32)
  dims[9] = 32.0;
  dims[10] = 32.0;
  dims[11] = 32.0;
  sim.SetCurrentBondDimensions(dims);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::CentralBondCubic);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 32768.0);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 32768.0); // uniform property: C_old = C_svd = chi^3

  // Test (64, 32, 64)
  dims[9] = 64.0;
  dims[10] = 32.0;
  dims[11] = 64.0;
  sim.SetCurrentBondDimensions(dims);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::CentralBondCubic);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 32.0 * 32.0 * 32.0); // 32768
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 64.0 * 64.0 * 64.0); // 262144

  // Test (4, 32, 64)
  dims[9] = 4.0;
  dims[10] = 32.0;
  dims[11] = 64.0;
  sim.SetCurrentBondDimensions(dims);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 4.0 * 64.0 * 4.0); // 1024

  // Test (64, 32, 4)
  dims[9] = 64.0;
  dims[10] = 32.0;
  dims[11] = 4.0;
  sim.SetCurrentBondDimensions(dims);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 64.0 * 4.0 * 4.0); // 1024
}

BOOST_AUTO_TEST_CASE(TestBoundariesAndRecomputationAndCopies) {
  const size_t N = 20;
  Simulators::MPSDummySimulator sim(N);
  sim.SetMaxBondDimension(1024);

  std::vector<double> dims(N - 1, 1.0);
  // bond 0 is left boundary (max extent = 2). dims[1] (right neighbor) = 2.0
  dims[0] = 2.0;
  dims[1] = 4.0;
  // bond N-2 is right boundary (max extent = 2). dims[N-3] (left neighbor) = 4.0
  dims[N - 2] = 2.0;
  dims[N - 3] = 4.0;

  // Middle bond 10 with left neighbor 9 (16.0) and right neighbor 11 (8.0)
  dims[9] = 16.0;
  dims[10] = 32.0;
  dims[11] = 8.0;

  sim.SetCurrentBondDimensions(dims);
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);

  // Left boundary (bond 0): chiL = 1.0 (boundary), chiR = dims[1] = 4.0
  // C_svd(0) = 1.0 * 4.0 * min(1.0, 4.0) = 4.0
  BOOST_CHECK_EQUAL(sim.computeBondCost(0), 4.0);

  // Right boundary (bond N-2): chiL = dims[N-3] = 4.0, chiR = 1.0 (boundary)
  // C_svd(N-2) = 4.0 * 1.0 * min(4.0, 1.0) = 4.0
  BOOST_CHECK_EQUAL(sim.computeBondCost(N - 2), 4.0);

  // Middle bond 10: chiL = 16.0, chiR = 8.0 -> 16 * 8 * min(16, 8) = 1024.0
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 1024.0);

  // Recomputation after right neighbor bond (bond 11) changes from 8.0 to 64.0
  dims[11] = 64.0;
  sim.SetCurrentBondDimensions(dims);
  // C_svd(10) = 16.0 * 64.0 * min(16.0, 64.0) = 16384.0
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 16384.0);

  // Copy/Clone behavior test
  auto clone = sim.Clone();
  BOOST_CHECK(clone->getCostModel() == Simulators::MPSDummySimulator::CostModel::ThinSVDGeometry);
  BOOST_CHECK_EQUAL(clone->computeBondCost(10), 16384.0);

  // Switching cost model back to CentralBondCubic
  sim.setCostModel(Simulators::MPSDummySimulator::CostModel::CentralBondCubic);
  BOOST_CHECK_EQUAL(sim.computeBondCost(10), 32.0 * 32.0 * 32.0); // 32768

  std::cout << "ALL MPSCostModelTests PASSED SUCCESSFULLY!" << std::endl;
}

BOOST_AUTO_TEST_SUITE_END()
