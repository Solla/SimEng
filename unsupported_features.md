# C1 Ultra Unsupported Features in SimEng

Based on the provided Software Optimization Guide and LLVM scheduling patches, the following features of the C1 Ultra microarchitecture are currently unsupported or only partially modeled in SimEng.

## 1. CME (SME2) Co-Processor Routing
*   **Feature**: C1 Ultra uses a dedicated **CME Operations Block** for SME2 instructions. When the CPU enters **Streaming SVE (SSVE)** mode (`PSTATE.SM = 1`), specific vector instructions are rerouted from the standard `V0-V5` ports to the external CME block.
*   **SimEng Status**: Currently, SimEng does not support dynamic instruction rerouting based on the `PSTATE.SM` flag in the Fetch/Decode stage. Instruction-to-Port mapping is static in the YAML configuration.

## 2. Late-Forwarding of Accumulators (MAC Bypassing)
*   **Feature**: The integer and floating-point pipelines support specialized internal data paths for **late-forwarding of accumulate operands**. This allows back-to-back Multiply-Accumulate (MAC) or Dot-Product instructions to effectively have a 1-cycle latency for the accumulator dependency, even if the base execution latency is 3 or 4 cycles.
*   **SimEng Status**: SimEng models instruction dependencies based on the full execution latency defined in the YAML. It does not currently support operand-specific bypass latencies (e.g., Read-after-Write for the accumulator vs. other operands).

## 3. Dynamic Vector Length (VL) Switching
*   **Feature**: C1 Ultra supports a 128-bit VL for standard SVE but mandates a **512-bit VL** when in Streaming SVE mode.
*   **SimEng Status**: SimEng's vector length is a static parameter in the YAML configuration (`Vector-Length`). It cannot dynamically switch between 128-bit and 512-bit modes during a single simulation run.

## 4. Renaming of NZCV and SP
*   **Feature**: The microarchitecture performs complete physical register renaming for **Condition Flags (NZCV)** and the **Stack Pointer (SP)** to eliminate false structural dependencies.
*   **SimEng Status**: While SimEng supports register renaming for general-purpose and vector registers, it treats system registers and flags with varying levels of architectural faithfulness. Specific renaming of the condition code register as a first-class renamed entity might not be fully aligned with the C1 Ultra's high-width renaming logic.

## 5. 23-Wide Heterogeneous Issue
*   **Feature**: The C1 Ultra has **23 independent issue pipelines**, many with extremely specific instruction group support (e.g., `C1UUnitV0134`).
*   **SimEng Status**: While SimEng can model many ports, the YAML configuration for 23 ports with highly overlapping but distinct `Instruction-Group-Support` sets may lead to suboptimal port allocation in the simulator's `Balanced` allocator compared to the hardware's proprietary logic.

## 6. Architectural Extensions (Armv9.3-A)
*   **Feature**: Armv9.3-A introduces features like Memory Tagging (MTE), Pointer Authentication (PAC), and Activity Monitoring (AMU).
*   **SimEng Status**: 
    *   **MTE/PAC**: Only partially supported if Capstone/ISA logic handles them, but the pipeline-level performance impact (e.g., specific latencies for `AUTDA`) might not be fully accurate.
    *   **AMU/MPAM**: Hardware activity monitoring and resource partitioning are not modeled.

## 7. Reliability and ECC (SECDED)
*   **Feature**: SECDED ECC protection on L1/L2 caches and MMU.
*   **SimEng Status**: SimEng is a performance and functional simulator; it does not model hardware errors or ECC correction cycles.
