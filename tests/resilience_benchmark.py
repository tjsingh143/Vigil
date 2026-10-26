#!/usr/bin/env python3
"""Matched, sequential Linux benchmarks. No external database/target argument.
Creates a detached baseline worktree and disposable native spools/PG clusters.
All raw trials retained; percentiles use nearest rank, no outlier removal.
"""
from verification import require  # also refuses optimized execution before fixture imports
import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from evidence import Evidence, source_digest
from monitor_integration import Daemon, bus, config_text, send, source
from persistence_integration import wait_p, caught_up, latest
from resilience_database_contract import ReplayPostgres
from resilience_integration import create, daemon, wait_local, inventory
from simulator.protocol import reading, heartbeat
from fault_scenarios import load

BASELINE='d179681a40497ada533d2f61e50b5ac60a6ca293'
ROOT=Path(__file__).resolve().parents[1]


def distribution(values):
    v=sorted(values)
    require((v))
    return dict(n=len(v),min=v[0],p50=v[math.ceil(.5*len(v))-1],p95=v[math.ceil(.95*len(v))-1],
                p99=v[math.ceil(.99*len(v))-1],max=v[-1],mean=statistics.mean(v),
                stdev=statistics.stdev(v) if len(v)>1 else 0)


def resource(d,path):
    fields=Path(f'/proc/{d.child.pid}/stat').read_text().rpartition(')')[2].split()
    status=dict(line.split(':',1) for line in Path(f'/proc/{d.child.pid}/status').read_text().splitlines() if ':' in line)
    return dict(at_s=time.monotonic(),cpu_ticks=int(fields[11])+int(fields[12]),
                rss_bytes=int(status['VmRSS'].split()[0])*1024,hwm_bytes=int(status['VmHWM'].split()[0])*1024,
                files={} if path is None else {s or 'db':Path(str(path)+s).stat().st_size for s in ('','-wal','-shm') if Path(str(path)+s).exists()})


@contextmanager
def running(exe,pg,path):
    config=config_text(pods=(1,),timeout=500,status=100,rate=0)
    if path:
        with daemon(exe,path,env=pg.env(),capacity=1024,pipesize=1024*1024,config=config) as d: yield d
    else:
        d=Daemon(str(exe),config,persistence=True,extra_args=('--queue-capacity','1024'),env=pg.env(),pipesize=1024*1024)
        try: yield d
        finally: d.close()


