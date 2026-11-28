"""Read-only V3 frozen candidate inference and historical parity."""
import hashlib
import sys
from pathlib import Path

from plan import HERE, ROOT

V3=ROOT/"联合建模_v3_状态与时序"
sys.path.insert(0,str(V3))
import common as c
import state

NAMES={"T0":"S0","T1":"S2"}
HASHES={
    "T0":"6C3EFBDAEEA9928328AAF8AC75BAA97FF68358360688CAE0B92BF5B708E4672B",
    "T1":"528E981F4E5070C1435CE455CEB8D79A334364BAF028B48C019E31ABB2726014",
}


def sha(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f,"sha256").hexdigest()


def weight(name):
    return V3/"results/fit"/f"internal__{NAMES[name]}__MLP__426.npz"


def check_inputs():
    audit=c.read(ROOT/"B0/audit_provenance.json")
    for path,key in ((c.prep.POSITION,"position_model_sha256"),(c.prep.ALPHA,"alpha_source_sha256"),
                     (c.prep.FRICTION,"friction_model_sha256")):
        if sha(path)!=audit[key]:
            raise ValueError(f"Frozen mechanism hash differs: {path}")


def predict(grid,name):
    path=weight(name)
    if sha(path)!=HASHES[name].lower():
        raise ValueError(f"Frozen weights hash differs: {path}")
    check_inputs()
    shaped,supported,_=state.transformed(grid,NAMES[name],None)
    with c.np.load(path,allow_pickle=False) as f:
        features=list(f["features"])
        if features!=c.EXPLICIT:
            raise ValueError("Frozen feature order differs")
        x=(shaped[features].to_numpy()-f["x_mean"])/f["x_scale"]
        arrays=[f[k] for k in ("W1","b1","W2","b2","W3","b3")]
        pred=c.base.forward(x,arrays)[2][:,0]*f["y_scale"]+f["y_mean"]+shaped.m1_raw.to_numpy()
    return shaped,pred,supported & c.np.isfinite(pred)


def reproduce():
    t=state.load_grid()
    test=t.loc[t.split.eq("internal")].reset_index(drop=True)
    info={}
    for name,est in NAMES.items():
        shaped,y,domain=predict(test,name)
        ref=c.np.load(V3/"results/fit"/f"internal__{est}__MLP__426.npy",allow_pickle=False)
        assert y.shape==ref.shape and c.np.array_equal(c.np.isfinite(y),c.np.isfinite(ref))
        ix=c.np.isfinite(ref)
        delta=float(c.np.abs(y[ix]-ref[ix]).max())
        assert delta<1e-7,(name,delta)
        info[name]=dict(points=len(y),finite=int(ix.sum()),original_valid=int(test.candidate.sum()),
                        common_valid=int((test.candidate.to_numpy()&domain).sum()),max_abs_raw=delta)
    return info


if __name__=="__main__":
    import json
    data=reproduce()
    (HERE/"historical_reproduction.json").write_text(json.dumps(data,indent=2),encoding="utf-8")
    print(data)
