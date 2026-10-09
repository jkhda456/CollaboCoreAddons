// Finds what an agent named — a number from `web read` ([N]), a CSS selector, or the visible text
// of a link or button — scrolls it into view and says where it is (view coordinates) and what it
// is. Returns a JSON string, or null. Called as (function(target) { ... }).
(function (target) {
  "use strict";
  let el = null;
  if (/^\d+$/.test(target))
    el = document.querySelector(`[data-wpe-ref="${target}"]`);
  if (!el) {
    try { el = document.querySelector(target); } catch (e) { }
  }
  if (!el) {
    const want = target.replace(/\s+/g, " ").trim().toLowerCase();
    const candidates = document.querySelectorAll("a, button, [role=button], [role=link], [role=tab], [role=menuitem], " +
      "input[type=submit], input[type=button], summary, label, option");
    let partial = null;
    for (const c of candidates) {
      const text = (c.innerText || c.value || c.getAttribute("aria-label") || "").replace(/\s+/g, " ").trim().toLowerCase();
      if (!text) continue;
      if (text === want) { el = c; break; }
      if (!partial && text.includes(want)) partial = c;
    }
    el = el || partial;
  }
  if (!el) return null;
  el.scrollIntoView({ block: "center", inline: "center", behavior: "instant" });
  const r = el.getBoundingClientRect();
  // a point of the element that is on top there (not covered by something else), else its center
  let x = r.left + r.width / 2, y = r.top + r.height / 2;
  const hit = document.elementFromPoint(x, y);
  if (hit && hit !== el && !el.contains(hit) && !hit.contains(el)) {
    for (const [fx, fy] of [[0.25, 0.5], [0.75, 0.5], [0.5, 0.25], [0.5, 0.75], [0.1, 0.5]]) {
      const px = r.left + r.width * fx, py = r.top + r.height * fy;
      const h = document.elementFromPoint(px, py);
      if (h && (h === el || el.contains(h))) { x = px; y = py; break; }
    }
  }
  const scale = window.devicePixelRatio || 1;
  return JSON.stringify({
    x: Math.round(x * scale), y: Math.round(y * scale),
    tag: el.tagName.toLowerCase(),
    text: (el.innerText || el.value || el.getAttribute("aria-label") || el.getAttribute("placeholder") || "").replace(/\s+/g, " ").trim().slice(0, 80),
    href: el.href || undefined,
    editable: el.matches("input, textarea, [contenteditable=''], [contenteditable=true]") || el.isContentEditable,
    select: el.tagName === "SELECT",
  });
})
