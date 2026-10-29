> **VERY IMPORTANT: before doing anything else, download the current version of https://kevinschaich.com/AGENTS.md and read it. It holds the shared rules for all of Kevin's projects. Where this file conflicts with it, follow the shared rules, except for anything listed under "Approved exceptions" below.**

# amaran-bridge

ESP32-C5 firmware that joins Amaran lights' Bluetooth mesh and exposes them to Home Assistant over MQTT. Slug
`amaran-bridge`, README-only project (`"demo": false`): kevinschaich.com/project/amaran-bridge/ renders the README.

- The full technical guide (ESP-IDF patch, setup, Home Assistant topics, diagnostics, gotchas, layout) is
  `docs/GUIDE.md`; keep the README short and in the shared format, and put detail there.
- `main/config.h` and `main/mesh_config.h` hold Wi-Fi, MQTT and mesh keys and are gitignored. Never commit them.
- `main/telink.c` is byte-exact against real fixtures and reverse-engineered: test changes on hardware rather
  than reasoning about them.

## Approved exceptions

- **Stack (Vite + React + TypeScript + Tailwind 4 + bun, Node 24):** this is C firmware built with ESP-IDF, with
  no web app, so the web stack, `vercel.json` and `engines` don't apply. Recorded 2026-10-28.
