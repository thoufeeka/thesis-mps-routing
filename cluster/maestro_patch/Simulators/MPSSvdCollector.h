#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

namespace Simulators {

class MPSSvdCollector;
MPSSvdCollector& getMPSSvdCollectorInstance();

struct OperationRecord {
  uint64_t operation_index = 0;
  std::string gate_type = "GATE";
  bool is_swap = false;
  int64_t bond = 0;

  double chi_left = 1.0;
  double chi_center_before = 1.0;
  double chi_right = 1.0;
  double chi_center_after = 1.0;

  int64_t svd_rows = 2;
  int64_t svd_cols = 2;
  std::string svd_solver = "jacobi";

  double cost_cubic_real = 1.0;
  double cost_svd_real = 1.0;

  uint64_t svd_time_ns = 0;

  // Stage 8: Dummy dimensions & costs
  double dummy_chi_left = 0.0;
  double dummy_chi_center = 0.0;
  double dummy_chi_right = 0.0;
  double cost_cubic_dummy = 0.0;
  double cost_svd_dummy = 0.0;
};

class MPSSvdCollector {
 public:
  bool enabled = false;
  std::string current_circuit = "";
  std::string current_cost_model = "cubic";
  int current_repetition = 1;
  uint64_t operation_counter = 0;
  uint64_t total_svd_time_ns = 0;
  uint64_t svd_call_count = 0;
  size_t real_peak_bond_dim = 1;
  uint64_t inserted_swap_count = 0;

  std::vector<OperationRecord> records;

  static MPSSvdCollector& getInstance() {
    return getMPSSvdCollectorInstance();
  }

  void startSession(const std::string& circuit, const std::string& cost_model, int repetition) {
    enabled = true;
    current_circuit = circuit;
    current_cost_model = cost_model;
    current_repetition = repetition;
    operation_counter = 0;
    total_svd_time_ns = 0;
    svd_call_count = 0;
    real_peak_bond_dim = 1;
    inserted_swap_count = 0;
    records.clear();
  }

  void stopSession() {
    enabled = false;
  }

  void recordOperation(const OperationRecord& rec) {
    if (!enabled) return;
    records.push_back(rec);
    total_svd_time_ns += rec.svd_time_ns;
    svd_call_count++;
    if (rec.is_swap) {
      inserted_swap_count++;
    }
  }

  void updatePeakBondDim(size_t bondDim) {
    if (bondDim > real_peak_bond_dim) {
      real_peak_bond_dim = bondDim;
    }
  }

  void writeOperationTraceCSV(const std::string& filepath = "mps_operation_trace.csv") {
    if (records.empty()) return;

    bool writeHeader = false;
    {
      std::ifstream check(filepath);
      if (!check.good()) writeHeader = true;
    }

    std::ofstream csv(filepath, std::ios::app);
    if (!csv.is_open()) return;

    if (writeHeader) {
      csv << "circuit,repetition,cost_model,operation_index,gate_type,is_swap,bond,"
          << "chi_left,chi_center_before,chi_right,chi_center_after,"
          << "svd_rows,svd_cols,svd_solver,cost_cubic_real,cost_svd_real,svd_time_ns,"
          << "dummy_chi_left,dummy_chi_center,dummy_chi_right,cost_cubic_dummy,cost_svd_dummy\n";
    }

    for (const auto& r : records) {
      csv << current_circuit << ","
          << current_repetition << ","
          << current_cost_model << ","
          << r.operation_index << ","
          << r.gate_type << ","
          << (r.is_swap ? 1 : 0) << ","
          << r.bond << ","
          << r.chi_left << ","
          << r.chi_center_before << ","
          << r.chi_right << ","
          << r.chi_center_after << ","
          << r.svd_rows << ","
          << r.svd_cols << ","
          << r.svd_solver << ","
          << r.cost_cubic_real << ","
          << r.cost_svd_real << ","
          << r.svd_time_ns << ","
          << r.dummy_chi_left << ","
          << r.dummy_chi_center << ","
          << r.dummy_chi_right << ","
          << r.cost_cubic_dummy << ","
          << r.cost_svd_dummy << "\n";
    }
  }
};

} // namespace Simulators
