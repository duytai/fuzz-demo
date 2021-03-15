cargo clean
RUSTFLAGS="--cfg fuzzing
  -C debug-assertions
  -C overflow_checks
  -C passes=sancov
  -C llvm-args=-sanitizer-coverage-level=3
  -C llvm-args=-sanitizer-coverage-trace-pc-guard
  -C llvm-args=-sanitizer-coverage-prune-blocks=0
  -C link-arg=-fuse-ld=gold
  -C opt-level=3
  -C target-cpu=native
  -C debuginfo=0
  -l afl-llvm-rt
  -L $PWD
" cargo build --release --all-targets
