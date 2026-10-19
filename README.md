# Core

Shared core abstractions for control software, consumed as cpkg packages from a host STM32 project. This repository currently contains one package:

| Package | Purpose | Documentation |
| --- | --- | --- |
| `Core::IController` | Header-only C++17 lifecycle interface for hierarchical control chains | [IController usage and constraints](IController/README.md) |

Package-specific ownership, lifecycle, failure, and concurrency contracts are maintained in the package README. Exact symbol contracts are documented by the existing [IController header Doxygen](IController/IController.hpp).
