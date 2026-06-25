# M5Tab5 HyperFrames Tutorial Asset Inventory

## Asset Policy

Use real project evidence first. Use generated or diagrammatic assets only when real screenshots or footage are unavailable. Do not fake live values.

## Production Format

All captures, screenshots, generated visuals, and diagrams must be framed for 16:9 landscape composition and remain safe within a 1920x1080 output area.

## Required Assets

| Asset ID | Scenes | Type | Source Or Capture Path | Status | Notes |
|---|---|---|---|---|---|
| A001 | S1, S7 | Device hero image | Real M5Tab5 photo or generated product-style render | Generated placeholder ready | `video/m5tab5-local-ai-terminal/assets/generated/m5tab5-device-frame.svg`; real photo can replace later. |
| A002 | S2, S4, S7 | Architecture diagram data | Derived from repo and gateway memory | Generated placeholder ready | `video/m5tab5-local-ai-terminal/assets/generated/local-bridge-diagram.svg`; firmware, app layer, Mac services, LAN HTTP JSON. |
| A003 | S3 | HA app screenshot or simulator capture | `app/apps/app_ha` runtime | Missing | Prefer real device or desktop simulator. |
| A004 | S3 | Xiaozhi app screenshot or still | `app/apps/app_xiaozhi` runtime | Missing | Include idle face or voice state. |
| A005 | S3, S4 | Claude assistant screenshot | `app/apps/app_project_assistant` runtime | Missing | Show quick actions if possible. |
| A006 | S3 | Voice input screenshot | `app/apps/app_voice_input` runtime | Missing | Show waveform UI. |
| A007 | S3 | Tools/settings screenshot | `app/apps/app_settings` runtime | Missing | Calculator, currency, units, mail list. |
| A008 | S3 | Email LED photo or generated strip visualization | `app/apps/app_email_led` runtime | Generated placeholder ready | `video/m5tab5-local-ai-terminal/assets/generated/email-led-strip.svg`; clearly labeled as generated, not live capture. |
| A009 | S3 | LoRa chat screenshot | `app/apps/app_lora_chat` runtime | Missing | Show chat bubbles and input. |
| A010 | S3 | Stocks screenshot | `app/apps/app_stocks` runtime | Missing | Show table and conclusion line. |
| A011 | S5 | Pitfall cards | Drawn in HyperFrames | Implemented in composition | Problem/fix cards are rendered from `src/story-data.js`. |
| A012 | S6 | Terminal command snippets | `docs/TUTORIAL-outline.md` | Ready | Use shortened command reveal. |
| A013 | S3 | Unit-Puzzle LED photo or generated strip visualization | `app/apps/app_unit_puzzle` runtime | Generated placeholder ready | `video/m5tab5-local-ai-terminal/assets/generated/unit-puzzle-strip.svg`; separate from Email LED slot. |

## Generated Placeholder Files

- `video/m5tab5-local-ai-terminal/assets/generated/m5tab5-device-frame.svg`
- `video/m5tab5-local-ai-terminal/assets/generated/m5tab5-device-frame-closing.svg`
- `video/m5tab5-local-ai-terminal/assets/generated/local-bridge-diagram.svg`
- `video/m5tab5-local-ai-terminal/assets/generated/email-led-strip.svg`
- `video/m5tab5-local-ai-terminal/assets/generated/unit-puzzle-strip.svg`

These files are intentionally diagrammatic placeholders. They must not be treated as real device screenshots or live LED captures.

## Required Evidence Sources

| Evidence ID | Purpose | Source Or Capture Path | Required For | Status | Notes |
|---|---|---|---|---|---|
| E001 | Tutorial outline evidence | `docs/TUTORIAL-outline.md` | Later production and terminal snippet selection | Required | Source of the build/flash command framing. |
| E002 | Git timeline evidence | `git history` | Later production and edit chronology | Required | Use history to narrate iteration and evolution. |
| E003 | HA stability / flashing memory | Project memory and verified local service behavior | Later production and reliability claims | Required | Use only validated local evidence, not fabricated metrics. |
| E004 | Tab5-to-local-Claude approval / handoff memory | Project memory and handoff notes | Later production and workflow narration | Required | Required context for approval and handoff language. |

## Optional Assets

| Asset ID | Scenes | Type | Source Or Capture Path | Status | Notes |
|---|---|---|---|---|---|
| O001 | S1 | Short real device montage | Phone footage | Optional | Useful if available. |
| O002 | S6 | Flashing or serial monitor footage | Terminal recording | Optional | Use only if readable. |

## Generated Assets Allowed

| Asset ID | Scenes | Description | Constraints |
|---|---|---|---|
| G001 | S1, S7 | Clean M5Tab5 device render | Must look like a hardware product frame, not a fake UI screenshot. |
| G002 | S2, S4 | System diagrams | Must use project-accurate labels and boundaries. |
| G003 | S5 | Pitfall icon set | Use simple technical icons, no decorative clutter. |

## Capture Checklist

- [ ] Capture or provide M5Tab5 hero photo.
- [ ] Capture HA screen.
- [ ] Capture Xiaozhi screen.
- [ ] Capture Claude assistant screen.
- [ ] Capture Voice Input screen.
- [ ] Capture Settings tools screen.
- [ ] Capture Email LED strip or decide to draw it.
- [ ] Capture Unit-Puzzle LED strip or decide to draw it.
- [ ] Capture LoRa chat screen.
- [ ] Capture Stocks screen.
- [ ] Confirm terminal snippets for build and flash.

## Scene Coverage Check

- S1 has A001 plus app flashes.
- S2 has A002.
- S3 has A003 through A010, plus A013 for Unit-Puzzle LED, with A008 reserved for Email LED.
- S4 has A002 and A005.
- S5 has A011.
- S6 has A012.
- S7 has A001 and A002.
