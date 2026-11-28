"""Window and streaming residual LSTMs. All trace masks are applied to the loss only."""
from common import *
import torch
from torch import nn

torch.set_num_threads(1)
torch.set_num_interop_threads(1)
HISTORY = EXPLICIT[3:]


class Model(nn.Module):
    def __init__(self, family, hidden):
        super().__init__()
        self.recurrent = nn.LSTM(3, hidden, batch_first=True)
        self.output = nn.Sequential(nn.Linear(hidden + (len(HISTORY) if family == "L2" else 0), 8),
                                    nn.Tanh(), nn.Linear(8, 1))

    def forward(self, x, explicit, state=None, endpoint=False):
        z, state = self.recurrent(x, state)
        if endpoint:
            z = z[:, -1, :]
        if explicit is not None:
            z = torch.cat((z, explicit), dim=-1)
        return self.output(z).squeeze(-1), state


def traces(t):
    ix = list(groups(t))
    begin = np.zeros(len(t), dtype=int)
    for block in ix:
        begin[block] = block[0]
        assert np.array_equal(block,np.arange(block[0],block[-1]+1))
    return ix, begin


def batches(blocks, size, rng=None):
    blocks = sorted(blocks, key=len)
    if rng is not None:
        # Shuffle within nearby lengths to limit padding without fixing partners.
        for first in range(0, len(blocks), size*4):
            sector = blocks[first:first+size*4]
            rng.shuffle(sector)
            blocks[first:first+size*4] = sector
    packed = [blocks[k:k+size] for k in range(0,len(blocks),size)]
    if rng is not None:
        rng.shuffle(packed)
    return packed


def pack(index, start, width):
    length = np.array([len(ix) for ix in index])
    offset = np.arange(start,min(start+width,length.max()))
    active = offset[None,:] < length[:,None]
    rows = np.array([ix[0] for ix in index])[:,None] + np.minimum(offset,length[:,None]-1)
    return rows, active


class SequenceInputs:
    def __init__(self, t, mask, family):
        w = base.weights(t.loc[mask])
        values = t[XVA].to_numpy()
        # Only the unavailable derivative prefix is filled, with zero motion.
        values[:,1:] = np.nan_to_num(values[:,1:],nan=0.)
        assert np.isfinite(values).all()
        self.center,self.scale = base.normalize(values[mask],w)
        self.x = torch.tensor((values-self.center)/self.scale,dtype=torch.float32)
        self.exp_center = self.exp_scale = self.exp = None
        if family == "L2":
            exp = t[HISTORY].to_numpy()
            self.exp_center,self.exp_scale = base.normalize(exp[mask],w)
            self.exp = torch.tensor((exp-self.exp_center)/self.exp_scale,dtype=torch.float32)
        self.blocks,self.begin = traces(t)
        self.y = np.zeros(len(t),np.float32)
        self.w = np.zeros(len(t),np.float32)
        target = t.label_raw.to_numpy()[mask]
        self.target_center = float(w@target)
        self.target_scale = float(np.sqrt(w@(target-self.target_center)**2))
        self.y[mask] = (target-self.target_center)/self.target_scale
        self.w[mask] = w

    def windows(self, rows, window):
        offsets = np.arange(1-window,1)
        idx = np.maximum(rows[:,None]+offsets,self.begin[rows,None])
        return self.x[idx], self.exp[rows] if self.exp is not None else None

    def packed(self, rows):
        return self.x[rows], self.exp[rows] if self.exp is not None else None


def train_windows(net, data, mask, window, optimizer, loss, rng):
    selected = np.flatnonzero(mask)
    cdf = np.cumsum(data.w[selected], dtype=np.float64)
    cdf /= cdf[-1]
    for _ in range(base.CONFIG["epochs"]*base.CONFIG["steps_per_epoch"]):
        rows = selected[np.searchsorted(cdf,rng.random(base.CONFIG["batch_size"]))]
        optimizer.zero_grad()
        pred,_ = net(*data.windows(rows,window),endpoint=True)
        loss(pred,torch.from_numpy(data.y[rows])).mean().backward()
        optimizer.step()
    return base.CONFIG["epochs"]*base.CONFIG["steps_per_epoch"]


