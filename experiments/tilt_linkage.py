from pathlib import Path

code = r'''import math
import numpy as np
import matplotlib.pyplot as plt

# ============================================================
# SENTRY TILT LINKAGE SEARCH
#
# Coordinate system:
#   lead-screw axis: x = 0
#   Z home/endstop:  z = 0
#
# B = (0, z)          moving Z-carriage link pin
# A0 = (31, 3)        camera-side link pin when camera is level
# C = (Cx, Cz)        camera tilt pivot
# L                   rigid link length, B -> A
#
# Camera target range: -60 deg to +60 deg
# ============================================================

A0 = np.array([31.0, 3.0])

CURRENT_C = np.array([40.0, 4.0])
CURRENT_L = 32.0

Z_MIN = 0.0
Z_MAX = 50.0

THETA_MIN = -60.0
THETA_MAX = +60.0
ANGLES = np.linspace(THETA_MIN, THETA_MAX, 481)

# Search ranges. Change these freely.
CX_VALUES = np.arange(28.0, 45.01, 0.5)
CZ_VALUES = np.arange(4.0, 18.01, 0.5)
L_VALUES  = np.arange(32.0, 50.01, 0.5)

# Practical filters.
MIN_TRANSMISSION = 0.30
MAX_SLOPE_RATIO = 3.0
MIN_REQUIRED_TRAVEL = 10.0

# Change this to inspect another ranked candidate.
PLOT_RANK = 0

# How many candidates to print.
N_PRINT = 20


def camera_pin_positions(C, angles_deg):
    """Rotate A0 around camera pivot C."""
    t = np.radians(angles_deg)
    c = np.cos(t)
    s = np.sin(t)

    dx = A0[0] - C[0]
    dz = A0[1] - C[1]

    ax = C[0] + c * dx - s * dz
    az = C[1] + s * dx + c * dz

    return ax, az


def evaluate(Cx, Cz, L, branch_sign):
    """
    Evaluate one geometric configuration.

    Link constraint:
        Ax^2 + (Az - z)^2 = L^2

    so:
        z = Az +/- sqrt(L^2 - Ax^2)
    """
    C = np.array([Cx, Cz])
    ax, az = camera_pin_positions(C, ANGLES)

    q = L * L - ax * ax

    if np.any(q <= 0):
        return None

    root = np.sqrt(q)
    z = az + branch_sign * root

    # Must remain inside shortened 0..50 mm Z axis.
    if np.min(z) < Z_MIN or np.max(z) > Z_MAX:
        return None

    # Z(theta) must be one-to-one over the entire requested camera range.
    dz = np.diff(z)
    increasing = np.all(dz > 1e-6)
    decreasing = np.all(dz < -1e-6)

    if not (increasing or decreasing):
        return None

    dz_ddeg = np.gradient(z, ANGLES)
    abs_slope = np.abs(dz_ddeg)

    if np.min(abs_slope) <= 1e-6:
        return None

    slope_ratio = np.max(abs_slope) / np.min(abs_slope)

    # Transmission metric = |sin(angle between rocker CA and link AB)|.
    # 1 is geometrically strong. 0 is a toggle/dead-center condition.
    px = ax - Cx
    pz = az - Cz

    lx = -ax
    lz = z - az

    rocker_len = np.sqrt(px * px + pz * pz)
    cross = np.abs(px * lz - pz * lx)
    transmission = cross / (rocker_len * L)

    min_transmission = float(np.min(transmission))

    travel = float(np.ptp(z))
    zmin = float(np.min(z))
    zmax = float(np.max(z))
    radius = float(np.linalg.norm(A0 - C))
    move_from_current = float(np.linalg.norm(C - CURRENT_C))

    # Minimum horizontal reach reserve.
    # Larger means the link stays farther from its geometric reach limit.
    reach_margin = float(np.min(L - np.abs(ax)))

    # Ranking:
    #   prefer good transmission
    #   prefer linear-ish z(theta)
    #   prefer roughly 20-35 mm of useful carriage travel
    #   mildly prefer smaller changes from the current pivot
    target_travel = 28.0

    score = (
        2.0 * (slope_ratio - 1.0)
        + 2.0 * (1.0 - min_transmission)
        + abs(travel - target_travel) / 15.0
        + move_from_current / 25.0
    )

    return {
        "Cx": float(Cx),
        "Cz": float(Cz),
        "L": float(L),
        "branch": int(branch_sign),
        "r": radius,
        "z": z,
        "zmin": zmin,
        "zmax": zmax,
        "travel": travel,
        "z0": float(z[len(z) // 2]),
        "slope_min": float(np.min(abs_slope)),
        "slope_max": float(np.max(abs_slope)),
        "slope_ratio": float(slope_ratio),
        "min_transmission": min_transmission,
        "reach_margin": reach_margin,
        "move_from_current": move_from_current,
        "score": float(score),
    }


def inspect_unfiltered(Cx, Cz, L):
    """Print basic reach information for a specific geometry."""
    C = np.array([Cx, Cz])
    ax, az = camera_pin_positions(C, ANGLES)

    max_abs_ax = float(np.max(np.abs(ax)))
    radius = float(np.linalg.norm(A0 - C))

    print(f"C = ({Cx:.1f}, {Cz:.1f}) mm, L = {L:.1f} mm")
    print(f"  rocker radius |C-A0| = {radius:.2f} mm")
    print(f"  max |Ax| over +/-60 deg = {max_abs_ax:.2f} mm")

    if L < max_abs_ax:
        print(
            f"  FAIL reach: link must be at least {max_abs_ax:.2f} mm "
            f"just to geometrically reach the full +/-60 deg."
        )
        return

    for sign in (+1, -1):
        q = L * L - ax * ax
        z = az + sign * np.sqrt(q)
        monotonic = (
            np.all(np.diff(z) > 1e-6)
            or np.all(np.diff(z) < -1e-6)
        )
        print(
            f"  branch {sign:+d}: "
            f"z = {np.min(z):.2f}..{np.max(z):.2f} mm, "
            f"travel = {np.ptp(z):.2f} mm, "
            f"monotonic = {monotonic}"
        )


def search():
    results = []

    for Cx in CX_VALUES:
        for Cz in CZ_VALUES:
            for L in L_VALUES:
                for sign in (+1, -1):
                    r = evaluate(Cx, Cz, L, sign)

                    if r is None:
                        continue

                    if r["min_transmission"] < MIN_TRANSMISSION:
                        continue

                    if r["slope_ratio"] > MAX_SLOPE_RATIO:
                        continue

                    if r["travel"] < MIN_REQUIRED_TRAVEL:
                        continue

                    results.append(r)

    results.sort(key=lambda x: x["score"])
    return results


def print_results(results):
    print()
    print("BEST FEASIBLE +/-60 DEG GEOMETRIES")
    print("=" * 108)
    print(
        f"{'#':>3} "
        f"{'Cx':>6} {'Cz':>6} {'L':>6} {'r':>6} "
        f"{'Z min':>7} {'Z max':>7} {'travel':>8} "
        f"{'linear':>8} {'trans':>7} {'reach':>7} {'move C':>7}"
    )
    print("-" * 108)

    for i, r in enumerate(results[:N_PRINT]):
        print(
            f"{i:3d} "
            f"{r['Cx']:6.1f} {r['Cz']:6.1f} {r['L']:6.1f} {r['r']:6.2f} "
            f"{r['zmin']:7.2f} {r['zmax']:7.2f} {r['travel']:8.2f} "
            f"{r['slope_ratio']:8.2f} {r['min_transmission']:7.2f} "
            f"{r['reach_margin']:7.2f} {r['move_from_current']:7.2f}"
        )


def plot_candidate(r):
    C = np.array([r["Cx"], r["Cz"]])
    ax_pin, az_pin = camera_pin_positions(C, ANGLES)
    z = r["z"]

    fig, (ax_mech, ax_curve) = plt.subplots(1, 2, figsize=(13, 6))

    # Draw linkage at -60, 0, +60 degrees.
    for angle in (-60.0, 0.0, +60.0):
        idx = int(np.argmin(np.abs(ANGLES - angle)))

        A = np.array([ax_pin[idx], az_pin[idx]])
        B = np.array([0.0, z[idx]])

        ax_mech.plot([C[0], A[0]], [C[1], A[1]], marker="o")
        ax_mech.plot([A[0], B[0]], [A[1], B[1]], marker="o")
        ax_mech.text(A[0], A[1], f"  {angle:+.0f} deg")

    ax_mech.axvline(0.0, linestyle="--")
    ax_mech.plot(C[0], C[1], marker="o")
    ax_mech.text(C[0], C[1], f"  C=({C[0]:.1f},{C[1]:.1f})")

    ax_mech.set_aspect("equal", adjustable="box")
    ax_mech.set_xlabel("x [mm]")
    ax_mech.set_ylabel("z [mm]")
    ax_mech.set_title(
        f"Linkage geometry | L={r['L']:.1f} mm | r={r['r']:.2f} mm"
    )
    ax_mech.grid(True)

    ax_curve.plot(ANGLES, z)
    ax_curve.axhline(Z_MIN, linestyle="--")
    ax_curve.axhline(Z_MAX, linestyle="--")
    ax_curve.set_xlabel("camera tilt [deg]")
    ax_curve.set_ylabel("Z carriage position [mm]")
    ax_curve.set_title(
        f"Required travel = {r['travel']:.2f} mm | "
        f"linearity ratio = {r['slope_ratio']:.2f}"
    )
    ax_curve.grid(True)

    fig.tight_layout()
    plt.show()


def main():
    print("CURRENT GEOMETRY")
    print("=" * 60)
    inspect_unfiltered(
        CURRENT_C[0],
        CURRENT_C[1],
        CURRENT_L
    )

    results = search()

    if not results:
        print()
        print("No candidates passed the current search limits.")
        print("Expand CX_VALUES, CZ_VALUES, or L_VALUES.")
        return

    print_results(results)

    rank = min(PLOT_RANK, len(results) - 1)
    print()
    print(f"Plotting rank {rank}:")
    print(results[rank])

    plot_candidate(results[rank])


if __name__ == "__main__":
    main()
'''

path = Path("/mnt/data/tilt_linkage_search.py")
path.write_text(code, encoding="utf-8")
print(f"Created {path}")
