# AGENTS.md — Repository Guidance for Autonomous AI Agents

This document defines the repository context, engineering workflow, architecture principles, coding standards, and verification requirements for AI coding agents working in **Bloodborne PC**. Follow these rules alongside the current task, existing code, repository documentation, and recorded architectural decisions.

## 1. Project Overview & Subsystem Boundaries

The project is an x86-64 native Linux execution layer for *Bloodborne* (PlayStation 4, CUSA03173 / CUSA00900, update 1.09), replacing PS4 system libraries and translating GPU commands to Vulkan without CPU emulation.

```text
bloodborne_pc/
├── src/                 # Native C11 Runtime (process loader, memory virtualization, POSIX/syscall bridges)
├── core/                # C++23 Core Translation Engine (GNM/PM4 command processor, Vulkan translation, FSR/DLSS, bbnet)
│   ├── bbnet/           # Online multiplayer subsystem (shadNet client, HTTPS, UPnP, BBSC seamless co-op)
│   └── third_party/     # Submodules (fsr-vulkan, imgui, sirit)
├── launcher/            # C++23 Client Launcher (SDL3, OpenGL 3.3 Core, RmlUi 6.x)
│   ├── src/             # Launcher controller, model, config persistence, process pipe streaming
│   └── assets/          # RML templates, RCSS stylesheets, TrueType fonts
├── docs/                # Architecture documentation, ADRs, coding standards (100% English)
├── out/                 # Build artifacts directory (binaries, logs, temporary overlays)
├── scripts/             # Build and execution scripts, mods and patch linkers
│   ├── build.sh         # Unified compilation script
│   ├── run.sh           # Execution entry point (Linux)
│   └── run.bat          # Execution entry point (Windows)
└── flake.nix            # Declarative Nix flake environment
```

Treat this tree as a guide, not a substitute for inspecting the current repository. Verify actual paths, build targets, and existing implementations before changing them.

## 2. Repository Reconnaissance & Change Scope

Before editing code:

1. Inspect the repository state with `git status --short` and preserve unrelated user changes. Never discard, overwrite, reset, or reformat unrelated work.
2. Read the relevant source, its callers, tests, and the nearest build configuration. Search for an existing pattern before creating a new one.
3. Read relevant architecture documentation and ADRs under `docs/`. If a domain glossary such as `GLOSSARY.md` or `CONTEXT.md` exists, use its terms for concepts, tests, and module names. Treat recorded decisions as constraints unless the task explicitly asks to revisit them.
4. For architectural work without a named target, inspect recent history (`git log --oneline`) to identify frequently changed areas before broadening the scan. Prefer high-friction code that is actively changing over speculative cleanup in stable code.
5. Establish the smallest scope that fully fixes the requested behavior. Avoid drive-by refactors, unrelated renames, dependency additions, and speculative generalization.
6. Inspect `justfile`, build scripts, CMake files, and existing tool configuration before running or adding checks. Do not assume a test framework, build directory, target name, compiler flag, or tool configuration exists merely because it is common elsewhere.

Do not edit generated files or third-party sources unless the task explicitly requires it. In particular, respect the ownership and regeneration model documented by each affected subsystem.

## 3. Development Workflow: Understand → Test → Implement → Refactor → Verify

Use a behavior-first, incremental workflow. The default for bug fixes and feature work is **test-driven development (TDD)**.

### 3.1 Define behavior and interfaces first

Before implementation, identify:

- The externally observable behavior that must change or remain stable.
- The public interface and the **seam** through which that behavior can be observed or replaced.
- The highest-value behaviors to test, including important failure modes and invariants.
- Any interface, ABI, configuration, threading, performance, or compatibility constraints.

Write down the proposed behaviors and test seams in the plan for non-trivial work. If a public-interface or architectural decision is materially ambiguous, resolve that ambiguity before committing the repository to a design. Do not ask questions whose answers are already present in the task or repository.

Design for testability at the interface; do not add test-only hooks or widen a public interface without a real need. Prefer pure computations and explicit dependencies where they simplify behavior and testing, while preserving existing ownership, ABI, and performance requirements.

### 3.2 Use vertical RED → GREEN → REFACTOR cycles

For each behavior, repeat this sequence:

1. **RED:** Add one focused test that describes one observable behavior. Run it and confirm that it fails for the expected reason. A compile failure, malformed test, or unrelated setup error is not a valid RED result.
2. **GREEN:** Implement only enough production code to make that test pass. Do not implement speculative future cases before they are required by a test or specification.
3. **REFACTOR:** Once the relevant tests pass, improve naming, remove duplication, deepen a module, or simplify the implementation without changing behavior. Run the affected tests again after each meaningful refactor.
4. Repeat for the next behavior, then run the broader test suite and project checks.

