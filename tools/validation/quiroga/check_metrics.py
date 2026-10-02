#!/usr/bin/env python3
"""Independent checks on saved detector output and matching assumptions."""
from pathlib import Path
import json
import numpy as np
P=Path(__file__).resolve().parent
r=json.loads((P/'results.json').read_text())
def nmatch(a,b,tol):
    i=j=n=0
    while i<len(a) and j<len(b):
        if a[i]+tol<b[j]:i+=1
        elif b[j]+tol<a[i]:j+=1
        else:n+=1;i+=1;j+=1
    return n
# Compare the greedy cardinality with a separate dynamic-programming oracle.
rng=np.random.default_rng(20261002)
for _ in range(10000):
    a=np.sort(rng.integers(0,25,size=int(rng.integers(0,10))))
    b=np.sort(rng.integers(0,25,size=int(rng.integers(0,10))))
    tol=int(rng.integers(0,6));dp=np.zeros((len(a)+1,len(b)+1),dtype=int)
    for i in range(1,len(a)+1):
      for j in range(1,len(b)+1):
        dp[i,j]=max(dp[i-1,j],dp[i,j-1],dp[i-1,j-1]+int(abs(a[i-1]-b[j-1])<=tol))
    assert nmatch(a,b,tol)==dp[-1,-1]
checks={'matching_random_cases_vs_dp':10000,'primary_rows_independently_verified':0,'null_control':[],'mat_index_convention_sensitivity':[]}
for polarity in ['negative','positive']:
    nulltp=origindextp=totald=totalt=0
    for source in r['provenance']['files']:
        stem=source['name'];truth=np.loadtxt(P/'truth'/f'{stem}.csv',delimiter=',',skiprows=1,dtype=int)[:,1]
        det=np.loadtxt(P/'events'/f'{stem}_{polarity}.csv',skiprows=1,dtype=int,ndmin=1)
        assert (np.diff(det)>0).all()
        t=truth[(truth>=6048)&(truth<1439952)];d=det[(det>=6048)&(det<1439952)]
        rows=[x for x in r['results'] if x['dataset']==stem and x['polarity']==polarity]
        for row in rows:
            tp=nmatch(t,d,int(round(row['tolerance_ms']*24)))
            assert tp==row['tp'] and row['fp']==len(d)-tp and row['fn']==len(t)-tp
            assert np.isclose(row['precision'],tp/len(d))
            assert np.isclose(row['recall'],tp/len(t))
            assert np.isclose(row['f1'],2*tp/(len(d)+len(t)))
            checks['primary_rows_independently_verified']+=1
        # Shift detections circularly by 0.5 s within the evaluation window.
        shifted=np.sort((d-6048+12000)%(1439952-6048)+6048)
        nulltp+=nmatch(t,shifted,48);origindextp+=nmatch(t+1,d,48)
        totalt+=len(t);totald+=len(d)
    for key,tp in [('null_control',nulltp),('mat_index_convention_sensitivity',origindextp)]:
        checks[key].append({'polarity':polarity,'tolerance_ms':2,'tp':tp,'fp':totald-tp,'fn':totalt-tp,
          'precision':tp/totald,'recall':tp/totalt,'f1':2*tp/(totald+totalt)})
(P/'metric_checks.json').write_text(json.dumps(checks,indent=2)+'\n')
print(json.dumps(checks,indent=2))
