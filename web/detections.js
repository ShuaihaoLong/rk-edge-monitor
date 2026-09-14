"use strict";
(() => {
  const video = document.getElementById("monitor");
  const canvas = document.getElementById("boxes");
  const context = canvas.getContext("2d");
  const toggle = document.getElementById("show-boxes");
  const state = document.getElementById("ai-state");
  const log = document.getElementById("detection-log");
  const empty = document.getElementById("log-empty");
  const names = {person: "人", car: "汽车", bus: "公交车", truck: "卡车", bicycle: "自行车", motorcycle: "摩托车", cat: "猫", dog: "狗", chair: "椅子", bottle: "瓶子", "cell phone": "手机", laptop: "笔记本电脑", tv: "显示器", book: "书"};
  const label = (object) => names[object.label] || object.label;
  let result = null, receivedAt = 0, lastKey = "", lastChanged = performance.now();
  let lastLog = 0, lastSummary = "", lastState = "", active = true;
  let request = null, timer = null, animation = null;
  let videoTime = -1, videoProgress = performance.now();
  try { toggle.checked = localStorage.getItem("rkmon-show-boxes") !== "false"; } catch (_) {}
  toggle.addEventListener("change", () => {
    try { localStorage.setItem("rkmon-show-boxes", String(toggle.checked)); } catch (_) {}
    if (!toggle.checked) context.clearRect(0, 0, canvas.width, canvas.height);
  });
  function append(message) {
    empty.hidden = true;
    const item = document.createElement("li");
    const time = document.createElement("time");
    time.textContent = new Date().toLocaleTimeString("zh-CN", {hour12: false});
    item.append(time, document.createTextNode(message));
    log.prepend(item);
    while (log.children.length > 80) log.lastChild.remove();
  }
  function setState(value) {
    state.dataset.state = value;
    const text = {ok: "识别中", starting: "模型加载中", waiting: "等待视频", unavailable: "识别暂不可用", stopped: "识别已停止", offline: "结果连接中断", stale: "等待新结果"}[value] || "等待识别";
    state.textContent = text;
    if (value !== lastState && value !== "ok") append(text);
    lastState = value;
  }
  async function poll() {
    if (!active) return;
    request = new AbortController();
    const timeout = setTimeout(() => request?.abort(), 1500);
    try {
      const response = await fetch("/api/detections", {cache: "no-store", signal: request.signal});
      if (!response.ok) throw new Error("snapshot unavailable");
      const data = await response.json();
      if (!Array.isArray(data.objects) || typeof data.session !== "string") throw new Error("invalid snapshot");
      const now = performance.now();
      const key = `${data.session}:${data.revision}`;
      if (key !== lastKey) { lastChanged = now; lastKey = key; }
      // 文件存活时间使用服务器自己的 Date/Last-Modified，不要求 PC 与板卡时钟同步。
      const fileAge = Date.parse(response.headers.get("Date")) - Date.parse(response.headers.get("Last-Modified"));
      if (fileAge > 2000 || now - lastChanged > 1500 || data.age_ms > 1000) {
        result = null; setState("stale"); return;
      }
      setState(data.status);
      if (data.status !== "ok") { result = null; return; }
      result = data; receivedAt = now;
      const counts = new Map();
      data.objects.forEach((object) => counts.set(label(object), (counts.get(label(object)) || 0) + 1));
      const summary = counts.size ? [...counts].map(([name, count]) => `${name} × ${count}`).join("，") : "未检测到目标";
      if (summary !== lastSummary || now - lastLog > (counts.size ? 1500 : 5000)) {
        append(`${summary} · ${Math.round(data.inference_ms)} ms`);
        lastSummary = summary; lastLog = now;
      }
    } catch (_) {
      if (active) { result = null; setState("offline"); }
    } finally {
      clearTimeout(timeout); request = null;
      if (active) timer = setTimeout(poll, 250);
    }
  }
  function draw() {
    if (!active) return;
    const rect = canvas.getBoundingClientRect(), ratio = window.devicePixelRatio || 1;
    const width = Math.round(rect.width * ratio), height = Math.round(rect.height * ratio);
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, rect.width, rect.height);
    const now = performance.now();
    if (video.currentTime !== videoTime) { videoTime = video.currentTime; videoProgress = now; }
    // 最新结果叠加不是逐帧同步；视频冻结、元数据过期或断线后及时清框。
    if (toggle.checked && result && result.width > 0 && result.height > 0 && video.readyState >= 2 &&
        now - receivedAt + result.age_ms < 1000 && now - videoProgress < 1000 &&
        Math.abs(video.videoWidth / video.videoHeight - result.width / result.height) < 0.01) {
      const scale = Math.min(rect.width / result.width, rect.height / result.height);
      const offsetX = (rect.width - result.width * scale) / 2, offsetY = (rect.height - result.height * scale) / 2;
      context.lineWidth = 2; context.font = "13px system-ui";
      result.objects.forEach((object) => {
        const [left, top, right, bottom] = object.box;
        const x = offsetX + left * scale, y = offsetY + top * scale;
        const text = `${label(object)} ${Math.round(object.confidence * 100)}%`;
        context.strokeStyle = "#75f3a8";
        context.strokeRect(x, y, (right - left) * scale, (bottom - top) * scale);
        context.fillStyle = "#101b20e8";
        const textY = Math.max(offsetY, y - 21);
        context.fillRect(x, textY, context.measureText(text).width + 10, 21);
        context.fillStyle = "#a5ffcb"; context.fillText(text, x + 5, textY + 15);
      });
    }
    animation = requestAnimationFrame(draw);
  }
  window.addEventListener("pagehide", () => {
    active = false; request?.abort(); clearTimeout(timer); cancelAnimationFrame(animation); result = null;
  });
  window.addEventListener("pageshow", () => {
    if (!active) { active = true; poll(); draw(); }
  });
  poll(); draw();
})();