def train_stream(net, data, mask, optimizer, loss, rng):
    cfg = P["lstm"]
    eligible = [ix for ix in data.blocks if mask[ix].any()]
    assert all(data.w[ix].sum() > 0 for ix in eligible)
    updates = 0
    for _ in range(cfg["stream_epochs"]):
        epoch_batches = batches(eligible,cfg["stream_traces_per_batch"],rng)
        for idx in epoch_batches:
            optimizer.zero_grad()
            state = None
            for start in range(0,max(map(len,idx)),cfg["gradient_block_grids"]):
                rows, active = pack(idx,start,cfg["gradient_block_grids"])
                yp,state = net(*data.packed(rows),state)
                weighted = data.w[rows]*active
                if weighted.any():
                    term = loss(yp,torch.from_numpy(data.y[rows]))
                    (term*torch.from_numpy(weighted)).sum().mul(len(epoch_batches)).backward()
                state = tuple(s.detach() for s in state)
            optimizer.step()
            updates += 1
    return updates


def evaluate(net, data, prediction, mode, window):
    values = np.zeros(len(prediction))
    selected = np.flatnonzero(prediction)
    with torch.no_grad():
        if mode == "window":
            for start in range(0,len(selected),512):
                rows = selected[start:start+512]
                values[rows] = net(*data.windows(rows,window),endpoint=True)[0].numpy()
        else:
            blocks = [ix for ix in data.blocks if prediction[ix].any()]
            for idx in batches(blocks, 16):
                rows, active = pack(idx,0,max(map(len,idx)))
                yp,_ = net(*data.packed(rows))
                values[rows[active]] = yp.numpy()[active]

        # Online endpoint equality and timing, including creation of its input window.
        ix = next(ix for ix in data.blocks if prediction[ix].all())
        points = ix[:min(len(ix),256)]
        step_values = []
        state = None
        start = perf_counter()
        for row in points:
            if mode == "window":
                out,_ = net(*data.windows(np.array([row]),window),endpoint=True)
            else:
                out,state = net(*data.packed(np.array([[row]])),state)
            step_values.append(out.item())
        elapsed = (perf_counter()-start)*1e6/len(points)
        assert np.allclose(values[points],step_values,rtol=2e-4,atol=2e-5)
    return values[selected]*data.target_scale+data.target_center,elapsed


def fit(t, labels, predict, seed, spec):
    family, mode, hidden, window = spec
    data = SequenceInputs(t,labels,family)
    torch.manual_seed(seed)
    net = Model(family,hidden)
    decay = [parameter for name,parameter in net.named_parameters() if "weight" in name]
    bias = [parameter for name,parameter in net.named_parameters() if "bias" in name]
    optimizer = torch.optim.Adam([dict(params=decay,weight_decay=base.CONFIG["l2"]),
                                  dict(params=bias)],lr=base.CONFIG["learning_rate"])
    loss = nn.HuberLoss(delta=base.CONFIG["huber_delta_raw"]/data.target_scale,reduction="none")
    rng = np.random.default_rng(seed)
    start = perf_counter()
    if mode == "window":
        updates = train_windows(net,data,labels,window,optimizer,loss,rng)
    else:
        updates = train_stream(net,data,labels,optimizer,loss,rng)
    elapsed = perf_counter()-start
    net.eval()
    residual,online = evaluate(net,data,predict,mode,window)
    params = {k:v.detach().cpu().numpy() for k,v in net.state_dict().items()}
    stats = dict(parameters=sum(v.size for v in params.values()),train_s=elapsed,
                 online_us_per_grid=online,updates=updates,sequence=mode,precision="float32")
    params.update(sequence_center=data.center,sequence_scale=data.scale,
                  y_mean=data.target_center,y_scale=data.target_scale,
                  family=family,mode=mode,window=window,hidden=hidden)
    if data.exp is not None:
        params.update(history_features=HISTORY,history_center=data.exp_center,
                      history_scale=data.exp_scale)
    return residual,stats,params

