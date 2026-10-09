// The page as text for an agent: what a reader sees, in reading order, with every visible
// interactive element numbered — [N] — so that `web click N`, `web type TEXT N`, `web select N
// VALUE` can name it. The numbers are kept on the elements (data-wpe-ref) until the next read.
// Returns a string. Called as (function(maxChars, includeHidden) { ... }).
(function (maxChars, includeHidden) {
  "use strict";
  const out = [];
  let size = 0, truncated = false, ref = 0;
  const SKIP = new Set(["SCRIPT", "STYLE", "NOSCRIPT", "TEMPLATE", "SVG", "CANVAS", "IFRAME", "OBJECT", "EMBED", "META", "LINK", "HEAD"]);
  const BLOCK = new Set(["ADDRESS", "ARTICLE", "ASIDE", "BLOCKQUOTE", "DD", "DETAILS", "DIALOG", "DIV", "DL", "DT", "FIELDSET",
    "FIGCAPTION", "FIGURE", "FOOTER", "FORM", "H1", "H2", "H3", "H4", "H5", "H6", "HEADER", "HR", "LI", "MAIN", "NAV", "OL", "P",
    "PRE", "SECTION", "SUMMARY", "TABLE", "TBODY", "THEAD", "TFOOT", "TR", "UL", "BODY", "CAPTION", "LEGEND", "OPTION"]);
  const INTERACTIVE = "a[href], button, input, textarea, select, summary, [role=button], [role=link], [role=checkbox], " +
    "[role=radio], [role=tab], [role=menuitem], [role=option], [role=switch], [role=combobox], [role=textbox], " +
    "[contenteditable=''], [contenteditable=true], [onclick], [tabindex]:not([tabindex='-1'])";

  for (const e of document.querySelectorAll("[data-wpe-ref]"))
    e.removeAttribute("data-wpe-ref");

  let line = "";
  function emit(text) {
    if (truncated) return;
    if (size + text.length > maxChars) {
      text = text.slice(0, Math.max(0, maxChars - size));
      truncated = true;
    }
    line += text;
    size += text.length;
  }
  function newline() {
    // a line of nothing but table-cell bars is empty; leading bars and spaces go
    const trimmed = line.replace(/[ \t|]+$/, "").replace(/^[ \t]*(\|[ \t]*)+/, "").replace(/^[ \t]+(?=\[)/, "");
    if (trimmed.trim() || (out.length && out[out.length - 1] !== ""))
      out.push(trimmed.trim() ? trimmed : "");
    line = "";
  }
  function visible(el) {
    if (includeHidden) return true;
    const style = getComputedStyle(el);
    if (style.display === "none" || style.visibility === "hidden" || style.visibility === "collapse") return false;
    if (parseFloat(style.opacity) === 0 && !el.matches("input, select, textarea")) return false;
    const rect = el.getBoundingClientRect();
    return rect.width > 0 || rect.height > 0 || style.display === "contents";
  }
  function label(el) {
    const aria = el.getAttribute("aria-label") || el.getAttribute("title") || el.getAttribute("alt");
    const text = (el.innerText || el.textContent || "").replace(/\s+/g, " ").trim();
    return (text || aria || "").slice(0, 120);
  }
  function describeInput(el) {
    const tag = el.tagName;
    if (tag === "SELECT") {
      const options = Array.from(el.options).map(o => (o.selected ? "*" : "") + o.text.trim()).slice(0, 30);
      return `select${el.name ? " " + el.name : ""}: ${options.join(" | ")}`;
    }
    if (tag === "TEXTAREA" || el.isContentEditable)
      return `textbox${el.name ? " " + el.name : ""}${el.placeholder ? ' "' + el.placeholder + '"' : ""}: ${(el.value ?? el.innerText ?? "").slice(0, 200)}`;
    const type = (el.getAttribute("type") || "text").toLowerCase();
    const name = el.name || el.id || "";
    if (type === "checkbox" || type === "radio")
      return `${type}${name ? " " + name : ""}${el.checked ? " (checked)" : ""}${el.labels && el.labels[0] ? ": " + el.labels[0].innerText.trim().slice(0, 80) : ""}`;
    if (type === "submit" || type === "button" || type === "reset")
      return `button: ${el.value || label(el) || type}`;
    if (type === "hidden") return null;
    const value = type === "password" ? (el.value ? "********" : "") : el.value;
    return `input ${type}${name ? " " + name : ""}${el.placeholder ? ' "' + el.placeholder + '"' : ""}: ${value}`;
  }

  function walk(node, pre) {
    if (truncated) return;
    if (node.nodeType === Node.TEXT_NODE) {
      const text = pre ? node.data : node.data.replace(/\s+/g, " ");
      if (!text) return;
      if (!pre && text === " " && (!line || line.endsWith(" "))) return;
      emit(line.endsWith(" ") && text.startsWith(" ") ? text.slice(1) : text);
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    const el = node;
    const tag = el.tagName;
    if (SKIP.has(tag.toUpperCase())) return;
    if (!visible(el)) return;

    if (el.matches(INTERACTIVE)) {
      const n = ++ref;
      el.setAttribute("data-wpe-ref", String(n));
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || el.isContentEditable) {
        const description = describeInput(el);
        if (description === null) { ref--; el.removeAttribute("data-wpe-ref"); return; }
        emit(` [${n}] ${description} `);
        return;
      }
      if (tag === "A") {
        emit(` [${n}]`);
        const text = label(el);
        emit(text ? `${text}` : "(link)");
        const href = el.getAttribute("href");
        if (href && !href.startsWith("javascript:") && !href.startsWith("#"))
          emit(` <${el.href}>`);
        emit(" ");
        return;
      }
      if (tag === "BUTTON" || el.getAttribute("role")) {
        emit(` [${n}] ${el.getAttribute("role") || "button"}: ${label(el)} `);
        return;
      }
      emit(` [${n}]`);
    }

    const block = BLOCK.has(tag) || ["block", "flex", "grid", "list-item", "table", "table-row"].includes(getComputedStyle(el).display);
    if (block) newline();
    if (/^H[1-6]$/.test(tag)) emit("#".repeat(+tag[1]) + " ");
    else if (tag === "LI") emit("- ");
    else if (tag === "IMG") {
      const alt = el.getAttribute("alt");
      if (alt) emit(`[image: ${alt.slice(0, 100)}] `);
    } else if (tag === "BR") { newline(); return; }
    else if (tag === "HR") { emit("---"); newline(); return; }
    else if ((tag === "TD" || tag === "TH") && line.trim() && !/\|\s*$/.test(line)) emit(" | ");

    const isPre = pre || tag === "PRE" || tag === "TEXTAREA";
    if (el.shadowRoot)
      for (const child of el.shadowRoot.childNodes) walk(child, isPre);
    for (const child of el.childNodes) walk(child, isPre);
    if (block) newline();
  }

  walk(document.body || document.documentElement, false);
  newline();
  while (out.length && out[out.length - 1] === "") out.pop();
  let text = out.join("\n");
  if (truncated) text += `\n[... cut at ${maxChars} characters: \`web read --max N\` for more]`;
  return `${document.title}\n${location.href}\n\n${text}\n`;
})
