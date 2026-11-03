#!/usr/bin/env python3
"""Read-only audit of sensor sessions. Python standard library only."""
import argparse,csv,json,collections,sys
from pathlib import Path


def is_valid(value):
    return str(value).strip().lower() in {'1','true','yes','y'}


def session_dirs(root):
    if (root/'session.json').is_file() or (root/'summary.csv').is_file():
        return [root]
    return sorted(p for p in root.iterdir() if p.is_dir() and
                  ((p/'session.json').is_file() or (p/'summary.csv').is_file()))


def audit_one(folder):
    issues=[]
    notes=[]
    doc={}
    if (folder/'session.json').is_file():
        try:doc=json.loads((folder/'session.json').read_text(encoding='utf-8-sig'))
        except (OSError,ValueError) as e:issues.append('session_json_unreadable:'+str(e))
    else:notes.append('missing_session_json')
    rows=[]
    if (folder/'summary.csv').is_file():
        try:
            with (folder/'summary.csv').open('r',newline='',encoding='utf-8-sig') as f:
                reader=csv.DictReader(f)
                fields=set(reader.fieldnames or [])
                miss={'round','track_id','valid'}-fields
                if miss:issues.append('missing_columns:'+','.join(sorted(miss)))
                rows=list(reader)
        except (OSError,ValueError,csv.Error) as e:issues.append('summary_csv_unreadable:'+str(e))
    else:issues.append('missing_summary_csv')
    track_rows=collections.defaultdict(list)
    seen=set()
    repeat_count=0
    for r in rows:
        track=str(r.get('track_id','')).strip()
        if not track:continue
        track_rows[track].append(r)
        key=(str(r.get('round','')),track)
        if key in seen:repeat_count+=1
        seen.add(key)
    if repeat_count:notes.append('multiple_records_per_round_track:'+str(repeat_count))
    mtracks={str(x.get('track_id')):x.get('metrics',{}) for x in doc.get('tracks',[]) if isinstance(x,dict)}
    for track,items in sorted(track_rows.items()):
        metrics=mtracks.get(track)
        if not isinstance(metrics,dict):continue
        for name,val in [('total_count',len(items)),('valid_count',sum(is_valid(r.get('valid','')) for r in items))]:
            expect=metrics.get(name)
            if expect is not None:
                try:
                    if int(expect)<val:issues.append(f'track_{track}_{name}_above_reported:{val}>{expect}')
                    elif int(expect)>val:notes.append(f'track_{track}_{name}_coverage:{val}/{expect}')
                except (TypeError,ValueError):issues.append(f'track_{track}_{name}_invalid')
    state=str(doc.get('state','unknown'))
    total=doc.get('completed_rounds','')
    target=doc.get('target_rounds','')
    return {'session_id':str(doc.get('session_id',folder.name)),'state':state,
        'target_rounds':target,'completed_rounds':total,
        'record_rows':len(rows),'track_count':len(track_rows),
        'valid_rows':sum(is_valid(r.get('valid','')) for r in rows),
        'issue_count':len(issues),'issues':issues,'notes':notes}


def audit(root,destination):
    folders=session_dirs(root)
    results=[audit_one(p) for p in folders]
    destination.mkdir(parents=True,exist_ok=True)
    fields=['session_id','state','target_rounds','completed_rounds','record_rows','track_count','valid_rows','issue_count']
    with (destination/'sessions.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader()
        for r in results:writer.writerow({k:r[k] for k in fields})
    states=dict(sorted(collections.Counter(r['state'] for r in results).items()))
    out={'input_session_count':len(results),'states':states,
         'sessions_with_issues':sum(bool(r['issues']) for r in results),
         'total_observation_rows':sum(r['record_rows'] for r in results),
         'sessions':results}
    (destination/'audit_summary.json').write_text(json.dumps(out,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return out


def main(argv=None):
    ap=argparse.ArgumentParser(description='Audit recorded sensor measurement sessions without editing source files')
    ap.add_argument('--input',required=True,type=Path)
    ap.add_argument('--output',required=True,type=Path)
    a=ap.parse_args(argv)
    if not a.input.is_dir():ap.error('input directory does not exist')
    result=audit(a.input,a.output)
    print(json.dumps({k:v for k,v in result.items() if k!='sessions'},ensure_ascii=False,indent=2))
    return 0

if __name__=='__main__':sys.exit(main())
