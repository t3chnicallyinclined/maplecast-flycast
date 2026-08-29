#!/usr/bin/env python3
"""TDW2 floor study — Q2 (intrinsic dimension / PCA) + Q3 (prediction residual).
Operates on the 2104-frame char-struct series extracted from cap.gsta.mcrr."""
import numpy as np, struct, sys

pref=sys.argv[1] if len(sys.argv)>1 else '_bwlab/_td_cap'
C=np.load(pref+'_charst.npy')      # (F, 8664) uint8  -- 6 char structs
G=np.load(pref+'_gstate.npy')      # (F, 96)   uint8
F=C.shape[0]
STRIDE=0x5A4
SLOTS=['P1C1','P2C1','P1C2','P2C2','P1C3','P2C3']
print(f"char-struct series: {F} frames x {C.shape[1]} bytes; gstate {G.shape[1]} bytes")

def field_f32(slot,off):
    base=slot*STRIDE+off
    return C[:,base:base+4].copy().view(np.float32).reshape(F) if False else \
           np.frombuffer(C[:,base:base+4].tobytes(),dtype=np.float32).reshape(F,)
def col_f32(slot,off):
    base=slot*STRIDE+off
    b=np.ascontiguousarray(C[:,base:base+4]); return b.view(np.float32).reshape(-1)
def col_u16(slot,off):
    base=slot*STRIDE+off
    b=np.ascontiguousarray(C[:,base:base+2]); return b.view(np.uint16).reshape(-1)
def col_u8(slot,off): return C[:,slot*STRIDE+off].astype(np.int32)

# ---- confirm live motion + active slots ----
print("\nActivity check (per slot):")
active_slots=[]
for s,nm in enumerate(SLOTS):
    act=col_u8(s,0x000); px=col_f32(s,0x34); py=col_f32(s,0x38); sid=col_u16(s,0x144)
    nact=int((act!=0).sum())
    print(f"  {nm}: active_frames={nact}/{F}  px[{px.min():.0f},{px.max():.0f}] py[{py.min():.0f},{py.max():.0f}] "
          f"sid_uniq={len(np.unique(sid))} sid[{sid.min()},{sid.max()}]")
    if nact>F*0.5: active_slots.append(s)
print(f"  active slots (>50% frames): {[SLOTS[s] for s in active_slots]}")

# =========================================================================
# Q2  INTRINSIC DIMENSION
# =========================================================================
def pca_report(M, name):
    """M: (F, d) float. Report #PCs for 99% / 99.9% / 99.99% variance."""
    M=M.astype(np.float64)
    Mc=M-M.mean(0)
    # drop zero-variance cols
    v=Mc.var(0); keep=v>0
    Mc=Mc[:,keep]
    if Mc.shape[1]==0:
        print(f"  [{name}] no varying columns"); return
    U,S,Vt=np.linalg.svd(Mc,full_matrices=False)
    ev=S**2; cum=np.cumsum(ev)/ev.sum()
    def pk(th): return int(np.searchsorted(cum,th)+1)
    print(f"  [{name}] varying_dims={Mc.shape[1]:5d}  PCs@99%={pk(0.99):3d}  @99.9%={pk(0.999):3d}  @99.99%={pk(0.9999):4d}  (rank<= {min(Mc.shape)})")
    return cum

print("\n=== Q2 INTRINSIC DIMENSION (PCA/SVD, cumulative variance) ===")
# (A) byte-level on char_st changing columns (literal 'changing bytes as a vector')
Cb=C.astype(np.float64); varmask=Cb.var(0)>0
print(f"(A) BYTE-LEVEL char_st: {int(varmask.sum())} of {C.shape[1]} byte-columns ever vary")
pca_report(Cb[:,varmask],"char_st raw bytes")

# (B) byte-level standardized (z-score) — removes scale, keeps wrap structure
Cs=Cb[:,varmask]; Cs=(Cs-Cs.mean(0))/ (Cs.std(0)+1e-9)
pca_report(Cs,"char_st z-scored bytes")

# (C) SEMANTIC per-object float/int feature matrix (the interpretable DOF)
feats=[]; names=[]
fdefs=[('pos_x',0x34,'f'),('pos_y',0x38,'f'),('xscale',0x50,'f'),('yscale',0x54,'f'),
       ('scr_x',0xE0,'f'),('scr_y',0xE4,'f'),('facing',0x110,'b'),('anim_t',0x142,'u16'),
       ('sprite',0x144,'u16'),('anim_st',0x1D0,'u16'),('hp',0x420,'b'),('rhp',0x424,'b')]
for s in active_slots:
    for fn,off,ty in fdefs:
        if ty=='f': v=col_f32(s,off).astype(np.float64)
        elif ty=='u16': v=col_u16(s,off).astype(np.float64)
        else: v=col_u8(s,off).astype(np.float64)
        feats.append(v); names.append(f"{SLOTS[s]}.{fn}")
Msem=np.stack(feats,1)
print(f"(C) SEMANTIC matrix: {Msem.shape[1]} decoded fields over {len(active_slots)} active slots")
# standardize semantic fields (mixed units) then PCA
Msem_s=(Msem-Msem.mean(0))/(Msem.std(0)+1e-9)
pca_report(Msem_s,"semantic z-scored fields")

