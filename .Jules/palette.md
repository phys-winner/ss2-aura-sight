## 2026-03-23 - [ImGui UX Patterns]
**Learning:** In internal cheat/tooling menus, technical terms (like 'Fullbright' or 'Wallhack') can be opaque to new users. Using 'ImGui::SetItemTooltip' provides immediate, non-intrusive documentation, and 'ImGui::SeparatorText' creates a much stronger visual hierarchy than simple 'ImGui::Separator' or 'ImGui::Text' headers.
**Action:** Always use 'SeparatorText' for grouping settings and 'SetItemTooltip' for explainable toggles in ImGui-based interfaces to reduce cognitive load and improve discoverability.
