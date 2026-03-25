## 2026-03-23 - [ImGui UX Patterns]
**Learning:** In internal cheat/tooling menus, technical terms (like 'Fullbright' or 'Wallhack') can be opaque to new users. Using 'ImGui::SetItemTooltip' provides immediate, non-intrusive documentation, and 'ImGui::SeparatorText' creates a much stronger visual hierarchy than simple 'ImGui::Separator' or 'ImGui::Text' headers.
**Action:** Always use 'SeparatorText' for grouping settings and 'SetItemTooltip' for explainable toggles in ImGui-based interfaces to reduce cognitive load and improve discoverability.

## 2026-03-24 - [Legibility in 3D Overlays]
**Learning:** Text labels in 3D ESP (Extra Sensory Perception) overlays are often unreadable when positioned over bright in-game surfaces or light sources. Adding a semi-transparent black 'plate' behind the text using 'ImGui::CalcTextSize' and 'AddRectFilled' significantly improves accessibility and legibility without obstructing too much of the game view.
**Action:** Always include high-contrast background rectangles for 3D world-space text labels to ensure consistent readability across varying environments.

## 2026-03-25 - [Contextual State Reset]
**Learning:** In complex tables (like an Entity Explorer) where users can 'Track' or 'Select' specific rows, it is a significant UX burden to force the user to locate the specific active row to undo the action. Providing a contextual 'Clear' or 'Reset' button that only appears when a selection is active significantly reduces friction and improves state discoverability.
**Action:** Always provide a high-level "Clear [State]" button near controls that can toggle specific, persistent highlights or tracking modes.
