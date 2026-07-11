# src/editor/gui/session/ — M5 GUI SESSION state surfaces (R-FILE-006: ephemeral session state layered
# on the write path, distinct from authored file state). Each subdir is one headless session
# affordance built ON the F0b uitree tree + the panel write seams, CI-assertable WITHOUT booting CEF.
# One add_subdirectory per surface; add new ones on their OWN line so concurrent sibling tasks merge
# cleanly.

# M5-F7 (issue #162): Ctrl+Z/Y session undo/redo over the inspector override-write journal
# (context_gui_undo). Added after panels/ (in gui/CMakeLists.txt) — it builds ON
# context_gui_panel_inspector's commit engine + gateway seam.
add_subdirectory(undo)
