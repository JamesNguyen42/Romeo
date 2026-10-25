const STEPS = [
  {short:'What went wrong',tech:'case_opened'},
  {short:'What it saw',tech:'action_result'},
  {short:'Picked tools',tech:'decision → action → fact'},
  {short:'Ran a test',tech:'action_result'},
  {short:'Restart',tech:'restarted'},
  {short:'What it found',tech:'comparison_complete / conclusion'},
  {short:'Still untested',tech:'stopped'}
];
const POS = [[190,110],[390,110],[590,110],[790,110],[790,250],[990,110],[990,250]];
const INPUTS = ['Report','Measurements','Saved evidence'];
const fmt = (value,unit='') => typeof value==='number' && Number.isFinite(value) ? value.toLocaleString(undefined,{maximumFractionDigits:1})+(unit?' '+unit:'') : '—'+(unit?' '+unit:'');
const lastEvent = (events,type) => [...events].reverse().find(e=>e.type===type);
const actualStatus = c => !c ? 'Not started' : c.status==='done' ? 'Run finished' : c.status==='stopped' ? 'Needs evidence' : 'Running';

class Component extends DCLogic {
  state = {i:0,t:0,playing:false,toolRound:null,toolsPlayback:false,scenarioId:'',side:'current',live:{connected:false,ready:false,error:'',scenarios:[],cases:[],selectedId:'',selected:null,runs:[],loading:false,starting:false}};
  componentDidMount() {
    this.disposed=false;
    this.store=new LiveStore(live=>{
      if(this.disposed)return;
      const change=live.selected?.scenario_id && live.selected.id!==this.state.live.selected?.id ? {scenarioId:live.selected.scenario_id,toolRound:null,toolsPlayback:false,t:0} : {};
      this.setState({live,...change});
      this.chartKey='';
    });
    this.store.start();
    this.onKey=e=>{
      if(e.target && /INPUT|SELECT|TEXTAREA|BUTTON/.test(e.target.tagName))return;
      if(e.key==='ArrowRight')this.go(this.state.i+1);
      if(e.key==='ArrowLeft')this.go(this.state.i-1);
      if(e.key===' '){e.preventDefault();this.togglePlay();}
    };
    this.onResize=()=>{this.chartKey='';};
    window.addEventListener('keydown',this.onKey);
    window.addEventListener('resize',this.onResize);
    let previous=performance.now();
    this.tick=setInterval(()=>{
      const now=performance.now(),dt=Math.min(0.1,(now-previous)/1000);previous=now;
      if(this.state.playing)this.setState(s=>{
        const t=s.t+dt,dur=this.duration(s.i);
        if(t>dur)return s.i<6?{i:s.i+1,t:0,toolRound:null,toolsPlayback:s.i+1===2}:{playing:false,t:dur};
        return {t};
      });
      if(this.state.i===1)this.drawCharts();
    },33);
  }
  componentWillUnmount(){this.disposed=true;this.store?.stop();clearInterval(this.tick);window.removeEventListener('keydown',this.onKey);window.removeEventListener('resize',this.onResize);}
  duration(i){return i===2?Math.max(1,(this.state.live.selected?.events||[]).filter(e=>e.type==='decision').length)*4:(this.props.seconds??7);}
  go(i){this.chartKey='';const next=Math.max(0,Math.min(6,i));this.setState({i:next,t:0,toolRound:null,toolsPlayback:this.state.playing&&next===2});}
  togglePlay(){this.setState(s=>s.playing?{playing:false}:{playing:true,...(s.i===6?{i:0,t:0,toolsPlayback:false}:s.i===2?{toolsPlayback:true,toolRound:null,t:s.toolsPlayback?s.t:(s.toolRound??0)*4}:{})});}
  goToolRound(delta){const story=this.toolStory();this.setState({playing:false,toolsPlayback:false,toolRound:Math.max(0,Math.min(story.roundCount-1,story.roundIndex+delta)),t:0});}
  toolStory(){const s=this.state;return buildToolStory({selected:s.live.selected,roundIndex:s.toolsPlayback?Math.floor(s.t/4):s.toolRound,progress:s.toolsPlayback?(s.t%4)/4:1});}
  chooseScenario(e){this.store.select('');this.setState({scenarioId:e.target.value,i:0,t:0,playing:false,toolRound:null,toolsPlayback:false});}
  chooseRun(e){this.setState({i:0,t:0,playing:false,toolRound:null,toolsPlayback:false});this.store.select(e.target.value);}
  async run(mode='comparison',fresh=false){
    const scenarioId=this.state.scenarioId || this.state.live.selected?.scenario_id;
    if(!scenarioId||this.state.live.starting)return;
    const seed=fresh?(crypto.getRandomValues(new Uint32Array(1))[0]||1):0;
    this.setState({playing:false,t:0,i:3,side:'current'});
    try{await this.store.run(scenarioId,mode,seed);}catch{/* The store exposes the server error. */}
  }
  relevantRuns(){
    const runs=this.state.live.runs.filter(r=>r.image||r.summary||r.files.some(p=>p.endsWith('/frames.csv')||p.endsWith('/memory.csv')));
    const current=[...runs].reverse().find(r=>!r.baseline);
    const baseline=[...runs].reverse().find(r=>r.baseline && (!current || !r.event.data.setup || r.event.data.setup===current.event.data.setup));
    return {current,baseline};
  }
  drawCharts(){
    const {current,baseline}=this.relevantRuns();
    for(const memory of [false,true]){
      const canvas=document.getElementById(memory?'story-memory':'story-timing');
      if(!canvas)continue;
      const w=canvas.clientWidth,h=canvas.clientHeight;
      const key=[memory,w,h,current?.event.seq,baseline?.event.seq,this.state.live.selectedId].join(':');
      if(canvas.dataset.drawn===key && this.chartKey===this.state.live.selectedId)continue;
      const ctx=canvas.getContext('2d');if(!ctx)continue;
      const ratio=window.devicePixelRatio||1;canvas.width=w*ratio;canvas.height=h*ratio;ctx.scale(ratio,ratio);ctx.clearRect(0,0,w,h);
      const series=[current,baseline].filter(Boolean).map(run=>({baseline:run.baseline,points:(memory?run.memory||[]:run.frames||[]).filter(p=>memory||p.frame>=(run.summary?.warmup_frames??Math.min(40,Math.max(0,(run.summary?.frames||41)-1)))).map(p=>({x:memory?p.t_ms/1000:p.frame,y:memory?p.resident_bytes/1048576:p.ms})).filter(p=>Number.isFinite(p.x)&&Number.isFinite(p.y))}));
      const all=series.flatMap(s=>s.points);ctx.font='12px Archivo, sans-serif';ctx.fillStyle=memory?'#605d5d':'#d7d3d3';
      if(!all.length){ctx.textAlign='center';ctx.fillText('No measurements yet',w/2,h/2);}
      else{
        const xmin=Math.min(...all.map(p=>p.x)),xmax=Math.max(...all.map(p=>p.x)),ymax=Math.max(1,...all.map(p=>p.y))*1.15;
        const x=v=>37+(v-xmin)/(xmax-xmin||1)*(w-51),y=v=>h-30-v/ymax*(h-55);
        ctx.textAlign='left';ctx.lineWidth=1;ctx.strokeStyle=memory?'#d7d3d3':'#444141';
        for(let j=0;j<4;j++){const value=j*ymax/3;ctx.beginPath();ctx.moveTo(37,y(value));ctx.lineTo(w-14,y(value));ctx.stroke();ctx.fillText(Math.round(value),4,y(value)+4);}
        ctx.fillText(memory?'seconds':'frame',37,h-8);ctx.textAlign='right';ctx.fillText(fmt(xmax),w-14,h-8);
        for(const s of series){ctx.strokeStyle=s.baseline?(memory?'#201e1d':'#d7d3d3'):'#ec3013';ctx.lineWidth=2;ctx.beginPath();s.points.forEach((p,k)=>k?ctx.lineTo(x(p.x),y(p.y)):ctx.moveTo(x(p.x),y(p.y)));ctx.stroke();}
        ctx.textAlign='left';ctx.fillText(memory?'MiB':'ms',8,16);
      }
      canvas.dataset.drawn=key;
    }
    this.chartKey=this.state.live.selectedId;
  }
  renderVals(){
    const {i,t,playing,live,side}=this.state,dur=this.duration(i);
    const selected=live.selected,events=selected?.events||[];
    const {current,baseline}=this.relevantRuns();
    const scenarioId=this.state.scenarioId||selected?.scenario_id||'';
    const scenario=live.scenarios.find(s=>s.id===scenarioId);
    const opened=lastEvent(events,'case_opened'),decision=lastEvent(events,'decision'),restart=lastEvent(events,'restarted'),conclusion=lastEvent(events,'conclusion'),stopped=lastEvent(events,'stopped');
    const valid=r=>Number.isFinite(r?.summary?.p50_ms)&&r?.event.data.exit_code===0;
    const measured=valid(current)&&valid(baseline);
    const comparison=selected?.mode==='comparison'||opened?.data.mode==='comparison';
    const imageSrc=current?.image;
    const imageSource=current?.event.data.source==='fixture'?'Fixture':current?.image?'Real render':'No image yet';
    const report=opened?.data.report||selected?.report||scenario?.report;
    const captions=[
      {line:report||'Choose a run to see its story.',why:report?'The report gives Juliet something concrete to check.':'Open Live controls below. Nothing runs until you press Run test.'},
      {line:current?.summary?'The picture and the measurements came from the renderer.':'No measured result has arrived yet.',why:'Images, timing and memory describe the same run.'},
      {line:decision?'Clef picks one tool. Juliet runs it and saves what it finds.':comparison?'This run followed a direct comparison.':'The next tool depends on the evidence so far.',why:decision?'Each saved result gives the next choice more evidence. The scores show tool choice, not certainty about a cause.':comparison?'The renderer checks the affected version and its baseline. No model choice is needed.':'Clef scores the options the service offers. A score appears only after a real decision.'},
      {line:(side==='baseline'?baseline:current)?.preview?'These are the saved frames from the run.':'The replay images have not arrived yet.',why:'The renderer re-creates sampled frames after timing. Each badge shows the recorded frame time.'},
      {line:restart?'Juliet recovered the saved case after a restart.':'No restart has been recorded for this case.',why:restart?'The saved record keeps completed work and any pending actions.':'This scene only changes when a restart event arrives.'},
      {line:stopped?stopped.data.reason||'More evidence is needed.':conclusion?conclusion.data.diagnosis:measured?'The affected render and its baseline are ready to compare.':'The comparison is not ready yet.',why:stopped?stopped.data.missing||'Check the saved evidence.':conclusion?conclusion.data.fix||'Check the evidence before accepting a fix.':measured?'Both sides use the same setup and camera path. A comparison alone does not prove a fix.':'No result is assumed before the renderer finishes.'},
      {line:'One run does not cover every scene.',why:current?.preview?'These pictures are sampled views from this run. Other scenes and setups still need checks.':'The test pictures stay empty until there is real renderer output.'}
    ];
    const cur={...STEPS[i],...captions[i]};for(let n=1;n<=7;n++)cur['is'+n]=i===n-1;
    const P=(x,y,w,h)=>({l:x/11.5+'%',t:y/3.6+'%',w:w/11.5+'%',h:h/3.6+'%'});
    const steps=STEPS.map((s,k)=>{const active=k===i,past=k<i,[x,y]=POS[k];return{...s,...(active?P(x-9,y-4,168,80):P(x,y,150,72)),num:String(k+1).padStart(2,'0'),bg:active?'var(--color-accent)':past?'var(--color-text)':'var(--color-bg)',fg:active||past?'#fff':'var(--color-text)',border:active?'var(--color-accent)':'var(--color-text)',progressPct:(active?Math.min(100,t/dur*100):0)+'%',go:()=>this.go(k)};});
    const inputs=INPUTS.map((label,k)=>({...P(190+k*200,10,150,54),label,stroke:k===i?'var(--color-accent)':'var(--color-text)'}));
    const toolStory=this.toolStory();
    const replay=side==='baseline'?baseline:current,rawFrames=Array.isArray(replay?.preview?.frames)?replay.preview.frames:[];
    const imageFor=f=>replay?.event.data.action_id&&f?.file?liveArtifactUrl(selected.id,replay.event.data.action_id+'/'+f.file,this.store?.base||''):undefined;
    const active=rawFrames.length?Math.floor(t/0.16)%rawFrames.length:-1;
    const frames=Array.from({length:40},(_,k)=>{const f=rawFrames[k],src=imageFor(f);return{number:f?f.frame:k+1,src,hasImage:!!src,complete:!!f,label:f?fmt(f.benchmark_frame_ms,'ms'):'',border:k===active?'var(--color-accent)':'var(--color-text)',imageOpacity:1};});
    const scale=Math.max(1,current?.summary?.p50_ms||0,baseline?.summary?.p50_ms||0);
    const compare=[{run:baseline,id:'render-baseline',label:'Baseline',color:'var(--color-text)'},{run:current,id:'render-current',label:'With the issue',color:'var(--color-accent)'}].map(c=>({...c,src:c.run?.image,hasImage:!!c.run?.image,noImage:!c.run?.image,alt:c.label+' renderer output',label:c.label+(c.run?.event.data.source==='fixture'?' · Fixture':''),pct:fmt(c.run?.summary?.p50_ms,'ms'),w:((c.run?.summary?.p50_ms||0)/scale*100)+'%',tc:c.color}));
    const samples=[0,Math.floor(rawFrames.length/2),Math.max(0,rawFrames.length-1)];
    const unknowns=samples.map((n,k)=>{const f=rawFrames[n],src=imageFor(f);return{id:'other-view-'+k,src,hasImage:!!src,noImage:!src,alt:f?'Measured renderer frame '+f.frame:'No test image',label:f?'Frame '+f.frame+' · '+fmt(f.benchmark_frame_ms,'ms'):'View '+(k+1)+' · not tested'};});
    const pending=new Set();for(const e of events){if(e.type==='action_requested')pending.add(e.data.action_id);if(e.type==='action_result')pending.delete(e.data.action_id);}
    const busy=!!selected&&!['done','stopped'].includes(selected.status);
    return{
      steps,inputs,cur,showTech:!!this.props.showTech,atStart:i===0,atEnd:i===6,prev:()=>this.go(i-1),next:()=>this.go(i+1),togglePlay:()=>this.togglePlay(),playLabel:playing?'Pause':'Play',
      caseLabel:selected?'Juliet · '+(selected.scenario_id||selected.id)+' · Renderer':'Juliet · No run selected',caseTitle:selected?.title||scenario?.title||'Renderer case story',
      imageSrc,hasImage:!!imageSrc,noImage:!imageSrc,imageSource,pictureStatus:live.loading?'Loading saved evidence…':selected?'Waiting for renderer output':'Choose a run to see its picture',runStatus:current?.event.data.source==='fixture'?'Fixture evidence':actualStatus(selected),
      typical:fmt(current?.summary?.p50_ms,'ms'),slowest:fmt(current?.summary?.max_ms,'ms'),memory:fmt(typeof current?.summary?.resident_end_bytes==='number'?current.summary.resident_end_bytes/1048576:undefined,'MiB'),blink:playing?0.45+0.55*Math.abs(Math.sin(t*3)):1,
      ...toolStory,toolGroups:toolStory.groups,hasToolRounds:toolStory.roundCount>0,previousToolRound:()=>this.goToolRound(-1),nextToolRound:()=>this.goToolRound(1),atFirstToolRound:toolStory.roundIndex<=0,atLastToolRound:toolStory.roundIndex>=toolStory.roundCount-1,
      frames,framesDone:rawFrames.length,framesTotal:40,showCurrent:()=>this.setState({side:'current',t:0}),showBaseline:()=>this.setState({side:'baseline',t:0}),replaySideLabel:side==='baseline'?'Baseline frames':'Affected frames',
      screenText:restart?'Recovered saved case':'No restart recorded',screenBg:restart?'var(--color-text)':'var(--color-bg)',screenFg:restart?'var(--color-bg)':'var(--color-text)',savedLabel:events.length+' events saved',recordStatus:selected?'Saved case record':'No case selected',pendingLabel:pending.size+' actions waiting',
      compare,findingA:current?.summary?fmt(current.summary.p50_ms,'ms')+' affected':'Affected: —',findingB:baseline?.summary?fmt(baseline.summary.p50_ms,'ms')+' baseline':'Baseline: —',findingResult:measured?'Compare the evidence':'Not measured',unknowns,
      connectionLabel:live.connected?'Connected to Juliet':'Waiting for Juliet',evidenceLabel:selected?selected.id+(live.runs.some(r=>r.event.data.source==='fixture')?' · Contains fixture evidence':''):'No test has been started',hasError:!!(live.error||live.runs.find(r=>r.error)?.error),error:live.error||live.runs.find(r=>r.error)?.error||'',
      hasCase:!!selected,exportUrl:selected?'/api/cases/'+encodeURIComponent(selected.id)+'/export':undefined,scenarioId,scenarios:live.scenarios,cases:live.cases.map(c=>({...c,label:c.title+' · '+c.id})),selectedId:live.selectedId,
      selectScenario:e=>this.chooseScenario(e),selectRun:e=>this.chooseRun(e),runTest:()=>this.run(),newAngles:()=>this.run('comparison',true),investigate:()=>this.run('investigation'),refresh:()=>this.store.refresh(),cannotRun:!live.connected||!scenarioId||live.starting||busy,runButton:live.starting?'Starting…':busy?'Running…':'Run test'
    };
  }
}
