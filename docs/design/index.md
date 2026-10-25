# Design records

Each record is the plan for one feature. It gives the goal, the options, the decisions and the
implementation notes. The records are a history: they show the state when the work was done. The
user pages describe the current behavior. If a record and a user page are different, the user page
is correct.

| Record | Issue | State |
|---|---|---|
| [Audio demodulation](audio-demod-plan.md) | #7 | Implemented |
| [Verbose logging](verbose-logging-plan.md) | #14 | Implemented (PR #15) |
| [Time segments](time-segment-plan.md) | #16 | Implemented (PR #17) |
| [CUDA GPU acceleration](cuda-gpu-plan.md) | #3 | Implemented |
| [Region export](region-export-plan.md) | #18 | Implemented (PR #22) |
| [Apple GPU (Metal)](apple-gpu-plan.md) | #21 | Implemented (PR #25) |
| [UI cleanup](ui-cleanup-plan.md) | #12, #19 | Implemented (PR #26) |
| [Signal generator sweeps](siggen-sweep-plan.md) | #28 | Implemented (PR #30) |
| [Signal generator chains](siggen-chain-plan.md) | #27 | Implemented (PR #31) |
| [Plot snapshots](plot-snapshot-plan.md) | #13 | Implemented |
| [Other USRP models](usrp-models-plan.md) | #11 | Implemented. Needs network USRPs to verify |
| [Signal Hound BB60](bb60-integration-plan.md) | #2 | Implemented with a simulated device. Needs a BB60 to verify |
| [Epiq Sidekiq](sidekiq-integration-plan.md) | #4 | Receive implemented, tested with a simulated card and built against SDK 4.26.0. Needs a card to verify |
| [Point measurements](point-measurement-plan.md) | #80 | Implemented |
| [Run-time configuration file](config-file-plan.md) | #88 | Implemented |

```{toctree}
:hidden:

audio-demod-plan
verbose-logging-plan
time-segment-plan
cuda-gpu-plan
region-export-plan
apple-gpu-plan
ui-cleanup-plan
siggen-sweep-plan
siggen-chain-plan
plot-snapshot-plan
usrp-models-plan
bb60-integration-plan
sidekiq-integration-plan
point-measurement-plan
config-file-plan
```
