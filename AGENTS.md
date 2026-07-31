# Godot Project

You are in a GDAgent terminal inside the Godot editor. This is your workspace for AI-assisted Godot development.

## Working with This Project

**Understand before implementing.** When given a task, first understand the existing code and scene structure. Look at how similar features are implemented in the project before starting your own approach.

**Work in focused increments.** Make one logical change at a time and verify it works before moving on. Don't try to refactor multiple systems in a single pass.

**Test when you think you're done.** After making changes, run the game to verify the specific behavior works. Don't test after every tiny edit - test when you believe the task is complete, then present results to the user.

**Handle errors properly.** When the game crashes or has issues, use godot-mcp to get the actual error messages. Understand the root cause before fixing, don't just chase symptoms.

**Respect the project's design.** Games have intentional design decisions - mechanics, pacing, feel. When modifying behavior, understand why it was done that way first. Don't second-guess design without reason.

**Be explicit about uncertainty.** If you're unsure what the user wants, ask. If you're unsure how something works in the project, investigate first rather than guessing.

## Using godot-mcp

Use godot-mcp for most tasks - it connects directly to the Godot editor and the running game. It can run the game, inspect scenes and nodes, write scripts, modify nodes, take screenshots, get errors, debug, and much more. This is your primary interface for working with the project. Run `help` to see what's available.

### Workflow: How to Edit and Test Efficiently

Godot locks GDScript files while the game is running, meaning you **must stop the game to edit scripts**. To avoid wasting time on constant start/stop cycles, plan your work in batches:

1. **Prefer editor-side inspection:** Many tasks can be done without starting the game. Try to inspect the editor's current scene, list project files, or check for script warnings before deciding to run the game.
2. **Make your edits:** Apply your code changes while the game is stopped.
3. **Start the game once:** Launch the game and wait until it is ready.
4. **Batch your runtime inspections:** Do ALL your runtime data gathering in this single session. Check the node tree, get errors, capture output logs, and take screenshots before stopping the game.
5. **Capture errors immediately:** If the game crashes or you stop it, immediately fetch the structured errors and output logs before they are lost.
6. **Stop the game:** Stop the session to release script locks.
7. **Repeat:** Fix issues based on the batch of collected data, then test again.

## Rules

- DO NOT modify or read `addons/gdagent/` or `addons/godot_mcp/` - these are the GDAgent tool addons, not your game code
- Work in the project's own directories: `scenes/`, `scripts/`, `ui/`, `levels/`, `assets/`, etc.
- Don't run `godot` from terminal and avoid editing scene files directly - use godot-mcp tools which communicate with the running editor
