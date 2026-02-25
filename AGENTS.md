## Guidelines (Mostly from Handmade hero, Cassey Muratori, Ryan Fleury)
- Favor simplicity, explicitness, and clarity over magic or hidden logic
- Use fat structs / dense data layouts aligned with real usage patterns 
- Avoid premature abstraction; only generalize when it’s justified
- Minimize dependencies and external frameworks
- Avoid deep inheritance hierarchies and over-complex OOP
- Prefer full rebuilds over fragile partial build systems 
- Let design emerge from working prototypes and feedback
- Stay close to hardware: care about performance, memory behavior, and correctness
- Recognize tradeoffs: every abstraction or “nice feature” has a cost
- Design UI/tooling systems iteratively; evolve based on real use (rather than all upfront)
- Use clear, modular boundaries with unambiguous APIs
- Avoid hidden side-effects and implicit behavior
- Treat tooling, build systems, debugging, and editor support as first-class design areas
- Hot-reload friendly: no globals, all state in struct
- Performance: unity build, single allocation, cache-friendly
- Simplicity: single build.sh, no CMake

## Architecture
**General**
- C99

## Build System
```bash
./build.sh      # Build both layers
./bin/info_paster  # Run - auto-detects changes and reloads game code
```

## Memory Management
- Single large allocation at startup
- Arena allocators (permanent + transient)