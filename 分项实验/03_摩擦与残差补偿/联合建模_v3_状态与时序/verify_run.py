"""Check scope, frozen files, score masks, and generated artifact integrity."""
from xml.etree import ElementTree
from common import *
from state import load_grid


def main():
    seal = read(RESULTS/"selection_seal.json")
    for key, folder in (("code",HERE),("artifacts",RESULTS)):
        for name, expected in seal[key].items():
            assert file_hash(folder/name) == expected, name
    assert inventory() == read(RESULTS/"input_hashes.json")

    t = load_grid()
    fixture = read(RESULTS/"baseline_verification.json")
    valid = t.candidate & t.split.eq("internal")
    t0 = pd.read_csv(RESULTS/"C_internal_internal_predictions.csv.gz")
    scored = t0[t0.candidate]
    expected = t.loc[valid,["trace","frame"]].reset_index(drop=True)
    assert scored[["trace","frame"]].reset_index(drop=True).equals(expected)
    assert len(scored) == fixture["rows"]
    assert scored.unit.nunique() == fixture["units"]
    all_models = [f"T{model}_{seed}" for model in range(4) for seed in P["seeds"]]
    assert np.isfinite(scored[all_models]).all().all()

    cold = scored.loc[scored.trace.eq("B1:R5-P2-V40-A600_leg1")&scored.frame.eq(26200)]
    assert len(cold) == 1 and cold.current_raw.iat[0] == 623
    assert cold.v_pulse_s.iat[0] < -2000
    final = pd.read_csv(RESULTS/"C_internal_metrics.csv")
    overall = final[final.cohort.eq("ALL")&final.phase.eq("ALL")]
    assert set(overall.seed)==set(P["seeds"])
    assert overall.rows.eq(len(scored)).all()
    assert not overall[["rmse","mae","bias","p95","max_abs"]].isna().any().any()
    for name in ("brake_reversal","low_speed_hold","long_hold","real_reversal"):
        svg = ElementTree.parse(RESULTS/f"{name}.svg").getroot()
        assert svg.find(".//{http://www.w3.org/2000/svg}polyline") is not None

    save("verification.json",dict(status="passed",protected_files=len(inventory()),
                                  internal_rows=len(scored),internal_units=scored.unit.nunique(),
                                  models=len(all_models),original_anomaly_retained=True,
                                  figures_checked=4))
    print("Protected sources, original evaluation masks and 4 SVG files checked.",flush=True)


if __name__ == "__main__":
    main()

