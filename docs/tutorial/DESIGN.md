# M5Tab5 Tutorial Visual Identity

## Status

Draft for review before HyperFrames HTML composition.

## Style Prompt

Dark technical maker documentary. The canvas should feel like a precise local control room: crisp device frames, readable diagrams, terminal snippets, and compact UI cards. Use color to separate hardware, local services, AI handoff, and smart-home control. Keep the style practical, modern, and evidence-driven rather than futuristic for its own sake.

Make the identity unmistakably M5Tab5-specific. Treat the composition as a centered device topology: M5Tab5 sits in the middle, with Home Assistant, Xiaozhi, Hermes, Claude, and Mac services arranged around it as visible nodes in a local control map. Use that node map repeatedly so the viewer always feels the project's actual system shape, not a generic app ecosystem.

Make the project's commit history part of the visual language. A build-history or commit-timeline strip should feel like an active story device, showing the tutorial as an evolution of flashes, fixes, and handoffs rather than optional decoration.

Use a guarded LAN bridge / gateway metaphor for the AI flow. Tab5 should read as sending intent toward the Mac through a constrained local bridge; the Mac owns secrets, approvals, execution packets, and the latest results. The bridge should feel protected, deliberate, and local.

For embedded issues, show immediate problem/fix pairing. Use split cards or adjacent panels so a pitfall appears next to its correction, making the fix legible at a glance instead of hiding the remedy in a later scene.

## Colors

| Role | Hex | Use |
|---|---|---|
| Canvas | `#0B0F14` | Main background. |
| Panel | `#141B24` | Diagram blocks and app cards. |
| Text Primary | `#F4F7FA` | Titles and main labels. |
| Text Secondary | `#A9B4C0` | Captions and source paths. |
| Hardware Accent | `#35D0BA` | M5Tab5 and firmware layer. |
| AI Accent | `#8B7CFF` | Claude and AI workflow. |
| Service Accent | `#FFB020` | Hermes and Mac services. |
| Smart Home Accent | `#46A6FF` | Home Assistant and device control. |
| Warning Accent | `#FF5C5C` | Pitfalls and blocked execution. |

## Typography

- Display: `Inter` or `Source Han Sans SC`.
- Body: `Source Han Sans SC`.
- Mono: `JetBrains Mono`.
- Keep Chinese narration captions large and sparse. Do not place dense paragraphs on screen.

## Layout Rules

- Use 16:9 landscape at 1920x1080.
- Keep scene content inside generous margins.
- Prefer diagrams, cards, and source captions over long text.
- Source paths should be small captions, not the primary focus.
- App tour cards should show one use case per card.
- Keep the center-device topology readable in every major scene, with M5Tab5 as the spatial anchor and the surrounding services tied to it by clear, local lines.
- Carry the commit timeline through the whole tutorial as a recurring ribbon or strip so the project history reads as a core narrative thread.
- Surface evidence anchors as compact labels or callouts tied to the relevant scene: `docs/TUTORIAL-outline.md`, git history, HA stability/flashing memory, and the Tab5-to-local-Claude approval/handoff memory.

## Motion Rules

- Scene transitions are required between all major scenes.
- Use entrance animations on every major element.
- Favor precise slides, fades, wipes, and diagram reveals.
- Avoid chaotic movement; the video should feel controlled and useful.
- Local AI sequence should animate one step at a time.
- Pitfall cards should reveal problem first, fix second.
- Timeline motion should feel cumulative, as if each commit or build step adds one more layer to the project story.
- Bridge/approval motion should be explicit: Tab5 intent travels first, then Mac approval or execution packets move back with the result.
- Problem/fix pairs should animate together so the viewer sees the mismatch and the correction in one glance.

## What NOT To Do

- Do not use generic cyberpunk neon styling.
- Do not use fake dashboards, fake metrics, or fake screenshots.
- Do not overload the screen with code.
- Do not imply Tab5 directly executes shell commands.
- Do not make it look like a product landing page.
- Do not reduce the project to a generic network diagram; the center-device topology and named services must stay visible.
- Do not treat the commit timeline as optional decoration or a background texture.
- Do not show Tab5 as owning secrets or executing privileged actions directly; the Mac remains the guarded execution side.
- Do not bury embedded pitfalls without their matching fixes beside them.