def stream(d,raw,start_counter,count,rate,path):
    start=time.monotonic(); samples=[]; lateness=[]
    for n in range(count):
        target=start+n/rate
        while time.monotonic()<target: d.pump(max(0,min(.005,target-time.monotonic())))
        lateness.append(max(0,time.monotonic()-target))
        send(raw,reading(1,0,2200,0,(start_counter+n)%256))
        if n%10==0: send(raw,heartbeat(1,1,(start_counter+n)//100,((start_counter+n)//10)%256))
        d.pump(0)
        if n%100==0: samples.append(resource(d,path))
    end=start+count/rate
    while time.monotonic()<end: d.pump(max(0,min(.005,end-time.monotonic())))
    return dict(elapsed_s=time.monotonic()-start,samples=samples,send_lateness_s=distribution(lateness))


def trial(label,exe,init,seconds,root,output):
    pg=ReplayPostgres(resilience=label=='resilience')
    try:
        path=create(root/'state',init) if label=='resilience' else None
        with running(exe,pg,path) as d,bus() as raw:
            wait_p(d,caught_up)
            stream(d,raw,0,200,100,path)  # identical two-second warmup, excluded
            warm=wait_p(d,lambda p:p['readings_written']==200 and caught_up(p))
            warm_local=next((e for e in reversed(d.events) if e['event']=='outbox'),None)
            before=resource(d,path)
            measured=stream(d,raw,200,seconds*100,100,path)
            cpu_end=resource(d,path)
            endpoint=latest(d)
            endpoint_local=next((e for e in reversed(d.events) if e['event']=='outbox'),None)
            status_window_s=(endpoint['now_ms']-warm['now_ms'])/1000
            require((status_window_s>0))
            complete_start=time.monotonic()
            healthy=wait_p(d,lambda p:p['readings_written']==200+seconds*100 and caught_up(p),timeout=40)
            completion_s=time.monotonic()-complete_start
            local=wait_local(d,lambda s:s['local_readings']==200+seconds*100) if path else None
            remote_records=healthy['records_written']-warm['records_written']
            if path: require((local['local_committed']==healthy['records_written']))
            # Save the healthy boundary before deliberately creating staleness.
            healthy_resources=measured['samples']+[before,cpu_end]
            reading_count=200+seconds*100
            require((healthy['readings_dropped']==healthy['events_dropped']==0))
            require((pg.sql("SELECT count(*) FROM vigil.history WHERE kind='reading'")==str(reading_count)))
            silent=d.wait(lambda s:s['valid_readings']==reading_count and source(s,1,0)['freshness']=='stale')
            alarm=next(e for e in reversed(d.events) if e['event']=='alarm' and e['pod']==1 and e['channel']==0 and e['kind']=='communication_lost' and e['state']=='active')
            age=alarm['now_ms']-source(silent,1,0)['last_valid_ms']
            require((500<=age<=720))
            wait_p(d,caught_up)
            pg.stop()
            wait_p(d,lambda p:not p['connected'] and p['degraded'],timeout=5)
            # 600 readings fits the common RAM lane; this measures replay fairly.
            outage=stream(d,raw,reading_count,600,100,path)
            if path: wait_local(d,lambda s:s['local_readings']==reading_count+600)
            pending=wait_p(d,lambda p:p['readings_queued']==reading_count+600)
            require((pending['readings_dropped']==pending['events_dropped']==0))
            # Preserve same-process backlog, no kill in comparative performance.
            restored=time.monotonic(); pg.start(); available=time.monotonic()
            d.pump(0)
            recovery_start_index=len(d.events)
            done=wait_p(d,lambda p:p['readings_written']==reading_count+600 and caught_up(p),timeout=90)
            drained=time.monotonic()
            first=next(e for e in d.events[recovery_start_index:] if e['event']=='persistence' and e['readings_written']>reading_count)
            require((done['readings_dropped']==done['events_dropped']==0))
            require((pg.sql("SELECT count(*) FROM vigil.history WHERE kind='reading'")==str(reading_count+600)))
            d.stop(); final=latest(d)
            require((final['abandoned_readings']==final['abandoned_events']==0))
        inv=inventory(path,output/'local-final') if path else None
        if inv:
            require((inv['meta']['pending_records']==inv['meta']['pending_batches']==inv['meta']['pending_payload_bytes']==0))
            require((inv['meta']['admitted_readings']==inv['meta']['retired_readings']==reading_count+600))
        wall=cpu_end['at_s']-before['at_s']
        result=dict(label=label,warmup_readings=200,measured_readings=seconds*100,healthy=healthy,local=local,
            measured=measured,healthy_completion_observation_s=completion_s,
            observed_monitor_readings_s=seconds*100/measured['elapsed_s'],
            healthy_remote_readings_s=seconds*100/(measured['elapsed_s']+completion_s),
            healthy_remote_records_s=remote_records/(measured['elapsed_s']+completion_s),
            status_window_s=status_window_s,endpoint_persistence=endpoint,endpoint_outbox=endpoint_local,
            sampled_remote_records_s=(endpoint['records_written']-warm['records_written'])/status_window_s,
            sampled_remote_readings_s=(endpoint['readings_written']-warm['readings_written'])/status_window_s,
            healthy_local_records_s=None if not path else (endpoint_local['local_committed']-warm_local['local_committed'])/status_window_s,
            healthy_local_readings_s=None if not path else (endpoint_local['local_readings']-warm_local['local_readings'])/status_window_s,
            cpu_one_core_percent=100*(cpu_end['cpu_ticks']-before['cpu_ticks'])/os.sysconf('SC_CLK_TCK')/wall,
            peak_rss_bytes=max(s['rss_bytes'] for s in healthy_resources),peak_hwm_bytes=max(s['hwm_bytes'] for s in healthy_resources),
            healthy_max_files={k:max(s['files'].get(k,0) for s in healthy_resources) for k in ('db','-wal','-shm')},
            alarm_age_ms=age,alarm_deadline_lateness_ms=age-500,outage=outage,
            pg_start_s=available-restored,recovery_from_restore_request_s=drained-restored,
            backlog_drain_from_pg_available_s=drained-available,
            first_progress_status_ms=first['now_ms'],
            first_progress_after_pg_available_s=max(0,first['now_ms']/1000-available),
            observed_replay_readings_s=600/(drained-available),final=final,
            local_final=None if inv is None else inv['meta'])
        (output/'trial.json').write_text(json.dumps(result,indent=2)+'\n')
        return {k:v for k,v in result.items() if k not in ('measured','outage','healthy','local','final','local_final','endpoint_persistence','endpoint_outbox')}
    finally:
        try:
            (output/'postgres.log').write_text((pg.root/'postgres.log').read_text())
        finally:
            pg.close()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,default=Path('build-resilience-release'))
    ap.add_argument('--runs',type=int,default=3)
    ap.add_argument('--seconds',type=int,default=60)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args()
    require((sys.platform=='linux' and not sys.flags.optimize))
    require((3<=args.runs<=10 and 60<=args.seconds<=600))
    def terminate(signum, _frame):
        raise SystemExit(128+signum)
    signal.signal(signal.SIGTERM,terminate)
    for k in list(os.environ):
        if k.startswith(('PG','VIGIL_DB_','VIGIL_TEST_')): os.environ.pop(k)
    build=args.build.resolve()
    cache=(build/'CMakeCache.txt').read_text()
    for required in ('CMAKE_BUILD_TYPE:STRING=Release','VIGIL_SANITIZERS:BOOL=OFF','VIGIL_TSAN:BOOL=OFF'):
        require((required in cache), lambda: ('matched benchmark requires unsanitized GCC Release: '+required))
    evidence=Evidence(args.output)
    report=evidence.report
    report.update(baseline_commit=BASELINE,run_count=args.runs,seconds=args.seconds,warmup_seconds=2,
                  compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],
                  cmake=subprocess.check_output(['cmake','--version'],text=True).splitlines()[0],
                  postgres=subprocess.check_output(['pg_config','--version'],text=True).strip(),
                  sqlite=subprocess.check_output(['/opt/vigil-sqlite-3.51.3/bin/sqlite3','--version'],text=True).strip(),
                  cpu=Path('/proc/cpuinfo').read_text(),memory=Path('/proc/meminfo').read_text(),
                  kernel=platform.platform(),implementation_sha256=source_digest())
    with tempfile.TemporaryDirectory(prefix='vigil-rc-benchmark-') as tmp:
        root=Path(tmp); baseline=root/'original'
        def run(cmd,name):
            with (evidence.directory/(name+'.log')).open('w') as log:
                subprocess.run(cmd,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=180)
        print('EXPECT: pinned original worktree; matched GCC Release, native ext4, PG durability on; all offered readings reconcile; finite measurements, no universal speed threshold.',flush=True)
        subprocess.run(['git','worktree','add','--detach',str(baseline),BASELINE],check=True)
        try:
            run(['cmake','-S',str(baseline),'-B',str(baseline/'build'),'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=gcc'],'baseline-configure')
            run(['cmake','--build',str(baseline/'build'),'--parallel','2'],'baseline-build')
            run(['ctest','--test-dir',str(baseline/'build'),'-V'],'baseline-units')
            def selected_cache(text):
                keys=('CMAKE_C_COMPILER:','CMAKE_C_FLAGS:','CMAKE_C_FLAGS_RELEASE:')
                return [line for line in text.splitlines() if line.startswith(keys)]
            report['matched_cmake_cache']=selected_cache(cache)
            require((report['matched_cmake_cache']==selected_cache((baseline/'build/CMakeCache.txt').read_text())))
            report['original_source_sha256']=subprocess.check_output([sys.executable,'-c',
                'import sys; sys.path.insert(0,sys.argv[1]); from evidence import source_digest; print(source_digest())',str(baseline/'tests')],cwd=baseline,text=True).strip()
            # Identical probe source/flags, each tree's actual unchanged core.
            for label,tree in (('original',baseline),('resilience',ROOT)):
                cmd=['gcc','-std=c11','-O3','-DNDEBUG','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Wpedantic','-Werror','-Wconversion','-Wsign-conversion','-Wshadow','-Wstrict-prototypes','-Wmissing-prototypes','-Wformat=2','-Wundef',
                     '-I'+str(tree/'include'),str(ROOT/'tests/benchmark_core.c'),str(tree/'src/monitor.c'),str(tree/'src/config.c'),str(tree/'src/protocol.c'),'-o',str(root/('core-'+label))]
                run(cmd,'core-build-'+label)
                report[label+'_core_command']=cmd
                report[label+'_binary_sha256']=hashlib.sha256(((baseline/'build' if label=='original' else build)/'vigild').read_bytes()).hexdigest()
            report['probe_source_sha256']=hashlib.sha256((ROOT/'tests/benchmark_core.c').read_bytes()).hexdigest()
            evidence.save()
            all_trials=[]
            for i in range(args.runs):
                # Alternate order to expose time/thermal/order effects, never run competing daemons.
                for label in (('original','resilience') if i%2==0 else ('resilience','original')):
                    name=f'{i+1}-{label}'
                    probe=json.loads(subprocess.check_output([str(root/('core-'+label))],text=True))
                    (evidence.directory/(name+'-core.json')).write_text(json.dumps(probe,indent=2)+'\n')
                    exe=(baseline/'build' if label=='original' else build)/'vigild'
                    case=root/name; case.mkdir()
                    evidence.run(name,'2s excluded warmup, 60s or requested >=60s at 100 readings/s plus 10 heartbeat/s, 500ms stale, 600-reading PG outage/recovery; no drops and exact SQL/local counts',
                                 lambda label=label,exe=exe,case=case,name=name:trial(label,exe,build/'vigil-outbox-init',args.seconds,case,evidence.directory/name))
                    metrics=report['scenarios'][-1]['measurements']
                    evidence.run(name+'-burst','finite 50001-frame no-persistence vcan burst; sent = received + kernel drops; no physical bus or sustained capacity claim',
                                 lambda exe=exe:load(str(exe)))
                    all_trials.append(dict(run=i+1,label=label,core=probe,metrics=metrics,burst=report['scenarios'][-1]['measurements']))
                for batch in (1,32):
                    store=root/f'local-{i}-{batch}'; store.mkdir(mode=0o700)
                    result=json.loads(subprocess.check_output([str(build/'benchmark-outbox'),str(store/'outbox.sqlite3'),str(batch)],text=True,timeout=60))
                    (evidence.directory/f'local-{i+1}-{batch}.json').write_text(json.dumps(result,indent=2)+'\n')
            summary={label:{key:distribution([t['metrics'][key] for t in all_trials if t['label']==label]) for key in
                     ('observed_monitor_readings_s','healthy_remote_readings_s','healthy_remote_records_s','sampled_remote_records_s','sampled_remote_readings_s','cpu_one_core_percent','peak_rss_bytes','alarm_deadline_lateness_ms','recovery_from_restore_request_s','backlog_drain_from_pg_available_s','observed_replay_readings_s')} for label in ('original','resilience')}
            for label in ('original','resilience'):
                for key in ('p50_ns','p95_ns','p99_ns'):
                    summary[label]['core_frame_'+key]=distribution([t['core']['frame'][key] for t in all_trials if t['label']==label])
                summary[label]['burst_kernel_drops']=distribution([t['burst']['kernel_drops'] for t in all_trials if t['label']==label])
                summary[label]['burst_offered_frames_s']=distribution([t['burst']['offered_frames_per_s'] for t in all_trials if t['label']==label])
                summary[label]['core_bulk_frames_s']=distribution([t['core']['bulk_frames_s'] for t in all_trials if t['label']==label])
            for key in ('healthy_local_records_s','healthy_local_readings_s'):
                summary['resilience'][key]=distribution([t['metrics'][key] for t in all_trials if t['label']=='resilience'])
            local={}
            for batch in (1,32):
                trials=[json.loads((evidence.directory/f'local-{i+1}-{batch}.json').read_text()) for i in range(args.runs)]
                local[batch]={key.removesuffix('_ns')+'_ms':distribution([s[key]/1e6 for t in trials for s in t['samples']]) for key in ('append_ns','queue_to_commit_ns','transaction_ns')}
                local[batch]['checkpoint_ms']=distribution([t['checkpoint_ns']/1e6 for t in trials])
                local[batch]['records_per_append_second']=distribution([200*batch/(sum(s['append_ns'] for s in t['samples'])/1e9) for t in trials])
                local[batch]['max_db_bytes']=max(s['db_bytes'] for t in trials for s in t['samples'])
                local[batch]['max_wal_bytes']=max(s['wal_bytes'] for t in trials for s in t['samples'])
            (evidence.directory/'summary.json').write_text(json.dumps(dict(comparison=summary,local_api=local,trials=all_trials),indent=2)+'\n')
        finally:
            # Only the newly allocated detached worktree; no user branches changed.
            subprocess.run(['git','worktree','remove','--force',str(baseline)],check=True)
    evidence.finish()


if __name__=='__main__': main()