Do **not** write an entire batch of tests and only then implement the entire feature (horizontal slicing). Keep work in small, end-to-end vertical slices so that every test informs the next implementation step.

For a bug fix, first reproduce the defect with a regression test at the narrowest meaningful public seam. If a reliable automated test is impractical—for example, due to external hardware, guest execution, or unavailable game data—document the limitation and use the closest repeatable validation available. Never claim a test passed when it was not run.

### 3.3 Write behavior-focused tests

- Test observable behavior through public interfaces, not private methods, internal data layout, call counts, or implementation-specific sequences.
- Prefer integration-style tests that exercise real code paths. Tests should read like concise behavioral specifications and remain valid when the implementation is refactored.
- Test at agreed, meaningful seams. For each important seam, know what the test covers and what it does not cover; do not attempt to test every imaginable edge case.
- Mock or fake only genuine external boundaries where necessary, such as OS services, a remote endpoint, time, nondeterminism, or an unavailable external device. Do not mock the project's own logic merely to make tests easier to write.
- Avoid testing the same behavior redundantly at many layers. Concentrate effort on critical paths, complex logic, regressions, and important invariants.
- Do not add a new testing framework or dependency when the existing test facilities can express the required behavior.
- For graphics, guest execution, parsers, and other data-heavy paths, consider suitable project-compatible approaches such as deterministic replay, captured-input regression tests, golden outputs, corpus tests, or differential tests—but only when their assumptions are understood and their fixtures are available.

### 3.4 Refactor only after behavior is protected

Refactoring must preserve externally observable behavior. First get the tests to GREEN; then make one coherent structural improvement at a time and rerun the relevant checks. Do not combine a behavior change with a large unrelated refactor, because doing so makes failures harder to diagnose.

## 4. Architecture & Codebase Design

Optimize for **leverage**, **locality**, **testability**, and ease of understanding—not for the number of files, classes, abstraction layers, or design patterns.

Use the following vocabulary consistently when discussing architecture:

- **Module:** Anything with an interface and an implementation, from a function or class to a package or a slice spanning multiple runtime layers.
- **Interface:** Everything a caller must know to use a module correctly: types, invariants, ordering, error behavior, configuration, ownership, threading, and relevant performance characteristics—not only method signatures.
- **Implementation:** The behavior and details hidden behind the interface.
- **Depth:** How much useful behavior a module provides relative to the complexity of the interface callers must learn. Prefer deep modules with small, stable interfaces over shallow modules that merely forward calls.
- **Seam:** A place where behavior can be changed or observed through an interface without editing all the callers. Choosing where the seam lives is a design decision.
- **Adapter:** A concrete implementation that fulfills an interface at a seam.
- **Leverage:** How much capability a module gives callers without requiring them to understand its internals.
- **Locality:** How easy it is to understand, change, and verify a behavior without searching across many unrelated files.

Apply these principles:

1. **Keep interfaces small and explicit.** Put cohesive behavior and policy behind the interface instead of exposing internal steps to every caller. Document meaningful invariants, ownership rules, ordering requirements, error behavior, and threading assumptions.
2. **Prefer deep modules, not thin wrappers.** A module that only forwards calls, mirrors another interface, or forces callers to understand its internals is likely too shallow. Do not split code into extra layers without a concrete benefit.
3. **Use the deletion test before extracting or introducing a module.** Ask: if this module were removed, would the complexity move behind a smaller, more coherent interface, or would it spread through its callers? Prefer the extraction only when it improves locality and leverage rather than moving names and files around.
4. **Treat the interface as the test surface.** If a module is difficult to test without inspecting private state, first consider improving its interface or moving the seam. Do not make private implementation details public solely to satisfy a test.
5. **Do not invent abstractions for hypothetical futures.** One implementation alone may indicate a hypothetical seam rather than a proven need for an adapter. Multiple real implementations can demonstrate that a seam is useful, but two adapters are evidence—not a mandate to abstract every repeated shape. In particular, do not introduce a generic backend, plugin system, or compatibility layer without a current, concrete requirement.
6. **Keep behavior local.** Avoid concepts that must be understood by reading many files, duplicated policy scattered across callers, and modules whose interface leaks implementation details. A small amount of intentional duplication can be better than a premature abstraction; consolidate when the shared behavior and ownership are clear.
7. **Respect domain language and existing decisions.** Use repository terminology in names, tests, and documentation. Do not re-litigate an ADR implicitly as part of an unrelated task.
8. **Prefer evidence over generic cleanup.** For broader architecture improvements, look at recent change history and actual friction. State the problem, affected modules, candidate seam, expected test improvements, trade-offs, and why the change is worth its cost before refactoring.
9. **Keep architecture reviews separate from implementation when the scope is broad.** An audit should report candidates and trade-offs without silently changing the code. Implement the selected, bounded improvement through the normal test/build workflow.

