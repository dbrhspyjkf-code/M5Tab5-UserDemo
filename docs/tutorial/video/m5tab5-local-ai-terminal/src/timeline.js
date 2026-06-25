(() => {
  const { scenes } = window.m5tab5StoryData;

  window.__timelines = window.__timelines || {};

  const tl = gsap.timeline({ paused: true });

  tl.set(".scene", { opacity: 0 }, 0);
  tl.set("#s1", { opacity: 1 }, 0);
  tl.set(".transition-wipe", { opacity: 0, xPercent: -110 }, 0);

  scenes.forEach((scene, index) => {
    const selector = `#${scene.id}`;

    tl.set(selector, { opacity: 1 }, scene.start);
    tl.from(
      `${selector} .eyebrow, ${selector} .title, ${selector} .subtitle`,
      {
        y: 36,
        opacity: 0,
        duration: 0.7,
        stagger: 0.12,
        ease: "power3.out",
      },
      scene.start + 0.2
    );

    if (index > 0) {
      tl.fromTo(
        ".transition-wipe",
        {
          xPercent: -110,
          opacity: 1,
        },
        {
          xPercent: 110,
          duration: 0.65,
          ease: "power2.inOut",
        },
        scene.start - 0.35
      );
    }
  });

  tl.to("#s7", { opacity: 0, duration: 0.6, ease: "power2.in" }, 419.2);

  window.__timelines["m5tab5-local-ai-terminal"] = tl;
})();
