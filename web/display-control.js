"use strict";
(() => {
  const buttons = [...document.querySelectorAll("[data-display-mode]")];
  if (!buttons.length) return;
  const setActive = (mode) => buttons.forEach((button) => {
    button.classList.toggle("active", button.dataset.displayMode === mode);
  });
  const load = async () => {
    try {
      const response = await fetch("/api/display/mode", {cache: "no-store"});
      if (!response.ok) throw new Error("mode request failed");
      setActive((await response.json()).mode);
    } catch (_) {}
  };
  buttons.forEach((button) => button.addEventListener("click", async () => {
    buttons.forEach((item) => { item.disabled = true; });
    try {
      const response = await fetch("/api/display/mode", {
        method: "PUT",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({mode: button.dataset.displayMode}),
      });
      if (!response.ok) throw new Error("mode update failed");
      setActive((await response.json()).mode);
    } catch (_) {
      button.title = "切换失败，请检查板端服务";
    } finally {
      buttons.forEach((item) => { item.disabled = false; });
    }
  }));
  load();
})();
