/* MintOS26 website — small, dependency-free enhancements. */
(function () {
  "use strict";

  document.documentElement.classList.add("js");
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ---------- Footer year ---------- */
  var year = document.getElementById("year");
  if (year) year.textContent = String(new Date().getFullYear());

  /* ---------- Nav: border on scroll, mobile menu, active link ---------- */
  var nav = document.getElementById("nav");
  var toggle = document.querySelector(".menu-toggle");
  var links = document.getElementById("nav-links");

  function onScroll() {
    if (nav) nav.classList.toggle("is-scrolled", window.scrollY > 8);
  }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  function closeMenu() {
    if (!toggle || !links) return;
    toggle.setAttribute("aria-expanded", "false");
    toggle.setAttribute("aria-label", "Open menu");
    links.classList.remove("is-open");
  }
  if (toggle && links) {
    toggle.addEventListener("click", function () {
      var open = toggle.getAttribute("aria-expanded") !== "true";
      toggle.setAttribute("aria-expanded", String(open));
      toggle.setAttribute("aria-label", open ? "Close menu" : "Open menu");
      links.classList.toggle("is-open", open);
    });
    links.addEventListener("click", function (e) {
      if (e.target.closest("a")) closeMenu();
    });
    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape") closeMenu();
    });
  }

  if ("IntersectionObserver" in window && links) {
    var navMap = {};
    links.querySelectorAll('a[href^="#"]').forEach(function (a) {
      navMap[a.getAttribute("href").slice(1)] = a;
    });
    var sectionObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        var link = navMap[entry.target.id];
        if (!link) return;
        if (entry.isIntersecting) {
          links.querySelectorAll("a").forEach(function (a) { a.classList.remove("is-active"); });
          link.classList.add("is-active");
        }
      });
    }, { rootMargin: "-45% 0px -50% 0px" });
    Object.keys(navMap).forEach(function (id) {
      var el = document.getElementById(id);
      if (el) sectionObserver.observe(el);
    });
  }

  /* ---------- Reveal on scroll ---------- */
  var reveals = document.querySelectorAll(".reveal");
  if (!reduceMotion && "IntersectionObserver" in window) {
    var revealObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-in");
          revealObserver.unobserve(entry.target);
        }
      });
    }, { rootMargin: "0px 0px -8% 0px", threshold: 0.05 });
    reveals.forEach(function (el) { revealObserver.observe(el); });
  } else {
    reveals.forEach(function (el) { el.classList.add("is-in"); });
  }

  /* ---------- Count-up stats ---------- */
  function countUp(el) {
    var original = el.textContent;
    var match = original.match(/^(\D*)([\d,]+)(.*)$/);
    var target = parseInt(el.getAttribute("data-count"), 10);
    if (!match || !target || reduceMotion) return;
    var prefix = match[1], suffix = match[3];
    var start = null, duration = 1200;
    function frame(t) {
      if (start === null) start = t;
      var p = Math.min((t - start) / duration, 1);
      var eased = 1 - Math.pow(1 - p, 3);
      el.textContent = prefix + Math.round(target * eased).toLocaleString("en-US") + suffix;
      if (p < 1) requestAnimationFrame(frame);
      else el.textContent = original;
    }
    requestAnimationFrame(frame);
  }
  if ("IntersectionObserver" in window) {
    var statObserver = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          countUp(entry.target);
          statObserver.unobserve(entry.target);
        }
      });
    }, { threshold: 0.6 });
    document.querySelectorAll("[data-count]").forEach(function (el) { statObserver.observe(el); });
  }

  /* ---------- Screenshot tabs ---------- */
  var tablist = document.querySelector('[role="tablist"]');
  if (tablist) {
    var tabs = Array.prototype.slice.call(tablist.querySelectorAll('[role="tab"]'));
    function select(tab, focus) {
      tabs.forEach(function (t) {
        var on = t === tab;
        t.setAttribute("aria-selected", String(on));
        t.tabIndex = on ? 0 : -1;
        var panel = document.getElementById(t.getAttribute("aria-controls"));
        if (panel) panel.hidden = !on;
      });
      if (focus) tab.focus();
      tab.scrollIntoView({ block: "nearest", inline: "nearest", behavior: reduceMotion ? "auto" : "smooth" });
    }
    tabs.forEach(function (tab, i) {
      tab.addEventListener("click", function () { select(tab, false); });
      tab.addEventListener("keydown", function (e) {
        var next = null;
        if (e.key === "ArrowRight") next = tabs[(i + 1) % tabs.length];
        if (e.key === "ArrowLeft") next = tabs[(i - 1 + tabs.length) % tabs.length];
        if (e.key === "Home") next = tabs[0];
        if (e.key === "End") next = tabs[tabs.length - 1];
        if (next) { e.preventDefault(); select(next, true); }
      });
    });
  }

  /* ---------- Copy buttons ---------- */
  document.querySelectorAll(".code .copy").forEach(function (btn) {
    btn.addEventListener("click", function () {
      var code = btn.parentElement.querySelector("code");
      if (!code) return;
      var text = code.textContent;
      var done = function () {
        btn.textContent = "Copied";
        btn.classList.add("is-done");
        setTimeout(function () { btn.textContent = "Copy"; btn.classList.remove("is-done"); }, 1600);
      };
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(done, function () { fallbackCopy(text); done(); });
      } else {
        fallbackCopy(text);
        done();
      }
    });
  });
  function fallbackCopy(text) {
    var ta = document.createElement("textarea");
    ta.value = text;
    ta.setAttribute("readonly", "");
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand("copy"); } catch (err) { /* ignore */ }
    document.body.removeChild(ta);
  }

  /* ---------- MintDE Terminal re-creation ----------
     Replies are copied from kernel/user/desktop.c (MintDE 1.0.7).
     PCI values come from the 26.5.008.0 QEMU COM1 boot log. */
  var out = document.getElementById("term-out");
  var form = document.getElementById("term-form");
  var input = document.getElementById("term-input");
  if (!out || !form || !input) return;

  var bootTime = Date.now();
  var PROMPT = "root#mintos >";

  var replies = {
    help: [
      "Commands: help, clear, ver, uptime, status, cpu",
      "pci, resources, drivers, mounts, io, logs, net",
      "drives, volumes, use sda/sdb, net start/status/test, ls, cd, pwd, cat",
      "use sda | ls [name] | cd <dir> | pwd | cat <file>"
    ],
    ver: ["MintOS26 26.5.008.0 / MintDE 1.0.7"],
    status: [
      "Kernel services: ring-3 syscall ABI active",
      "Filesystem: FAT16 mounted read-only",
      "Persistent file writes: disabled"
    ],
    cpu: [
      "Architecture: x86-64; GDT/TSS and IDT active",
      "Timer: PIT near 100 Hz; cooperative scheduler active",
      "CPUID feature and per-core topology API not yet exported"
    ],
    pci: [
      "0:0.0 0x8086:0x29C0 class 0x6:0x0",
      "0:1.0 0x1234:0x1111 class 0x3:0x0",
      "0:2.0 0x8086:0x100E class 0x2:0x0",
      "0:31.0 0x8086:0x2918 class 0x6:0x1",
      "0:31.2 0x8086:0x2922 class 0x1:0x6",
      "0:31.3 0x8086:0x2930 class 0xC:0x5"
    ],
    mounts: [
      "/session  RAMFS from ramfs [volatile]",
      "/Volumes/sdb  FAT16 from sdb [read-only]"
    ],
    net: [
      "VirtualBox E1000: detected but not initialized",
      { note: "… register snapshot shortened on this page …" },
      "No TCP/IP, DHCP, DNS, or external packet send service yet"
    ]
  };
  replies["net status"] = replies.net;

  var needsVm = ["resources", "drivers", "io", "logs", "drives", "volumes", "use", "ls", "cd", "pwd", "cat", "net start", "net test"];

  function row(text, cls) {
    var div = document.createElement("div");
    div.className = "row" + (cls ? " " + cls : "");
    div.textContent = text;
    out.appendChild(div);
    return div;
  }
  function commandRow(cmd) {
    var div = row("", "cmd");
    var p = document.createElement("span");
    p.className = "p";
    p.textContent = PROMPT + " ";
    div.appendChild(p);
    div.appendChild(document.createTextNode(cmd));
  }
  function scrollDown() { out.scrollTop = out.scrollHeight; }

  function run(raw) {
    var cmd = raw.trim().replace(/\s+/g, " ");
    if (!cmd) return;
    if (cmd === "clear") { out.textContent = ""; return; }
    commandRow(cmd);

    var lines = replies[cmd];
    if (cmd === "uptime") {
      lines = ["Uptime: " + Math.floor((Date.now() - bootTime) / 1000) + " seconds"];
    }
    if (!lines) {
      var first = cmd.split(" ")[0];
      var vm = needsVm.indexOf(cmd) !== -1 || needsVm.indexOf(first) !== -1;
      lines = vm
        ? [{ note: "`" + cmd + "` reads real hardware. Build MintOS26 and try it in a VM." }]
        : ["Unknown command; type help"];
    }
    lines.forEach(function (l) {
      if (typeof l === "string") row(l);
      else row(l.note, "note");
    });
    scrollDown();
  }

  var history = [], histIndex = 0;
  form.addEventListener("submit", function (e) {
    e.preventDefault();
    var value = input.value;
    if (value.trim()) { history.push(value.trim()); histIndex = history.length; }
    run(value);
    input.value = "";
  });
  input.addEventListener("keydown", function (e) {
    if (e.key === "ArrowUp" && history.length) {
      e.preventDefault();
      histIndex = Math.max(0, histIndex - 1);
      input.value = history[histIndex];
    } else if (e.key === "ArrowDown" && history.length) {
      e.preventDefault();
      histIndex = Math.min(history.length, histIndex + 1);
      input.value = history[histIndex] || "";
    }
  });

  document.querySelectorAll(".chip[data-cmd]").forEach(function (chip) {
    chip.addEventListener("click", function () {
      run(chip.getAttribute("data-cmd"));
      input.focus({ preventScroll: true });
    });
  });

  // Greet with "ver" typed out the first time the terminal comes into view.
  var greeted = false;
  function greet() {
    if (greeted) return;
    greeted = true;
    if (reduceMotion) { run("ver"); return; }
    var text = "ver", i = 0;
    var timer = setInterval(function () {
      if (document.activeElement === input && input.value) { clearInterval(timer); return; }
      input.value = text.slice(0, ++i);
      if (i >= text.length) {
        clearInterval(timer);
        setTimeout(function () {
          if (input.value === text) { run(text); input.value = ""; }
        }, 350);
      }
    }, 120);
  }
  if ("IntersectionObserver" in window) {
    var termObserver = new IntersectionObserver(function (entries) {
      if (entries[0].isIntersecting) { greet(); termObserver.disconnect(); }
    }, { threshold: 0.5 });
    termObserver.observe(document.getElementById("term"));
  } else {
    greet();
  }
})();