The purpose of architecture work is to make the next change easier, the behavior more testable, and the code easier to navigate—not merely to make the diagram look cleaner.

## 5. Language & Toolchain Constraints

- **Runtime (`src/`):** Hybrid **C11** (`-std=c11`) / modern **C++23** (`-std=c++23`). Low-level PS4 guest ABI interception, syscall bridges, and assembly trampolines are strict C11 (`sysv_abi` / `extern "C"` where appropriate). Runtime subsystems, PAL, and high-level services use modern C++23.
- **Core Translation Engine (`core/`):** Modern **C++23** (`-std=c++23`).
- **Launcher (`launcher/`):** Modern **C++23** (`-std=c++23`). Zero Python dependencies.
- **Documentation:** All documentation must be maintained in **English**. Do not create or commit localized `.ru.md` or non-English files.

Apply C++ rules only to C++ code. Do not introduce C++ constructs into strict C11 files, change guest ABI contracts casually, or apply C++ ownership guidance to C code without translating it into an appropriate C design. Preserve the language standard already required by each target; do not raise a target's standard just to use a newer feature.

## 6. C++ Coding Standards

Use `docs/cpp-coding-standards.md` as the repository-specific reference. The following rules summarize the baseline, derived from the C++ Core Guidelines, while remaining subordinate to explicit project conventions and ABI constraints.

### 6.1 Intent, types, and simplicity

- Express intent through names, types, and small functions with one coherent responsibility.
- Prefer compile-time guarantees and strongly typed interfaces over loosely related primitive values when this makes invalid states harder to represent.
- Initialize every object at declaration. Prefer `const` and `constexpr` by default; use mutability when it is needed and clear.
- Prefer `enum class` over unscoped enums, `nullptr` over `0` or `NULL`, and standard containers (`std::array`, `std::vector`, `std::string`) over raw arrays and manual ownership where compatible with the task.
- Avoid magic numbers, narrowing conversions, C-style casts, needless global mutable state, and global-scope `using namespace` declarations in headers.
- Keep function argument lists and visible interfaces small. Prefer return values (including a small result struct when appropriate) over output parameters when that improves clarity.
- Use `std::string_view` and other non-owning views only when the referenced storage is guaranteed to outlive their use. Make ownership and lifetime explicit.
- Use templates to express real generic behavior. Constrain template parameters with standard concepts where appropriate; do not introduce complex template machinery when a simpler function or type suffices.

### 6.2 Ownership, lifetime, and errors

- Use RAII for resources, including file descriptors, pipes, locks, Vulkan objects, SDL windows, GL contexts, and other acquired handles. Ensure resources are released on every exit path.
- Prefer the Rule of Zero. Use standard-library or project-owned RAII wrappers rather than manual ownership; avoid raw `new`/`delete` and `malloc`/`free` in C++ unless an explicit low-level integration requirement justifies them.
- Treat raw pointers and references as non-owning unless the interface clearly documents otherwise. Prefer `std::unique_ptr` for exclusive dynamic ownership; use `std::shared_ptr` only when shared lifetime is genuinely required.
- Constructors should establish a valid object. Use `explicit` for single-argument constructors unless implicit conversion is deliberate.
- Follow the error-handling strategy already used by the target. Do not introduce exceptions, error codes, or status wrappers inconsistently across a module or across a C ABI. Use `noexcept` only when the implementation can actually guarantee it.
- Destructors and cleanup paths must not fail. Avoid catch-all handlers that hide errors or convert failures into false success.

### 6.3 Headers and implementation structure

- Headers must be self-contained and compile without relying on incidental include order. Include what the header needs and avoid unnecessary include dependencies where a forward declaration is valid.
- Do not place `using namespace` at global scope in headers.
- Keep implementation details private unless exposing them is part of the designed interface. Avoid unnecessary inheritance; use composition and value semantics where they fit better.
- Define or delete copy/move/destructor operations consistently when managing a resource; otherwise, prefer the Rule of Zero.
- Mark overrides with `override` and final implementations with `final` when appropriate. Polymorphic base classes must have a correct destruction policy.
- Keep naming and file organization consistent with nearby code and `docs/cpp-coding-standards.md`. Do not impose a wholesale naming migration during an unrelated change.

