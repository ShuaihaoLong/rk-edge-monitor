"use strict";
(() => {
  const state = document.getElementById("mqtt-state");
  const list = document.getElementById("device-list");
  const empty = document.getElementById("device-empty");
  const devices = new Map();
  let client = null;

  function setConnection(connected) {
    state.textContent = connected ? "已连接" : "正在重连";
    state.dataset.state = connected ? "ok" : "offline";
  }

  function validStatus(value) {
    return value && value.schema === 1 && typeof value.device_id === "string" &&
      /^[A-Za-z0-9_-]+$/.test(value.device_id) && typeof value.online === "boolean";
  }

  function valueText(value, suffix = "") {
    return typeof value === "number" && Number.isFinite(value) ? `${value.toFixed(1)}${suffix}` : "—";
  }

  function addField(details, name, value, className = "") {
    const term = document.createElement("dt");
    const description = document.createElement("dd");
    term.textContent = name;
    description.textContent = value;
    if (className) description.className = className;
    details.append(term, description);
  }

  function render() {
    const now = Date.now();
    list.replaceChildren();
    [...devices.values()].sort((a, b) => a.device_id.localeCompare(b.device_id)).forEach((device) => {
      // retained 在线消息如果长期没有刷新，也应显示离线，覆盖异常断网未送达 LWT 的边界。
      const interval = Number.isFinite(device.heartbeat_interval_ms) ? device.heartbeat_interval_ms : 2000;
      const stale = typeof device.updated_at_ms !== "number" ||
        now - device.updated_at_ms > Math.max(8000, interval * 3);
      const online = device.online && !stale;
      const card = document.createElement("article");
      card.className = `device${online ? "" : " offline"}`;
      const heading = document.createElement("h2");
      const name = document.createElement("span");
      const badge = document.createElement("span");
      name.textContent = device.device_id;
      badge.className = "badge";
      badge.textContent = online ? "在线" : "离线";
      heading.append(name, badge);
      const details = document.createElement("dl");
      addField(details, "角色", device.role === "center" ? "中心节点" : "边缘节点");
      addField(details, "IP", device.ip || "—");
      addField(details, "CPU", valueText(device.cpu_usage_percent, "%"));
      addField(details, "温度", valueText(device.cpu_temperature_c, " °C"));
      addField(details, "摄像头", online && device.camera_online ? "在线" : "离线",
        online && device.camera_online ? "" : "bad");
      card.append(heading, details);
      list.append(card);
    });
    empty.hidden = devices.size > 0;
  }

  function start() {
    if (!window.mqtt) {
      state.textContent = "客户端加载失败";
      return;
    }
    const protocol = location.protocol === "https:" ? "wss" : "ws";
    client = mqtt.connect(`${protocol}://${location.host}/mqtt`, {
      // 页面当前通过局域网 HTTP 打开，避免依赖仅在安全上下文开放的 randomUUID。
      clientId: `rkmon-web-${Date.now().toString(36)}-${Math.random().toString(16).slice(2)}`,
      clean: true,
      connectTimeout: 3000,
      reconnectPeriod: 2000,
      keepalive: 15,
    });
    client.on("connect", () => {
      setConnection(true);
      client.subscribe("rkmon/devices/+/status", {qos: 0}, (error) => {
        if (error) setConnection(false);
      });
    });
    client.on("message", (topic, payload) => {
      try {
        const status = JSON.parse(payload.toString());
        const parts = topic.split("/");
        if (!validStatus(status) || parts.length !== 4 || parts[0] !== "rkmon" ||
            parts[1] !== "devices" || parts[2] !== status.device_id || parts[3] !== "status") return;
        devices.set(status.device_id, status);
        render();
      } catch (_) {}
    });
    client.on("offline", () => setConnection(false));
    client.on("close", () => setConnection(false));
    client.on("error", () => setConnection(false));
  }

  const refresh = setInterval(render, 1000);
  window.addEventListener("pagehide", () => {
    clearInterval(refresh);
    if (client) client.end(true);
  });
  setConnection(false);
  start();
})();
