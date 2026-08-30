import csv, statistics
from pathlib import Path

ROOT = Path('/home/steffi/thesis-mps-routing/results')

# Load BuildChain costs
bc = {}
with open(ROOT / 'temporal_mapping_results.csv') as f:
    for row in csv.DictReader(f):
        if row['strategy'] == 'BuildChain':
            bc[row['circuit']] = float(row['predicted_cost'])

# Load QMAP heuristic costs
qmap = {}
with open(ROOT / 'qmap_benchmark_results.csv') as f:
    for row in csv.DictReader(f):
        qmap[row['circuit']] = {
            'cost':  float(row['predicted_cost']),
            'swaps': int(row['qmap_swaps']),
            'ms':    float(row['mapping_time_ms']),
        }

print()
print(f"{'Circuit':<18} {'BuildChain':>11} {'QMAP-H':>8} {'Delta%':>8}  {'SWAPs':>6}  {'Map_ms':>7}  Result")
print('-' * 75)

wins = ties = losses = 0
deltas = []
for circ in sorted(bc.keys()):
    if circ not in qmap:
        continue
    bc_c  = bc[circ]
    qm_c  = qmap[circ]['cost']
    d     = (bc_c - qm_c) / bc_c * 100   # positive = QMAP is better
    deltas.append(d)
    mark  = 'WIN ✓' if d > 0.1 else ('TIE =' if abs(d) <= 0.1 else 'LOSS✗')
    if   d >  0.1: wins   += 1
    elif d < -0.1: losses += 1
    else:          ties   += 1
    print(f"{circ:<18} {bc_c:>11.1f} {qm_c:>8.1f} {d:>+7.1f}%  "
          f"{qmap[circ]['swaps']:>6}  {qmap[circ]['ms']:>7.1f}  {mark}")

avg = statistics.mean(deltas)
print('-' * 75)
print(f"{'AGGREGATE':<18} {'':>11} {'':>8} {avg:>+7.1f}%  "
      f"Wins={wins}  Ties={ties}  Losses={losses}")
print()
print("=== KEY FINDING ===")
if wins > losses:
    print(f"QMAP-Heuristic BEATS BuildChain on {wins}/{len(deltas)} circuits!")
elif wins == 0 and losses == 0:
    print("QMAP-Heuristic TIES BuildChain on all circuits.")
else:
    print(f"QMAP-Heuristic wins {wins}, ties {ties}, loses {losses} vs BuildChain.")

# Correlation: min-SWAP vs min-MPS-cost?
print()
print("=== SWAP COUNT vs MPS COST CORRELATION ===")
for circ in sorted(bc.keys()):
    if circ not in qmap:
        continue
    qm_swaps = qmap[circ]['swaps']
    qm_cost  = qmap[circ]['cost']
    bc_cost  = bc[circ]
    wins_cost  = "QMAP cheaper" if qm_cost < bc_cost else ("same" if qm_cost == bc_cost else "BC cheaper")
    print(f"  {circ:<18}  QMAP={qm_swaps:>4} SWAPs  MPS-cost={qm_cost:>7.1f}  BC-cost={bc_cost:>7.1f}  → {wins_cost}")
