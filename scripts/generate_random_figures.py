import csv, math, os
from collections import defaultdict
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy.stats import spearmanr

os.chdir("/home/steffi/thesis-mps-routing")
os.makedirs("results/figures", exist_ok=True)

def _f(s):
    try: return float(s)
    except: return None

def median(vals):
    v = sorted(x for x in vals if x is not None and not math.isnan(x))
    if not v: return float("nan")
    n = len(v)
    return v[n//2] if n%2==1 else (v[n//2-1]+v[n//2])/2

print("Loading random circuit rows from trace...", flush=True)
valid = []
with open("mps_operation_trace.csv", newline="") as f:
    for row in csv.DictReader(f):
        if "random" not in row["circuit"]: continue
        cc=_f(row["cost_cubic_real"]); cs=_f(row["cost_svd_real"]); t=_f(row["svd_time_ns"])
        cl=_f(row["chi_left"]); cr=_f(row["chi_right"])
        if cc and cs and t and t>0:
            valid.append({"cc":cc,"cs":cs,"t":t,"cl":cl,"cr":cr,
                          "circuit":row["circuit"],"solver":row["svd_solver"],
                          "rows":int(row["svd_rows"]),"cols":int(row["svd_cols"])})
print(f"  Loaded {len(valid)} records", flush=True)

plt.rcParams.update({"font.size":11,"axes.spines.top":False,"axes.spines.right":False,"figure.dpi":150})

CC = np.array([r["cc"] for r in valid])
CS = np.array([r["cs"] for r in valid])
T  = np.array([r["t"]  for r in valid])

# Fig R1: side-by-side hexbin scatter
print("Generating Fig R1...", flush=True)
fig, axes = plt.subplots(1,2,figsize=(13,5))
for ax, cost, label, cmap, clr in [
    (axes[0], CC, r"$C_{\rm cubic}=\chi_C^3$", "Reds", "#c0392b"),
    (axes[1], CS, r"$C_{\rm svd}=\chi_L\chi_R\min(\chi_L,\chi_R)$", "Blues", "#2980b9"),
]:
    rho,_ = spearmanr(cost, T)
    ax.hexbin(np.log10(np.maximum(cost,1e-9)), np.log10(np.maximum(T,1)),
              gridsize=40, cmap=cmap, mincnt=1, linewidths=0.0)
    ax.set_xlabel("log10(predicted cost)")
    ax.set_ylabel("log10(SVD time [ns])")
    ax.set_title(f"{label}\nSpearman ρ = {rho:.4f}")
    ax.text(0.05,0.95,f"ρ = {rho:.4f}", transform=ax.transAxes, va="top",
            fontsize=12, fontweight="bold", color=clr)
fig.suptitle(f"Fig R1: Random Circuits — Cost Model vs Measured SVD Time\n(N={len(valid):,} operations)", fontsize=13)
plt.tight_layout()
plt.savefig("results/figures/fig_random_R1_scatter.png")
plt.close()

# Fig R2: asymmetry histogram
print("Generating Fig R2...", flush=True)
asym = np.array([abs(r["cl"]-r["cr"])/max(r["cl"],r["cr"])
                 for r in valid if r["cl"] and r["cr"] and max(r["cl"],r["cr"])>0])
fig, ax = plt.subplots(figsize=(7,4))
ax.hist(asym, bins=30, color="#98C379", edgecolor="white", alpha=0.85)
ax.axvline(asym.mean(), color="red", linestyle="--", linewidth=2,
           label=f"Mean = {asym.mean():.3f}")
ax.set_xlabel("|χL − χR| / max(χL, χR)  [asymmetry ratio]")
ax.set_ylabel("Number of operations")
ax.set_title("Fig R2: Bond Dimension Asymmetry in Random Circuits\n(0 = symmetric, 1 = one neighbour is zero)")
ax.legend()
plt.tight_layout()
plt.savefig("results/figures/fig_random_R2_asymmetry.png")
plt.close()

# Fig R3: spearman bar chart
print("Generating Fig R3...", flush=True)
subsets = [
    ("All existing\ncircuits", 0.9511, 0.9815),
    ("Random\n(all)",          0.9569, 0.9781),
    ("Random\n10-qubit",       0.9593, 0.9645),
    ("Random\n20-qubit",       0.9152, 0.9586),
    ("Random\nJacobi solver",  0.7768, 0.8780),
    ("Random\nBDC solver",     0.9168, 0.9576),
]
labels  = [s[0] for s in subsets]
r_cubic = [s[1] for s in subsets]
r_svd   = [s[2] for s in subsets]
x = np.arange(len(labels)); w = 0.35
fig, ax = plt.subplots(figsize=(11,5))
b1 = ax.bar(x-w/2, r_cubic, w, color="#E06C75", label=r"$C_{\rm cubic}$", alpha=0.85)
b2 = ax.bar(x+w/2, r_svd,   w, color="#61AFEF", label=r"$C_{\rm svd}$",   alpha=0.85)
ax.set_xticks(x); ax.set_xticklabels(labels, fontsize=9)
ax.set_ylim(0.6, 1.02)
ax.set_ylabel("Spearman ρ")
ax.set_title("Fig R3: Spearman ρ — Standard vs Random Circuit Results")
ax.legend(loc="lower right")
ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
for bar in list(b1)+list(b2):
    ax.annotate(f"{bar.get_height():.3f}",
                xy=(bar.get_x()+bar.get_width()/2, bar.get_height()),
                xytext=(0,2), textcoords="offset points", ha="center", va="bottom", fontsize=8)
plt.tight_layout()
plt.savefig("results/figures/fig_random_R3_spearman_bar.png")
plt.close()

# Fig R4: shape-grouped scatter
print("Generating Fig R4...", flush=True)
sg = defaultdict(list)
for r in valid:
    sg[(r["rows"],r["cols"],r["solver"])].append(r)
stable=[(k,v) for k,v in sg.items() if len(v)>=3]
mt  = np.array([median([r["t"]  for r in v]) for k,v in stable])
rc_ = np.array([median([r["cc"] for r in v]) for k,v in stable])
rs_ = np.array([median([r["cs"] for r in v]) for k,v in stable])
cnt = np.array([len(v) for k,v in stable])

fig, axes = plt.subplots(1,2,figsize=(13,5))
for ax, cost, clr, label in [
    (axes[0], rc_, "#E06C75", r"$C_{\rm cubic}$"),
    (axes[1], rs_, "#61AFEF", r"$C_{\rm svd}$"),
]:
    v = ~(np.isnan(cost)|np.isnan(mt))
    rho,_ = spearmanr(cost[v], mt[v])
    sc = ax.scatter(np.log10(np.maximum(cost[v],1e-6)), np.log10(np.maximum(mt[v],1)),
                    c=np.log10(cnt[v]+1), cmap="viridis", s=40+cnt[v]*0.5, alpha=0.8, edgecolors="none")
    plt.colorbar(sc, ax=ax, label="log10(count)")
    ax.set_xlabel("log10(representative cost)")
    ax.set_ylabel("log10(median SVD time [ns])")
    ax.set_title(f"{label}\nSpearman ρ = {rho:.4f}")
fig.suptitle(f"Fig R4: Shape-Grouped — Random Circuits ({len(stable)} shapes, ≥3 obs)", fontsize=13)
plt.tight_layout()
plt.savefig("results/figures/fig_random_R4_shape_grouped.png")
plt.close()

print("Done. All 4 figures saved to results/figures/")
