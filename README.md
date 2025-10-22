# Multithreaded Plugin-Based Analyzer

## Overview
This project implements a **multithreaded, plugin-based text analyzer** in **C**.  
It reads input lines from **STDIN** and processes them through a **pipeline of dynamically loaded plugins**, each running in its own thread.  
Communication between plugins is handled using **bounded, thread-safe queues**, following the **producer–consumer** model.

The system is designed to be **modular**, **scalable**, and **synchronized**, supporting flexible runtime configuration and dynamic plugin loading.

---

## Features
- **Dynamic plugin loading** via `.so` shared libraries (`dlopen` / `dlmopen`)
- **Multithreading**: each plugin runs in its own thread
- **Bounded queues** for producer–consumer synchronization
- **Thread-safe monitors** built with mutexes and condition variables
- **Graceful pipeline shutdown** and memory cleanup
- **Automatic build & test scripts** (`build.sh`, `test.sh`)
- Compatible with **GCC 13** and **Valgrind**

---

## Available Plugins
| Plugin | Description |
|---------|--------------|
| `logger` | Logs all processed strings |
| `uppercaser` | Converts text to uppercase |
| `rotator` | Shifts each character one position to the right (last → first) |
| `flipper` | Reverses character order |
| `expander` | Adds spaces between characters |
| `typewriter` | Prints text with delay to simulate typing |

---

## Project Structure
```
Final Project/
├── build.sh
├── test.sh
├── main.c
├── plugin_sdk.h
├── consumer_producer.c
├── consumer_producer.h
├── monitor.c
├── monitor.h
├── plugins/
│   ├── logger.c
│   ├── uppercaser.c
│   ├── rotator.c
│   ├── flipper.c
│   ├── expander.c
│   └── typewriter.c
└── output/
    ├── analyzer
    ├── logger.so
    ├── ...
```

---

## Build Instructions
Run the build script to compile everything:

```bash
./build.sh
```

This will:
- Verify **GCC 13** is available
- Compile the main analyzer binary (`output/analyzer`)
- Build all plugins into `.so` files under `output/`

---

## Usage
Once built, you can run the analyzer with any combination of plugins:

```bash
echo "hello world" | ./output/analyzer <queue_size> <plugin1> <plugin2> ... <pluginN>
```

Example:
```bash
echo "hello" | ./output/analyzer 20 uppercaser rotator logger
```

**Expected output:**
```
[logger] ELLOH
```

---

## Testing
To verify functionality and memory safety:

```bash
./test.sh
```

The test script runs:
- Functional tests  
- Edge cases (empty input, whitespace, large input, etc.)  
- Thread synchronization checks  
- **Valgrind** memory leak detection  
- **ASAN** optional sanitizer tests  

Example output:
```
[INFO] Test run finished — passed: 106, failed: 0, ran: 106
```

---

## Technical Details
- **Language:** C (C11)
- **Compiler:** GCC 13
- **Threading:** POSIX threads (`pthread`)
- **Synchronization:** mutexes, condition variables
- **Dynamic loading:** `dlopen()` / `dlmopen()` for isolated plugin instances
- **Error handling:** custom monitor structure for safe signaling
- **Memory safety:** tested via `valgrind` and AddressSanitizer

---

## 🧑‍💻 Author
**Itay Aharoni**    

---
