
import wave, numpy as np, sys, os
def analyze(path, thr=1e-4, minrun_ms=1.0):
    w = wave.open(path,'rb'); n=w.getnframes(); ch=w.getnchannels(); sw=w.getsampwidth(); sr=w.getframerate()
    raw=w.readframes(n); w.close()
    if sw==2: x=np.frombuffer(raw,dtype='<i2').astype(np.float64)/32768.0
    elif sw==3:
        b=np.frombuffer(raw,dtype=np.uint8).reshape(-1,3); v=(b[:,0].astype(np.int32)|(b[:,1].astype(np.int32)<<8)|(b[:,2].astype(np.int32)<<16)); v=np.where(v>=1<<23,v-(1<<24),v); x=v.astype(np.float64)/(1<<23)
    elif sw==4: x=np.frombuffer(raw,dtype='<f4').astype(np.float64)
    else: x=np.frombuffer(raw,dtype='<i4').astype(np.float64)/(1<<31)
    x=x.reshape(-1,ch)
    total_frames=x.shape[0]; dur=total_frames/sr
    print(f"FILE {os.path.basename(path)} sr={sr} ch={ch} bits={sw*8} frames={total_frames} dur={dur:.4f}s")
    for c in range(min(ch,4)):
        y=x[:,c]; absy=np.abs(y)
        zero=absy<thr
        runs=[]; i=0
        while i<len(zero):
            if zero[i]:
                j=i
                while j<len(zero) and zero[j]: j+=1
                if (j-i)/sr>=minrun_ms/1000.0: runs.append((i,j,j-i))
                i=j
            else: i+=1
        tot=sum(r[2] for r in runs)/sr
        print(f"  ch{c}: peak={absy.max():.6f} rms={np.sqrt((y*y).mean()):.6f} zero_runs>= {minrun_ms}ms: n={len(runs)} total={tot*1000:.1f}ms max={ (max([r[2] for r in runs])/sr*1000.0) if runs else 0:.1f}ms")
        if runs: print("    first:", [(round(r[0]/sr,3), round(r[2]/sr*1000,1)) for r in runs[:12]])
    # expected vs actual duration when sample rate known; print adjacent-sample jump stats
    if ch>=1:
        y=x[:,0]; d=np.abs(np.diff(y)); 
        print(f"  dmax={d.max():.4f} p999={np.percentile(d,99.9):.5f} median={np.median(d):.6f}")
for p in sys.argv[1:]: analyze(p)
