"use strict";
(() => {
  const video = document.getElementById("monitor");
  const status = document.getElementById("status");
  const fpsDisplay = document.getElementById("playback-fps");
  const supportsFrameCallback = typeof video.requestVideoFrameCallback === "function" &&
    typeof video.cancelVideoFrameCallback === "function";
  let fpsCallback = null, fpsTimer = null, fpsBaseline = null, lastPresented = null;
  let measuringFps = false;
  let reader = null;
  let lastTime = -1;
  let lastProgress = performance.now();

  function resetFps() {
    fpsBaseline = null;
    lastPresented = null;
    fpsDisplay.textContent = "播放帧率 —";
  }
  function stopFps() {
    measuringFps = false;
    if (fpsCallback !== null) video.cancelVideoFrameCallback(fpsCallback);
    fpsCallback = null;
    clearInterval(fpsTimer);
    fpsTimer = null;
    resetFps();
  }
  function onVideoFrame(now, metadata) {
    fpsCallback = null;
    if (!measuringFps) return;
    if (document.hidden || video.paused || video.ended ||
        !Number.isFinite(metadata.presentedFrames) || !Number.isFinite(metadata.presentationTime)) {
      resetFps();
    } else {
      if (lastPresented !== null && now - lastPresented > 1500) resetFps();
      lastPresented = now;
      const frames = metadata.presentedFrames;
      const time = metadata.presentationTime;
      // 使用浏览器提交给合成器的帧数增量，避免主线程漏回调导致直接计数偏低。
      if (!fpsBaseline || frames < fpsBaseline.frames || time <= fpsBaseline.time) {
        fpsDisplay.textContent = "播放帧率 —";
        fpsBaseline = {frames, time};
      } else if (time - fpsBaseline.time >= 1000) {
        const fps = (frames - fpsBaseline.frames) * 1000 / (time - fpsBaseline.time);
        fpsDisplay.textContent = `播放帧率 ${fps.toFixed(1)} FPS`;
        fpsBaseline = {frames, time};
      }
    }
    fpsCallback = video.requestVideoFrameCallback(onVideoFrame);
  }
  function startFps() {
    stopFps();
    if (!supportsFrameCallback) {
      fpsDisplay.title = "当前浏览器不支持播放帧率统计";
      return;
    }
    measuringFps = true;
    fpsCallback = video.requestVideoFrameCallback(onVideoFrame);
    fpsTimer = setInterval(() => {
      if (document.hidden || video.paused || video.ended ||
          (lastPresented !== null && performance.now() - lastPresented > 1500)) resetFps();
    }, 250);
  }
  document.addEventListener("visibilitychange", resetFps);
  ["waiting", "stalled", "pause", "ended", "emptied"].forEach((event) => video.addEventListener(event, resetFps));

  function showStatus(message) {
    status.textContent = message;
    status.hidden = false;
  }
  function connect() {
    stopFps();
    if (reader) reader.close();
    video.srcObject = null;
    lastTime = -1;
    lastProgress = performance.now();
    showStatus("正在连接摄像头…");
    // 信令走同源 Nginx 代理，媒体通过 WebRTC 直连板卡。
    reader = new MediaMTXWebRTCReader({
      url: new URL("rtc/camera/whep", window.location.href).href,
      onError: () => { stopFps(); showStatus("连接中断，正在重连…"); },
      onTrack: (event) => {
        video.srcObject = event.streams[0] || new MediaStream([event.track]);
        startFps();
        video.play().catch((error) => {
          if (error.name === "NotAllowedError") showStatus("点击画面开始播放");
        });
      },
    });
  }
  video.addEventListener("click", () => video.play().catch(() => {}));
  // ICE 仍连接但媒体已停止时，重新创建会话；常规网络错误由 reader 自动重试。
  const watchdog = setInterval(() => {
    if (!reader) return;
    if (video.currentTime !== lastTime && video.readyState >= 2) {
      lastTime = video.currentTime;
      lastProgress = performance.now();
      status.hidden = true;
    } else if (performance.now() - lastProgress > 5000) {
      connect();
    }
  }, 1000);
  window.addEventListener("pagehide", () => {
    stopFps();
    if (reader) reader.close();
    reader = null;
    video.srcObject = null;
  });
  window.addEventListener("pageshow", () => {
    if (!("RTCPeerConnection" in window)) {
      clearInterval(watchdog);
      showStatus("当前浏览器不支持 WebRTC，请使用新版 Chrome、Edge 或 Firefox");
      return;
    }
    if (!reader) connect();
  });
})();