### 6.4 Concurrency and performance

- Prevent data races and make mutable shared state explicit. Use RAII locking (`std::lock_guard`, `std::unique_lock`, or `std::scoped_lock`) and name lock objects.
- Do not use `volatile` for thread synchronization. Do not detach threads or introduce lock-free algorithms without a specific, justified need.
- Avoid calling unknown callbacks or arbitrary external code while holding locks. Keep lock duration and lock ordering easy to reason about.
- Prefer straightforward code over clever low-level tricks. Do not claim a change is faster without measurement, and do not perform speculative optimization before profiling identifies a real bottleneck.
- When performance work is required, define the measured workload and baseline, preserve correctness tests, and compare results under repeatable conditions.

## 7. Static Analysis, clangd, clang-format, and Build Feedback

Use static analysis and language tooling as part of the development loop, not as a substitute for understanding the code or running tests.

### 7.1 Discover the existing configuration first

Before running or configuring tools, inspect the actual repository setup, including where present:

- `.clang-format` and `.clang-tidy`;
- CMake configuration and target-specific compiler options;
- `compile_commands.json` and the build directory that generated it;
- `justfile`, `scripts/build.sh`, existing lint targets, and CI workflows;
- `docs/cpp-coding-standards.md` and any subsystem-specific guidance.

Use the existing configuration and compiler/toolchain expected by the project. Do not invent a new formatting style, run a repository-wide rewrite, or add broad warning policies that break third-party or ABI-sensitive code without a scoped reason. If no configuration exists, use the repository's existing checks and propose the smallest useful addition when static-analysis configuration is part of the task.

### 7.2 clangd

- Use `clangd` as the C/C++ language-server and diagnostics engine where available.
- Prefer a valid `compile_commands.json` so `clangd` sees the same include paths, defines, language standard, and compiler options as the real build.
- When diagnostics seem inconsistent, verify that the compilation database corresponds to the current target and build configuration before changing code to silence them.
- Treat diagnostics as evidence to investigate. Do not suppress warnings, add casts, or change interfaces merely to silence a warning without understanding its cause.
- `clangd` diagnostics do not replace a successful project build and test run.

### 7.3 clang-format

- Follow the repository's `.clang-format` when present. Format only the files or changed regions relevant to the task; do not reformat unrelated files.
- Prefer the existing `just format` or repository formatting target when that target is appropriately scoped. For a manual check, use the installed `clang-format` version and its supported options.
- Review formatting diffs before accepting them. Formatting must not hide semantic changes or generate unrelated churn.

### 7.4 Static analysis and compiler diagnostics

- Run the repository's existing `just lint` or equivalent check when available.
- Use configured `clang-tidy` with the correct compilation database for changed C++ files when the tool and project configuration are available. Respect existing `.clang-tidy` rules and per-target exceptions.
- Review compiler warnings, especially lifetime/ownership issues, narrowing conversions, undefined behavior risks, unused values, and concurrency hazards.
- Use sanitizers or additional analyzers only when supported by the target and build configuration. Do not apply incompatible instrumentation blindly to guest ABI trampolines, third-party dependencies, or specialized low-level code.
- Separate pre-existing diagnostics from regressions introduced by the change. Report any warning or analyzer that could not be run and why; never imply that an unexecuted check passed.

## 8. Key Commands & Feedback Loops

Prefer the repository's `just` targets because they encode the intended build workflow. Verify target definitions in `justfile` if behavior is unclear.

### Building and validation

```bash
# Standard feedback loops via CMake presets
cmake --preset default
cmake --build --preset default
ctest --preset default
cmake --build --preset lint
cmake --build --preset format

# Unified task runner alternatives (justfile / scripts)
just build
just test
just lint
just format

./scripts/build.sh
./scripts/build.sh --test
./scripts/build.sh --lint
./scripts/build.sh --format
```

Run the narrowest relevant test/check frequently, then the full applicable suite before declaring the task complete. Do not run every command blindly if a target is unavailable or inappropriate; investigate the supported target and document any limitation.

### Extract game dumps (PKG / ISO)

```bash
just extract /path/to/game.pkg ./games/CUSA03173
# or
./tools/extractor/extract_dump.sh /path/to/game.pkg ./games/CUSA03173
```

### Running the launcher and game

