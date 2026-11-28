"""Diagnostic intervention: reset the fitted recurrent state at D HOLD onset."""
from common import *
from sequence import Model, torch


def check(grid):
    result = []
    select = grid.cohort.eq("D") & grid.split.eq("dev")
    for k in (RESULTS/"fit").glob("*__S0__L*_stream_16__*.npz"):
        fold,state,name,seed = k.stem.split("__")
        if not fold.isdigit():
            continue
        with np.load(k,allow_pickle=False) as p:
            family = str(p["family"])
            net = Model(family,int(p["hidden"]))
            net.load_state_dict({key:torch.from_numpy(p[key]) for key in net.state_dict()})
            net.eval()
            for _,g in grid.loc[select & grid["round"].eq(int(fold))].groupby("trace",sort=False):
                x = g[XVA].to_numpy(copy=True)
                x[:,1:] = np.nan_to_num(x[:,1:],nan=0.)
                x = torch.tensor((x-p["sequence_center"])/p["sequence_scale"],dtype=torch.float32)[None]
                exp = None
                if family == "L2":
                    features = g[p["history_features"].tolist()].to_numpy()
                    exp = torch.tensor((features-p["history_center"])/p["history_scale"],dtype=torch.float32)[None]
                cut = np.flatnonzero(g.phase.eq("hold"))[0]
                with torch.no_grad():
                    history, _ = net(x,exp)
                    reset, _ = net(x[:,cut:], None if exp is None else exp[:,cut:])
                delta = (history[0,cut:]-reset[0]).numpy()*p["y_scale"]
                hold = g.iloc[cut:]
                for lo,hi in zip(P["hold_windows_s"][:-1],P["hold_windows_s"][1:]):
                    mask = (hold.hold_age_s >= lo) & (hold.hold_age_s < hi) & hold.candidate
                    if mask.any():
                        values = np.abs(delta[mask])
                        result.append(dict(model=name,fold=fold,seed=seed,trace=g.trace.iat[0],
                                           hold_bin=f"{lo:g}-{hi:g}s",points=len(values),
                                           mean_abs_delta_raw=values.mean(), max_abs_delta_raw=values.max()))
    csv("B_D_hold_hidden_memory.csv",result)
    return pd.DataFrame(result)

