import json, math, sys
def sig(z):
    z=max(-30.0,min(30.0,z)); return 1/(1+math.exp(-z))
R = json.load(open(sys.argv[1]))
K = ["pot","long","safety","fouls","visit","big"]
for r in R: r["d_play"] = sum(r["d_"+k] for k in K)/len(K)
cut = int(len(R)*0.6); tr, te = R[:cut], R[cut:]
def X(r, feats): return [r["dc"]/400.0 if f == "dc" else r[f] for f in feats]
def fit(rows, feats, l2=0.5, it=100):
    w = [0.0]*len(feats); n=len(w)
    for _ in range(it):
        g=[0.0]*n; H=[[0.0]*n for _ in range(n)]
        for r in rows:
            x=X(r,feats); z=sum(a*b for a,b in zip(w,x)); p=sig(z)
            for i in range(n):
                g[i]+=(1-p)*x[i]
                for j in range(n): H[i][j]-=p*(1-p)*x[i]*x[j]
        for i in range(n): g[i]-=l2*w[i]; H[i][i]-=l2
        A=[H[i][:]+[-g[i]] for i in range(n)]
        for c in range(n):
            pv=max(range(c,n),key=lambda q:abs(A[q][c])); A[c],A[pv]=A[pv],A[c]
            for q in range(n):
                if q!=c and A[c][c]: f_=A[q][c]/A[c][c]; A[q]=[a-f_*b for a,b in zip(A[q],A[c])]
        d=[A[i][n]/A[i][i] if A[i][i] else 0 for i in range(n)]
        step=max(1.0,max(abs(x) for x in d)/0.5); w=[a+b/step for a,b in zip(w,d)]
        if max(abs(x) for x in d)<1e-8: break
    return w
def score(rows, feats, w):
    ll=0; right=0; calls=0
    for r in rows:
        z=sum(a*b for a,b in zip(w,X(r,feats))); p=sig(z); ll+=-math.log(max(p,1e-9))
        if abs(z)>1e-9: calls+=1; right+=z>0
    return ll/len(rows), right/max(1,calls)
print("fit on %d frames, tested on the next %d" % (len(tr), len(te)))
s=score(te,["dc"],[math.log(10)]); print("  %-24s %.4f  picks %.0f%%" % ("CUELO as it is", s[0], 100*s[1]))
for name, feats in (("CUELO rescaled",["dc"]),("CUELO + play",["dc","d_play"]),("CUELO + each measure",["dc"]+["d_"+k for k in K]),("play alone",["d_play"])):
    w=fit(tr,feats); s=score(te,feats,w)
    print("  %-24s %.4f  picks %.0f%%  weights %s" % (name, s[0], 100*s[1], [round(x,3) for x in w]))
