"use strict";
(() => {
  const list = document.getElementById("ad-list");
  const empty = document.getElementById("empty");
  const status = document.getElementById("upload-status");
  const mode = document.getElementById("display-mode");
  let ads = [];

  async function request(url, options) {
    const response = await fetch(url, options);
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || "请求失败");
    return data;
  }

  function duration(seconds) {
    const total = Math.round(Number(seconds) || 0);
    return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, "0")}`;
  }

  async function saveOrder() {
    await request("/api/ads/order", {
      method: "PUT", headers: {"Content-Type": "application/json"},
      body: JSON.stringify({ids: ads.map((item) => item.id)}),
    });
  }

  function render() {
    list.replaceChildren();
    empty.hidden = ads.length > 0;
    ads.forEach((ad, index) => {
      const item = document.createElement("li");
      item.className = ad.enabled ? "ad-item" : "ad-item disabled";
      const info = document.createElement("div");
      const name = document.createElement("strong");
      name.textContent = ad.original_name;
      const meta = document.createElement("small");
      meta.textContent = `${duration(ad.duration)} · ${(ad.bytes / 1048576).toFixed(1)} MiB`;
      info.append(name, meta);
      const actions = document.createElement("div");
      actions.className = "ad-actions";
      [["上移", -1], ["下移", 1]].forEach(([label, offset]) => {
        const button = document.createElement("button");
        button.type = "button"; button.textContent = label;
        button.disabled = index + offset < 0 || index + offset >= ads.length;
        button.addEventListener("click", async () => {
          [ads[index], ads[index + offset]] = [ads[index + offset], ads[index]];
          render();
          try { await saveOrder(); } catch (error) { status.textContent = error.message; }
        });
        actions.append(button);
      });
      const remove = document.createElement("button");
      remove.type = "button"; remove.textContent = "删除";
      remove.addEventListener("click", async () => {
        if (!window.confirm(`删除“${ad.original_name}”？`)) return;
        try { await request(`/api/ads/${encodeURIComponent(ad.id)}`, {method: "DELETE"}); await load(); }
        catch (error) { status.textContent = error.message; }
      });
      actions.append(remove);
      item.append(info, actions);
      list.append(item);
    });
  }

  async function load() {
    const data = await request("/api/ads", {cache: "no-store"});
    ads = data.ads || [];
    mode.textContent = data.mode === "ad" ? "当前：广告" : "当前：实时监控";
    render();
  }

  document.getElementById("upload-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const input = document.getElementById("video-file");
    if (!input.files.length) return;
    status.textContent = "上传并校验中…";
    const form = new FormData(); form.append("video", input.files[0]);
    try {
      await request("/api/ads", {method: "POST", body: form});
      input.value = ""; status.textContent = "上传完成"; await load();
    } catch (error) { status.textContent = error.message; }
  });
  load().catch((error) => { status.textContent = error.message; });
})();
