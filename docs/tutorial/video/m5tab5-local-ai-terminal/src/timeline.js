(() => {
  const data = window.m5tab5StoryData;
  const {
    scenes,
    appFlashes,
    systemLayers,
    featureCards,
    aiLoopSteps,
    guardrails,
    pitfalls,
    buildCommands,
    verificationChecks,
    evidence,
    sceneEvidence,
  } = data;

  window.__timelines = window.__timelines || {};

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text) node.textContent = text;
    return node;
  }

  function fillSceneText() {
    scenes.forEach((scene) => {
      const section = document.getElementById(scene.id);
      section.querySelectorAll("[data-field]").forEach((node) => {
        node.textContent = scene[node.dataset.field] || "";
      });
    });
  }

  function renderOrbitMaps() {
    const nodes = [
      ["Home Assistant", "实体控制与状态回读", "node-home"],
      ["Xiaozhi", "语音入口与音频状态", "node-xiaozhi"],
      ["Hermes", "本地服务桥", "node-service"],
      ["Claude", "请求处理与结果整理", "node-ai"],
      ["Mac Gateway", "审批、执行包与回传", "node-mac"],
    ];

    document.querySelectorAll("[data-orbit-map]").forEach((root) => {
      nodes.forEach(([title, copy, className]) => {
        const node = el("div", `node ${className}`);
        node.append(el("strong", "", title));
        node.append(el("span", "", copy));
        root.append(node);
      });
    });
  }

  function renderAppFlashes() {
    const root = document.querySelector("[data-app-flashes]");
    appFlashes.forEach(([title, copy]) => {
      const card = el("div", "mini-app");
      card.append(el("strong", "", title));
      card.append(el("span", "", copy));
      root.append(card);
    });
  }

  function renderSystemLayers() {
    const root = document.querySelector("[data-system-layers]");
    systemLayers.forEach((layer) => {
      const card = el("article", `system-card accent-${layer.accent}`);
      card.append(el("div", "system-card-title", layer.title));
      const list = el("div", "system-items");
      layer.items.forEach((item) => list.append(el("div", "system-item", item)));
      card.append(list);
      root.append(card);
    });
  }

  function renderRibbon() {
    document.querySelectorAll("[data-scene-ribbon]").forEach((root) => {
      scenes.forEach((scene) => {
        const item = el("div", "ribbon-step");
        item.append(el("strong", "", scene.title));
        item.append(el("span", "", scene.timeline));
        root.append(item);
      });
    });
  }

  function renderFeatures() {
    const root = document.querySelector("[data-feature-grid]");
    featureCards.forEach(([title, copy, source, accent]) => {
      const card = el("article", `card feature-card accent-${accent}`);
      card.append(el("div", "card-title", title));
      card.append(el("div", "card-copy", copy));
      card.append(el("div", "card-source", source));
      root.append(card);
    });
  }

  function renderAiFlow() {
    const root = document.querySelector("[data-ai-flow]");
    aiLoopSteps.forEach(([index, title, copy]) => {
      const step = el("article", "step ai-step");
      step.append(el("div", "step-index", index));
      step.append(el("div", "step-title", title));
      step.append(el("div", "step-copy", copy));
      root.append(step);
    });

    const guardRoot = document.querySelector("[data-guardrails]");
    guardrails.forEach((label) => guardRoot.append(el("div", "guardrail", label)));
  }

  function renderPitfalls() {
    const root = document.querySelector("[data-pitfall-grid]");
    pitfalls.forEach(([problem, fix]) => {
      const card = el("article", "pitfall");
      const left = el("div", "pitfall-problem");
      left.append(el("div", "pitfall-label", "Problem"));
      left.append(el("div", "pitfall-copy", problem));
      const right = el("div", "pitfall-fix");
      right.append(el("div", "pitfall-label", "Fix"));
      right.append(el("div", "pitfall-copy", fix));
      card.append(left, right);
      root.append(card);
    });
  }

  function renderBuild() {
    const terminal = document.querySelector("[data-build-commands]");
    buildCommands.forEach((command) => terminal.append(el("div", "terminal-line", command)));

    const checks = document.querySelector("[data-verification-checks]");
    verificationChecks.forEach((check) => checks.append(el("div", "panel check", check)));

    const strip = document.querySelector("[data-evidence]");
    evidence.forEach((item) => strip.append(el("div", "source-pill", item)));
  }

  function renderCaptions() {
    scenes.forEach((scene) => {
      const section = document.getElementById(scene.id);
      const captionRoot = section.querySelector("[data-captions]");
      scene.captions.forEach((caption, index) => {
        const node = el("div", "caption-line", caption);
        node.dataset.captionIndex = String(index);
        captionRoot.append(node);
      });

      const evidenceRoot = section.querySelector("[data-scene-evidence]");
      sceneEvidence[scene.id].forEach((label) => evidenceRoot.append(el("div", "evidence-tag", label)));
    });
  }

  function renderAll() {
    fillSceneText();
    renderOrbitMaps();
    renderAppFlashes();
    renderSystemLayers();
    renderRibbon();
    renderFeatures();
    renderAiFlow();
    renderPitfalls();
    renderBuild();
    renderCaptions();
  }

  renderAll();

  const tl = gsap.timeline({ paused: true });

  tl.set(".transition-wipe", { opacity: 0, xPercent: -110 }, 0);

  scenes.forEach((scene, index) => {
    const selector = `#${scene.id}`;
    const start = scene.start;
    const sceneEnd = scene.start + scene.duration;

    tl.from(
      `${selector} .eyebrow, ${selector} .title, ${selector} .subtitle, ${selector} .timecode`,
      { y: 34, opacity: 0, duration: 0.7, stagger: 0.1, ease: "power3.out", immediateRender: false },
      start + 0.2
    );
    tl.from(
      `${selector} .card, ${selector} .step, ${selector} .pitfall, ${selector} .system-card, ${selector} .node, ${selector} .mini-app, ${selector} .source-pill, ${selector} .check`,
      { y: 26, opacity: 0, duration: 0.55, stagger: 0.05, ease: "power2.out", immediateRender: false },
      start + 0.8
    );
    if (index > 0) {
      tl.fromTo(
        ".transition-wipe",
        { xPercent: -110, opacity: 1 },
        { xPercent: 110, duration: 0.65, ease: "power2.inOut" },
        start - 0.35
      );
    }

    if (scene.id === "s4") {
      tl.from(
        `${selector} .guardrail`,
        { scale: 0.96, opacity: 0, duration: 0.45, stagger: 0.12, ease: "power2.out", immediateRender: false },
        start + 4.2
      );
    }

    if (scene.id === "s5") {
      tl.from(
        `${selector} .evidence-line`,
        { y: 20, opacity: 0, duration: 0.6, ease: "power2.out", immediateRender: false },
        sceneEnd - 8
      );
    }
  });

  window.__timelines["m5tab5-local-ai-terminal"] = tl;
})();
