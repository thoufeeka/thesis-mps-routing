# Running Real MPS Simulations & Benchmarks on the University Cluster (Slurm)

Everything required to build Maestro, apply the thesis routing/cost-model patches, and run the real MPS simulations is self-contained inside this repository (`cluster/`). **No internet access is required on the cluster compute nodes.**

---

## 1. Push from Your Local Machine

```bash
git add submit_slurm.sh cluster/ benchmarks/ scripts/
git commit -m "Prepare offline Slurm cluster build and real simulation benchmark suite"
git push origin main
```

---

## 2. Clone & Submit on the University Cluster

SSH into your university cluster login node and run:

```bash
git clone https://github.com/thoufeeka/thesis-mps-routing.git
cd thesis-mps-routing

# Submit the main Slurm job:
sbatch submit_slurm.sh
```

> **Note on Slurm Partition / Account**:
> If your university cluster requires a specific `--partition` or `--account`, either edit the `#SBATCH` lines at the top of `submit_slurm.sh` or pass them directly on the command line:
> ```bash
> sbatch --partition=<your_partition> --account=<your_account> submit_slurm.sh
> ```
> If you want to submit to a GPU partition, you can also use:
> ```bash
> sbatch cluster/slurm_gpu.sh
> ```

### Monitoring the Job

```bash
# Check job queue status:
squeue -u $USER

# Watch live progress (updated after every circuit):
tail -f results/slurm_<JOBID>.out
```

### Customizing Sweep Parameters (Optional)

By default, `submit_slurm.sh` runs:
- **Bond dimensions ($\chi$)**: `32 64 128 256`
- **Cost models**: `cubic svd`
- **Repetitions**: `5` timed runs (+ 1 warmup/SVD-collector pass per circuit)

You can override any parameter via `--export` without modifying any files:

```bash
sbatch --export=ALL,REPS=5,BOND_DIMS="64 128 256",COST_MODELS="cubic svd" submit_slurm.sh
```

---

## 3. What the Slurm Job Produces

When the job finishes, all outputs are saved in `results/`:

1. **`results/Cluster_Real_Simulation_Report.md`**
   Complete Markdown report with summary tables across bond dimensions, cost models, thread counts, circuit families, and per-circuit wall-clock speedups, SVD speedups, and real inserted SWAP counts.
2. **`results/real_sim_comparison_results.csv`**
   Standard side-by-side comparison (`BuildChain` vs `RigidSeedLocalSearch`) at $\chi = 64$.
3. **`results/real_sim_detailed_results.csv`** & **`results/real_sim_all_sweeps.csv`**
   Detailed metrics across all swept configurations (`bc_map_ms`, `rs_map_ms`, `bc_dummy_cost`, `rs_dummy_cost`, `bc_real_swaps`, `rs_real_swaps`, `bc_real_peak_bond`, `rs_real_peak_bond`, `bc_svd_ms`, `rs_svd_ms`, `bc_sim_ms_avg`, `rs_sim_ms_avg`, `sim_speedup_pct`, `sim_speedup_x`).
4. **`results/mapping_comparison_results.csv`** & **`results/cluster_run_<JOBID>/mapping_comparison_chi*.csv`**
   All 14 initial mapping strategies evaluated across bond dimensions and cost models.
5. **`results/tables/table1..4.csv`** & **`results/analysis_summary.json`**
   Operation-level SVD timing and cost-model validation tables generated on the cluster hardware.
6. **`results/cluster_results_<JOBID>.tar.gz`**
   Single compressed archive containing all CSVs, reports, and hardware info for one-command download (`scp`) or `git push`.

---

## 4. Retrieving Results Back to Your Machine

Either copy the single results tarball via `scp` from your local machine:

```bash
scp <cluster_user>@<cluster_host>:~/thesis-mps-routing/results/cluster_results_*.tar.gz ./results/
```

Or commit and push the results from the cluster login node:

```bash
git add results/
git commit -m "Add cluster real simulation results"
git push
```

