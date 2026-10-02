#!/usr/bin/env python3
"""Bounded, reproducible run of real NL_source SpikeProcessor on published data.
Never fit/tune detector parameters or temporal offsets to the ground-truth labels.
"""
from pathlib import Path
from zipfile import ZipFile
from io import BytesIO
import csv, hashlib, json, os, subprocess, time
from datetime import datetime, timezone
import numpy as np
from scipy.io import loadmat
P=Path(__file__).resolve().parent
REPO=Path(os.environ.get('NL_SOURCE_REPO',P.parents[2]))
FS=24000.0
TOLERANCES_MS=[0.5,1.0,2.0]
# Primary 2 ms tolerance is declared before evaluating outputs. Source markers
# are retained without peak-alignment or fitted filter/threshold delay correction.
GRID=[(family,noise) for family in ['Easy1','Easy2','Difficult1','Difficult2'] for noise in ['005','01','015','02']]

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def match(truth,det,tol):
    i=j=0;pairs=[]
    while i<len(truth) and j<len(det):
        if det[j]<truth[i]-tol:j+=1
        elif truth[i]<det[j]-tol:i+=1
        else:pairs.append((i,j));i+=1;j+=1
    return np.asarray(pairs,dtype=int).reshape(-1,2)
def score(truth,classes,det,tol):
    pairs=match(truth,det,tol);tp=len(pairs);fp=len(det)-tp;fn=len(truth)-tp
    delays=(det[pairs[:,1]]-truth[pairs[:,0]])/FS*1000
    return {'tp':tp,'fp':fp,'fn':fn,'truth_events':len(truth),'detected_events':len(det),
      'precision':tp/len(det) if len(det) else 0,'recall':tp/len(truth) if len(truth) else 0,
      'f1':2*tp/(2*tp+fp+fn) if 2*tp+fp+fn else 0,
      'matched_delta_ms_median':float(np.median(delays)) if tp else None,
      'matched_delta_ms_p05_p95':np.percentile(delays,[5,95]).tolist() if tp else None,
      'per_source_unit_recall_diagnostic_only':{str(int(u)):float(np.sum(classes[pairs[:,0]]==u)/np.sum(classes==u)) for u in np.unique(classes)}}

article=json.loads((P/'figshare_article.json').read_text())
provenance={'title':article['title'],'citation':article['citation'],'author':article['authors'][0]['full_name'],
 'landing_url':article['figshare_url'],'api_url':article['url'],'download_url':article['files'][0]['download_url'],
 'license':article['license'],'download_md5_published':article['files'][0]['computed_md5'],
 'download_md5_actual':hashlib.md5((P/'Simulator.zip').read_bytes()).hexdigest(),
 'download_sha256':digest(P/'Simulator.zip'),'validation_run_utc':datetime.now(timezone.utc).isoformat(), 'retrieved_utc':'2026-10-02',
 'dataset_kind':'Published standard synthetic ground-truth benchmark, not real acquisition data',
 'voltage_units':'Not declared in MAT metadata; source data treated as arbitrary normalized amplitude units',
 'harness_scale':0.001,'harness_scale_description':'Conventional 0.001 V per source amplitude unit; not a physical calibration. RMS thresholding should be scale invariant, checked separately.',
 'sample_rate_hz':FS,'sample_rate_evidence':'samplingInterval=0.041666666666666664 (interpreted as ms) in every MAT; independently corroborated by par.sr=24000 in archive times_C_Difficult1_noise015.mat',
 'time_index_conversion':'Original MAT spike_times retained, interpreted as MATLAB one-based samples and converted to zero-based by subtracting 1. Original indices also exported. No fitted temporal shift.',
 'event_matching':'Time-ordered one-to-one maximum-cardinality matching within symmetric tolerance; each truth/detection counts at most once. No class matching or sorting score.',
 'primary_tolerance_ms':2.0,'sensitivity_tolerances_ms':[0.5,1.0],
 'analysis_window_seconds':[0.252,59.998],
 'warmup_exclusion':'First 250 ms RMS warmup plus 2 ms boundary guard; final 2 ms boundary guard',
 'parameters':'SpikeProcConfig defaults except sampleRate=24000. Separate positive polarity mode flips negativePolarity=false; no other changes. notch50Hz, band250–6000Hz, order2, 4xRMS, refractory1ms.',
 'positive_mode_rationale':'Source archive example processing metadata par.detection=pos; positive mode is a separate configuration test, not a default-performance claim.',
 'repo_commit':subprocess.check_output(['git','-C',str(REPO),'rev-parse','HEAD'],text=True).strip(),
 'detector_source_sha256':{str(x.relative_to(REPO)):digest(x) for x in [REPO/'src/signal/spike_filter.cpp',REPO/'src/signal/spike_filter.h']},
 'limitations':['Single-channel synthetic data only; no simultaneous multichannel, electrode drift, hardware, ADC scaling, TCP framing, dropped packet, queue latency, or calibrated amplitude validation.',
 'Measures event detection, not supervised/unsupervised sorting or manual class separation.',
 'Matching is against original stored markers; causal filtering, chosen polarity, threshold crossings and waveform alignment cause nonzero timing offsets. A 2 ms tolerance is permissive and close overlaps can be temporally ambiguous.',
 'Per-unit recall breakdown is diagnostic attribution from time-only matching, not unit assignment accuracy.']}
