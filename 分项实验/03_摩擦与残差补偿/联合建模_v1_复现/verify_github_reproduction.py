"""Verify the rerun against tracked sources and the committed reference artifacts."""
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
import pandas as pd

OUT = Path(__file__).resolve().parent
STAGE = OUT.parent
REPO = STAGE.parents[1]
REFERENCE = STAGE / "联合建模_v1"
COMMIT = "676b7fef50e5b11f81cff363f57d0d22b256fd2d"
REPO_URL = "https://github.com/Qinyuan-Zeng/ZR_Experiment"


def git(*args):
    return subprocess.check_output(["git", "-C", str(REPO), *args]).decode("utf-8")


def sha(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def tracked(tree, path):
    relative = path.relative_to(REPO).as_posix()
    return tree[relative]


def main():
    assert git("rev-parse", "HEAD").strip() == COMMIT
    assert not git("status", "--porcelain", "-uno")
    tree = {}
    for row in git("ls-tree", "-r", "-z", COMMIT).strip("\0").split("\0"):
        obj, name = row.split("\t", 1)
        tree[name] = obj.split()[-1]
    sources = pd.read_csv(OUT/"sources.csv")
    files = {}
    for row in sources.itertuples(index=False):
        for name, digest in (dict(json.loads(row.inputs)) | {row.acceptance:row.acceptance_sha256}).items():
            path = STAGE/name
            if path not in files:
                assert sha(path) == digest, path
                files[path] = (tracked(tree,path),digest)
    config = json.loads((OUT/"data_config.json").read_text(encoding="utf-8"))
    fixed = {
        "B1/manifest/Z-B1-20261127-01/protocol.json",
        "B1/manifest/Z-B1-20261127-02/protocol.json",
        "B1/results/Z-B1-20261127-02/accepted_pairs.csv",
        "C/manifest/Z-C-HISTORY-01/protocol.json",
        "C/results/Z-C-HISTORY-01/accepted_trials.csv",
        "D/manifest/Z-D-HOLD-01/protocol.json",
        "D/results/Z-D-HOLD-01/accepted_trials.csv",
        "D/results/Z-D-HOLD-01/hold_windows.csv",
    }
    for name in sorted(fixed):
        path = STAGE/name
        files[path] = (tracked(tree,path),sha(path))
    assert len(pd.read_csv(STAGE/"D/results/Z-D-HOLD-01/hold_windows.csv").query("valid")) == 90
    from prepare_joint import POSITION, ALPHA, FRICTION
    for path in (POSITION, ALPHA, FRICTION):
        digest = sha(path)
        assert digest == config["models"][path.name]
        files[path] = (tracked(tree,path),digest)
    csv_names = ["evaluation.csv","seed_sensitivity.csv","coverage.csv","splits.csv",
                 "C_matched_summary.csv","D_hold_summary.csv","D_hold_ablation_by_age.csv",
                 "paired_regression_check.csv"]
    comparison = {}
    for name in ["joint_samples.csv.gz","internal_predictions.csv.gz",*csv_names]:
        path = OUT/name
        ref = REFERENCE/name
        equal = sha(path)==sha(ref)
        if not equal:
            pd.testing.assert_frame_equal(pd.read_csv(path,low_memory=False),pd.read_csv(ref,low_memory=False),
                                          check_exact=False,rtol=1e-8,atol=1e-8)
        comparison[name] = dict(sha256=sha(path),reference_sha256=sha(ref),byte_exact=equal)
    models = {}
    for path in sorted((OUT/"models").glob("*.npz")):
        with np.load(path,allow_pickle=False) as a, np.load(REFERENCE/"models"/path.name,allow_pickle=False) as b:
            assert sorted(a.files)==sorted(b.files)
            equal = all(np.array_equal(a[k],b[k]) for k in a.files)
            if not equal:
                for k in a.files:
                    if np.issubdtype(a[k].dtype,np.number):
                        np.testing.assert_allclose(a[k],b[k],rtol=1e-8,atol=1e-8)
                    else:
                        np.testing.assert_array_equal(a[k],b[k])
            models[path.name] = dict(arrays_exact=equal,sha256=sha(path))
    assert len(models)==4
    pd.DataFrame([(p.relative_to(REPO).as_posix(),obj,h) for p,(obj,h) in files.items()],
                 columns=["repository_path","git_blob","sha256"]).sort_values("repository_path").to_csv(
        OUT/"github_inputs.csv",index=False,encoding="utf-8-sig")
    doc = dict(repository=REPO_URL, default_branch="main", commit=COMMIT, tracked_inputs=len(files),
               source_trace_counts=sources.cohort.value_counts().to_dict(), comparison=comparison,
               models=models,E_used=False)
    (OUT/"github_reproduction.json").write_text(json.dumps(doc,indent=2),encoding="utf-8")
    print("Verified tracked inputs:",len(files))
    print("Comparisons:",{k:v["byte_exact"] for k,v in comparison.items()})
    print("Model arrays:",{k:v["arrays_exact"] for k,v in models.items()})


if __name__=="__main__":
    main()