```bash
# Launch the native graphical interface
just launcher
# or: ./launcher/bb-launcher.sh
# or: ./out/bb-launcher

# Launch game directly (requires a valid game directory in bbport.toml)
just run
# or: ./scripts/run.sh
# or: ./scripts/run.bat (Windows)
```

Game execution may depend on user-provided game files, runtime environment, and hardware. Do not report an execution-based verification as complete unless it was actually performed under a suitable setup.

## 9. RmlUi & UI Styling Constraints

When modifying `launcher/assets/launcher.rml` or `launcher/assets/style.rcss`:

1. **Universal block reset:** RmlUi has no built-in user-agent stylesheet; elements default to `display: inline`. Preserve the block reset in every stylesheet:

   ```css
   div, p, h1, h2, h3, h4, section, header, footer {
       display: block;
       box-sizing: border-box;
   }
   ```

2. **Single-string font families:** RCSS accepts only a single string identifier for `font-family` (for example, `font-family: Noto Sans;`), not comma-separated fallback lists.
3. **Viewport resets:** Reset container scroll offsets (`content->SetScrollTop(0.0f)`) when switching navigation tabs, where that container's scroll position should start at the top.
4. **Valid XML:** Explicitly close every RML tag (for example, `<input type="text" />` and `<br/>`).
5. **Preserve project patterns:** Before changing the launcher model, controller, view markup, or styling, inspect the existing interactions and adjacent tests. Prefer behavior-focused tests for logic that can be exercised without rendering; use visual/manual checks for presentation behavior that cannot be reliably asserted by existing automated tests.

## 10. Dependencies & Automated Fallbacks

### Arch Linux / Omarchy host packages

```bash
sudo pacman -S --needed vulkan-headers boost robin-map zydis sdl3 freetype2
```

### CMake automated fallbacks

- `Vulkan-Headers`: fetched via `FetchContent` if system headers are missing.
- `VulkanMemoryAllocator`: fetched via `FetchContent` if VMA is missing.
- `tsl-robin-map`, `magic_enum`, and `xbyak`: automatically fetched if not provided by the host.

Check current CMake files before changing dependency behavior. Prefer existing system packages or the project's declared fallback mechanism. Do not add a dependency to solve a problem already addressed by the standard library, existing infrastructure, or a small coherent module unless the trade-off is justified.

## 11. Completion Checklist

Before marking a task complete, verify the applicable items:

- [ ] The implementation addresses the requested behavior without unrelated changes.
- [ ] Relevant documentation, glossary terms, and ADRs were consulted; no existing decision was contradicted accidentally.
- [ ] Each important behavior is covered at a meaningful public seam, or any testing limitation is explicitly documented.
- [ ] For TDD work, at least one expected RED result was observed for each new behavior where a reliable automated test is feasible, followed by GREEN.
- [ ] Tests describe behavior rather than private implementation details and avoid unnecessary mocks.
- [ ] The design keeps interfaces explicit and small, improves locality where needed, and avoids speculative abstraction.
- [ ] C++ changes follow the project standards for ownership, initialization, type safety, errors, concurrency, and headers; C11 and ABI-sensitive code remain compatible with their constraints.
- [ ] Formatting, static analysis, and compiler diagnostics were run where configured and applicable.
- [ ] Relevant tests and builds were run after the final code change. Any skipped or failed check is reported honestly, with its reason.
- [ ] No generated artifacts, unrelated formatting changes, local game data, or third-party files were modified unintentionally.
- [ ] Documentation changes are in English.

## 12. Methodology References

These repository rules summarize and adapt the following engineering resources; follow the repository's actual toolchain and project-specific constraints where they differ:

- [TDD — Matt Pocock](https://www.skills.sh/mattpocock/skills/tdd): behavior-first tests, public test seams, vertical RED → GREEN → REFACTOR cycles, and minimal implementation slices.
- [Improve Codebase Architecture — Matt Pocock](https://www.skills.sh/mattpocock/skills/improve-codebase-architecture): evidence-driven architecture reviews, deletion test, module deepening, locality, and keeping audits separate from implementation.
- [C++ Coding Standards — Everything Claude Code](https://www.skills.sh/affaan-m/ecc/cpp-coding-standards): modern C++ and C++ Core Guidelines principles for resource safety, type safety, immutability, clarity, concurrency, and performance.
- [Codebase Design — Matt Pocock](https://www.skills.sh/mattpocock/skills/codebase-design): shared vocabulary and principles for modules, interfaces, depth, seams, adapters, leverage, and locality.