assert provenance['download_md5_actual']==provenance['download_md5_published']
results=[];files=[]
(P/'traces').mkdir(exist_ok=True);(P/'truth').mkdir(exist_ok=True);(P/'events').mkdir(exist_ok=True)
start=time.monotonic()
with ZipFile(P/'Simulator.zip') as archive:
    legacy=loadmat(BytesIO(archive.read('Simulator/times_C_Difficult1_noise015.mat')),squeeze_me=True,struct_as_record=False)
    assert legacy['par'].sr==FS and legacy['par'].detection=='pos'
    for family,noise in GRID:
        stem=f'C_{family}_noise{noise}';member=f'Simulator/{stem}.mat'
        raw=archive.read(member);data=loadmat(BytesIO(raw),squeeze_me=True)
        trace=data['data'].astype('<f8');orig=data['spike_times'].astype(np.int64)
        labels=data['spike_class'][0].astype(np.int64);truth=orig-1
        assert len(trace)==1440000 and int(data['chan'])==1
        assert np.isclose(data['samplingInterval'],1000/FS) and np.isfinite(trace).all()
        assert len(truth)==len(labels) and (np.diff(truth)>=0).all() and set(labels)=={1,2,3}
        tracepath=P/'traces'/f'{stem}.f64';trace.tofile(tracepath)
        with (P/'truth'/f'{stem}.csv').open('w') as handle:
            writer=csv.writer(handle);writer.writerow(['original_mat_sample_index','zero_based_sample_index','source_unit'])
            writer.writerows(zip(orig,truth,labels))
        files.append({'name':stem,'zip_member':member,'member_sha256':hashlib.sha256(raw).hexdigest(),
          'trace_sha256':digest(tracepath),'samples':len(trace),'duration_seconds':len(trace)/FS,
          'channels':1,'units':{str(int(u)):int(np.sum(labels==u)) for u in np.unique(labels)},
          'truth_events_total':len(truth),'source_data_min_max':[float(trace.min()),float(trace.max())]})
        keep=(truth>=6048)&(truth<1439952);t=truth[keep];cl=labels[keep]
        for polarity in ['negative','positive']:
            outpath=P/'events'/f'{stem}_{polarity}.csv'
            command=[str(P/'detector_harness'),str(tracepath),str(FS),str(outpath),polarity,'512','0.001']
            terminal=json.loads(subprocess.check_output(command,text=True))
            events=np.loadtxt(outpath,skiprows=1,dtype=np.int64,ndmin=1)
            det=events[(events>=6048)&(events<1439952)]
            assert terminal['samples']==len(trace) and terminal['nonfinite_display']==0 and terminal['ready']
            for tolerance in TOLERANCES_MS:
                results.append({'dataset':stem,'polarity':polarity,'tolerance_ms':tolerance,
                  **score(t,cl,det,int(round(tolerance*FS/1000))), 'terminal':terminal})
            primary=results[-1]
            print(stem,polarity,'TP',primary['tp'],'FP',primary['fp'],'FN',primary['fn'],'P/R/F1',*[round(primary[k],4) for k in ['precision','recall','f1']],flush=True)
# Explicit invariance checks on one representative full source trace.
checks=[];stem='C_Easy1_noise01';tracepath=P/'traces'/f'{stem}.f64'
reference=np.loadtxt(P/'events'/f'{stem}_negative.csv',skiprows=1,dtype=np.int64,ndmin=1)
for label,batch,scale in [('single_sample_batches',1,0.001),('whole_trace_batch',1440000,0.001),('amplitude_scale_1000x',512,1.0)]:
    out=P/'events'/f'{stem}_check_{label}.csv'
    subprocess.run([str(P/'detector_harness'),str(tracepath),str(FS),str(out),'negative',str(batch),str(scale)],check=True,capture_output=True)
    test=np.loadtxt(out,skiprows=1,dtype=np.int64,ndmin=1)
    checks.append({'check':label,'exact_event_index_match':bool(np.array_equal(reference,test)),'reference_events':len(reference),'test_events':len(test)})
provenance['files']=files;provenance['runtime_seconds_including_conversion']=time.monotonic()-start
aggregates=[]
for polarity in ['negative','positive']:
 for tolerance in TOLERANCES_MS:
    rows=[r for r in results if r['polarity']==polarity and r['tolerance_ms']==tolerance]
    tp,fp,fn=[sum(r[k] for r in rows) for k in ['tp','fp','fn']]
    aggregates.append({'polarity':polarity,'tolerance_ms':tolerance,'tp':tp,'fp':fp,'fn':fn,'precision':tp/(tp+fp),'recall':tp/(tp+fn),'f1':2*tp/(2*tp+fp+fn)})
report={'provenance':provenance,'aggregates':aggregates,'invariance_checks':checks,'results':results}
(P/'results.json').write_text(json.dumps(report,indent=2)+'\n')
with (P/'results.csv').open('w') as f:
 keys=['dataset','polarity','tolerance_ms','truth_events','detected_events','tp','fp','fn','precision','recall','f1','matched_delta_ms_median']
 w=csv.DictWriter(f,fieldnames=keys,extrasaction='ignore');w.writeheader();w.writerows(results)
print(json.dumps({'aggregates':aggregates,'invariance_checks':checks,'run_seconds':provenance['runtime_seconds_including_conversion']},indent=2))
