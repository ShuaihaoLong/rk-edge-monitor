"use strict";
(() => {
  const state = document.getElementById("mqtt-state");
  const list = document.getElementById("device-list");
  const empty = document.getElementById("device-empty");
  const sensorList = document.getElementById("sensor-list");
  const sensorEmpty = document.getElementById("sensor-empty");
  const devices = new Map();
  const sensors = new Map();
  let client = null;
  let connected = false;

  function setConnection(value) {
    connected = value;
    state.textContent = value ? "已连接" : "正在重连";
    state.dataset.state = value ? "ok" : "offline";
    if (!value) sensors.forEach((sensor) => { sensor.sample = null; sensor.status = null; });
    render();
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
      const online = connected && device.online && !stale;
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
      addField(details, "CPU 温度", valueText(device.cpu_temperature_c, " °C"));
      addField(details, "摄像头", online && device.camera_online ? "在线" : "离线",
        online && device.camera_online ? "" : "bad");
      card.append(heading, details);
      list.append(card);
    });
    empty.hidden = devices.size > 0;
    renderSensors();
  }

  function renderSensors() {
    sensorList.replaceChildren();
    const now = performance.now();
    [...sensors.entries()].sort(([a], [b]) => a.localeCompare(b)).forEach(([id, sensor]) => {
      const timeout = sensor.status?.stale_timeout_ms || 5000;
      const interval = devices.get(id)?.heartbeat_interval_ms || 2000;
      const statusFresh = sensor.status && now - sensor.statusAt <= Math.max(8000, interval * 3);
      const sampleFresh = sensor.sample && now - sensor.sampleAt <= timeout;
      const live = connected && statusFresh && sensor.status.online && sampleFresh;
      const label = !connected ? "连接中断" : !statusFresh ? "等待设备状态" :
        !sensor.status.online ? "暂无有效数据" : live ? "实时" : sensor.sample ? "数据已过期" : "等待采样";
      const card = document.createElement("article");
      card.className = "sensor";
      card.dataset.state = live ? "ok" : "stale";
      const heading = document.createElement("h2");
      const name = document.createElement("span");
      name.textContent = id;
      const badge = document.createElement("span");
      badge.className = "sensor-state";
      badge.textContent = label;
      heading.append(name, badge);
      const readings = document.createElement("dl");
      readings.className = "sensor-readings";
      [["环境温度", "temperature_c", "°C", "temperature"],
       ["相对湿度", "humidity_percent", "%RH", "humidity"]].forEach(([title, field, unit, className]) => {
        const group = document.createElement("div");
        const term = document.createElement("dt");
        term.textContent = title;
        const value = document.createElement("dd");
        value.className = className;
        value.textContent = live ? valueText(sensor.sample[field]) : "—";
        const suffix = document.createElement("small");
        suffix.textContent = unit;
        value.append(suffix);
        group.append(term, value);
        readings.append(group);
      });
      const updated = document.createElement("p");
      updated.className = "sensor-update";
      updated.textContent = live ? `最近接收 · ${Math.floor((now - sensor.sampleAt) / 1000)} 秒前` : "等待新的有效温湿度数据";
      card.append(heading, readings, updated);
      sensorList.append(card);
    });
    sensorEmpty.hidden = sensors.size > 0;
    sensorEmpty.textContent = connected ? "等待温湿度数据…" : "数据连接中断，正在重连…";
  }

  function receiveSensor(id, kind, value, packet) {
    if (!value || value.schema !== 1) return;
    const sensor = sensors.get(id) || {};
    if (kind === "status") {
      if (typeof value.online !== "boolean" || !Number.isFinite(value.stale_timeout_ms) ||
          value.stale_timeout_ms < 500 || value.stale_timeout_ms > 60000) return;
      sensor.status = value;
      sensor.statusAt = performance.now();
      if (!value.online) sensor.sample = null;
    } else {
      // 温湿度不是 retained 消息，避免把 broker 留存的历史读数当作当前采样。
      if (packet?.retain || !Number.isFinite(value.temperature_c) || value.temperature_c < 0 || value.temperature_c > 50 ||
          !Number.isFinite(value.humidity_percent) || value.humidity_percent < 0 || value.humidity_percent > 100) return;
      sensor.sample = value;
      sensor.sampleAt = performance.now();
    }
    sensors.set(id, sensor);
    render();
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
      client.subscribe(["rkmon/devices/+/status", "rkmon/devices/+/stm32/status",
        "rkmon/devices/+/stm32/telemetry"], {qos: 0}, (error, granted) => {
        if (error || granted?.some((subscription) => subscription.qos === 128)) setConnection(false);
      });
    });
    client.on("message", (topic, payload, packet) => {
      try {
        const status = JSON.parse(payload.toString());
        const parts = topic.split("/");
        if (parts[0] !== "rkmon" || parts[1] !== "devices" || !/^[A-Za-z0-9_-]{1,64}$/.test(parts[2])) return;
        if (parts.length === 5 && parts[3] === "stm32" && ["status", "telemetry"].includes(parts[4])) {
          receiveSensor(parts[2], parts[4], status, packet);
          return;
        }
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

  let refresh = setInterval(render, 1000);
  window.addEventListener("pagehide", () => {
    clearInterval(refresh);
    refresh = null;
    if (client) client.end(true);
    client = null;
    setConnection(false);
  });
  window.addEventListener("pageshow", () => {
    if (refresh === null) {
      refresh = setInterval(render, 1000);
      start();
    }
  });
  setConnection(false);
  start();
})();