# (D) whole per-frame game-state churn: char_st + gstate changing bytes together
GB=G.astype(np.float64); gvar=GB.var(0)>0
allb=np.concatenate([Cb[:,varmask],GB[:,gvar]],1)
print(f"(D) char_st+gstate changing bytes: {allb.shape[1]} cols")
pca_report(allb,"char_st+gstate bytes")

# =========================================================================
# Q3  PREDICTION RESIDUAL
# =========================================================================
print("\n=== Q3 PREDICTION RESIDUAL (bytes/frame after a trivial predictor) ===")
# (a) constant-velocity extrapolation on pos_x/pos_y and screen_x/y
def cv_residual(vals, tol):
    # predict v[t] = 2 v[t-1] - v[t-2]; residual = |actual-pred|; count "miss" > tol
    pred=2*vals[1:-1]-vals[0:-2]
    act=vals[2:]
    err=np.abs(act-pred)
    return err, (err>tol)
print("\n(a) const-velocity (2*prev-prevprev) vs const-accel (3p1-3p2+p3) on position fields:")
posfields=[('pos_x',0x34),('pos_y',0x38),('scr_x',0xE0),('scr_y',0xE4)]
tol=0.01
cv_hits=0; cv_tot=0
for s in active_slots:
    for fn,off in posfields:
        v=col_f32(s,off).astype(np.float64)
        err,miss=cv_residual(v,tol)
        # const-acceleration predictor: v[t]=3v[t-1]-3v[t-2]+v[t-3]
        pa=3*v[3:-0-1+1] if False else (3*v[2:-1]-3*v[1:-2]+v[0:-3])
        erra=np.abs(v[3:]-pa)
        rate=miss.mean(); cv_hits+=int(miss.sum()); cv_tot+=len(miss)
        print(f"  {SLOTS[s]}.{fn:6s}: CV mean|resid|={err.mean():.4f}px miss>{tol}={100*rate:4.1f}% max={err.max():5.2f} | "
              f"CA mean|resid|={erra.mean():.4f}px miss={100*(erra>tol).mean():4.1f}% max={erra.max():.2f}")
print(f"  --> overall const-velocity: {100*cv_hits/cv_tot:.1f}% of (slot,posfield,frame) deviate > {tol}px")

# (b) animation-cursor playback: does sprite_id/anim_timer advance deterministically?
print("\n(b) animation-cursor playback (sprite_id + anim_timer deterministic advance):")
# model: anim_timer counts down each frame; when it hits 0, sprite_id steps to next cell.
# trivial predictor: sprite_id[t] = sprite_id[t-1] UNLESS a keyframe boundary -> then it changes.
# Measure: fraction of frames where sprite_id is UNCHANGED (held) vs changed (needs 1 event).
for s in active_slots:
    sid=col_u16(s,0x144); at=col_u16(s,0x142)
    sid_changes=int((np.diff(sid.astype(np.int32))!=0).sum())
    at_dec=int((np.diff(at.astype(np.int32))==-1).sum())  # counts that just decrement by 1
    print(f"  {SLOTS[s]}: sprite_id changes {sid_changes}/{F-1} frames ({100*sid_changes/(F-1):.1f}%); "
          f"anim_timer -1 steps {at_dec}/{F-1} ({100*at_dec/(F-1):.1f}%)")

# ---- residual bytes/frame after predictor: how many of the 8664 char_st bytes/frame are
#      NOT explained by (const-vel pos) + (held sprite/anim)?  approximate empirically:
#  for each frame, count changed bytes; subtract bytes attributable to predictable fields.
diff=(C[1:]!=C[:-1])
changed_per_frame=diff.sum(1)
print(f"\nchar_st raw changed bytes/frame: mean={changed_per_frame.mean():.1f} median={np.median(changed_per_frame):.0f} "
      f"p95={np.percentile(changed_per_frame,95):.0f} max={changed_per_frame.max()}")
# residual after removing the const-vel-predicted position bytes:
# a field is 'predicted' on a frame if its const-vel error<=tol (pos) or it's a held/stepped anim.
# Build a per-byte 'explained' mask per frame for the position + screen + scale float fields.
explained=np.zeros_like(diff)
for s in active_slots:
    for fn,off in [('pos_x',0x34),('pos_y',0x38),('scr_x',0xE0),('scr_y',0xE4),('xs',0x50),('ys',0x54)]:
        v=col_f32(s,off).astype(np.float64)
        err=np.abs(2*v[1:-1]-v[0:-2]-v[2:])
        base=s*STRIDE+off
        ok=err<=tol   # frames 2..F-1
        for i,o in enumerate(ok):
            if o: explained[i+1,base:base+4]=True
resid=diff & ~explained
resid_per_frame=resid.sum(1)
print(f"char_st residual bytes/frame AFTER const-velocity position prediction: "
      f"mean={resid_per_frame.mean():.1f} median={np.median(resid_per_frame):.0f} p95={np.percentile(resid_per_frame,95):.0f}")
np.savez(pref+'_pca_out.npz', changed_per_frame=changed_per_frame, resid_per_frame=resid_per_frame)
