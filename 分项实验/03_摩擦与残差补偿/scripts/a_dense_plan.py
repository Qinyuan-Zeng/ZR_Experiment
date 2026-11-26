"""Print the proposed stage-A offline point table as CSV. No device or SDK access."""
import argparse
import csv
import sys


SPEEDS = (2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000,
          16000, 18000, 20000, 24000, 28000, 32000, 36000, 40000)
HELD_SPEEDS = {4000, 14000, 32000}
WINDOW = (5000, 22000)


def planned_pairs():
    # Interleave slow and fast settings, rotating their position between rounds.
    ordered = []
    for i in range(len(SPEEDS) // 2):
        ordered.extend((SPEEDS[i], SPEEDS[-i - 1]))
    ordered.append(SPEEDS[len(SPEEDS) // 2])
    for round_index in range(5):
        offset = 7 * round_index % len(ordered)
        for i in range(len(ordered)):
            speed = ordered[(offset + i) % len(ordered)]
            first_direction = 1 if (SPEEDS.index(speed) + round_index) % 2 == 0 else -1
            if round_index == 4:
                split = "independent_session"
            elif speed in HELD_SPEEDS:
                split = "speed_validation"
            else:
                split = "development"
            yield round_index + 1, i + 1, speed, first_direction, split


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args()
    pairs = list(planned_pairs())
    assert len({(r, v) for r, _, v, _, _ in pairs}) == 85
    if args.summary:
        print("17 speeds; 5 rounds; 85 round trips; 170 measurement legs")
        for split in ("development", "speed_validation", "independent_session"):
            print(split, sum(row[-1] == split for row in pairs))
        return
    out = csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(("round", "batch_in_round", "pair_id", "leg", "speed_pulse_s",
                  "direction", "start_pulse", "end_pulse", "accel_pulse_s2",
                  "estimated_plateau_lo_pulse", "estimated_plateau_hi_pulse",
                  "grid_lo_pulse", "grid_hi_pulse", "grid_step_pulse", "split"))
    for r, order, v, first, split in pairs:
        # Existing TRAP preview: acceleration distance plus 50 ms derivative margin.
        accel = 400000
        crop = v * v / (2 * accel) + 0.05 * v
        if crop > WINDOW[0] or 32000 - crop < WINDOW[1]:
            raise ValueError(f"No common nominal analysis window at {v} pulses/s")
        for leg, direction in enumerate((first, -first), 1):
            start, end = (0, 32000) if direction > 0 else (32000, 0)
            out.writerow((r, (order - 1) // 4 + 1, f"R{r}-V{v}", leg, v, direction,
                          start, end, accel, crop, 32000 - crop, *WINDOW, 250, split))


if __name__ == "__main__":
    main()
