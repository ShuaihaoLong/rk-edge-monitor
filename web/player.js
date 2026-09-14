"use strict";
(() => {
  const video = document.getElementById("monitor");
  const status = document.getElementById("status");
  let reader = null;
  let lastTime = -1;
  let lastProgress = performance.now();

  function showStatus(message) {
    status.textContent = message;
    status.hidden = false;
  }
  function connect() {
    if (reader) reader.close();
    video.srcObject = null;
    lastTime = -1;
    lastProgress = performance.now();
    showStatus("正在连接摄像头…");
    // 信令走同源 Nginx 代理，媒体通过 WebRTC 直连板卡。
    reader = new MediaMTXWebRTCReader({
      url: new URL("rtc/camera/whep", window.location.href).href,
      onError: () => showStatus("连接中断，正在重连…"),
      onTrack: (event) => {
        video.srcObject = event.streams[0] || new MediaStream([event.track]);
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
