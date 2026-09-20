(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const video = $('recording-player'), date = $('recording-date'), seek = $('recording-seek');
  const format = new Intl.DateTimeFormat('zh-CN', {timeZone:'Asia/Shanghai', year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit',hourCycle:'h23'});
  const clock = value => format.format(new Date(value));
  const today = new Intl.DateTimeFormat('en-CA',{timeZone:'Asia/Shanghai',year:'numeric',month:'2-digit',day:'2-digit'}).format(new Date());
  let data = null, windowInfo = null, playbackToken = 0, loadingToken = 0, dragging = false;
  date.value = today;
  async function api(path) {
    const response = await fetch(path,{cache:'no-store',signal:AbortSignal.timeout(15000)});
    const value = await response.json();
    if(!response.ok)throw new Error(value.error || '请求失败');
    return value;
  }
  function message(text) { $('recording-message').textContent = text; }
  function draw() {
    const canvas = $('recording-timeline'), scale = window.devicePixelRatio || 1;
    canvas.width = Math.round(canvas.clientWidth*scale);canvas.height = 44*scale;
    const ctx=canvas.getContext('2d');ctx.scale(scale,scale);
    if(!data)return;
    const width=canvas.clientWidth, x=time=>(time-data.start_ms)/(data.end_ms-data.start_ms)*width;
    ctx.fillStyle='#60c58b';
    data.recordings.forEach(r=>ctx.fillRect(Math.max(0,x(r.start_ms)),12,Math.max(2,x(Math.min(r.end_ms,data.end_ms))-Math.max(0,x(r.start_ms))),24));
    ctx.fillStyle='#ffba6a';data.events.forEach(e=>ctx.fillRect(Math.max(0,x(e.start_ms)),3,Math.max(2,x(e.end_ms)-x(e.start_ms)),6));
  }
  async function load() {
    const token=++loadingToken;
    try {
      const [result,days,status]=await Promise.all([api(`/api/recordings?date=${encodeURIComponent(date.value)}`),api('/api/recordings/days'),api('/api/recordings/status')]);
      if(token!==loadingToken)return;
      data=result;draw();seek.max=Math.floor((data.end_ms-data.start_ms)/1000)-1;
      $('storage-state').textContent=`剩余 ${(status.free_bytes/1024**3).toFixed(1)} GiB${status.status==='ok'?'':' · '+status.detail}`;
      const dayList=$('recording-days');dayList.replaceChildren(new Option('选择日期',''));
      days.forEach(d=>dayList.add(new Option(d,d)));dayList.value=date.value;
      $('recording-count').textContent=`${data.recordings.length} 个可播放片段`;
      const list=$('recording-list');list.replaceChildren();
      [...data.recordings].reverse().forEach(r=>{
        const item=document.createElement('li'),button=document.createElement('button'),download=document.createElement('a');
        button.textContent=`${clock(r.start_ms)} · ${Math.round(r.duration)} 秒`;
        button.onclick=()=>play(r.start_ms);
        download.textContent='下载';download.href=`/api/recordings/${r.id}/download`;
        item.append(button,download);list.append(item);
      });
      if(!data.recordings.length)list.textContent='当天暂无已完成录像。';
      const events=$('event-list');events.replaceChildren();
      data.events.forEach(e=>{
        const item=document.createElement('li'),button=document.createElement('button');
        button.textContent=`${clock(e.start_ms)} · ${e.label} · ${Math.round(e.confidence*100)}%`;
        button.onclick=()=>{
          const before=e.start_ms-3000;
          const r=data.recordings.find(r=>r.start_ms<=e.start_ms && r.end_ms>e.start_ms);
          if(r)play(Math.max(r.start_ms,before));else message('该事件对应的录像尚未完成或已清理。');
        };
        item.append(button);events.append(item);
      });
      if(!data.events.length)events.textContent='当天暂无检测事件。';
    } catch(error) {message(`读取录像失败：${error.message}`);}
  }
  async function play(stamp) {
    const token=++playbackToken;
    if(!Number.isFinite(stamp))return;
    message('正在加载录像…');video.pause();
    try {
      const selected=await api(`/api/playback?start=${Math.round(stamp)}&duration=60`);
      if(token!==playbackToken)return;
      windowInfo=selected;video.src=selected.media_url;video.playbackRate=Number($('recording-speed').value);
      $('selected-time').textContent=clock(selected.start_ms);
      message('正在回放');
      try {await video.play();}catch(error){if(error.name!=='AbortError')message('录像已加载，点击播放按钮开始。');}
    } catch(error) {if(token===playbackToken)message(error.message);}
  }
  function select(stamp){$('selected-time').textContent=clock(stamp);play(stamp);}
  $('recording-timeline').onclick=event=>{
    if(!data)return;const bounds=event.currentTarget.getBoundingClientRect();
    select(data.start_ms+Math.min(.999999,Math.max(0,(event.clientX-bounds.left)/bounds.width))*(data.end_ms-data.start_ms));
  };
  let pointerSeekCommitted=false;
  const previewSeek=()=>{if(data)$('selected-time').textContent=clock(data.start_ms+Number(seek.value)*1000);};
  const commitSeek=()=>{if(data)select(data.start_ms+Number(seek.value)*1000);};
  seek.onpointerdown=()=>{dragging=true;pointerSeekCommitted=false;};
  seek.oninput=previewSeek;
  seek.onpointerup=()=>{
    if(!dragging)return;
    dragging=false;pointerSeekCommitted=true;commitSeek();
  };
  seek.onpointercancel=()=>{dragging=false;};
  seek.onchange=()=>{
    if(pointerSeekCommitted){pointerSeekCommitted=false;return;}
    dragging=false;commitSeek();
  };
  video.ontimeupdate=()=>{
    if(!windowInfo)return;const stamp=windowInfo.start_ms+video.currentTime*1000;
    $('recording-clock').textContent=clock(stamp);
    if(data && !dragging)seek.value=Math.max(0,(stamp-data.start_ms)/1000);
  };
  video.onended=()=>{
    if(!windowInfo || !data)return;
    const next=windowInfo.end_ms;
    const row=data.recordings.find(r=>r.end_ms>next+50);
    if(row)play(Math.max(next,row.start_ms));else message('已播放到当前已完成录像末尾，可刷新获取新片段。');
  };
  video.onerror=()=>message('录像加载失败，可能已被清理；请刷新列表后重试。');
  $('recording-speed').onchange=()=>{video.playbackRate=Number($('recording-speed').value);};
  $('skip-back').onclick=()=>{
    if(windowInfo) video.currentTime=Math.max(0,video.currentTime-10);
  };
  $('skip-forward').onclick=()=>{
    if(windowInfo && Number.isFinite(video.duration))
      video.currentTime=Math.min(video.duration,video.currentTime+10);
  };
  date.onchange=()=>{++playbackToken;video.pause();video.removeAttribute('src');video.load();windowInfo=null;load();};
  $('recording-days').onchange=event=>{if(event.target.value){date.value=event.target.value;date.onchange();}};
  $('recording-refresh').onclick=load;
  new ResizeObserver(draw).observe($('recording-timeline'));
  load();setInterval(()=>{if(!document.hidden)load();},15000);
})();
